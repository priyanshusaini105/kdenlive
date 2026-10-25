"""Timecode <-> frame conversion, matching what Kdenlive displays (utils/timecode.cpp).

HH:MM:SS:FF at 29.97 (30000/1001) is drop-frame, written HH:MM:SS,FF by Kdenlive.
Other rates: frames = round((h*3600 + m*60 + s) * fps) + ff.
Clock forms (MM:SS, HH:MM:SS.mmm, 12.5s) use the real rate.
"""

import math
import re

_FRAMES = re.compile(r"^(\d+)f?$")
_SECONDS = re.compile(r"^(\d+(?:\.\d+)?)s$")
_CLOCK = re.compile(r"^(?:(\d+):)?(\d+):(\d+(?:\.\d+)?)$")
_SMPTE = re.compile(r"^(\d+):(\d+):(\d+)[:;,](\d+)$")


def _drop_frame(fps: float) -> bool:
    return math.isclose(fps, 30000 / 1001, rel_tol=1e-9)


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
        if _drop_frame(fps):
            drop = round(fps * 0.066666)
            total_minutes = 60 * h + mi
            return nominal * (3600 * h + 60 * mi + s) + ff - drop * (total_minutes - total_minutes // 10)
        return round((h * 3600 + mi * 60 + s) * fps + ff)
    if m := _CLOCK.match(text):
        h = int(m[1] or 0)
        mi, s = int(m[2]), float(m[3])
        if m[1] is not None and mi >= 60 or s >= 60:
            raise ValueError(f"field out of range in {text!r}")
        return _from_seconds(h * 3600 + mi * 60 + s, fps)
    raise ValueError(f"unrecognised time {text!r}; use HH:MM:SS:FF, MM:SS, 2.5s or frames")


def format(frames: int, fps: float) -> str:
    nominal = round(fps)
    if _drop_frame(fps):
        drop = round(fps * 0.066666)
        per10 = round(fps * 600)
        d, m = divmod(frames, per10)
        frames += drop * 9 * d + (drop * ((m - drop) // (round(fps * 60) - drop)) if m > drop else 0)
        secs, ff = divmod(frames, nominal)
        return f"{secs // 3600:02d}:{secs // 60 % 60:02d}:{secs % 60:02d},{ff:02d}"
    hours = int(frames / (fps * 3600))
    frames -= math.floor(hours * 3600 * fps)
    minutes = int(frames / (fps * 60))
    frames -= math.floor(minutes * 60 * fps)
    seconds = int(frames / fps)
    frames -= math.ceil(seconds * fps)
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}:{frames:02d}"
