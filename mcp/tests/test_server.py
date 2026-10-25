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


class FakeGroups:
    """Records undo.begin/end; the first begin after `auto_closed` succeeds (Kdenlive's idle close), later ones report group_open."""

    def __init__(self):
        self.open = False
        self.begins = 0

    async def __call__(self, command, params=None):
        from kdenlive_mcp.bus import KdenliveError
        if command == "undo.begin":
            if self.open:
                raise KdenliveError("group_open", "an undo group is already open")
            self.open = True
            self.begins += 1
            return {}
        if command == "undo.end":
            if not self.open:
                raise KdenliveError("no_group", "no undo group is open")
            self.open = False
            return {}
        return {}


def test_request_group_is_reopened_after_idle_close(monkeypatch):
    fake = FakeGroups()
    monkeypatch.setattr(server, "call", fake)
    monkeypatch.setattr(server.kd, "call", fake)
    asyncio.run(server.begin_request("make it pop"))
    fake.open = False  # Kdenlive closed the idle group while the AI was thinking

    async def edit():
        async with server.step("edit"):
            pass

    asyncio.run(edit())
    assert fake.open and fake.begins == 2  # the request group was reopened, not one step per tool
    asyncio.run(server.end_request())
    assert not fake.open


def test_delete_clips_skips_group_partners_already_deleted(monkeypatch):
    clips = {1, 2}

    async def fake(command, params=None):
        from kdenlive_mcp.bus import KdenliveError
        if command == "timeline.get":
            return {"selection": [1, 2], "clips": [{"id": c} for c in clips]}
        if command == "clip.delete":
            if params["clipId"] not in clips:
                raise KdenliveError("not_found", "no clip")
            clips.clear()  # deleting one deletes its AV group
            return {}
        return {}

    monkeypatch.setattr(server, "call", fake)
    assert asyncio.run(server.delete_clips()) == "ok"
