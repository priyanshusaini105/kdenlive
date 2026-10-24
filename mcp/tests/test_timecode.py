import pytest

from kdenlive_mcp import timecode

NTSC = 30000 / 1001
FILM = 24000 / 1001


def test_smpte_at_25():
    assert timecode.parse("00:00:01:05", 25) == 30


def test_minutes_seconds():
    assert timecode.parse("1:10", 25) == 1750


def test_seconds_suffix_rounds_half_up():
    assert timecode.parse("2.5s", 25) == 63


def test_frames_forms():
    assert timecode.parse(40, 25) == 40
    assert timecode.parse("40", 25) == 40
    assert timecode.parse("40f", 25) == 40


def test_frame_field_out_of_range():
    with pytest.raises(ValueError):
        timecode.parse("00:00:00:25", 25)


def test_negative_rejected():
    with pytest.raises(ValueError):
        timecode.parse("-1", 25)


def test_garbage_rejected():
    with pytest.raises(ValueError):
        timecode.parse("soon", 25)


def test_ntsc_smpte_uses_nominal_rate():
    assert timecode.parse("00:01:00:00", NTSC) == 1800


def test_ntsc_seconds_use_real_rate():
    assert timecode.parse("60s", NTSC) == 1798


def test_film_smpte():
    assert timecode.parse("00:00:10:00", FILM) == 240


def test_format_roundtrip():
    assert timecode.format(1750, 25) == "00:01:10:00"
