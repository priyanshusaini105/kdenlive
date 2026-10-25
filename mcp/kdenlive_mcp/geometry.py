"""Value strings for Kdenlive's geometry parameters (qtblend rect, MLT animation, rotoscoping spline)."""

import json

# MLT animation separators: none = linear, | = discrete, ~ = smooth, $ = smooth natural
_INTERP = {"linear": "", "discrete": "|", "smooth": "~", "smooth_natural": "$"}


def rect(x: float, y: float, w: float, h: float, opacity: float = 1.0) -> str:
    """qtblend/composite 'rect' value: "x y w h opacity" in project pixels."""
    return f"{round(x)} {round(y)} {round(w)} {round(h)} {opacity:g}"


def zoom_rect(scale: float, width: int, height: int) -> str:
    """A rect that scales the frame around its center (scale 2.0 = 2x zoom in)."""
    w, h = width * scale, height * scale
    return rect((width - w) / 2, (height - h) / 2, w, h)


def animate(frames_values: list[tuple[int, str]], interp: str = "linear") -> str:
    """MLT animation string from (item-local frame, value) pairs, e.g. "0~=0;50~=1"."""
    if interp not in _INTERP:
        raise ValueError(f"interp must be one of {', '.join(_INTERP)}")
    sep = _INTERP[interp]
    return ";".join(f"{frame}{sep}={value}" for frame, value in frames_values)


def roto(points: list[tuple[float, float]], frame: int = 0, duration: int = 1) -> str:
    """Rotoscoping 'spline' JSON for a polygon mask; points are 0..1 of the frame, handles sit on the points.
    Kdenlive zero-pads the frame key to the digit count of the clip duration."""
    if len(points) < 3:
        raise ValueError("a mask needs at least 3 points")
    if any(not (0 <= x <= 1 and 0 <= y <= 1) for x, y in points):
        raise ValueError("mask points must be within 0..1 (fraction of the frame)")
    key = str(frame).rjust(len(str(max(duration, 1))), "0")
    return json.dumps({key: [[[x, y], [x, y], [x, y]] for x, y in points]})
