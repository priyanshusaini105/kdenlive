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
            raise ConnectionError(f"{reply.error_name}: {reply.body}")
        return reply

    async def _discover(self) -> str:
        if self._bus is None or not self._bus.connected:
            self._bus = await MessageBus().connect()
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
            except (ConnectionError, OSError):
                self._name = None  # app restarted: re-discover once
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
