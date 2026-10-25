"""Live end-to-end check against a running remote-control Kdenlive (dev/kdenlive-rc.sh).

Run explicitly: uv run python tests/e2e_live.py
It loads generated bars into an empty project itself, and leaves the timeline as it found it.
"""

import asyncio
import base64
import pathlib
import subprocess

from kdenlive_mcp.bus import Kdenlive, KdenliveError

MEDIA = pathlib.Path("/tmp/claude-1002/kdenlive-e2e")


def make_media() -> pathlib.Path:
    MEDIA.mkdir(parents=True, exist_ok=True)
    out = MEDIA / "bars.mp4"
    if not out.exists():
        subprocess.run(
            ["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "smptebars=s=1280x720:r=25:d=10",
             "-f", "lavfi", "-i", "sine=d=10", "-shortest", str(out)],
            check=True,
        )
    return out


async def until(predicate, what: str, tries: int = 50):
    for _ in range(tries):
        if result := await predicate():
            return result
        await asyncio.sleep(0.2)
    raise AssertionError(f"timed out waiting for {what}")


async def video_clip(kd: Kdenlive, media: pathlib.Path) -> dict:
    """A clip on a video track; imports the bars with bin.import + clip.insert when there is none."""
    tl = await kd.call("timeline.get")
    video_tracks = [t["id"] for t in tl["tracks"] if not t["audio"]]
    on_video = [c for c in tl["clips"] if c["trackId"] in video_tracks]
    if on_video:
        return on_video[0]
    bin_id = (await kd.call("bin.import", {"paths": [str(media)]}))["binIds"][0]

    async def ready():
        return any(c["binId"] == bin_id and c["ready"] for c in (await kd.call("bin.list"))["clips"])

    await until(ready, "imported clip to load")
    cid = (await kd.call("clip.insert", {"binId": bin_id, "trackId": video_tracks[0], "position": 500}))["clipId"]
    return next(c for c in (await kd.call("timeline.get"))["clips"] if c["id"] == cid)


async def main() -> None:
    media = make_media()
    kd = Kdenlive()
    await kd.call("project.info")  # discovers the instance
    clip = await video_clip(kd, media)
    start_count = len((await kd.call("timeline.get"))["clips"])
    mid = clip["position"] + clip["duration"] // 2

    # Phase 1: cut + effect as one undo step
    await kd.call("undo.begin", {"label": "e2e"})
    cut = await kd.call("clip.cut", {"clipId": clip["id"], "position": mid})
    right = {"type": "clip", "id": cut["rightId"]}
    await kd.call("effect.add", {"target": right, "effectId": "sepia"})
    await kd.call("undo.end")
    assert len((await kd.call("timeline.get"))["clips"]) > start_count  # AV groups cut on both tracks
    assert [e["effectId"] for e in await kd.call("effect.get", {"target": right})] == ["sepia"]
    await kd.call("undo")
    assert len((await kd.call("timeline.get"))["clips"]) == start_count, "one undo should revert the whole group"

    # Phase 2: move, trim, marker as one undo step; seek
    await kd.call("undo.begin", {"label": "e2e phase 2"})
    await kd.call("clip.move", {"clipId": clip["id"], "trackId": clip["trackId"], "position": clip["position"] + 1000})
    await kd.call("clip.resize", {"clipId": clip["id"], "duration": 100})
    await kd.call("marker.add", {"scope": "guides", "position": 50, "comment": "e2e"})
    await kd.call("undo.end")
    moved = next(c for c in (await kd.call("timeline.get"))["clips"] if c["id"] == clip["id"])
    assert (moved["position"], moved["duration"]) == (clip["position"] + 1000, 100), moved
    assert any(m["comment"] == "e2e" for m in await kd.call("marker.list", {"scope": "guides"}))
    await kd.call("undo")
    back = next(c for c in (await kd.call("timeline.get"))["clips"] if c["id"] == clip["id"])
    assert (back["position"], back["duration"]) == (clip["position"], clip["duration"]), back
    assert not any(m["comment"] == "e2e" for m in await kd.call("marker.list", {"scope": "guides"}))

    # Phase 3: effect lookup, smooth keyframe, same-track dissolve as one undo step
    assert any(e["id"] == "sepia" for e in await kd.call("asset.list", {"kind": "effect", "filter": "sepia"}))
    assert any(p["name"] == "u" and p["animated"] for p in (await kd.call("asset.schema", {"kind": "effect", "id": "sepia"}))["params"])
    count = len((await kd.call("timeline.get"))["clips"])
    await kd.call("undo.begin", {"label": "e2e phase 3"})
    cut = await kd.call("clip.cut", {"clipId": clip["id"], "position": mid})
    target = {"type": "clip", "id": clip["id"]}
    index = (await kd.call("effect.add", {"target": target, "effectId": "sepia"}))["index"]
    await kd.call("keyframe.set", {"target": target, "index": index, "param": "u", "frame": 10, "value": "200", "interp": "smooth"})
    await kd.call("mix.add", {"clipId": cut["rightId"]})
    await kd.call("undo.end")
    tl = await kd.call("timeline.get")
    assert any(m["clipId"] == cut["rightId"] for m in tl["mixes"]), tl["mixes"]
    keys = await kd.call("keyframe.list", {"target": target, "index": index, "param": "u"})
    assert any(k["frame"] == 10 and k["interp"] == "smooth" for k in keys), keys
    await kd.call("undo")
    tl = await kd.call("timeline.get")
    assert len(tl["clips"]) == count and not tl["mixes"], "phase 3 undo should revert cut, effect, keyframe and mix"

    # Phase 4: title on the top video track, look at it, undo
    tl = await kd.call("timeline.get")
    top_video = next(t["id"] for t in tl["tracks"] if not t["audio"])
    title = (await kd.call("title.create", {"name": "e2e", "duration": 50,
                                             "spec": {"items": [{"type": "text", "text": "E2E", "size": 120, "align": "center", "y": 400}]}}))["binId"]

    async def title_ready():
        return any(c["binId"] == title and c["ready"] for c in (await kd.call("bin.list"))["clips"])

    await until(title_ready, "title to load")
    clip_at = clip["position"]
    try:
        tid = (await kd.call("clip.insert", {"binId": title, "trackId": top_video, "position": clip_at}))["clipId"]
    except KdenliveError:  # top track is the bars' track: use a fresh one
        top_video = (await kd.call("track.add", {"audio": False}))["trackId"]
        tid = (await kd.call("clip.insert", {"binId": title, "trackId": top_video, "position": clip_at}))["clipId"]
    await kd.call("monitor.seek", {"position": clip_at + 10})
    await asyncio.sleep(0.5)
    shot = await kd.call("monitor.grab", {"width": 320})
    png = base64.b64decode(shot["data"])
    assert png[:8] == b"\x89PNG\r\n\x1a\n" and shot["width"] == 320, shot["width"]
    (MEDIA / "grab.png").write_bytes(png)
    await kd.call("clip.delete", {"clipId": tid})
    await kd.call("bin.delete", {"binId": title})

    await kd.call("monitor.seek", {"position": 30})

    async def seeked():
        return (await kd.call("monitor.position"))["position"] == 30

    await until(seeked, "seek to 30")

    assert await kd.call("action.list", {"filter": "razor"}), "razor action missing"
    print("E2E OK")


if __name__ == "__main__":
    asyncio.run(main())
