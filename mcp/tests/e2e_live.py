"""Live end-to-end check against a running remote-control Kdenlive (dev/kdenlive-rc.sh).

Run explicitly: uv run python tests/e2e_live.py
Precondition (phase 1): at least one clip of 6s+ on the timeline.
"""

import asyncio
import pathlib
import subprocess

from kdenlive_mcp.bus import Kdenlive

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


async def main() -> None:
    print("media:", make_media())
    kd = Kdenlive()
    tl = await kd.call("timeline.get")
    assert tl["clips"], "load a clip onto the timeline first"
    audio_tracks = {t["id"] for t in tl["tracks"] if t["audio"]}
    video = [c for c in tl["clips"] if c["trackId"] not in audio_tracks]
    clip = (video or tl["clips"])[0]
    effect = "sepia" if video else "volume"
    start_count = len(tl["clips"])
    mid = clip["position"] + clip["duration"] // 2

    await kd.call("undo.begin", {"label": "e2e"})
    cut = await kd.call("clip.cut", {"clipId": clip["id"], "position": mid})
    right = {"type": "clip", "id": cut["rightId"]}
    await kd.call("effect.add", {"target": right, "effectId": effect})
    await kd.call("undo.end")

    tl = await kd.call("timeline.get")
    assert len(tl["clips"]) == start_count + 1
    assert [e["effectId"] for e in await kd.call("effect.get", {"target": right})] == [effect]

    await kd.call("undo")
    tl = await kd.call("timeline.get")
    assert len(tl["clips"]) == start_count, "one undo should revert the whole group"

    await kd.call("redo")
    tl = await kd.call("timeline.get")
    assert len(tl["clips"]) == start_count + 1
    right_now = next(c for c in tl["clips"] if c["position"] == mid and c["trackId"] == clip["trackId"])
    assert [e["effectId"] for e in await kd.call("effect.get", {"target": {"type": "clip", "id": right_now["id"]}})] == [effect]

    assert await kd.call("action.list", {"filter": "razor"}), "razor action missing"
    await kd.call("undo")  # leave the timeline as we found it, so the check is repeatable
    print("E2E OK")


if __name__ == "__main__":
    asyncio.run(main())
