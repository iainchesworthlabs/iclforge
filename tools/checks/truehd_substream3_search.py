#!/usr/bin/env python3
"""Which reading of the block header parses the object substream (substream 3) of a TrueHD stream.

For the owner to run on a disc excerpt on the owner's machine (planning/work-packages/WP1, step 1).
It reads nothing but the stream given and prints parameters and counts: no sample, no residual bit.
The stream is an elementary TrueHD stream (a `.thd`, or `ffmpeg -c copy` out of the disc's `.m2ts`).

    truehd_substream3_search.py STREAM [--units N]

Substream 3 is parsed under each combination of the readings the specification leaves open
(libs/truehd/ERRATA.md, entries 8 to 10 and 60): how many coefficients a matrix sends, whether a
matrix sends a noise shift, whether the FIR filter has a state flag, how many output shifts and
quantiser steps a block header sends, and whether extra bits follow the restart header. A reading
is right when every block of every segment parses to the end of the segment, and for the tied best
readings the script then decodes the first restart intervals of all four substreams and prints
whether each interval's lossless check passes.
"""

import argparse
import collections
import importlib.util
import itertools
import pathlib
import sys

REFERENCE = pathlib.Path(__file__).resolve().parents[1] / "references" / "truehd_syntax.py"
spec = importlib.util.spec_from_file_location("truehd_syntax", REFERENCE)
ref = importlib.util.module_from_spec(spec)
sys.modules["truehd_syntax"] = ref
spec.loader.exec_module(ref)

READINGS = {
    # how many coefficient-present bits a matrix sends
    "coeffs": ("matrix+noise", "matrix", "chan+noise", "chan"),
    # when a matrix ends with a 4 bit noise shift
    "noise_shift": ("by_sync", "always", "never"),
    # the FIR filter's state flag (ERRATA 9) and the IIR filter's
    "fir_state_flag": (True, False),
    # how many output shifts and how many quantiser steps the header sends
    "shift_count": ("matrix", "chan"),
    "quant_count": ("chan", "matrix"),
    # bits between the restart header's CRC and the first block header (0 in every other substream)
    "gap_bits": tuple(range(0, 33)),
}


class Variant(ref.Substream):
    """Substream 3 with one reading of each open point."""

    def __init__(self, index, reading):
        super().__init__(index)
        self.reading = reading

    def restart(self, bits, segment):
        super().restart(bits, segment)
        bits.read(self.reading["gap_bits"])

    def matrix_count(self):
        kind = self.reading["coeffs"]
        base = self.max_matrix_chan + 1 if kind.startswith("matrix") else self.max_chan + 1
        return base + (ref.NOISE_CHANNELS if kind.endswith("+noise") else 0)

    def has_noise_shift(self):
        mode = self.reading["noise_shift"]
        return self.noise_type if mode == "by_sync" else mode == "always"

    def read_matrices(self, bits):
        matrices = []
        for _ in range(bits.read(4)):
            out = bits.read(4)
            frac_bits = bits.read(4)
            lsb_bypass = bits.read(1)
            count = self.matrix_count()
            columns = self.max_matrix_chan + 1 + ref.NOISE_CHANNELS
            coeffs = [0] * max(columns, count)
            for channel in range(count):
                if bits.read(1):
                    coeffs[channel] = bits.signed(frac_bits + 2) << (ref.MATRIX_SHIFT - frac_bits)
            noise_shift = bits.read(4) if self.has_noise_shift() else 0
            matrices.append((out, coeffs[:columns], lsb_bypass, noise_shift))
        return matrices

    def read_filter(self, bits, max_order):
        flt = ref.Filter()
        order = bits.read(4)
        if order > max_order:
            raise ref.StreamError("filter order")
        flt.state = None
        flt.state_shift = 0
        if order:
            flt.shift = bits.read(4)
            coeff_bits = bits.read(5)
            coeff_shift = bits.read(3)
            if coeff_bits < 1 or coeff_bits > 16:
                raise ref.StreamError("coefficient width")
            flt.coeff = [bits.signed(coeff_bits) << coeff_shift for _ in range(order)]
            if (self.reading["fir_state_flag"] or max_order == 4) and bits.read(1):
                state_bits = bits.read(4)
                flt.state_shift = bits.read(4)
                flt.state = [
                    bits.signed(state_bits) << flt.state_shift if state_bits else 0
                    for _ in range(order)
                ]
        return flt

    def block_header(self, bits):
        if bits.read(1):
            self.flags = bits.read(8)
        flags = self.flags
        if flags & 0x80 and bits.read(1):
            self.block_size = bits.read(9)
        if flags & 0x40 and bits.read(1):
            self.matrices = self.read_matrices(bits)
        if flags & 0x20 and bits.read(1):
            count = (
                self.max_matrix_chan if self.reading["shift_count"] == "matrix" else self.max_chan
            )
            self.output_shift = [bits.signed(4) for _ in range(count + 1)]
            self.output_shift += [0] * (16 - len(self.output_shift))
        if flags & 0x10 and bits.read(1):
            count = self.max_chan if self.reading["quant_count"] == "chan" else self.max_matrix_chan
            self.quant = [bits.read(4) for _ in range(count + 1)]
            self.quant += [0] * (16 - len(self.quant))
        for ch in range(self.min_chan, self.max_chan + 1):
            if not bits.read(1):
                continue
            channel = self.channels[ch]
            if flags & 0x08 and bits.read(1):
                channel.fir = self.read_filter(bits, 15)
                if channel.fir.state is not None:
                    raise ref.StreamError("FIR filter with state")
            if flags & 0x04 and bits.read(1):
                channel.iir = self.read_filter(bits, 15)
                if channel.iir.state is not None:
                    for lag, value in enumerate(channel.iir.state):
                        channel.iir_history[lag] = value
            if flags & 0x02 and bits.read(1):
                channel.huff_offset = bits.signed(15)
            channel.huff_type = bits.read(2)
            channel.huff_lsbs = bits.read(5)
            if len(channel.fir.coeff) + len(channel.iir.coeff) > 8:
                raise ref.StreamError("filter orders")


def units_of(data, limit):
    """Yield (restart-bearing, unit, directory entries, segment offsets, substream count)."""
    count = 0
    for number, unit in enumerate(ref.access_units(data)):
        if number >= limit:
            return
        offset = 4
        if unit[offset : offset + 3] == ref.SYNC_WORD:
            count = unit[offset + 16] >> 4
            # The 16-channel extension makes the major sync longer than 28 bytes (32 on the discs).
            size = ref.major_sync_size(unit, offset)
            offset += size
        entries = []
        for _ in range(count):
            word = int.from_bytes(unit[offset : offset + 2], "big")
            entries.append(
                {"extra": word >> 15, "crc_present": (word >> 13) & 1, "end": word & 0xFFF}
            )
            offset += 2 + (2 if word >> 15 else 0)
        yield number, unit, entries, offset, count


def segments_of(unit, entries, offset):
    previous = 0
    for entry in entries:
        yield entry, unit[offset + previous * 2 : offset + entry["end"] * 2]
        previous = entry["end"]


def parse_segment(sub, entry, segment):
    """Parse every block of a segment; return the bits left after the last block, or raise."""
    # A reading that gives a matrix a noise shift under generator 0 has no buffer to draw from;
    # the draw changes samples and not the parse.
    zero_buffer = [0] * sub.noise_size
    payload = segment[:-2] if entry["crc_present"] else segment
    bits = ref.Bits(payload)
    rows = []
    first = 0
    sub.noise_buffer = None
    while True:
        if bits.read(1):
            if bits.read(1):
                sub.restart(bits, payload)
            if not sub.running:
                raise ref.StreamError("no restart header")
            sub.block_header(bits)
        elif not sub.running:
            raise ref.StreamError("no restart header")
        while len(rows) < first + sub.block_size:
            rows.append([0] * (16 + ref.NOISE_CHANNELS))
        if not sub.noise_type and sub.noise_buffer is None:
            sub.noise_buffer = zero_buffer
        ref.decode_block(sub, bits, rows, first)
        first += sub.block_size
        if bits.read(1):
            break
    sub.interval = None  # the samples are not decoded here, so the fold means nothing
    return bits.left(), None


def try_reading(data, reading, units):
    sub = Variant(3, reading)
    sub.running = False
    ok = 0
    tried = 0
    first_error = None
    started = False
    for _number, unit, entries, offset, count in units_of(data, units):
        if count < 4:
            continue
        segments = list(segments_of(unit, entries, offset))
        entry, segment = segments[3]
        has_restart = segment[0] & 0xC0 == 0xC0
        if not started and not has_restart:
            continue
        started = True
        tried += 1
        try:
            left, _tail = parse_segment(sub, entry, segment)
        except (ref.StreamError, IndexError, ValueError) as error:
            first_error = first_error or f"unit {_number}: {error}"
            break
        # What follows the last block is padding to the word, terminatorB (16 bits) and padding.
        if left > 40:
            first_error = first_error or f"unit {_number}: {left} bits left after the last block"
            break
        ok += 1
    return ok, tried, first_error


def describe_restart(data, units):
    """The restart header of substream 3 and the bits after it, as parameters of the stream."""
    for number, unit, entries, offset, count in units_of(data, units):
        if count < 4:
            continue
        segments = list(segments_of(unit, entries, offset))
        entry, segment = segments[3]
        if segment[0] & 0xC0 != 0xC0:
            continue
        sub = ref.Substream(3)
        bits = ref.Bits(segment[:-2] if entry["crc_present"] else segment, 2)
        sub.restart(bits, segment[:-2] if entry["crc_present"] else segment)
        return number, sub, bits, segment
    return None


def full_decode(data, reading, units):
    """Decode all four substreams for the first restart interval of substream 3."""
    subs = [ref.Substream(i) for i in range(3)] + [Variant(3, reading)]
    results = collections.Counter()
    started = False
    for _number, unit, entries, offset, count in units_of(data, units):
        if count < 4:
            continue
        for sub in subs:
            sub.noise_size = 64
        rows = []
        segments = list(segments_of(unit, entries, offset))
        if not started and segments[3][1][0] & 0xC0 != 0xC0:
            continue
        started = True
        try:
            for index, (entry, segment) in enumerate(segments):
                # A restart header carries the check of the interval that ended: it is a failure of
                # the reading when the check does not match, and the restart raises.
                ref.decode_segment(subs[index], entry, segment, rows)
            results["units_ok"] += 1
        except ref.StreamError as error:
            results[f"stopped: {error}"] += 1
            break
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("stream", type=pathlib.Path)
    parser.add_argument("--units", type=int, default=300, help="access units to read")
    args = parser.parse_args()
    data = args.stream.read_bytes()
    found = describe_restart(data, args.units)
    if found is None:
        print("no restart header in substream 3 within the units read")
        return 1
    number, sub, bits, _segment = found
    print(
        f"substream 3 restart header at unit {number}: min_chan {sub.min_chan} "
        f"max_chan {sub.max_chan} max_matrix_chan {sub.max_matrix_chan} "
        f"noise_shift {sub.noise_shift} data_check {sub.data_check} "
        f"ch_assign {sub.ch_assign}"
    )
    keys = list(READINGS)
    scored = []
    for values in itertools.product(*(READINGS[key] for key in keys)):
        reading = dict(zip(keys, values, strict=True))
        ok, tried, error = try_reading(data, reading, args.units)
        scored.append((ok, tried, reading, error))
    scored.sort(key=lambda item: -item[0])  # stable: the library's reading comes first among ties
    ties = sum(1 for item in scored if item[0] == scored[0][0])
    print(f"{len(scored)} readings tried, {ties} tie for the most segments parsed to their end:")
    for ok, tried, reading, error in scored[:8]:
        print(f"  {ok:4d}/{tried:<4d} {reading}" + (f"   first stop: {error}" if error else ""))
    best_ok = scored[0][0]
    if best_ok == 0:
        print("no reading parses a whole segment; header bits after the restart header follow")
        print("  " + "".join(str(bits.bits(bits.pos + i, 1)) for i in range(min(480, bits.left()))))
        return 2
    # Segments that parse to their end do not tell a reading from a shifted one when the stream is
    # regular: the lossless check of a whole restart interval does.
    print(
        "lossless check of the first restart intervals, all four substreams, for each tied reading:"
    )
    verdict = None
    for _ok, _tried, reading, _error in [item for item in scored if item[0] == best_ok][:8]:
        outcome = full_decode(data, reading, args.units)
        stopped = [key for key in outcome if key.startswith("stopped")]
        print(f"  {reading}")
        print("    " + ", ".join(f"{key}: {value}" for key, value in sorted(outcome.items())))
        if not stopped and verdict is None:
            verdict = reading
    failed = "no tied reading decodes past its first restart interval"
    print(f"verdict: {verdict if verdict else failed}")
    return 0 if verdict else 3


if __name__ == "__main__":
    sys.exit(main())
