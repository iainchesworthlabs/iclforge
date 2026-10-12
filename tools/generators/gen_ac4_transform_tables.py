"""The inverse transform's tables in double, for the float and fixed-point tiers to keep in flash.

Writes libs/dsp/src/tiered/tables/transform_tables.hpp: for each block length Table 186 lists
at 44.1 or 48 kHz, which are every block of every frame length the decoder has (context.hpp's
frame_len_base: 2048 at frame rate indices 2 and 13, 1920 at 0 and 1, 1536 at 3 and 4, 960 at 5 and
6, 1024 at 7, 768 at 8 and 9, 512 at 10, and 384 at 11 and 12, divided by the high frame rate
mode's factor of 2 or 4, and each halved down to a sixteenth where Table 186 has the length:
2048 to 128, 1920 to 120, 1536 to 96, 1024 to 128, 960 to 120, 768 to 96, 512 to 128 and 384 to
96), the values the decoder otherwise computes when it builds its transforms (libs/dsp/src/tiered/fft.cpp, mdct.cpp,
kbd.cpp and synthesis.cpp), in the order it stores them:

  kFftRootsN    e^(-2 pi i (p k mod n) / n) for each pass of the N/2-point FFT, {re, im};
  kPreTwiddleN  -cos(2 pi (8k + 1) / 16N) and -sin(...) for k < N/2, Pseudocode 60;
  kKbdLeftN     KBD_LEFT(N) with Table 186's alpha.

Each is computed here with the same double operations in the same order as the C++, and the
same C library's cos, sin and sqrt, and written as an exact hexadecimal literal.
libs/dsp/src/tiered/transform_tables.cpp narrows them to the tier's scalar at compile time, as
the runtime code narrows its own, and libs/dsp/tests/tiered/test_transform_tables.cpp holds
every narrowed value equal to what the runtime code computes on the compiler that runs it.

Usage:
    gen_ac4_transform_tables.py [--check]

--check fails, writing nothing, when the header is not what this would write.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _header_gen import wrap, write_or_check

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
HEADER = (
    REPO_ROOT
    / "libs"
    / "dsp"
    / "src"
    / "tiered"
    / "tables"
    / "transform_tables.hpp"
)

# Table 186's lengths at 44.1 and 48 kHz and their alpha (libs/dsp/src/tiered/kbd.cpp, kAlphas):
# the three columns, the 2048-sample frame's, the 1920's and the 1536's.
ALPHA = {
    2048: 3.0, 1920: 3.0, 1536: 3.0,
    1024: 4.0, 960: 4.0, 768: 4.0,
    512: 4.5, 480: 4.5, 384: 4.5,
    256: 5.0, 240: 5.0, 192: 5.0,
    128: 6.0, 120: 6.0, 96: 6.0,
}
LENGTHS = tuple(ALPHA)


def radices(length: int) -> list[int]:
    """fft.cpp's factor(): radix 4 first, then 2, 3 and 5."""
    result: list[int] = []
    for radix in (4, 2, 3, 5):
        while length % radix == 0:
            result.append(radix)
            length //= radix
    if length != 1:
        sys.exit(f"{length} has a prime factor above 5")
    return result


def root(num: int, den: int) -> tuple[float, float]:
    """fft.cpp's root(): -2.0 * pi * (num % den) / den, then cos and sin."""
    angle = -2.0 * math.pi * float(num % den) / float(den)
    return math.cos(angle), math.sin(angle)


def fft_roots(length: int) -> list[float]:
    """The twiddles of Fft(length), pass after pass, as Fft's constructor appends them."""
    values: list[float] = []
    n = length
    for radix in radices(length):
        m = n // radix
        for p in range(m):
            for k in range(radix):
                re, im = root(p * k, n)
                values += (re, im)
        n = m
    return values


def pre_twiddles(length: int) -> list[float]:
    """mdct.cpp's pre_twiddles(), before its cast."""
    values: list[float] = []
    n16 = 16.0 * float(length)
    for k in range(length // 2):
        angle = 2.0 * math.pi * float(8 * k + 1) / n16
        values += (-math.cos(angle), -math.sin(angle))
    return values


def bessel_i0(x: float) -> float:
    """kbd.hpp's bessel_i0()."""
    half = x / 2.0
    term = 1.0
    total = 1.0
    for k in range(1, 500):
        term *= half / float(k)
        add = term * term
        nxt = total + add
        if nxt == total:
            break
        total = nxt
    return total


def kbd_left(length: int, alpha: float) -> list[float]:
    """kbd.cpp's kbd_left()."""
    pi_alpha = math.pi * alpha
    norm = bessel_i0(pi_alpha)
    cumulative: list[float] = []
    running = 0.0
    for p in range(length + 1):
        t = 2.0 * float(p) / float(length) - 1.0
        radicand = max(0.0, 1.0 - t * t)
        running += bessel_i0(pi_alpha * math.sqrt(radicand)) / norm
        cumulative.append(running)
    total = cumulative[length]
    return [math.sqrt(cumulative[n] / total) for n in range(length)]


def literal(value: float) -> str:
    return "0.0" if value == 0.0 and math.copysign(1.0, value) > 0 else float.hex(value)


def table(name: str, values: list[float]) -> list[str]:
    return [
        f"inline constexpr std::array<double, {len(values)}> {name} = {{",
        *wrap([literal(v) for v in values]),
        "};",
        "",
    ]


def render() -> str:
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "",
        "// The inverse transform's tables in double (planning/ac4.md, the decoder's memory).",
        "// GENERATED by tools/generators/gen_ac4_transform_tables.py; do not edit by hand. Read",
        "// only by libs/dsp/src/tiered/transform_tables.cpp, which narrows them to the tier's",
        "// scalar while it compiles.",
        "",
        "namespace iclforge::dsp::tiered::tables {",
        "",
    ]
    for length in LENGTHS:
        lines += table(f"kFftRoots{length}", fft_roots(length // 2))
        lines += table(f"kPreTwiddle{length}", pre_twiddles(length))
        lines += table(f"kKbdLeft{length}", kbd_left(length, ALPHA[length]))
    lines += ["}  // namespace iclforge::dsp::tiered::tables", ""]
    return "\n".join(lines)


def main() -> int:
    return write_or_check(
        __doc__.split("\n\n")[0],
        render,
        HEADER,
        REPO_ROOT,
        "tools/generators/gen_ac4_transform_tables.py",
    )


if __name__ == "__main__":
    sys.exit(main())
