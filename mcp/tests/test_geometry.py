import json

import pytest

from kdenlive_mcp import geometry


def test_rect():
    assert geometry.rect(0, 0, 1920, 1080) == "0 0 1920 1080 1"
    assert geometry.rect(10.6, 20, 100, 50, 0.5) == "11 20 100 50 0.5"


def test_animate_smooth():
    assert geometry.animate([(0, "0"), (50, "1")], "smooth") == "0~=0;50~=1"


def test_animate_linear_and_discrete():
    assert geometry.animate([(0, "a"), (10, "b")]) == "0=a;10=b"
    assert geometry.animate([(0, "a")], "discrete") == "0|=a"


def test_animate_unknown_interp():
    with pytest.raises(ValueError):
        geometry.animate([(0, "a")], "wobbly")


def test_roto_square():
    spline = json.loads(geometry.roto([(0.25, 0.25), (0.75, 0.25), (0.75, 0.75), (0.25, 0.75)]))
    points = spline["0"]
    assert len(points) == 4 and all(len(p) == 3 for p in points)
    assert points[1] == [[0.75, 0.25], [0.75, 0.25], [0.75, 0.25]]


def test_roto_rejects_out_of_range():
    with pytest.raises(ValueError):
        geometry.roto([(0, 0), (1.5, 0), (0, 1)])


def test_zoom_rect_centered():
    # 2x zoom on 1920x1080 keeps the center: x,y = -960,-540, size 3840x2160
    assert geometry.zoom_rect(2.0, 1920, 1080) == "-960 -540 3840 2160 1"
