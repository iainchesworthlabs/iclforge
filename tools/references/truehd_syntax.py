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
TERMINATOR_A = (0xD234E, 0x348D3)  # TH spec 16.5.1: both are accepted
NOISE_CHANNELS = 2
MATRIX_SHIFT = 15  # coefficients are scaled to 2^-15 (ERRATA 60); frac_bits runs to 15

# TH spec 17.4: generator 1's noise table, indexed by the seed's bits 15 to 22.
NOISE_TABLE = (
    30, 51, 22, 54, 3, 7, -4, 38, 14, 55, 46, 81, 22, 58, -3, 2,
    52, 31, -7, 51, 15, 44, 74, 30, 85, -17, 10, 33, 18, 80, 28, 62,
    10, 32, 23, 69, 72, 26, 35, 17, 73, 60, 8, 56, 2, 6, -2, -5,
    51, 4, 11, 50, 66, 76, 21, 44, 33, 47, 1, 26, 64, 48, 57, 40,
    38, 16, -10, -28, 92, 22, -18, 29, -10, 5, -13, 49, 19, 24, 70, 34,
    61, 48, 30, 14, -6, 25, 58, 33, 42, 60, 67, 17, 54, 17, 22, 30,
    67, 44, -9, 50, -11, 43, 40, 32, 59, 82, 13, 49, -14, 55, 60, 36,
    48, 49, 31, 47, 15, 12, 4, 65, 1, 23, 29, 39, 45, -2, 84, 69,
    0, 72, 37, 57, 27, 41, -15, -16, 35, 31, 14, 61, 24, 0, 27, 24,
    16, 41, 55, 34, 53, 9, 56, 12, 25, 29, 53, 5, 20, -20, -8, 20,
    13, 28, -3, 78, 38, 16, 11, 62, 46, 29, 21, 24, 46, 65, 43, -23,
    89, 18, 74, 21, 38, -12, 19, 12, -19, 8, 15, 33, 4, 57, 9, -8,
    36, 35, 26, 28, 7, 83, 63, 79, 75, 11, 3, 87, 37, 47, 34, 40,
    39, 19, 20, 42, 27, 34, 39, 77, 13, 42, 59, 64, 45, -1, 32, 37,
    45, -5, 53, -6, 7, 36, 50, 23, 6, 32, 9, -21, 18, 71, 27, 52,
    -25, 31, 35, 42, -1, 68, 63, 52, 26, 43, 66, 37, 41, 25, 40, 70,
)  # fmt: skip

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
            flt.state_shift = 0
            if bits.read(1):  # ERRATA 9: the FIR filter has the flag too
                state_bits = bits.read(4)
                state_shift = bits.read(4)
                flt.state_shift = state_shift
                state = [
                    bits.signed(state_bits) << state_shift if state_bits else 0
                    for _ in range(order)
                ]
            flt.state = state
        else:
            flt.state = None
            flt.state_shift = 0
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
        self.object_matrices = False
        self.object_set = []
        self.fine = []
        self.interval = None  # the fold of the output since the last restart
        self.noise_buffer = None  # generator 1's values for this access unit (TH spec 17.3)
        self.noise_size = 64  # access_unit_size_pow2: 64, 128 or 256 (ERRATA, noise generator 1)

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
        # TH spec 7.4: ch_assign maps matrix channels to output channels, so output channel p takes
        # the matrix channel m with ch_assign[m] == p (ERRATA 42). A value that repeats leaves an
        # output channel with no matrix channel, which is refused (ERRATA question 41).
        if sorted(self.ch_assign) != list(range(self.max_matrix_chan + 1)):
            raise StreamError("ch_assign is not a permutation of the matrix channels")
        self.output_source = [self.ch_assign.index(p) for p in range(self.max_matrix_chan + 1)]
        crc_bit = bits.pos
        stored = bits.read(8)
        if crc8_restart(segment, start, crc_bit) != stored:
            raise StreamError("restart header CRC")
        # Restart sync 0x31EB is noise_type 1: generator 1 (TH spec 17.3) and matrices with no noise
        # coefficients but a noise shift each (ERRATA 32).
        # Restart sync 0x31EC draws generator 1's noise too, its matrices' `field_m4` the noise
        # shift (ERRATA 69).
        self.noise_type = sync in (0x31EB, 0x31EC)
        # Restart sync 0x31EC has the object-audio matrix section (TH spec 10.2.1a).
        self.object_matrices = sync == 0x31EC
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
        self.object_set = []  # the last object matrix set, headers, widths and deltas (ERRATA 69)
        self.fine = []
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

    def read_object_matrices(self, bits):
        """TH spec 10.2.1a (ERRATA 69). A: the matrices change (0: the last set stands, with its
        deltas); B: new headers follow (0: the last set's headers stand and only coefficients are
        sent); with B the count less one and the header of every matrix (output channel, frac_bits,
        f_shift + 1, 2 bits of LSB bypass, a 4 bit noise shift, a presence mask), then every
        matrix's coefficients. Then the extension: A2, B2, C2 (each inside the one before); with
        C2 a width (4 bits) and 2 bits nothing reads for each matrix, else the last widths stand;
        then a signed delta of width + 1 bits for each coefficient of each matrix whose width is
        not 0. New headers keep the width of the matrix at the same place in the last set; new
        coefficients clear the deltas; a restart clears the set.

        The coefficient is base + delta / 2^(width + 1), in units of the base's last place. The
        reader leaves the base coefficients in the matrix tuples and the deltas, scaled as the base
        is (to 2^-15), with their bit count in `self.fine`, which decode_block applies."""
        channels = self.max_matrix_chan + 1
        previous = self.object_set
        if bits.read(1):
            if bits.read(1):
                count = bits.read(4) + 1
                current = []
                for place in range(count):
                    matrix = {
                        "out": bits.read(4),
                        "frac_bits": bits.read(4),
                        "f_shift": bits.read(3) - 1,
                        "bypass": bits.read(
                            2
                        ),  # the bits a sample of the LSB bypass (ERRATA 69, provisional)
                        "noise_shift": bits.read(4),
                        "mask": bits.read(channels),
                        "width": previous[place]["width"] if place < len(previous) else 0,
                    }
                    if matrix["frac_bits"] - matrix["f_shift"] > MATRIX_SHIFT:
                        raise StreamError("matrix coefficients finer than 2^-15")
                    current.append(matrix)
            else:
                if not previous:
                    raise StreamError("object matrices reuse headers none were sent for")
                current = [dict(matrix) for matrix in previous]
            for matrix in current:
                matrix["base"] = {}
                matrix["delta"] = {}
                for channel in range(channels):
                    if matrix["mask"] >> channel & 1:
                        matrix["base"][channel] = bits.signed(matrix["frac_bits"] + 2)
        else:
            current = [dict(matrix) for matrix in previous]
        if bits.read(1) and bits.read(1):
            if bits.read(1):
                for matrix in current:
                    matrix["width"] = bits.read(4)
                    bits.read(2)
            for matrix in current:
                if matrix["width"]:
                    for channel in range(channels):
                        if matrix["mask"] >> channel & 1:
                            matrix["delta"][channel] = bits.signed(matrix["width"] + 1)
        self.object_set = current
        self.fine = []
        matrices = []
        for matrix in current:
            scale = MATRIX_SHIFT - matrix["frac_bits"] + matrix["f_shift"]
            coeffs = [0] * (channels + NOISE_CHANNELS)
            deltas = [0] * (channels + NOISE_CHANNELS)
            for channel, raw in matrix["base"].items():
                coeffs[channel] = raw << scale
            for channel, delta in matrix["delta"].items():
                deltas[channel] = delta << scale
            matrices.append((matrix["out"], coeffs, matrix["bypass"], matrix["noise_shift"]))
            self.fine.append((deltas, matrix["width"] + 1 if matrix["width"] else 0))
        return matrices

    def read_matrices(self, bits):
        if self.object_matrices:
            return self.read_object_matrices(bits)
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
            noise_shift = 0
            if self.noise_type:
                coeffs.extend([0] * NOISE_CHANNELS)
                noise_shift = bits.read(4)
            matrices.append((out, coeffs, lsb_bypass, noise_shift))
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
        bits_for_sample = [bits.read(matrix[2]) if matrix[2] else 0 for matrix in sub.matrices]
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
    if sub.noise_type:
        # Generator 1 (TH spec 17.3): a buffer filled once for the access unit, from the seed the
        # restart header loaded and the units before this one have run on.
        if sub.noise_buffer is None:
            sub.noise_buffer = []
            seed = sub.noise_seed
            for _ in range(sub.noise_size):
                table_index = (seed >> 15) & 0xFF
                sub.noise_buffer.append(NOISE_TABLE[table_index])
                seed = ((seed << 8) ^ table_index ^ (table_index << 5)) & 0xFFFFFFFF
            sub.noise_seed = seed
    else:
        # Generator 0 (TH spec 17.2, ERRATA 23): two noise channels, one step of the LFSR a sample.
        seed = sub.noise_seed
        for i in range(size):
            shr7 = (seed >> 7) & 0xFFFF  # 16 bits, as FFmpeg reads it (ERRATA 23, provisional)
            rows[first + i][16] = int8(seed >> 15) * (1 << sub.noise_shift)
            rows[first + i][17] = int8(shr7) * (1 << sub.noise_shift)
            seed = ((seed << 16) ^ shr7 ^ (shr7 << 5)) & 0xFFFFFFFF
        sub.noise_seed = seed
    # The primitive matrices (ERRATA 8).
    for index, (out, coeffs, _lsb_bypass, noise_shift) in enumerate(sub.matrices):
        keep = s32((-1 << sub.quant[out]) & 0xFFFFFFFF) if out <= sub.max_chan else -1
        # Generator 1's noise (TH spec 17.3): the matrix starts the unit at the index of the
        # matrices left (this one included) and steps by twice that, plus one, a sample.
        left = len(sub.matrices) - index
        deltas, fine_bits = sub.fine[index] if index < len(sub.fine) else (None, 0)
        for i in range(size):
            row = rows[first + i]
            total = sum(row[c] * coeffs[c] for c in range(sub.max_matrix_chan + 1))
            for noise in range(NOISE_CHANNELS):  # the noise channels follow the matrix channels
                total += row[16 + noise] * coeffs[sub.max_matrix_chan + 1 + noise]
            if noise_shift:
                slot = (left + (first + i) * (2 * left + 1)) % sub.noise_size
                total += sub.noise_buffer[slot] * (1 << (noise_shift + 7 + MATRIX_SHIFT - 14))
            shift = MATRIX_SHIFT
            if fine_bits:
                # The extension (ERRATA 69): base + delta / 2^fine_bits for each coefficient, so
                # the sum of the base coefficients and the noise is lifted by fine_bits places and
                # the products with the deltas, which are in the same units, are added.
                total = (total << fine_bits) + sum(
                    row[c] * deltas[c] for c in range(sub.max_matrix_chan + 1)
                )
                shift += fine_bits
            row[out] = s32(((total >> shift) & keep) + bypass[i][index])
    # The output: the matrix channel ch_assign assigns to the output channel (ERRATA 42), then the
    # output shift of that matrix channel (TH spec 7.3, ERRATA 12): the shifts are listed by matrix
    # channel.
    output_rows = []
    for i in range(size):
        out_row = []
        for ch in range(sub.max_matrix_chan + 1):
            value = rows[first + i][sub.output_source[ch]]
            shift = sub.output_shift[sub.output_source[ch]]  # by matrix channel (ERRATA 43)
            out_row.append(s32(value << shift) if shift >= 0 else value >> -shift)
        output_rows.append(out_row)
        if sub.interval is None:
            sub.interval = 0
        for ch, value in enumerate(out_row):
            # The shift is the matrix channel the output channel takes (ERRATA 12, provisional).
            sub.interval ^= (value & 0xFFFFFF) << sub.output_source[ch]
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
    sub.noise_buffer = None
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
    # What may follow the last block (TH spec 3.5, 16.5): terminatorB after substream 3, right
    # after the block or ending the segment; terminatorA and the 12 bit count of padding samples
    # ending the segment (ERRATA 46).
    padding = 0
    end = len(payload) * 8
    if sub.index == 3 and bits.left() >= 16 and bits.bits(bits.pos, 16) == 0x1234:
        bits.pos += 16
    elif sub.index == 3 and bits.left() >= 16 and bits.bits(end - 16, 16) == 0x1234:
        bits.pos = end
    elif bits.left() >= 32 and bits.bits(end - 32, 20) in TERMINATOR_A:
        padding = bits.bits(end - 12, 12)
    return out, padding


AC3_WORDS_48K = (64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896)
AC3_WORDS_48K += (1024, 1152, 1280)


def ac3_core_frame_bytes(data, offset):
    """The length of the AC-3 frame at `offset`, or 0: a Blu-ray track carries the programme's AC-3
    core (48 kHz, bit stream id 8 or less) between its access units."""
    if data[offset : offset + 2] != b"\x0b\x77" or offset + 6 > len(data):
        return 0
    fscod, size_code = data[offset + 4] >> 6, data[offset + 4] & 0x3F
    if fscod != 0 or size_code >= 2 * len(AC3_WORDS_48K) or data[offset + 5] >> 3 > 8:
        return 0
    size = AC3_WORDS_48K[size_code // 2] * 2
    return size if offset + size <= len(data) else 0


def access_units(data):
    """Yield each access unit's bytes; its first two bytes give its length in words (3.3). AC-3
    core frames between the units are passed over."""
    offset = 0
    while offset + 4 <= len(data):
        core = ac3_core_frame_bytes(data, offset)
        if core:
            offset += core
            continue
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


def major_sync_size(unit, offset):
    """The major sync's length in bytes: 28, or more with the 16-channel extension of TH spec 14.2
    (32 on the owner's discs). The extension's size is found by the CRC, which ends it, as the
    library does (ERRATA 44): the sizes the extension's 4 bit length gives first, then every
    even size."""
    length_words = (unit[offset + 26] >> 4) + 1
    for size in (28, 26 + 2 * length_words + 2, 26 + 2 * (length_words + 1) + 2, *range(28, 62, 2)):
        if offset + size > len(unit):
            continue
        stored = int.from_bytes(unit[offset + size - 2 : offset + size], "big")
        if crc16_major_sync(unit[offset : offset + size - 2]) == stored:
            return size
    raise StreamError("major sync CRC")


def decode_stream(data, presentation="best"):
    """Decode a whole stream. Returns (rows of interleaved samples, sample rate, channel count)."""
    substreams = [Substream(i) for i in range(4)]
    count = 0
    rate = 0
    pcm = []
    for unit in access_units(data):
        offset = 4
        if unit[offset : offset + 3] == SYNC_WORD:
            size = major_sync_size(unit, offset)
            format_info = int.from_bytes(unit[offset + 4 : offset + 8], "big")
            ratebits = format_info >> 28 if unit[offset + 3] == 0xBA else (format_info >> 20) & 0xF
            rate = (48000 << ratebits) if ratebits <= 2 else (44100 << (ratebits - 8))
            for substream in substreams:
                substream.noise_size = 64 << (ratebits & 3)
            count = unit[offset + 16] >> 4
            offset += size
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
