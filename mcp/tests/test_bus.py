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


def test_bus_address_from_env():
    from kdenlive_mcp.bus import session_bus_address
    assert session_bus_address({"DBUS_SESSION_BUS_ADDRESS": "unix:path=/x"}, 1000, lambda p: False) == "unix:path=/x"


def test_bus_address_falls_back_to_systemd_socket():
    from kdenlive_mcp.bus import session_bus_address
    assert session_bus_address({}, 1000, lambda p: p == "/run/user/1000/bus") == "unix:path=/run/user/1000/bus"


def test_bus_address_none_when_nothing():
    from kdenlive_mcp.bus import session_bus_address
    assert session_bus_address({}, 1000, lambda p: False) is None
