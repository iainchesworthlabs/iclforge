"""Independent Python reference of the AC-4 speech spectral frontend (SSF).

A transcription of ETSI TS 103 190-1 V1.4.1 (2025-07) clause 4.2.9 (Tables
43-46, ssf_data() and its three sub-elements), clause 4.3.7 (semantics, Tables
111-113, Pseudocode 7) and clause 5.2 (decoding, Pseudocodes 4a-58), with the
tables of Annex C. It decodes ssf_data() into the spectral lines the inverse
MDCT of clause 5.5 consumes, keeping every piece of decoder state across
granules and frames. Standard library only: it is an oracle for the C++
decoder, written from the same text and sharing none of its code.

Tables. The numeric tables of Annex C (POST_GAIN_LUT ... OFFSETS_LIN_TO_DB, the
37 ssf_pred_coeff_mat arrays) are the standard's own C file, parsed at import
time; its location is --spec-dir, else the environment variable SSF_TABLES_C,
else spec/ts_10319001_attach in the repository root. Table C.1 (SSF bandwidths) exists only in the
text and is transcribed here (BAND_WIDTHS), checked against the page image.

Usage, from the repo root:
    python tools/references/ssf_ref.py selftest
    python tools/references/ssf_ref.py vectors --seed 1 --cases 16 --frames 6 --out ssf.vec

READINGS. The text is defective in several places; the decisions taken are
numbered here and cited at the line that implements them.

  R1   Coefficient symbols are signed: the Pseudocode 44 search for the
       transform coefficients runs from -i_max_idx to +i_max_idx ascending,
       taking the first symbol whose [CdfLow, CdfHigh) holds uiTarget; none
       holds it -> SsfError("no symbol"). Envelope / predictor gain indices:
       symbols 0..31 only (the tables hold 33 entries).
  R2   HeuristicScaling's iRfu = floor(f_rfu * 1024 + 0.5).
  R3   `band` is reset to 0 before the reverse water-filling loop.
  R4   `x = x++` in Pseudocodes 56/57 is x = x + 1 (mod 256); the noise sum
       is formed in float32.
  R5   Quantized prediction coefficient layout: ((nu + rfs) * 33 + eta) * rts
       + k, not Pseudocode C.1's printed formula.
  R6   Pseudocode 36's sign handling is replaced by the displayed equation of
       clause 5.2.6: (-1)^((k+1)*p). round() is floor(x + 0.5) throughout.
  R7   f_pred_lag uses real division by 170.
  R8   Validity checks (env range, lag index range, stride vs frame length,
       first granule of a stream must be an I granule).
  R9   State reset at an SSF-I granule; dither drawn per granule up front;
       noise drawn in dequantisation order.
  R10  Predictor buffers are zero-padded / truncated when n_mdct changes.
  R11  The arithmetic decoder's look-ahead beyond the substream reads zeros;
       a granule's AC data occupies exactly the AcDecodeFinish count of bits.
  R12  Exact-integer parts use Python ints with the SSF_I32_* operators; a
       signed result outside int32 is SsfError("overflow").
  R13  Predictor gain / lag for blocks without a predictor.
  R14  SSF_HIGH_FREQ_GAIN_THRESHOLD = 2; bands 0 and 1 are not clamped.
  R15  Trace records, one per fixed-width field plus one per ssf_ac_data().

Beyond R1-R15 (each is a named constant or a commented branch below, and is
listed in the report that came with this file):

  E1   Shaper reads an empty envelope-buffer entry -> SsfError (EMPTY_ENV_IS_ERROR),
       now fixed by R16 as SsfError("lag before first block").
  R16  (coordinator) Pseudocode 51 clamps both ends of iLeft and iRight to
       +-i_max_value, so symbols wholly beyond +-10 have an empty interval
       instead of indexing CDF_TABLE out of range; AcDecodeFinish without a
       break is SsfError("termination"); f_pred_gain == 0 skips extractor and
       shaper; an unfilled envelope buffer entry with gain != 0 and
       integer_lag >= 4 are SsfError.
  E3   Output zeros are normalised to +0.0; a block with predictor gain 0
       skips the extractor (its product is zero either way).
  E4   FLOAT(x) in Pseudocode 27 is x / 1024.0 (Qx.10 to float).
  E5   Summation order of the extractor follows Pseudocode 36 (nu outer, k
       inner), not the displayed double sum's order.
  E6   Arithmetic-decoder state is uint32 and wraps (as the unsigned C types
       do) for streams that violate the encoder's offset < range invariant.
"""

from __future__ import annotations

import argparse
import copy
import itertools
import math
import os
import random
import re
import struct
import sys
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

# ---------------------------------------------------------------------------
# Where the normative C tables live (see the module docstring)
# ---------------------------------------------------------------------------

TABLES_C_NAME = "ts_103190_tables.c"
TABLES_ENV = "SSF_TABLES_C"
# The repository's spec/ directory, as tools/generators/gen_ac4_tables.py reads it.
DEFAULT_TABLES_C = (
    Path(__file__).resolve().parents[2] / "spec" / "ts_10319001_attach" / TABLES_C_NAME
)

# Table C.1: SSF bandwidths, number of bins per band 0..18, keyed by the SSF
# block length n_mdct (the eight columns of the table).
BAND_WIDTHS = {
    192: [2] * 10 + [3] * 3 + [4, 4, 5, 5, 5, 6],
    240: [3] * 10 + [4] * 3 + [5, 5, 6, 7, 7, 8],
    256: [3] * 10 + [4] * 3 + [5, 5, 6, 7, 7, 8],
    384: [5] * 10 + [6] * 3 + [8, 8, 9, 11, 11, 12],
    512: [6] * 10 + [8] * 3 + [10, 10, 12, 14, 14, 16],
    768: [9] * 10 + [12] * 3 + [15, 15, 18, 21, 21, 24],
    960: [11] * 10 + [15] * 3 + [19, 19, 23, 26, 26, 30],
    1024: [12] * 10 + [16] * 3 + [20, 20, 24, 28, 28, 32],
}
MAX_NUM_BANDS = 19  # Pseudocode 7

# Clause 4.3.7.2.2, Tables 112/113: frame_len_base values this reference takes.
# 384 and 512 only allow LONG_STRIDE ("maximum number of SSF blocks" is 1).
SUPPORTED_FRAME_LEN_BASE = (384, 512, 768, 960, 1024, 1536, 1920, 2048)
LONG_ONLY_FRAME_LEN_BASE = (384, 512)

# E1: Pseudocode 37 divides by f_env_buffer[integer_lag][band]; an entry that was
# never filled (buffers are zero after an SSF-I reset, R9) would give infinity.
EMPTY_ENV_IS_ERROR = True


class SsfError(Exception):
    """An invalid or truncated SSF stream (clause 5.2 and R8, R11, R12)."""


# ---------------------------------------------------------------------------
# Table loading (Annex C)
# ---------------------------------------------------------------------------


def _f32(x: float) -> float:
    """Round a double to the nearest float32 (the C `float` type)."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _f32_from_decimal(token: str) -> float:
    """A C float literal: the decimal rounded once, directly, to float32."""
    exact = Fraction(token)
    near = _f32(float(exact))
    bits = struct.unpack("<I", struct.pack("<f", near))[0]
    cands = [near]
    for d in (-1, 1):
        c = struct.unpack("<f", struct.pack("<I", (bits + d) & 0xFFFFFFFF))[0]
        if math.isfinite(c):
            cands.append(c)
    return min(cands, key=lambda c: abs(Fraction(c) - exact))


def _c_array_tokens(src: str, name: str) -> list[str]:
    m = re.search(
        r"\b" + re.escape(name) + r"\s*\[[^\]]*\]\s*(?:\[[^\]]*\])?\s*=\s*\{(.*?)\};", src, re.S
    )
    if m is None:
        raise ValueError(f"table {name} not found in the C file")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [t for t in re.split(r"[,\s]+", body) if t]


def _int_token(tok: str) -> int:
    t = tok.lower()
    if t.startswith(("0x", "-0x")):
        return int(t, 16)
    return int(t)


class Tables:
    """The Annex C tables, parsed from the standard's C file."""

    def __init__(self, path: Path) -> None:
        src = path.read_text(encoding="latin-1")

        def ints(name: str, length: int) -> list[int]:
            v = [_int_token(t) for t in _c_array_tokens(src, name)]
            if len(v) != length:
                raise ValueError(f"{name}: {len(v)} entries, expected {length}")
            return v

        def dbls(name: str, length: int) -> list[float]:
            v = [float(t.rstrip("fF")) for t in _c_array_tokens(src, name)]
            if len(v) != length:
                raise ValueError(f"{name}: {len(v)} entries, expected {length}")
            return v

        self.path = path
        # Table C.2, C.3: decimal literals converted to double (R12).
        self.post_gain_lut = dbls("POST_GAIN_LUT", 20)
        self.pred_gain_quant_tab = dbls("PRED_GAIN_QUANT_TAB", 32)
        self.pred_rfs = ints("PRED_RFS_TABLE", 37)  # Table C.4
        self.pred_rts = ints("PRED_RTS_TABLE", 37)  # Table C.5
        self.pred_coeff_mat = [  # clause C.6
            ints(f"ssf_pred_coeff_mat{i}", 33 * (2 * self.pred_rfs[i] + 1) * self.pred_rts[i])
            for i in range(37)
        ]
        self.cdf_table = ints("CDF_TABLE", 705)  # Table C.6
        self.predictor_gain_cdf = ints("PREDICTOR_GAIN_CDF_LUT", 33)  # Table C.7
        self.envelope_cdf = ints("ENVELOPE_CDF_LUT", 33)  # Table C.8
        self.dither_table = ints("DITHER_TABLE", 256)  # Table C.9
        # Table C.10: C `float` literals, so float32 values (R4).
        toks = _c_array_tokens(src, "RANDOM_NOISE_TABLE")
        if len(toks) != 256:
            raise ValueError("RANDOM_NOISE_TABLE: wrong length")
        self.random_noise_table = [_f32_from_decimal(t.rstrip("fF")) for t in toks]
        self.step_sizes = ints("STEP_SIZES_Q4_15", 21)  # Table C.11
        self.ac_coeff_max_index = ints("AC_COEFF_MAX_INDEX", 21)  # Table C.12
        self.slopes_db_to_lin = ints("SLOPES_DB_TO_LIN", 10)  # Table C.13
        self.offsets_db_to_lin = ints("OFFSETS_DB_TO_LIN", 10)  # Table C.14
        self.slopes_lin_to_db = ints("SLOPES_LIN_TO_DB", 50)  # Table C.15
        self.offsets_lin_to_db = ints("OFFSETS_LIN_TO_DB", 50)  # Table C.16
        self._c_all: dict[int, list] = {}

    def c_all(self, tab_idx: int) -> list:
        """C_ALL[tab_idx][nu + Rf][eta + 32][k], clause 5.2.8.1 / Pseudocode 39."""
        cached = self._c_all.get(tab_idx)
        if cached is not None:
            return cached
        rf, rt = self.pred_rfs[tab_idx], self.pred_rts[tab_idx]
        mat = self.pred_coeff_mat[tab_idx]
        c = [[[0.0] * rt for _ in range(65)] for _ in range(2 * rf + 1)]
        for nu in range(-rf, rf + 1):
            for eta in range(33):
                for k in range(rt):
                    # R5: the measured layout (eta smooth axis, nu slowest, k fastest).
                    q = mat[((nu + rf) * 33 + eta) * rt + k]
                    c[nu + rf][eta + 32][k] = 1.1787855 * (q - 146) / 128
        for k in range(rt):
            s = -1.0 if k % 2 == 0 else 1.0  # (-1)^(k+1), clause 5.2.8.1
            for eta in range(-32, 0):
                for nu in range(-rf, rf + 1):
                    c[nu + rf][eta + 32][k] = s * c[-nu + rf][-eta + 32][k]
        self._c_all[tab_idx] = c
        return c


def locate_tables_c(spec_dir: str | os.PathLike | None = None) -> Path:
    """Resolve the C file: --spec-dir, then $SSF_TABLES_C, then spec/ in the repository root."""
    cands: list[Path] = []
    if spec_dir:
        cands.append(Path(spec_dir))
    elif os.environ.get(TABLES_ENV):
        cands.append(Path(os.environ[TABLES_ENV]))
    else:
        cands.append(DEFAULT_TABLES_C)
    for c in cands:
        if c.is_file():
            return c
        if c.is_dir():
            direct = c / TABLES_C_NAME
            if direct.is_file():
                return direct
            found = sorted(c.rglob(TABLES_C_NAME))
            if found:
                return found[0]
    raise FileNotFoundError(
        f"cannot find {TABLES_C_NAME}: pass --spec-dir or set {TABLES_ENV} (tried {cands[0]})"
    )


# Module state in a dict (not globals): the tables, or why they could not be loaded yet.
_STATE: dict[str, object] = {"tables": None, "error": None}


def load_tables(spec_dir: str | os.PathLike | None = None) -> Tables:
    """(Re)load the tables; returns and installs them as the module's tables."""
    loaded = Tables(locate_tables_c(spec_dir))
    _STATE["tables"] = loaded
    _STATE["error"] = None
    _COEF_CACHE.clear()
    return loaded


def tables() -> Tables:
    t = _STATE["tables"]
    if t is None:
        raise RuntimeError(f"SSF tables not loaded: {_STATE['error']}")
    return t  # type: ignore[return-value]


# ---------------------------------------------------------------------------
# Fixed point operators, Pseudocode 40 (R12)
# ---------------------------------------------------------------------------

INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1
M32 = 0xFFFFFFFF


def _i32(v: int) -> int:
    """A signed 32-bit result; leaving int32 is a stream outside the design range (R12)."""
    if v < INT32_MIN or v > INT32_MAX:
        raise SsfError("overflow")
    return v


def i32_add(a: int, b: int) -> int:  # SSF_I32_ADD_I32
    return _i32(a + b)


def i32_sub(a: int, b: int) -> int:  # SSF_I32_SUB_I32
    return _i32(a - b)


def i16_mul(a: int, b: int) -> int:  # SSF_I16_MUL_I16: a 32-bit signed product
    return _i32(a * b)


def i32_shl(a: int, b: int) -> int:  # SSF_I32_SHIFT_LEFT, sign preserving
    return _i32((a << b) if a >= 0 else -((-a) << b))


def i32_shr(a: int, b: int) -> int:  # SSF_I32_SHIFT_RIGHT, sign preserving (towards zero)
    return (a >> b) if a >= 0 else -((-a) >> b)


def c_div(a: int, b: int) -> int:
    """C integer division: truncates towards zero. b == 0 is undefined in C."""
    if b == 0:
        raise SsfError("division by zero")
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def c_round(x: float) -> int:
    """R6: round() is floor(x + 1/2) everywhere (differs from C only on negative .5)."""
    return math.floor(x + 0.5)


# ---------------------------------------------------------------------------
# dB <-> linear maps, Pseudocodes 29 and 30
# ---------------------------------------------------------------------------


def map_db_to_lin(i_input: int) -> int:
    """Pseudocode 29: Qx.10 dB to Qx.10 amplitude, piecewise linear (approximation)."""
    t = tables()
    i_input = i32_shr(i_input, 6)  # Qx.4
    i_index = i32_shr(i_input, 6)
    if i_index < 0:
        raise SsfError("map_db_to_lin index")
    if i_index < 10:
        res = i16_mul(t.slopes_db_to_lin[i_index], i_input)
        res = i32_shr(res, 4)
        res = i32_add(res, t.offsets_db_to_lin[i_index])
        return i32_shl(res, 6)
    return i32_shl(100, 10)


def map_lin_to_db(i_input: int) -> int:
    """Pseudocode 30: Qx.10 amplitude to Q6.10 dB, piecewise linear (approximation)."""
    t = tables()
    i_input = i32_shr(i_input, 2)  # Qx.8
    i_quant_in = i32_shr(i_input, 1)
    i_index = i32_shr(i_quant_in, 8)
    i_int = i32_shl(i_index, 8 + 1)
    i_fract = i32_sub(i_input, i_int)
    if i_index < 0:
        raise SsfError("map_lin_to_db index")
    if i_index < 50:  # iMaxTableSize
        tmp2 = i32_shl(i_index, 1)
        tmp1 = i16_mul(t.slopes_lin_to_db[i_index], tmp2)
        tmp2 = i16_mul(t.slopes_lin_to_db[i_index], i_fract)
        tmp2 = i32_shr(tmp2, 8)
        res = i32_add(tmp1, tmp2)
        res = i32_add(res, t.offsets_lin_to_db[i_index])
        return i32_shl(res, 2)
    return i32_shl(40, 10)


def heuristic_scaling(
    i_rfu: int, env_in: list[int], band_widths: list[int], num_bins: int
) -> list[int]:
    """Pseudocode 28. i_rfu in Q.10 (R2), env_in in Q.0 (1 dB); returns Q10.10 weights."""
    n = len(env_in)
    i_dyn_threshold = i32_shl(40, 10)
    i_max_iw_db = i32_shl(15, 10)
    i_inv_three = 341
    i_max_env = max(env_in)
    i_min_env = min(env_in)
    i_dyn_unscaled = i_max_env - i_min_env
    i_dyn = i32_shl(i_max_env - i_min_env, 10)
    if i_dyn > i_dyn_threshold:
        i_cmp_fact = c_div(i_dyn_threshold, i_dyn_unscaled)
        env_local = [i16_mul(i32_sub(e, i_min_env), i_cmp_fact) for e in env_in]
    else:
        env_local = [i32_shl(i32_sub(e, i_min_env), 10) for e in env_in]
    # Sort in descending order. Ties keep band order; the order inside a tie group does not
    # reach the output (a tie group that reaches the last band never adds to iMnt, and
    # i_bsum is then replaced by num_bins).
    env_indices = sorted(range(n), key=lambda b: -env_local[b])
    env_sorted = [env_local[b] for b in env_indices]
    i_mtr = 0
    weights_lin = []
    for b in range(n):
        weights_lin.append(map_db_to_lin(env_sorted[b]))
        i_tmp = i16_mul(weights_lin[b], band_widths[env_indices[b]])
        i_mtr = i32_add(i_mtr, i_tmp)
    i_mtr = i32_shr(i_mtr, 10)
    i_mtr = i16_mul(i_mtr, i_rfu)
    i_mtr = i32_shr(i_mtr, 7)
    i_mtr = i16_mul(i_mtr, i_rfu)
    i_mtr = i32_shr(i_mtr, 3)
    # reverse water-filling
    band = 0  # R3: the text never resets it
    i_mnt = 0
    i_bsum = 0
    while i_mnt < i_mtr and band < n - 1:
        i_curr_lev = weights_lin[band]
        while weights_lin[band] == i_curr_lev and band < n - 1:
            i_bsum = i32_add(i_bsum, band_widths[env_indices[band]])
            band += 1
        i_tmp2 = i32_sub(i_curr_lev, weights_lin[band])
        i_tmp2 = i16_mul(i_tmp2, i_bsum)
        i_mnt = i32_add(i_tmp2, i_mnt)
    if i_mnt < i_mtr:
        i_bsum = num_bins
    i_tmp = i32_sub(i_mnt, i_mtr)
    i_tmp = i32_shl(i_tmp, 4)
    i_tmp2 = c_div(i_tmp, i_bsum)
    i_tmp2 = i32_shr(i_tmp2, 4)
    i_curr_lev = i32_add(weights_lin[band], i_tmp2)
    level_db = map_lin_to_db(i_curr_lev)
    out = []
    for b in range(n):
        i_tmp = i32_sub(env_local[b], level_db)
        i_tmp = i16_mul(i_tmp, i_inv_three)
        i_tmp = i32_shr(i_tmp, 10)
        i_tmp = i_tmp if i_tmp > 0 else 0
        i_tmp = i_tmp if i_tmp < i_max_iw_db else i_max_iw_db
        out.append(i_tmp)
    return out


# ---------------------------------------------------------------------------
# Arithmetic decoder, Pseudocodes 41-47 (uint32 state, E6)
# ---------------------------------------------------------------------------

SSF_MODEL_BITS = 15
SSF_MODEL_UNIT = 1 << SSF_MODEL_BITS
SSF_RANGE_BITS = 30
SSF_THRESHOLD_LARGE = 1 << (SSF_RANGE_BITS - 1)
SSF_THRESHOLD_SMALL = 1 << (SSF_RANGE_BITS - 2)
SSF_OFFSET_BITS = 14
COEF_MAX_VALUE = 327680  # Pseudocode 51, i_max_value


class BitReader:
    """MSB-first reader over data[start_bit:end_bit] (bit 0 = MSB of data[0])."""

    def __init__(self, data: bytes, start_bit: int, end_bit: int) -> None:
        self.data = data
        self.pos = start_bit
        self.end = min(end_bit, 8 * len(data))

    def read(self, n: int) -> int:
        """Fixed-width field; reading past the end is SsfError("truncated")."""
        if self.pos + n > self.end:
            raise SsfError("truncated")
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def bit_lookahead(self) -> int:
        """One bit for the arithmetic decoder: zero beyond the end (R11); pos still moves."""
        p = self.pos
        self.pos = p + 1
        if p >= self.end:
            return 0
        return (self.data[p >> 3] >> (7 - (p & 7))) & 1

    def bits_value(self, start: int, count: int) -> int:
        v = 0
        for p in range(start, start + count):
            b = 0
            if p < self.end:
                b = (self.data[p >> 3] >> (7 - (p & 7))) & 1
            v = (v << 1) | b
        return v


class AcDecoder:
    """One arithmetic decoder instance, per SSF granule (clause 5.2.8.2)."""

    def __init__(self, rd: BitReader) -> None:
        # Pseudocode 43: AcDecoderInit
        self.rd = rd
        self.start = rd.pos
        self.low = 0
        self.range = SSF_THRESHOLD_LARGE
        off = rd.bit_lookahead()
        for _ in range(1, SSF_RANGE_BITS):
            off = ((off << 1) + rd.bit_lookahead()) & M32
        self.offset = off
        self.offset2 = off
        # A range coder's offset is always below its range. Random bits break that (the first
        # bit alone does, half the time); the decoder then runs on, uint32 arithmetic wrapping
        # (E6). `wellformed` records whether it ever happened, for the vector generator.
        self.wellformed = off < self.range

    def decode_target(self) -> int:
        """Pseudocode 45: AcDecodeTarget, Q0.15."""
        ui_range = self.range >> SSF_MODEL_BITS
        num_shifts = SSF_MODEL_BITS if ui_range < (1 << SSF_OFFSET_BITS) else SSF_MODEL_BITS - 1
        num = self.offset
        den = (ui_range << num_shifts) & M32
        target = 0
        for _ in range(num_shifts):
            if num >= den:
                num = (num - den) & M32
                target = (target + 1) & M32
            num = (num << 1) & M32
            target = (target << 1) & M32
        if num >= den:
            num = (num - den) & M32
            target = (target + 1) & M32
        if target >= SSF_MODEL_UNIT:
            target = SSF_MODEL_UNIT - 1
        return target

    def advance(self, cdf_low: int, cdf_high: int) -> None:
        """Pseudocode 46: AcDecode."""
        if self.offset >= self.range:
            self.wellformed = False
        ui_range = self.range >> SSF_MODEL_BITS
        tmp1 = (ui_range * cdf_low) & M32
        self.offset = (self.offset - tmp1) & M32
        if cdf_high < SSF_MODEL_UNIT:
            self.range = (ui_range * ((cdf_high - cdf_low) & M32)) & M32
        else:
            self.range = (self.range - tmp1) & M32
        if self.range == 0:
            raise SsfError("range collapsed")
        while self.range <= SSF_THRESHOLD_SMALL:  # denormalize
            b = self.rd.bit_lookahead()
            self.range = (self.range << 1) & M32
            self.offset = ((self.offset << 1) + b) & M32
            self.offset2 = (self.offset2 << 1) & M32
            if self.offset & 1:
                self.offset2 = (self.offset2 + 1) & M32

    def decode_table_symbol(self, cdf: list[int]) -> int:
        """Pseudocode 44 with a CDF table: symbols 0..31, symbol s is [cdf[s], cdf[s+1]) (R1)."""
        target = self.decode_target()
        for s in range(32):
            lo, hi = cdf[s], cdf[s + 1]
            if lo <= target < hi:
                self.advance(lo, hi)
                return s
        raise SsfError("no symbol")

    def decode_coef_symbol(self, intervals: list[tuple[int, int, int]]) -> int:
        """Pseudocode 44 for transform coefficients: first symbol (ascending) holding the target."""
        target = self.decode_target()
        for sym, lo, hi in intervals:
            if lo <= target < hi:
                self.advance(lo, hi)
                return sym
        raise SsfError("no symbol")

    def finish(self) -> int:
        """Pseudocode 47: AcDecodeFinish; bits the granule's AC data occupies (R11)."""
        res = (self.rd.pos - self.start) - SSF_RANGE_BITS
        low = self.offset2 & (SSF_THRESHOLD_LARGE - 1)
        low = (low + ((SSF_THRESHOLD_LARGE - self.offset) & M32)) & M32
        bit_idx = 1
        while bit_idx <= SSF_RANGE_BITS:
            rev_idx = SSF_RANGE_BITS - bit_idx
            up_fact = ((1 << rev_idx) - 1) & M32
            bits = ((low + up_fact) & M32) >> rev_idx
            val = (bits << rev_idx) & M32
            tmp1 = (val + up_fact) & M32
            tmp2 = (((self.range - 1) & M32) + low) & M32
            if low <= val and tmp1 <= tmp2:
                break
            bit_idx += 1
        if bit_idx > SSF_RANGE_BITS:
            raise SsfError("termination")  # R16: the C loop would leave iBitIdx at 31
        return res + bit_idx


# ---------------------------------------------------------------------------
# Coefficient CDF, Pseudocodes 51-53
# ---------------------------------------------------------------------------


def idx2reconstruction(i_index: int, i_dither: int, i_step: int) -> int:
    """Pseudocode 52."""
    tmp1 = i32_shl(i_index, 15)
    recon = i32_sub(tmp1, i_dither)
    tmp2 = i32_shr(recon, 15)
    tmp1 = i32_shl(tmp2, 15)
    tmp1 = i32_sub(recon, tmp1)
    tmp1 = i32_shr(tmp1, 3)
    recon = i16_mul(tmp1, i_step)
    recon = i32_shr(recon, 12)
    tmp1 = i16_mul(tmp2, i_step)
    return i32_add(recon, tmp1)


def cdf_est(i_in_val: int) -> int:
    """Pseudocode 53; the caller keeps i_in_val in [-327680, 327680]."""
    idx = i32_shr(i_in_val, 10) + 352
    return tables().cdf_table[idx]


def coef_interval(symbol: int, dither: int, step: int) -> tuple[int, int] | None:
    """Pseudocode 51: (CdfLow, CdfHigh) of a coefficient symbol, or None when empty."""
    mid = idx2reconstruction(symbol, dither, step)
    half = i32_shr(step, 1)
    left = i32_sub(mid, half)
    right = i32_add(left, step)
    # R16: clamp BOTH ends to [-i_max_value, +i_max_value]. Pseudocode 51 clamps only the low
    # end of iLeft and the high end of iRight, so a symbol wholly beyond +-10 would index
    # CDF_TABLE out of range; clamped at both ends its interval is empty (low == high).
    left = max(-COEF_MAX_VALUE, min(COEF_MAX_VALUE, left))
    right = max(-COEF_MAX_VALUE, min(COEF_MAX_VALUE, right))
    lo, hi = cdf_est(left), cdf_est(right)
    if hi <= lo:
        return None
    return lo, hi


_COEF_CACHE: dict[tuple[int, int], list[tuple[int, int, int]]] = {}


def coef_intervals(alloc: int, dither: int) -> list[tuple[int, int, int]]:
    """Non-empty (symbol, low, high) for symbols -i_max_idx..i_max_idx ascending (R1)."""
    key = (alloc, dither)
    got = _COEF_CACHE.get(key)
    if got is None:
        t = tables()
        step = t.step_sizes[alloc]
        i_max_idx = t.ac_coeff_max_index[alloc] + 1  # Pseudocode 50
        got = []
        for sym in range(-i_max_idx, i_max_idx + 1):
            iv = coef_interval(sym, dither, step)
            if iv is not None:
                got.append((sym, iv[0], iv[1]))
        _COEF_CACHE[key] = got
    return got


# ---------------------------------------------------------------------------
# Random number generators, Pseudocodes 54-58 (R4)
# ---------------------------------------------------------------------------


class RndGen:
    """ssf_rndgen_state: four uint8 counters."""

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:  # Pseudocode 55
        self.offset_a = 0
        self.offset_b = 0
        self.state_idx = 1
        self.current_idx = 0

    def _update(self) -> None:
        # Shared tail of Pseudocodes 56 and 57; `x = x++` read as x + 1 (R4), all mod 256.
        self.offset_a = (self.offset_a + 1) & 255
        self.state_idx = (self.state_idx + 1) & 255
        if self.offset_a == 255:
            self.current_idx = (self.current_idx + 1) & 255
            self.offset_b = (self.offset_b + 1) & 255
            self.offset_a = 0
        self.current_idx = (self.current_idx + self.offset_a) & 255
        self.state_idx = (self.state_idx + self.offset_b + self.offset_a) & 255

    def dither(self) -> int:
        """Pseudocode 56: GetDitherValue (Q0.15)."""
        res = tables().dither_table[self.current_idx]
        self._update()
        return res

    def noise(self) -> float:
        """Pseudocode 57: GetRandomNoiseValue; the sum is a float32 addition."""
        t = tables().random_noise_table
        res = _f32(t[self.current_idx] + t[self.state_idx])
        self._update()
        return res


# ---------------------------------------------------------------------------
# Result types
# ---------------------------------------------------------------------------


@dataclass
class SsfGranule:
    stride_flag: int
    num_bands: int
    n_mdct: int
    num_bins: int
    blocks: list[list[float]]  # per block, n_mdct lines, zero above num_bins


@dataclass
class SsfFrame:
    bits: int  # bits ssf_data() occupied, from start_bit
    granules: list[SsfGranule] = field(default_factory=list)
    records: list[tuple[str, int, int]] = field(default_factory=list)  # (name, bits, value)
    # False if any granule's arithmetic decoder ever had offset >= range (a bit pattern no
    # encoder produces; the decode is still well defined, E6). Not part of the C++ contract.
    ac_wellformed: bool = True


# ---------------------------------------------------------------------------
# Helpers of the spectrum decoder (clause 5.2.5)
# ---------------------------------------------------------------------------


def mmse_laplace(f_mid_point: float, f_step_size: float) -> float:
    """Pseudocode 33."""
    r2 = math.sqrt(2)
    f_upper = f_mid_point + f_step_size / 2.0
    f_lower = f_mid_point - f_step_size / 2.0
    f_pdf_lower = r2 / 2 * math.exp(-abs(f_lower) * r2)
    f_pdf_upper = r2 / 2 * math.exp(-abs(f_upper) * r2)
    f_pdf_lower = max(f_pdf_lower, 0.0)
    f_pdf_upper = max(f_pdf_upper, 0.0)
    try:
        f_mmse_n = f_pdf_lower * (r2 * f_lower - 1) + f_pdf_upper * (r2 * f_upper + 1)
        f_mmse_n /= r2 * (f_pdf_lower + f_pdf_upper) - 2
        if f_lower > 0:
            f_mmse_n = f_pdf_upper * (r2 * f_upper + 1.0)
            f_mmse_n -= f_pdf_lower * (r2 * f_lower + 1.0)
            f_mmse_n /= r2 * (f_pdf_upper - f_pdf_lower)
        if f_upper < 0:
            f_mmse_n = f_pdf_upper * (r2 * f_upper - 1.0)
            f_mmse_n -= f_pdf_lower * (r2 * f_lower - 1.0)
            f_mmse_n /= r2 * (f_pdf_upper - f_pdf_lower)
    except ZeroDivisionError:
        raise SsfError("mmse division by zero") from None
    return f_mmse_n


def rfu_from_gain(f_gain: float) -> float:
    """Pseudocode 26: f_rfu from the predictor gain."""
    if f_gain < -1.0:
        return 1.0
    if f_gain < 0.0:
        return -f_gain
    if f_gain < 1.0:
        return f_gain
    if f_gain < 2.0:
        return 2.0 - f_gain
    return 0.0


def decode_env(env_idx: list[int]) -> list[int]:
    """Pseudocode 4a (with the validity range of its NOTE, R8)."""
    env_delta_min = -16
    env_band0_min = -28
    env = [env_idx[0] + env_band0_min]
    for band in range(1, len(env_idx)):
        env.append(env[band - 1] + env_idx[band] + env_delta_min)
    if any(e < -64 or e > 63 for e in env):
        raise SsfError("envelope out of range")
    return env


def interpolate_env(env: list[int], env_prev: list[int], num_blocks: int) -> list[list[int]]:
    """Pseudocode 4b (SHORT_STRIDE); LONG_STRIDE is env_interp[0] = env."""
    if num_blocks == 1:
        return [list(env)]
    unit, half, inv_num_blocks = 1024, 512, 256
    interp = [[0] * len(env) for _ in range(num_blocks)]
    for band in range(len(env)):
        left_delta = i16_mul(env[band] - env_prev[band], unit)
        left_slope = i16_mul(left_delta, inv_num_blocks)
        left_slope = i32_shr(left_slope, 10)
        for block in range(num_blocks):
            i_interp = i16_mul(1 + block, left_slope)
            tmp = i16_mul(env_prev[band], unit)
            i_interp = i32_add(i_interp, tmp)
            if i_interp > 0:
                i_interp = i32_add(i_interp, half)
            else:
                i_interp = i32_sub(i_interp, half)
            interp[block][band] = i32_shr(i_interp, 10)
    return interp


HIGH_FREQ_GAIN_THRESHOLD = 2  # R14: SSF_HIGH_FREQ_GAIN_THRESHOLD, Pseudocode 4d


def refine_env(
    env_interp: list[list[int]], gain_idx: list[int]
) -> tuple[list[list[float]], list[list[int]]]:
    """Pseudocodes 4c, 4d: (f_env_signal[block][band], env_alloc[block][band])."""
    f_env_signal = []
    env_alloc = []
    for block, row in enumerate(env_interp):
        gain = 10.0 ** (gain_idx[block] * 0.1)  # Pseudocode 4c
        sig = []
        alloc = []
        for band, e in enumerate(row):
            s = 2.0 ** (0.5 * e)
            a = e
            if band >= HIGH_FREQ_GAIN_THRESHOLD:
                s *= gain
                a += c_round(2.0 * gain_idx[block] / 3.0)
                a = max(-64, min(63, a))
            sig.append(s)
            alloc.append(a)
        f_env_signal.append(sig)
        env_alloc.append(alloc)
    return f_env_signal, env_alloc


# ---------------------------------------------------------------------------
# The decoder
# ---------------------------------------------------------------------------

NUM_SPEC_BUF = 5  # clause 5.2.6
NUM_ENV_BUF = 4
PRED_LAG_DELTA_MIN = -8  # Pseudocode 4e


class SsfDecoder:
    """Decodes ssf_data() (Table 43), keeping all state across granules and frames."""

    def __init__(self) -> None:
        tables()  # fail early if the Annex C file was not found
        self._dither = RndGen()
        self._noise = RndGen()
        self._started = False
        self._reset_state()

    def _reset_state(self) -> None:
        """R9: what an SSF-I granule resets."""
        self._dither.reset()
        self._noise.reset()
        self._prev_pred_lag_idx = 0
        self._spec_buf: list[list[float]] = [[] for _ in range(NUM_SPEC_BUF)]
        self._env_buf: list[list[float]] = [[] for _ in range(NUM_ENV_BUF)]
        self._last_spec: list[float] = []  # output of the previous block
        self._prev_env: list[int] | None = None
        self._num_bands = 0

    # -- ssf_data(), Table 43 -------------------------------------------------

    def decode(
        self, data: bytes, start_bit: int, end_bit: int, b_iframe: bool, frame_len_base: int
    ) -> SsfFrame:
        if frame_len_base not in SUPPORTED_FRAME_LEN_BASE:
            raise ValueError(f"unsupported frame_len_base {frame_len_base}")
        two_granules = frame_len_base >= 1536
        granule_length = frame_len_base // 2 if two_granules else frame_len_base
        rd = BitReader(data, start_bit, end_bit)
        frame = SsfFrame(bits=0)
        try:
            if b_iframe:
                b_ssf_iframe = 1
            else:
                b_ssf_iframe = self._field(rd, frame, "b_ssf_iframe", 1)
            for g in range(2 if two_granules else 1):
                frame.granules.append(
                    self._granule(
                        rd,
                        frame,
                        bool(b_ssf_iframe) if g == 0 else False,
                        granule_length,
                        frame_len_base,
                    )
                )
        except SsfError:
            self._started = False  # state is unspecified after an error: an I-frame must follow
            raise
        frame.bits = rd.pos - start_bit
        return frame

    @staticmethod
    def _field(rd: BitReader, frame: SsfFrame, name: str, n: int) -> int:
        v = rd.read(n)
        frame.records.append((name, n, v))  # R15
        return v

    # -- ssf_granule(), ssf_st_data(), ssf_ac_data(): Tables 44-46 ---------------

    def _granule(
        self,
        rd: BitReader,
        frame: SsfFrame,
        b_iframe: bool,
        granule_length: int,
        frame_len_base: int,
    ) -> SsfGranule:
        fld = self._field
        t = tables()
        stride_flag = fld(rd, frame, "stride_flag", 1)
        if stride_flag and frame_len_base in LONG_ONLY_FRAME_LEN_BASE:
            raise SsfError("short stride not allowed")  # R8, clause 4.3.7.2.1
        if b_iframe:
            num_bands = fld(rd, frame, "num_bands_minus12", 3) + 12
            self._reset_state()  # R9
            self._started = True
            self._num_bands = num_bands
        else:
            if not self._started:
                raise SsfError("P granule without a preceding I granule")  # R8
            num_bands = self._num_bands  # R10
        short = stride_flag == 1
        start_block = 0
        end_block = 0
        if not short and not b_iframe:
            end_block = 1
        if short:
            end_block = 4
            if b_iframe:
                start_block = 1
        presence = [0] * 4
        delta = [0] * 4
        for block in range(start_block, end_block):
            presence[block] = fld(rd, frame, "predictor_presence_flag", 1)
            if presence[block] == 1:
                if start_block == 1 and block == 1:
                    delta[block] = 0
                else:
                    delta[block] = fld(rd, frame, "delta_flag", 1)
        # ssf_st_data()
        env_idx = [fld(rd, frame, "env_curr_band0_bits", 5)]
        startup_idx = []
        if b_iframe and short:
            startup_idx = [fld(rd, frame, "env_startup_band0_bits", 5)]
        gain_bits = [8] * 4  # gain_idx = 0 for LONG_STRIDE
        if short:
            gain_bits = [fld(rd, frame, "gain_bits", 4) for _ in range(4)]
        num_blocks = 4 if short else 1
        lag_bits = [0] * 4
        var_pres = [0] * 4
        alloc_off = [0] * 4
        for block in range(num_blocks):
            if start_block <= block < end_block and presence[block] == 1:
                if delta[block] == 1:
                    lag_bits[block] = fld(rd, frame, "predictor_lag_delta_bits", 4)
                else:
                    lag_bits[block] = fld(rd, frame, "predictor_lag_bits", 9)
            var_pres[block] = fld(rd, frame, "variance_preserving_flag", 1)
            alloc_off[block] = fld(rd, frame, "alloc_offset_bits", 5)
        # helper elements, Pseudocode 7
        n_mdct = granule_length // num_blocks
        widths = BAND_WIDTHS[n_mdct]
        start_bin = [sum(widths[:b]) for b in range(MAX_NUM_BANDS)]
        end_bin = [start_bin[b] + widths[b] - 1 for b in range(MAX_NUM_BANDS)]
        num_bins = end_bin[num_bands - 1] + 1
        if num_bins > n_mdct:
            raise AssertionError("num_bins exceeds n_mdct")  # cannot happen with Table C.1
        # ssf_ac_data(): one arithmetic decoder per granule
        ac_start = rd.pos
        ac = AcDecoder(rd)
        env = decode_env(
            env_idx + [ac.decode_table_symbol(t.envelope_cdf) for _ in range(1, num_bands)]
        )
        if b_iframe and short:
            startup = decode_env(
                startup_idx + [ac.decode_table_symbol(t.envelope_cdf) for _ in range(1, num_bands)]
            )
            env_prev = startup  # clause 5.2.3.0 NOTE 2
        else:
            env_prev = self._prev_env if self._prev_env is not None else env
            if len(env_prev) != num_bands:
                raise SsfError("envelope state mismatch")
        gain_idx = [g - 8 for g in gain_bits]
        env_interp = interpolate_env(env, env_prev, num_blocks)
        f_env_signal, env_alloc = refine_env(env_interp, gain_idx)
        # Pseudocode 58: dither for the whole granule, drawn up front (R9)
        dither_cur = [[self._dither.dither() for _ in range(num_bins)] for _ in range(num_blocks)]
        blocks_out = []
        for block in range(num_blocks):
            spec = self._block(
                ac,
                block,
                b_iframe,
                n_mdct,
                num_bins,
                num_bands,
                widths,
                start_bin,
                end_bin,
                has_predictor=start_block <= block < end_block and presence[block] == 1,
                pred_in_range=start_block <= block < end_block,
                delta_flag=delta[block],
                lag_field=lag_bits[block],
                var_pres=var_pres[block],
                alloc_offset_bits=alloc_off[block],
                env_alloc=env_alloc[block],
                f_env_signal=f_env_signal[block],
                dither_cur=dither_cur[block],
            )
            blocks_out.append([x + 0.0 for x in spec] + [0.0] * (n_mdct - num_bins))  # E3
        count = ac.finish()
        frame.ac_wellformed = frame.ac_wellformed and ac.wellformed
        if count > rd.end - ac_start:
            raise SsfError("truncated")  # R11
        rd.pos = ac_start + count
        value = rd.bits_value(max(ac_start, rd.pos - 64), min(64, count))
        frame.records.append(("ssf_ac_data", count, value))  # R15
        self._prev_env = env
        return SsfGranule(stride_flag, num_bands, n_mdct, num_bins, blocks_out)

    # -- one SSF block: clauses 5.2.4 - 5.2.7 -------------------------------------

    def _block(
        self,
        ac: AcDecoder,
        block: int,
        b_iframe: bool,
        n_mdct: int,
        num_bins: int,
        num_bands: int,
        widths: list[int],
        start_bin: list[int],
        end_bin: list[int],
        *,
        has_predictor: bool,
        pred_in_range: bool,
        delta_flag: int,
        lag_field: int,
        var_pres: int,
        alloc_offset_bits: int,
        env_alloc: list[int],
        f_env_signal: list[float],
        dither_cur: list[int],
    ) -> list[float]:
        t = tables()
        # -- predictor decoder, Pseudocode 4e (R13)
        f_pred_gain = 0.0
        if pred_in_range and has_predictor:
            i_pred_gain_idx = ac.decode_table_symbol(t.predictor_gain_cdf)
            f_pred_gain = t.pred_gain_quant_tab[i_pred_gain_idx]
            if delta_flag == 1:
                i_pred_lag_idx = lag_field + self._prev_pred_lag_idx + PRED_LAG_DELTA_MIN
            else:
                i_pred_lag_idx = lag_field
        else:
            i_pred_lag_idx = 0
        if not 0 <= i_pred_lag_idx <= 509:
            raise SsfError("predictor lag index out of range")  # R8
        self._prev_pred_lag_idx = i_pred_lag_idx
        f_pred_lag = 640 * 2.0 ** ((i_pred_lag_idx - 509) / 170.0)  # R7

        # -- spectrum decoder helpers, Pseudocode 26
        f_rfu = rfu_from_gain(f_pred_gain)
        thr = 3 if f_rfu > 0.75 else 5
        if var_pres == 1:
            thr = 5
        f_noise_gain_var_pres = math.sqrt(max(0.0, 1 - f_rfu * f_rfu))
        f_noise_gain = 1 - f_rfu

        # -- heuristic scaling and envelope allocation, Pseudocode 27
        f_gain_q = [1.0] * num_bands
        env_alloc_mod = list(env_alloc)
        if f_rfu > 0 and var_pres == 0:
            i_rfu = math.floor(f_rfu * 1024 + 0.5)  # R2
            env_in = [3 * a for a in env_alloc]
            w_db = heuristic_scaling(i_rfu, env_in, widths[:num_bands], num_bins)
            i_w_db = [i32_shr(w // 2, 10) for w in w_db]
            lf_boost_threshold = 3
            i_w_db[0] = i_w_db[0] - lf_boost_threshold if i_w_db[0] > lf_boost_threshold else 0
            for band in range(num_bands):
                f_w_db = w_db[band] / 1024.0  # E4: FLOAT(), Qx.10 to float
                f_gain_q[band] = 10.0 ** (1.5 / 20.0 * f_w_db)
                env_alloc_mod[band] = max(-64, min(63, env_alloc[band] - i_w_db[band]))

        # -- lossless decoding, Pseudocode 31
        i_alloc_offset = alloc_offset_bits - 21
        i_max = max(env_alloc_mod) - 20
        i_alloc_table = [
            max(0, min(20, env_alloc_mod[b] - i_max + i_alloc_offset)) for b in range(num_bands)
        ]
        # Pseudocode 50: arithmetic_decode_coeffs
        quant_idx = [0] * num_bins
        for band in range(num_bands):
            alloc = i_alloc_table[band]
            if alloc == 0:
                continue
            dithered = alloc < thr
            for bin_ in range(start_bin[band], end_bin[band] + 1):
                d = dither_cur[bin_] if dithered else 0
                quant_idx[bin_] = ac.decode_coef_symbol(coef_intervals(alloc, d))

        # -- inverse quantization, Pseudocode 32
        unit = float(1 << 15)
        spec_res = [0.0] * num_bins
        for band in range(num_bands):
            alloc = i_alloc_table[band]
            step = t.step_sizes[alloc]
            vp_band = var_pres == 1 and band > 1
            for bin_ in range(start_bin[band], end_bin[band] + 1):
                if alloc == 0:
                    v = self._noise.noise()
                    v *= f_noise_gain_var_pres if vp_band else f_noise_gain
                elif alloc < thr:
                    mid = idx2reconstruction(quant_idx[bin_], dither_cur[bin_], step)
                    f_post_gain = t.post_gain_lut[alloc - 1]
                    if vp_band:
                        g2 = math.sqrt(f_post_gain) * f_noise_gain_var_pres
                        if g2 > f_post_gain:
                            f_post_gain = g2
                    v = (mid / unit) * f_post_gain
                else:
                    mid = idx2reconstruction(quant_idx[bin_], 0, step)
                    v = mmse_laplace(mid / unit, step / unit)
                # Pseudocode 34: heuristic inverse scaling (f_gain_q is 1.0 when not used)
                spec_res[bin_] = v * (1.0 / f_gain_q[band]) if var_pres == 0 else v

        # -- subband predictor, Pseudocodes 35-37
        self._spec_buf = [self._last_spec, *self._spec_buf[: NUM_SPEC_BUF - 1]]
        self._env_buf = [f_env_signal, *self._env_buf[: NUM_ENV_BUF - 1]]
        spec_pred = [0.0] * num_bins
        if f_pred_gain != 0.0:  # E3
            extract = self._extract(f_pred_lag, n_mdct, num_bins)
            integer_lag = c_round(f_pred_lag / n_mdct)
            if b_iframe and integer_lag > 0:
                integer_lag = 0  # limit to the available envelope buffer entries
            if integer_lag >= NUM_ENV_BUF:
                raise SsfError("lag beyond the envelope buffer")
            env_hist = self._env_buf[integer_lag]
            for band in range(num_bands):
                e = env_hist[band] if band < len(env_hist) else 0.0
                if e == 0.0 and EMPTY_ENV_IS_ERROR:
                    raise SsfError("lag before first block")  # E1/R16
                f_envelope = 1.0 / e
                for bin_ in range(start_bin[band], end_bin[band] + 1):
                    spec_pred[bin_] = extract[bin_] * f_envelope * f_pred_gain

        # -- inverse flattening, Pseudocode 38
        f_spec = [0.0] * num_bins
        for band in range(num_bands):
            for bin_ in range(start_bin[band], end_bin[band] + 1):
                f_spec[bin_] = (spec_res[bin_] + spec_pred[bin_]) * f_env_signal[band]
        self._last_spec = f_spec
        return f_spec

    def _extract(self, f_pred_lag: float, n_mdct: int, num_bins: int) -> list[float]:
        """Pseudocode 36 with the displayed equation of clause 5.2.6 (R6, R10, E5)."""
        t = tables()
        f_period = f_pred_lag / n_mdct  # T0
        k_s = 0
        if f_period > 81.0 / 32.0:
            k_s = 1
            f_period -= 1.0  # T, the reduced period
        tab_idx = 0 if f_period <= 9.0 / 32.0 else c_round(16 * f_period) - 4
        if not 0 <= tab_idx <= 36:
            raise SsfError("predictor table index out of range")
        rt = t.pred_rts[tab_idx]
        rf = t.pred_rfs[tab_idx]
        c = t.c_all(tab_idx)
        # Z[n][k] for n = -Rf .. num_bins-1+Rf, stored at index n + Rf (R10 for the lengths).
        z = []
        for k in range(rt):
            base = self._spec_buf[k + k_s]
            vals = (list(base) + [0.0] * num_bins)[:num_bins]
            row = [vals[-1 - n] for n in range(-rf, 0)] + vals + [0.0] * rf
            z.append(row)
        # f = g(Phi(mu)) for every mu = 2*bin + nu + 1 that occurs
        min_2t = 2 * f_period if 2 * f_period < 1 else 1.0
        f_of_mu: dict[int, int] = {}
        for mu in range(1 - rf, 2 * (num_bins - 1) + rf + 2):
            phi = (f_period / 4) * mu
            phi = math.floor(phi + 0.5) - phi
            if phi > f_period:
                f = 32
            elif phi < -f_period:
                f = -32
            else:
                f = c_round(64 * phi / min_2t)
            f_of_mu[mu] = f
        out = []
        for p in range(num_bins):
            tmp = 0.0
            for nu in range(-rf, rf + 1):
                f = f_of_mu[2 * p + nu + 1]
                coeffs = c[nu + rf][f + 32]
                for k in range(rt):
                    s = -1.0 if (p % 2 == 1 and k % 2 == 0) else 1.0  # (-1)^((k+1)p)
                    tmp += s * coeffs[k] * z[k][p + nu + rf]
            out.append(tmp)
        return out


# ---------------------------------------------------------------------------
# Test support: a range coder matching Pseudocodes 40-47, minimal termination
# ---------------------------------------------------------------------------


class AcEncoder:
    """Arithmetic encoder for the model of clause 5.2.8.2 (used by selftest only).

    Exact integers, no overflow: low is unbounded so a carry needs no special case. The
    decoder's registers are the 30-bit windows of low and the code value.
    """

    def __init__(self) -> None:
        self.low = 0
        self.range = SSF_THRESHOLD_LARGE
        self.shifts = 0

    def encode(self, cdf_low: int, cdf_high: int) -> None:
        r = self.range >> SSF_MODEL_BITS
        self.low += r * cdf_low
        if cdf_high < SSF_MODEL_UNIT:
            self.range = r * (cdf_high - cdf_low)
        else:
            self.range -= r * cdf_low
        while self.range <= SSF_THRESHOLD_SMALL:
            self.range <<= 1
            self.low <<= 1
            self.shifts += 1

    def finish(self) -> tuple[int, int]:
        """Minimal termination: the fewest bits after which every continuation decodes the
        same. Returns (number of bits emitted, their value as an integer)."""
        for k in range(1, SSF_RANGE_BITS + 1):
            r = SSF_RANGE_BITS - k
            val = -(-self.low // (1 << r)) << r  # smallest multiple of 2^r that is >= low
            if val + (1 << r) - 1 <= self.low + self.range - 1:
                return self.shifts + k, val >> r
        raise AssertionError("no termination found")

    def finish_zero_padded(self) -> tuple[int, int]:
        """Fewest bits such that the stream followed by zero bits lies inside the interval."""
        for k in range(1, SSF_RANGE_BITS + 1):
            r = SSF_RANGE_BITS - k
            val = -(-self.low // (1 << r)) << r
            if val < self.low + self.range:
                return self.shifts + k, val >> r
        raise AssertionError("no termination found")


# ---------------------------------------------------------------------------
# Random vectors
# ---------------------------------------------------------------------------

CASE_FRAME_LEN_BASES = (384, 512, 768, 960, 1024, 1536, 1920, 2048)


def _frame_text(index: int, b_iframe: bool, data: bytes, frame: SsfFrame | None, err: str) -> str:
    lines = [f"frame {index} b_iframe {int(b_iframe)} bytes {data.hex()}"]
    if frame is None:
        lines.append(f"  error {err}")
        return "\n".join(lines)
    lines.append(f"  ok bits {frame.bits} granules {len(frame.granules)}")
    for g, gr in enumerate(frame.granules):
        lines.append(
            f"  granule {g} stride {gr.stride_flag} num_bands {gr.num_bands} "
            f"n_mdct {gr.n_mdct} num_bins {gr.num_bins}"
        )
        for b, blk in enumerate(gr.blocks):
            # Only the num_bins coded lines; the n_mdct - num_bins above them are zero.
            lines.append(f"  block {b} " + " ".join(float(x).hex() for x in blk[: gr.num_bins]))
    return "\n".join(lines)


_Frame = tuple[bool, bytes, "SsfFrame | None", str]  # (b_iframe, data, decoded, error)


def _new_stats() -> dict:
    return {
        "attempts": 0,
        "valid": 0,
        "malformed_valid": 0,
        "first": [0, 0],
        "later": [0, 0],
        "reasons": {},
        "base": {},
    }


def _gen_case(
    seed: int,
    case: int,
    flb: int,
    nframes: int,
    attempts: int,
    kind: str,
    stats: dict | None = None,
) -> list[_Frame]:
    """One case: a sequence of random-byte frames decoded by one SsfDecoder.

    Frame i is random bytes drawn from a generator seeded by (seed, case, i, attempt).
      "raw"   takes the first attempt of every frame as it comes (the case ends at the first
              invalid frame), so error paths are represented;
      "ok"    redraws a frame, from the same decoder state, until it is valid and its
              arithmetic decoder stayed well formed (offset < range throughout, the only
              streams a real encoder makes) - at most `attempts` times;
      "wrap"  the same without the well-formedness condition: the uint32 wrap-around
              behaviour (E6) of the arithmetic decoder is part of what is compared.
    A case that cannot find its next frame stops early (it never ends on a frame whose
    recorded outcome would be wrong). Every attempt counts in `stats`, so the valid fraction
    is that of random frames given the decoder state."""
    dec = SsfDecoder()
    frames: list[_Frame] = []
    for i in range(nframes):
        b_iframe = i == 0 or random.Random(f"{seed}/{case}/{i}/iframe").random() < 0.2
        done = False
        for a in range(1 if kind == "raw" else attempts):
            rng = random.Random(f"{seed}/{case}/{i}/{a}")
            data = bytes(rng.getrandbits(8) for _ in range(rng.randint(256, 2048)))
            trial = copy.deepcopy(dec)
            try:
                fr = trial.decode(data, 0, 8 * len(data), b_iframe, flb)
                err = ""
            except SsfError as e:
                fr, err = None, str(e)
            if stats is not None:
                group = "first" if i == 0 else "later"
                stats["attempts"] += 1
                stats["valid"] += fr is not None
                stats[group][0] += 1
                stats[group][1] += fr is not None
                pb = stats["base"].setdefault(flb, [0, 0])
                pb[0] += 1
                pb[1] += fr is not None
                if fr is None:
                    stats["reasons"][err] = stats["reasons"].get(err, 0) + 1
                elif not fr.ac_wellformed:
                    stats["malformed_valid"] += 1
            if fr is None and kind != "raw" and a + 1 < attempts:
                continue
            if fr is not None and kind == "ok" and not fr.ac_wellformed:
                continue
            frames.append((b_iframe, data, fr, err))
            if fr is not None:
                dec = trial
                done = True
            break
        if not done:
            break
    return frames


def make_vectors(
    seed: int, cases: int, frames: int, attempts: int = 300, raw_every: int = 5
) -> tuple[str, dict]:
    """Vector file text and statistics. Case kinds: see _gen_case; every raw_every-th case is
    "raw", every raw_every-th one offset by two is "wrap", the rest are "ok"."""
    text = ["ssfvec 1"]
    stats = _new_stats()
    stats["cases_full"] = 0
    stats["cases_3valid"] = 0
    stats["kinds"] = {}
    for c in range(cases):
        flb = CASE_FRAME_LEN_BASES[c % len(CASE_FRAME_LEN_BASES)]
        kind = "ok"
        if raw_every > 0 and c % raw_every == raw_every - 1:
            kind = "raw"
        elif raw_every > 0 and c % raw_every == 2:
            kind = "wrap"
        stats["kinds"][kind] = stats["kinds"].get(kind, 0) + 1
        got = _gen_case(seed, c, flb, frames, attempts, kind, stats)
        valid = sum(fr is not None for _, _, fr, _ in got)
        stats["cases_full"] += valid == frames
        stats["cases_3valid"] += valid >= min(3, frames)
        text.append(f"case {kind}_{flb}_{c} frame_len_base {flb}")
        for i, (b_iframe, data, fr, why) in enumerate(got):
            text.append(_frame_text(i, b_iframe, data, fr, why))
        text.append("end")
    return "\n".join(text) + "\n", stats


def trace_text(vec_text: str, spec_dir: str | None = None) -> str:
    """Re-decode a vector file and list the R15 records of every ok frame."""
    del spec_dir
    out = []
    dec = None
    flb = 0
    for line in vec_text.splitlines():
        parts = line.split()
        if parts[:1] == ["case"]:
            out.append(line)
            dec = SsfDecoder()
            flb = int(parts[3])
        elif parts[:1] == ["frame"]:
            data = bytes.fromhex(parts[5]) if len(parts) > 5 else b""
            try:
                fr = dec.decode(data, 0, 8 * len(data), parts[3] == "1", flb)
            except SsfError as e:
                out.append(f"frame {parts[1]} error {e}")
                continue
            out.append(f"frame {parts[1]}")
            out.extend(f"  rec {n} {b} {v}" for n, b, v in fr.records)
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# Self tests
# ---------------------------------------------------------------------------


def _check(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def _raises(exc: type[Exception], fn, *args) -> bool:
    try:
        fn(*args)
    except exc:
        return True
    return False


def test_tables() -> str:
    t = tables()
    # Table C.1: 19 rows, widths non-decreasing with the band index, scaling with the block length
    for n, w in BAND_WIDTHS.items():
        _check(len(w) == 19, f"C.1 column {n} has {len(w)} rows")
        _check(all(a <= b for a, b in itertools.pairwise(w)), f"C.1 column {n} not monotone")
        _check(sum(w) <= n, f"C.1 column {n}: all 19 bands exceed the block")
    _check(sum(BAND_WIDTHS[1024]) == 320 and sum(BAND_WIDTHS[768]) == 240, "C.1 column sums")
    # CDF tables: non-decreasing, spanning the model unit
    for name, c, last in (
        ("envelope", t.envelope_cdf, 32768),
        ("predictor", t.predictor_gain_cdf, 32768),
    ):
        _check(c[0] == 0 and c[-1] == last, f"{name} CDF ends")
        _check(all(a < b for a, b in itertools.pairwise(c)), f"{name} CDF not strictly rising")
    cdf = t.cdf_table
    _check(all(a <= b for a, b in itertools.pairwise(cdf)), "CDF_TABLE not monotone (R16)")
    _check(
        abs(cdf[352] - 16384) < 400 and cdf[704] == 32768,
        f"CDF_TABLE centre/end {cdf[352]} {cdf[704]}",
    )
    _check(all(0 <= d < 32768 for d in t.dither_table), "dither range")
    _check(all(a > b for a, b in itertools.pairwise(t.step_sizes[1:])), "steps")

    # R5 evidence: eta (33 entries) is the smooth axis of every coefficient table. Mean
    # |second difference| along eta, read with the layout of R5 and with the printed formula
    # of Pseudocode C.1 (the two coincide when Rt == 1).
    def eta_roughness(mat, rf, rt, printed):
        total = 0.0
        for nu in range(2 * rf + 1):
            for k in range(rt):
                idx = [
                    ((nu * rt + k) * 33 + e) if printed else ((nu * 33 + e) * rt + k)
                    for e in range(33)
                ]
                v = [mat[i] for i in idx]
                total += sum(abs(v[i] - 2 * v[i + 1] + v[i + 2]) for i in range(31)) / 31
        return total / ((2 * rf + 1) * rt)

    for ti in range(37):
        rf, rt = t.pred_rfs[ti], t.pred_rts[ti]
        mat = t.pred_coeff_mat[ti]
        ours = eta_roughness(mat, rf, rt, printed=False)
        theirs = eta_roughness(mat, rf, rt, printed=True)
        _check(ours < 1.5, f"R5 layout not smooth in eta, table {ti}: {ours:.2f}")
        if rt > 1:
            _check(ours < 0.5 * theirs, f"printed layout as smooth as R5, table {ti}")
    return (
        "Annex C tables parse; C.1 has 19 rows; CDFs monotone; coefficient layout R5 smooth in eta"
    )


def test_fixed_point() -> str:
    _check(i32_shr(-5, 1) == -2 and i32_shr(5, 1) == 2, "sign-preserving right shift")
    _check(i32_shr(-1, 10) == 0 and i32_shr(-1024, 10) == -1, "right shift towards zero")
    _check(i32_shl(-3, 4) == -48 and i32_shl(3, 4) == 48, "sign-preserving left shift")
    _check(c_div(-7, 2) == -3 and c_div(7, -2) == -3 and c_div(-7, -2) == 3, "C division")
    _check(_raises(SsfError, c_div, 1, 0), "division by zero")
    _check(_raises(SsfError, i32_shl, 1, 31), "left shift into the sign bit is an overflow")
    _check(i32_shl(1, 30) == 1 << 30, "shift to bit 30")
    _check(_raises(SsfError, i32_add, INT32_MAX, 1), "add overflow")
    _check(_raises(SsfError, i32_sub, INT32_MIN, 1), "sub overflow")
    _check(_raises(SsfError, i16_mul, 1 << 16, 1 << 15), "mul overflow")
    _check(i16_mul(-46341, 46340) == -2147441940, "mul in range")
    _check(c_round(2.5) == 3 and c_round(-2.5) == -2 and c_round(-2.6) == -3, "R6 round")
    # Idx2Reconstruction: with no dither it is exactly index * step; with dither it is
    # (index - d/2^15) * step to within the fixed point truncation
    for step in (167936, 3600, 36000):
        for idx in range(-9, 10):
            _check(idx2reconstruction(idx, 0, step) == idx * step, f"recon idx {idx} step {step}")
            for d in (0, 1, 16384, 32640, 12345):
                got = idx2reconstruction(idx, d, step)
                want = (idx - d / 32768) * step
                _check(abs(got - want) <= step / 4096 + 2, f"recon {idx} {d} {step}: {got} {want}")
    # the ten-line envelope index decode and env interpolation on a hand example
    _check(decode_env([20, 16, 17, 15]) == [-8, -8, -7, -8], "Pseudocode 4a")
    _check(_raises(SsfError, decode_env, [31] * 40), "env range")
    ip = interpolate_env([4], [0], 4)  # slope 1 per block -> 1, 2, 3, 4
    _check([r[0] for r in ip] == [1, 2, 3, 4], f"Pseudocode 4b ramp {ip}")
    ip = interpolate_env([-4], [0], 4)
    _check([r[0] for r in ip] == [-1, -2, -3, -4], f"Pseudocode 4b negative ramp {ip}")
    _check(interpolate_env([5], [5], 4) == [[5]] * 4, "Pseudocode 4b constant")
    return "shift/division/overflow semantics, Idx2Reconstruction, env decode and interpolation"


def test_db_maps() -> str:
    # Map_dB_to_Lin: Qx.10 dB -> Qx.10 amplitude 10^(dB/20); its 'index out of range'
    # branch (>= 40 dB) returns 100.0.
    worst = 0.0
    for x in range(0, 40 * 1024, 7):
        got = map_db_to_lin(x) / 1024
        want = 10 ** (x / 1024 / 20)
        worst = max(worst, abs(got / want - 1))
    _check(worst < 0.10, f"Map_dB_to_Lin deviates {worst:.3%} from 10^(x/20)")
    _check(map_db_to_lin(40 * 1024) == 100 * 1024, "dB_to_Lin out of range")
    worst_lin = 0.0
    for x in range(1024, 100 * 1024, 5):
        got = map_lin_to_db(x) / 1024
        want = 20 * math.log10(x / 1024)
        worst_lin = max(worst_lin, abs(got - want))
    _check(worst_lin < 1.4, f"Map_Lin_to_dB deviates {worst_lin:.3f} dB from 20*log10(x)")
    worst_hi = max(
        abs(map_lin_to_db(x) / 1024 - 20 * math.log10(x / 1024))
        for x in range(6 * 1024, 100 * 1024, 5)
    )
    _check(worst_hi < 0.5, f"Map_Lin_to_dB deviates {worst_hi:.3f} dB above 6.0")
    # round trip through both maps stays within the two approximation errors
    worst_rt = 0.0
    for x in range(0, 39 * 1024, 11):
        back = map_lin_to_db(map_db_to_lin(x)) / 1024
        worst_rt = max(worst_rt, abs(back - x / 1024))
    _check(worst_rt < 2.5, f"round trip {worst_rt:.3f} dB")
    _check(map_lin_to_db(100 * 1024) == 40 * 1024, "Lin_to_dB out of range")
    return (
        f"Map_dB_to_Lin within {worst:.2%} of 10^(x/20) on [0,40) dB; "
        f"Map_Lin_to_dB within {worst_lin:.3f} dB of 20*log10(x) on [1,100) "
        f"({worst_hi:.3f} dB above 6.0); "
        f"round trip within {worst_rt:.3f} dB"
    )


def _float_water_filling(env_in: list[int], widths: list[int], rfu: float) -> list[float]:
    """The weights of Pseudocode 28 in exact arithmetic: remove the volume rfu^2 * sum(w * L)
    from the top of the amplitude profile L = 10^(dB/20) (dB compressed to 40 dB), spreading
    any remainder below the lowest band uniformly; weights are (dB - level dB) / 3 in [0, 15]."""
    mn, mx = min(env_in), max(env_in)
    scale = 40.0 / (mx - mn) if (mx - mn) > 40 else 1.0
    loc = [(e - mn) * scale for e in env_in]
    amp = [10 ** (x / 20) for x in loc]
    target = rfu * rfu * sum(a * w for a, w in zip(amp, widths, strict=True))
    lo, hi = 0.0, max(amp)
    for _ in range(200):  # bisect the water level
        lam = (lo + hi) / 2
        if sum(max(a - lam, 0.0) * w for a, w in zip(amp, widths, strict=True)) > target:
            lo = lam
        else:
            hi = lam
    lam = (lo + hi) / 2
    if lam < min(amp):  # below the lowest band: uniform removal over all bins
        above = sum((a - min(amp)) * w for a, w in zip(amp, widths, strict=True))
        lam = min(amp) - (target - above) / sum(widths)
    db = 20 * math.log10(max(lam, 1e-9))
    return [max(0.0, min(15.0, (x - db) / 3)) for x in loc]


def test_heuristic() -> str:
    """Pseudocode 28 against a floating point reverse water-filling.

    Run twice: with the two maps replaced by exact functions (so only the integer
    water-filling logic is under test; it must match closely), then with the standard's
    approximate maps (the deviation is then the maps' own, reported)."""
    rng = random.Random(7)
    exact_db_to_lin = lambda x: max(0, round(10 ** (x / 1024 / 20) * 1024))  # noqa: E731
    exact_lin_to_db = lambda x: round(20 * math.log10(max(x, 1) / 1024) * 1024)  # noqa: E731
    approx = (map_db_to_lin, map_lin_to_db)
    worst = {"exact": 0.0, "exact_mid": 0.0, "standard": 0.0}
    n_trials = 0
    try:
        for _ in range(300):
            nb = rng.randint(12, 19)
            widths = BAND_WIDTHS[rng.choice([192, 256, 768, 1024])][:nb]
            env_alloc = [
                max(-64, min(63, rng.randint(-20, 5) + int(rng.gauss(0, 6)))) for _ in range(nb)
            ]
            env_in = [3 * e for e in env_alloc]
            rfu = rng.choice([abs(g) for g in tables().pred_gain_quant_tab if g != 0])
            i_rfu = math.floor(rfu * 1024 + 0.5)
            want = _float_water_filling(env_in, widths, i_rfu / 1024)
            for which, maps in (
                ("exact", (exact_db_to_lin, exact_lin_to_db)),
                ("standard", approx),
            ):
                globals()["map_db_to_lin"], globals()["map_lin_to_db"] = maps
                got = heuristic_scaling(i_rfu, env_in, widths, sum(widths))
                _check(all(0 <= g <= 15 * 1024 for g in got), "output range")
                for g, w in zip(got, want, strict=True):
                    worst[which] = max(worst[which], abs(g / 1024 - w))
                    if which == "exact" and rfu <= 0.8:
                        worst["exact_mid"] = max(worst["exact_mid"], abs(g / 1024 - w))
            n_trials += 1
    finally:
        globals()["map_db_to_lin"], globals()["map_lin_to_db"] = approx
    # With rfu near 1 the level sinks to a few percent of the lowest band and the Q10
    # truncations in iMtr show (up to ~0.13 weight units); below 0.8 they stay small.
    _check(worst["exact_mid"] < 0.03, f"water-filling logic deviates {worst['exact_mid']:.4f}")
    _check(worst["exact"] < 0.2, f"water-filling logic deviates {worst['exact']:.4f}")
    _check(worst["standard"] < 1.0, f"standard maps deviate {worst['standard']:.3f}")
    return (
        f"HeuristicScaling vs float water-filling over {n_trials} trials: max deviation "
        f"{worst['exact_mid']:.4f} (rfu <= 0.8; {worst['exact']:.3f} all rfu) with exact maps "
        f"(logic), {worst['standard']:.3f} with the "
        f"standard maps (weight units of 1 env step)"
    )


def _encode_symbols(syms: list[tuple[str, object]]) -> tuple[AcEncoder, list[int]]:
    """Encode a mixed symbol list with the same models the decoder uses."""
    t = tables()
    enc = AcEncoder()
    expected = []
    for kind, arg in syms:
        if kind == "env":
            s = arg
            enc.encode(t.envelope_cdf[s], t.envelope_cdf[s + 1])
        elif kind == "pred":
            s = arg
            enc.encode(t.predictor_gain_cdf[s], t.predictor_gain_cdf[s + 1])
        else:
            (alloc, dither, s) = arg
            ivs = {sym: (lo, hi) for sym, lo, hi in coef_intervals(alloc, dither)}
            enc.encode(*ivs[s])
        expected.append(s if kind != "coef" else arg[2])
    return enc, expected


def _random_symbols(rng: random.Random, n: int) -> list[tuple[str, object]]:
    out: list[tuple[str, object]] = []
    for _ in range(n):
        kind = rng.choice(["env", "pred", "coef", "coef", "coef"])
        if kind in ("env", "pred"):
            out.append((kind, rng.randrange(32)))
        else:
            alloc = rng.randint(1, 20)
            dither = rng.choice([0, rng.choice(tables().dither_table)])
            ivs = coef_intervals(alloc, dither)
            out.append((kind, (alloc, dither, rng.choice(ivs)[0])))
    return out


def _decode_symbols(ac: AcDecoder, syms: list[tuple[str, object]]) -> list[int]:
    t = tables()
    got = []
    for kind, arg in syms:
        if kind == "env":
            got.append(ac.decode_table_symbol(t.envelope_cdf))
        elif kind == "pred":
            got.append(ac.decode_table_symbol(t.predictor_gain_cdf))
        else:
            alloc, dither, _ = arg
            got.append(ac.decode_coef_symbol(coef_intervals(alloc, dither)))
    return got


def _bits_to_bytes(nbits: int, value: int, tail: bytes) -> tuple[bytes, int]:
    """The stream: nbits of value, then the tail bytes starting at bit nbits."""
    total = nbits + 8 * len(tail)
    big = (value << (8 * len(tail))) | int.from_bytes(tail or b"\0", "big") if tail else value
    pad = (-total) % 8
    big <<= pad
    return big.to_bytes((total + pad) // 8, "big"), total


def test_arithmetic_coder() -> str:
    rng = random.Random(11)
    t = tables()
    n_streams = 0
    n_symbols = 0
    mismatches_zero_pad = 0
    for trial in range(400):
        syms = _random_symbols(rng, rng.randint(1, 60))
        enc, expected = _encode_symbols(syms)
        nbits, value = enc.finish()
        zbits, _ = enc.finish_zero_padded()
        _check(zbits <= nbits, "zero padded termination cannot be longer than the safe one")
        # every continuation after the emitted bits must decode identically
        for tail in (b"", b"\x00" * 16, b"\xff" * 16, bytes(rng.getrandbits(8) for _ in range(16))):
            data, total = _bits_to_bytes(nbits, value, tail)
            rd = BitReader(data, 0, total)
            ac = AcDecoder(rd)
            got = _decode_symbols(ac, syms)
            _check(got == expected, f"trial {trial}: symbols differ, tail {tail[:2]!r}")
            count = ac.finish()
            _check(count == nbits, f"trial {trial}: AcDecodeFinish {count} != encoder {nbits}")
        n_streams += 1
        n_symbols += len(syms)
        mismatches_zero_pad += zbits != nbits
    # a stream placed at a non-zero bit offset, with a preceding and a following field
    syms = _random_symbols(rng, 40)
    enc, expected = _encode_symbols(syms)
    nbits, value = enc.finish()
    prefix_bits = 11
    big = (0x5A5 << nbits) | value
    big = (big << 21) | 0x1ABCDE
    total = prefix_bits + nbits + 21
    pad = (-total) % 8
    data = (big << pad).to_bytes((total + pad) // 8, "big")
    rd = BitReader(data, 0, total)
    rd.pos = prefix_bits
    ac = AcDecoder(rd)
    _check(_decode_symbols(ac, syms) == expected, "offset stream symbols")
    _check(ac.finish() == nbits, "offset stream count")
    # the all-ones and all-zeros extreme streams
    for fill in (b"\x00", b"\xff"):
        data = fill * 64
        ac = AcDecoder(BitReader(data, 0, 8 * len(data)))
        try:
            ac.decode_table_symbol(t.envelope_cdf)
            ac.finish()
        except SsfError:
            pass
    _check(AcEncoder().finish() == (1, 0), "empty message terminates in one bit")
    return (
        f"decoder agrees with an independent range coder on {n_streams} streams / {n_symbols} "
        f"symbols x 4 continuations; AcDecodeFinish == bits emitted at the minimal safe "
        f"termination every time (zero-padded termination would be shorter in "
        f"{mismatches_zero_pad} streams)"
    )


def test_coef_cdf() -> str:
    t = tables()
    holes = 0
    overlaps = 0
    total = 0
    lost = 0.0
    for alloc in range(1, 21):
        for d in [0, *t.dither_table[::17]]:
            ivs = coef_intervals(alloc, d)
            total += 1
            for (_, _, h1), (_, l2, _) in itertools.pairwise(ivs):
                holes += l2 > h1
                overlaps += l2 < h1
            lost += (ivs[0][1] + 32768 - ivs[-1][2]) / 32768
    _check(holes == 0 or holes < total, "coefficient intervals")
    return (
        f"coefficient CDFs: {total} (alloc, dither) models; neighbouring-interval gaps {holes}, "
        f"overlaps {overlaps}; mean probability outside the symbol set {lost / total:.4%}"
    )


def test_rng() -> str:
    t = tables()
    g = RndGen()
    # first draws by hand (Pseudocode 55, 56): cur follows the triangular numbers 0,1,3,6,..
    cur = 0
    for i in range(254):
        _check(g.current_idx == cur % 256, f"current index at draw {i}")
        _check(g.dither() == t.dither_table[cur % 256], f"dither value at draw {i}")
        cur += i + 1
    _check(g.offset_a == 254, "offset A after 254 draws")
    # the 255th draw wraps A (R4: current index also increments, B increments)
    before = (g.current_idx, g.offset_b)
    g.dither()
    _check(g.offset_a == 0 and g.offset_b == before[1] + 1, "wrap of offset A")
    _check(g.current_idx == (before[0] + 1) & 255, "current index after the wrap")
    # the noise generator walks the same counters and returns a float32 sum
    gd, gn = RndGen(), RndGen()
    for i in range(3000):
        _check(
            (gd.current_idx, gd.state_idx, gd.offset_a, gd.offset_b)
            == (gn.current_idx, gn.state_idx, gn.offset_a, gn.offset_b),
            f"generators diverged at {i}",
        )
        ci, si = gn.current_idx, gn.state_idx
        v = gn.noise()
        _check(v == _f32(v), "noise value is a float32")
        _check(v == _f32(t.random_noise_table[ci] + t.random_noise_table[si]), "noise sum")
        gd.dither()
    # reset restores the initial state (Pseudocode 55)
    gn.reset()
    _check((gn.offset_a, gn.offset_b, gn.state_idx, gn.current_idx) == (0, 0, 1, 0), "reset")
    _check(gn.noise() == _f32(t.random_noise_table[0] + t.random_noise_table[1]), "first noise")
    # the table values are float32
    _check(all(x == _f32(x) for x in t.random_noise_table), "noise table float32")
    return "generators follow the hand-derived counter sequence; noise is a float32 sum; reset"


def _find_valid_stream(flb: int, nframes: int, seed: int) -> list[tuple[bool, bytes]]:
    got = _gen_case(seed, 0, flb, nframes, 3000, "ok")
    _check(all(fr is not None for _, _, fr, _ in got) and len(got) == nframes, "valid stream")
    return [(b, d) for b, d, _, _ in got]


def _decode_all(frames: list[tuple[bool, bytes]], flb: int) -> list[SsfFrame]:
    dec = SsfDecoder()
    return [dec.decode(d, 0, 8 * len(d), b, flb) for b, d in frames]


def test_end_to_end() -> str:
    notes = []
    for flb in (768, 1536, 512):
        frames = _find_valid_stream(flb, 3, 1)
        a = _decode_all(frames, flb)
        b = _decode_all(frames, flb)
        _check(a == b, f"determinism, frame_len_base {flb}")
        for fr in a:
            for gr in fr.granules:
                _check(len(gr.blocks) == (4 if gr.stride_flag else 1), "block count")
                for blk in gr.blocks:
                    _check(len(blk) == gr.n_mdct, "n_mdct lines")
                    _check(all(x == 0.0 for x in blk[gr.num_bins :]), "zero above num_bins")
                    _check(all(math.isfinite(x) for x in blk), "finite output")
                    _check(any(x != 0.0 for x in blk[: gr.num_bins]), "non-silent")
            _check(fr.bits > 0 and fr.records[-1][0] == "ssf_ac_data", "records")
        notes.append(f"{flb}:{sum(f.bits for f in a)}b")
        # state continuity: a P frame decoded after other predecessors changes (the random
        # generators, envelope and predictor state all carry over); an I frame never does.
        # One P frame may by chance not touch the carried state, so look for one that does.
        saw_p_vary = False
        for seed in range(1, 25):
            fr3 = _find_valid_stream(flb, 3, seed) if seed > 1 else frames
            outs = set()
            for alt_seed in range(5):
                d = SsfDecoder()
                src = (
                    fr3[0][1] if alt_seed == 0 else _find_valid_stream(flb, 1, 90 + alt_seed)[0][1]
                )
                d.decode(src, 0, 8 * len(src), True, flb)
                try:
                    f = d.decode(fr3[1][1], 0, 8 * len(fr3[1][1]), fr3[1][0], flb)
                    outs.add(repr(f.granules))
                except SsfError:
                    outs.add("error")
            if fr3[1][0]:
                _check(len(outs) == 1, f"I-frame depends on history ({flb})")
            else:
                saw_p_vary = saw_p_vary or len(outs) > 1
        _check(saw_p_vary, f"P-frames ignored their predecessors ({flb})")
    # an I-frame decodes the same after any history (R9), and the same as from a fresh decoder
    flb = 1536
    frames = _find_valid_stream(flb, 3, 5)
    iframe = _find_valid_stream(flb, 1, 31)[0][1]
    ref = SsfDecoder().decode(iframe, 0, 8 * len(iframe), True, flb)
    dec = SsfDecoder()
    for b, d in frames:
        dec.decode(d, 0, 8 * len(d), b, flb)
    got = dec.decode(iframe, 0, 8 * len(iframe), True, flb)
    _check(got == ref, "I-frame after history differs from a fresh decode")
    # a P granule first is refused; truncation is reported; trailing data is irrelevant
    d0 = bytes([frames[1][1][0] & 0x7F]) + frames[1][1][1:]  # b_ssf_iframe = 0
    try:
        SsfDecoder().decode(d0, 0, 8 * len(d0), False, flb)
        _check(False, "P granule accepted on a fresh decoder")
    except SsfError as e:
        _check("I granule" in str(e), f"P-first reason {e}")
    try:
        SsfDecoder().decode(iframe, 0, 3, True, flb)  # ends inside the fixed-width fields
        _check(False, "short substream accepted")
    except SsfError as e:
        _check(str(e) == "truncated", f"short substream reason {e}")
    full = SsfDecoder().decode(iframe, 0, 8 * len(iframe), True, flb)
    try:  # one bit short of what ssf_data() occupies: refused (zeros replace the look-ahead)
        SsfDecoder().decode(iframe, 0, full.bits - 1, True, flb)
        _check(False, "stream one bit short accepted")
    except SsfError:
        pass
    fr = SsfDecoder().decode(iframe, 0, 8 * len(iframe), True, flb)
    longer = iframe + b"\x55" * 9
    fr2 = SsfDecoder().decode(longer, 0, 8 * len(longer), True, flb)
    _check(fr.granules[0] == fr2.granules[0], "first granule changed by trailing bytes")
    # a stream at a non-zero bit offset decodes identically
    pre = (0b10110 << (8 * len(iframe))) | int.from_bytes(iframe, "big")
    data = (pre << 3).to_bytes(len(iframe) + 1, "big")  # 5 prefix bits, 3 trailing pad bits
    fr3 = SsfDecoder().decode(data, 5, 5 + 8 * len(iframe), True, flb)
    _check(fr3.granules == fr.granules and fr3.bits == fr.bits, "bit-offset decode")
    return "deterministic; I-frames independent of history; P-frames depend on it; " + ", ".join(
        notes
    )


def test_predictor_block() -> str:
    """Extractor properties: zero in, zero out; linear; reflection extension at the low end."""
    dec = SsfDecoder()
    nb = 24
    lag = 640 * 2.0 ** ((300 - 509) / 170.0)
    dec._spec_buf = [[0.0] * nb for _ in range(5)]
    _check(all(x == 0.0 for x in dec._extract(lag, 256, nb)), "zero spectrum")
    rng = random.Random(3)
    a = [[rng.uniform(-1, 1) for _ in range(nb)] for _ in range(5)]
    b = [[rng.uniform(-1, 1) for _ in range(nb)] for _ in range(5)]
    dec._spec_buf = [[x + y for x, y in zip(r, s, strict=True)] for r, s in zip(a, b, strict=True)]
    ab = dec._extract(lag, 256, nb)
    dec._spec_buf = a
    ea = dec._extract(lag, 256, nb)
    dec._spec_buf = b
    eb = dec._extract(lag, 256, nb)
    _check(max(abs(x - (y + z)) for x, y, z in zip(ab, ea, eb, strict=True)) < 1e-12, "linearity")
    # R10: shorter / longer stored spectra are zero padded / truncated
    dec._spec_buf = [[1.0] * 10 for _ in range(5)]
    short = dec._extract(lag, 256, nb)
    dec._spec_buf = [[1.0] * 10 + [0.0] * (nb - 10) for _ in range(5)]
    _check(short == dec._extract(lag, 256, nb), "R10 padding")
    dec._spec_buf = [[1.0] * nb + [9.0] * 7 for _ in range(5)]
    longer = dec._extract(lag, 256, nb)
    dec._spec_buf = [[1.0] * nb for _ in range(5)]
    _check(longer == dec._extract(lag, 256, nb), "R10 truncation")
    # table index bounds over every lag index and block length
    for flb in (192, 240, 256, 384, 512, 768, 960, 1024):
        for li in range(510):
            t0 = 640 * 2.0 ** ((li - 509) / 170.0) / flb
            ks = 1 if t0 > 81 / 32 else 0
            period = t0 - ks
            ti = 0 if period <= 9 / 32 else math.floor(16 * period + 0.5) - 4
            _check(0 <= ti <= 36, f"tab_idx {ti} for lag {li}, n_mdct {flb}")
    return "extractor linear, zero-preserving, R10 padding/truncation; tab_idx within 0..36"


SELFTESTS = (
    test_tables,
    test_fixed_point,
    test_db_maps,
    test_heuristic,
    test_coef_cdf,
    test_arithmetic_coder,
    test_rng,
    test_predictor_block,
    test_end_to_end,
)


def run_selftest() -> int:
    failed = 0
    for fn in SELFTESTS:
        try:
            note = fn()
        except Exception as e:  # report every failure, then exit non-zero
            failed += 1
            print(f"FAIL {fn.__name__}: {type(e).__name__}: {e}")
        else:
            print(f"ok   {fn.__name__}: {note}")
    print("selftest FAILED" if failed else "selftest passed")
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--spec-dir", help=f"directory (or file) holding {TABLES_C_NAME}")
    sub = parser.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("selftest", help="run the unit tests")
    v = sub.add_parser("vectors", help="write random-bitstream test vectors")
    for sp in (st, v):  # --spec-dir is accepted before or after the sub-command
        sp.add_argument("--spec-dir", default=argparse.SUPPRESS, help=argparse.SUPPRESS)
    v.add_argument("--seed", type=int, default=1)
    v.add_argument("--cases", type=int, default=16)
    v.add_argument("--frames", type=int, default=6)
    v.add_argument("--attempts", type=int, default=300, help="retries per case for valid frames")
    v.add_argument("--out", required=True)
    v.add_argument("--trace", help="also write the R15 trace records of every ok frame here")
    args = parser.parse_args(argv)
    try:
        load_tables(args.spec_dir)
    except (FileNotFoundError, ValueError, OSError) as e:
        print(f"ssf_ref: cannot load the Annex C tables: {e}", file=sys.stderr)
        return 2
    if args.cmd == "selftest":
        return run_selftest()
    text, st = make_vectors(args.seed, args.cases, args.frames, args.attempts)
    Path(args.out).write_text(text, encoding="ascii")
    print(f"wrote {args.out}: {args.cases} cases, seed {args.seed}")
    print(
        f"random frames valid: {st['valid']}/{st['attempts']} = "
        f"{st['valid'] / max(1, st['attempts']):.1%}"
        f" (first frame of a case {st['first'][1]}/{st['first'][0]} = "
        f"{st['first'][1] / max(1, st['first'][0]):.1%}, later frames {st['later'][1]}/"
        f"{st['later'][0]} = {st['later'][1] / max(1, st['later'][0]):.1%})"
    )
    print(
        f"valid random frames whose arithmetic decoder broke offset < range: "
        f"{st['malformed_valid']}/{st['valid']} (kept only in 'wrap' cases); cases by kind "
        f"{st['kinds']}"
    )
    print(
        f"cases with all {args.frames} frames valid: {st['cases_full']}/{args.cases}; "
        f"with >= {min(3, args.frames)} valid frames: {st['cases_3valid']}/{args.cases}"
    )
    for flb, (n, ok) in sorted(st["base"].items()):
        print(f"  frame_len_base {flb}: {ok}/{n} random frames valid = {ok / n:.1%}")
    for why, n in sorted(st["reasons"].items(), key=lambda kv: -kv[1]):
        print(f"  error {why!r}: {n}")
    if args.trace:
        Path(args.trace).write_text(trace_text(text), encoding="ascii")
    return 0


try:
    load_tables()
except (FileNotFoundError, ValueError, OSError) as _e:  # --spec-dir may supply it later
    _STATE["error"] = _e

if __name__ == "__main__":
    sys.exit(main())
