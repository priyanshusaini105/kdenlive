import pytest

from kdenlive_mcp.bus import AmbiguousInstance, KdenliveNotRunning, pick_instance


def test_no_instance():
    with pytest.raises(KdenliveNotRunning):
        pick_instance([], None)


def test_single_instance():
    assert pick_instance([("org.kde.kdenlive-5", 5)], None) == "org.kde.kdenlive-5"


def test_two_instances_need_pid():
    with pytest.raises(AmbiguousInstance):
        pick_instance([("org.kde.kdenlive-5", 5), ("org.kde.kdenlive-7", 7)], None)


def test_pid_selects():
    assert pick_instance([("org.kde.kdenlive-5", 5), ("org.kde.kdenlive-7", 7)], "7") == "org.kde.kdenlive-7"


def test_pid_not_found():
    with pytest.raises(KdenliveNotRunning):
        pick_instance([("org.kde.kdenlive-5", 5)], "9")
