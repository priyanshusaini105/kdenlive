"""Live end-to-end check against a running remote-control Kdenlive (dev/kdenlive-rc.sh).

Run explicitly: uv run python tests/e2e_live.py
It loads generated bars into an empty project itself, and leaves the timeline as it found it.
"""

import asyncio
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
    """A clip on a video track, loading the bars through stock D-Bus + clip.insert when the timeline is empty."""
    tl = await kd.call("timeline.get")
    video_tracks = [t["id"] for t in tl["tracks"] if not t["audio"]]
    on_video = [c for c in tl["clips"] if c["trackId"] in video_tracks]
    if on_video:
        return on_video[0]
    if not tl["clips"]:
        await kd._dbus(kd._name, "/kdenlive/MainWindow_1", "org.kde.kdenlive.MainWindow", "addProjectClip", "s", [str(media)])

        async def inserted():
            await kd._dbus(kd._name, "/kdenlive/MainWindow_1", "org.kde.kdenlive.MainWindow", "addTimelineClip", "s", [str(media)])
            return (await kd.call("timeline.get"))["clips"]

        await until(inserted, "stock addTimelineClip")
    bin_id = (await kd.call("timeline.get"))["clips"][0]["binId"]

    async def insert():
        try:
            return await kd.call("clip.insert", {"binId": bin_id, "trackId": video_tracks[0], "position": 500})
        except KdenliveError as e:
            if "loading" in e.message:
                return None
            raise

    cid = (await until(insert, "clip.insert"))["clipId"]
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

    await kd.call("monitor.seek", {"position": 30})

    async def seeked():
        return (await kd.call("monitor.position"))["position"] == 30

    await until(seeked, "seek to 30")

    assert await kd.call("action.list", {"filter": "razor"}), "razor action missing"
    print("E2E OK")


if __name__ == "__main__":
    asyncio.run(main())
