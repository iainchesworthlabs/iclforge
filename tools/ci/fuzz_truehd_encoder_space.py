#!/usr/bin/env python3
"""Property/fuzz harness over the TrueHD encoder's input space.

The question tools/ci/fuzz_ac4_encoder_space.py asks of `ac4-encode`, asked of the TrueHD encoder:
driven across its configuration space by adversarial but valid audio, does it ever write a stream
a reader disagrees with, or refuse or crash on something it should have encoded?

Each case is a pure function of one 64-bit case seed. It draws a configuration the encoder accepts
(rate, 1 to 8 channels, 16, 20 or 24 bit, restart interval, FIR order, matrix search, chunk size,
stereo mode, and the standard downmix where the layout allows it) and a signal chosen to be hard
for a lossless coder: silence, DC, full-scale noise, full-scale squares, alternating extremes,
impulses, one loud channel among silent ones, identical and inverted channels, signals with only
some of their low bits used, a level that jumps inside a restart interval. A length that is not a
whole number of access units is the usual case. Now and then it draws a configuration outside the
encoder's range, which has to be refused with a message and no stream.

The encoder (iclforge-truehd-encode-tool, with its own verification on: the in-repo decoder reads
every unit back) has to succeed, and FFmpeg's strict decode of the stream (-v error -xerror
-err_detect crccheck+bitstream+buffer+explode) has to give back the source exactly. Without
ffmpeg only the first of the two is checked.

Usage (repo root, after building):
  python tools/ci/fuzz_truehd_encoder_space.py --tool build/dev/bin/iclforge-truehd-encode-tool
  python tools/ci/fuzz_truehd_encoder_space.py --seconds 120      # bounded, for CI
  python tools/ci/fuzz_truehd_encoder_space.py --replay 1234567   # one exact case
  python tools/ci/fuzz_truehd_encoder_space.py --regressions      # the seeds that ever failed
"""

import argparse
import concurrent.futures
import importlib.util
import os
import pathlib
import random
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None

HERE = pathlib.Path(__file__).resolve().parent
INTEROP = HERE.parent / "checks" / "verify_truehd_encode_interop.py"

# Case seeds that ever failed, with why; --regressions replays them.
REGRESSION_SEEDS = {
    # The coder tried only the widths a block's spread suggests: a constant first block far from
    # zero (a full-scale square's pair matrix) fitted none, and the retry loop never ended.
    1202854766287567315: "constant block far from zero, 48 kHz stereo",
    943944432613544624: "constant block far from zero, 176.4 kHz stereo",
    1252235890460517299: "constant block far from zero, 88.2 kHz 5 channels",
    8564473343251234302: "constant block far from zero in a downmix, 5.1",
}

RATES = (48000, 48000, 48000, 44100, 96000, 88200, 192000, 176400)
PROFILES = (
    "silence", "dc", "noise", "quiet-noise", "square", "nyquist", "impulses", "one-loud",
    "identical", "inverted", "low-bits", "cliff", "tone", "sweep", "extremes",
)
CHUNKS = (0, 1, 7, 40, 333, 4096)
RESTARTS = (1, 8, 9, 16, 50, 128)
# A configuration outside the encoder's range, and a word the refusal must contain.
BAD_CONFIGS = (
    ("restart=5", "restart"),
    ("restart=2", "restart"),
    ("bits=18", "bits"),
    ("order=9", "order"),
)


def load_interop():
    spec = importlib.util.spec_from_file_location("truehd_interop", INTEROP)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def case_seed(master, index):
    return (master * 6364136223846793005 + (index + 1) * 1442695040888963407) & (2**63 - 1)


@dataclass
class Case:
    seed: int
    rate: int
    channels: int
    bits: int
    seconds: float
    profile: str
    args: list = field(default_factory=list)
    refusal: str = ""  # nonempty: the configuration is outside the range and must be refused


@dataclass
class Result:
    case: Case
    status: str  # ok, refused, fail
    stage: str = ""
    detail: str = ""


def draw_case(seed):
    rng = random.Random(seed)
    rate = rng.choice(RATES)
    high = rate >= 176400
    channels = rng.choice((1, 2, 2, 2, 3, 4, 5, 6, 6, 6, 7, 8) if not high else (1, 2, 2, 3, 4, 5, 6))
    bits = rng.choice((16, 20, 24, 24))
    seconds = rng.choice((0.0012, 0.02, 0.1, 0.25, 0.5017, 0.7))
    if high or channels >= 7:
        seconds = min(seconds, 0.35)
    profile = rng.choice(PROFILES)
    args = []
    restart = rng.choice(RESTARTS)
    if restart != 128:
        args.append(f"restart={restart}")
    if rng.random() < 0.3:
        args.append(f"order={rng.randrange(0, 9)}")
    if rng.random() < 0.15:
        args.append("matrix=0")
    chunk = rng.choice(CHUNKS)
    if chunk:
        args.append(f"chunk={chunk}")
    if channels == 2 and rng.random() < 0.2:
        args.append(f"stereo-mode={rng.choice((1, 2))}")
    if 3 <= channels <= 8 and not high and rng.random() < 0.3:
        args.append("downmix=standard")
    # Adversarial noise at a high rate and channel count is over the 18 Mbps the stream declares:
    # the encoder counts that instead of failing when asked to.
    args.append("rate-violations=1")
    refusal = ""
    if rng.random() < 0.05:
        bad, refusal = rng.choice(BAD_CONFIGS)
        args = [a for a in args if not a.startswith(bad.split("=")[0] + "=")]
        args.append(bad)
    return Case(seed, rate, channels, bits, seconds, profile, args, refusal)


def make_pcm(case):
    """int32 samples (frames, channels), each `case.bits` wide, from the case's own seed."""
    rng = np.random.default_rng(case.seed)
    n = max(1, int(case.rate * case.seconds))
    top = (1 << (case.bits - 1)) - 1
    bottom = -(1 << (case.bits - 1))
    t = np.arange(n)
    out = np.zeros((n, case.channels), dtype=np.int64)
    for c in range(case.channels):
        f = 110.0 * (c + 1) * 1.0597
        wave = np.sin(2 * np.pi * f * t / case.rate)
        if case.profile == "silence":
            pass
        elif case.profile == "dc":
            out[:, c] = (c + 1) * (top // 7) + rng.integers(-2, 3, size=n)
        elif case.profile == "noise":
            out[:, c] = rng.integers(bottom, top + 1, size=n)
        elif case.profile == "quiet-noise":
            out[:, c] = rng.integers(-3, 4, size=n)
        elif case.profile == "square":
            out[:, c] = np.where(wave >= 0, top, bottom)
        elif case.profile == "nyquist":
            out[:, c] = np.where(t % 2 == 0, top, bottom)
        elif case.profile == "impulses":
            out[rng.integers(0, n, size=max(3, n // 3000)), c] = top if c % 2 else bottom
        elif case.profile == "one-loud":
            out[:, c] = rng.integers(bottom, top + 1, size=n) if c == case.channels // 2 else 0
        elif case.profile in ("identical", "inverted"):
            base = np.round(0.4 * top * np.sin(2 * np.pi * 330.0 * t / case.rate))
            out[:, c] = -base if (case.profile == "inverted" and c % 2) else base
        elif case.profile == "low-bits":
            step = 1 << rng.integers(1, max(2, case.bits - 6))
            out[:, c] = (rng.integers(bottom // step, top // step + 1, size=n)) * step
        elif case.profile == "cliff":
            level = np.where((t // max(1, n // 7)) % 2 == 0, 0.9, 0.001)
            out[:, c] = np.round(level * top * wave) + (t > n // 2) * rng.integers(-40, 41, size=n)
        elif case.profile == "tone":
            out[:, c] = np.round(0.5 * top * wave)
        elif case.profile == "sweep":
            out[:, c] = np.round(0.45 * top * np.sin(2 * np.pi * (30 + 300 * t / case.rate) * t / case.rate * (c + 1)))
        elif case.profile == "extremes":
            out[:, c] = rng.choice(np.array([bottom, top, 0, -1, 1]), size=n)
        else:
            raise ValueError(case.profile)
    return np.clip(out, bottom, top).astype(np.int32)


def describe(case):
    return (f"{case.profile} {case.rate} Hz {case.channels} ch {case.bits} bit {case.seconds:g} s "
            f"{' '.join(case.args)}" + (f"  [expects a refusal: {case.refusal}]" if case.refusal else ""))


def run_case(tool, interop, ffmpeg, case, workdir):
    wav = pathlib.Path(workdir) / f"{case.seed}.wav"
    stream = pathlib.Path(workdir) / f"{case.seed}.thd"
    source = make_pcm(case)
    # A 16 bit source is a 16 bit file; a 20 bit one is a 24 bit file whose low 4 bits are clear.
    if case.bits == 16:
        interop.write_wav(wav, case.rate, source, 16)
    else:
        interop.write_wav(wav, case.rate, source << (24 - case.bits), 24)
    bits = 18 if any(a == "bits=18" for a in case.args) else case.bits
    args = [a for a in case.args if not a.startswith("bits=")]
    command = [*tool.split(), str(wav), str(stream), "verify=1", f"bits={bits}", *args]
    try:
        run = subprocess.run(command, capture_output=True, check=False, timeout=300)
    except subprocess.TimeoutExpired:
        return Result(case, "fail", "encode", "the encoder did not finish in 300 s")
    stderr = run.stderr.decode(errors="replace").strip()
    try:
        if case.refusal:
            if 0 < run.returncode < 128 and stderr and not stream.exists():
                return Result(case, "refused")
            return Result(case, "fail", "refusal",
                          "a configuration outside the range was not refused cleanly: "
                          f"exit {run.returncode}, stream {'written' if stream.exists() else 'absent'}, "
                          f"message {stderr!r}")
        if run.returncode != 0:
            return Result(case, "fail", "encode", f"exit {run.returncode}: {stderr}")
        if ffmpeg is None:
            return Result(case, "ok")
        pcm, error = interop.ffmpeg_decode(stream)
        if pcm is None:
            return Result(case, "fail", "ffmpeg", f"strict decode failed: {error}")
        ordered = source[:, interop.FFMPEG_ORDER[case.channels]] if case.channels in interop.FFMPEG_ORDER else source
        expected = (ordered.astype(np.int64) << (32 - case.bits)).astype("<i4").tobytes()
        if pcm != expected:
            return Result(case, "fail", "ffmpeg",
                          f"the decode differs from the source ({len(pcm)} vs {len(expected)} bytes)")
        return Result(case, "ok")
    finally:
        wav.unlink(missing_ok=True)
        stream.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", default=os.environ.get("ICLFORGE_TRUEHD_ENCODE_TOOL",
                                                         "build/dev/bin/iclforge-truehd-encode-tool"),
                        help="the encoder program (or set ICLFORGE_TRUEHD_ENCODE_TOOL)")
    parser.add_argument("--no-ffmpeg", action="store_true", help="skip FFmpeg's strict decode")
    parser.add_argument("--seed", type=int, default=None, help="master seed; drawn and printed if omitted")
    parser.add_argument("--cases", type=int, default=None, help="run exactly this many cases")
    parser.add_argument("--seconds", type=float, default=None, help="run until this many seconds have passed")
    parser.add_argument("--replay", type=int, default=None, help="run the one case with this case seed")
    parser.add_argument("--regressions", action="store_true", help="replay every REGRESSION_SEEDS case")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 2))
    parser.add_argument("--max-failures", type=int, default=10)
    args = parser.parse_args()

    if np is None:
        sys.exit("fuzz_truehd_encoder_space: numpy is required")
    tool = args.tool
    if not shutil.which(tool.split()[0]) and not pathlib.Path(tool.split()[0]).exists():
        sys.exit(f"fuzz_truehd_encoder_space: no encoder program at {tool} (--tool)")
    ffmpeg = None if args.no_ffmpeg or shutil.which("ffmpeg") is None else "ffmpeg"
    interop = load_interop()

    def one(seed):
        with tempfile.TemporaryDirectory(prefix="thdspace_") as workdir:
            case = draw_case(seed)
            result = run_case(tool, interop, ffmpeg, case, workdir)
        print(f"{'ok  ' if result.status != 'fail' else 'FAIL'} {describe(case)}")
        if result.status == "fail":
            print(f"  [{result.stage}] {result.detail}")
        return result.status != "fail"

    if args.regressions:
        if not REGRESSION_SEEDS:
            print("no regression seeds recorded")
        sys.exit(1 if sum(not one(seed) for seed in REGRESSION_SEEDS) else 0)
    if args.replay is not None:
        sys.exit(0 if one(args.replay) else 1)

    if args.cases is None and args.seconds is None:
        args.cases = 40
    master = args.seed if args.seed is not None else random.SystemRandom().randrange(2**63)
    print(f"TrueHD encoder-space fuzz: tool={tool} ffmpeg={ffmpeg or 'off'}")
    print(f"master seed {master}, "
          + (f"{args.cases} cases" if args.cases is not None else f"{args.seconds:g}s budget")
          + f", {args.jobs} jobs")
    print("(every failure prints its case seed; --replay <seed> reruns it)\n")

    started = time.monotonic()
    counts = {"ok": 0, "refused": 0, "fail": 0}
    failures = []
    index = 0

    def budget_left():
        if args.cases is not None:
            return index < args.cases
        return (time.monotonic() - started) < args.seconds

    with (tempfile.TemporaryDirectory(prefix="thdspace_") as workdir,
          concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool):
        pending = set()
        while (budget_left() or pending) and len(failures) < args.max_failures:
            while budget_left() and len(pending) < args.jobs * 2:
                case = draw_case(case_seed(master, index))
                index += 1
                pending.add(pool.submit(run_case, tool, interop, ffmpeg, case, workdir))
            if not pending:
                break
            done, pending = concurrent.futures.wait(pending, return_when=concurrent.futures.FIRST_COMPLETED)
            for future in done:
                result = future.result()
                counts[result.status] += 1
                if result.status == "fail":
                    failures.append(result)
                    print(f"FAIL [{result.stage}] {describe(result.case)}")
                    print(f"  {result.detail}")
                    print("  replay: python tools/ci/fuzz_truehd_encoder_space.py "
                          f"--replay {result.case.seed}\n")

    total = sum(counts.values())
    print(f"{total} cases in {time.monotonic() - started:.1f}s: {counts['ok']} encoded and read back "
          f"exactly, {counts['refused']} refused as intended, {counts['fail']} failed")
    if counts["fail"]:
        sys.exit(1)
    if total == 0 or counts["ok"] < total * 0.5:
        print("too few cases encoded to call this a pass")
        sys.exit(1)


if __name__ == "__main__":
    main()
