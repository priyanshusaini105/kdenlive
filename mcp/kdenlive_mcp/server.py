"""MCP server: live control of the Kdenlive remote-control fork."""

from contextlib import asynccontextmanager
from typing import Any

from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError

from . import timecode
from .bus import AmbiguousInstance, Kdenlive, KdenliveError, KdenliveNotRunning

INSTRUCTIONS = (
    "Wrap each user request in begin_request/end_request so it is one Ctrl+Z. "
    "Read get_timeline before editing; ids can go stale if the user edits. "
    "Times accept HH:MM:SS:FF, MM:SS, 2.5s or frames."
)

server = MCPServer("kdenlive", instructions=INSTRUCTIONS)
kd = Kdenlive()
_request_open = False


async def call(command: str, params: dict | None = None) -> Any:
    try:
        return await kd.call(command, params)
    except (KdenliveError, KdenliveNotRunning, AmbiguousInstance) as e:
        raise ToolError(str(e)) from e


async def frames(value: str | int | None, default_playhead: bool = True) -> int:
    if value is None:
        if not default_playhead:
            raise ToolError("a time is required")
        tl = await call("timeline.get")
        if tl.get("playhead") is None:
            raise ToolError("no playhead available; pass a time")
        return tl["playhead"]
    try:
        return timecode.parse(value, await kd.fps())
    except ValueError as e:
        raise ToolError(str(e)) from e


@asynccontextmanager
async def step(label: str):
    """One undo entry per tool, unless the AI already opened a request group."""
    if _request_open:
        yield
        return
    await call("undo.begin", {"label": label})
    try:
        yield
    finally:
        await call("undo.end")


def _target(clip_id: int | None, track_id: int | None, master: bool) -> dict | None:
    if master:
        return {"type": "master"}
    if clip_id is not None:
        return {"type": "clip", "id": clip_id}
    if track_id is not None:
        return {"type": "track", "id": track_id}
    return None


async def _targets(clip_id: int | None, track_id: int | None, master: bool) -> list[dict]:
    t = _target(clip_id, track_id, master)
    if t:
        return [t]
    selection = (await call("timeline.get"))["selection"]
    if not selection:
        raise ToolError("nothing selected; pass clip_id, track_id or master")
    return [{"type": "clip", "id": cid} for cid in selection]


@server.tool()
async def get_timeline() -> dict:
    """Tracks, clips, compositions, playhead and selection. Positions are frames, with *_tc timecodes added."""
    tl = await call("timeline.get")
    fps = tl["fps"]
    for item in tl["clips"] + tl["compositions"]:
        item["position_tc"] = timecode.format(item["position"], fps)
        item["duration_tc"] = timecode.format(item["duration"], fps)
    if tl.get("playhead") is not None:
        tl["playhead_tc"] = timecode.format(tl["playhead"], fps)
    return tl


@server.tool()
async def cut_at(time: str | int | None = None, track_id: int | None = None) -> list[int]:
    """Razor cut at a time (default: playhead). Targets clips on track_id, else selected clips, else all clips on unlocked tracks. Returns new right-hand clip ids."""
    t = await frames(time)
    tl = await call("timeline.get")
    locked = {tr["id"] for tr in tl["tracks"] if tr["locked"]}
    spanning = [c for c in tl["clips"] if c["position"] < t < c["position"] + c["duration"]]
    if track_id is not None:
        targets = [c for c in spanning if c["trackId"] == track_id]
    else:
        targets = [c for c in spanning if c["id"] in tl["selection"]] or [c for c in spanning if c["trackId"] not in locked]
    if not targets:
        raise ToolError(f"no clip spans frame {t}")
    new_ids = []
    async with step("cut"):
        for c in targets:
            new_ids.append((await call("clip.cut", {"clipId": c["id"], "position": t}))["rightId"])
    return new_ids


@server.tool()
async def add_effect(effect_id: str, clip_id: int | None = None, track_id: int | None = None, master: bool = False, params: dict | None = None) -> list[int]:
    """Add an effect (e.g. 'sepia', 'fade_to_black') to a clip, track or master; default: selected clips. Returns the effect index per target."""
    indexes = []
    async with step(f"add {effect_id}"):
        for target in await _targets(clip_id, track_id, master):
            p = {"target": target, "effectId": effect_id}
            if params:
                p["params"] = {k: str(v) for k, v in params.items()}
            indexes.append((await call("effect.add", p))["index"])
    return indexes


@server.tool()
async def set_effect_params(index: int, params: dict, clip_id: int | None = None, track_id: int | None = None, master: bool = False) -> str:
    """Set parameters of the effect at `index` (visible stack order). Values may be MLT animation strings like '0=0;50~=1'."""
    async with step("set effect"):
        for target in await _targets(clip_id, track_id, master):
            await call("effect.set", {"target": target, "index": index, "params": {k: str(v) for k, v in params.items()}})
    return "ok"


@server.tool()
async def list_actions(filter: str | None = None) -> list[dict]:
    """Kdenlive's registered actions (menus/shortcuts). Text ending in '…' opens a dialog the user must handle."""
    return await call("action.list", {"filter": filter} if filter else {})


@server.tool()
async def trigger_action(name: str) -> str:
    """Trigger a Kdenlive action by name (from list_actions)."""
    await call("action.trigger", {"name": name})
    return "ok"


@server.tool()
async def undo() -> dict:
    """Undo the last step (one AI request when grouped)."""
    return await call("undo")


@server.tool()
async def redo() -> dict:
    """Redo the last undone step."""
    return await call("redo")


@server.tool()
async def begin_request(label: str) -> str:
    """Start grouping edits so the whole user request is one Ctrl+Z."""
    global _request_open
    await call("undo.begin", {"label": label})
    _request_open = True
    return "ok"


@server.tool()
async def end_request() -> str:
    """Finish the current request group."""
    global _request_open
    _request_open = False
    try:
        await kd.call("undo.end")
    except KdenliveError as e:
        if e.code != "no_group":  # idle auto-close already ended it
            raise ToolError(str(e)) from e
    return "ok"


@server.tool()
async def kdenlive_commands() -> list[dict]:
    """Every remote-control command with its parameters (for kdenlive_call)."""
    try:
        return await kd.commands()
    except (KdenliveNotRunning, AmbiguousInstance) as e:
        raise ToolError(str(e)) from e


@server.tool()
async def kdenlive_call(command: str, params: dict | None = None) -> Any:
    """Raw remote-control command (see kdenlive_commands). Positions are frames."""
    return await call(command, params)


def main() -> None:
    server.run("stdio")
