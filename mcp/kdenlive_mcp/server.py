"""MCP server: live control of the Kdenlive remote-control fork."""

from contextlib import asynccontextmanager
from typing import Any

import asyncio
import base64

from mcp.server.mcpserver import Image, MCPServer
from mcp.server.mcpserver.exceptions import ToolError

from . import geometry, timecode
from .bus import AmbiguousInstance, Kdenlive, KdenliveError, KdenliveNotRunning

INSTRUCTIONS = (
    "Wrap each user request in begin_request/end_request so it is one Ctrl+Z. "
    "Read get_timeline before editing; ids can go stale if the user edits. "
    "Times accept HH:MM:SS:FF, MM:SS, 2.5s or frames."
)

server = MCPServer("kdenlive", instructions=INSTRUCTIONS)
kd = Kdenlive()
_request_open = False
_request_label = ""


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
        # plain frame counts need no fps (and no round trip)
        return timecode.parse(value, 0 if isinstance(value, int) else await kd.fps())
    except ValueError as e:
        raise ToolError(str(e)) from e


@asynccontextmanager
async def step(label: str):
    """One undo entry per tool, unless the AI already opened a request group."""
    if _request_open:
        # Kdenlive closes a group after 10 s without calls (so a stalled AI can't swallow user edits); reopen it so the rest of
        # the request still lands in one undo step.
        try:
            await kd.call("undo.begin", {"label": _request_label})
        except KdenliveError as e:
            if e.code != "group_open":
                raise ToolError(str(e)) from e
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
    global _request_open, _request_label
    await call("undo.begin", {"label": label})
    _request_open, _request_label = True, label
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
            # deleting a grouped clip deletes its partners too
            if any(c["id"] == cid for c in (await call("timeline.get"))["clips"]):
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


@server.tool()
async def import_media(paths: list[str], folder_id: str | None = None) -> list[str]:
    """Import media files into the project bin. Returns bin ids (clips load asynchronously; see list_bin 'ready')."""
    params: dict[str, Any] = {"paths": paths}
    if folder_id:
        params["folderId"] = folder_id
    async with step("import"):
        return (await call("bin.import", params))["binIds"]


@server.tool()
async def list_bin() -> dict:
    """Project bin: clips (binId, name, type, duration, ready) and folders."""
    return await call("bin.list")


@server.tool()
async def add_color_clip(color: str, duration: str | int, name: str = "Color") -> str:
    """Create a color clip (#RRGGBB or #RRGGBBAA) in the bin. Returns its bin id."""
    async with step("color clip"):
        return (await call("bin.color", {"color": color, "duration": await frames(duration, False), "name": name}))["binId"]


async def _wait_ready(bin_id: str, timeout_s: float = 10) -> None:
    for _ in range(int(timeout_s / 0.2)):
        if any(c["binId"] == bin_id and c["ready"] for c in (await call("bin.list"))["clips"]):
            return
        await asyncio.sleep(0.2)
    raise ToolError(f"bin clip {bin_id} did not finish loading")


def _title_spec(text: str, size: int, color: str, align: str, y: int | None, background: str | None, font: str) -> dict:
    item: dict[str, Any] = {"type": "text", "text": text, "size": size, "color": color, "align": align, "font": font}
    if y is not None:
        item["y"] = y
    spec: dict[str, Any] = {"items": [item]}
    if background:
        spec["background"] = background
    return spec


@server.tool()
async def add_title(text: str, duration: str | int = "5s", time: str | int | None = None, track_id: int | None = None, size: int = 96,
                    color: str = "#ffffff", align: str = "center", y: int | None = None, background: str | None = None, font: str = "Sans") -> int:
    """Create a title and place it on the timeline at a time (default: playhead) on a video track (default: top). Returns the clip id.
    y is in project pixels from the top (default: vertically centered)."""
    d = await frames(duration, False)
    t = await frames(time)
    tl = await call("timeline.get")
    if track_id is None:
        video = [tr["id"] for tr in tl["tracks"] if not tr["audio"] and not tr["locked"]]
        if not video:
            raise ToolError("no unlocked video track")
        track_id = video[0]
    if y is None:
        y = max(0, (await call("project.info"))["height"] // 2 - size)
    async with step("title"):
        bin_id = (await call("title.create", {"name": text[:30], "duration": d, "spec": _title_spec(text, size, color, align, y, background, font)}))["binId"]
        await _wait_ready(bin_id)
        return (await call("clip.insert", {"binId": bin_id, "trackId": track_id, "position": t}))["clipId"]


@server.tool()
async def update_title(bin_id: str, text: str, size: int = 96, color: str = "#ffffff", align: str = "center", y: int | None = None,
                       background: str | None = None, font: str = "Sans") -> str:
    """Replace a title clip's content (every timeline use updates)."""
    async with step("update title"):
        await call("title.update", {"binId": bin_id, "spec": _title_spec(text, size, color, align, y, background, font)})
    return "ok"


@server.tool()
async def add_subtitle(start: str | int, end: str | int, text: str, layer: int = 0) -> int:
    """Add a subtitle between two times. Returns its id."""
    async with step("subtitle"):
        return (await call("subtitle.add", {"start": await frames(start, False), "end": await frames(end, False), "text": text, "layer": layer}))["id"]


@server.tool()
async def list_subtitles() -> list[dict]:
    """Subtitles (id, layer, start, end, text, style) with timecodes."""
    fps = await kd.fps()
    subs = await call("subtitle.list")
    for s_ in subs:
        s_["start_tc"], s_["end_tc"] = timecode.format(s_["start"], fps), timecode.format(s_["end"], fps)
    return subs


@server.tool()
async def edit_subtitle(subtitle_id: int, text: str | None = None, start: str | int | None = None, end: str | int | None = None, style: str | None = None) -> str:
    """Change a subtitle's text, timing or style."""
    params: dict[str, Any] = {"id": subtitle_id}
    if text is not None:
        params["text"] = text
    if start is not None:
        params["start"] = await frames(start, False)
    if end is not None:
        params["end"] = await frames(end, False)
    if style is not None:
        params["style"] = style
    async with step("edit subtitle"):
        await call("subtitle.edit", params)
    return "ok"


@server.tool()
async def grab_frame(time: str | int | None = None, width: int = 640) -> Image:
    """Look at the project monitor: seeks to `time` if given, then returns the frame as an image."""
    if time is not None:
        await call("monitor.seek", {"position": await frames(time, False)})
        await asyncio.sleep(0.3)
    shot = await call("monitor.grab", {"width": width})
    return Image(data=base64.b64decode(shot["data"]), format="png")


@server.tool()
async def set_volume(track_id: int, db: float) -> str:
    """Set an audio track's (or master, track_id=-1) volume in dB (0 = unchanged). Not undoable."""
    await call("mixer.set", {"trackId": track_id, "volumeDb": db})
    return "ok"


@server.tool()
async def set_pan(track_id: int, pan: float) -> str:
    """Set an audio track's balance: 0 = left, 0.5 = center, 1 = right. Not undoable."""
    await call("mixer.set", {"trackId": track_id, "pan": pan})
    return "ok"


async def _effect_index(clip_id: int, effect_id: str) -> int:
    """Index of effect_id on the clip, adding it if absent."""
    target = {"type": "clip", "id": clip_id}
    for fx in await call("effect.get", {"target": target}):
        if fx["effectId"] == effect_id:
            return fx["index"]
    return (await call("effect.add", {"target": target, "effectId": effect_id}))["index"]


@server.tool()
async def transform_clip(clip_id: int, x: float, y: float, w: float, h: float, opacity: float = 1.0, at: str | int | None = None) -> str:
    """Position/scale a clip (project pixels) with the Transform effect. With `at` (timeline time) it sets a keyframe there, else a static value."""
    value = geometry.rect(x, y, w, h, opacity)
    async with step("transform"):
        index = await _effect_index(clip_id, "qtblend")
        if at is None:
            await call("effect.set", {"target": {"type": "clip", "id": clip_id}, "index": index, "params": {"rect": value}})
        else:
            clip = await _clip(clip_id)
            local = await frames(at, False) - clip["position"]
            await call("keyframe.set", {"target": {"type": "clip", "id": clip_id}, "index": index, "param": "rect", "frame": local, "value": value})
    return "ok"


@server.tool()
async def zoom_clip(clip_id: int, from_scale: float = 1.0, to_scale: float = 1.2, interp: str = "smooth") -> str:
    """Ken Burns style zoom over the whole clip, around the center (1.0 = no zoom)."""
    clip = await _clip(clip_id)
    info = await call("project.info")
    anim = geometry.animate([(0, geometry.zoom_rect(from_scale, info["width"], info["height"])),
                             (clip["duration"] - 1, geometry.zoom_rect(to_scale, info["width"], info["height"]))], interp)
    async with step("zoom"):
        index = await _effect_index(clip_id, "qtblend")
        await call("effect.set", {"target": {"type": "clip", "id": clip_id}, "index": index, "params": {"rect": anim}})
    return "ok"


@server.tool()
async def mask_clip(clip_id: int, points: list[list[float]]) -> str:
    """Polygon mask (rotoscoping): points as [x, y] fractions 0..1 of the frame, e.g. [[0.25,0.25],[0.75,0.25],[0.75,0.75],[0.25,0.75]]."""
    clip = await _clip(clip_id)
    try:
        spline = geometry.roto([(p_[0], p_[1]) for p_ in points], 0, clip["duration"])
    except (ValueError, IndexError) as e:
        raise ToolError(str(e)) from e
    async with step("mask"):
        index = await _effect_index(clip_id, "rotoscoping")
        await call("effect.set", {"target": {"type": "clip", "id": clip_id}, "index": index, "params": {"spline": spline}})
    return "ok"


async def _wait_job(job_id: int, command: str = "job.status", timeout_s: float = 3600, every_s: float = 1.0) -> dict:
    for _ in range(int(timeout_s / every_s)):
        status = await call(command, {"jobId": job_id})
        if status["state"] != "running":
            if status["state"] == "failed":
                raise ToolError(status.get("error", "job failed"))
            return status
        await asyncio.sleep(every_s)
    raise ToolError(f"job {job_id} still running after {timeout_s:.0f}s")


@server.tool()
async def save_project(path: str | None = None) -> str:
    """Save the project (path required when it is untitled; must end in .kdenlive)."""
    params = {"path": path} if path else {}
    await _wait_job((await call("project.save", params))["jobId"], timeout_s=120, every_s=0.3)
    return "saved"


@server.tool()
async def new_project(profile: str | None = None, discard_changes: bool = False) -> str:
    """Start a new empty project. Refuses if there are unsaved changes unless discard_changes=True."""
    if discard_changes:
        await call("project.discard")
    await _wait_job((await call("project.new", {"profile": profile} if profile else {}))["jobId"], timeout_s=120, every_s=0.3)
    return "ok"


@server.tool()
async def open_project(path: str, discard_changes: bool = False) -> str:
    """Open a .kdenlive project. Refuses if there are unsaved changes unless discard_changes=True. Kdenlive may show the user a recovery prompt."""
    if discard_changes:
        await call("project.discard")
    await _wait_job((await call("project.open", {"path": path}))["jobId"], timeout_s=300, every_s=0.5)
    return "ok"


@server.tool()
async def list_sequences() -> list[dict]:
    """Sequences (timelines) in the project."""
    return await call("sequence.list")


@server.tool()
async def open_sequence(uuid: str) -> str:
    """Switch to a sequence."""
    await call("sequence.open", {"uuid": uuid})
    return "ok"


@server.tool()
async def list_render_presets(filter: str | None = None) -> list[dict]:
    """Render presets (name, category, extension)."""
    presets = await call("render.presets")
    return [p_ for p_ in presets if not filter or filter.lower() in (p_["name"] + p_["extension"]).lower()]


@server.tool()
async def render(preset: str, output: str, range: str = "all", wait: bool = False) -> dict:
    """Render to a file. range: 'all', 'zone', or 'START-END' times (e.g. '0:10-0:20'). wait=True blocks until finished."""
    params: dict[str, Any] = {"preset": preset, "output": output}
    if range in ("all", "zone"):
        params["range"] = range
    else:
        start, _, end = range.partition("-")
        params["range"] = {"in": await frames(start.strip(), False), "out": await frames(end.strip(), False)}
    job = (await call("render.start", params))["jobId"]
    if not wait:
        return {"jobId": job, "state": "running"}
    return await _wait_job(job, command="render.status")


@server.tool()
async def render_status(job_id: int) -> dict:
    """Progress of a render started with wait=False."""
    return await call("render.status", {"jobId": job_id})


def main() -> None:
    server.run("stdio")
