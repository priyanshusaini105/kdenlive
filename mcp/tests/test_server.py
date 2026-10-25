import asyncio

from kdenlive_mcp import server


class FakeKdenlive:
    """Two grouped clips (video 1, audio 2) spanning 0-100; cutting either cuts both, like Kdenlive groups."""

    def __init__(self):
        self.clips = [
            {"id": 1, "trackId": 10, "position": 0, "duration": 100},
            {"id": 2, "trackId": 20, "position": 0, "duration": 100},
        ]
        self.next_id = 3
        self.cuts = []

    async def __call__(self, command, params=None):
        if command == "timeline.get":
            return {"fps": 25, "playhead": 50, "selection": [], "clips": [dict(c) for c in self.clips],
                    "tracks": [{"id": 10, "locked": False}, {"id": 20, "locked": False}]}
        if command == "clip.cut":
            t = params["position"]
            target = next(c for c in self.clips if c["id"] == params["clipId"])
            assert target["position"] < t < target["position"] + target["duration"], "cut outside clip"
            self.cuts.append(params["clipId"])
            right = None
            for c in [c for c in self.clips if c["id"] in (1, 2)]:  # the group
                new = {"id": self.next_id, "trackId": c["trackId"], "position": t, "duration": c["position"] + c["duration"] - t}
                self.next_id += 1
                c["duration"] = t - c["position"]
                self.clips.append(new)
                right = right or new["id"]
            return {"leftId": params["clipId"], "rightId": right}
        if command in ("undo.begin", "undo.end"):
            return {}
        raise AssertionError(command)


def test_cut_at_cuts_a_group_once(monkeypatch):
    fake = FakeKdenlive()
    monkeypatch.setattr(server, "call", fake)
    asyncio.run(server.cut_at(50))
    assert fake.cuts == [1]
