"""E-AC-3 access units and the burst chunk of `_iclforge_player@v1`, with no dependency.

A Python port of what Hearth's server does to an E-AC-3 stream before it goes to a sink
(libs/containers `Eac3BurstPacker`, without its IEC 61937 stuffing): split the stream into
syncframes, join an independent substream's syncframe and its dependents' into an access unit,
collect access units until their blocks total six (1,536 samples, 32 ms at 48 kHz), and write
the burst the sink takes: Pc 0x0015, Pd the payload's length in BYTES, and the payload, which is
the access units' bytes in stream order, with no sync words, byte swapping or stuffing
(planning/hearth-sendspin-extension.md, Burst chunks).

`tests/golden/eac3_bursts.txt` holds the packer's own payload digests for two fixtures, and the
C++ test `iec61937: the E-AC-3 burst vectors ...` holds that file to the packer, so this module
and the packer cannot disagree without a test failing on one side.

Only E-AC-3 (bsid 11 to 16) is handled. An AC-3 syncframe, alone or as the core of a stream with
E-AC-3 extension substreams, raises Eac3Error rather than being packed on a guess.
"""

from __future__ import annotations

import struct
from collections.abc import Iterable, Iterator
from dataclasses import dataclass

SYNCWORD = b"\x0b\x77"
# IEC 61937-3 data type 21 with no data-type-dependent bits.
PC_EAC3 = 0x0015
# `_iclforge_player@v1`'s burst chunk, server to sink.
BURST_MESSAGE_ID = 192
BURST_HEADER_BYTES = 17
# One burst's period on a link is 24,576 bytes and carries Pa, Pb, Pc and Pd ahead of the payload.
MAX_PAYLOAD_BYTES = 24_576 - 8
SAMPLES_PER_BURST = 1536
SAMPLE_RATES = (48_000, 44_100, 32_000)

_BLOCKS_BY_NUMBLKSCOD = (1, 2, 3, 6)
_STRMTYP_INDEPENDENT = 0
_STRMTYP_DEPENDENT = 1
_STRMTYP_CONVERTIBLE = 2  # an independent substream coded earlier as AC-3 (Annex E, E1.3.1)
_HEADER = struct.Struct(">BqIHH")


class Eac3Error(ValueError):
    """A stream or access unit this module cannot pack."""


@dataclass(frozen=True, slots=True)
class Syncframe:
    """What the framing layer reads of one syncframe: where it lies and what it opens."""

    offset: int
    size: int
    strmtyp: int
    substreamid: int
    blocks: int
    sample_rate: int

    @property
    def begins_access_unit(self) -> bool:
        return self.strmtyp in (_STRMTYP_INDEPENDENT, _STRMTYP_CONVERTIBLE)


@dataclass(frozen=True, slots=True)
class Burst:
    """One burst: the payload a chunk carries and the Pc and Pd that describe it."""

    payload: bytes
    pc: int = PC_EAC3
    # What the burst plays for, in samples at the stream's rate.
    samples: int = SAMPLES_PER_BURST

    @property
    def pd(self) -> int:
        return len(self.payload)


def syncframe_at(stream: bytes | memoryview, offset: int) -> Syncframe:
    """The syncframe that begins at `offset`, from its first six bytes."""
    head = bytes(stream[offset : offset + 6])
    if len(head) < 6 or head[:2] != SYNCWORD:
        raise Eac3Error(f"no syncword at byte {offset}")
    bsid = head[5] >> 3
    if not 11 <= bsid <= 16:
        raise Eac3Error(f"bsid {bsid} at byte {offset}: only E-AC-3 (bsid 11 to 16) is packed")
    strmtyp = head[2] >> 6
    if strmtyp == 3:
        raise Eac3Error(f"reserved strmtyp 3 at byte {offset}")
    frmsiz = ((head[2] & 0x07) << 8) | head[3]
    fscod = head[4] >> 6
    if fscod == 3:
        # Reduced sample rates (E2.3.1.3): numblkscod is not sent and the frame is six blocks.
        fscod2 = (head[4] >> 4) & 0x03
        if fscod2 == 3:
            raise Eac3Error(f"reserved fscod2 at byte {offset}")
        blocks = 6
        sample_rate = SAMPLE_RATES[fscod2] // 2
    else:
        blocks = _BLOCKS_BY_NUMBLKSCOD[(head[4] >> 4) & 0x03]
        sample_rate = SAMPLE_RATES[fscod]
    return Syncframe(
        offset=offset,
        size=(frmsiz + 1) * 2,
        strmtyp=strmtyp,
        substreamid=(head[2] >> 3) & 0x07,
        blocks=blocks,
        sample_rate=sample_rate,
    )


def split_syncframes(stream: bytes | memoryview) -> Iterator[Syncframe]:
    """Every syncframe of `stream`, in order. A truncated last frame raises."""
    offset = 0
    while offset < len(stream):
        frame = syncframe_at(stream, offset)
        if offset + frame.size > len(stream):
            raise Eac3Error(f"the syncframe at byte {offset} runs past the end of the stream")
        yield frame
        offset += frame.size


def split_access_units(stream: bytes) -> list[bytes]:
    """The stream's access units: an independent substream's syncframe plus its dependents'.

    Like `iclforge::ac3::split_access_units`, this delimits and does not select: a stream with more
    than one programme yields their units interleaved.
    """
    units: list[bytes] = []
    start = 0
    for frame in split_syncframes(stream):
        if frame.begins_access_unit and frame.offset != start:
            units.append(stream[start : frame.offset])
            start = frame.offset
    if start != len(stream):
        units.append(stream[start:])
    if units and syncframe_at(units[0], 0).strmtyp == _STRMTYP_DEPENDENT:
        raise Eac3Error("the stream begins with a dependent substream")
    return units


class Eac3BurstBuilder:
    """Collects access units into bursts, as `Eac3BurstPacker` does.

    `push` takes whole access units and returns a burst once their blocks total six, else None.
    Access units pending when the stream ends are not a burst; `pending` says whether any are.
    """

    def __init__(self) -> None:
        self._pending = bytearray()
        self._blocks = 0
        self._sample_rate: int | None = None

    @property
    def pending(self) -> bool:
        return bool(self._pending)

    @property
    def sample_rate(self) -> int | None:
        """The rate of the first access unit pushed, which a stream keeps."""
        return self._sample_rate

    def reset(self) -> None:
        self._pending.clear()
        self._blocks = 0

    def push(self, access_unit: bytes) -> Burst | None:
        frame = syncframe_at(access_unit, 0)
        if len(access_unit) % 2 != 0:
            raise Eac3Error("an access unit is a whole number of 16-bit words")
        if len(self._pending) + len(access_unit) > MAX_PAYLOAD_BYTES:
            self.reset()
            raise Eac3Error("access units over a burst's capacity")
        if self._sample_rate is None:
            self._sample_rate = frame.sample_rate
        self._pending += access_unit
        self._blocks += frame.blocks
        if self._blocks < 6:
            return None
        payload = bytes(self._pending)
        samples = self._blocks * 256
        self.reset()
        return Burst(payload=payload, samples=samples)


def build_bursts(stream: bytes) -> list[Burst]:
    """All the complete bursts of a raw E-AC-3 stream."""
    builder = Eac3BurstBuilder()
    bursts = []
    for unit in split_access_units(stream):
        if (burst := builder.push(unit)) is not None:
            bursts.append(burst)
    return bursts


def pack_burst_chunk(timestamp_us: int, send_ahead_us: int, burst: Burst) -> bytes:
    """The binary message of `_iclforge_player@v1`: id 192, timestamp, send_ahead, Pc, Pd, payload.

    Integers are big-endian. `send_ahead_us` saturates to the uint32 range, as `player@v1`'s does.
    """
    if burst.pd > 0xFFFF:
        raise Eac3Error("Pd is sixteen bits")
    send_ahead = max(0, min(send_ahead_us, 0xFFFFFFFF))
    return (
        _HEADER.pack(BURST_MESSAGE_ID, timestamp_us, send_ahead, burst.pc, burst.pd) + burst.payload
    )


@dataclass(frozen=True, slots=True)
class BurstChunk:
    """A burst chunk taken apart, for tests and for a harness that checks what it sent."""

    timestamp_us: int
    send_ahead_us: int
    pc: int
    pd: int
    payload: bytes


def parse_burst_chunk(message: bytes) -> BurstChunk:
    """Read what `pack_burst_chunk` wrote, checking what one chunk can say of itself."""
    if len(message) < BURST_HEADER_BYTES + 1:
        raise Eac3Error("shorter than a burst chunk's header and a payload")
    message_id, timestamp, send_ahead, pc, pd = _HEADER.unpack_from(message)
    if message_id != BURST_MESSAGE_ID:
        raise Eac3Error(f"message id {message_id}, not {BURST_MESSAGE_ID}")
    payload = message[BURST_HEADER_BYTES:]
    if pc & 0x7F == PC_EAC3 and pd != len(payload):
        raise Eac3Error(f"Pd {pd} is not the payload's {len(payload)} bytes")
    return BurstChunk(timestamp, send_ahead, pc, pd, payload)


def timeline(start_us: int, sample_rate: int, samples: Iterable[int]) -> Iterator[int]:
    """Each burst's timestamp, from the count of samples before it, never from summed durations.

    `samples` is each burst's length in samples; the first timestamp is `start_us`. Integer
    arithmetic, rounding down, so the timeline does not drift over a long stream.
    """
    count = 0
    for length in samples:
        yield start_us + (count * 1_000_000) // sample_rate
        count += length
