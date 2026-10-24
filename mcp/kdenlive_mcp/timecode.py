"""Timecode <-> frame conversion.

HH:MM:SS:FF uses the nominal (rounded) rate, like non-drop-frame timecode shown
by Kdenlive. Clock forms (MM:SS, HH:MM:SS.mmm, 12.5s) use the real rate.
"""

import re

_FRAMES = re.compile(r"^(\d+)f?$")
_SECONDS = re.compile(r"^(\d+(?:\.\d+)?)s$")
_CLOCK = re.compile(r"^(?:(\d+):)?(\d+):(\d+(?:\.\d+)?)$")
_SMPTE = re.compile(r"^(\d+):(\d+):(\d+):(\d+)$")


def _from_seconds(seconds: float, fps: float) -> int:
    return int(seconds * fps + 0.5)


def parse(value: str | int, fps: float) -> int:
    if isinstance(value, bool):
        raise ValueError(f"not a time: {value!r}")
    if isinstance(value, int):
        if value < 0:
            raise ValueError(f"negative time: {value}")
        return value
    text = str(value).strip()
    if m := _FRAMES.match(text):
        return int(m[1])
    if m := _SECONDS.match(text):
        return _from_seconds(float(m[1]), fps)
    if m := _SMPTE.match(text):
        h, mi, s, ff = map(int, m.groups())
        nominal = round(fps)
        if ff >= nominal or mi >= 60 or s >= 60:
            raise ValueError(f"field out of range in {text!r}")
        return (h * 3600 + mi * 60 + s) * nominal + ff
    if m := _CLOCK.match(text):
        h = int(m[1] or 0)
        mi, s = int(m[2]), float(m[3])
        if m[1] is not None and mi >= 60 or s >= 60:
            raise ValueError(f"field out of range in {text!r}")
        return _from_seconds(h * 3600 + mi * 60 + s, fps)
    raise ValueError(f"unrecognised time {text!r}; use HH:MM:SS:FF, MM:SS, 2.5s or frames")


def format(frames: int, fps: float) -> str:
    nominal = round(fps)
    secs, ff = divmod(frames, nominal)
    h, rem = divmod(secs, 3600)
    mi, s = divmod(rem, 60)
    return f"{h:02d}:{mi:02d}:{s:02d}:{ff:02d}"
