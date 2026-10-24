"""The burst builder against the C++ packer's vectors, and the chunk's wire form."""

from __future__ import annotations

from pathlib import Path

import pytest
from ma_role import eac3

REPOSITORY = Path(__file__).resolve().parents[4]
VECTORS = Path(__file__).with_name("golden") / "eac3_bursts.txt"
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


def fnv1a(data: bytes, hash_: int = FNV_OFFSET) -> int:
    for byte in data:
        hash_ = ((hash_ ^ byte) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return hash_


def read_vectors() -> dict[str, tuple[tuple[int, int, int, int], list[tuple[int, int, int]]]]:
    """Per fixture: (bursts, bytes, whole-stream digest) and each burst's (Pc, Pd, digest)."""
    vectors: dict[str, tuple[tuple[int, int, int, int], list[tuple[int, int, int]]]] = {}
    current: list[tuple[int, int, int]] | None = None
    for line in VECTORS.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        fields = line.split()
        if fields[0] == "stream":
            current = []
            vectors[fields[1]] = ((int(fields[2]), int(fields[3]), int(fields[4], 16), 0), current)
        else:
            assert current is not None and fields[0] == "burst"
            current.append((int(fields[1], 16), int(fields[2]), int(fields[3], 16)))
    return vectors


VECTOR_SET = read_vectors()


@pytest.mark.parametrize("fixture", sorted(VECTOR_SET))
def test_bursts_equal_the_cpp_packers(fixture: str) -> None:
    (count, total, whole, _), expected = VECTOR_SET[fixture]
    bursts = eac3.build_bursts((REPOSITORY / fixture).read_bytes())
    assert len(bursts) == count == len(expected)
    digest = FNV_OFFSET
    for burst, (pc, pd, payload_digest) in zip(bursts, expected, strict=True):
        assert (burst.pc, burst.pd) == (pc, pd)
        assert fnv1a(burst.payload) == payload_digest
        digest = fnv1a(burst.payload, digest)
        assert burst.samples == eac3.SAMPLES_PER_BURST
    assert sum(burst.pd for burst in bursts) == total
    assert digest == whole


def test_access_units_hold_a_dependent_substream_with_its_parent() -> None:
    stream = (REPOSITORY / "firmware/hearth-sink/www/514-joc-dee.ec3").read_bytes()
    units = eac3.split_access_units(stream)
    assert b"".join(units) == stream
    for unit in units:
        frames = list(eac3.split_syncframes(unit))
        assert frames[0].begins_access_unit
        assert all(not frame.begins_access_unit for frame in frames[1:])


def frame_bytes(*, strmtyp: int = 0, blocks_code: int = 3, words: int = 8, bsid: int = 16) -> bytes:
    """A syncframe of `words` 16-bit words with the header fields the framing layer reads."""
    frmsiz = words - 1
    body = bytearray(words * 2)
    body[0:2] = eac3.SYNCWORD
    body[2] = (strmtyp << 6) | (frmsiz >> 8)
    body[3] = frmsiz & 0xFF
    body[4] = (0 << 6) | (blocks_code << 4)
    body[5] = bsid << 3
    return bytes(body)


def test_blocks_accumulate_to_six() -> None:
    builder = eac3.Eac3BurstBuilder()
    half = frame_bytes(blocks_code=2, words=8)  # three blocks
    assert builder.push(half) is None
    assert builder.pending
    burst = builder.push(half)
    assert burst is not None
    assert burst.payload == half + half
    assert burst.pd == 32
    assert burst.samples == 1536
    assert not builder.pending


def test_a_dependent_substream_joins_its_parent() -> None:
    parent = frame_bytes(words=10)
    dependent = frame_bytes(strmtyp=1, words=6)
    units = eac3.split_access_units(parent + dependent + parent)
    assert units == [parent + dependent, parent]


@pytest.mark.parametrize(
    "stream",
    [
        b"",
        b"\x00" * 8,
        frame_bytes(bsid=8),
        frame_bytes(strmtyp=1),
        frame_bytes(words=8)[:-2],
    ],
)
def test_streams_it_cannot_pack_are_refused(stream: bytes) -> None:
    if stream == b"":
        assert eac3.split_access_units(stream) == []
        return
    with pytest.raises(eac3.Eac3Error):
        eac3.split_access_units(stream)


def test_the_chunk_header_is_seventeen_big_endian_bytes() -> None:
    burst = eac3.Burst(payload=b"\x0b\x77" + bytes(14))
    message = eac3.pack_burst_chunk(0x0102030405060708, 0xA0B0C0D0, burst)
    assert message[:17] == bytes.fromhex("c0 0102030405060708 a0b0c0d0 0015 0010")
    assert message[17:] == burst.payload
    parsed = eac3.parse_burst_chunk(message)
    assert (parsed.timestamp_us, parsed.send_ahead_us, parsed.pc, parsed.pd) == (
        0x0102030405060708,
        0xA0B0C0D0,
        0x15,
        16,
    )


def test_send_ahead_saturates_and_never_wraps() -> None:
    burst = eac3.Burst(payload=bytes(16))
    assert eac3.parse_burst_chunk(eac3.pack_burst_chunk(1, -5, burst)).send_ahead_us == 0
    assert (
        eac3.parse_burst_chunk(eac3.pack_burst_chunk(1, 1 << 40, burst)).send_ahead_us == 0xFFFFFFFF
    )


def test_a_chunk_that_contradicts_itself_is_refused() -> None:
    message = bytearray(eac3.pack_burst_chunk(1, 0, eac3.Burst(payload=bytes(16))))
    message[16] = 17  # Pd
    with pytest.raises(eac3.Eac3Error):
        eac3.parse_burst_chunk(bytes(message))
    with pytest.raises(eac3.Eac3Error):
        eac3.parse_burst_chunk(b"\x04" + bytes(20))


def test_the_timeline_counts_samples_so_it_does_not_drift() -> None:
    stamps = list(eac3.timeline(1_000, 48_000, [1536] * 3))
    assert stamps == [1_000, 1_000 + 32_000, 1_000 + 64_000]
    # 44.1 kHz bursts are not a whole number of microseconds: the sum of rounded durations
    # would drift, the sample count does not.
    stamps = list(eac3.timeline(0, 44_100, [1536] * 1000))
    assert stamps[-1] == (999 * 1536 * 1_000_000) // 44_100
