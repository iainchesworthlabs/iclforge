#!/usr/bin/env python3
"""An independent transcription of the TrueHD and MLP syntax and of the lossless decode.

It is written from the owner's TrueHD (MLP) Reference Specification (v3.5) and from what the
streams of FFmpeg's encoders show (libs/truehd/ERRATA.md says where the two differ), in plain
Python with no code shared with libs/truehd. The C++ decoder is held to it on the committed
fixtures (--check), and both are held to FFmpeg's strict decode
(tools/checks/verify_truehd_interop.py). It is slow and it is meant to be read.

    truehd_syntax.py decode STREAM OUT.s32   # 32-bit little-endian PCM, samples shifted by 8
    truehd_syntax.py check [MANIFEST]        # decode every fixture, compare with the manifest
"""

import argparse
import hashlib
import json
import pathlib
import struct
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO / "testdata" / "truehd"

SYNC_WORD = b"\xf8\x72\x6f"
RESTART_SYNCS = (0x31EA, 0x31EB, 0x31EC)
END_MARKER = 0xD234E  # TH spec 16.5 terminatorA, ERRATA 14
NOISE_CHANNELS = 2
MATRIX_SHIFT = 14

# TH spec App. C: (symbol, code, bits) of the three codebooks.
CODEBOOKS = {
    1: [
        (-7, 0x01, 9),
        (-6, 0x01, 8),
        (-5, 0x01, 7),
        (-4, 0x01, 6),
        (-3, 0x01, 5),
        (-2, 0x01, 4),
        (-1, 0x01, 3),
        (0, 0x04, 3),
        (1, 0x05, 3),
        (2, 0x06, 3),
        (3, 0x07, 3),
        (4, 0x03, 3),
        (5, 0x05, 4),
        (6, 0x09, 5),
        (7, 0x11, 6),
        (8, 0x21, 7),
        (9, 0x41, 8),
        (10, 0x81, 9),
    ],
    2: [
        (-7, 0x01, 9),
        (-6, 0x01, 8),
        (-5, 0x01, 7),
        (-4, 0x01, 6),
        (-3, 0x01, 5),
        (-2, 0x01, 4),
        (-1, 0x01, 3),
        (0, 0x02, 2),
        (1, 0x03, 2),
        (2, 0x03, 3),
        (3, 0x05, 4),
        (4, 0x09, 5),
        (5, 0x11, 6),
        (6, 0x21, 7),
        (7, 0x41, 8),
        (8, 0x81, 9),
    ],
    3: [
        (-7, 0x01, 9),
        (-6, 0x01, 8),
        (-5, 0x01, 7),
        (-4, 0x01, 6),
        (-3, 0x01, 5),
        (-2, 0x01, 4),
        (-1, 0x01, 3),
        (0, 0x01, 1),
        (1, 0x03, 3),
        (2, 0x05, 4),
        (3, 0x09, 5),
        (4, 0x11, 6),
        (5, 0x21, 7),
        (6, 0x41, 8),
        (7, 0x81, 9),
    ],
}
CODES = {
    book: {format(code, f"0{bits}b"): symbol for symbol, code, bits in entries}
    for book, entries in CODEBOOKS.items()
}


class StreamError(Exception):
    pass


class Bits:
    """MSB-first reader over bytes; reading past the end raises."""

    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos

    def read(self, n):
        if n and self.pos + n > len(self.data) * 8:
            raise StreamError("read past the end")
        value = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            value = (value << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return value

    def signed(self, n):
        value = self.read(n)
        return value - (1 << n) if value >> (n - 1) else value

    def left(self):
        return len(self.data) * 8 - self.pos

    def bits(self, start, n):
        saved, self.pos = self.pos, start
        value = self.read(n)
        self.pos = saved
        return value


# --- the check values (ERRATA 4, 5, 6) ----------------------------------------


def crc16_major_sync(covered):
    """CRC-16, polynomial 0x002D, over all but the last two bytes, which are XORed in as a word."""
    register = 0
    for byte in covered[:-2]:
        register ^= byte << 8
        for _ in range(8):
            register = ((register << 1) ^ 0x002D if register & 0x8000 else register << 1) & 0xFFFF
    return register ^ int.from_bytes(covered[-2:], "big")


def crc8_restart(data, first_bit, crc_bit):
    """CRC-8, polynomial 0x1D, over the bits [first_bit, crc_bit - 8); the last 8 are XORed in."""
    reader = Bits(data)
    register = 0
    for position in range(first_bit, crc_bit - 8):
        bit = reader.bits(position, 1)
        feedback = ((register >> 7) & 1) ^ bit
        register = (register << 1) & 0xFF
        if feedback:
            register ^= 0x1D
    return register ^ reader.bits(crc_bit - 8, 8)


def crc8_substream(payload):
    """CRC-8, polynomial 0x63, initial value 0x3C, over all but the last byte, which is XORed in."""
    register = 0x3C
    for byte in payload[:-1]:
        register ^= byte
        for _ in range(8):
            register = ((register << 1) ^ 0x63 if register & 0x80 else register << 1) & 0xFF
    return register ^ payload[-1]


def parity_substream(payload):
    parity = 0xA9
    for byte in payload:
        parity ^= byte
    return parity


def int8(value):
    value &= 0xFF
    return value - 256 if value & 0x80 else value


def s32(value):
    value &= 0xFFFFFFFF
    return value - (1 << 32) if value & 0x80000000 else value


# --- the syntax ----------------------------------------


class Filter:
    def __init__(self):
        self.coeff = []
        self.shift = 0

    @staticmethod
    def read(bits, max_order):
        flt = Filter()
        order = bits.read(4)
        if order > max_order:
            raise StreamError("filter order")
        if order:
            flt.shift = bits.read(4)
            coeff_bits = bits.read(5)
            coeff_shift = bits.read(3)
            flt.coeff = [bits.signed(coeff_bits) << coeff_shift for _ in range(order)]
            state = None
            if bits.read(1):  # ERRATA 9: the FIR filter has the flag too
                state_bits = bits.read(4)
                state_shift = bits.read(4)
                state = [
                    bits.signed(state_bits) << state_shift if state_bits else 0
                    for _ in range(order)
                ]
            flt.state = state
        else:
            flt.state = None
        return flt


class Channel:
    def __init__(self):
        self.fir = Filter()
        self.iir = Filter()
        self.huff_offset = 0
        self.huff_type = 0
        self.huff_lsbs = 24
        self.fir_history = [0] * 8
        self.iir_history = [0] * 8


class Substream:
    """One substream's state, from its restart header on."""

    def __init__(self, index):
        self.index = index
        self.running = False
        self.noise_type = False
        self.interval = None  # the fold of the output since the last restart

    def restart(self, bits, segment):
        start = bits.pos
        sync = bits.read(14)
        if sync not in RESTART_SYNCS:
            raise StreamError("restart sync")
        bits.read(16)  # output_timing
        self.min_chan = bits.read(4)
        self.max_chan = bits.read(4)
        self.max_matrix_chan = bits.read(4)
        self.noise_shift = bits.read(4)
        self.noise_seed = bits.read(23)
        bits.read(19)  # max_shift, max_lsbs, max_bits, max_bits_repeat (ERRATA 7)
        self.data_check = bits.read(1)
        lossless_check = bits.read(8)
        bits.read(16)
        self.ch_assign = [bits.read(6) for _ in range(self.max_matrix_chan + 1)]
        crc_bit = bits.pos
        stored = bits.read(8)
        if crc8_restart(segment, start, crc_bit) != stored:
            raise StreamError("restart header CRC")
        # Restart sync 0x31EB is noise_type 1 (generator 1, whose table is not available). Its
        # matrices have no noise coefficients, so a stream that draws on no noise decodes; one that
        # does (a non-zero matrix noise shift, LSB bypass) is refused in read_matrices (ERRATA 32).
        self.noise_type = sync == 0x31EB
        if self.running and self.interval is not None:
            if fold_check(self.interval) != lossless_check:
                raise StreamError("lossless check")
        self.interval = None
        # The filters' histories run on across a restart (ERRATA 13, provisional): FFmpeg's decoder
        # keeps them, the specification says a restart resets all state.
        previous = getattr(self, "channels", None) if self.running else None
        self.running = True
        self.flags = 0xFF
        self.block_size = 8
        self.matrices = []
        self.output_shift = [0] * (self.max_matrix_chan + 1)
        self.quant = [0] * (self.max_chan + 1)
        self.channels = [Channel() for _ in range(self.max_chan + 1)]
        if previous:
            for channel, old in zip(self.channels, previous, strict=False):
                channel.fir_history = list(old.fir_history)
                channel.iir_history = list(old.iir_history)

    def block_header(self, bits):
        if bits.read(1):
            self.flags = bits.read(8)
        flags = self.flags
        if flags & 0x80 and bits.read(1):
            self.block_size = bits.read(9)
        if flags & 0x40 and bits.read(1):
            self.matrices = self.read_matrices(bits)
        if flags & 0x20 and bits.read(1):
            self.output_shift = [bits.signed(4) for _ in range(self.max_matrix_chan + 1)]
        if flags & 0x10 and bits.read(1):
            self.quant = [bits.read(4) for _ in range(self.max_chan + 1)]
        for ch in range(self.min_chan, self.max_chan + 1):
            if not bits.read(1):
                continue
            channel = self.channels[ch]
            if flags & 0x08 and bits.read(1):
                channel.fir = Filter.read(bits, 8)
            if flags & 0x04 and bits.read(1):
                channel.iir = Filter.read(bits, 4)
                if channel.iir.state is not None:
                    for lag, value in enumerate(channel.iir.state):
                        channel.iir_history[lag] = value
            if flags & 0x02 and bits.read(1):
                channel.huff_offset = bits.signed(15)
            channel.huff_type = bits.read(2)
            channel.huff_lsbs = bits.read(5)

    def read_matrices(self, bits):
        matrices = []
        for _ in range(bits.read(4)):
            out = bits.read(4)
            frac_bits = bits.read(4)
            lsb_bypass = bits.read(1)
            coeffs = []
            count = (
                self.max_matrix_chan + 1 + (0 if self.noise_type else NOISE_CHANNELS)
            )  # ERRATA 8, 32
            for _channel in range(count):
                coeffs.append(
                    bits.signed(frac_bits + 2) << (MATRIX_SHIFT - frac_bits) if bits.read(1) else 0
                )
            if self.noise_type:
                coeffs.extend([0] * NOISE_CHANNELS)
                if bits.read(4) != 0:
                    raise StreamError(
                        "noise_type 1: a non-zero matrix noise shift needs generator 1's table, "
                        "which is not available"
                    )
                if lsb_bypass:
                    raise StreamError(
                        "noise_type 1: LSB bypass needs generator 1's table, which is not available"
                    )
            matrices.append((out, coeffs, lsb_bypass))
        return matrices


def fold_check(fold):
    return (fold ^ (fold >> 8) ^ (fold >> 16) ^ (fold >> 24)) & 0xFF


def residual(bits, channel, quant):
    """One residual (ERRATA 11): signed Huffman symbol and raw LSBs, centred, then the offset."""
    lsb_bits = channel.huff_lsbs - quant
    raw_bits = max(lsb_bits, 0)
    value = channel.huff_offset
    if channel.huff_type:
        table = CODES[channel.huff_type]
        code = ""
        while code not in table:
            code += str(bits.read(1))
            if len(code) > 9:
                raise StreamError("Huffman code")
        value += table[code] << raw_bits
        sign_shift = lsb_bits + 2 - channel.huff_type
        if sign_shift >= 0:
            value -= 1 << sign_shift
    elif lsb_bits > 0:
        value -= 1 << (lsb_bits - 1)
    value += bits.read(raw_bits)
    return s32(value << quant)


def decode_block(sub, bits, rows, first):
    """Decode one block into rows[first:first + block_size], which the substreams share."""
    size = sub.block_size
    start = bits.pos  # the length counts from the start of its 16 bit field (ERRATA 21)
    expected_bits = bits.read(16) if sub.data_check else None
    for row in rows[first : first + size]:
        for column in range(sub.max_matrix_chan + 1):
            if sub.index == 0:
                row[column] = 0
    bypass = []
    for i in range(size):
        bits_for_sample = [
            bits.read(1) if lsb_bypass else 0 for _out, _coeffs, lsb_bypass in sub.matrices
        ]
        bypass.append(bits_for_sample)
        for ch in range(sub.min_chan, sub.max_chan + 1):
            rows[first + i][ch] = residual(bits, sub.channels[ch], sub.quant[ch])
    if sub.data_check:
        if bits.pos - start != expected_bits:
            raise StreamError("block data length")
        bits.read(8)  # block_data_crc: the algorithm is not known (ERRATA open question 9)
    # The prediction filters (ERRATA 13).
    for ch in range(sub.min_chan, sub.max_chan + 1):
        channel = sub.channels[ch]
        keep = s32((-1 << sub.quant[ch]) & 0xFFFFFFFF)
        fir, iir = channel.fir.coeff, channel.iir.coeff
        shift = channel.fir.shift if fir else channel.iir.shift
        for i in range(size):
            prediction = sum(h * c for h, c in zip(channel.fir_history, fir, strict=False))
            prediction += sum(h * c for h, c in zip(channel.iir_history, iir, strict=False))
            prediction >>= shift
            output = s32((prediction + rows[first + i][ch]) & keep)
            channel.fir_history = [output, *channel.fir_history[:7]]
            channel.iir_history = [s32(output - prediction), *channel.iir_history[:7]]
            rows[first + i][ch] = output
    # Generator 0 (TH spec 17.2, ERRATA 23): two noise channels, one step of the LFSR a sample.
    seed = sub.noise_seed
    for i in range(size):
        shr7 = (seed >> 7) & 0xFFFF  # 16 bits, as FFmpeg reads it (ERRATA 23, provisional)
        rows[first + i][16] = int8(seed >> 15) * (1 << sub.noise_shift)
        rows[first + i][17] = int8(shr7) * (1 << sub.noise_shift)
        seed = ((seed << 16) ^ shr7 ^ (shr7 << 5)) & 0xFFFFFFFF
    sub.noise_seed = seed
    # The primitive matrices (ERRATA 8).
    for index, (out, coeffs, _lsb_bypass) in enumerate(sub.matrices):
        keep = s32((-1 << sub.quant[out]) & 0xFFFFFFFF) if out <= sub.max_chan else -1
        for i in range(size):
            row = rows[first + i]
            total = sum(row[c] * coeffs[c] for c in range(sub.max_matrix_chan + 1))
            for noise in range(NOISE_CHANNELS):  # the noise channels follow the matrix channels
                total += row[16 + noise] * coeffs[sub.max_matrix_chan + 1 + noise]
            row[out] = s32(((total >> MATRIX_SHIFT) & keep) + bypass[i][index])
    # The output: ch_assign picks the matrix channel, the output shift is applied (ERRATA 12).
    output_rows = []
    for i in range(size):
        out_row = []
        for ch in range(sub.max_matrix_chan + 1):
            value = rows[first + i][sub.ch_assign[ch]]
            shift = sub.output_shift[ch]
            out_row.append(s32(value << shift) if shift >= 0 else value >> -shift)
        output_rows.append(out_row)
        if sub.interval is None:
            sub.interval = 0
        for ch, value in enumerate(out_row):
            # The shift is the matrix channel the output channel takes (ERRATA 12, provisional).
            sub.interval ^= (value & 0xFFFFFF) << sub.ch_assign[ch]
            sub.interval &= 0xFFFFFFFF
    return output_rows


def decode_segment(sub, entry, segment, rows):
    payload = segment[:-2] if entry["crc_present"] else segment
    if entry["crc_present"]:
        if parity_substream(payload) != segment[-2]:
            raise StreamError("substream parity")
        if crc8_substream(payload) != segment[-1]:
            raise StreamError("substream CRC")
    bits = Bits(payload)
    out = []
    first = 0
    while True:
        if bits.read(1):
            if bits.read(1):
                sub.restart(bits, payload)
            if not sub.running:
                raise StreamError("no restart header")
            sub.block_header(bits)
        elif not sub.running:
            raise StreamError("no restart header")
        while len(rows) < first + sub.block_size:
            rows.append([0] * (16 + NOISE_CHANNELS))
        out += decode_block(sub, bits, rows, first)
        first += sub.block_size
        if bits.read(1):
            break
    padding = 0
    if sub.index == 3 and bits.left() >= 16 and bits.bits(bits.pos, 16) == 0x1234:
        bits.pos += 16  # terminatorB (TH spec 16.5): no stream of FFmpeg's has it
    elif bits.left() >= 32 and bits.bits(len(payload) * 8 - 32, 20) == END_MARKER:
        padding = bits.bits(len(payload) * 8 - 12, 12)
    return out, padding


def access_units(data):
    """Yield each access unit's bytes; its first two bytes give its length in words (3.3)."""
    offset = 0
    while offset + 4 <= len(data):
        length = (int.from_bytes(data[offset : offset + 2], "big") & 0xFFF) * 2
        if length < 4 or offset + length > len(data):
            return
        yield data[offset : offset + length]
        offset += length


def nibble_fold(data):
    folded = 0
    for byte in data:
        folded ^= byte
    return ((folded >> 4) ^ folded) & 0xF


def decode_stream(data, presentation="best"):
    """Decode a whole stream. Returns (rows of interleaved samples, sample rate, channel count)."""
    substreams = [Substream(i) for i in range(4)]
    count = 0
    rate = 0
    pcm = []
    for unit in access_units(data):
        offset = 4
        if unit[offset : offset + 3] == SYNC_WORD:
            stored = int.from_bytes(unit[offset + 26 : offset + 28], "big")
            if crc16_major_sync(unit[offset : offset + 26]) != stored:
                raise StreamError("major sync CRC")
            format_info = int.from_bytes(unit[offset + 4 : offset + 8], "big")
            ratebits = format_info >> 28 if unit[offset + 3] == 0xBA else (format_info >> 20) & 0xF
            rate = (48000 << ratebits) if ratebits <= 2 else (44100 << (ratebits - 8))
            count = unit[offset + 16] >> 4
            offset += 28
        directory_start = offset
        entries = []
        for _ in range(count):
            word = int.from_bytes(unit[offset : offset + 2], "big")
            entry = {"extra": word >> 15, "crc_present": (word >> 13) & 1, "end": word & 0xFFF}
            offset += 2
            if entry["extra"]:
                offset += 2
            entries.append(entry)
        if nibble_fold(unit[:4]) ^ nibble_fold(unit[directory_start:offset]) != 0xF:
            raise StreamError("check nibble")
        rows = []
        previous = 0
        out = padding = None
        wanted = {"best": min(count, 3), "2ch": 1, "6ch": 2, "8ch": 3, "all": count}[presentation]
        for index, entry in enumerate(entries[:wanted]):
            segment = unit[offset + previous * 2 : offset + entry["end"] * 2]
            previous = entry["end"]
            out, padding = decode_segment(substreams[index], entry, segment, rows)
        pcm += out[: len(out) - padding]
    return pcm, rate, len(pcm[0]) if pcm else 0


def to_s32le(rows):
    flat = [value << 8 for row in rows for value in row]
    return struct.pack(f"<{len(flat)}i", *flat)


def check(manifest_path):
    manifest = json.loads(pathlib.Path(manifest_path).read_text())
    failures = 0
    for fixture in manifest["fixtures"]:
        data = (pathlib.Path(manifest_path).parent / fixture["file"]).read_bytes()
        rows, rate, channels = decode_stream(data)
        digest = hashlib.sha256(to_s32le(rows)).hexdigest()
        good = digest == fixture["pcm_sha256"] and rate == fixture["sample_rate"]
        failures += not good
        print(f"{'ok  ' if good else 'FAIL'} {fixture['name']}: {len(rows)} samples, {channels} ch")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    decode = sub.add_parser("decode")
    decode.add_argument("stream", type=pathlib.Path)
    decode.add_argument("output", type=pathlib.Path)
    verify = sub.add_parser("check")
    verify.add_argument(
        "manifest", nargs="?", type=pathlib.Path, default=FIXTURES / "manifest.json"
    )
    args = parser.parse_args()
    if args.command == "decode":
        rows, _rate, _channels = decode_stream(args.stream.read_bytes())
        args.output.write_bytes(to_s32le(rows))
        return 0
    return check(args.manifest)


if __name__ == "__main__":
    sys.exit(main())
