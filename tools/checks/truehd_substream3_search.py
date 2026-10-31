#!/usr/bin/env python3
"""Which reading of the block header parses the object substream (substream 3) of a TrueHD stream.

For the owner to run on a disc excerpt on the owner's machine (planning/work-packages/WP1, step 1).
It reads nothing but the stream given and prints parameters and counts: no sample, no residual bit.
The stream is an elementary TrueHD stream (a `.thd`, or `ffmpeg -c copy` out of the disc's `.m2ts`).

    truehd_substream3_search.py STREAM [--units N] [--trace] [--restart-index K] [--trace-only]
                                     [--directory] [--agreement] [--common-prefix]
                                     [--dump-header]
                                     [--first-unit N] [--last-unit N]
                                     [--dump-bits N] [--lossless]

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
    # (the disc's substream 3 has min_chan 8, max_chan = max_matrix_chan = 11; "all" is the 16 rows)
    "coeffs": ("matrix+noise", "matrix", "chan+noise", "chan", "all+noise", "all"),
    # when a matrix ends with a 4 bit noise shift
    "noise_shift": ("by_sync", "always", "never"),
    # the FIR filter's state flag (ERRATA 9) and the IIR filter's
    "fir_state_flag": (True, False),
    # how many output shifts and how many quantiser steps the header sends
    # ("own" is the channels min_chan to max_chan only)
    "shift_count": ("matrix", "chan", "own"),
    "quant_count": ("chan", "matrix", "own"),
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
        if kind.startswith("all"):
            base = 16
        else:
            base = self.max_matrix_chan + 1 if kind.startswith("matrix") else self.max_chan + 1
        return base + (ref.NOISE_CHANNELS if kind.endswith("+noise") else 0)

    def has_noise_shift(self):
        mode = self.reading["noise_shift"]
        return self.noise_type if mode == "by_sync" else mode == "always"

    def read_matrices(self, bits):
        if self.object_matrices:
            return self.read_object_matrices(bits)
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
            kind = self.reading["shift_count"]
            count = {
                "matrix": self.max_matrix_chan + 1,
                "chan": self.max_chan + 1,
                "own": self.max_chan - self.min_chan + 1,
            }[kind]
            self.output_shift = [bits.signed(4) for _ in range(count)]
            self.output_shift += [0] * (16 - len(self.output_shift))
        if flags & 0x10 and bits.read(1):
            kind = self.reading["quant_count"]
            count = {
                "chan": self.max_chan + 1,
                "matrix": self.max_matrix_chan + 1,
                "own": self.max_chan - self.min_chan + 1,
            }[kind]
            self.quant = [bits.read(4) for _ in range(count)]
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
                {
                    "extra": word >> 15,
                    "restart_nonexistent": (word >> 14) & 1,
                    "crc_present": (word >> 13) & 1,
                    "end": word & 0xFFF,
                }
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
        if not segment:
            continue
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
        if not segment or segment[0] & 0xC0 != 0xC0:
            continue
        sub = ref.Substream(3)
        bits = ref.Bits(segment[:-2] if entry["crc_present"] else segment, 2)
        sub.restart(bits, segment[:-2] if entry["crc_present"] else segment)
        return number, sub, bits, segment
    return None


class TracedBits(ref.Bits):
    """Bits that remember every read: where, how wide, what value, and which function asked."""

    def __init__(self, data, pos=0):
        super().__init__(data, pos)
        self.trace = []

    def read(self, n):
        start = self.pos
        value = super().read(n)
        if n:
            self.trace.append((start, n, value, sys._getframe(1).f_code.co_name))
        return value


def trace_first_block(data, units, reading, limit=800, skip=0):
    """The reads of substream 3's restart header and first block header under one reading, up to
    where it stops: position, width, value and the function that read it. Parameters of the stream
    and nothing else: the block data is not read."""
    for _number, unit, entries, offset, count in units_of(data, units):
        if count < 4:
            continue
        entry, segment = list(segments_of(unit, entries, offset))[3]
        if not segment or segment[0] & 0xC0 != 0xC0:
            continue
        if skip:
            skip -= 1
            continue
        payload = segment[:-2] if entry["crc_present"] else segment
        bits = TracedBits(payload)
        sub = Variant(3, reading)
        error = None
        try:
            bits.read(2)
            sub.restart(bits, payload)
            sub.block_header(bits)
        except (ref.StreamError, IndexError, ValueError) as caught:
            error = f"{type(caught).__name__}: {caught}"
        lines = [
            f"  bit {start:5d}  width {width:2d}  value {value:<10d} {name}"
            for start, width, value, name in bits.trace[-limit:]
        ]
        lines.insert(0, f"  (access unit {_number}, segment of {len(segment)} bytes)")
        return lines, error, bits.pos
    return [], "no restart header in substream 3", 0


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


def print_directory(data, units):
    """Per access unit: the directory entries of the substreams and the first bits of substream 3's
    segment (its block-header flags or its restart header: parameters, not audio)."""
    print(
        "unit  directory (end/restart_nonexistent/crc/extra)"
        "                      seg3 bytes  seg3 first 24 bits"
    )
    for number, unit, entries, offset, count in units_of(data, units):
        if count < 4:
            continue
        segment = list(segments_of(unit, entries, offset))[3][1]
        first = "".join(f"{byte:08b}" for byte in segment[:3])
        flags = " ".join(
            f"{e['end']}/{(e['restart_nonexistent'])}/{e['crc_present']}/{e['extra']}"
            for e in entries
        )
        sync = " restart-like" if segment and segment[0] & 0xC0 == 0xC0 else ""
        print(f"{number:5d}  {flags:44s} {len(segment):8d}  {first}{sync}")


def matrix_list(payload, start, gap, flags_count, shift_bits):
    """The (output channel, frac_bits, lsb_bypass) of each matrix in the first block after a restart
    header that ends at bit `start`, read with `gap` extra bits, `flags_count` presence bits per
    matrix and `shift_bits` bits after each matrix. Whatever was read before an error."""
    bits = ref.Bits(payload, start + gap)
    found = []
    try:
        flags = bits.read(8) if bits.read(1) else 0xFF
        if flags & 0x80 and bits.read(1):
            bits.read(9)
        if not (flags & 0x40 and bits.read(1)):
            return found
        for _ in range(bits.read(4)):
            out, frac = bits.read(4), bits.read(4)
            lsb = bits.read(1)
            for _flag in range(flags_count):
                if bits.read(1):
                    bits.signed(frac + 2)
            if shift_bits:
                bits.read(shift_bits)
            found.append((out, frac, lsb))
    except (ref.StreamError, IndexError, ValueError):
        pass
    return found


def print_agreement(data, units):
    """Parse the first block of every restart of substream 3 under each layout of the matrix
    section and score the layout by how many leading matrices (output channel, frac_bits, bypass)
    repeat from the first restart to the others. An encoder that repeats its matrices makes the
    right layout agree with itself and a wrong one not. Parameters only."""
    restarts = []
    for number, unit, entries, offset, count in units_of(data, units):
        if count < 4:
            continue
        entry, segment = list(segments_of(unit, entries, offset))[3]
        if not segment or segment[0] & 0xC0 != 0xC0:
            continue
        payload = segment[:-2] if entry["crc_present"] else segment
        sub = ref.Substream(3)
        bits = ref.Bits(payload, 2)
        try:
            sub.restart(bits, payload)
        except (ref.StreamError, IndexError, ValueError):
            continue
        restarts.append((number, payload, bits.pos))
    print(f"{len(restarts)} restarts of substream 3 in {units} units")
    if len(restarts) < 2:
        return
    ranked = []
    # Restarts whose matrix bits are the first restart's agree under every layout: not counted.
    region = 64
    first = restarts[0][1][restarts[0][2] // 8 :][:region]
    distinct = [
        index
        for index, (_n, payload, start) in enumerate(restarts)
        if index > 0 and payload[start // 8 :][:region] != first
    ]
    print(f"{len(distinct)} restarts differ from the first in their first {region} bytes")
    for gap in range(0, 33):
        for flags_count in range(10, 20):
            for shift_bits in (0, 4):
                lists = [
                    matrix_list(payload, start, gap, flags_count, shift_bits)
                    for _number, payload, start in restarts
                ]
                base = lists[0]
                score = 0
                for index in distinct:
                    for a, b in zip(base, lists[index], strict=False):
                        if a != b:
                            break
                        score += 1
                ranked.append((score, gap, flags_count, shift_bits, lists))
    ranked.sort(key=lambda item: -item[0])
    print("score = matrices that repeat in order from the first restart, summed over the others")
    for score, gap, flags_count, shift_bits, _lists in ranked[:8]:
        print(
            f"  score {score:5d}  gap {gap:2d}  presence bits {flags_count:2d}  shift {shift_bits}"
        )
    score, gap, flags_count, shift_bits, lists = ranked[0]
    print(f"first restarts under the best layout (gap {gap}, {flags_count}, {shift_bits}):")
    for (number, _payload, _start), found in list(zip(restarts, lists, strict=True))[:6]:
        print(f"  unit {number:5d}: " + " ".join(f"{o}:{f}:{b}" for o, f, b in found))


def common_prefix_bits(first, second):
    """How many leading bits two byte strings share."""
    for index, (a, b) in enumerate(zip(first, second, strict=False)):
        if a != b:
            return index * 8 + (8 - (a ^ b).bit_length())
    return min(len(first), len(second)) * 8


def restart_tails(data, units, first_unit=0, last_unit=None):
    """(unit, segment bytes, the bits after the restart header, from a byte boundary) for every
    restart of substream 3 from `first_unit` to `last_unit`."""
    restarts = []
    for number, unit, entries, offset, count in units_of(data, units):
        if count < 4 or number < first_unit or (last_unit is not None and number > last_unit):
            continue
        entry, segment = list(segments_of(unit, entries, offset))[3]
        if not segment or segment[0] & 0xC0 != 0xC0:
            continue
        payload = segment[:-2] if entry["crc_present"] else segment
        sub = ref.Substream(3)
        bits = ref.Bits(payload, 2)
        try:
            sub.restart(bits, payload)
        except (ref.StreamError, IndexError, ValueError):
            continue
        start = bits.pos
        # Shift the payload so that the bits after the restart header begin on a byte boundary.
        value = int.from_bytes(payload, "big")
        total = len(payload) * 8
        tail = value & ((1 << (total - start)) - 1)
        length = (total - start + 7) // 8
        tail <<= length * 8 - (total - start)
        restarts.append((number, len(payload), tail.to_bytes(length, "big")))
    return restarts


def print_common_prefix(data, units, first_unit=0, last_unit=None):
    """For every restart of substream 3, how many bits after the restart header it shares with the
    first restart and with the one before. A restart whose matrices and filters repeat shares them
    up to where its sample data begins, which is the length of its block header. Counts only."""
    restarts = restart_tails(data, units, first_unit, last_unit)
    print(
        f"{len(restarts)} restarts of substream 3 in {units} units (bits after the restart header)"
    )
    if not restarts:
        return
    print("  unit  segment bytes  shared with the first  shared with the previous")
    previous = restarts[0][2]
    shown = []
    for number, size, tail in restarts:
        with_first = common_prefix_bits(restarts[0][2], tail)
        with_previous = common_prefix_bits(previous, tail)
        shown.append(with_first)
        previous = tail
        print(f"  {number:5d}  {size:12d}  {with_first:20d}  {with_previous:24d}")
    others = shown[1:]
    if others:
        print(f"shared with the first, over the others: min {min(others)} max {max(others)}")


def print_header_dump(data, units, skip=2, margin=8, first_unit=0, last_unit=None):
    """The bits after the restart header that every restart of substream 3 shares from the
    `skip`-th on, less a margin, as hex. Restarts seconds apart that agree on them cannot be
    carrying audio there: it is the block header of an encoder that repeats its parameters."""
    restarts = restart_tails(data, units, first_unit, last_unit)[skip:]
    if len(restarts) < 3:
        print(f"only {len(restarts)} restarts after the first {skip}: nothing to compare")
        return
    base = restarts[0][2]
    shared = min(common_prefix_bits(base, tail) for _n, _s, tail in restarts[1:]) - margin
    print(
        f"{len(restarts)} restarts from unit {restarts[0][0]} share {shared} bits "
        f"(less a margin of {margin})"
    )
    if shared <= 0:
        print("the restarts share nothing to speak of: their parameters differ")
        return
    nbytes = (shared + 7) // 8
    value = int.from_bytes(base[:nbytes], "big") >> (nbytes * 8 - shared)
    text = f"{value:0{(shared + 3) // 4}x}"
    print(f"bits {shared}, hex:")
    for at in range(0, len(text), 64):
        print(text[at : at + 64])


def print_restart_bits(data, units, count, first_unit=0, last_unit=None, limit=5):
    """The first `count` bits after the restart header of the first few restarts of substream 3 in
    the unit range, as binary, 64 to a line. For a stream of one's own test signal (silence): the
    bits are the encoder's parameters and nothing of a programme."""
    restarts = restart_tails(data, units, first_unit, last_unit)[:limit]
    if not restarts:
        print("no restart of substream 3 in that range")
        return
    for number, size, tail in restarts:
        text = "".join(f"{byte:08b}" for byte in tail)[:count]
        print(f"unit {number}: segment {size} bytes, {len(tail) * 8} bits after the restart header")
        for at in range(0, len(text), 64):
            print(f"{at:5d} {text[at : at + 64]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("stream", type=pathlib.Path)
    parser.add_argument("--units", type=int, default=300, help="access units to read")
    parser.add_argument(
        "--trace",
        action="store_true",
        help="also print every field of the first block header under the library's reading",
    )
    parser.add_argument(
        "--restart-index",
        type=int,
        default=0,
        help="trace the K-th restart header of substream 3 instead of the first",
    )
    parser.add_argument(
        "--directory",
        action="store_true",
        help="print the substream directory and the first bits of substream 3's segment per unit",
    )
    parser.add_argument(
        "--agreement",
        action="store_true",
        help="compare the matrices of the first block of every restart under each layout",
    )
    parser.add_argument("--first-unit", type=int, default=0, help="restarts from this unit on")
    parser.add_argument("--last-unit", type=int, default=None, help="restarts up to this unit")
    parser.add_argument(
        "--dump-bits",
        type=int,
        default=0,
        metavar="N",
        help="print N bits after the restart header of the first restarts in the unit range",
    )
    parser.add_argument(
        "--lossless",
        action="store_true",
        help="decode all four substreams under the standard reading and say where the lossless "
        "check or the parse first fails",
    )
    parser.add_argument(
        "--dump-header",
        action="store_true",
        help="print the bits after the restart header that the steady-state restarts all share",
    )
    parser.add_argument(
        "--common-prefix",
        action="store_true",
        help="count the bits each restart of substream 3 shares with the first after its header",
    )
    parser.add_argument(
        "--trace-only", action="store_true", help="print the trace and stop: no search"
    )
    args = parser.parse_args()
    data = args.stream.read_bytes()
    if args.directory:
        print_directory(data, min(args.units, 80))
        return 0
    if args.lossless:
        default = {key: values[0] for key, values in READINGS.items()}
        default["gap_bits"] = 0
        print(dict(full_decode(data, default, args.units)))
        return 0
    if args.dump_bits:
        print_restart_bits(data, args.units, args.dump_bits, args.first_unit, args.last_unit)
        return 0
    if args.dump_header:
        print_header_dump(
            data,
            args.units,
            skip=0 if args.first_unit else 2,
            first_unit=args.first_unit,
            last_unit=args.last_unit,
        )
        return 0
    if args.common_prefix:
        print_common_prefix(data, args.units, args.first_unit, args.last_unit)
        return 0
    if args.agreement:
        print_agreement(data, args.units)
        return 0
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
    default = {key: values[0] for key, values in READINGS.items()}
    if args.trace or args.trace_only:
        lines, error, position = trace_first_block(
            data, args.units, default, skip=args.restart_index
        )
        print(f"first block header under the library's reading, to bit {position}:")
        print("\n".join(lines))
        print(f"  stopped: {error}" if error else "  parsed to its end")
        if args.trace_only:
            return 0
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
        if not args.trace:
            lines, error, position = trace_first_block(data, args.units, default)
            print(f"first block header under the library's reading, to bit {position}:")
            print("\n".join(lines))
            print(f"  stopped: {error}" if error else "  parsed to its end")
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
