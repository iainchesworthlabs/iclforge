#!/usr/bin/env python3
"""What a TrueHD or MLP stream exercises: a census of its syntax, read by the Python transcription.

The census is the evidence that a fixture covers a feature. tools/checks/check_truehd_coverage.py
requires every feature it lists to appear in at least one committed fixture, so that removing the
fixture that carries a feature (or a generator change that stops writing it) fails a check instead
of leaving a test that passes over nothing.

It decodes the stream with tools/references/truehd_syntax.py (no code shared with libs/truehd) and
records what the decode passed through: sample rate, word size codes, substreams and their
channels, restart sync words and the gaps between restart headers, filter orders, residual
codebooks, matrices and what they carry, shifts, noise, the block data check, the end-of-stream
marker, extra data and the directory's DRC word.

    truehd_census.py STREAM...        # print one JSON object a stream
"""

import itertools
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "references"))
import truehd_syntax as ref


class Census:
    def __init__(self):
        self.restart_syncs = set()
        self.restarts = 0
        self.fir_orders = set()
        self.iir_orders = set()
        self.combined_orders = set()  # (fir order, iir order) of a channel that has both
        self.huff_types = set()
        self.huff_offsets = False
        self.matrix_counts = set()
        self.frac_bits = set()
        self.lsb_bypass = False
        self.lsb_bypass_noise_type = False  # LSB bypass in a substream under restart sync 0x31EB
        self.matrix_noise_shift = False  # a non-zero matrix_noise_shift (noise generator 1)
        self.noise_coefficients = False
        self.output_shifts = False
        self.output_shift_negative = False
        self.output_shift_on_moved_channel = False  # a shift on a matrix channel ch_assign moves
        self.iir_state_shift = False  # an IIR new_states with a non-zero state_shift
        self.quant_steps = set()
        self.ch_assign_permuted = False
        self.noise_shift = False
        self.noise_seed = False
        self.data_check = False
        self.block_sizes = set()
        self.iir_state = False
        self.fir_state = False
        self.max_shifts = set()  # the restart header's max_shift: the source's word size shows here
        self.max_lsbs = set()
        self.marker_units = 0
        self.padding_samples = 0
        self.substream_indexes = set()


def census_of(data):
    """The feature record of a stream: a dict of plain values (sets are sorted lists)."""
    c = Census()
    original = {
        "restart": ref.Substream.restart,
        "block_header": ref.Substream.block_header,
        "read_matrices": ref.Substream.read_matrices,
        "filter_read": ref.Filter.read,
        "decode_segment": ref.decode_segment,
    }

    def restart(self, bits, segment):
        start = bits.pos
        original["restart"](self, bits, segment)
        c.restarts += 1
        # Behind the 14 bit sync, 16 bit timing, four 4 bit fields and the 23 bit seed: max_shift
        # (4 bits) and max_lsbs (5 bits) lead the 19 bits ERRATA 7 describes.
        fields = bits.bits(start + 14 + 16 + 16 + 23, 9)
        c.max_shifts.add(fields >> 5)
        c.max_lsbs.add(fields & 0x1F)
        c.noise_shift |= self.noise_shift != 0
        c.noise_seed |= self.noise_seed != 0
        c.data_check |= bool(self.data_check)
        c.restart_syncs.add(0x31EB if self.noise_type else 0x31EA)  # 0x31EC reads as 0x31EA/B
        c.ch_assign_permuted |= self.ch_assign != sorted(self.ch_assign)

    def block_header(self, bits):
        original["block_header"](self, bits)
        c.block_sizes.add(self.block_size)
        c.matrix_counts.add(len(self.matrices))
        c.output_shifts |= any(self.output_shift)
        c.output_shift_negative |= any(shift < 0 for shift in self.output_shift)
        c.output_shift_on_moved_channel |= any(
            shift != 0 and self.ch_assign[m] != m for m, shift in enumerate(self.output_shift)
        )
        for ch in range(self.min_chan, self.max_chan + 1):
            channel = self.channels[ch]
            c.huff_types.add(channel.huff_type)
            c.huff_offsets |= channel.huff_offset != 0
            fir, iir = len(channel.fir.coeff), len(channel.iir.coeff)
            if fir:
                c.fir_orders.add(fir)
            if iir:
                c.iir_orders.add(iir)
            if fir and iir:
                c.combined_orders.add((fir, iir))
        c.quant_steps.update(q for q in self.quant[self.min_chan : self.max_chan + 1] if q)

    def read_matrices(self, bits):
        matrices = original["read_matrices"](self, bits)
        for _out, coeffs, lsb_bypass, noise_shift in matrices:
            c.lsb_bypass |= bool(lsb_bypass)
            c.lsb_bypass_noise_type |= bool(lsb_bypass) and self.noise_type
            c.matrix_noise_shift |= noise_shift != 0
            c.noise_coefficients |= any(coeffs[self.max_matrix_chan + 1 : self.max_matrix_chan + 3])
        return matrices

    def filter_read(bits, max_order):
        flt = original["filter_read"](bits, max_order)
        if flt.state is not None:
            if max_order == 8:
                c.fir_state = True
            else:
                c.iir_state = True
                c.iir_state_shift |= flt.state_shift != 0
        return flt

    def decode_segment(sub, entry, segment, rows):
        out, padding = original["decode_segment"](sub, entry, segment, rows)
        payload = segment[:-2] if entry["crc_present"] else segment
        if (
            len(payload) * 8 >= 32
            and ref.Bits(payload).bits(len(payload) * 8 - 32, 20) == ref.END_MARKER
        ):
            c.marker_units += 1
            c.padding_samples = max(c.padding_samples, padding)
        c.substream_indexes.add(sub.index)
        return out, padding

    ref.Substream.restart = restart
    ref.decode_segment = decode_segment
    ref.Substream.block_header = block_header
    ref.Substream.read_matrices = read_matrices
    ref.Filter.read = staticmethod(filter_read)
    try:
        rows, rate, _channels = ref.decode_stream(data, "all")
    finally:
        ref.Substream.restart = original["restart"]
        ref.decode_segment = original["decode_segment"]
        ref.Substream.block_header = original["block_header"]
        ref.Substream.read_matrices = original["read_matrices"]
        ref.Filter.read = staticmethod(original["filter_read"])
    facts = _stream_facts(data, rate, len(rows))
    # An MLP stream that declares 16 bit words (group 1 word length code 0) and codes samples
    # with bits set below bit 8 (TH spec 7.4).
    facts["mlp_word16_low_bits"] = bool(
        facts["kind"] == "mlp"
        and facts["word_code"] == 0
        and any(value & 0xFF for row in rows for value in row)
    )
    facts["channels"] = len(rows[0]) if rows else 0  # all substreams read: 16 for the object stream
    return {**facts, **_syntax_facts(c)}


def _stream_facts(data, rate, samples):
    """What the framing says, read directly: kind, substreams, the spacing of major syncs, extra
    data and the DRC word."""
    units = list(ref.access_units(data))
    facts = {"sample_rate": rate, "samples": samples, "access_units": len(units)}
    first = next(u for u in units if u[4:7] == ref.SYNC_WORD)
    facts["kind"] = "mlp" if first[7] == 0xBB else "truehd"
    count = first[4 + 16] >> 4
    facts["substreams"] = count
    facts["word_code"] = int.from_bytes(first[8:12], "big") >> 28 if first[7] == 0xBB else None
    major = [i for i, u in enumerate(units) if u[4:7] == ref.SYNC_WORD]
    facts["major_sync_units"] = len(major)
    facts["major_sync_gaps"] = sorted({b - a for a, b in itertools.pairwise(major)})
    facts["extra_data_units"] = 0
    facts["drc_word_units"] = 0
    for unit in units:
        offset = 4 + (28 if unit[4:7] == ref.SYNC_WORD else 0)
        end = 0
        drc = False
        entries = []
        for _ in range(count):
            word = int.from_bytes(unit[offset : offset + 2], "big")
            offset += 2
            if word >> 15:
                drc = True
                offset += 2
            entries.append(word & 0xFFF)
        end = offset + entries[-1] * 2
        facts["drc_word_units"] += drc
        facts["extra_data_units"] += end < len(unit)
    return facts


def _syntax_facts(c):
    return {
        "restart_headers": c.restarts,
        "restart_syncs": sorted(c.restart_syncs),
        "fir_orders": sorted(c.fir_orders),
        "iir_orders": sorted(c.iir_orders),
        "combined_orders": sorted(c.combined_orders),
        "huff_types": sorted(c.huff_types),
        "huff_offsets": c.huff_offsets,
        "matrix_counts": sorted(c.matrix_counts),
        "lsb_bypass": c.lsb_bypass,
        "lsb_bypass_noise_type": c.lsb_bypass_noise_type,
        "matrix_noise_shift": c.matrix_noise_shift,
        "noise_coefficients": c.noise_coefficients,
        "output_shifts": c.output_shifts,
        "output_shift_negative": c.output_shift_negative,
        "output_shift_on_moved_channel": c.output_shift_on_moved_channel,
        "iir_state_shift": c.iir_state_shift,
        "quant_steps": sorted(c.quant_steps),
        "ch_assign_permuted": c.ch_assign_permuted,
        "noise_shift": c.noise_shift,
        "noise_seed": c.noise_seed,
        "data_check": c.data_check,
        "block_sizes": sorted(c.block_sizes),
        "iir_state": c.iir_state,
        "fir_state": c.fir_state,
        "max_shifts": sorted(c.max_shifts),
        "max_lsbs": sorted(c.max_lsbs),
        "marker_units": c.marker_units,
        "padding_samples": c.padding_samples,
        "substream_indexes": sorted(c.substream_indexes),
    }


def main(argv):
    for name in argv[1:]:
        print(json.dumps({"file": name, **census_of(pathlib.Path(name).read_bytes())}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
