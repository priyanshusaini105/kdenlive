"""D-Bus client for the Kdenlive remote-control fork (/RemoteControl)."""

import json
import os
from typing import Any

from dbus_fast import Message, MessageType
from dbus_fast.aio import MessageBus

PREFIX = "org.kde.kdenlive-"
PATH = "/RemoteControl"
IFACE = "org.kde.kdenlive.RemoteControl"


class KdenliveNotRunning(RuntimeError):
    pass


class AmbiguousInstance(RuntimeError):
    pass


class KdenliveError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def session_bus_address(env, uid: int, exists=os.path.exists) -> str | None:
    """Session bus from the environment, else the standard systemd user socket (MCP hosts may strip env)."""
    if env.get("DBUS_SESSION_BUS_ADDRESS"):
        return env["DBUS_SESSION_BUS_ADDRESS"]
    path = f"/run/user/{uid}/bus"
    return f"unix:path={path}" if exists(path) else None


_VANISHED = {"org.freedesktop.DBus.Error.ServiceUnknown", "org.freedesktop.DBus.Error.NameHasNoOwner", "org.freedesktop.DBus.Error.Disconnected"}


def should_rediscover(error_name: str) -> bool:
    """Only a vanished instance is worth re-discovering; a NoReply timeout may mean the call is still running, so resending would run it twice."""
    return error_name in _VANISHED


class BusCallError(ConnectionError):
    def __init__(self, error_name: str, body):
        super().__init__(f"{error_name}: {body}")
        self.error_name = error_name


def pick_instance(candidates: list[tuple[str, int]], env_pid: str | None) -> str:
    """candidates: (bus name, pid) already filtered to those exposing /RemoteControl."""
    if env_pid:
        for name, pid in candidates:
            if str(pid) == env_pid.strip():
                return name
        raise KdenliveNotRunning(f"no remote-control Kdenlive with pid {env_pid}; start the forked build: dev/kdenlive-rc.sh")
    if not candidates:
        raise KdenliveNotRunning("start the forked build: dev/kdenlive-rc.sh")
    if len(candidates) > 1:
        pids = ", ".join(str(pid) for _, pid in candidates)
        raise AmbiguousInstance(f"several Kdenlive instances ({pids}); set KDENLIVE_PID")
    return candidates[0][0]


class Kdenlive:
    def __init__(self) -> None:
        self._bus: MessageBus | None = None
        self._name: str | None = None

    async def _dbus(self, destination: str, path: str, interface: str, member: str, signature: str = "", body: list | None = None) -> Message:
        assert self._bus is not None
        reply = await self._bus.call(
            Message(destination=destination, path=path, interface=interface, member=member, signature=signature, body=body or [])
        )
        if reply.message_type == MessageType.ERROR:
            raise BusCallError(reply.error_name, reply.body)
        return reply

    async def _discover(self) -> str:
        if self._bus is None or not self._bus.connected:
            address = session_bus_address(os.environ, os.getuid())
            if address is None:
                raise KdenliveNotRunning("no D-Bus session bus found (DBUS_SESSION_BUS_ADDRESS unset, no /run/user/<uid>/bus)")
            self._bus = await MessageBus(bus_address=address).connect()
        names = (await self._dbus("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames")).body[0]
        candidates = []
        for name in names:
            if not name.startswith(PREFIX):
                continue
            try:
                node = await self._bus.introspect(name, PATH)
            except Exception:
                continue  # stock Kdenlive: no /RemoteControl
            if any(i.name == IFACE for i in node.interfaces):
                candidates.append((name, int(name[len(PREFIX):])))
        return pick_instance(candidates, os.environ.get("KDENLIVE_PID"))

    async def _raw(self, member: str, signature: str = "", body: list | None = None) -> str:
        for attempt in (0, 1):
            if self._name is None:
                self._name = await self._discover()
            try:
                return (await self._dbus(self._name, PATH, IFACE, member, signature, body)).body[0]
            except BusCallError as e:
                if attempt or not should_rediscover(e.error_name):
                    raise
                self._name = None  # app restarted: re-discover once
            except OSError:
                self._name = None
                self._bus = None
                if attempt:
                    raise
        raise AssertionError("unreachable")

    async def call(self, command: str, params: dict | None = None) -> Any:
        reply = json.loads(await self._raw("call", "ss", [command, json.dumps(params or {})]))
        if not reply.get("ok"):
            err = reply.get("error", {})
            raise KdenliveError(err.get("code", "failed"), err.get("message", ""))
        return reply.get("result")

    async def commands(self) -> list[dict]:
        return json.loads(await self._raw("listCommands"))

    async def fps(self) -> float:
        # ponytail: one round trip per call instead of a cache; cache if latency matters
        return (await self.call("project.info"))["fps"]
