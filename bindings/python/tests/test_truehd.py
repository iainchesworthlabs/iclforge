"""iclforge.truehd - the TrueHD and MLP codec through the Python binding.

The committed FFmpeg fixtures (testdata/truehd, tools/generators/gen_truehd_ffmpeg.py) are the
oracle: the codec is lossless, so a decode returns the integers the stream was made from and the
SHA-256 of what FFmpeg's strict decode of the same file gives is the SHA-256 of ours. The same
equality is held to `forge decode` when a forge executable is named (ICLFORGE_FORGE, or one on
PATH), so the binding, the command line and FFmpeg all give the same samples.
"""

import hashlib
import json
import os
import shutil
import struct
import subprocess
from pathlib import Path

import iclforge as ac3
import numpy as np
import pytest

if not hasattr(ac3, "truehd"):
    pytest.skip("this build has no iclforge.truehd", allow_module_level=True)

truehd = ac3.truehd
FIXTURES = Path(__file__).resolve().parents[3] / "testdata" / "truehd"
MANIFEST = json.loads((FIXTURES / "manifest.json").read_text(encoding="utf-8"))["fixtures"]
IDS = [fixture["name"] for fixture in MANIFEST]


def stream_of(fixture: dict) -> bytes:
    return (FIXTURES / fixture["file"]).read_bytes()


def ffmpeg_s32le(pcm: np.ndarray) -> bytes:
    """What FFmpeg's `-f s32le` writes for 24-bit samples: each shifted left by 8, little-endian."""
    return (pcm.astype("<i4") << 8).astype("<i4").tobytes()


@pytest.fixture(params=MANIFEST, ids=IDS)
def fixture(request) -> dict:
    return request.param


def test_decode_gives_ffmpegs_samples(fixture: dict) -> None:
    decoded = truehd.decode(stream_of(fixture))
    assert decoded.pcm.dtype == np.int32
    assert decoded.sample_rate == fixture["sample_rate"]
    assert decoded.pcm.shape == (fixture["pcm_samples"], fixture["channels"])
    assert decoded.channels == fixture["channels"]
    assert hashlib.sha256(ffmpeg_s32le(decoded.pcm)).hexdigest() == fixture["pcm_sha256"]
    assert decoded.skipped_bytes == 0
    assert decoded.leading_bytes == 0
    # 24 bits, sign-extended.
    assert int(decoded.pcm.min()) >= -(1 << 23)
    assert int(decoded.pcm.max()) < (1 << 23)


def test_probe_and_scan_state_what_the_manifest_states(fixture: dict) -> None:
    stream = stream_of(fixture)
    info = truehd.probe(stream)
    assert info.kind == fixture["codec"]
    assert info.sample_rate == fixture["sample_rate"]
    assert info.access_units == fixture["access_units"]
    assert info.channels[info.substreams - 1] == fixture["channels"]
    assert info.first_unit_offset == 0

    scan = truehd.scan(stream)
    assert scan.kind == fixture["codec"]
    assert scan.units == fixture["access_units"]
    assert scan.samples == fixture["pcm_samples"]
    assert scan.unit_bytes == fixture["stream_bytes"] == len(stream)
    assert scan.skipped_bytes == 0
    assert scan.duration_seconds == pytest.approx(fixture["pcm_samples"] / fixture["sample_rate"])
    assert scan.restarts[0].unit == 0 and scan.restarts[0].offset == 0
    assert scan.samples + scan.padding_samples == scan.units * scan.samples_per_unit


def test_a_decode_from_the_restart_for_a_sample_gives_that_sample(fixture: dict) -> None:
    stream = stream_of(fixture)
    scan = truehd.scan(stream, restart_spacing=1)
    whole = truehd.decode(stream).pcm
    for sample in (0, 1, scan.samples // 3, scan.samples // 2 + 7, scan.samples - 1):
        restart = scan.restart_for_sample(sample)
        assert restart is not None
        first = scan.unit_start_sample(restart.unit)
        assert first <= sample
        joined = truehd.decode(stream[restart.offset :]).pcm
        assert np.array_equal(joined[sample - first], whole[sample])


def test_the_scan_places_time_and_samples() -> None:
    fixture = next(f for f in MANIFEST if f["name"] == "truehd_st48_s24")
    scan = truehd.scan(stream_of(fixture), restart_spacing=64)
    assert [r.unit for r in scan.restarts] == [0, 64, 128, 192, 256]
    assert scan.samples_per_unit == 40
    assert scan.unit_at_sample(39) == 0
    assert scan.unit_at_sample(40) == 1
    assert scan.unit_at_sample(scan.samples) is None
    assert scan.restart_for_sample(scan.samples) is None
    assert scan.restart_for_sample(64 * 40).unit == 64
    assert scan.sample_at_seconds(0.125) == 6000
    assert scan.sample_at_seconds(-2.0) == 0
    # A spacing of 0 keeps every unit with a major sync, as 1 does.
    assert len(truehd.scan(stream_of(fixture), restart_spacing=0).restarts) == 19


def test_units_one_at_a_time_give_the_whole_decode(fixture: dict) -> None:
    stream = stream_of(fixture)
    units, skipped = truehd.access_units(stream)
    assert skipped == 0
    assert len(units) == fixture["access_units"]
    assert b"".join(units) == stream

    decoder = truehd.Decoder()
    frames = []
    for unit in units:
        decoded = decoder.decode(unit)
        assert decoded is not None  # every fixture starts at a major sync
        assert decoded.pcm.shape == (decoded.samples, decoded.channels)
        frames.append(decoded.pcm)
    assert np.array_equal(np.concatenate(frames), truehd.decode(stream).pcm)
    assert decoder.units == len(units) == decoder.units_decoded
    assert decoder.check_failures == 0


def test_a_decoder_that_joins_late_waits_for_a_restart_and_a_lost_unit_makes_it_wait() -> None:
    fixture = next(f for f in MANIFEST if f["name"] == "truehd_st48_s24")
    units, _ = truehd.access_units(stream_of(fixture))
    scan = truehd.scan(stream_of(fixture), restart_spacing=1)
    restart_units = [r.unit for r in scan.restarts]
    assert restart_units[:3] == [0, 16, 32]

    decoder = truehd.Decoder()
    # Joined at unit 5, between restarts: nothing to give until the major sync at 16.
    assert all(decoder.decode(units[n]) is None for n in range(5, 16))
    assert decoder.decode(units[16]) is not None
    assert decoder.decode(units[17]) is not None

    decoder.lost_data()
    assert decoder.decode(units[18]) is None
    assert decoder.decode(units[32]) is not None
    decoder.reset()
    assert decoder.decode(units[33]) is None


def test_presentation_picks_the_substreams_read() -> None:
    fixture = next(f for f in MANIFEST if f["name"] == "truehd_m51_s24")
    stream = stream_of(fixture)
    best = truehd.decode(stream, presentation=truehd.Presentation.kBest)
    assert best.channels == fixture["channels"]
    assert len(best.speakers) in (0, best.channels)
    two = truehd.decode(stream, presentation=truehd.Presentation.k2ch)
    assert two.channels == 2
    assert two.pcm.shape[0] == best.pcm.shape[0]


def test_bytes_ahead_of_the_stream_are_counted_not_decoded() -> None:
    fixture = next(f for f in MANIFEST if f["name"] == "truehd_st48_s24")
    stream = stream_of(fixture)
    clean = truehd.decode(stream)
    shifted = truehd.decode(b"\x55" * 37 + stream)
    assert shifted.leading_bytes == 37 and shifted.skipped_bytes == 37
    assert np.array_equal(shifted.pcm, clean.pcm)
    assert truehd.scan(b"\x55" * 37 + stream).restarts[0].offset == 37
    assert truehd.probe(b"\x55" * 37 + stream).first_unit_offset == 37


def test_what_is_not_a_stream_raises_truehd_error() -> None:
    junk = b"\x5a" * 4096
    for call in (truehd.probe, truehd.scan, truehd.decode, truehd.access_units):
        with pytest.raises(truehd.TruehdError) as raised:
            call(junk)
        assert raised.value.kind == truehd.ErrorKind.kBadSync
        assert isinstance(raised.value, ValueError)
        assert raised.type is ac3.TruehdError
    with pytest.raises(truehd.TruehdError):
        truehd.Decoder().decode(junk[:64])


def test_a_corrupt_unit_stops_a_strict_decode_and_is_counted_by_a_lenient_one() -> None:
    fixture = next(f for f in MANIFEST if f["name"] == "truehd_st48_s24")
    units, _ = truehd.access_units(stream_of(fixture))
    damaged = bytearray(units[3])
    damaged[0] ^= 0xF0  # the check nibble of the unit's header (TH spec 3.2)
    strict = truehd.Decoder()
    for unit in units[:3]:
        assert strict.decode(unit) is not None
    with pytest.raises(truehd.TruehdError) as raised:
        strict.decode(bytes(damaged))
    assert raised.value.kind == truehd.ErrorKind.kBadCheckNibble
    assert strict.decode(units[4]) is None  # waiting for the next restart header

    lenient = truehd.Decoder(lenient=True)
    for unit in units[:3]:
        lenient.decode(unit)
    assert lenient.decode(bytes(damaged)) is not None
    assert lenient.check_failures >= 1


def test_the_buffer_protocol_is_accepted() -> None:
    fixture = MANIFEST[0]
    stream = stream_of(fixture)
    reference = truehd.decode(stream).pcm
    assert np.array_equal(truehd.decode(bytearray(stream)).pcm, reference)
    assert np.array_equal(truehd.decode(memoryview(stream)).pcm, reference)
    assert np.array_equal(truehd.decode(np.frombuffer(stream, dtype=np.uint8)).pcm, reference)


# --- the same samples as `forge decode` -------------------------------------------------------


def _forge() -> str | None:
    named = os.environ.get("ICLFORGE_FORGE")
    if named:
        return named if Path(named).is_file() else None
    return shutil.which("forge")


def _wav_samples(path: Path) -> tuple[int, int, np.ndarray]:
    """(sample_rate, bits, signed integers shaped (frames, channels)) of a PCM WAV file."""
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    position, fmt, pcm = 12, None, None
    while position + 8 <= len(data):
        tag, size = data[position : position + 4], struct.unpack_from("<I", data, position + 4)[0]
        body = data[position + 8 : position + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif tag == b"data":
            pcm = body
        position += 8 + size + (size & 1)
    assert fmt is not None and pcm is not None
    _, channels, rate, _, _, bits = fmt
    width = bits // 8
    raw = np.frombuffer(pcm, dtype=np.uint8)[: len(pcm) // width * width].reshape(-1, width)
    padded = np.zeros((raw.shape[0], 4), dtype=np.uint8)
    padded[:, :width] = raw
    values = padded.view("<u4")[:, 0].astype(np.int64)
    values = np.where(values >= 1 << (bits - 1), values - (1 << bits), values)
    return rate, bits, values.reshape(-1, channels).astype(np.int32)


@pytest.mark.skipif(_forge() is None, reason="set ICLFORGE_FORGE to a forge executable to run")
@pytest.mark.parametrize(
    "name", ["truehd_st48_s24", "truehd_m51_s24", "truehd_st48_s16", "mlp_st48_s24"]
)
def test_decode_equals_forge_decode(name: str, tmp_path: Path) -> None:
    fixture = next(f for f in MANIFEST if f["name"] == name)
    out = tmp_path / "forge.wav"
    run = subprocess.run(
        [_forge(), "decode", str(FIXTURES / fixture["file"]), str(out)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert run.returncode == 0, run.stderr
    rate, bits, forge_pcm = _wav_samples(out)
    ours = truehd.decode(stream_of(fixture))
    assert rate == ours.sample_rate
    # The WAV holds the stream's own width; ours is 24 bits with a narrower stream's values scaled.
    assert forge_pcm.shape == ours.pcm.shape
    assert np.array_equal(forge_pcm.astype(np.int64) << (24 - bits), ours.pcm.astype(np.int64))
