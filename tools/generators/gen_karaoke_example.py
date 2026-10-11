#!/usr/bin/env python3
"""The karaoke example under examples/karaoke/auld-lang-syne/, made from arithmetic.

"Auld Lang Syne": the traditional tune and Robert Burns's words (1788), both in the public
domain. Nothing here is a recording. The parts are synthesised by the code below, one voice at a
time, so that the script is the record of what is in the files and anyone can say where every
sample came from:

  parts/backing.wav            stereo   plucked chords, bass in the middle, chords spread
  parts/melody-guide.wav       mono     a hollow, clarinet-like tone on the tune
  parts/vocal-1-lead.wav       mono     a sung "ah" on the tune, with a little vibrato and breath
  parts/vocal-2-harmony.wav    mono     a darker "oo" a diatonic third below the tune
  auld-lang-syne.lrc           the words, timed to the syllable (Hearth's enhanced LRC)
  auld-lang-syne-lines-only.lrc  the same words timed by the line only
  auld-lang-syne.forgeproj     the Forge GUI project that opens the four parts in the Karaoke flow
  auld-lang-syne.ac3           with --forge: the four parts encoded, 3/2, bsmod 7 (karaoke)

The one verse is eight bars of 4/4 at 120 beats a minute with a beat of pick-up, 16.5 s. Every
part is 16-bit PCM at 48 kHz (a legal AC-3 rate) and is padded with silence to a whole number of
AC-3 frames (516 of 1536 samples), so that the Forge GUI and `forge encode` see the same length.
Five channels of that are 480 kB a second: the verse costs 7.9 MB of parts and 0.8 MB of stream.
A longer piece, or the chorus as well, costs 0.5 MB for every second.

Reproducible to the byte, on any platform: the sample path uses only + - * / and comparisons on
IEEE doubles, which Python does the same everywhere. The C library's sin, cos, exp and pow are not
used, because their last bit is not the same on every C library.

Stdlib only.

Usage (repo root):
    python tools/generators/gen_karaoke_example.py                  write the parts, words, project
    python tools/generators/gen_karaoke_example.py --forge <forge>  and encode the stream with it
    python tools/generators/gen_karaoke_example.py --check          compare, write nothing
    python tools/generators/gen_karaoke_example.py --check --forge <forge>  and the stream too
"""

from __future__ import annotations

import argparse
import array
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "examples" / "karaoke" / "auld-lang-syne"

RATE = 48_000
FRAME = 1536  # samples in one AC-3 frame
BPM = 120
BEAT = RATE * 60 // BPM  # 24000 samples
EIGHTH = BEAT // 2

# What `forge encode` is given: a 3/2 stream at 384 kbit/s, the melody guide where the centre
# goes and the two vocals where the surrounds go (Annex C, Table C.2.1).
BITRATE_KBPS = 384
STEM = "auld-lang-syne"

PART_FILES = {
    "backing": "parts/backing.wav",
    "melody": "parts/melody-guide.wav",
    "vocal1": "parts/vocal-1-lead.wav",
    "vocal2": "parts/vocal-2-harmony.wav",
}


# --- arithmetic that is the same on every platform ---------------------------------------------


def _sin_turns(x: float) -> float:
    """sin(2 pi x), by Taylor series after reducing to a quarter turn. + - * / only."""
    x -= int(x)  # (-1, 1)
    if x > 0.5:
        x -= 1.0
    elif x < -0.5:
        x += 1.0
    if x > 0.25:
        x = 0.5 - x
    elif x < -0.25:
        x = -0.5 - x
    t = x * 6.283185307179586
    t2 = t * t
    term = t
    total = t
    for k in range(1, 12):
        term = -term * t2 / ((2 * k) * (2 * k + 1))
        total += term
    return total


def _cos_turns(x: float) -> float:
    return _sin_turns(x + 0.25)


def _exp_neg(x: float) -> float:
    """exp(-x) for x >= 0, by halving until the series is short, then squaring back."""
    halvings = 0
    while x > 0.0625:
        x *= 0.5
        halvings += 1
    term = 1.0
    total = 1.0
    for k in range(1, 14):
        term = term * -x / k
        total += term
    for _ in range(halvings):
        total *= total
    return total


def _ipow(base: float, exponent: int) -> float:
    result = 1.0
    while exponent > 0:
        if exponent & 1:
            result *= base
        base *= base
        exponent >>= 1
    return result


def _root(target: float, n: int) -> float:
    """The n-th root of target (target in (0, 2]), by bisection on exact multiplications."""
    lo, hi = 0.0, 2.0
    for _ in range(100):
        mid = (lo + hi) * 0.5
        if _ipow(mid, n) < target:
            lo = mid
        else:
            hi = mid
    return (lo + hi) * 0.5


_SEMITONE = _root(2.0, 12)


def hz(midi: int) -> float:
    """Equal temperament, A4 = 440 Hz = midi 69."""
    f = 440.0
    for _ in range(abs(midi - 69)):
        f = f * _SEMITONE if midi > 69 else f / _SEMITONE
    return f


def half_life(samples: int) -> float:
    """The per-sample factor that halves a level every `samples` samples."""
    return _root(0.5, samples)


# --- the score ----------------------------------------------------------------------------------

# One syllable: (text, space after it?, [(midi pitch, length in eighths), ...]). More than one
# note on a syllable is a melisma. F major, 4/4, one beat of pick-up; midi 60 is middle C.
LINES: list[list[tuple[str, bool, list[tuple[int, int]]]]] = [
    [
        ("Should", True, [(60, 2)]),
        ("auld", True, [(65, 3)]),
        ("ac", False, [(65, 1)]),
        ("quain", False, [(65, 2)]),
        ("tance", True, [(69, 2)]),
        ("be", True, [(67, 3)]),
        ("for", False, [(65, 1)]),
        ("got,", False, [(67, 2)]),
    ],
    [
        ("And", True, [(69, 2)]),
        ("ne", False, [(65, 3)]),
        ("ver", True, [(65, 1)]),
        ("brought", True, [(69, 2)]),
        ("to", True, [(72, 2)]),
        ("mind?", False, [(74, 6)]),
    ],
    [
        ("Should", True, [(74, 2)]),
        ("auld", True, [(72, 3)]),
        ("ac", False, [(69, 1)]),
        ("quain", False, [(69, 2)]),
        ("tance", True, [(65, 2)]),
        ("be", True, [(67, 3)]),
        ("for", False, [(65, 1)]),
        ("got,", False, [(67, 2)]),
    ],
    [
        ("And", True, [(69, 2)]),
        ("auld", True, [(65, 3), (62, 1)]),
        ("lang", True, [(62, 2), (60, 2)]),
        ("syne!", False, [(65, 8)]),
    ],
]

# Pick-up + 8 bars, in eighths, and the stream length padded to whole AC-3 frames.
SCORE_EIGHTHS = sum(n for line in LINES for _, _, notes in line for _, n in notes)
SCORE_SAMPLES = SCORE_EIGHTHS * EIGHTH
FRAMES = -(-SCORE_SAMPLES // FRAME)
TOTAL = FRAMES * FRAME

# F major, as pitch classes, for the harmony: a diatonic third below the tune.
_SCALE_PCS = (0, 2, 4, 5, 7, 9, 10)
_SCALE = [12 * octave + pc for octave in range(3, 8) for pc in _SCALE_PCS]


def third_below(midi: int) -> int:
    return _SCALE[_SCALE.index(midi) - 2]


# The chord on each beat, beat 0 being the pick-up; "p" is the position in the bar, and a bass
# note goes on 0 and 2, a chord on 1 and 3.
_PROGRESSION = (
    "C7",
    *("F", "F", "F", "F"),
    *("Bb", "Bb", "C7", "C7"),
    *("F", "F", "F", "F"),
    *("Bb", "Bb", "Bb", "Bb"),
    *("F", "F", "F", "F"),
    *("Bb", "Bb", "C7", "C7"),
    *("F", "F", "C7", "C7"),
    *("F", "F", "F", "F"),
)
_BASS = {"F": 41, "Bb": 46, "C7": 48}
_STAB = {"F": (57, 60, 65), "Bb": (58, 62, 65), "C7": (58, 64, 67)}


# --- the voices ----------------------------------------------------------------------------------


def _add_partial(buf, start, count, freq, amp, decay=1.0, attack=0, release=0):
    """Add amp * sin(2 pi freq t) * decay**t to buf[start:start + count].

    The sine is the two-term recurrence y[n + 1] = 2 cos(w) y[n] - y[n - 1], three operations a
    sample. The note starts from a zero crossing and rises over `attack` samples; its last
    `release` samples fall linearly to nothing."""
    count = min(count, len(buf) - start)
    if count <= 0:
        return
    w = freq / RATE
    k = 2.0 * _cos_turns(w)
    y0 = 0.0
    y1 = _sin_turns(w)
    level = amp
    attack = min(attack, count)
    release = min(release, count - attack)
    i = start
    for j in range(attack):
        buf[i] += level * y0 * ((j + 1) / attack)
        y2 = k * y1 - y0
        y0 = y1
        y1 = y2
        level *= decay
        i += 1
    for _ in range(count - attack - release):
        buf[i] += level * y0
        y2 = k * y1 - y0
        y0 = y1
        y1 = y2
        level *= decay
        i += 1
    for j in range(release):
        buf[i] += level * y0 * (1.0 - (j + 1) / release)
        y2 = k * y1 - y0
        y0 = y1
        y1 = y2
        level *= decay
        i += 1


def render_backing():
    """Left and right: a bass in the middle on beats one and three, a three-note chord on two
    and four with its lowest note left, its highest right and the middle one in both. A pluck is
    partials that fade faster the higher they are."""
    size = TOTAL
    left = [0.0] * size
    right = [0.0] * size
    middle = [0.0] * size
    partials = ((1, 1.0, 0.30), (2, 0.50, 0.20), (3, 0.25, 0.13))
    decays = {seconds: half_life(int(seconds * RATE)) for _, _, seconds in partials}
    for beat, chord in enumerate(_PROGRESSION):
        start = beat * BEAT
        if (beat - 1) % 4 in (0, 2):
            voices = [(_BASS[chord], 0.30, middle)]
            ring = BEAT * 2
        else:
            low, mid, high = _STAB[chord]
            voices = [(low, 0.10, left), (mid, 0.10, middle), (high, 0.10, right)]
            ring = BEAT
        for midi, amp, buf in voices:
            f = hz(midi)
            for mult, weight, seconds in partials:
                # Past five half-lives a partial is below -30 dB of nothing worth rendering.
                length = min(ring, int(seconds * RATE * 5))
                _add_partial(buf, start, length, f * mult, amp * weight, decays[seconds],
                             attack=240, release=480)
    return [
        [a + c for a, c in zip(left, middle, strict=True)],
        [b + c for b, c in zip(right, middle, strict=True)],
    ]


def _syllable_spans():
    """One entry for each syllable: (first sample, samples, [(midi, samples), ...], text,
    space after, line number, first of its line)."""
    spans = []
    at = 0
    for line_index, line in enumerate(LINES):
        for i, (text, space, notes) in enumerate(line):
            length = sum(n for _, n in notes) * EIGHTH
            spans.append((at, length, [(m, n * EIGHTH) for m, n in notes], text, space,
                          line_index, i == 0))
            at += length
    return spans


def render_melody():
    """A hollow tone of the odd partials 1, 3 and 5 on the tune, one note after another, each
    note starting 20 ms in and ending 20 ms short of the next so the notes are separate."""
    out = [0.0] * TOTAL
    for at, _, notes, *_ in _syllable_spans():
        position = at
        for midi, length in notes:
            f = hz(midi)
            for mult, weight in ((1, 1.0), (3, 0.45), (5, 0.2)):
                _add_partial(out, position, length - 960, f * mult, weight, 1.0,
                             attack=960, release=480)
            position += length
    return [out]


# centre Hz, bandwidth Hz, weight: the three formants of each vowel.
_VOWELS = {
    "ah": ((700.0, 80.0, 1.0), (1150.0, 100.0, 0.55), (2650.0, 140.0, 0.25)),
    "oo": ((320.0, 70.0, 1.0), (800.0, 90.0, 0.18), (2250.0, 130.0, 0.05)),
}


def _resonator(centre, bandwidth, weight):
    """(gain, a1, a2) of y[n] = gain x[n] + a1 y[n-1] + a2 y[n-2], a two-pole resonance."""
    r = _exp_neg(3.141592653589793 * bandwidth / RATE)
    theta = centre / RATE
    # The gain that makes the peak of the response about `weight`, whatever the centre.
    return weight * (1.0 - r) * 2.0 * _sin_turns(theta), 2.0 * r * _cos_turns(theta), -r * r


def render_voice(syllables, vowel, vibrato, breath, seed):
    """A sung voice: a smooth pulse train through three formant resonators in parallel.

    `syllables` holds, for each syllable, its notes as (midi, samples). The gate rises over
    25 ms at each syllable and falls over the 60 ms before the next, so the words are separate;
    a second note on a syllable only moves the pitch. The pitch glides to each note, the vibrato
    is a slow sine on it, and the breath is integer pseudo-random noise added to the pulses."""
    n = TOTAL
    env = [0.0] * n
    target = [0.0] * n
    rise = 1200
    fall = 2880
    at = 0
    for notes in syllables:
        length = sum(count for _, count in notes)
        env[at:at + rise] = [j / rise for j in range(rise)]
        env[at + rise:at + length - fall] = [1.0] * (length - fall - rise)
        env[at + length - fall:at + length] = [1.0 - j / fall for j in range(fall)]
        position = at
        for midi, count in notes:
            target[position:position + count] = [hz(midi)] * count
            position += count
        at += length

    (g1, a11, a12), (g2, a21, a22), (g3, a31, a32) = (_resonator(*v) for v in _VOWELS[vowel])
    y11 = y12 = y21 = y22 = y31 = y32 = 0.0
    out = [0.0] * n
    inv_rate = 1.0 / RATE
    glide = 0.004
    lfo_k = 2.0 * _cos_turns(5.4 / RATE)
    lfo0, lfo1 = 0.0, _sin_turns(5.4 / RATE)
    phase = 0.0
    freq = 0.0
    state = seed
    mean = 1.0 / 6.0
    for i in range(at):
        lfo2 = lfo_k * lfo1 - lfo0
        lfo0 = lfo1
        lfo1 = lfo2
        t = target[i]
        freq = t if freq == 0.0 else freq + (t - freq) * glide
        phase += freq * (1.0 + vibrato * lfo1) * inv_rate
        if phase >= 1.0:
            phase -= 1.0
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        x = phase * (1.0 - phase) - mean + breath * ((state >> 8) / 4194304.0 - 1.0) * 0.25
        a = x * g1 + a11 * y11 + a12 * y12
        y12 = y11
        y11 = a
        b = x * g2 + a21 * y21 + a22 * y22
        y22 = y21
        y21 = b
        c = x * g3 + a31 * y31 + a32 * y32
        y32 = y31
        y31 = c
        out[i] = (a + b + c) * env[i]
    return [out]


def render_vocal(harmony):
    syllables = [
        [(third_below(m) if harmony else m, count) for m, count in notes]
        for _, _, notes, *_ in _syllable_spans()
    ]
    if harmony:
        return render_voice(syllables, "oo", vibrato=0.0015, breath=0.30, seed=7919)
    return render_voice(syllables, "ah", vibrato=0.004, breath=0.20, seed=104729)


# Each part is brought to this peak (of full scale) after it is made.
PEAKS = {"backing": 0.45, "melody": 0.30, "vocal1": 0.55, "vocal2": 0.45}


def render_parts():
    parts = {
        "backing": render_backing(),
        "melody": render_melody(),
        "vocal1": render_vocal(harmony=False),
        "vocal2": render_vocal(harmony=True),
    }
    scaled = {}
    for name, channels in parts.items():
        peak = max(abs(s) for channel in channels for s in channel)
        gain = PEAKS[name] / peak
        scaled[name] = [array.array("h", (_to_int16(s * gain) for s in c)) for c in channels]
    return scaled


def _to_int16(x):
    v = int(x * 32767.0 + 0.5) if x >= 0.0 else int(x * 32767.0 - 0.5)
    return max(-32767, min(32767, v))


def wav_bytes(channels):
    count = len(channels)
    frames = len(channels[0])
    interleaved = array.array("h", bytes(2 * count * frames))
    for c, samples in enumerate(channels):
        interleaved[c::count] = samples
    if sys.byteorder == "big":
        interleaved.byteswap()
    data = interleaved.tobytes()
    block = 2 * count
    fmt = struct.pack("<HHIIHH", 1, count, RATE, RATE * block, block, 16)
    return (
        b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE"
        + b"fmt " + struct.pack("<I", len(fmt)) + fmt
        + b"data" + struct.pack("<I", len(data)) + data
    )


# --- the words and the project ------------------------------------------------------------------


def _stamp(centiseconds, tag):
    minutes, rest = divmod(centiseconds, 6000)
    seconds, hundredths = divmod(rest, 100)
    text = f"{minutes:02d}:{seconds:02d}.{hundredths:02d}"
    return f"<{text}>" if tag == "word" else f"[{text}]"


def lrc(words):
    """The lyrics file: one line per sung line; `words` times every syllable, else only lines.
    A time is a whole number of hundredths, since an eighth note is 25 of them."""
    rows = [
        "[ti:Auld Lang Syne]",
        "[ar:Traditional; words by Robert Burns, 1788]",
        "[al:iclforge karaoke example]",
        "[by:tools/generators/gen_karaoke_example.py]",
        f"[length:{TOTAL // RATE // 60:02d}:{TOTAL // RATE % 60:02d}]",
    ]
    eighths = 0
    for line in LINES:
        row = _stamp(eighths * 25, "line")
        for text, space, notes in line:
            if words:
                row += _stamp(eighths * 25, "word")
            row += text + (" " if space else "")
            eighths += sum(n for _, n in notes)
        rows.append(row.rstrip())
    # The last line ends where the music does.
    rows.append(_stamp(SCORE_EIGHTHS * 25, "line"))
    return "\n".join(rows) + "\n"


def project_json():
    """The Forge GUI project: the Karaoke flow's four parts and the encode they make."""
    document = {
        "format": "iclforge-project",
        "version": 1,
        "title": "Auld Lang Syne (karaoke example)",
        "flow": "karaoke",
        "sources": [
            {"path": PART_FILES["backing"], "part": "backing"},
            {"path": PART_FILES["melody"], "part": "melody"},
            {"path": PART_FILES["vocal1"], "part": "vocal1"},
            {"path": PART_FILES["vocal2"], "part": "vocal2"},
        ],
        # The channel plan and where each channel goes, which the Karaoke flow works out from the
        # parts and `forge project` does not (it has the file and not the flow): Annex C's 3/2 is
        # L M R V1 V2, so the backing is L and R, the melody guide C and the vocals Ls and Rs.
        "bed": "3/2",
        "lfe": False,
        "assignments": [
            {"source": 0, "channel": 0, "to": "L"},
            {"source": 0, "channel": 1, "to": "R"},
            {"source": 1, "channel": 0, "to": "C"},
            {"source": 2, "channel": 0, "to": "Ls"},
            {"source": 3, "channel": 0, "to": "Rs"},
        ],
        # What the stream is made with, in the command line's own words: `bsmod=voiceover` is
        # `bsmod` 7, which at a layout above 1/0 is the karaoke main service. The centre and
        # surround levels are A/52's own defaults, which `forge encode` takes when it is not told
        # (so there is nothing to say), and that is also what the window starts with.
        "encode": {
            "codec": "ac3",
            "container": "elementary",
            "bitrateKbps": BITRATE_KBPS,
            "options": {"bsmod": "voiceover"},
        },
        "karaoke": {"input": "parts", "listening": "aware", "vocals": "both"},
    }
    return json.dumps(document, indent=2) + "\n"


def forge_command(forge, out):
    """The `forge encode` line of the README, run from the example folder."""
    p = PART_FILES
    return [
        str(forge), "encode", p["backing"], str(out), str(BITRATE_KBPS), "L,C,R,Ls,Rs",
        f"src={p['melody']}", f"src={p['vocal1']}", f"src={p['vocal2']}",
        "map=0.0:L,0.1:R,1.0:C,2.0:Ls,3.0:Rs", "bsmod=voiceover",
    ]


# --- writing and checking -----------------------------------------------------------------------


def build(forge=None):
    """Every file of the example, as {name relative to the example folder: bytes}."""
    files = {}
    parts = render_parts()
    for name, channels in parts.items():
        files[PART_FILES[name]] = wav_bytes(channels)
    files[f"{STEM}.lrc"] = lrc(words=True).encode("utf-8")
    files[f"{STEM}-lines-only.lrc"] = lrc(words=False).encode("utf-8")
    files[f"{STEM}.forgeproj"] = project_json().encode("utf-8")
    if forge is not None:
        files[f"{STEM}.ac3"] = encode_stream(forge, files)
    return files


def encode_stream(forge, files):
    with tempfile.TemporaryDirectory() as folder:
        scratch = pathlib.Path(folder)
        for name, data in files.items():
            if name.endswith(".wav"):
                (scratch / name).parent.mkdir(parents=True, exist_ok=True)
                (scratch / name).write_bytes(data)
        out = scratch / f"{STEM}.ac3"
        subprocess.run(forge_command(forge, out.name), cwd=scratch, check=True,
                       capture_output=True)
        return out.read_bytes()


def _normal(name, data):
    # The lyrics are text: a checkout may have turned LF into CRLF.
    return data.replace(b"\r\n", b"\n") if name.endswith((".lrc", ".forgeproj")) else data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT,
                        help="the example folder (default: examples/karaoke/auld-lang-syne)")
    parser.add_argument("--forge", type=pathlib.Path,
                        help="a built forge: also encode the stream with it")
    parser.add_argument("--check", action="store_true",
                        help="write nothing; fail if a file differs from what this makes")
    args = parser.parse_args(argv)

    files = build(args.forge)
    differs = []
    for name, data in files.items():
        path = args.out / name
        if args.check:
            if not path.is_file() or _normal(name, path.read_bytes()) != data:
                differs.append(name)
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"{name}  {len(data):,} bytes")
    if args.check:
        if differs:
            print("differs from what the generator makes: " + ", ".join(differs), file=sys.stderr)
            return 1
        print(f"{len(files)} files match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
