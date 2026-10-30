import sys
from pathlib import Path
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from airwindows_frames import validate_metadata


def frame():
    return dict(ok=True, protocol=1, format="ppm", source="airplay", receiver_pid=123,
                instance="987654321", generation=1, sequence=10, width=498, height=1080,
                image_bytes=1613536, received_tick_ms=2000, decoded_tick_ms=2010)


def test_accept_fresh_frame():
    validate_metadata(frame(), 2050, 123)


@pytest.mark.parametrize("change", [
    dict(ok=False, error="paused"), dict(source="desktop"), dict(source="selftest"),
    dict(receiver_pid=456), dict(generation=0), dict(width=99999), dict(image_bytes=90_000_000),
    dict(received_tick_ms=999), dict(decoded_tick_ms=5000), dict(received_tick_ms=2020),
])
def test_reject_unusable_or_misbound_frame(change):
    with pytest.raises(ValueError):
        validate_metadata({**frame(), **change}, 2050, 123)


def test_selftest_requires_explicit_opt_in():
    validate_metadata({**frame(), "source": "selftest"}, 2050, 123, allow_selftest=True)


def test_retained_image_does_not_bypass_legacy_freshness_check():
    # rc.2 adds an opt-in retained snapshot to iosctl; this reader stays strict.
    retained = {**frame(), "status_tick_ms": 5000, "mirror_active": True,
                "mirror_paused": False, "frame_age_ms": 3000, "frame_fresh": False}
    with pytest.raises(ValueError, match="stale"):
        validate_metadata(retained, 5000, 123)


def test_additional_diagnostics_do_not_break_fresh_capture():
    validate_metadata({**frame(), "status_tick_ms": 2050, "mirror_active": True,
                       "mirror_paused": False, "frame_age_ms": 50, "frame_fresh": True}, 2050, 123)
