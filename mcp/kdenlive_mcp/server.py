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
            # Cutting a grouped clip also cuts its partners, so skip targets that no longer span t.
            current = next((x for x in (await call("timeline.get"))["clips"] if x["id"] == c["id"]), None)
            if current and current["position"] < t < current["position"] + current["duration"]:
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


async def _clip(clip_id: int) -> dict:
    for c in (await call("timeline.get"))["clips"]:
        if c["id"] == clip_id:
            return c
    raise ToolError(f"no clip {clip_id}; ids change after edits, re-read get_timeline")


@server.tool()
async def insert_clip(bin_id: str, track_id: int, time: str | int | None = None) -> int:
    """Insert a bin clip on a track at a time (default: playhead). Returns the new clip id."""
    async with step("insert clip"):
        return (await call("clip.insert", {"binId": bin_id, "trackId": track_id, "position": await frames(time)}))["clipId"]


@server.tool()
async def move_clip(clip_id: int, time: str | int, track_id: int | None = None) -> str:
    """Move a clip to a time, optionally to another track."""
    t = await frames(time, default_playhead=False)
    track = track_id if track_id is not None else (await _clip(clip_id))["trackId"]
    async with step("move clip"):
        await call("clip.move", {"clipId": clip_id, "trackId": track, "position": t})
    return "ok"


@server.tool()
async def trim_clip(clip_id: int, duration: str | int, from_end: bool = True) -> int:
    """Set a clip's duration, trimming its end (default) or its start. Returns the new duration in frames."""
    d = await frames(duration, default_playhead=False)
    async with step("trim clip"):
        return (await call("clip.resize", {"clipId": clip_id, "duration": d, "fromEnd": from_end}))["duration"]


@server.tool()
async def delete_clips(clip_ids: list[int] | None = None) -> str:
    """Delete clips (default: the selection)."""
    ids = clip_ids or (await call("timeline.get"))["selection"]
    if not ids:
        raise ToolError("nothing selected; pass clip_ids")
    async with step("delete clips"):
        for cid in ids:
            await call("clip.delete", {"clipId": cid})
    return "ok"


@server.tool()
async def set_speed(clip_id: int, speed: float) -> str:
    """Change clip speed as a ratio: 2.0 = twice as fast, 0.5 = slow motion, -1 = reverse."""
    async with step("speed"):
        await call("clip.speed", {"clipId": clip_id, "speed": speed})
    return "ok"


@server.tool()
async def add_track(audio: bool, name: str | None = None) -> int:
    """Add a video (audio=false) or audio track. Returns its id."""
    params: dict[str, Any] = {"audio": audio}
    if name:
        params["name"] = name
    async with step("add track"):
        return (await call("track.add", params))["trackId"]


@server.tool()
async def set_track(track_id: int, name: str | None = None, locked: bool | None = None, hidden: bool | None = None, muted: bool | None = None) -> str:
    """Rename, lock, hide (video) or mute (audio) a track."""
    async with step("track"):
        if name is not None:
            await call("track.rename", {"trackId": track_id, "name": name})
        if locked is not None:
            await call("track.lock", {"trackId": track_id, "locked": locked})
        if hidden is not None:
            await call("track.hide", {"trackId": track_id, "hidden": hidden})
        if muted is not None:
            await call("track.mute", {"trackId": track_id, "muted": muted})
    return "ok"


@server.tool()
async def add_marker(time: str | int | None = None, comment: str = "", clip_bin_id: str | None = None) -> str:
    """Add a timeline guide (default) or a marker on a bin clip, at a time (default: playhead)."""
    params: dict[str, Any] = {"position": await frames(time), "comment": comment}
    params.update({"scope": "clip", "binId": clip_bin_id} if clip_bin_id else {"scope": "guides"})
    async with step("marker"):
        await call("marker.add", params)
    return "ok"


@server.tool()
async def set_zone(start: str | int, end: str | int) -> str:
    """Set the timeline in/out zone (end exclusive)."""
    await call("zone.set", {"in": await frames(start, False), "out": await frames(end, False)})
    return "ok"


@server.tool()
async def seek(time: str | int) -> str:
    """Move the playhead."""
    await call("monitor.seek", {"position": await frames(time, False)})
    return "ok"


@server.tool()
async def play(play: bool = True) -> str:
    """Play or pause the project monitor."""
    await call("monitor.play", {"play": play})
    return "ok"


@server.tool()
async def select(clip_ids: list[int]) -> str:
    """Select clips in the timeline."""
    await call("timeline.select", {"ids": clip_ids})
    return "ok"


@server.tool()
async def list_effects(filter: str | None = None, audio: bool | None = None) -> list[dict]:
    """Search available effects by id/name substring; audio filters audio (true) or video (false) effects."""
    params: dict[str, Any] = {"kind": "effect"}
    if filter:
        params["filter"] = filter
    if audio is not None:
        params["audio"] = audio
    return await call("asset.list", params)


@server.tool()
async def effect_schema(effect_id: str) -> dict:
    """Parameters of an effect: names, types, defaults, ranges, and whether they can be keyframed."""
    return await call("asset.schema", {"kind": "effect", "id": effect_id})


@server.tool()
async def list_transitions(filter: str | None = None) -> list[dict]:
    """Available transitions/compositions; composition=true ones go between tracks, others are same-track mixes."""
    params: dict[str, Any] = {"kind": "transition"}
    if filter:
        params["filter"] = filter
    return await call("asset.list", params)


@server.tool()
async def get_effects(clip_id: int | None = None, track_id: int | None = None, master: bool = False) -> list:
    """Effect stack with parameter values (default: first selected clip)."""
    return await call("effect.get", {"target": (await _targets(clip_id, track_id, master))[0]})


@server.tool()
async def remove_effect(index: int, clip_id: int | None = None, track_id: int | None = None, master: bool = False) -> str:
    """Remove the effect at `index` from a stack."""
    async with step("remove effect"):
        for target in await _targets(clip_id, track_id, master):
            await call("effect.remove", {"target": target, "index": index})
    return "ok"


@server.tool()
async def move_effect(from_index: int, to_index: int, clip_id: int | None = None, track_id: int | None = None, master: bool = False) -> str:
    """Reorder an effect in a stack."""
    async with step("move effect"):
        for target in await _targets(clip_id, track_id, master):
            await call("effect.move", {"target": target, "from": from_index, "to": to_index})
    return "ok"


@server.tool()
async def toggle_effect(index: int, enabled: bool, clip_id: int | None = None, track_id: int | None = None, master: bool = False) -> str:
    """Enable or bypass an effect."""
    async with step("toggle effect"):
        for target in await _targets(clip_id, track_id, master):
            await call("effect.enable", {"target": target, "index": index, "enabled": enabled})
    return "ok"


@server.tool()
async def set_keyframe(param: str, time: str | int, value: str | float, effect_index: int, clip_id: int, interp: str | None = None) -> str:
    """Keyframe an effect parameter on a clip at a TIMELINE time (converted to clip-local). interp: linear, discrete, smooth, cubic_in, … ."""
    clip = await _clip(clip_id)
    local = await frames(time, False) - clip["position"]
    if not 0 <= local < clip["duration"]:
        raise ToolError(f"time is outside clip {clip_id} ({clip['position']}–{clip['position'] + clip['duration'] - 1})")
    params: dict[str, Any] = {"target": {"type": "clip", "id": clip_id}, "index": effect_index, "param": param, "frame": local, "value": str(value)}
    if interp:
        params["interp"] = interp
    async with step("keyframe"):
        await call("keyframe.set", params)
    return "ok"


@server.tool()
async def add_transition(clip_a: int, clip_b: int, transition_id: str | None = None) -> str:
    """Transition between two clips: same track → crossfade mix (clips must touch); different tracks → composition over their overlap."""
    a, b = await _clip(clip_a), await _clip(clip_b)
    if a["position"] > b["position"]:
        a, b = b, a
    async with step("transition"):
        if a["trackId"] == b["trackId"]:
            await call("mix.add", {"clipId": b["id"], "transitionId": transition_id or "luma"})
        else:
            start = max(a["position"], b["position"])
            end = min(a["position"] + a["duration"], b["position"] + b["duration"])
            if end <= start:
                raise ToolError("the clips do not overlap in time")
            tl = await call("timeline.get")
            order = [t["id"] for t in tl["tracks"]]  # top first
            upper = a if order.index(a["trackId"]) < order.index(b["trackId"]) else b
            await call("composition.add", {"transitionId": transition_id or "composite", "trackId": upper["trackId"], "position": start, "duration": end - start})
    return "ok"


@server.tool()
async def add_composition(transition_id: str, track_id: int, start: str | int, end: str | int) -> int:
    """Add a composition (e.g. composite, wipe, qtblend) on a track over [start, end). Returns its id."""
    s0, e0 = await frames(start, False), await frames(end, False)
    async with step("composition"):
        return (await call("composition.add", {"transitionId": transition_id, "trackId": track_id, "position": s0, "duration": e0 - s0}))["compositionId"]


def main() -> None:
    server.run("stdio")
