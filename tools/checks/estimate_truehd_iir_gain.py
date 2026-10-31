#!/usr/bin/env python3
"""Estimates what an IIR filter (TH spec section 9.5) would save the TrueHD encoder, on real audio.

The encoder writes no IIR filter (libs/truehd/ERRATA.md question 91). Whether that costs bytes
depends
on the material, so this measures it, without writing a stream: for segments of a WAV file it fits

  * a FIR (filter A) of order 4 and of order 8, by windowed autocorrelation and the Levinson-Durbin
    recursion as the encoder does,
  * a long FIR of order 32, which the format does not allow, as an upper reference,
  * a FIR of order 8 with an IIR (filter B) of order 4, which the format allows, by the two-stage
    least-squares fit of Hannan and Rissanen,

quantises the coefficients as the format asks (one shift `coeff_q` of 8 to 15 for both filters of a
channel, coefficients within +-32767), runs the encoder's recursion in integers and estimates the
bits of each block of residual from its mean absolute value (a Laplacian's entropy). The decoder
keeps the past values of the signal for filter A and the past residuals for filter B (FFmpeg's
mlpdsp.c), so
the encoder's recursion is
  r[n] = x[n] - (sum a[i] x[n-i] + sum b[j] r[n-j]) >> coeff_q
and filter B's
roots must lie inside the unit circle; a fit that does not, or whose coefficients the format cannot
hold, is not used and is counted. The result is a bound on the bits of the residual, not a stream.

Usage: estimate_truehd_iir_gain.py FILE.wav [FILE.wav ...] [--segments N] [--length SAMPLES]
"""

import argparse
import math
import pathlib
import sys
import wave

import numpy as np

BLOCK = 80  # samples whose residual is coded with one codebook, to estimate its bits
CONTEXT = 64  # samples before a segment that the filters' history is taken from


def read_wav(path):
    """(rate, samples as float64 of shape (frames, channels)) for a 16 or 24 bit PCM file."""
    with wave.open(str(path), "rb") as w:
        rate, channels, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = np.frombuffer(w.readframes(w.getnframes()), dtype=np.uint8)
    if width == 2:
        data = raw.view("<i2").astype(np.float64)
    elif width == 3:
        b = raw.reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        data = np.where(v & 0x800000, v - (1 << 24), v).astype(np.float64)
    else:
        raise ValueError(f"{path}: {8 * width} bit samples are not read")
    return rate, data.reshape(-1, channels)


def levinson(r, order):
    """Coefficients a[1..order] (x[n] ~ sum a[i] x[n-i]) from the autocorrelation r[0..order]."""
    a = np.zeros(order)
    error = r[0]
    if error <= 0:
        return a
    for i in range(order):
        k = (r[i + 1] - np.dot(a[:i], r[i:0:-1])) / error
        a[:i] = a[:i] - k * a[:i][::-1]
        a[i] = k
        error *= 1.0 - k * k
        if error <= 0:
            break
    return a


def autocorrelation(x, order):
    w = x * np.hanning(len(x))
    full = np.correlate(w, w, mode="full")
    mid = len(w) - 1
    return full[mid : mid + order + 1]


def fit_fir(x, order):
    return levinson(autocorrelation(x, order), order)


def fit_arma(x, p, q, long_order=32):
    """(a, b): x[n] ~ sum a[i] x[n-i] + sum b[j] e[n-j] + e[n], e from a long fit (two stages)."""
    a_long = fit_fir(x, long_order)
    e = x.copy()
    for i in range(1, long_order + 1):
        e[i:] -= a_long[i - 1] * x[:-i]
    start = max(p, q)
    rows = [x[start - i : len(x) - i] for i in range(1, p + 1)]
    rows += [e[start - j : len(e) - j] for j in range(1, q + 1)]
    design = np.stack(rows, axis=1)
    target = x[start:]
    solution, *_ = np.linalg.lstsq(design, target, rcond=None)
    return solution[:p], solution[p:]


def quantise(a, b):
    """(coeff_q, int a, int b), one shift for both filters; None if the format cannot hold them."""
    biggest = max(np.max(np.abs(a), initial=0.0), np.max(np.abs(b), initial=0.0))
    if biggest == 0:
        return 15, np.zeros(len(a), dtype=np.int64), np.zeros(len(b), dtype=np.int64)
    q = min(15, math.floor(math.log2(32767.0 / biggest)))
    if q < 8:
        return None
    return q, np.rint(a * (1 << q)).astype(np.int64), np.rint(b * (1 << q)).astype(np.int64)


def residual(x_int, quantised, context=CONTEXT):
    """The encoder's recursion in integers over x_int[context:], history taken from before it."""
    q, a, b = quantised
    p, nb = len(a), len(b)
    r = np.zeros(len(x_int), dtype=np.int64)
    xs = x_int.tolist()
    al, bl = a.tolist(), b.tolist()
    rs = [0] * len(xs)
    for n in range(context, len(xs)):
        acc = 0
        for i in range(1, p + 1):
            acc += al[i - 1] * xs[n - i]
        for j in range(1, nb + 1):
            acc += bl[j - 1] * rs[n - j]
        rs[n] = xs[n] - (acc >> q)
    r[:] = rs
    return r[context:]


def estimated_bits(r):
    """Bits for a residual: a Laplacian's entropy for each block from its mean absolute value."""
    n = (len(r) // BLOCK) * BLOCK
    blocks = np.abs(r[:n].astype(np.float64)).reshape(-1, BLOCK).mean(axis=1)
    per_sample = np.maximum(0.0, np.log2(2.0 * math.e * np.maximum(blocks, 0.25)))
    return float(per_sample.sum() * BLOCK)


def stable(b):
    if len(b) == 0:
        return True
    return bool(np.all(np.abs(np.roots(np.concatenate(([1.0], b)))) < 0.999))


def measure(x, segments, length, seed=0):
    """Total estimated bits for each model over the segments of one channel, and the counts."""
    totals = {"fir4": 0.0, "fir8": 0.0, "fir32": 0.0, "arma_8_4": 0.0, "best_of_8_and_arma": 0.0}
    used = rejected = skipped = 0
    lo, hi = int(0.1 * len(x)), int(0.9 * len(x)) - length
    starts = np.linspace(lo, max(lo, hi), segments).astype(int)
    for s in starts:
        ext = x[s - CONTEXT : s + length]
        if len(ext) < length + CONTEXT or np.sqrt(np.mean(ext[CONTEXT:] ** 2)) < 16.0:
            skipped += 1
            continue
        ext_int = np.rint(ext).astype(np.int64)
        bits = {}
        for name, order in (("fir4", 4), ("fir8", 8), ("fir32", 32)):
            fitted = quantise(fit_fir(ext, order), np.zeros(0))
            bits[name] = estimated_bits(residual(ext_int, fitted)) if fitted else None
        if bits["fir8"] is None or bits["fir4"] is None or bits["fir32"] is None:
            skipped += 1
            continue
        a, b = fit_arma(ext, 8, 4)
        quantised = quantise(a, b) if stable(b) else None
        if quantised is None:
            rejected += 1
            bits["arma_8_4"] = bits["fir8"]
        else:
            used += 1
            bits["arma_8_4"] = estimated_bits(residual(ext_int, quantised))
        bits["best_of_8_and_arma"] = min(bits["fir8"], bits["arma_8_4"])
        for key in totals:
            totals[key] += bits[key]
    return totals, used, rejected, skipped


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("files", nargs="+", type=pathlib.Path)
    parser.add_argument("--segments", type=int, default=10, help="segments per channel")
    parser.add_argument("--length", type=int, default=5120, help="samples in a segment")
    parser.add_argument("--channels", type=int, default=2, help="how many leading channels to use")
    args = parser.parse_args(argv)
    print(
        "| file | rate | FIR 4 | FIR 8 | FIR 32 (not allowed) | FIR 8 + IIR 4 "
        "| best of FIR 8 and +IIR | segments: IIR used / unstable or out of range / silent |"
    )
    print("|---|---:|---:|---:|---:|---:|---:|---|")
    for path in args.files:
        rate, data = read_wav(path)
        sums = {}
        used = rejected = skipped = 0
        for channel in range(min(args.channels, data.shape[1])):
            totals, u, r, s = measure(data[:, channel], args.segments, args.length)
            for key, value in totals.items():
                sums[key] = sums.get(key, 0.0) + value
            used, rejected, skipped = used + u, rejected + r, skipped + s
        base = sums["fir8"] or 1.0
        cells = [
            f"{100.0 * sums[k] / base:.1f}"
            for k in ("fir4", "fir8", "fir32", "arma_8_4", "best_of_8_and_arma")
        ]
        print(
            f"| {path.name} | {rate} | "
            + " | ".join(cells)
            + f" | {used} / {rejected} / {skipped} |"
        )
        sys.stdout.flush()
    print("\nBits of the residual as a percentage of FIR order 8's (lower is smaller).")


if __name__ == "__main__":
    main()
