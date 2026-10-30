#!/usr/bin/env python3
"""Checks forge's decode against Dolby's Spott reference files (audio-only MP4s of E-AC-3 and AC-4).

The files come from Dolby's public test application: one stream each, `ec-3` or `ac-4` in the name,
a MP4 with a single audio track. No reference PCM comes with them, so this does not score the
decode against a known answer. It holds forge to what a decode can be checked against:

  complete   forge exits 0 and decodes every access unit the probe counts, with no frame left
             waiting for an I-frame (a decode that loses frames still exits 0 with one line to say
             so, which is how two of these streams once decoded 4 % of their frames);
  samples    the WAV holds the access units' worth of samples (E-AC-3: 256 per block; AC-4: the
             frame length at the stream's internal rate, converted to 48 kHz);
  channels   the WAV has the channels the decode names, and for AC-4 they are the presentation's, in
             its order;
  signal     nothing is NaN or clips, and the programme is not silent;
  clear      none of the markers of a content signature or of protection (docs/library/signing.md).

and prints as notes what a person should read: the channel count the MP4 sample entry gives (Dolby's
muxer writes 2 in every ec-3 entry, 5.1 and JOC included), and for E-AC-3 without objects the
agreement of the decode with FFmpeg's, at the lag found, which is dither-dependent and no verdict.

Not committed anywhere: the files are Dolby's. Point ICLFORGE_DOLBY_SPOTT_STREAMS at a directory
holding them (*.mp4) and ICLFORGE_CLI (or --forge) at a forge binary. With no directory this
prints SKIP and exits 0. numpy is needed; without it that is a SKIP too.

    python3 tools/checks/check_dolby_spott_streams.py [--dir DIR] [--forge PATH] [--work-dir DIR]

Exit 1 when a check fails.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_ac4_kit_streams as kit

np = kit.np

ENV_DIR = "ICLFORGE_DOLBY_SPOTT_STREAMS"
ENV_CLI = kit.ENV_CLI

SAMPLE_COUNT_TOLERANCE = 2  # samples; the AC-4 frame-rate converter may hold back a couple
E_AC3_BLOCK_SAMPLES = 256
PROTECTION_BOXES = (b"sinf", b"schm", b"schi", b"tenc", b"pssh", b"senc", b"enca", b"frma")


@dataclass
class Container:
    fourcc: str = ""
    samples: int | None = None
    rate: int | None = None
    channels: int | None = None


@dataclass
class Probe:
    codec: str = ""
    container: Container = field(default_factory=Container)
    units: int | None = None
    blocks_per_unit: int | None = None
    frame_samples: int | None = None
    frame_rate_hz: float | None = None
    names: list[str] = field(default_factory=list)
    renders: int | None = None
    joc: bool = False
    crc_ok: tuple[int, int] | None = None


def parse_probe(text: str) -> Probe:
    """The fields of `forge probe` that the checks use, for an E-AC-3 or an AC-4 file."""
    probe = Probe()
    if m := re.search(r"^codec\s+(E-AC-3|AC-4|AC-3)", text, re.M):
        probe.codec = m.group(1)
    if m := re.search(
        r"^container\s+\S+, track \d+, '([^']+)', (\d+) samples, (\d+) Hz, "
        r"(\d+) channel",
        text,
        re.M,
    ):
        probe.container = Container(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)))
    if m := re.search(r"^access units\s+(\d+)", text, re.M):
        probe.units = int(m.group(1))
    if m := re.search(r"^blocks\s+(\d+) per syncframe", text, re.M):
        probe.blocks_per_unit = int(m.group(1))
    if m := re.search(
        r"^frame rate\s+[\d.]+ fps, (\d+) samples a frame at ([\d.]+) Hz", text, re.M
    ):
        probe.frame_samples = int(m.group(1))
        probe.frame_rate_hz = float(m.group(2))
    if m := re.search(r"^renders\s+(\d+) channel", text, re.M):
        probe.renders = int(m.group(1))
    if m := re.search(
        r"^presentation\s+\d+\s+id\s+\d+,\s+md_compat\s+\d+,\s+([A-Za-z0-9 ]+);", text, re.M
    ):
        probe.names = m.group(1).split()
    probe.joc = bool(re.search(r"^JOC\s+(?!absent)\S", text, re.M))
    if m := re.search(r"^CRC\s+(\d+) of (\d+) ", text, re.M):
        probe.crc_ok = (int(m.group(1)), int(m.group(2)))
    return probe


def expected_samples(probe: Probe) -> float | None:
    """Samples at 48 kHz the access units give, or None where the probe does not say."""
    if probe.units is None:
        return None
    if probe.codec == "E-AC-3" and probe.blocks_per_unit:
        return float(probe.units * probe.blocks_per_unit * E_AC3_BLOCK_SAMPLES)
    if probe.codec == "AC-4" and probe.frame_samples and probe.frame_rate_hz:
        return probe.units * probe.frame_samples * 48000.0 / probe.frame_rate_hz
    return None


def parse_decoded_count(text: str) -> int | None:
    """N in forge decode's 'decoded N AC-4 frames' or 'decoded N E-AC-3 access units'."""
    m = re.search(r"decoded (\d+) (?:AC-4 frames|E-AC-3 access units|AC-3 frames)", text)
    return int(m.group(1)) if m else None


def parse_waiting(text: str) -> int:
    """The frames forge decode says waited for an I-frame and produced nothing."""
    m = re.search(r"(\d+) frames? waiting for an I-frame", text)
    return int(m.group(1)) if m else 0


def parse_layout(text: str) -> tuple[list[str], int] | None:
    """The channel names and rate in forge decode's report, for AC-4 ('-> file (L R, 48000 Hz)')
    and for E-AC-3 ('6 channels, 48000 Hz: L R C LFE Ls Rs')."""
    if m := re.search(r"->\s+.*\(([A-Za-z0-9 ]+), (\d+) Hz\)", text):
        return m.group(1).split(), int(m.group(2))
    if m := re.search(r"^\s*\d+ channels?, (\d+) Hz: ([A-Za-z0-9 ]+?)\s*$", text, re.M):
        return m.group(2).split(), int(m.group(1))
    return None


def find_protection(data: bytes) -> list[str]:
    """Signature markers and protection boxes in an MP4 (as text, for the report)."""
    found = kit.find_markers(data)
    found += [box.decode() for box in PROTECTION_BOXES if box in data]
    return found


def overall_snr_db(a, b) -> float:
    """10 log10 of the energy of `a` over the energy of a - b, over the channels they share."""
    n = min(len(a), len(b))
    ref = a[:n]
    err = ref - b[:n]
    return 10.0 * math.log10(max(float(np.sum(ref**2)), 1e-30) / max(float(np.sum(err**2)), 1e-30))


class Run:
    def __init__(self, directory: Path, forge: str, work: Path, report: kit.Report) -> None:
        self.directory = directory
        self.forge = forge
        self.work = work
        self.report = report

    def forge_run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([self.forge, *args], capture_output=True, text=True, check=False)

    def ffmpeg_decode(self, path: Path):
        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg is None:
            return None
        wav = self.work / (path.stem + ".ffmpeg.wav")
        result = subprocess.run(
            [
                ffmpeg,
                "-v",
                "error",
                "-y",
                "-drc_scale",
                "0",
                "-i",
                str(path),
                "-c:a",
                "pcm_f32le",
                str(wav),
            ],
            capture_output=True,
            check=False,
        )
        if result.returncode != 0 or not wav.exists():
            return None
        x = kit.read_wav(wav)[0]
        wav.unlink()
        return x


def check_file(run: Run, name: str) -> None:
    report = run.report
    path = run.directory / name
    probe = parse_probe(run.forge_run("probe", str(path)).stdout)
    wav = run.work / (path.stem + ".wav")
    result = run.forge_run("decode", str(path), str(wav))
    text = result.stdout + result.stderr
    if result.returncode != 0 or not wav.exists():
        last = (text.strip().splitlines() or ["(no message)"])[-1]
        report.add("FAIL", name, "decode", f"forge exit {result.returncode}: {last[:120]}")
        return
    x, rate, _tag, _bits, mask = kit.read_wav(wav)
    wav.unlink()
    decoded = parse_decoded_count(text)
    waiting = parse_waiting(text)
    report.check(
        decoded is not None and decoded == probe.units and waiting == 0,
        name,
        "every unit decoded",
        f"{decoded} of {probe.units} access units decoded, {waiting} waiting for an I-frame",
    )
    expected = expected_samples(probe)
    if expected is not None:
        report.check(
            abs(len(x) - expected) <= SAMPLE_COUNT_TOLERANCE,
            name,
            "sample count",
            f"{len(x)} samples, {expected:.1f} expected from {probe.units} access units",
        )
    layout = parse_layout(text)
    names = layout[0] if layout else []
    # A presentation with objects is rendered to speakers of the decode's choosing (7.1.4 by
    # default), not the presentation's own channels the probe lists, and the renderer is not
    # normative.
    rendered = "rendered to those speakers" in text
    if rendered:
        ok = bool(names) and x.shape[1] == len(names)
        detail = f"WAV {x.shape[1]} ch; decode reports {' '.join(names)} (objects rendered to them)"
    elif probe.codec == "AC-4":
        ok = bool(names) and x.shape[1] == len(names) and kit.is_subsequence(names, probe.names)
        detail = (
            f"WAV {x.shape[1]} ch; decode reports {' '.join(names)}; "
            f"probe presentation {' '.join(probe.names)}"
        )
    else:
        ok = bool(names) and x.shape[1] == len(names) == (probe.renders or len(names))
        detail = (
            f"WAV {x.shape[1]} ch; decode reports {' '.join(names)}; probe renders {probe.renders}"
        )
    report.check(ok, name, "channels", detail)
    if probe.container.channels is not None and probe.container.channels != x.shape[1]:
        report.add(
            "NOTE",
            name,
            "sample entry channels",
            f"the MP4 sample entry says {probe.container.channels}, the decode writes {x.shape[1]}",
        )
    if mask == 0 and x.shape[1] > 2:
        report.add("NOTE", name, "WAV channel mask", f"none written for {x.shape[1]} channels")
    finite = bool(np.isfinite(x).all())
    peak = float(np.abs(x).max()) if finite else float("nan")
    peak_db = 20 * math.log10(max(peak, 1e-12))
    if finite and peak > 1.0 and rendered:
        # Objects summed into speakers can pass full scale in float, and how they are summed is
        # the renderer's own choice: a person reads it, it is no verdict.
        report.add("NOTE", name, "peak over full scale", f"peak {peak_db:.1f} dBFS in the render")
    else:
        report.check(
            finite and peak <= 1.0, name, "finite, no clipping", f"peak {peak_db:.1f} dBFS"
        )
    windows = kit.activity(x, rate).any(axis=1)
    busy = float(windows.mean()) if len(windows) else 0.0
    report.check(
        busy > 0.5, name, "not silent", f"{100 * busy:.0f} % of 100 ms windows carry signal"
    )
    markers = find_protection(path.read_bytes())
    report.check(
        not markers, name, "clear", ", ".join(markers) or "no signature or protection marker"
    )
    if probe.codec == "E-AC-3" and not probe.joc:
        other = run.ffmpeg_decode(path)
        if other is not None and other.shape[1] == x.shape[1]:
            a, b, lag = kit.align(x, other, 2000)
            report.add(
                "NOTE",
                name,
                "FFmpeg agreement",
                f"{overall_snr_db(a, b):.1f} dB overall at lag {lag} (dither makes it a range, "
                "not a verdict)",
            )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--dir", default=os.environ.get(ENV_DIR, ""), help=f"the files (default ${ENV_DIR})"
    )
    parser.add_argument(
        "--forge",
        default=os.environ.get(ENV_CLI, "") or shutil.which("forge") or "",
        help=f"forge (default ${ENV_CLI})",
    )
    parser.add_argument("--work-dir", default="", help="scratch directory for decoded WAVs")
    args = parser.parse_args(argv)
    if not args.dir or not Path(args.dir).is_dir():
        print(f"SKIP: set {ENV_DIR} to a directory holding Dolby's Spott reference files (*.mp4)")
        return 0
    if np is None:
        print("SKIP: numpy is not installed")
        return 0
    if not args.forge or not Path(args.forge).exists():
        print(f"SKIP: no forge binary (set {ENV_CLI} or pass --forge)")
        return 0
    directory = Path(args.dir)
    names = sorted(p.name for p in directory.iterdir() if p.suffix == ".mp4")
    if not names:
        print(f"SKIP: no .mp4 files in {directory}")
        return 0
    with tempfile.TemporaryDirectory(dir=args.work_dir or None) as scratch:
        report = kit.Report()
        run = Run(directory, args.forge, Path(scratch), report)
        for name in names:
            check_file(run, name)
    print(f"\n{len(report.rows)} lines, {report.failed} failed")
    return 1 if report.failed else 0


if __name__ == "__main__":
    sys.exit(main())
