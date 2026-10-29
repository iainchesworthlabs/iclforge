#!/usr/bin/env python3
"""A TrueHD stream writer for hand-built streams, and the hand-built streams it makes.

FFmpeg's encoders do not write an IIR filter, LSB bypass, several matrices with a permuted channel
assignment, the block data check, the end-of-stream marker in every unit, extra data, the DRC word
of the directory, or the 16-channel substream. The decoders have to read them all, and nothing
outside this repository can say whether they read them right: **none of these streams has an
external oracle.** What they are held to is the signal each was made from. The codec is lossless,
so a decoder that is right gives that signal back, and the expected PCM in
testdata/truehd/handbuilt/manifest.json is the writer's input, not any decoder's output. The
layout the writer follows is libs/truehd/ERRATA.md's (the specification's where the streams of
FFmpeg cannot say), so a mistake in reading the specification is in the writer and both decoders
alike; what these streams do check is that the two decoders and the writer agree, that the
features work together, and that nothing regresses.

    truehd_writer.py generate [--out DIR]    # write the streams and the manifest
    truehd_writer.py check [DIR]             # decode them with truehd_syntax.py and compare

Not written: restart_header's heavy DRC (TH spec 12.2, 7.1), whose place among the header's bits
the streams do not show (ERRATA 7), and noise generator 1 (sync 0x31EB), whose table is not
available."""

import argparse
import hashlib
import json
import math
import pathlib
import random
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
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

    def __init__(self, fir=None, iir=None, huff_type=0, quant=0):
        self.fir = fir  # (coefficients, shift)
        self.iir = iir
        self.huff_type = huff_type
        self.quant = quant


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
    ):
        self.min_chan, self.max_chan, self.mm = min_chan, max_chan, mm
        self.channels = channels  # {ch: Channel}
        self.output = output
        self.coded = coded  # {ch: [samples]}
        self.ch_assign = ch_assign or list(range(mm + 1))
        self.matrices = list(matrices)  # (out, frac_bits, lsb_bypass, {channel: coefficient})
        self.bypass = bypass or {}  # {matrix index: [bit per sample]}
        self.out_shift = out_shift or [0] * (mm + 1)
        self.sync = sync
        self.data_check = data_check
        self.noise_shift = noise_shift
        self.noise_seed = noise_seed


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
        eightch_assignment=0x1,
    ):
        self.substreams = substreams
        self.samples = samples
        self.rate_code = rate_code
        self.kind = kind
        self.restart_interval = restart_interval
        self.marker = marker  # "last": the last unit only, "every": each unit
        self.drc_words = drc_words
        self.extra_data = extra_data  # a function of the unit's index giving its payload
        self.terminator_b = terminator_b
        self.eightch_assignment = eightch_assignment

    @property
    def per_unit(self):
        return 40 << {0: 0, 1: 1, 2: 2, 8: 0, 9: 1, 10: 2}[self.rate_code]

    @property
    def units(self):
        return -(-self.samples // self.per_unit)


# --- the residuals ----------------------------------------------------------------------------


def residuals(channel, coded, restart_samples):
    """The residual of each sample (ERRATA 13): the coded value less the prediction, in units of
    2^quant. The filters' histories start at zero at each restart."""
    fir, iir = channel.fir, channel.iir
    shift = fir[1] if fir else (iir[1] if iir else 0)
    out = []
    fir_history = [0] * 8
    iir_history = [0] * 8
    for n, value in enumerate(coded):
        if n % restart_samples == 0:
            fir_history = [0] * 8
            iir_history = [0] * 8
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


def write_filter(w, flt, is_iir):
    coeffs, shift = flt
    w.put(len(coeffs), 4)
    w.put(shift, 4)
    bits = max(max(abs(c) for c in coeffs).bit_length() + 1, 2)
    w.put(bits, 5)
    w.put(0, 3)  # coeff_shift
    for c in coeffs:
        w.signed(c, bits)
    w.put(0, 1)  # new_states / the FIR filter's flag (ERRATA 9)


def write_restart_header(sub, check, timing=0):
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
    for ch in sub.ch_assign:
        w.put(ch, 6)
    bits = BitWriter()
    bits.append(w)
    w.put(ref.crc8_restart(bits.bytes(), 0, w.count), 8)
    return w


def write_full_block_header(w, sub, params, size):
    w.put(0, 1)  # new_guards: the flags stay 0xFF
    w.put(1, 1)
    w.put(size, 9)
    if sub.matrices:
        w.put(1, 1)
        w.put(len(sub.matrices), 4)
        for out, frac_bits, lsb_bypass, coeffs in sub.matrices:
            w.put(out, 4)
            w.put(frac_bits, 4)
            w.put(1 if lsb_bypass else 0, 1)
            for c in range(sub.mm + 1 + ref.NOISE_CHANNELS):
                if coeffs.get(c):
                    w.put(1, 1)
                    w.signed(coeffs[c], frac_bits + 2)
                else:
                    w.put(0, 1)
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
                write_filter(w, flt, is_iir)
            else:
                w.put(0, 1)
        lsbs, offset = params[ch]
        w.put(1, 1)
        w.signed(offset, 15)
        w.put(channel.huff_type, 2)
        w.put(lsbs, 5)


def write_light_block_header(w, sub, size):
    w.put(0, 1)
    w.put(1, 1)
    w.put(size, 9)
    w.put(0, 1)  # matrices
    w.put(0, 1)  # output shifts
    w.put(0, 1)  # quantiser steps
    for _ in range(sub.min_chan, sub.max_chan + 1):
        w.put(0, 1)


def lossless_fold(rows):
    fold = 0
    for row in rows:
        for ch, value in enumerate(row):
            fold ^= (value & 0xFFFFFF) << ch
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
                    check = lossless_fold([sub.output[i] for i in previous]) if previous else 0
                    w.append(write_restart_header(sub, check))
                    write_full_block_header(w, sub, coding, size)
                else:
                    write_light_block_header(w, sub, size)
                body = BitWriter()
                for i in range(position, position + size):
                    for m, (_out, _frac, lsb_bypass, _coeffs) in enumerate(sub.matrices):
                        if lsb_bypass:
                            body.put(sub.bypass[m][i], 1)
                    for ch in range(sub.min_chan, sub.max_chan + 1):
                        lsbs, offset = coding[ch]
                        encode_residual(body, sub.channels[ch], res[ch][i], lsbs, offset)
                if sub.data_check:
                    w.put(body.count, 16)
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
                w.put(MARKER, 20)
                w.put(padding, 12)
            elif stream.terminator_b and index == 3:
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
        if stream.extra_data:
            payload = stream.extra_data(unit)
            frame = bytearray(b"\x00\x00") + len(payload).to_bytes(2, "big") + payload
            if len(frame) % 2 == 0:
                frame += b"\x00"
            frame += b"\x00"  # the parity byte, below
            frame[-1] = parity_extra(frame)
            words = len(frame) // 2
            nibble = ((words >> 8) ^ (words >> 4) ^ words) & 0xF
            frame[0:2] = ((nibble << 12) | words).to_bytes(2, "big")
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
        format_info = (stream.rate_code << 28) | stream.eightch_assignment
    else:
        format_info = (0x2F << 24) | (stream.rate_code << 20) | 0x0F0000
    body = sync + format_info.to_bytes(4, "big") + (0xB752).to_bytes(2, "big")
    body += (0x4000 if stream.kind == "mlp" else 0).to_bytes(2, "big") + b"\x00\x00"
    body += ((1 << 15) | 0x0C7F).to_bytes(2, "big")
    count = len(stream.substreams)
    body += bytes([count << 4, 0x80 if count == 4 else 0x14]) + b"\x00" * 8
    return body + ref.crc16_major_sync(body).to_bytes(2, "big")


# --- the signals and the cases -----------------------------------------------------------------


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
        0: Channel(fir=([96, -32], 6), huff_type=1),
        1: Channel(fir=([48], 6), iir=([20, -8], 6), huff_type=3),
    }
    sub = Substream(0, 1, 1, channels, list(zip(s0, s1, strict=True)), {0: s0, 1: s1})
    return (
        Stream([sub], n),
        "FIR, IIR and Huffman codebooks 1 and 3 with offsets",
        ["fir", "iir", "huffman"],
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
    channels = {0: Channel(fir=([64], 6)), 1: Channel(quant=1, fir=([32], 6))}
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
    # matrix channel m carries the output position j that ch_assign[j] == m
    x = [None] * 6
    for j, m in enumerate(assign):
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
        c: Channel(fir=([40], 6) if c % 2 == 0 else None, huff_type=huff[c]) for c in range(6)
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
        Stream([sub], n, eightch_assignment=0xF),
        "three matrices, a permuted ch_assign, an output shift",
        ["matrices", "ch_assign", "output_shift"],
    )


def case_data_check():
    n = 40 * 24
    s0, s1 = tone(n, 20), tone(n, 21)
    channels = {0: Channel(fir=([64], 6)), 1: Channel()}
    sub = Substream(
        0, 1, 1, channels, list(zip(s0, s1, strict=True)), {0: s0, 1: s1}, data_check=True
    )
    return Stream([sub], n), "data_check_present: block_data_bits and the CRC byte", ["data_check"]


def stereo_stream(seed, n, **options):
    s0, s1 = tone(n, seed), tone(n, seed + 1)
    channels = {0: Channel(fir=([64], 6)), 1: Channel(fir=([32], 6))}
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
            c: Channel(fir=([48], 6) if c % 3 == 0 else None, huff_type=c % 4)
            for c in range(lo, hi + 1)
        }
        rows = [tuple(src[c][i] for c in range(hi + 1)) for i in range(n)]
        return Substream(
            lo, hi, hi, channels, rows, {c: src[c] for c in range(lo, hi + 1)}, sync=sync
        )

    subs = [sub_for(0, 1), sub_for(2, 5), sub_for(6, 7, 0x31EC), sub_for(8, 15, 0x31EC)]
    stream = Stream(subs, n, terminator_b=True, eightch_assignment=0x4F)
    return (
        stream,
        "four substreams, the last of 16 channels with terminatorB",
        ["substream_3", "terminator_b"],
    )


def noise_sequences(count, interval, seed, shift):
    """Generator 0's two noise channels (TH spec 17.2); the state restarts with each interval."""
    first, second = [], []
    state = seed
    for n in range(count):
        if n % interval == 0:
            state = seed
        shr7 = state >> 7
        first.append(ref.int8(state >> 15) * (1 << shift))
        second.append(ref.int8(shr7) * (1 << shift))
        state = ((state << 16) ^ shr7 ^ (shr7 << 5)) & 0xFFFFFFFF
    return first, second


def case_noise():
    n = 40 * 40
    s0, s1 = tone(n, 60), tone(n, 61)
    seed, shift, c, d = 0x2A5F31, 3, 77, -45
    first, second = noise_sequences(n, 16 * 40, seed, shift)
    coded0 = [x - ((v * d) >> 8) for x, v in zip(s0, second, strict=True)]
    coded1 = [x - ((v * c) >> 8) for x, v in zip(s1, first, strict=True)]
    # Noise channel 1 is matrix coefficient mm + 1 = 2 and channel 2 is 3.
    matrices = [(1, 8, False, {1: 256, 2: c}), (0, 8, False, {0: 256, 3: d})]
    channels = {0: Channel(fir=([64], 6)), 1: Channel(huff_type=2)}
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


CASES = {
    "hb_filters": case_filters,
    "hb_lsb_bypass": case_bypass,
    "hb_matrices": case_matrices,
    "hb_noise_generator_0": case_noise,
    "hb_data_check": case_data_check,
    "hb_marker_last": case_marker_last,
    "hb_marker_every": case_marker_every,
    "hb_extra_data": case_extra_data,
    "hb_drc_word": case_drc_word,
    "hb_sixteen_channels": case_sixteen,
}


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
            fixtures.append(
                {
                    "name": name
                    if presentation == "best" and len(presentations) == 1
                    else f"{name}_{presentation}",
                    "file": f"{name}.thd",
                    "description": description,
                    "features": features,
                    "oracle": "none",
                    "presentation": presentation,
                    "sample_rate": 48000,
                    "channels": len(rows[0]),
                    "access_units": stream.units,
                    "pcm_samples": len(rows),
                    "stream_sha256": hashlib.sha256(data).hexdigest(),
                    "pcm_sha256": hashlib.sha256(s32le(rows)).hexdigest(),
                }
            )
            if stream.extra_data:
                fixtures[-1]["extra_data_first_hex"] = stream.extra_data(0).hex()
    comment = (
        "Made by tools/references/truehd_writer.py. No external decoder reads these streams: "
        "pcm_sha256 is the SHA-256 of the signal each was built from, as 32-bit little-endian "
        "samples (the 24-bit value shifted left by 8)."
    )
    manifest = {"_comment": comment, "fixtures": fixtures}
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
