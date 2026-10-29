#!/usr/bin/env python3
"""Holds the TrueHD encoder to FFmpeg's strict decode, and to FFmpeg's own encoder for size.

For each case of a matrix of rates, channel counts, widths and signals (real music, speech and
5.1 programme material from testdata/audio, tones, noise, silence, full-scale squares, impulses):

  * the source PCM is written as a WAV file and encoded by the program given with --encoder (run
    as `PROGRAM in.wav out.thd [ARGS]`; iclforge-truehd-encode-tool is one), with its own
    verification on (the in-repo decoder reads every unit back);
  * FFmpeg's strict decode of the stream (-v error -xerror -err_detect
    crccheck+bitstream+buffer+explode) must give back the source samples exactly. The codec is
    lossless, so this is a comparison of bytes, not an SNR;
  * where FFmpeg's TrueHD encoder writes the same layout, its stream's size is recorded beside
    ours, and --max-size-ratio turns a larger stream into a failure.

Channel order: FFmpeg's decoder may return the channels of a layout in its own order; a case whose
only difference is a permutation of channels is reported as such, with the permutation, and fails
unless --allow-permutation is given.

Exit status 1 on any failing case. Skipped with a notice when ffmpeg is not installed, unless
--require-ffmpeg.
"""

import argparse
import hashlib
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None

REPO = pathlib.Path(__file__).resolve().parents[2]
AUDIO = REPO / "testdata" / "audio"
STRICT = ["-v", "error", "-xerror", "-err_detect", "crccheck+bitstream+buffer+explode"]
# FFmpeg returns the channels of a layout in the order of its own channel bits: for a 7 or 8 channel
# TrueHD stream (L R C [LFE] Ls Rs Lrs Rrs) the back pair comes before the side pair. Entry k lists
# the stream channel FFmpeg's output column k holds (ERRATA.md, entry 33).
FFMPEG_ORDER = {7: [0, 1, 2, 5, 6, 3, 4], 8: [0, 1, 2, 3, 6, 7, 4, 5]}


def write_wav(path, rate, samples, container_bits):
    """samples: int array (frames, channels) of values that fit container_bits."""
    frames, channels = samples.shape
    width = container_bits // 8
    flat = samples.astype("<i4").reshape(-1)
    if container_bits == 16:
        body = flat.astype("<i2").tobytes()
    else:
        raw = flat.astype("<i4").tobytes()
        body = b"".join(raw[i : i + 3] for i in range(0, len(raw), 4))
    header = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * width,
                                    channels * width, container_bits)
    header += b"data" + struct.pack("<I", len(body))
    path.write_bytes(header + body)


def ffmpeg_decode(path):
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", *STRICT, "-f", "truehd", "-i", str(path), "-f", "s32le", "-"],
        capture_output=True, check=False)
    if result.returncode != 0:
        return None, result.stderr.decode(errors="replace").strip()
    return result.stdout, ""


def ffmpeg_reference_size(wav, scratch):
    out = scratch / "reference.thd"
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(wav), "-c:a", "truehd",
         "-strict", "-2", "-f", "truehd", str(out)], capture_output=True, check=False)
    if result.returncode != 0 or not out.exists():
        return None
    return out.stat().st_size


def load_flac(name, rate, channels, seconds, offset):
    """Real material from testdata/audio, as int32 in the 24 bit domain."""
    cmd = ["ffmpeg", "-v", "error", "-i", str(AUDIO / name), "-ss", str(offset), "-t", str(seconds),
           "-ar", str(rate), "-f", "s32le", "-"]
    data = subprocess.run(cmd, capture_output=True, check=True).stdout
    pcm = np.frombuffer(data, dtype="<i4").reshape(-1, 2) >> 8
    if channels == 1:
        return pcm[:, :1].copy()
    return pcm.copy()


def load_reference_51(rate, seconds, offset):
    cmd = ["ffmpeg", "-v", "error", "-i", str(AUDIO / "reference_51.wav"), "-ss", str(offset),
           "-t", str(seconds), "-ar", str(rate), "-f", "s32le", "-"]
    data = subprocess.run(cmd, capture_output=True, check=True).stdout
    return (np.frombuffer(data, dtype="<i4").reshape(-1, 6) >> 8).copy()


def synth(kind, rate, channels, seconds, seed):
    rng = np.random.default_rng(seed)
    n = int(rate * seconds)
    t = np.arange(n) / rate
    out = np.zeros((n, channels), dtype=np.int64)
    for c in range(channels):
        f = 220.0 * (c + 1) * 1.0597
        if kind == "tone":
            out[:, c] = np.round(0.5 * (1 << 23) * np.sin(2 * np.pi * f * t))
        elif kind == "noise":
            out[:, c] = rng.integers(-(1 << 22), 1 << 22, size=n)
        elif kind == "quiet-noise":
            out[:, c] = rng.integers(-40, 40, size=n)
        elif kind == "silence":
            pass
        elif kind == "square":
            out[:, c] = np.where(np.sin(2 * np.pi * f * t) >= 0, (1 << 23) - 1, -(1 << 23))
        elif kind == "impulses":
            out[rng.integers(0, n, size=max(4, n // 5000)), c] = (1 << 22) * (1 if c % 2 else -1)
        elif kind == "dc":
            out[:, c] = 1000 * (c + 1) + rng.integers(-3, 4, size=n)
        elif kind == "sweep":
            out[:, c] = np.round(0.4 * (1 << 23) * np.sin(2 * np.pi * (50 + 400 * t) * t * (c + 1)))
        elif kind == "mixed":
            # A tone whose level steps and a noise burst: changes character inside a restart interval.
            level = np.where((t * 3).astype(int) % 2 == 0, 0.6, 0.002)
            burst = (rng.integers(-(1 << 20), 1 << 20, size=n) * (t > seconds * 0.5)).astype(np.int64)
            out[:, c] = np.round(level * (1 << 23) * np.sin(2 * np.pi * f * t)) + burst
        else:
            raise ValueError(kind)
    return np.clip(out, -(1 << 23), (1 << 23) - 1).astype(np.int32)


def to_bits(pcm24, bits):
    """Reduce a 24 bit signal to `bits` bits by dropping the low bits (a 16 bit source)."""
    return (pcm24 >> (24 - bits)).astype(np.int32)


def cases(full):
    quick = [
        # name, source, rate, channels, bits, seconds, extra encoder args
        ("music-st48-s24", ("flac", "programme_music_stereo.flac"), 48000, 2, 24, 4.0, []),
        ("music-st48-s16", ("flac", "programme_music_stereo.flac"), 48000, 2, 16, 4.0, []),
        ("speech-mono48-s24", ("flac", "programme_speech_stereo.flac"), 48000, 1, 24, 4.0, []),
        ("music-st44-s24", ("flac", "programme_music_stereo.flac"), 44100, 2, 24, 3.0, []),
        ("music-st96-s24", ("flac", "programme_music_stereo.flac"), 96000, 2, 24, 3.0, []),
        ("music-st192-s24", ("flac", "programme_music_stereo.flac"), 192000, 2, 24, 2.0, []),
        ("ref51-48-s24", ("ref51",), 48000, 6, 24, 4.0, []),
        ("ref51-48-s16", ("ref51",), 48000, 6, 16, 4.0, []),
        ("ref51-96-s24", ("ref51",), 96000, 6, 24, 2.0, []),
        ("synth-71-48-s24", ("synth", "mixed"), 48000, 8, 24, 3.0, []),
        ("synth-st48-noise", ("synth", "noise"), 48000, 2, 24, 2.0, []),
        ("synth-st48-tone", ("synth", "tone"), 48000, 2, 24, 2.0, []),
        ("synth-st48-silence", ("synth", "silence"), 48000, 2, 24, 1.0, []),
        ("synth-st48-square", ("synth", "square"), 48000, 2, 24, 1.0, []),
        ("synth-st48-impulses", ("synth", "impulses"), 48000, 2, 24, 2.0, []),
        ("synth-st48-dc", ("synth", "dc"), 48000, 2, 24, 1.0, []),
        ("synth-st48-quiet", ("synth", "quiet-noise"), 48000, 2, 24, 2.0, []),
        ("synth-st48-mixed-s20", ("synth", "mixed"), 48000, 2, 20, 2.0, []),
        ("synth-3ch-48", ("synth", "tone"), 48000, 3, 24, 1.0, []),
        ("synth-5ch-48-s16", ("synth", "sweep"), 48000, 5, 16, 1.5, []),
        ("synth-st48-restart8", ("synth", "mixed"), 48000, 2, 24, 2.0, ["restart=8"]),
        ("synth-st48-restart1", ("synth", "tone"), 48000, 2, 24, 0.2, ["restart=1"]),
        ("synth-st48-order0", ("synth", "tone"), 48000, 2, 24, 1.0, ["order=0"]),
        ("synth-st48-nomatrix", ("synth", "tone"), 48000, 2, 24, 1.0, []),
        ("synth-st48-oddlength", ("synth", "mixed"), 48000, 2, 24, 0.5017, []),
        ("synth-st48-tiny", ("synth", "noise"), 48000, 2, 24, 0.0009, []),
    ]
    if not full:
        return quick
    extra = [
        ("music-st48-s24-long", ("flac", "programme_music_stereo.flac"), 48000, 2, 24, 20.0, []),
        ("ref51-44-s24", ("ref51",), 44100, 6, 24, 3.0, []),
        ("synth-71-96-s24", ("synth", "noise"), 96000, 8, 24, 1.0, []),
        ("synth-6ch-192", ("synth", "mixed"), 192000, 6, 24, 1.0, ["rate-violations=1"]),
        ("synth-71-44-s16", ("synth", "mixed"), 44100, 8, 16, 1.5, []),
        ("synth-st176-s24", ("synth", "mixed"), 176400, 2, 24, 1.0, []),
        ("synth-st88-s24", ("synth", "mixed"), 88200, 2, 24, 1.0, []),
        ("synth-7ch-48", ("synth", "sweep"), 48000, 7, 24, 1.0, []),
        ("synth-mono-96", ("synth", "sweep"), 96000, 1, 24, 1.0, []),
    ]
    return quick + extra


def build_source(spec, rate, channels, bits, seconds, seed):
    kind = spec[0]
    if kind == "flac":
        pcm = load_flac(spec[1], rate, channels, seconds, offset=2)
    elif kind == "ref51":
        pcm = load_reference_51(rate, seconds, offset=1)
    else:
        pcm = synth(spec[1], rate, channels, seconds, seed)
    pcm = pcm[:, :channels]
    if pcm.shape[1] < channels:  # more channels than the material has: mix neighbours
        extra = [np.roll(pcm[:, i % pcm.shape[1]], 37 * (i + 1)) // 3 for i in range(pcm.shape[1], channels)]
        pcm = np.column_stack([pcm, *extra])
    return to_bits(pcm.astype(np.int32), bits)


def find_permutation(expected, got, channels):
    """If `got` is `expected` with the channels reordered, return the order, else None."""
    if len(got) != len(expected) or len(got) % (4 * channels):
        return None
    e = np.frombuffer(expected, dtype="<i4").reshape(-1, channels)
    g = np.frombuffer(got, dtype="<i4").reshape(-1, channels)
    order = []
    for c in range(channels):
        match = [k for k in range(channels) if np.array_equal(g[:, c], e[:, k])]
        if not match:
            return None
        order.append(match[0])
    return order


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--encoder", required=True,
                        help="program run as PROGRAM in.wav out.thd [ARGS] (iclforge-truehd-encode-tool)")
    parser.add_argument("--full", action="store_true", help="the longer matrix")
    parser.add_argument("--only", action="append", default=[], help="run only cases whose name contains this")
    parser.add_argument("--max-size-ratio", type=float, default=0.0,
                        help="fail a case whose stream is larger than this multiple of FFmpeg's")
    parser.add_argument("--allow-permutation", action="store_true")
    parser.add_argument("--require-ffmpeg", action="store_true")
    parser.add_argument("--keep", type=pathlib.Path, help="keep the streams and WAV files here")
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()

    if shutil.which("ffmpeg") is None:
        if args.require_ffmpeg:
            sys.exit("verify_truehd_encode_interop: ffmpeg is required and not installed")
        print("notice: ffmpeg is not installed; nothing to compare with", file=sys.stderr)
        return 0
    if np is None:
        sys.exit("verify_truehd_encode_interop: numpy is required")

    failures = 0
    ran = 0
    totals = [0, 0]
    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = pathlib.Path(args.keep or scratch_name)
        scratch.mkdir(parents=True, exist_ok=True)
        for name, spec, rate, channels, bits, seconds, extra in cases(args.full):
            if args.only and not any(token in name for token in args.only):
                continue
            ran += 1
            problems = []
            note = ""
            source = build_source(spec, rate, channels, bits, seconds, args.seed)
            wav = scratch / f"{name}.wav"
            stream = scratch / f"{name}.thd"
            # A 16 bit source is a 16 bit file; a 20 bit one is a 24 bit file whose low 4 bits are
            # clear, which the encoder tool is told to read as 20 bits wide.
            if bits == 16:
                write_wav(wav, rate, source, 16)
            else:
                write_wav(wav, rate, source << (24 - bits), 24)
            command = [*args.encoder.split(), str(wav), str(stream), "verify=1", f"bits={bits}", *extra]
            run = subprocess.run(command, capture_output=True, check=False)
            if run.returncode != 0:
                problems.append("encoder failed: " + run.stderr.decode(errors="replace").strip())
            else:
                pcm, error = ffmpeg_decode(stream)
                ordered = source[:, FFMPEG_ORDER[channels]] if channels in FFMPEG_ORDER else source
                expected = (ordered.astype(np.int64) << (32 - bits)).astype("<i4").tobytes()
                if pcm is None:
                    problems.append(f"ffmpeg strict decode failed: {error}")
                elif pcm != expected:
                    order = find_permutation(expected, pcm, channels)
                    if order is not None and args.allow_permutation:
                        note = f"channels reordered by ffmpeg: {order}"
                    elif order is not None:
                        problems.append(f"ffmpeg returns the channels reordered: {order}")
                    else:
                        problems.append("ffmpeg's strict decode differs from the source "
                                        f"(sha256 {hashlib.sha256(pcm).hexdigest()[:12]} vs "
                                        f"{hashlib.sha256(expected).hexdigest()[:12]}, "
                                        f"{len(pcm)} vs {len(expected)} bytes)")
                ours = stream.stat().st_size
                reference = ffmpeg_reference_size(wav, scratch) if channels <= 6 else None
                size = f"{ours:>9d} B"
                if reference:
                    ratio = ours / reference
                    totals[0] += ours
                    totals[1] += reference
                    size += f"  ffmpeg {reference:>9d} B  ratio {ratio:6.3f}"
                    if args.max_size_ratio and ratio > args.max_size_ratio:
                        problems.append(f"larger than {args.max_size_ratio:.3f} x ffmpeg's ({ratio:.3f})")
                else:
                    size += "  (ffmpeg's encoder does not write this layout)"
                note = (note + "  " if note else "") + size
            failures += bool(problems)
            print(f"{'FAIL' if problems else 'ok  '} {name}  {note}" +
                  "".join(f"\n       {p}" for p in problems))
    if totals[1]:
        print(f"size against ffmpeg over the cases it writes: {totals[0] / totals[1]:.4f}")
    print(f"{ran} cases, {failures} failing")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
