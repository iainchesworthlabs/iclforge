#!/usr/bin/env python3
"""A TrueHD stream writer for hand-built streams, and the hand-built streams it makes.

FFmpeg's encoders do not write an IIR filter, LSB bypass, several matrices with a permuted channel
assignment, noise generator 0, the block data check, the end-of-stream marker in every unit, extra
data, the DRC word of the directory, or the 16-channel substream. The decoders have to read them
all. The streams carry a major sync FFmpeg accepts, and FFmpeg's strict decode is held to each of
them (tools/checks/verify_truehd_interop.py --handbuilt): for eleven of the fifteen it returns the
signal the stream was made from, which is the outside check; the manifest's `ffmpeg` field says
what it does with the others. What FFmpeg does not read has no outside reader: the extra data, the
values of the DRC word, the block data CRC, the 16-channel substream.

The codec is lossless, so a decoder that is right gives the signal back, and the expected PCM in
testdata/truehd/handbuilt/manifest.json is the writer's input, not any decoder's output. The layout
the writer follows is libs/truehd/ERRATA.md's (the specification's where FFmpeg's decode cannot
say), so where FFmpeg does not read a field a mistake in reading the specification is in the
writer and both decoders alike; what those streams check is that the two decoders and the writer
agree, that the features work together, and that nothing regresses.

    truehd_writer.py generate [--out DIR]    # write the streams and the manifest
    truehd_writer.py check [DIR]             # decode them with truehd_syntax.py and compare

Not written: restart_header's heavy DRC (TH spec 12.2, 7.1), whose place among the header's bits
the streams do not show (ERRATA 7)."""

import argparse
import hashlib
import json
import math
import pathlib
import random
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import truehd_evolution as evolution
import truehd_syntax as ref

REPO = pathlib.Path(__file__).resolve().parents[2]
HANDBUILT = REPO / "testdata" / "truehd" / "handbuilt"
MARKER = ref.END_MARKER


class BitWriter:
    def __init__(self):
        self.value = 0
        self.count = 0

    def put(self, value, bits):
        if value < 0 or value >> bits:
            raise ValueError(f"{value} does not fit {bits} bits")
        self.value = (self.value << bits) | value
        self.count += bits

    def signed(self, value, bits):
        if not -(1 << (bits - 1)) <= value < (1 << (bits - 1)):
            raise ValueError(f"{value} does not fit {bits} signed bits")
        self.put(value & ((1 << bits) - 1), bits)

    def align(self, bits=8):
        self.put(0, -self.count % bits)

    def append(self, other):
        self.put(other.value, other.count)

    def bytes(self):
        self.align(8)
        return self.value.to_bytes(self.count // 8, "big")


class Channel:
    """How one channel's samples are coded: its filters, quantiser step and residual codebook."""

    def __init__(self, fir=None, iir=None, huff_type=0, quant=0, iir_state=None):
        self.fir = fir  # (coefficients, shift)
        self.iir = iir
        self.huff_type = huff_type
        self.quant = quant
        # (state_bits, state_shift, values): the IIR filter's new_states, sent with its parameters
        # at every restart. The history at lag i + 1 becomes values[i] << state_shift (TH spec 9.2).
        self.iir_state = iir_state


class Substream:
    """One substream. `coded[ch]` is the channel as the prediction filters leave it, before the
    matrices (a multiple of 2^quant); `output` is what the substream has to give back, one row of
    mm + 1 samples per time."""

    def __init__(
        self,
        min_chan,
        max_chan,
        mm,
        channels,
        output,
        coded,
        ch_assign=None,
        matrices=(),
        bypass=None,
        out_shift=None,
        sync=0x31EA,
        data_check=False,
        noise_shift=0,
        noise_seed=0,
        matrix_noise_shift=0,
        ch_assign_cycle=None,
        sections=None,
    ):
        self.min_chan, self.max_chan, self.mm = min_chan, max_chan, mm
        self.channels = channels  # {ch: Channel}
        self.output = output
        self.coded = coded  # {ch: [samples]}
        self.ch_assign = ch_assign or (
            ch_assign_cycle[0] if ch_assign_cycle else list(range(mm + 1))
        )
        # The ch_assign of each restart interval in turn, for a stream whose assignment changes
        # between restart headers: interval k takes ch_assign_cycle[k % len(ch_assign_cycle)].
        self.ch_assign_cycle = ch_assign_cycle or [self.ch_assign]
        # (out, frac_bits, lsb_bypass, {channel: coefficient}[, noise shift]); the noise shift is
        # the matrix's own under noise_type 1 and defaults to `matrix_noise_shift`
        self.matrices = list(matrices)
        self.bypass = bypass or {}  # {matrix index: [bit per sample]}
        self.out_shift = out_shift or [0] * (mm + 1)
        self.sync = sync
        self.data_check = data_check
        self.noise_shift = noise_shift
        self.noise_seed = noise_seed
        # The 4 bit noise shift every matrix carries under noise_type 1 (sync 0x31EB, TH spec 10.2).
        self.matrix_noise_shift = matrix_noise_shift
        # {first sample of a block: ObjectSection}: the object matrix section that block sends
        # (sync 0x31EC), in place of `matrices`; every restart block needs one.
        self.sections = sections or {}


class ObjectSection:
    """One object matrix section as a block sends it (TH spec 10.2.1a, ERRATA 69): A (the matrices
    change), B (new headers), the headers (out, frac_bits, f_shift, noise shift), the coefficients
    of each matrix as {channel: raw} (the keys are the presence mask, so a raw 0 is a present
    coefficient), then A2, B2, C2, the widths C2 sends, their 2 reserved bits, and the deltas
    of each matrix with a width, {channel: delta}. `resolve_sections` fills `resolved`, the
    matrices in force after the section."""

    def __init__(
        self,
        coeffs=(),
        headers=(),
        a=True,
        b=True,
        a2=False,
        b2=False,
        c2=False,
        widths=(),
        reserved=(),
        deltas=(),
    ):
        self.coeffs = [dict(c) for c in coeffs]
        self.headers = list(headers)
        self.a, self.b, self.a2, self.b2, self.c2 = a, b, a2, b2, c2
        self.widths = list(widths)
        self.reserved = list(reserved) or [0] * len(self.widths)
        self.deltas = [dict(d) for d in deltas]
        self.resolved = None


def resolve_sections(sections, restarts):
    """The matrices in force after each section ({first sample: ObjectSection}, in order), by the
    rules of ERRATA 69: new headers (B) keep the width of the matrix at the same place; new
    coefficients clear the deltas; with A clear the last set stands, deltas and all; C2 replaces
    the widths, else they stand; a width not 0 takes a delta for each coefficient; a restart
    (`restarts`: the first samples of the restart blocks) clears the set. Each matrix is a dict
    with out, frac_bits, f_shift, noise_shift, raw, width and delta."""
    state = []
    for start in sorted(sections):
        section = sections[start]
        if start in restarts:
            state = []
        if section.a:
            fresh = []
            for place, coeffs in enumerate(section.coeffs):
                if section.b:
                    out, frac_bits, f_shift, noise_shift = section.headers[place]
                    width = state[place]["width"] if place < len(state) else 0
                else:
                    last = state[place]
                    out, frac_bits, f_shift, noise_shift, width = (
                        last["out"],
                        last["frac_bits"],
                        last["f_shift"],
                        last["noise_shift"],
                        last["width"],
                    )
                    if set(coeffs) != set(last["raw"]):
                        raise ValueError("coefficients do not match the headers that stand")
                fresh.append(
                    {
                        "out": out,
                        "frac_bits": frac_bits,
                        "f_shift": f_shift,
                        "noise_shift": noise_shift,
                        "raw": dict(coeffs),
                        "width": width,
                        "delta": {},
                    }
                )
            state = fresh
        else:
            state = [{**m, "delta": dict(m["delta"])} for m in state]
        if section.a2 and section.b2:
            if section.c2:
                for matrix, width in zip(state, section.widths, strict=True):
                    matrix["width"] = width
            for matrix, delta in zip(state, section.deltas, strict=True):
                if matrix["width"] and set(delta) != set(matrix["raw"]):
                    raise ValueError("a delta for every coefficient of a matrix with a width")
                matrix["delta"] = dict(delta) if matrix["width"] else {}
        section.resolved = [{**m, "delta": dict(m["delta"])} for m in state]
    return state


def write_object_section(w, sub, section):
    """The bits of the section, after the block header's matrices-present bit."""
    channels = sub.mm + 1
    w.put(1 if section.a else 0, 1)
    if section.a:
        w.put(1 if section.b else 0, 1)
        if section.b:
            w.put(len(section.resolved) - 1, 4)
            for matrix in section.resolved:
                mask = sum(1 << c for c in matrix["raw"])
                w.put(matrix["out"], 4)
                w.put(matrix["frac_bits"], 4)
                w.put(matrix["f_shift"] + 1, 3)
                w.put(0, 2)  # no LSB bypass
                w.put(matrix["noise_shift"], 4)
                w.put(mask, channels)
        for matrix in section.resolved:
            for c in sorted(matrix["raw"]):
                w.signed(matrix["raw"][c], matrix["frac_bits"] + 2)
    w.put(1 if section.a2 else 0, 1)
    if not section.a2:
        return
    w.put(1 if section.b2 else 0, 1)
    if not section.b2:
        return
    w.put(1 if section.c2 else 0, 1)
    if section.c2:
        for width, reserved in zip(section.widths, section.reserved, strict=True):
            w.put(width, 4)
            w.put(reserved, 2)
    for matrix in section.resolved:
        if matrix["width"]:
            for c in sorted(matrix["delta"]):
                w.signed(matrix["delta"][c], matrix["width"] + 1)


def assign_of(sub, interval):
    """The ch_assign of the restart interval with this index."""
    return sub.ch_assign_cycle[interval % len(sub.ch_assign_cycle)]


# The channel assignment (TH spec 13.1) for a channel count, as the library's encoder takes it.
DEFAULT_ASSIGNMENT = {1: 0x2, 2: 0x1, 3: 0x3, 4: 0x9, 5: 0xB, 6: 0xF, 7: 0x4B, 8: 0x4F}


class Stream:
    def __init__(
        self,
        substreams,
        samples,
        rate_code=0,
        kind="truehd",
        restart_interval=16,
        marker="last",
        drc_words=False,
        extra_data=None,
        terminator_b=False,
        channel_assignment=None,
        word_code=2,
        marker_pattern=MARKER,
    ):
        self.substreams = substreams
        self.samples = samples
        self.rate_code = rate_code
        self.kind = kind
        self.restart_interval = restart_interval
        self.marker = marker  # "last": the last unit only, "every": each unit
        self.marker_pattern = marker_pattern  # terminatorA: 0xD234E or 0x348D3 (TH spec 16.5.1)
        self.drc_words = drc_words
        self.extra_data = extra_data  # a function of the unit's index giving its payload
        # True: right after the last block; "end": ends the segment
        self.terminator_b = terminator_b
        self.word_code = word_code  # an MLP stream's group 1 word length code: 0 16, 1 20, 2 24 bit
        self.channel_assignment = channel_assignment or DEFAULT_ASSIGNMENT[self.channels]

    @property
    def channels(self):
        """The channels of the best presentation: the highest of substreams 0 to 2."""
        return self.substreams[min(len(self.substreams), 3) - 1].mm + 1

    @property
    def per_unit(self):
        return 40 << {0: 0, 1: 1, 2: 2, 8: 0, 9: 1, 10: 2}[self.rate_code]

    @property
    def sample_rate(self):
        return {0: 48000, 1: 96000, 2: 192000, 8: 44100, 9: 88200, 10: 176400}[self.rate_code]

    @property
    def units(self):
        return -(-self.samples // self.per_unit)


# --- the residuals ----------------------------------------------------------------------------


def residuals(channel, coded, interval=None):
    """The residual of each sample (ERRATA 13): the coded value less the prediction, in units of
    2^quant. The filters' histories start at zero at the first restart and run on across the later
    ones (TH spec 9.6), unless the channel's IIR filter sends new_states, which replace the IIR
    history at each restart, every `interval` samples."""
    fir, iir = channel.fir, channel.iir
    shift = fir[1] if fir else (iir[1] if iir else 0)
    out = []
    fir_history = [0] * 8
    iir_history = [0] * 8
    for n, value in enumerate(coded):
        if channel.iir_state and iir and n % interval == 0:
            _bits, state_shift, values = channel.iir_state
            iir_history = [v << state_shift for v in values] + [0] * (8 - len(values))
        prediction = 0
        if fir:
            prediction += sum(h * c for h, c in zip(fir_history, fir[0], strict=False))
        if iir:
            prediction += sum(h * c for h, c in zip(iir_history, iir[0], strict=False))
        prediction >>= shift
        q = channel.quant
        out.append((value >> q) - (prediction >> q))
        fir_history = [value, *fir_history[:7]]
        iir_history = [value - prediction, *iir_history[:7]]
    return out


def choose_coding(channel, values, lsb_limit=24):
    """huff_lsbs and huff_offset that code every residual of the channel (ERRATA 11)."""
    low, high = min(values), max(values)
    offset = max(-16384, min(16383, (low + high) // 2))
    top = {1: 10, 2: 8, 3: 7}.get(channel.huff_type)
    for raw_bits in range(0, lsb_limit + 1):
        if channel.huff_type == 0:
            centre = (1 << (raw_bits - 1)) if raw_bits else 0
            fits = (low - offset + centre >= 0) and (
                high - offset + centre < (1 << raw_bits) if raw_bits else low == high == offset
            )
        else:
            sign_shift = raw_bits + 2 - channel.huff_type
            bias = offset - ((1 << sign_shift) if sign_shift >= 0 else 0)
            fits = all(-7 <= (v - bias) >> raw_bits <= top for v in (low, high))
        if fits:
            return raw_bits + channel.quant, offset
    raise ValueError("a residual does not fit the codebook")


def encode_residual(w, channel, value, huff_lsbs, offset):
    raw_bits = max(huff_lsbs - channel.quant, 0)
    if channel.huff_type == 0:
        centre = (1 << (raw_bits - 1)) if raw_bits else 0
        w.put(value - offset + centre, raw_bits)
        return
    sign_shift = raw_bits + 2 - channel.huff_type
    bias = offset - ((1 << sign_shift) if sign_shift >= 0 else 0)
    rest = value - bias
    symbol = rest >> raw_bits
    for sym, code, bits in ref.CODEBOOKS[channel.huff_type]:
        if sym == symbol:
            w.put(code, bits)
            break
    w.put(rest & ((1 << raw_bits) - 1), raw_bits)


# --- the syntax, written ----------------------------------------------------------------------


def write_filter(w, flt, is_iir, state=None):
    coeffs, shift = flt
    w.put(len(coeffs), 4)
    w.put(shift, 4)
    bits = max(max(abs(c) for c in coeffs).bit_length() + 1, 2)
    w.put(bits, 5)
    w.put(0, 3)  # coeff_shift
    for c in coeffs:
        w.signed(c, bits)
    if state:
        state_bits, state_shift, values = state
        w.put(1, 1)  # new_states
        w.put(state_bits, 4)
        w.put(state_shift, 4)
        if state_bits:
            for value in values:
                w.signed(value, state_bits)
    else:
        w.put(0, 1)  # new_states / the FIR filter's flag (ERRATA 9)


def write_restart_header(sub, check, timing=0, assign=None):
    w = BitWriter()
    w.put(sub.sync, 14)
    w.put(timing, 16)
    w.put(sub.min_chan, 4)
    w.put(sub.max_chan, 4)
    w.put(sub.mm, 4)
    w.put(sub.noise_shift, 4)
    w.put(sub.noise_seed, 23)
    w.put(0, 19)  # max_shift, max_lsbs, max_bits, max_bits_repeat (ERRATA 7)
    w.put(1 if sub.data_check else 0, 1)
    w.put(check, 8)
    w.put(0, 16)
    for ch in assign or sub.ch_assign:
        w.put(ch, 6)
    bits = BitWriter()
    bits.append(w)
    w.put(ref.crc8_restart(bits.bytes(), 0, w.count), 8)
    return w


def write_full_block_header(w, sub, params, size, section=None):
    w.put(0, 1)  # new_guards: the flags stay 0xFF
    w.put(1, 1)
    w.put(size, 9)
    if section is not None:
        w.put(1, 1)
        write_object_section(w, sub, section)
    elif sub.matrices and sub.sync == 0x31EC:
        # TH spec 10.2.1a: the object-audio matrix section.
        w.put(1, 1)
        w.put(1, 1)
        w.put(1, 1)
        w.put(len(sub.matrices) - 1, 4)
        channels = sub.mm + 1
        # (out, frac_bits, lsb_bypass, {channel: coefficient}, f_shift, noise shift): f_shift
        # widens the range; the 4 bit field next to it is the matrix's noise shift (ERRATA 69)
        for out, frac_bits, lsb_bypass, coeffs, *_noise in sub.matrices:
            mask = sum(1 << c for c in range(channels) if coeffs.get(c))
            w.put(out, 4)
            w.put(frac_bits, 4)
            w.put((_noise[0] if _noise else 0) + 1, 3)  # f_shift + 1
            w.put(int(lsb_bypass), 2)  # the bits a sample of the LSB bypass (ERRATA 69)
            w.put(_noise[1] if len(_noise) > 1 else 0, 4)
            w.put(mask, channels)
        for _out, frac_bits, _bypass, coeffs, *_noise in sub.matrices:
            for c in range(channels):
                if coeffs.get(c):
                    w.signed(coeffs[c], frac_bits + 2)
        w.put(0, 1)  # no second precision term
    elif sub.matrices:
        w.put(1, 1)
        w.put(len(sub.matrices), 4)
        for out, frac_bits, lsb_bypass, coeffs, *noise in sub.matrices:
            w.put(out, 4)
            w.put(frac_bits, 4)
            w.put(1 if lsb_bypass else 0, 1)
            # Under noise_type 1 the matrices have no coefficients for the noise channels, and each
            # ends with its noise shift (ERRATA 32).
            noise_type = sub.sync == 0x31EB
            for c in range(sub.mm + 1 + (0 if noise_type else ref.NOISE_CHANNELS)):
                if coeffs.get(c):
                    w.put(1, 1)
                    w.signed(coeffs[c], frac_bits + 2)
                else:
                    w.put(0, 1)
            if noise_type:
                w.put(noise[0] if noise else sub.matrix_noise_shift, 4)
    else:
        w.put(0, 1)
    w.put(1, 1)
    for shift in sub.out_shift:
        w.signed(shift, 4)
    w.put(1, 1)
    for ch in range(sub.max_chan + 1):
        w.put(sub.channels[ch].quant if ch in sub.channels else 0, 4)
    for ch in range(sub.min_chan, sub.max_chan + 1):
        channel = sub.channels[ch]
        w.put(1, 1)
        for flt, is_iir in ((channel.fir, False), (channel.iir, True)):
            if flt:
                w.put(1, 1)
                write_filter(w, flt, is_iir, channel.iir_state if is_iir else None)
            else:
                w.put(0, 1)
        lsbs, offset = params[ch]
        w.put(1, 1)
        w.signed(offset, 15)
        w.put(channel.huff_type, 2)
        w.put(lsbs, 5)


def write_light_block_header(w, sub, size, section=None):
    w.put(0, 1)
    w.put(1, 1)
    w.put(size, 9)
    if section is None:
        w.put(0, 1)  # matrices
    else:
        w.put(1, 1)
        write_object_section(w, sub, section)
    w.put(0, 1)  # output shifts
    w.put(0, 1)  # quantiser steps
    for _ in range(sub.min_chan, sub.max_chan + 1):
        w.put(0, 1)


def output_sources(ch_assign):
    """The matrix channel each output channel takes: output p takes the matrix channel m with
    ch_assign[m] == p (TH spec 7.4, ERRATA 42)."""
    return [ch_assign.index(p) for p in range(len(ch_assign))]


def lossless_fold(rows, ch_assign):
    """The check of the interval whose output rows these are: each sample shifted by the matrix
    channel its output channel takes (ERRATA 12, 42; provisional)."""
    fold = 0
    sources = output_sources(ch_assign)
    for row in rows:
        for ch, value in enumerate(row):
            fold ^= (value & 0xFFFFFF) << sources[ch]
            fold &= 0xFFFFFFFF
    return ref.fold_check(fold)


def pad_signals(stream):
    """Zeros to the end of the last unit, which the padding samples of the end marker cover."""
    total = stream.units * stream.per_unit
    for sub in stream.substreams:
        extra = total - len(sub.output)
        sub.output = list(sub.output) + [(0,) * (sub.mm + 1)] * extra
        sub.coded = {ch: list(values) + [0] * extra for ch, values in sub.coded.items()}
        sub.bypass = {m: list(bits) + [0] * extra for m, bits in sub.bypass.items()}


def build_stream(stream):
    per_unit = stream.per_unit
    interval = stream.restart_interval * per_unit
    pad_signals(stream)
    prepared = []
    for sub in stream.substreams:
        coding = {}
        res = {}
        lsb_limit = 31 if sub.sync == 0x31EC else 24
        for ch in range(sub.min_chan, sub.max_chan + 1):
            channel = sub.channels[ch]
            res[ch] = residuals(channel, sub.coded[ch], interval)
            coding[ch] = choose_coding(channel, res[ch], lsb_limit)
        prepared.append((coding, res))

    data = bytearray()
    for unit in range(stream.units):
        is_restart = unit % stream.restart_interval == 0
        first = unit * per_unit
        padding = per_unit * stream.units - stream.samples if unit == stream.units - 1 else 0
        segments = []
        for index, sub in enumerate(stream.substreams):
            coding, res = prepared[index]
            w = BitWriter()
            blocks = [8, per_unit - 8] if is_restart else [per_unit]
            position = first
            for block_index, size in enumerate(blocks):
                w.put(1, 1)
                restart_here = is_restart and block_index == 0
                w.put(1 if restart_here else 0, 1)
                if restart_here:
                    previous = list(range(first - interval, first)) if unit else []
                    rows = [sub.output[i] for i in previous]
                    this = unit // stream.restart_interval
                    check = lossless_fold(rows, assign_of(sub, this - 1)) if previous else 0
                    w.append(write_restart_header(sub, check, assign=assign_of(sub, this)))
                    write_full_block_header(w, sub, coding, size, sub.sections.get(position))
                else:
                    write_light_block_header(w, sub, size, sub.sections.get(position))
                body = BitWriter()
                for i in range(position, position + size):
                    for m, matrix in enumerate(sub.matrices):
                        if matrix[2]:
                            body.put(sub.bypass[m][i], int(matrix[2]))
                    for ch in range(sub.min_chan, sub.max_chan + 1):
                        lsbs, offset = coding[ch]
                        encode_residual(body, sub.channels[ch], res[ch][i], lsbs, offset)
                if sub.data_check:
                    w.put(body.count + 16, 16)  # counted from the start of this field (ERRATA 21)
                w.append(body)
                if sub.data_check:
                    w.put(
                        0, 8
                    )  # block_data_crc: the algorithm is not known (ERRATA open question 9)
                w.put(1 if block_index == len(blocks) - 1 else 0, 1)
                position += size
            if stream.marker == "every" or (stream.marker == "last" and padding):
                w.align(1)
                w.put(0, -(w.count + 32) % 16)
                w.put(stream.marker_pattern, 20)
                w.put(padding, 12)
            elif stream.terminator_b and index == 3:
                if stream.terminator_b == "end":
                    w.put(0, -w.count % 16)
                w.put(0x1234, 16)
            payload = w.bytes()
            if len(payload) % 2:
                payload += b"\x00"
            segments.append(
                payload + bytes([ref.parity_substream(payload), ref.crc8_substream(payload)])
            )

        directory = bytearray()
        end = 0
        for segment in segments:
            end += len(segment) // 2
            word = (1 if stream.drc_words else 0) << 15
            word |= (0 if is_restart else 1) << 14
            word |= 1 << 13
            word |= end
            directory += word.to_bytes(2, "big")
            if stream.drc_words:
                gain = (-100 + unit) & 0x1FF
                directory += ((gain << 7) | ((unit % 8) << 4)).to_bytes(2, "big")
        body = b"".join(segments)
        extra = b""
        payload = stream.extra_data(unit) if stream.extra_data else None
        if payload is not None:  # a unit with no payload has no extra data region
            frame = bytearray(b"\x00\x00") + len(payload).to_bytes(2, "big") + payload
            if len(frame) % 2 == 0:
                frame += b"\x00"
            frame += b"\x00"  # the parity byte, below
            frame[-1] = parity_extra(frame)
            words = len(frame) // 2
            length = words - 1  # extra_data_length: the words less one (TH spec 16.8)
            nibble = ((length >> 8) ^ (length >> 4) ^ length ^ 0xF) & 0xF  # TH spec 16.2
            frame[0:2] = ((nibble << 12) | length).to_bytes(2, "big")
            extra = bytes(frame)
        major_sync = write_major_sync(stream) if is_restart else b""
        length = (4 + len(major_sync) + len(directory) + len(body) + len(extra)) // 2
        timing = (unit * per_unit) & 0xFFFF
        header = bytearray((length & 0xFFF).to_bytes(2, "big") + timing.to_bytes(2, "big"))
        nibble = 0xF ^ ref.nibble_fold(bytes(header)) ^ ref.nibble_fold(bytes(directory))
        header[0] |= nibble << 4
        data += bytes(header) + major_sync + bytes(directory) + body + extra
    return bytes(data)


def parity_extra(frame):
    """TH spec 16.4: the framing words, the seed and the bytes between them and the parity byte."""
    reserved = frame[2] >> 4
    length = ((frame[2] & 0xF) << 8) | frame[3]
    accumulator = reserved ^ length ^ 0xA9
    for byte in frame[4:-1]:
        accumulator ^= byte
    return ((accumulator >> 8) ^ accumulator) & 0xFF


def write_major_sync(stream):
    sync = b"\xf8\x72\x6f" + (b"\xba" if stream.kind == "truehd" else b"\xbb")
    if stream.kind == "truehd":
        # As the library's encoder writes it (TH spec 5.1): the assignment in the eight channel
        # field and, as far as six channels fit, in the six channel field; the modifiers 0, or 3
        # for mono. FFmpeg's decoder refuses a major sync whose six channel assignment is zero or
        # whose channel count disagrees with it.
        assignment = stream.channel_assignment
        six = assignment if stream.channels <= 6 else assignment & 0xF
        modifier = 3 if stream.channels == 1 else 0
        format_info = (stream.rate_code << 28) | (modifier << 22) | (modifier << 20)
        format_info |= ((six & 0x1F) << 15) | (modifier << 13) | (assignment & 0x1FFF)
    else:
        format_info = (stream.word_code << 28) | (0xF << 24) | (stream.rate_code << 20) | 0x0F0000
        format_info |= {1: 0, 2: 1}.get(stream.channels, 0)  # the channel arrangement
    body = sync + format_info.to_bytes(4, "big") + (0xB752).to_bytes(2, "big")
    body += (0x4000 if stream.kind == "mlp" else 0).to_bytes(2, "big") + b"\x00\x00"
    body += ((1 << 15) | 0x0C7F).to_bytes(2, "big")
    count = len(stream.substreams)
    body += bytes([count << 4, 0x80 if count == 4 else 0x14]) + b"\x00" * 8
    return body + ref.crc16_major_sync(body).to_bytes(2, "big")


# --- the signals and the cases -----------------------------------------------------------------


def filt(coefficients, shift):
    """A filter whose coeff_q is 8 (TH spec 9.3 says at least 8) and whose prediction is that of
    `coefficients` at `shift`: the coefficients scaled up by the difference."""
    return [c << (8 - shift) for c in coefficients], 8


def tone(count, seed, amplitude=2**21):
    rng = random.Random(seed)
    parts = [
        (rng.uniform(0.004, 0.2), rng.uniform(0, 6.28), rng.uniform(0.15, 0.5)) for _ in range(3)
    ]
    return [
        int(amplitude * sum(a * math.sin(f * n + p) for f, p, a in parts)) + rng.randint(-300, 300)
        for n in range(count)
    ]


def case_filters():
    n = 40 * 48
    s0, s1 = tone(n, 1), tone(n, 2)
    channels = {
        0: Channel(fir=filt([96, -32], 6), huff_type=1),
        1: Channel(fir=filt([48], 6), iir=filt([20, -8], 6), huff_type=3),
    }
    sub = Substream(0, 1, 1, channels, list(zip(s0, s1, strict=True)), {0: s0, 1: s1})
    return (
        Stream([sub], n),
        "FIR, IIR and Huffman codebooks 1 and 3 with offsets",
        ["fir", "iir", "huffman"],
    )


def case_filter_orders():
    """Every order a channel's filters can have: IIR 1 to 4, FIR 8, and the two filters together
    at the 8 they may add up to (TH spec 9.2; the decoder refuses 9). An IIR filter feeds back its
    own residual, so its coefficients add up to less than 1 (here 64 of 64) to stay stable."""
    n = 40 * 24
    sources = [tone(n, 60 + c, 2**14) for c in range(6)]
    fir8 = [96, -64, 40, -24, 16, -10, 6, -3]
    channels = {
        0: Channel(iir=filt([40], 6)),
        1: Channel(iir=filt([24, -12, 6], 6)),
        2: Channel(iir=filt([20, -10, 6, -3], 6)),
        3: Channel(fir=filt(fir8, 6)),
        4: Channel(fir=filt(fir8[:4], 6), iir=filt([16, -8, 4, -2], 6)),
        5: Channel(fir=filt(fir8[:5], 6), iir=filt([24, -12, 6], 6)),
    }
    sub = Substream(0, 5, 5, channels, list(zip(*sources, strict=True)), dict(enumerate(sources)))
    return (
        Stream([sub], n),
        "IIR orders 1 to 4, FIR order 8, and FIR and IIR together at 4 + 4 and 5 + 3",
        ["fir", "iir", "filter_orders"],
    )


def case_bypass():
    n = 40 * 40
    s0, s1 = tone(n, 3), tone(n, 4)
    k = 77
    coded1, bits = [], []
    for a, x in zip(s0, s1, strict=True):
        t = (a * k) >> 8
        even = x & ~1
        c = even - t
        if c & 1:
            c += 1
        coded1.append(c)
        bits.append(x & 1)
    channels = {0: Channel(fir=filt([64], 6)), 1: Channel(quant=1, fir=filt([32], 6))}
    matrices = [(1, 8, True, {0: k, 1: 256})]
    sub = Substream(
        0,
        1,
        1,
        channels,
        list(zip(s0, s1, strict=True)),
        {0: s0, 1: coded1},
        matrices=matrices,
        bypass={0: bits},
    )
    return (
        Stream([sub], n),
        "LSB bypass: a matrix with a quantised output channel",
        ["lsb_bypass", "matrix"],
    )


def case_matrices():
    n = 40 * 40
    sources = [tone(n, 10 + c) for c in range(6)]
    sources[3] = [v & ~1 for v in sources[3]]  # shifted left by 1 at the output
    assign = [1, 0, 2, 3, 5, 4]
    shifts = [0, 0, 0, 1, 0, 0]
    # matrix channel m carries the output position j = ch_assign[m] (TH spec 7.4, ERRATA 42; the
    # permutation is its own inverse, so the other reading gives the same stream)
    x = [None] * 6
    for m, j in enumerate(assign):
        x[m] = [v >> shifts[j] for v in sources[j]]
    a, b, d, e = 41, -23, 90, -61
    pre = [list(col) for col in x]
    pre[0] = [x0 - ((x1 * e) >> 8) for x0, x1 in zip(x[0], x[1], strict=True)]
    pre[5] = [x5 - ((p0 * d) >> 8) for x5, p0 in zip(x[5], pre[0], strict=True)]
    pre[4] = [x4 - ((x2 * a + x3 * b) >> 8) for x4, x2, x3 in zip(x[4], x[2], x[3], strict=True)]
    matrices = [
        (4, 8, False, {2: a, 3: b, 4: 256}),
        (5, 8, False, {0: d, 5: 256}),
        (0, 8, False, {1: e, 0: 256}),
    ]
    huff = [0, 2, 0, 1, 3, 0]
    channels = {
        c: Channel(fir=filt([40], 6) if c % 2 == 0 else None, huff_type=huff[c]) for c in range(6)
    }
    output = [tuple(sources[j][i] for j in range(6)) for i in range(n)]
    sub = Substream(
        0,
        5,
        5,
        channels,
        output,
        dict(enumerate(pre)),
        ch_assign=assign,
        matrices=matrices,
        out_shift=shifts,
    )
    return (
        Stream([sub], n),
        "three matrices, a permuted ch_assign, an output shift",
        ["matrices", "ch_assign", "output_shift"],
    )


def case_data_check():
    n = 40 * 24
    s0, s1 = tone(n, 20), tone(n, 21)
    channels = {0: Channel(fir=filt([64], 6)), 1: Channel()}
    sub = Substream(
        0, 1, 1, channels, list(zip(s0, s1, strict=True)), {0: s0, 1: s1}, data_check=True
    )
    return Stream([sub], n), "data_check_present: block_data_bits and the CRC byte", ["data_check"]


def stereo_stream(seed, n, **options):
    s0, s1 = tone(n, seed), tone(n, seed + 1)
    channels = {0: Channel(fir=filt([64], 6)), 1: Channel(fir=filt([32], 6))}
    sub = Substream(0, 1, 1, channels, list(zip(s0, s1, strict=True)), {0: s0, 1: s1})
    return Stream([sub], n, **options)


def case_marker_last():
    return (
        stereo_stream(30, 40 * 20 - 17),
        "end-of-stream marker, 17 padding samples",
        ["end_marker"],
    )


def case_marker_every():
    stream = stereo_stream(32, 40 * 20 - 9, marker="every")
    return (
        stream,
        "end-of-stream marker in every unit, 9 padding samples at the end",
        ["end_marker"],
    )


def case_marker_shifted():
    stream = stereo_stream(39, 40 * 20 - 11, marker="every", marker_pattern=0x348D3)
    return (
        stream,
        "terminatorA as 0x348D3 and its count ending the segment of every unit, 11 padding samples",
        ["end_marker", "terminator_a_0x348d3"],
    )


def case_extra_data():
    stream = stereo_stream(
        34, 40 * 20, extra_data=lambda u: bytes((u * 7 + i) & 0xFF for i in range(5 + u % 17))
    )
    return stream, "extra data with an opaque payload in every unit", ["extra_data"]


def case_drc_word():
    return (
        stereo_stream(36, 40 * 20, drc_words=True),
        "the directory's DRC word in every unit",
        ["drc_word"],
    )


def case_sixteen():
    n = 40 * 32
    src = [tone(n, 50 + c, 2**20) for c in range(16)]

    def sub_for(lo, hi, sync=0x31EA):
        channels = {
            c: Channel(fir=filt([48], 6) if c % 3 == 0 else None, huff_type=c % 4)
            for c in range(lo, hi + 1)
        }
        rows = [tuple(src[c][i] for c in range(hi + 1)) for i in range(n)]
        return Substream(
            lo, hi, hi, channels, rows, {c: src[c] for c in range(lo, hi + 1)}, sync=sync
        )

    subs = [sub_for(0, 1), sub_for(2, 5), sub_for(6, 7, 0x31EC), sub_for(8, 15, 0x31EC)]
    stream = Stream(subs, n, terminator_b=True)
    return (
        stream,
        "four substreams, the last of 16 channels with terminatorB",
        ["substream_3", "terminator_b"],
    )


def case_sixteen_terminator_end():
    stream, _description, _features = case_sixteen()
    stream.terminator_b = "end"
    return (
        stream,
        "four substreams, terminatorB ending the segment of substream 3 instead of following "
        "its last block",
        ["substream_3", "terminator_b_at_end"],
    )


def case_eight_channels():
    """7.1 as the library's encoder lays it out: substreams of 2, 6 and 8 channels, the third
    with restart sync 0x31EB, the only one FFmpeg's decoder reads above 6 channels (ERRATA 32)."""
    n = 40 * 24
    src = [tone(n, 80 + c, 2**14) for c in range(8)]

    def sub_for(lo, hi, sync=0x31EA):
        channels = {
            c: Channel(fir=filt([48], 6) if c % 3 == 0 else None, huff_type=c % 4)
            for c in range(lo, hi + 1)
        }
        rows = [tuple(src[c][i] for c in range(hi + 1)) for i in range(n)]
        return Substream(
            lo, hi, hi, channels, rows, {c: src[c] for c in range(lo, hi + 1)}, sync=sync
        )

    return (
        Stream([sub_for(0, 1), sub_for(2, 5), sub_for(6, 7, 0x31EB)], n),
        "7.1: substreams of 2, 6 and 8 channels, the last under restart sync 0x31EB",
        ["eight_channels", "substreams_3"],
    )


def case_substream3_limits():
    """Substream 3 at the limits TH spec v3.6 gives it (8.3, 9.3, 10.3, 15.3): restart sync
    0x31EC, 15 primitive matrices, max_matrix_chan 15, `huff_lsbs` above 24 and values past 24
    bits before the matrices. The signal is 24 bit throughout; what is large is the prediction. A
    filter that predicts badly (a gain of about -6, or an IIR filter fed by residuals of 2^28)
    leaves a residual of 2^28 and more, which `huff_lsbs` 29 to 31 carry, and the inverse of the
    matrices makes the coded channels larger than the output."""
    n = 40 * 32
    src = [tone(n, 70 + c, 2**22) for c in range(16)]
    plan = [(8 + i % 8, 8 + (i * 3 + 1) % 8) for i in range(15)]
    assert all(out != other for out, other in plan)
    weights = [41, -23, 90, -61, 17, -75, 33, 120, -9, 64, -111, 5, 77, -48, 100]
    matrices = [
        (out, 8, False, {out: 256, other: w}) for (out, other), w in zip(plan, weights, strict=True)
    ]
    coded = [list(column) for column in src]
    for (out, other), w in reversed(list(zip(plan, weights, strict=True))):
        coded[out] = [o - ((x * w) >> 8) for o, x in zip(coded[out], coded[other], strict=True)]

    def sub_for(lo, hi, sync=0x31EA):
        channels = {
            c: Channel(fir=filt([48], 6) if c % 3 == 0 else None, huff_type=c % 4)
            for c in range(lo, hi + 1)
        }
        rows = [tuple(src[c][i] for c in range(hi + 1)) for i in range(n)]
        return Substream(
            lo, hi, hi, channels, rows, {c: src[c] for c in range(lo, hi + 1)}, sync=sync
        )

    bad_fir, bad_iir = filt([-1500], 8), filt([200], 8)
    wide = {
        c: Channel(
            fir=bad_fir if c in (8, 10, 12) else None,
            iir=bad_iir if c in (9, 11) else None,
            huff_type=0,
        )
        for c in range(8, 16)
    }
    rows = [tuple(src[c][i] for c in range(16)) for i in range(n)]
    last = Substream(
        8, 15, 15, wide, rows, {c: coded[c] for c in range(8, 16)}, matrices=matrices, sync=0x31EC
    )
    subs = [sub_for(0, 1), sub_for(2, 5), sub_for(6, 7, 0x31EC), last]
    stream = Stream(subs, n, terminator_b=True)
    return (
        stream,
        "substream 3 with 15 matrices, huff_lsbs up to 31 and coded values past 24 bits",
        ["substream_3", "terminator_b", "matrices_15", "huff_lsbs_31", "wide_prediction"],
    )


def case_substream3_fine_matrices(
    extra_data=None,
    noise_shifts=None,
    seed=0x2A5F31,
    bypass_index=None,
    assign=None,
    bypass_width=1,
):
    """A twelve channel substream 3 (restart sync 0x31EC) whose matrices carry `frac_bits` 15, 14,
    13 and 0, as the object substream of the owner's discs does (ERRATA 60): four channels of
    objects predicted from the 7.1 bed and from each other, with coefficients that need the last
    bit of the 17 bit field (an odd value at `frac_bits` 15) and one that reads two channels.
    With `assign` the substream's ch_assign is that permutation of 0 to 11: the matrices and the
    coded channels are the same, and output position p carries the matrix channel m with
    assign[m] == p (TH spec 7.4, ERRATA 42), which is how the owner's discs order their objects."""
    n = 40 * 32
    src = [tone(n, 90 + c, 2**21) for c in range(12)]
    # (out, frac_bits, {channel: coefficient}); the unity coefficient of `out` is 2^frac_bits
    plan = [
        (8, 15, {1: 12345, 4: -20001}),
        (9, 15, {8: 32767, 0: -1}),
        (10, 14, {2: 8191, 9: -16383}),
        (11, 13, {10: 4001, 7: -3333}),
        (9, 0, {11: 1}),
        (8, 15, {3: 65535, 5: -65536}),
        # f_shift 1 (TH spec 10.2.1a): the coefficient is raw / 2^(frac_bits - 1), so 9489 at
        # frac_bits 13 is 2.3167 and the unity coefficient of the output is 4096
        (11, 13, {0: 9489, 6: -4001}, 1),
    ]
    steps = [(*item, 0) if len(item) == 3 else item for item in plan]
    shifts = list(noise_shifts) if noise_shifts else [0] * len(steps)
    matrices = [
        (
            out,
            frac,
            bypass_width if k == bypass_index else False,
            {out: 1 << (frac - fs), **others},
            fs,
            shift,
        )
        for k, ((out, frac, others, fs), shift) in enumerate(zip(steps, shifts, strict=True))
    ]
    bypass_bits = {}
    # Generator 1's noise (TH spec 17.3) as each matrix adds it, in the matrices' 2^-15 domain
    terms = noise1_terms(n, 40, 0, 16, seed, shifts)
    coded = [list(column) for column in src]
    for k in reversed(range(len(steps))):
        out, frac, others, fs = steps[k]
        scale = 15 - frac + fs
        if k == bypass_index:
            # the matrix's output channel is quantised to a step of 2^width and the bits it loses
            # are sent in the block data, `width` bits a sample (ERRATA 69)
            wanted = coded[out]
            stepped = [
                ((sum(coded[c][i] * w for c, w in others.items()) << scale) + 2 * terms[k][i]) >> 15
                for i in range(n)
            ]
            low = (1 << bypass_width) - 1
            coded[out] = [(x & ~low) - (st & ~low) for x, st in zip(wanted, stepped, strict=True)]
            bypass_bits[k] = [x & low for x in wanted]
            continue
        coded[out] = [
            o
            - (((sum(coded[c][i] * w for c, w in others.items()) << scale) + 2 * terms[k][i]) >> 15)
            for i, o in enumerate(coded[out])
        ]

    def sub_for(lo, hi, sync=0x31EA):
        channels = {
            c: Channel(fir=filt([48], 6) if c % 3 == 0 else None, huff_type=c % 4)
            for c in range(lo, hi + 1)
        }
        rows = [tuple(src[c][i] for c in range(hi + 1)) for i in range(n)]
        return Substream(
            lo, hi, hi, channels, rows, {c: src[c] for c in range(lo, hi + 1)}, sync=sync
        )

    wide = {
        c: Channel(
            fir=filt([-1500], 8) if c == 8 else None,
            huff_type=c % 4,
            quant=bypass_width if bypass_index is not None and c == steps[bypass_index][0] else 0,
        )
        for c in range(8, 12)
    }
    sources = output_sources(assign) if assign else list(range(12))
    rows = [tuple(src[sources[p]][i] for p in range(12)) for i in range(n)]
    last = Substream(
        8,
        11,
        11,
        wide,
        rows,
        {c: coded[c] for c in range(8, 12)},
        ch_assign=assign,
        matrices=matrices,
        bypass=bypass_bits,
        sync=0x31EC,
        noise_seed=seed,
    )
    subs = [sub_for(0, 1), sub_for(2, 5), sub_for(6, 7, 0x31EB), last]
    stream = Stream(subs, n, terminator_b=True, extra_data=extra_data)
    return (
        stream,
        "a twelve channel substream 3 whose matrices have frac_bits 15, 14, 13 and 0",
        ["substream_3", "twelve_channels", "frac_bits_15", "matrices_6"],
    )


# Two object audio metadata payloads (TS 103 420 clause 5.5, payload id 11), made by
# iclforge::objects::oba::build_payload for a dynamic-object-only programme with an LFE and 11
# dynamic objects (12 objects: the 12 channels of the substream), one update block, ramp 1536. The
# objects of the second are the first's moved 0.1 to the right. test_cli_truehd_objects.cpp reads
# both back.
OAMD_A = bytes.fromhex(
    "17844a0000a210c6804091925d020138fc04091a89c0201658a04091bee60201"
    "960804091d52d0201c29c04091eb6c0201ef2a0400"
)
OAMD_B = bytes.fromhex(
    "17844a0000a212468040919e5d020150fc04091b49c0201818a04091cae60201"
    "ae0804091e12d0201da9c04091f96c0201fb2a0400"
)


def atmos_extra_data(unit):
    """An evolution frame (the EMDF reading) holding the object audio metadata, in units 0 and 16:
    the first at the start of the stream and the second 640 samples on."""
    payload = {0: OAMD_A, 16: OAMD_B}.get(unit)
    if payload is None:
        return None
    return evolution.build_evo_frame("emdf", payloads=[(11, evolution.default_config(), payload)])


def case_substream3_object_noise():
    """The stream of `case_substream3_fine_matrices` with a noise shift on four of its seven
    matrices (2, 0, 7, 11, 0, 4, 1): the object matrices draw generator 1's noise (ERRATA 69)."""
    stream, _description, _features = case_substream3_fine_matrices(
        noise_shifts=(2, 0, 7, 11, 0, 4, 1)
    )
    return (
        stream,
        "a twelve channel substream 3 whose matrices carry noise shifts",
        ["substream_3", "twelve_channels", "object_noise", "matrices_7"],
    )


def case_substream3_object_bypass_wide():
    """As `case_substream3_object_bypass`, with a bypass of 2 bits a sample (field_2b 2), as
    soll_e12 and soll_sil6 have it: the output channel is quantised to a step of 4 and the two
    bits it loses come before the first channel's residual in every sample (ERRATA 69)."""
    stream, _description, _features = case_substream3_fine_matrices(
        noise_shifts=(2, 0, 7, 11, 0, 4, 1), bypass_index=2, bypass_width=2
    )
    return (
        stream,
        "a twelve channel substream 3 whose third matrix has a 2 bit LSB bypass",
        ["substream_3", "twelve_channels", "object_noise", "lsb_bypass", "matrices_7"],
    )


def case_substream3_object_bypass():
    """The stream of `case_substream3_fine_matrices` with noise shifts, and an LSB bypass on its
    third matrix (output channel 10, quantised to a step of 2): the two bits next to `f_shift` in
    an object matrix header have bit 0 as the bypass (ERRATA 69, provisional)."""
    stream, _description, _features = case_substream3_fine_matrices(
        noise_shifts=(2, 0, 7, 11, 0, 4, 1), bypass_index=2
    )
    return (
        stream,
        "a twelve channel substream 3 whose third matrix has an LSB bypass",
        ["substream_3", "twelve_channels", "object_noise", "lsb_bypass", "matrices_7"],
    )


def case_substream3_object_extension():
    """A twelve channel substream 3 whose object matrices carry the extension of the section
    (ERRATA 69): a finer part for each coefficient, coefficient = base + delta / 2^(width + 1) in
    units of the base's last place. Four matrices (output 8 to 11, `frac_bits` 12, 13 with
    `f_shift` 1, 0 and 15; noise shifts 2, 0, 5 and 3), a block of the stream for each case:

      sample   0  restart: new headers, C2 with widths 3, 5, 2 and 0 (a matrix with no deltas),
                  deltas, and a present coefficient of base 0 with a delta
      sample  80  new headers with different masks, C2 clear: the widths of the matrices at the
                  same place stand and the deltas follow the new masks
      sample 160  A clear: the matrices and the deltas stand
      sample 240  A set, B clear: the headers stand, new coefficients, the widths stand, new deltas
      sample 320  restart: no deltas (A2 set, B2 clear), which also clears the widths
      sample 400  A clear with C2: widths 0, 4, 6 and 2 on the matrices that stand, and deltas
      sample 480  A clear, no extension: the deltas of sample 400 stand

    The coded channels are what the matrices, run backwards in integers, leave of the signal: the
    sum of a matrix is (base sum + noise) << bits plus the sum of rows times deltas, shifted down
    by 15 + bits, with bits = width + 1 (0 without a width)."""
    n = 40 * 16
    seed = 0x2A5F31
    src = [tone(n, 110 + c, 2**21) for c in range(12)]
    shifts = (2, 0, 5, 3)
    # (out, frac_bits, f_shift, noise shift) of the four matrices, which every section repeats;
    # the unity coefficient of the output is 2^(frac_bits - f_shift)
    heads = [
        (8, 12, 0, shifts[0]),
        (9, 13, 1, shifts[1]),
        (10, 0, 0, shifts[2]),
        (11, 15, 0, shifts[3]),
    ]
    unity = [1 << (frac - fs) for _out, frac, fs, _noise in heads]
    sections = {
        0: ObjectSection(
            coeffs=[
                {8: unity[0], 1: 777, 4: 0, 3: -1500},
                {9: unity[1], 8: 2500, 0: -3333},
                {10: unity[2], 9: 1, 2: -1},
                {11: unity[3], 10: -20001, 7: 12345},
            ],
            headers=heads,
            a2=True,
            b2=True,
            c2=True,
            widths=[3, 5, 2, 0],
            reserved=[1, 2, 3, 0],
            deltas=[
                {8: 0, 1: -5, 4: 6, 3: 7},
                {9: 0, 8: -23, 0: 17},
                {10: 0, 9: 3, 2: -4},
                {},
            ],
        ),
        80: ObjectSection(
            coeffs=[
                {8: unity[0], 1: -300, 5: 421, 6: 0},
                {9: unity[1], 8: 2500, 10: -900, 0: 1},
                {10: unity[2], 9: -1},
                {11: unity[3], 10: 5000, 7: -12345},
            ],
            headers=heads,
            a2=True,
            b2=True,
            deltas=[
                {8: 0, 1: 4, 5: -8, 6: 3},
                {9: 0, 8: 5, 10: 31, 0: -32},
                {10: 0, 9: 2},
                {},
            ],
        ),
        160: ObjectSection(a=False),
        240: ObjectSection(
            coeffs=[
                {8: unity[0], 1: 512, 5: -77, 6: 0},
                {9: unity[1], 8: -2100, 10: 333, 0: 200},
                {10: unity[2], 9: 1},
                {11: unity[3], 10: -1, 7: 999},
            ],
            b=False,
            a2=True,
            b2=True,
            deltas=[
                {8: 0, 1: 7, 5: -8, 6: -1},
                {9: 0, 8: 5, 10: -17, 0: 31},
                {10: 0, 9: -4},
                {},
            ],
        ),
        320: ObjectSection(
            coeffs=[
                {8: unity[0], 2: -1234, 3: 1000},
                {9: unity[1], 8: -1500, 1: 3},
                {10: unity[2], 8: -1},
                {11: unity[3], 9: -16000, 0: 1},
            ],
            headers=heads,
            a2=True,
        ),
        400: ObjectSection(
            a=False,
            a2=True,
            b2=True,
            c2=True,
            widths=[0, 4, 6, 2],
            reserved=[0, 3, 2, 1],
            deltas=[
                {},
                {9: 0, 8: 15, 1: -16},
                {10: 0, 8: -33},
                {11: 0, 9: -4, 0: 3},
            ],
        ),
        480: ObjectSection(a=False),
    }
    restarts = {0, 320}
    resolve_sections(sections, restarts)
    terms = noise1_terms(n, 40, 0, 8, seed, shifts)
    starts = sorted(sections)
    coded = [list(column) for column in src]
    for start, end in zip(starts, [*starts[1:], n], strict=True):
        matrices = sections[start].resolved
        for i in range(start, end):
            for k in reversed(range(len(matrices))):
                m = matrices[k]
                out, scale = m["out"], 15 - m["frac_bits"] + m["f_shift"]
                assert m["raw"][out] == 1 << (m["frac_bits"] - m["f_shift"])
                assert m["delta"].get(out, 0) == 0
                base = sum(coded[c][i] * (r << scale) for c, r in m["raw"].items() if c != out)
                fine = sum(coded[c][i] * (d << scale) for c, d in m["delta"].items() if c != out)
                bits = m["width"] + 1 if m["width"] else 0
                total = ((base + 2 * terms[k][i]) << bits) + fine
                coded[out][i] -= total >> (15 + bits)

    def sub_for(lo, hi, sync=0x31EA):
        channels = {
            c: Channel(fir=filt([48], 6) if c % 3 == 0 else None, huff_type=c % 4)
            for c in range(lo, hi + 1)
        }
        rows = [tuple(src[c][i] for c in range(hi + 1)) for i in range(n)]
        return Substream(
            lo, hi, hi, channels, rows, {c: src[c] for c in range(lo, hi + 1)}, sync=sync
        )

    wide = {
        c: Channel(fir=filt([-1500], 8) if c == 8 else None, huff_type=c % 4) for c in range(8, 12)
    }
    rows = [tuple(src[c][i] for c in range(12)) for i in range(n)]
    last = Substream(
        8,
        11,
        11,
        wide,
        rows,
        {c: coded[c] for c in range(8, 12)},
        matrices=[(out, frac, False, {}, fs, noise) for out, frac, fs, noise in heads],
        sync=0x31EC,
        noise_seed=seed,
        sections=sections,
    )
    subs = [sub_for(0, 1), sub_for(2, 5), sub_for(6, 7, 0x31EB), last]
    stream = Stream(subs, n, terminator_b=True, restart_interval=8)
    return (
        stream,
        "a twelve channel substream 3 whose object matrices carry the extension's finer parts",
        ["substream_3", "twelve_channels", "object_extension", "object_noise", "matrices_4"],
    )


def case_atmos_objects():
    stream, _description, _features = case_substream3_fine_matrices(atmos_extra_data)
    return (
        stream,
        "a twelve channel substream 3 with object audio metadata in two evolution frames",
        ["substream_3", "twelve_channels", "extra_data", "oamd"],
    )


# The ch_assign of the permuted twelve channel stream below: assign[m] is the output position of
# matrix channel m, so output position p carries the signal of the m with assign[m] == p.
PERMUTED_ASSIGN = [1, 2, 3, 4, 5, 0, 6, 7, 8, 9, 10, 11]


def case_atmos_objects_permuted():
    """`hb_atmos_objects` with a ch_assign on substream 3 that is not its own inverse: a cycle of
    the first six matrix channels (the LFE of the programme, output position 0, is the signal of
    matrix channel 5). The signals, the matrices and the metadata are the same, so the twelve
    channels of the decode are those of `hb_atmos_objects` in another order."""
    stream, _description, _features = case_substream3_fine_matrices(
        atmos_extra_data, assign=PERMUTED_ASSIGN
    )
    return (
        stream,
        "a twelve channel substream 3 with a permuted ch_assign and object audio metadata",
        ["substream_3", "twelve_channels", "extra_data", "oamd", "ch_assign"],
    )


def noise_sequences(count, interval, seed, shift):
    """Generator 0's two noise channels (TH spec 17.2); the state restarts with each interval."""
    first, second = [], []
    state = seed
    for n in range(count):
        if n % interval == 0:
            state = seed
        shr7 = (state >> 7) & 0xFFFF  # 16 bits (ERRATA 23, provisional)
        first.append(ref.int8(state >> 15) * (1 << shift))
        second.append(ref.int8(shr7) * (1 << shift))
        state = ((state << 16) ^ shr7 ^ (shr7 << 5)) & 0xFFFFFFFF
    return first, second


def case_iir_state():
    """new_states with a state_shift (TH spec 9.2): the IIR history at lag i + 1 is
    values[i] << state_shift, sent with the filter at every restart. Two channels with an IIR
    filter, the states of 4 and 8 bits wide and shifted by 6 and 0, and one with a state of zero
    width (all zero). The first samples after each restart depend on the states."""
    n = 40 * 40
    sources = [tone(n, 100 + c, 2**20) for c in range(3)]
    channels = {
        0: Channel(iir=filt([40, -12], 6), iir_state=(6, 9, [23, -17])),
        1: Channel(fir=filt([48], 6), iir=filt([20, -8, 4], 6), iir_state=(8, 0, [90, -101, 33])),
        2: Channel(iir=filt([30], 6), iir_state=(0, 5, [0])),
    }
    sub = Substream(0, 2, 2, channels, list(zip(*sources, strict=True)), dict(enumerate(sources)))
    return (
        Stream([sub], n),
        "IIR new_states: signed values loaded shifted left by state_shift, at every restart",
        ["iir", "iir_state"],
    )


def case_output_shift_permuted():
    """The output shift belongs to the matrix channel (TH spec 7.3: output_shift[mat_ch]), and a
    ch_assign that is a three-cycle puts a different output position on each: the first header
    value shifts matrix channel 0, which output position 2 takes. FFmpeg's decode agrees."""
    n = 40 * 24
    assign = [1, 2, 0]  # matrix channel m goes to output position assign[m]
    shifts = [1, 2, 3]  # indexed by matrix channel
    wanted = [tone(n, 110 + p, 2**20) for p in range(3)]  # by output position
    coded = {}
    for m, position in enumerate(assign):
        # the low `shifts[m]` bits of the output are zero: the output is the coded value << shift
        wanted[position] = [v & ~((1 << shifts[m]) - 1) for v in wanted[position]]
        coded[m] = [v >> shifts[m] for v in wanted[position]]
    channels = {m: Channel(fir=filt([40], 6) if m == 1 else None, huff_type=m) for m in range(3)}
    sub = Substream(
        0,
        2,
        2,
        channels,
        list(zip(*wanted, strict=True)),
        coded,
        ch_assign=assign,
        out_shift=shifts,
    )
    return (
        Stream([sub], n),
        "ch_assign [1, 2, 0] with output shifts 1, 2, 3 by matrix channel",
        ["ch_assign", "output_shift"],
    )


def case_ch_assign_changes():
    """ch_assign changes between restart headers, as the owner's second disc does ((4, 5, 1, 0, 2,
    3) to (4, 5, 2, 1, 0, 3) there). The filters' histories belong to the matrix channels (TH spec
    9.6), and a restart header does not clear them, so each matrix channel's filters run on
    through a change, over a coded signal that now carries another output position. Every channel
    has a FIR filter, so a decoder that forgets its histories at the change decodes the first
    samples after it to other values."""
    units, restart_interval = 48, 8
    n = 40 * units
    cycle = [[4, 5, 1, 0, 2, 3], [4, 5, 2, 1, 0, 3]]
    wanted = [tone(n, 130 + p, 2**21) for p in range(6)]  # by output position
    length = 40 * restart_interval
    coded = {m: [wanted[cycle[(i // length) % 2][m]][i] for i in range(n)] for m in range(6)}
    channels = {
        0: Channel(fir=filt([96, -32], 6), huff_type=1),
        1: Channel(fir=filt([48], 6), iir=filt([20, -8], 6), huff_type=3),
        2: Channel(fir=filt([64, 16, -8], 6), huff_type=2),
        3: Channel(fir=filt([40], 6)),
        4: Channel(fir=filt([72, -24], 6), iir=filt([12], 6), huff_type=1),
        5: Channel(fir=filt([56], 6), huff_type=3),
    }
    sub = Substream(
        0,
        5,
        5,
        channels,
        list(zip(*wanted, strict=True)),
        coded,
        ch_assign_cycle=cycle,
    )
    return (
        Stream([sub], n, restart_interval=restart_interval),
        "ch_assign (4, 5, 1, 0, 2, 3) and (4, 5, 2, 1, 0, 3) in turn at each restart, FIR and IIR",
        ["ch_assign", "ch_assign_changes", "fir", "iir"],
    )


def case_output_shift_negative():
    """A negative output shift is valid and scales the output down by an arithmetic right shift
    (TH spec 8.2.1): the coded values are the output shifted up, with low bits that the shift
    drops. FFmpeg's decoder does not apply it (it forces the shift to zero) and returns the coded
    values."""
    n = 40 * 24
    assign = [2, 0, 1]
    shifts = [-1, -3, 0]  # by matrix channel
    wanted = [tone(n, 120 + p, 2**19) for p in range(3)]  # the coded values stay within 24 bits
    rng = random.Random(7)
    coded = {}
    for m, position in enumerate(assign):
        low = -shifts[m]
        coded[m] = [(v << low) + rng.randrange(1 << low) if low else v for v in wanted[position]]
    channels = {
        m: Channel(fir=filt([40], 6) if m != 1 else None, huff_type=m + 1) for m in range(3)
    }
    sub = Substream(
        0,
        2,
        2,
        channels,
        list(zip(*wanted, strict=True)),
        coded,
        ch_assign=assign,
        out_shift=shifts,
    )
    return (
        Stream([sub], n),
        "negative output shifts -1 and -3: an arithmetic right shift of the coded values",
        ["output_shift", "output_shift_negative", "ch_assign"],
    )


def case_mlp_word16():
    """An MLP stream that declares 16 bit words (group 1 word length code 0) and codes samples
    that have bits set below bit 8 (TH spec 7.4): the decoder does not clear them. FFmpeg's
    decoder does, and returns the samples with their low byte zero."""
    n = 40 * 24
    s0, s1 = tone(n, 130, 2**20), tone(n, 131, 2**20)
    channels = {0: Channel(fir=filt([64], 6)), 1: Channel(fir=filt([32], 6))}
    sub = Substream(0, 1, 1, channels, list(zip(s0, s1, strict=True)), {0: s0, 1: s1})
    return (
        Stream([sub], n, kind="mlp", word_code=0),
        "an MLP stream declaring 16 bit words and coding bits below bit 8",
        ["mlp", "word_length_16"],
    )


def case_noise():
    n = 40 * 40
    s0, s1 = tone(n, 60), tone(n, 61)
    seed, shift, c, d = 0x2A5F31, 3, 77, -45
    first, second = noise_sequences(n, 16 * 40, seed, shift)
    coded0 = [x - ((v * d) >> 8) for x, v in zip(s0, second, strict=True)]
    coded1 = [x - ((v * c) >> 8) for x, v in zip(s1, first, strict=True)]
    # Noise channel 1 is matrix coefficient mm + 1 = 2 and channel 2 is 3.
    matrices = [(1, 8, False, {1: 256, 2: c}), (0, 8, False, {0: 256, 3: d})]
    channels = {0: Channel(fir=filt([64], 6)), 1: Channel(huff_type=2)}
    sub = Substream(
        0,
        1,
        1,
        channels,
        list(zip(s0, s1, strict=True)),
        {0: coded0, 1: coded1},
        matrices=matrices,
        noise_shift=shift,
        noise_seed=seed,
    )
    description = "noise generator 0: two noise channels through matrix coefficients"
    return Stream([sub], n), description, ["noise_generator_0"]


def case_noise_type1():
    """Restart sync 0x31EB (noise_type 1), one matrix, no noise coefficients and a matrix noise
    shift of zero: nothing draws on generator 1."""
    n = 40 * 40
    s0, s1 = tone(n, 70), tone(n, 71)
    c = 77
    coded1 = [b - ((a * c) >> 8) for a, b in zip(s0, s1, strict=True)]
    channels = {0: Channel(fir=filt([64], 6)), 1: Channel(huff_type=2)}
    sub = Substream(
        0,
        1,
        1,
        channels,
        list(zip(s0, s1, strict=True)),
        {0: s0, 1: coded1},
        matrices=[(1, 8, False, {0: c, 1: 256})],
        sync=0x31EB,
    )
    description = "noise_type 1 (restart sync 0x31EB) with every matrix noise shift zero"
    return Stream([sub], n), description, ["noise_type_1"]


def noise1_terms(samples, per_unit, rate_code, interval_units, seed, shifts):
    """What each matrix adds to its sum for each sample under noise generator 1 (TH spec 17.3):
    terms[k][n] is table value * 2^(shift + 7), or 0 where matrix k's shift is 0. A buffer of
    64 << rate factor values is filled for every access unit from the seed, which each restart
    header loads and each unit runs on; matrix k of `count` starts the unit at index `count - k`
    and steps by twice that plus one (ERRATA, noise generator 1)."""
    size = 64 << (rate_code & 3)
    count = len(shifts)
    terms = [[0] * samples for _ in shifts]
    state = seed
    for unit in range(samples // per_unit):
        if unit % interval_units == 0:
            state = seed
        buffer = []
        for _ in range(size):
            index = (state >> 15) & 0xFF
            buffer.append(ref.NOISE_TABLE[index])
            state = ((state << 8) ^ index ^ (index << 5)) & 0xFFFFFFFF
        for k, shift in enumerate(shifts):
            if not shift:
                continue
            left = count - k
            for i in range(per_unit):
                slot = (left + i * (2 * left + 1)) % size
                terms[k][unit * per_unit + i] = buffer[slot] << (shift + 7)
    return terms


def case_noise1(rate_code=0, shifts=(3, 0, 12), seed=0x2A5F31, label="48 kHz"):
    """Noise generator 1: three matrices on a four-channel substream, the first with a noise shift
    of 3 (a sum that is not a multiple of 2^14), the second with none at 48 kHz (it still counts
    for the start index of the others), the third with 12 and a quantised output channel. The
    signal is 24 bit; the coded channels are the signal less what the matrices will add."""
    per_unit = 40 << (rate_code & 3)
    n = per_unit * 40
    src = [tone(n, 90 + c, 2**21) for c in range(4)]
    src[3] = [v & ~1 for v in src[3]]  # the output channel of the third matrix has a step of 2
    terms = noise1_terms(n, per_unit, rate_code, 16, seed, shifts)
    a, b, d = 77, -45, 91
    coded = [list(column) for column in src]
    # matrix 0: row 1 += (row 0 * a + noise) >> 14 ; matrix 1: row 2 += row 1 * b >> 8 ;
    # matrix 2: row 3 += (row 0 * d + noise) >> 14, kept to a multiple of 2.
    coded[1] = [
        x1 - (((x0 * a << 6) + t) >> 14) for x0, x1, t in zip(src[0], src[1], terms[0], strict=True)
    ]
    coded[2] = [
        x2 - (((x1 * b << 6) + t) >> 14) for x1, x2, t in zip(src[1], src[2], terms[1], strict=True)
    ]
    coded[3] = [
        x3 - ((((x0 * d << 6) + t) >> 14) & ~1)
        for x0, x3, t in zip(src[0], src[3], terms[2], strict=True)
    ]
    matrices = [
        (1, 8, False, {0: a, 1: 256}, shifts[0]),
        (2, 8, False, {1: b, 2: 256}, shifts[1]),
        (3, 8, False, {0: d, 3: 256}, shifts[2]),
    ]
    channels = {
        0: Channel(fir=filt([48], 6)),
        1: Channel(huff_type=2),
        2: Channel(huff_type=1),
        3: Channel(huff_type=0, quant=1),
    }
    output = [tuple(src[c][i] for c in range(4)) for i in range(n)]
    sub = Substream(
        0,
        3,
        3,
        channels,
        output,
        dict(enumerate(coded)),
        matrices=matrices,
        sync=0x31EB,
        noise_seed=seed,
    )
    description = f"noise generator 1 at {label}: three matrices, noise shifts {list(shifts)}"
    return (
        Stream([sub], n, rate_code=rate_code),
        description,
        ["noise_type_1", "noise_generator_1", "matrix_noise_shift"],
    )


def case_noise1_96k():
    return case_noise1(1, (7, 2, 15), 0x5A3C71, "96 kHz")


def case_noise1_192k():
    return case_noise1(2, (1, 4, 9), 0x0F00D1, "192 kHz")


def case_noise1_shift():
    """One matrix with a noise shift of 3 on a stereo substream: the smallest stream that draws on
    generator 1."""
    n = 40 * 40
    s0, s1 = tone(n, 72), tone(n, 73)
    c, seed, shift = 77, 0x13579B, 3
    terms = noise1_terms(n, 40, 0, 16, seed, [shift])
    coded1 = [b - (((a * c << 6) + t) >> 14) for a, b, t in zip(s0, s1, terms[0], strict=True)]
    channels = {0: Channel(fir=filt([64], 6)), 1: Channel(huff_type=2)}
    sub = Substream(
        0,
        1,
        1,
        channels,
        list(zip(s0, s1, strict=True)),
        {0: s0, 1: coded1},
        matrices=[(1, 8, False, {0: c, 1: 256}, shift)],
        sync=0x31EB,
        noise_seed=seed,
    )
    description = "noise_type 1 with a matrix noise shift of 3: the smallest stream drawing on it"
    features = ["noise_type_1", "noise_generator_1", "matrix_noise_shift"]
    return Stream([sub], n), description, features


def case_noise1_bypass():
    """LSB bypass under restart sync 0x31EB: the bypassed bit is one explicit bit a sample from the
    block data (TH spec 10.2.1), whatever the noise type; here the matrix draws noise as well."""
    n = 40 * 40
    s0, s1 = tone(n, 74), tone(n, 75)
    k, seed, shift = 77, 0x2468AC, 5
    terms = noise1_terms(n, 40, 0, 16, seed, [shift])
    coded1, bits = [], []
    for a, x, t in zip(s0, s1, terms[0], strict=True):
        step = ((a * k << 6) + t) >> 14
        coded1.append((x & ~1) - (step & ~1))
        bits.append(x & 1)
    channels = {0: Channel(fir=filt([64], 6)), 1: Channel(quant=1, fir=filt([32], 6))}
    sub = Substream(
        0,
        1,
        1,
        channels,
        list(zip(s0, s1, strict=True)),
        {0: s0, 1: coded1},
        matrices=[(1, 8, True, {0: k, 1: 256}, shift)],
        bypass={0: bits},
        sync=0x31EB,
        noise_seed=seed,
    )
    description = "LSB bypass under restart sync 0x31EB, the matrix drawing noise as well"
    return Stream([sub], n), description, ["noise_type_1", "lsb_bypass", "matrix_noise_shift"]


CASES = {
    "hb_filters": case_filters,
    "hb_filter_orders": case_filter_orders,
    "hb_lsb_bypass": case_bypass,
    "hb_matrices": case_matrices,
    "hb_iir_state": case_iir_state,
    "hb_output_shift_permuted": case_output_shift_permuted,
    "hb_ch_assign_changes": case_ch_assign_changes,
    "hb_output_shift_negative": case_output_shift_negative,
    "hb_mlp_word16": case_mlp_word16,
    "hb_noise_generator_0": case_noise,
    "hb_noise_type1": case_noise_type1,
    "hb_noise_type1_shift": case_noise1_shift,
    "hb_noise_type1_lsb_bypass": case_noise1_bypass,
    "hb_noise_generator_1": case_noise1,
    "hb_noise_generator_1_96k": case_noise1_96k,
    "hb_noise_generator_1_192k": case_noise1_192k,
    "hb_data_check": case_data_check,
    "hb_marker_last": case_marker_last,
    "hb_marker_every": case_marker_every,
    "hb_marker_shifted": case_marker_shifted,
    "hb_extra_data": case_extra_data,
    "hb_drc_word": case_drc_word,
    "hb_sixteen_channels": case_sixteen,
    "hb_sixteen_channels_b_end": case_sixteen_terminator_end,
    "hb_eight_channels": case_eight_channels,
    "hb_substream3_limits": case_substream3_limits,
    "hb_substream3_fine_matrices": case_substream3_fine_matrices,
    "hb_substream3_object_noise": case_substream3_object_noise,
    "hb_substream3_object_bypass": case_substream3_object_bypass,
    "hb_substream3_object_bypass_wide": case_substream3_object_bypass_wide,
    "hb_substream3_object_extension": case_substream3_object_extension,
    "hb_atmos_objects": case_atmos_objects,
    "hb_atmos_objects_permuted": case_atmos_objects_permuted,
}


# Streams a decoder has to refuse: written and listed in the manifest, but not decoded to a signal.
# None at present: the streams generator 1's missing table made a decoder refuse decode now.
REFUSED = {}

# What FFmpeg's strict decode does with a stream, where it is not "the signal it was built from,
# whole", as seen with FFmpeg 6.1.1 (tools/checks/verify_truehd_interop.py --handbuilt holds FFmpeg
# to this; with another major version only the "equal" outcomes are held):
#   bed      - it decodes substreams 0 to 2 and leaves substream 3 alone: it returns the best
#              presentation (`pcm_sha256` here is that presentation's), in its own order
#   short    - it returns `returned` samples, the first `equal_samples` of them the signal's
#   refused  - it fails
#   decodes  - it decodes, and the library refuses the stream (REFUSED)
#   differs  - it decodes to other samples than the signal (the manifest's `ffmpeg` entry holds
#              the SHA-256 of what it returns: the model of what it does is in ffmpeg_outcome)
FFMPEG_VERSION = "6.1.1"
FFMPEG = {
    "hb_marker_every": {
        "outcome": "short",
        "returned": 80,
        "equal_samples": 40,
        "note": "with the end-of-stream marker in every access unit FFmpeg returns two of the 20 "
        "(the first is the signal's) and skips the rest "
        '("Stream parameters not seen; skipping frame")',
    },
    "hb_eight_channels": {
        "outcome": "equal",
        "columns": [0, 1, 2, 3, 6, 7, 4, 5],
        "note": "FFmpeg returns the channels of a 7.1 stream in the order of its own layout "
        "bits, the back pair before the side pair (ERRATA 33): its column k holds the stream's "
        "channel `columns`[k]",
    },
    "hb_output_shift_negative": {
        "outcome": "differs",
        "note": "FFmpeg does not apply a negative output shift (it forces the shift to zero) and "
        "returns the coded values of the matrix channels (TH spec 8.2.1)",
    },
    "hb_mlp_word16": {
        "outcome": "differs",
        "codec": "mlp",
        "note": "FFmpeg writes a 16 bit MLP stream as 16 bit samples and clears the bits below bit "
        "8; the decoder does not (TH spec 7.4)",
    },
    "hb_marker_shifted": {
        "outcome": "refused",
        "note": "FFmpeg recognises the end-of-stream marker as 0xD234E only; with 0x348D3 it reads "
        "the end of each segment as data and fails",
    },
    "hb_sixteen_channels_b_end": {
        "outcome": "refused",
        "note": "restart sync 0x31EC in substreams 2 and 3 (FFmpeg reads 0x31EA and 0x31EB only)",
    },
    "hb_sixteen_channels": {
        "outcome": "refused",
        "note": "restart sync 0x31EC in substreams 2 and 3 (FFmpeg reads 0x31EA and 0x31EB only)",
    },
    "hb_substream3_limits": {
        "outcome": "refused",
        "note": "restart sync 0x31EC in substreams 2 and 3 (FFmpeg reads 0x31EA and 0x31EB only)",
    },
    "hb_substream3_fine_matrices": {
        "outcome": "bed",
        "columns": [0, 1, 2, 3, 6, 7, 4, 5],
        "note": "FFmpeg reads substreams 0 to 2 and leaves substream 3 (restart sync 0x31EC) "
        "alone: it returns the 7.1 bed, the best presentation, in its own channel order "
        "(ERRATA 33)",
    },
    "hb_substream3_object_noise": {
        "outcome": "bed",
        "columns": [0, 1, 2, 3, 6, 7, 4, 5],
        "note": "FFmpeg reads substreams 0 to 2 and leaves substream 3 (restart sync 0x31EC) "
        "alone: it returns the 7.1 bed, the best presentation, in its own channel order "
        "(ERRATA 33)",
    },
    "hb_substream3_object_bypass_wide": {
        "outcome": "bed",
        "columns": [0, 1, 2, 3, 6, 7, 4, 5],
        "note": "FFmpeg reads substreams 0 to 2 and leaves substream 3 (restart sync 0x31EC) "
        "alone: it returns the 7.1 bed, the best presentation, in its own channel order "
        "(ERRATA 33)",
    },
    "hb_substream3_object_bypass": {
        "outcome": "bed",
        "columns": [0, 1, 2, 3, 6, 7, 4, 5],
        "note": "FFmpeg reads substreams 0 to 2 and leaves substream 3 (restart sync 0x31EC) "
        "alone: it returns the 7.1 bed, the best presentation, in its own channel order "
        "(ERRATA 33)",
    },
    "hb_substream3_object_extension": {
        "outcome": "bed",
        "columns": [0, 1, 2, 3, 6, 7, 4, 5],
        "note": "FFmpeg reads substreams 0 to 2 and leaves substream 3 (restart sync 0x31EC) "
        "alone: it returns the 7.1 bed, the best presentation, in its own channel order "
        "(ERRATA 33)",
    },
    "hb_atmos_objects": {
        "outcome": "bed",
        "columns": [0, 1, 2, 3, 6, 7, 4, 5],
        "note": "FFmpeg reads substreams 0 to 2 and leaves substream 3 (restart sync 0x31EC) "
        "alone: it returns the 7.1 bed, the best presentation, in its own channel order "
        "(ERRATA 33)",
    },
    "hb_atmos_objects_permuted": {
        "outcome": "bed",
        "columns": [0, 1, 2, 3, 6, 7, 4, 5],
        "note": "FFmpeg reads substreams 0 to 2 and leaves substream 3 (restart sync 0x31EC) "
        "alone: it returns the 7.1 bed, the best presentation, in its own channel order "
        "(ERRATA 33)",
    },
}
ORACLE = {
    "equal": "ffmpeg",
    "short": "ffmpeg-short",
    "bed": "ffmpeg-bed",
    "refused": "none",
    "decodes": "ffmpeg-only",
    "differs": "none",
}


def ffmpeg_outcome(name, rows, stream=None):
    """What the manifest says FFmpeg's strict decode of stream `name` does (its signal is rows)."""
    outcome = dict(FFMPEG.get(name, {"outcome": "equal"}))
    if outcome["outcome"] == "short":
        outcome["pcm_sha256"] = hashlib.sha256(s32le(rows[: outcome["equal_samples"]])).hexdigest()
    if outcome["outcome"] == "bed":  # FFmpeg returns the best presentation of the stream
        best = expected(stream, "best")
        outcome["pcm_sha256"] = hashlib.sha256(s32le(best)).hexdigest()
    if outcome["outcome"] == "differs":
        if name == "hb_mlp_word16":  # the bits below bit 8 cleared
            returned = [tuple(v >> 8 << 8 for v in row) for row in rows]
        else:  # hb_output_shift_negative: the coded values, by output position
            sub = stream.substreams[0]
            sources = output_sources(sub.ch_assign)
            returned = [
                tuple(sub.coded[sources[p]][i] for p in range(len(sources)))
                for i in range(stream.samples)
            ]
        outcome["pcm_sha256"] = hashlib.sha256(s32le(returned)).hexdigest()
    return outcome


def s32le(rows):
    flat = [value << 8 for row in rows for value in row]
    return struct.pack(f"<{len(flat)}i", *flat)


def expected(stream, presentation):
    index = (
        min(len(stream.substreams), 3) - 1 if presentation == "best" else len(stream.substreams) - 1
    )
    return stream.substreams[index].output[: stream.samples]


def generate(out):
    out.mkdir(parents=True, exist_ok=True)
    fixtures = []
    for name, make in CASES.items():
        stream, description, features = make()[:3]
        data = build_stream(stream)
        (out / f"{name}.thd").write_bytes(data)
        presentations = ["all", "best"] if len(stream.substreams) == 4 else ["best"]
        for presentation in presentations:
            rows = expected(stream, presentation)
            outcome = ffmpeg_outcome(name, rows, stream)
            fixtures.append(
                {
                    "name": name
                    if presentation == "best" and len(presentations) == 1
                    else f"{name}_{presentation}",
                    "file": f"{name}.thd",
                    "description": description,
                    "features": features,
                    "oracle": ORACLE[outcome["outcome"]],
                    "ffmpeg": outcome,
                    "codec": outcome.get("codec", "truehd"),
                    "presentation": presentation,
                    "sample_rate": stream.sample_rate,
                    "channels": len(rows[0]),
                    "access_units": stream.units,
                    "pcm_samples": len(rows),
                    "stream_sha256": hashlib.sha256(data).hexdigest(),
                    "pcm_sha256": hashlib.sha256(s32le(rows)).hexdigest(),
                }
            )
            if stream.extra_data:
                fixtures[-1]["extra_data_first_hex"] = stream.extra_data(0).hex()
    refused = []
    for name, make in REFUSED.items():
        stream, description, features = make()[:3]
        data = build_stream(stream)
        (out / f"{name}.thd").write_bytes(data)
        refused.append(
            {
                "name": name,
                "file": f"{name}.thd",
                "description": description,
                "features": features,
                "oracle": ORACLE[FFMPEG[name]["outcome"]],
                "ffmpeg": FFMPEG[name],
                "error": "kUnsupported",
                "stream_sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    comment = (
        "Made by tools/references/truehd_writer.py. pcm_sha256 is the SHA-256 of the signal each "
        "was built from, as 32-bit little-endian samples (the 24-bit value shifted left by 8). "
        "`ffmpeg` says what FFmpeg's strict decode of the stream does, as seen with "
        "ffmpeg_version (outcome equal: it returns that signal; the other outcomes are described "
        "in their note), and `oracle` names the result: ffmpeg, ffmpeg-short, ffmpeg-bed, "
        "ffmpeg-only "
        "(FFmpeg decodes a stream the library refuses) or none. "
        "tools/checks/verify_truehd_interop.py --handbuilt holds FFmpeg and the library to it."
    )
    manifest = {
        "_comment": comment,
        "ffmpeg_version": FFMPEG_VERSION,
        "fixtures": fixtures,
        "refused": refused,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for fixture in fixtures:
        name, channels, samples = fixture["name"], fixture["channels"], fixture["pcm_samples"]
        print(f"{name:<28} {channels:>2} ch  {samples:>6} samples")


def check(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    failures = 0
    for fixture in manifest["fixtures"]:
        data = (directory / fixture["file"]).read_bytes()
        rows, _rate, _channels = ref.decode_stream(data, fixture["presentation"])
        good = hashlib.sha256(s32le(rows)).hexdigest() == fixture["pcm_sha256"]
        failures += not good
        print(f"{'ok  ' if good else 'FAIL'} {fixture['name']}")
    for fixture in manifest.get("refused", []):
        data = (directory / fixture["file"]).read_bytes()
        try:
            ref.decode_stream(data, "best")
            refused = False
        except ref.StreamError:
            refused = True
        failures += not refused
        print(f"{'ok  ' if refused else 'FAIL'} {fixture['name']} is refused")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate")
    gen.add_argument("--out", type=pathlib.Path, default=HANDBUILT)
    chk = sub.add_parser("check")
    chk.add_argument("directory", nargs="?", type=pathlib.Path, default=HANDBUILT)
    args = parser.parse_args()
    if args.command == "generate":
        generate(args.out)
        return 0
    return check(args.directory)


if __name__ == "__main__":
    sys.exit(main())
