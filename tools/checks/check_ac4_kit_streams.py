#!/usr/bin/env python3
"""Checks forge's AC-4 decode against the test streams of Dolby's AC-4 Online Delivery Kit.

The kit's elementary streams (Audio_ID_*, Holi_*) are material Dolby made to verify playback
and content-creation products. No reference PCM comes with them and no independent AC-4 decoder
is installed here, so this does not score the decode against a known answer. It holds the decode
to everything the streams themselves and their siblings make checkable:

  framing    the stream is a whole number of sync frames with nothing after the last one, and
             carries no content-signature or protection marker (docs/library/signing.md);
  decode     forge decodes every frame, the sample count follows the frame rate, the channels
             are those the presentation names, in the order the decode reports, nothing is NaN
             or clips, and the programme is not silent;
  channels   the LFE holds only low frequencies, a 5.1.4 stream's height channels carry the
             programme, a stereo stream is not dual mono;
  loudness   the integrated loudness (ITU-R BS.1770-4) of a 2.0 or 5.1 decode is close to the
             LKFS the stream declares;
  siblings   the 25 fps and 29.97 fps versions of a programme level-match; the two languages of
             a programme (Holi_en, Holi_fr) share every channel but the centre; the DD+ version
             of a programme (Audio_ID_6ch_256kbps_ddp.ec3) shows the same timeline, centre and
             LFE as the AC-4 one.

Not committed anywhere: the kit is Dolby's. Point ICLFORGE_DOLBY_AC4_KIT_STREAMS at a directory
holding the kit's Test_Signals/elementary_streams/Audio files (*.ac4, and *.ec3 for the DD+
sibling) and ICLFORGE_CLI (or --forge) at a forge binary. With no directory this prints SKIP and
exits 0. numpy is needed; without it that is a SKIP too.

    python3 tools/checks/check_ac4_kit_streams.py [--dir DIR] [--forge PATH] [--work-dir DIR]

Exit 1 when a check fails. A NOTE is a measurement printed for a person to read.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

try:
    import numpy as np
except ImportError:  # main() reports a SKIP; the pure helpers below need numpy
    np = None

ENV_DIR = "ICLFORGE_DOLBY_AC4_KIT_STREAMS"
ENV_CLI = "ICLFORGE_CLI"

# Thresholds, each with the measurement that set it (kit v1.5 streams, forge 2026-10-29).
LFE_HIGH_BAND_HZ = 200.0
LFE_HIGH_BAND_MAX_FRACTION = 1e-3  # measured 0.0 % above 200 Hz on every 5.1 and 5.1.4 stream
ACTIVE_BELOW_PEAK_DB = 35.0  # a channel is active in a window within 35 dB of the loudest window
HEIGHT_ACTIVE_MIN_FRACTION = 0.25  # the heights of 5.1.4 are active in 80 % of the windows
LOUDNESS_TOLERANCE_LU = 3.0
RATE_PAIR_LEVEL_TOLERANCE_DB = 1.5
LANGUAGE_SHARED_MIN_CORRELATION = 0.98
LANGUAGE_CENTRE_MAX_CORRELATION = 0.9
DDP_CENTRE_MIN_CORRELATION = 0.99
DDP_LFE_MIN_CORRELATION = 0.98
DDP_TIMELINE_MIN_AGREEMENT = 0.9

MARKERS = (
    b"b715f9db-6b41-403d-8bbe-429f3209ca2e",
    bytes.fromhex("b715f9db6b41403d8bbe429f3209ca2e"),
    b"pace-ccjf1",
    b"ecc_r_value",
    b"ContentSignature",
)


# --- sync frames -------------------------------------------------------------------------------


@dataclass
class SyncWalk:
    frames: list[bytes] = field(default_factory=list)  # each raw_ac4_frame
    forms: set[tuple[int, int]] = field(default_factory=set)  # (sync word low byte, header bytes)
    leftover: int = 0  # bytes after the last whole frame
    id3_tags: int = 0


def walk_sync_frames(data: bytes, skip_id3: bool = False) -> SyncWalk:
    """Walks AC-4 sync frames (ETSI TS 103 190-1 Annex C): 0xAC40 or 0xAC41, a 2-byte frame_size
    or 0xFFFF and 3 more, the raw frame, and for 0xAC41 a 16-bit CRC the size does not count.
    skip_id3 passes an ID3v2 tag between frames, as HLS packed audio has at each segment."""
    walk = SyncWalk()
    p = 0
    while p + 4 <= len(data):
        if skip_id3 and data[p : p + 3] == b"ID3" and p + 10 <= len(data):
            size = (data[p + 6] << 21) | (data[p + 7] << 14) | (data[p + 8] << 7) | data[p + 9]
            walk.id3_tags += 1
            p += 10 + size
            continue
        if data[p] != 0xAC or data[p + 1] not in (0x40, 0x41):
            break
        size = (data[p + 2] << 8) | data[p + 3]
        header = 4
        if size == 0xFFFF:
            if p + 7 > len(data):
                break
            size = (data[p + 4] << 16) | (data[p + 5] << 8) | data[p + 6]
            header = 7
        crc = 2 if data[p + 1] == 0x41 else 0
        end = p + header + size + crc
        if end > len(data):
            break
        walk.frames.append(data[p + header : p + header + size])
        walk.forms.add((data[p + 1], header))
        p = end
    walk.leftover = len(data) - p
    return walk


def exact_frame_rate(printed: float) -> float:
    """forge prints a frame rate to three decimals; the NTSC rates are 30000/1001 and kin."""
    for ntsc in (24000 / 1001, 30000 / 1001, 48000 / 1001, 60000 / 1001, 120000 / 1001):
        if abs(printed - ntsc) < 0.005:
            return ntsc
    return printed


def is_subsequence(part: list[str], whole: list[str]) -> bool:
    it = iter(whole)
    return all(any(item == other for other in it) for item in part)


def find_markers(data: bytes) -> list[str]:
    return [m.decode("ascii", "replace") if m.isascii() else m.hex() for m in MARKERS if m in data]


# --- WAV and signal helpers (numpy imported on use) ----------------------------------------------


def read_wav(path: Path):
    """(samples as float64 [frames, channels], rate, format tag, bits, channel mask)."""
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path.name}: not a RIFF/WAVE file")
    p = 12
    fmt = None
    payload = None
    while p + 8 <= len(data):
        chunk = data[p : p + 4]
        size = struct.unpack("<I", data[p + 4 : p + 8])[0]
        body = data[p + 8 : p + 8 + size]
        if chunk == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            mask = struct.unpack("<I", body[20:24])[0] if len(body) >= 24 else 0
            if tag == 0xFFFE and len(body) >= 26:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, channels, rate, bits, mask)
        elif chunk == b"data":
            payload = body
        p += 8 + size + (size & 1)
    if fmt is None or payload is None:
        raise ValueError(f"{path.name}: no fmt or data chunk")
    tag, channels, rate, bits, mask = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(payload, dtype="<f4").astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(payload, dtype="<i2").astype(np.float64) / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 3).astype(np.int64)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v & 0x800000, v - 0x1000000, v).astype(np.float64) / 8388608.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(payload, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise ValueError(f"{path.name}: unsupported WAV format tag {tag}, {bits} bits")
    n = len(x) // channels
    return x[: n * channels].reshape(n, channels), rate, tag, bits, mask


def window_rms_db(x, rate: int, seconds: float = 0.1):
    """RMS in dBFS of each non-overlapping window, shape [windows, channels]."""
    win = int(rate * seconds)
    n = len(x) // win
    if n == 0:  # shorter than one window: no window to measure
        return np.zeros((0, x.shape[1]))
    rms = np.sqrt((x[: n * win].reshape(n, win, -1) ** 2).mean(axis=1))
    return 20.0 * np.log10(np.maximum(rms, 1e-9))


def activity(x, rate: int, below_peak_db: float = ACTIVE_BELOW_PEAK_DB):
    """Boolean [windows, channels]: a channel is active in a window when it is within
    below_peak_db of the loudest window of any channel."""
    db = window_rms_db(x, rate)
    if db.size == 0:
        return np.zeros(db.shape, dtype=bool)
    return db > db.max() - below_peak_db


def high_band_fraction(signal, rate: int, cutoff_hz: float) -> float:
    """Share of the signal's energy above cutoff_hz (one windowed FFT over a power-of-two span)."""
    n = 1 << (len(signal).bit_length() - 1)
    spectrum = np.abs(np.fft.rfft(signal[:n] * np.hanning(n))) ** 2
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    total = spectrum.sum()
    return float(spectrum[freqs > cutoff_hz].sum() / total) if total > 0 else 0.0


def best_lag(a, b, max_lag: int) -> int:
    """The shift s for which a[s:] best matches b (s < 0: b[-s:] matches a), by FFT correlation."""
    n = min(len(a), len(b))
    fa = np.fft.rfft(a[:n], 2 * n)
    fb = np.fft.rfft(b[:n], 2 * n)
    cc = np.fft.irfft(fa * np.conj(fb))
    idx = np.concatenate([np.arange(0, max_lag), np.arange(2 * n - max_lag, 2 * n)])
    lags = np.concatenate([np.arange(0, max_lag), np.arange(-max_lag, 0)])
    return int(lags[int(np.argmax(cc[idx]))])


def align(a, b, max_lag: int = 8000):
    """a and b trimmed to the span they share once the lag of their channel sums is removed."""
    lag = best_lag(a.sum(axis=1), b.sum(axis=1), max_lag)
    if lag >= 0:
        a = a[lag:]
    else:
        b = b[-lag:]
    n = min(len(a), len(b))
    return a[:n], b[:n], lag


def correlation(u, v) -> float:
    if float(np.std(u)) == 0.0 or float(np.std(v)) == 0.0:
        return 0.0
    return float(np.corrcoef(u, v)[0, 1])


def level_db(signal) -> float:
    return 20.0 * math.log10(max(float(np.sqrt(np.mean(signal**2))), 1e-12))


# ITU-R BS.1770-4 Annex 1: the two-stage K-weighting filter for 48 kHz, the channel weights, the
# 400 ms blocks at 75 % overlap, the -70 LKFS absolute gate and the -10 LU relative gate.
_K_SHELF = (
    (1.53512485958697, -2.69169618940638, 1.19839281085285),
    (1.0, -1.69065929318241, 0.73248077421585),
)
_K_HIGHPASS = ((1.0, -2.0, 1.0), (1.0, -1.99004745483398, 0.99007225036621))
_SURROUND_WEIGHT = 1.41


def _impulse_response(stage, taps: int):
    b, a = stage
    h = np.zeros(taps)
    x1 = x2 = y1 = y2 = 0.0
    for i in range(taps):
        x0 = 1.0 if i == 0 else 0.0
        y0 = b[0] * x0 + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
        h[i] = y0
        x2, x1 = x1, x0
        y2, y1 = y1, y0
    return h


def k_weight(signal):
    h = np.convolve(_impulse_response(_K_SHELF, 4096), _impulse_response(_K_HIGHPASS, 16384))[
        :16384
    ]
    n = len(signal)
    size = 1 << (n + len(h) - 1).bit_length()
    return np.fft.irfft(np.fft.rfft(signal, size) * np.fft.rfft(h, size), size)[:n]


def integrated_loudness(x, rate: int, names: list[str]) -> float | None:
    """LKFS of a 48 kHz decode whose channel names say who is who; None for other rates or when
    a channel's weight is not defined by BS.1770 (heights)."""
    if rate != 48000:
        return None
    weights = []
    for name in names:
        if name == "LFE":
            weights.append(0.0)
        elif name in ("L", "R", "C"):
            weights.append(1.0)
        elif name in ("Ls", "Rs"):
            weights.append(_SURROUND_WEIGHT)
        else:
            return None
    filtered = [k_weight(x[:, c]) if w else None for c, w in enumerate(weights)]
    block = int(0.4 * rate)
    step = int(0.1 * rate)
    n = len(x)
    energies = []
    for start in range(0, n - block + 1, step):
        total = 0.0
        for c, w in enumerate(weights):
            if w:
                total += w * float(np.mean(filtered[c][start : start + block] ** 2))
        energies.append(total)
    z = np.array(energies)
    loud = -0.691 + 10.0 * np.log10(np.maximum(z, 1e-20))
    keep = loud > -70.0
    if not keep.any():
        return None
    relative = -0.691 + 10.0 * math.log10(float(z[keep].mean())) - 10.0
    keep &= loud > relative
    if not keep.any():
        return None
    return -0.691 + 10.0 * math.log10(float(z[keep].mean()))


# --- forge -------------------------------------------------------------------------------------


@dataclass
class Probe:
    frames: int | None = None
    crc_ok: tuple[int, int] | None = None
    fps: float | None = None
    names: list[str] = field(default_factory=list)
    declared_lkfs: float | None = None
    dialnorm: int | None = None


def parse_probe(text: str) -> Probe:
    probe = Probe()
    if m := re.search(r"^access units\s+(\d+)", text, re.M):
        probe.frames = int(m.group(1))
    if m := re.search(r"^CRC\s+(\d+) of (\d+) valid", text, re.M):
        probe.crc_ok = (int(m.group(1)), int(m.group(2)))
    if m := re.search(r"^frame rate\s+([\d.]+) fps", text, re.M):
        probe.fps = float(m.group(1))
    if m := re.search(
        r"^presentation\s+\d+\s+id\s+\d+,\s+md_compat\s+\d+,\s+([A-Za-z0-9 ]+);", text, re.M
    ):
        probe.names = m.group(1).split()
    if m := re.search(r"^loudness\s+(-?[\d.]+) LKFS", text, re.M):
        probe.declared_lkfs = float(m.group(1))
    if m := re.search(r"^dialnorm\s+(-?\d+) dBFS", text, re.M):
        probe.dialnorm = int(m.group(1))
    return probe


def parse_decode_layout(text: str) -> tuple[list[str], int] | None:
    """The channel names and rate in forge decode's 'decoded N frames -> file (L R, 48000 Hz)'."""
    m = re.search(r"->\s+.*\(([A-Za-z0-9 ]+), (\d+) Hz\)", text)
    return (m.group(1).split(), int(m.group(2))) if m else None


@dataclass
class Decoded:
    x: object
    rate: int
    names: list[str]
    mask: int


class Report:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str, str]] = []

    def add(self, status: str, stream: str, check: str, detail: str) -> None:
        self.rows.append((status, stream, check, detail))
        print(f"{status:5s} {stream:38s} {check:22s} {detail}", flush=True)

    @property
    def failed(self) -> int:
        return sum(1 for r in self.rows if r[0] == "FAIL")

    def check(self, ok: bool, stream: str, check: str, detail: str) -> bool:
        self.add("PASS" if ok else "FAIL", stream, check, detail)
        return ok


class Kit:
    def __init__(self, directory: Path, forge: str, work: Path, report: Report) -> None:
        self.directory = directory
        self.forge = forge
        self.work = work
        self.report = report
        self.cache: dict[str, Decoded | None] = {}

    def run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([self.forge, *args], capture_output=True, text=True, check=False)

    def probe(self, path: Path) -> Probe:
        return parse_probe(self.run("probe", str(path)).stdout)

    def decode(self, name: str) -> Decoded | None:
        if name in self.cache:
            return self.cache[name]
        path = self.directory / name
        wav = self.work / (path.stem + ".wav")
        result = self.run("decode", str(path), str(wav))
        text = result.stdout + result.stderr
        decoded = None
        if result.returncode == 0 and wav.exists():
            x, rate, _tag, _bits, mask = read_wav(wav)
            layout = parse_decode_layout(text)
            decoded = Decoded(x, rate, layout[0] if layout else [], mask)
            wav.unlink()
        else:
            last = (text.strip().splitlines() or ["(no message)"])[-1]
            self.report.add("FAIL", name, "decode", f"forge exit {result.returncode}: {last[:120]}")
        self.cache[name] = decoded
        return decoded


# --- the checks ----------------------------------------------------------------------------------


def check_framing(kit: Kit, name: str) -> None:
    data = (kit.directory / name).read_bytes()
    walk = walk_sync_frames(data)
    forms = ", ".join(f"0x{w:02X} with a {h}-byte header" for w, h in sorted(walk.forms))
    kit.report.check(
        walk.leftover == 0 and len(walk.frames) > 0,
        name,
        "framing",
        f"{len(walk.frames)} sync frames ({forms}), {walk.leftover} byte(s) after the last",
    )
    markers = find_markers(data)
    kit.report.check(not markers, name, "no signature marker", ", ".join(markers) or "none found")


def check_stream(kit: Kit, name: str) -> None:
    report = kit.report
    probe = kit.probe(kit.directory / name)
    decoded = kit.decode(name)
    if decoded is None:
        return
    x, rate = decoded.x, decoded.rate
    if probe.crc_ok:
        report.check(
            probe.crc_ok[0] == probe.crc_ok[1],
            name,
            "frame CRCs",
            f"{probe.crc_ok[0]} of {probe.crc_ok[1]} valid",
        )
    if probe.frames and probe.fps:
        fps = exact_frame_rate(probe.fps)
        expected = probe.frames * rate / fps
        report.check(
            abs(len(x) - expected) <= 1,
            name,
            "sample count",
            f"{len(x)} samples, {expected:.1f} expected from {probe.frames} frames "
            f"at {fps:.4f} fps",
        )
    expected_names = probe.names
    in_order = is_subsequence(decoded.names, expected_names)
    report.check(
        bool(decoded.names) and x.shape[1] == len(decoded.names) and in_order,
        name,
        "channels and order",
        f"WAV {x.shape[1]} ch; decode reports {' '.join(decoded.names)}; "
        f"probe presentation {' '.join(expected_names)}",
    )
    if decoded.names != expected_names:
        report.add(
            "NOTE",
            name,
            "probe vs decode layout",
            "probe lists channels the decode does not write: "
            + " ".join(n for n in expected_names if n not in decoded.names),
        )
    finite = bool(np.isfinite(x).all())
    peak = float(np.abs(x).max()) if finite else float("nan")
    report.check(
        finite and peak <= 1.0,
        name,
        "finite, no clipping",
        f"peak {20 * math.log10(max(peak, 1e-12)):.1f} dBFS",
    )
    act = activity(x, rate)
    busy = float(act.any(axis=1).mean())
    report.check(
        busy > 0.5, name, "not silent", f"{100 * busy:.0f} % of 100 ms windows carry signal"
    )
    if decoded.mask == 0:
        report.add(
            "NOTE",
            name,
            "WAV channel mask",
            f"none written for {x.shape[1]} channels (layout is only in forge's message)",
        )
    names = decoded.names or expected_names
    if "LFE" in names:
        lfe = x[:, names.index("LFE")]
        frac = high_band_fraction(lfe, rate, LFE_HIGH_BAND_HZ)
        report.check(
            frac <= LFE_HIGH_BAND_MAX_FRACTION,
            name,
            "LFE band-limited",
            f"{100 * frac:.4f} % of its energy above {LFE_HIGH_BAND_HZ:.0f} Hz, "
            f"level {level_db(lfe):.1f} dBFS",
        )
    heights = [i for i, n in enumerate(names) if n.startswith("T")]
    if heights:
        loud = act.any(axis=1)
        shares = [float(act[loud, i].mean()) for i in heights]
        report.check(
            min(shares) >= HEIGHT_ACTIVE_MIN_FRACTION,
            name,
            "height channels",
            "active in "
            + ", ".join(f"{names[i]} {100 * s:.0f} %" for i, s in zip(heights, shares, strict=True))
            + " of the loud windows",
        )
    if x.shape[1] == 2:
        c = correlation(x[:, 0], x[:, 1])
        report.check(abs(c) < 0.9999, name, "stereo, not dual mono", f"L/R correlation {c:.4f}")
    if probe.declared_lkfs is not None:
        measured = integrated_loudness(x, rate, names)
        if measured is None:
            report.add(
                "NOTE",
                name,
                "loudness",
                f"declared {probe.declared_lkfs} LKFS; no BS.1770 weights for {' '.join(names)}",
            )
        else:
            report.check(
                abs(measured - probe.declared_lkfs) <= LOUDNESS_TOLERANCE_LU,
                name,
                "loudness vs declared",
                f"measured {measured:.1f} LKFS, declared {probe.declared_lkfs} LKFS "
                f"(dialnorm {probe.dialnorm})",
            )


def check_rate_pairs(kit: Kit, names: list[str]) -> None:
    for name in names:
        if "_25fps_" not in name:
            continue
        other = name.replace("_25fps_", "_2997fps_")
        if other not in names:
            continue
        a, b = kit.decode(name), kit.decode(other)
        if a is None or b is None or a.x.shape[1] != b.x.shape[1]:
            continue
        diffs = [abs(level_db(a.x[:, c]) - level_db(b.x[:, c])) for c in range(a.x.shape[1])]
        kit.report.check(
            max(diffs) <= RATE_PAIR_LEVEL_TOLERANCE_DB,
            f"{name[:-4]} / 29.97",
            "25 vs 29.97 levels",
            "per-channel level difference max "
            + f"{max(diffs):.2f} dB ({' '.join(f'{d:.2f}' for d in diffs)})",
        )


def check_languages(kit: Kit, names: list[str]) -> None:
    for name in names:
        if not name.startswith("Holi_en_"):
            continue
        other = name.replace("Holi_en_", "Holi_fr_")
        if other not in names:
            continue
        a, b = kit.decode(name), kit.decode(other)
        if a is None or b is None or a.x.shape != b.x.shape:
            continue
        labels = a.names or [str(c) for c in range(a.x.shape[1])]
        centre = labels.index("C") if "C" in labels else 2
        corr = [correlation(a.x[:, c], b.x[:, c]) for c in range(a.x.shape[1])]
        shared = [corr[c] for c in range(len(corr)) if c != centre]
        detail = " ".join(f"{labels[c]} {corr[c]:.3f}" for c in range(len(corr)))
        kit.report.check(
            min(shared) >= LANGUAGE_SHARED_MIN_CORRELATION
            and corr[centre] <= LANGUAGE_CENTRE_MAX_CORRELATION,
            f"{name[:-4]} / fr",
            "languages differ in C only",
            f"en/fr correlation per channel: {detail}",
        )


def check_ddp(kit: Kit, names: list[str]) -> None:
    pairs = (("Audio_ID_6ch_128kbps_25fps_ac4.ac4", "Audio_ID_6ch_256kbps_ddp.ec3"),)
    for ac4_name, ec3_name in pairs:
        if ac4_name not in names or ec3_name not in names:
            continue
        a, d = kit.decode(ac4_name), kit.decode(ec3_name)
        if a is None or d is None or a.x.shape[1] != d.x.shape[1]:
            continue
        x, y, lag = align(a.x, d.x)
        n = min(len(a.x), len(d.x))
        act_a = activity(a.x[:n], a.rate)
        act_d = activity(d.x[:n], d.rate)
        # the codecs' delays differ by about 40 ms: compare the timelines at one-second grain
        per = 10
        m = (len(act_a) // per) * per
        ta = act_a[:m].reshape(-1, per, act_a.shape[1]).any(axis=1)
        td = act_d[:m].reshape(-1, per, act_d.shape[1]).any(axis=1)
        agree = float((ta == td).mean())
        centre, lfe = 2, 3
        c_corr = correlation(x[:, centre], y[:, centre])
        l_corr = correlation(x[:, lfe], y[:, lfe])
        others = [correlation(x[:, c], y[:, c]) for c in (0, 1, 4, 5)]
        kit.report.check(
            agree >= DDP_TIMELINE_MIN_AGREEMENT
            and c_corr >= DDP_CENTRE_MIN_CORRELATION
            and l_corr >= DDP_LFE_MIN_CORRELATION,
            f"{ac4_name[:-4]} / DD+",
            "AC-4 vs DD+ decode",
            f"lag {lag} samples; timeline agreement {100 * agree:.0f} %; "
            f"centre {c_corr:.4f}, LFE {l_corr:.4f}",
        )
        kit.report.add(
            "NOTE",
            f"{ac4_name[:-4]} / DD+",
            "L R Ls Rs correlation",
            " ".join(f"{c:.3f}" for c in others)
            + " (independent quantisation noise at 128 vs 256 kbps limits these)",
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--dir", default=os.environ.get(ENV_DIR, ""), help=f"the kit's streams (default ${ENV_DIR})"
    )
    parser.add_argument(
        "--forge",
        default=os.environ.get(ENV_CLI, "") or shutil.which("forge") or "",
        help=f"forge (default ${ENV_CLI})",
    )
    parser.add_argument("--work-dir", default="", help="scratch directory for decoded WAVs")
    args = parser.parse_args(argv)
    if not args.dir or not Path(args.dir).is_dir():
        print(f"SKIP: set {ENV_DIR} to a directory holding the kit's elementary-stream Audio files")
        return 0
    if np is None:
        print("SKIP: numpy is not installed")
        return 0
    if not args.forge or not Path(args.forge).exists():
        print(f"SKIP: no forge binary (set {ENV_CLI} or pass --forge)")
        return 0
    directory = Path(args.dir)
    names = sorted(p.name for p in directory.iterdir() if p.suffix in (".ac4", ".ec3"))
    if not names:
        print(f"SKIP: no .ac4 or .ec3 files in {directory}")
        return 0
    with tempfile.TemporaryDirectory(dir=args.work_dir or None) as scratch:
        report = Report()
        kit = Kit(directory, args.forge, Path(scratch), report)
        for name in names:
            if name.endswith(".ac4"):
                check_framing(kit, name)
                check_stream(kit, name)
        check_rate_pairs(kit, names)
        check_languages(kit, names)
        check_ddp(kit, names)
    print(f"\n{len(report.rows)} lines, {report.failed} failed")
    return 1 if report.failed else 0


if __name__ == "__main__":
    sys.exit(main())
