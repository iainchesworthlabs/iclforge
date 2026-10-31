#!/usr/bin/env python3
"""A short Dolby Atmos (E-AC-3 JOC) demo piece, synthesised here from nothing
but numpy and a seed, so the file is ours to share.

    python tools/generators/gen_atmos_demo_track.py --forge <path to forge> --out <dir>

writes into <dir>:

    Iclforge - Atmos Demo.ec3   raw E-AC-3 + JOC, 5.1 core + 9 objects
    Iclforge - Atmos Demo.m4a   the same payload, `-c copy` into MP4, tagged,
                                with a generated cover (what a music library wants)
    atmos-demo-bed-objects.wav  the multichannel source (one channel per object)
    atmos-demo-paths.txt        the object motion, in forge's keyframe columns

The music is a minor-key loop (Am F C G) at 100 bpm: pad, bass, kick, hats,
shaker, an arpeggio, a lead, a bell. forge's E-AC-3 `atmos-encode` builds the
5.1 core from the objects (a map= speaker destination is accepted but does not
feed the core, and the core's LFE comes out empty, lfe_send or not), so every
part is an object; the "bed" parts are static ones at the speakers. Nine:

    0 pad            static, front left          5 arpeggio  circles the room at ear
    1 pad            static, front right                     height, a lap per 9.6 s
    2 rear ambience  static, rear left (+ rain)  6 lead      sweeps front-back over
    3 rear ambience  static, rear right (+ rain)                the ceiling and back
    4 drums + bass   static, front centre        7 bell      circles overhead, the
                                                                opposite way, rising
    8 fly-by         every 4 bars from the rear floor over the room to the front

The boards that decode it hold at most 12 objects (S3 and C6: none, bed only),
so this stays at nine. Dependencies: numpy, scipy, ffmpeg and ffprobe on PATH,
and a built `forge`. The same seed gives the same WAV; the E-AC-3 stream is as
reproducible as forge's encoder, which is deterministic.
"""

import argparse
import pathlib
import struct
import subprocess
import sys
import zlib

import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter

RATE = 48000
BPM = 100
BAR = 4 * 60.0 / BPM  # 2.4 s
BARS = 30  # 72 s
SEED = 20261024
OBJECTS = 9
KBPS = 768
NAME = "Iclforge - Atmos Demo"

CHORDS = [  # (bass root, chord tones) in Hz: Am F C G
    (110.00, [220.00, 261.63, 329.63]),
    (87.31, [174.61, 220.00, 261.63]),
    (130.81, [196.00, 261.63, 329.63]),
    (98.00, [196.00, 246.94, 293.66]),
]


def env_ad(n, attack, decay_tau):
    t = np.arange(n) / RATE
    return np.minimum(1.0, t / attack) * np.exp(-t / decay_tau)


def pluck(freq, dur, tau):
    n = int(dur * RATE)
    t = np.arange(n) / RATE
    s = (
        np.sin(2 * np.pi * freq * t)
        + 0.4 * np.sin(4 * np.pi * freq * t)
        + 0.15 * np.sin(6 * np.pi * freq * t)
    )
    return s * env_ad(n, 0.004, tau)


def place(buf, start_s, sig, gain=1.0):
    i = int(start_s * RATE)
    j = min(len(buf), i + len(sig))
    if i < len(buf):
        buf[i:j] += gain * sig[: j - i]


def lowpass(x, cutoff):
    a = np.exp(-2 * np.pi * cutoff / RATE)
    return lfilter([1 - a], [1, -a], x)


def synth():
    rng = np.random.default_rng(SEED)
    n = int(BARS * BAR * RATE)
    t = np.arange(n) / RATE
    beat = 60.0 / BPM

    pad_l, pad_r = np.zeros(n), np.zeros(n)
    bass, kick, hats = np.zeros(n), np.zeros(n), np.zeros(n)
    arp, lead, bell, shaker, shimmer = (np.zeros(n) for _ in range(5))

    kick_hit = np.sin(
        2
        * np.pi
        * (50 * np.exp(-np.arange(int(0.25 * RATE)) / RATE * 18) * 0.9 + 42)
        * np.arange(int(0.25 * RATE))
        / RATE
    ) * env_ad(int(0.25 * RATE), 0.001, 0.07)
    hat_hit = np.diff(rng.standard_normal(int(0.06 * RATE) + 1)) * env_ad(
        int(0.06 * RATE), 0.0005, 0.012
    )
    shk_hit = np.diff(rng.standard_normal(int(0.09 * RATE) + 1)) * env_ad(
        int(0.09 * RATE), 0.01, 0.03
    )

    for bar in range(BARS):
        root, tones = CHORDS[bar % 4]
        t0 = bar * BAR
        # pad: detuned saws-ish from sines, slow swell per bar
        i, j = int(t0 * RATE), int((t0 + BAR) * RATE)
        tt = t[i:j] - t0
        swell = np.minimum(1, tt / 0.6) * np.minimum(1, (BAR - tt) / 0.5)
        for k, f in enumerate(tones):
            for det, buf in ((0.996, pad_l), (1.004, pad_r)):
                buf[i:j] += (
                    0.11
                    * swell
                    * (
                        np.sin(2 * np.pi * f * det * tt + k)
                        + 0.3 * np.sin(4 * np.pi * f * det * tt)
                    )
                )
        # bass: on beats 1 and 3 (long) plus an off-beat push
        for b, ln in ((0, 0.9), (2, 0.9), (3.5, 0.4)):
            place(bass, t0 + b * beat, pluck(root, ln * beat, 0.35), 0.5)
        for b in range(4):
            place(kick, t0 + b * beat, kick_hit, 0.8)
        for b in range(8):
            place(hats, t0 + b * beat / 2, hat_hit, 0.22 if b % 2 else 0.12)
        # arpeggio: sixteenths up and down the chord, an octave up
        seq = [tones[0], tones[1], tones[2], tones[1]] * 2
        seq = [f * 2 for f in seq]
        for s16, f in enumerate(seq * 2):
            place(arp, t0 + s16 * beat / 4, pluck(f, 0.4, 0.12), 0.35)
        # lead: a held note per half bar, from bar 4 on
        if bar >= 4:
            for h in range(2):
                f = tones[(bar + h) % 3] * 2
                tl = np.arange(int(BAR / 2 * RATE)) / RATE
                v = np.sin(
                    2 * np.pi * f * tl
                    + 0.8 * np.sin(2 * np.pi * 5.2 * tl) * np.minimum(1, tl / 0.4)
                )
                place(
                    lead,
                    t0 + h * BAR / 2,
                    0.3 * v * np.minimum(1, tl / 0.05) * np.minimum(1, (BAR / 2 - tl) / 0.2),
                )
        # bell: every other bar, high and long
        if bar % 2 == 1:
            place(bell, t0 + beat, pluck(tones[2] * 4, 2.0, 0.7), 0.3)
        # shaker on the off-beats from bar 2
        if bar >= 2:
            for b in range(4):
                place(shaker, t0 + (b + 0.5) * beat, shk_hit, 0.5)
        # shimmer: quiet high fifth, whole bar
        sh = (
            np.sin(2 * np.pi * tones[0] * 3 * tt) + np.sin(2 * np.pi * tones[2] * 3 * 1.003 * tt)
        ) * swell
        shimmer[i:j] += 0.08 * sh

    rain_l = lowpass(rng.standard_normal(n), 6000) * 0.05
    rain_r = lowpass(rng.standard_normal(n), 6000) * 0.05
    # the fly-by: filtered noise swept up, one rise every 4 bars from bar 8
    fly = np.zeros(n)
    sweep = int(3.0 * RATE)
    ts = np.arange(sweep) / RATE
    whoosh = lowpass(rng.standard_normal(sweep), 1500) * np.sin(np.pi * ts / 3.0) ** 2
    for bar in range(8, BARS - 1, 4):
        place(fly, bar * BAR, whoosh, 1.2)
    fly = np.diff(fly, prepend=0)  # tilt towards the highs
    fly *= 0.45 / np.abs(fly).max()

    fade = np.minimum(1, t / 0.05) * np.minimum(1, (n / RATE - t) / 1.5)
    # forge's E-AC-3 atmos-encode takes a speaker destination in map= but does not
    # feed the 5.1 core from it: the core is the objects rendered. So everything
    # here is an object; the "bed" parts are static ones placed at the speakers.
    objs = (
        np.stack(
            [
                pad_l * 0.9 + bass * 0.0,  # 0 pad, front left
                pad_r * 0.9,  # 1 pad, front right
                pad_l * 0.4 + rain_l * 2 + shimmer * 0.5,  # 2 rear ambience, left
                pad_r * 0.4 + rain_r * 2 + shimmer * 0.5,  # 3 rear ambience, right
                kick + bass * 0.8 + hats + shaker * 0.6,  # 4 drums + bass, front centre (+LFE)
                arp,  # 5 arpeggio
                lead,  # 6 lead
                bell + shimmer * 0.4,  # 7 bell
                fly,  # 8 fly-by
            ]
        )
        * fade
    )
    objs *= 0.45 / np.sqrt(
        (objs**2).mean()
    )  # loud before the soft ceiling: the 5.1 core is a fold of nine objects
    objs = 0.9 * np.tanh(objs / 0.9)  # soft ceiling, so nothing clips
    objs *= min(1.0, 0.7 / np.abs(objs).max())  # and a hard -3 dBFS peak
    return objs.T.astype(np.float32)


def paths(duration):
    """forge keyframe columns: object_index time_s x y z gain lfe_send.
    x 0 left..1 right, y 0 front..1 back, z -1 floor..+1 ceiling. The index is
    the object's number among the objects, in WAV-channel order, from 0."""
    rows = []

    def circle(idx, period, radius, zfn, gain, direction=1, phase=0.0, step=0.4):
        for k in range(int(duration / step) + 2):
            tm = k * step
            a = phase + direction * 2 * np.pi * tm / period
            rows.append(
                (idx, tm, 0.5 + radius * np.sin(a), 0.5 - radius * np.cos(a), zfn(tm), gain)
            )

    circle(5, 4 * BAR, 0.46, lambda tm: 0.0, 0.9)
    for k in range(int(duration / 0.4) + 2):  # lead: front-back over the ceiling
        tm = k * 0.4
        ph = (tm / (2 * BAR)) % 2
        y = ph if ph <= 1 else 2 - ph
        rows.append(
            (6, tm, 0.5 + 0.25 * np.sin(2 * np.pi * tm / (4 * BAR)), 0.05 + 0.9 * y, 0.9, 0.9)
        )
    circle(
        7,
        3 * BAR,
        0.35,
        lambda tm: 0.65 + 0.3 * np.sin(2 * np.pi * tm / (6 * BAR)),
        0.9,
        direction=-1,
    )
    for bar in range(8, BARS - 1, 4):  # fly-by: rear floor, up and over, to the front ceiling
        rows += [
            (8, bar * BAR - 0.01, 0.5, 0.95, -0.3, 0.0),
            (8, bar * BAR, 0.5, 0.95, -0.3, 1.0),
            (8, bar * BAR + 1.5, 0.5, 0.5, 0.9, 1.0),
            (8, bar * BAR + 3.0, 0.5, 0.05, 0.4, 0.0),
        ]
    rows += [
        (0, 0.0, 0.1, 0.1, 0.0, 0.9),
        (1, 0.0, 0.9, 0.1, 0.0, 0.9),
        (2, 0.0, 0.05, 0.95, 0.0, 0.8),
        (3, 0.0, 0.95, 0.95, 0.0, 0.8),
        (4, 0.0, 0.5, 0.05, 0.0, 0.9),
    ]
    rows.sort(key=lambda r: (r[0], r[1]))
    lines = ["# object_index time_s x y z gain lfe_send  (generated by gen_atmos_demo_track.py)"]
    lines += [f"{i} {t:.3f} {x:.4f} {y:.4f} {z:.4f} {g:.3f} 0.0" for i, t, x, y, z, g in rows]
    return "\n".join(lines) + "\n"


def cover_png(path, size=600):
    """A plain generated cover: dark field, rings in five hues, a dot above."""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32) / size
    r = np.hypot(xx - 0.5, yy - 0.62)
    img = np.zeros((size, size, 3), np.float32) + np.array([0.04, 0.05, 0.10])
    for k, col in enumerate(
        [(0.2, 0.7, 0.9), (0.5, 0.4, 0.9), (0.9, 0.4, 0.6), (0.95, 0.7, 0.3), (0.4, 0.85, 0.5)]
    ):
        m = np.abs(r - (0.08 + 0.07 * k)) < 0.008
        img[m] = col
    dot = np.hypot(xx - 0.5, yy - 0.2) < 0.035
    img[dot] = (1, 1, 1)
    raw = b"".join(
        b"\x00" + (np.clip(img[row], 0, 1) * 255).astype(np.uint8).tobytes() for row in range(size)
    )

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data))

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def run(cmd):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--forge", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--kbps", type=int, default=KBPS)
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    audio = synth()
    wav = out / "atmos-demo-bed-objects.wav"
    wavfile.write(wav, RATE, audio)
    pth = out / "atmos-demo-paths.txt"
    pth.write_text(paths(len(audio) / RATE))

    ec3 = out / (NAME + ".ec3")
    run([a.forge, "atmos-encode", wav, ec3, a.kbps, pth])

    cover = out / "atmos-demo-cover.png"
    cover_png(cover)
    m4a = out / (NAME + ".m4a")
    run(
        [
            "ffmpeg",
            "-y",
            "-loglevel",
            "error",
            "-f",
            "eac3",
            "-i",
            ec3,
            "-i",
            cover,
            "-map",
            "0:a",
            "-map",
            "1:v",
            "-c",
            "copy",
            "-disposition:v:0",
            "attached_pic",
            "-metadata",
            "title=Atmos Demo",
            "-metadata",
            "artist=Iclforge",
            "-metadata",
            "album_artist=Iclforge",
            "-metadata",
            "album=Atmos Demo",
            "-metadata",
            "date=2026",
            "-metadata",
            "track=1/1",
            "-metadata",
            "genre=Test",
            "-metadata",
            "comment=Synthesised in the iclforge repository; free to share",
            "-f",
            "mp4",
            m4a,
        ]
    )
    print("wrote", ec3, m4a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
