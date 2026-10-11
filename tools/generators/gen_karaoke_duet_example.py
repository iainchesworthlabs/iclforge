#!/usr/bin/env python3
"""The karaoke duet example under examples/karaoke/leave-her-johnny/, made from arithmetic.

"Leave Her, Johnny": a capstan shanty, traditional, in the public domain. The tune and the verse
are as Joanna C. Colcord printed them in "Roll and Go: Songs of American Sailormen" (1924, p. 58),
which is itself out of copyright; the notes below are that page's, read off it. Nothing here is a
recording. The parts are synthesised by the code below, one voice at a time, so that the script is
the record of what is in the files and anyone can say where every sample came from.

It is a duet in the way a shanty is one: the shantyman sings a line and the crew answers it, so
the two vocals sing *different* lines and each is silent while the other sings. That is what
Annex C's two vocal channels are for, and what "vocals: V1 only, V2 only or both" is for:

  parts/backing.wav               stereo   a stamped bass and chord on the beat
  parts/melody-guide.wav          mono     a hollow, clarinet-like tone on the whole tune
  parts/vocal-1-shantyman.wav     mono     a sung "ah", the solo lines (1 and 3), an octave down
  parts/vocal-2-crew.wav          mono     a darker "oo", the crew's lines (2 and 4), an octave down
  leave-her-johnny.lrc            the words, timed to the syllable (Hearth's enhanced LRC), each
                                  line marked with its singer, V1: or V2:, and the two named
  leave-her-johnny-lines-only.lrc the same words timed by the line only
  leave-her-johnny.forgeproj      the Forge GUI project: the four parts and nothing of how they
                                  are laid out, which the Karaoke flow and `forge project` do
  leave-her-johnny.ac3            with --forge: the four parts encoded, 3/2, bsmod 7 (karaoke)

The verse is eight bars of 4/4 at 100 beats a minute with a beat of pick-up, 19.2 s: exactly 600
AC-3 frames of 1536 samples at 48 kHz, so that the Forge GUI and `forge encode` see one length.
Five channels of that are 480 kB a second: the verse is 9.2 MB of parts and 0.9 MB of stream. A
second verse costs as much again.

Reproducible to the byte, on any platform, as gen_karaoke_example.py is: the synthesis is
karaoke_synth.py's, which uses only + - * / and comparisons on IEEE doubles.

Stdlib only.

Usage (repo root):
    python tools/generators/gen_karaoke_duet_example.py                  write the files
    python tools/generators/gen_karaoke_duet_example.py --forge <forge>  and encode the stream
    python tools/generators/gen_karaoke_duet_example.py --check          compare, write nothing
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

# The synthesis is shared with the other karaoke example. A test loads this file by its path, so
# its folder is put on the path here and not left to the caller.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from karaoke_synth import (
    RATE,
    add_partial,
    half_life,
    hz,
    render_voice,
    scaled,
    stamp,
    wav_bytes,
)

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "examples" / "karaoke" / "leave-her-johnny"

FRAME = 1536  # samples in one AC-3 frame
BPM = 100
BEAT = RATE * 60 // BPM  # 28800 samples: an eighth is 14400, which is 30 hundredths of a second
EIGHTH = BEAT // 2

# What `forge encode` is given: a 3/2 stream at 384 kbit/s, the melody guide where the centre
# goes and the two vocals where the surrounds go (Annex C, Table C.2.1).
BITRATE_KBPS = 384
STEM = "leave-her-johnny"

PART_FILES = {
    "backing": "parts/backing.wav",
    "melody": "parts/melody-guide.wav",
    "vocal1": "parts/vocal-1-shantyman.wav",
    "vocal2": "parts/vocal-2-crew.wav",
}

# --- the score ----------------------------------------------------------------------------------

# Who sings a line: the shantyman (vocal 1) or the crew (vocal 2).
SOLO = "vocal1"
CREW = "vocal2"

# midi 60 is middle C. D mixolydian: one sharp in the signature, the tune on D, and a C natural.
D4, FS4, G4, A4, B4, C5 = 62, 66, 67, 69, 71, 72

# One syllable: (text, space after it?, [(midi pitch, length in eighths), ...]). More than one
# note on a syllable is a melisma. 4/4, one beat of pick-up, so the last bar is three beats and
# the pick-up of the next verse would complete it. The notes, the syllables and who sings each
# line are those of the page: the shantyman's lines are marked Solo, the crew's Cho.
LINES: list[tuple[str, list[tuple[str, bool, list[tuple[int, int]]]]]] = [
    (
        SOLO,
        [
            ("I", True, [(D4, 2)]),
            ("thought", True, [(G4, 2)]),
            ("I", True, [(G4, 2)]),
            ("heard", True, [(FS4, 3)]),
            ("the", True, [(D4, 1)]),
            ("Old", True, [(G4, 2)]),
            ("Man", True, [(G4, 2)]),
            ("say,", False, [(FS4, 3), (D4, 1)]),
        ],
    ),
    (
        CREW,
        [
            ("Leave", True, [(G4, 2)]),
            ("her,", True, [(G4, 2)]),
            ("John", False, [(G4, 2)]),
            ("ny,", True, [(G4, 2)]),
            ("leave", True, [(FS4, 2)]),
            ("her!", False, [(A4, 4)]),
        ],
    ),
    (
        SOLO,
        [
            ("You", True, [(G4, 1)]),
            ("can", True, [(A4, 1)]),
            ("go", True, [(B4, 2)]),
            ("a", False, [(B4, 2)]),
            ("shore", True, [(B4, 2)]),
            ("and", True, [(FS4, 2)]),
            ("draw", True, [(A4, 2)]),
            ("your", True, [(A4, 2)]),
            ("pay,", False, [(G4, 2)]),
        ],
    ),
    (
        CREW,
        [
            ("It's", True, [(G4, 1), (A4, 1)]),
            ("time", True, [(B4, 3)]),
            ("for", True, [(C5, 1)]),
            ("us", True, [(B4, 3)]),
            ("to", True, [(G4, 1)]),
            ("leave", True, [(A4, 2)]),
            ("her!", False, [(FS4, 4)]),
        ],
    ),
]

# Pick-up + 8 bars less the pick-up's beat, in eighths, and the length in whole AC-3 frames.
SCORE_EIGHTHS = sum(n for _, line in LINES for _, _, notes in line for _, n in notes)
SCORE_SAMPLES = SCORE_EIGHTHS * EIGHTH
FRAMES = -(-SCORE_SAMPLES // FRAME)
TOTAL = FRAMES * FRAME

# The chord on each beat, beat 0 being the pick-up: D for the shantyman's "I thought I heard",
# G where the crew comes in, back to D. A bass note goes on the first and third beats of a bar,
# a chord on the second and fourth.
_BARS = ("D", "D", "G", "D", "G", "D", "G")
_PROGRESSION = ("D", *(chord for chord in _BARS for _ in range(4)), "D", "D", "D")
_BASS = {"D": 38, "G": 43}
_STAB = {"D": (57, 62, 66), "G": (59, 62, 67)}

# The two voices sing an octave below the written tune, where a shantyman and a crew sing it.
_VOICE_OCTAVE = -12


# --- the parts -----------------------------------------------------------------------------------


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
                add_partial(buf, start, length, f * mult, amp * weight, decays[seconds],
                            attack=240, release=480)
    return [
        [a + c for a, c in zip(left, middle, strict=True)],
        [b + c for b, c in zip(right, middle, strict=True)],
    ]


def _syllable_spans():
    """One entry for each syllable: (first sample, samples, [(midi, samples), ...], text,
    space after, line number, first of its line, who sings the line)."""
    spans = []
    at = 0
    for line_index, (singer, line) in enumerate(LINES):
        for i, (text, space, notes) in enumerate(line):
            length = sum(n for _, n in notes) * EIGHTH
            spans.append((at, length, [(m, n * EIGHTH) for m, n in notes], text, space,
                          line_index, i == 0, singer))
            at += length
    return spans


def render_melody():
    """A hollow tone of the odd partials 1, 3 and 5 on the whole tune, one note after another,
    each note starting 20 ms in and ending 20 ms short of the next so the notes are separate."""
    out = [0.0] * TOTAL
    for at, _, notes, *_ in _syllable_spans():
        position = at
        for midi, length in notes:
            f = hz(midi)
            for mult, weight in ((1, 1.0), (3, 0.45), (5, 0.2)):
                add_partial(out, position, length - 960, f * mult, weight, 1.0,
                            attack=960, release=480)
            position += length
    return [out]


def render_singer(singer):
    """One voice: it sings its own lines and is silent through the other's, as a rest."""
    syllables = []
    for _, length, notes, *_, who in _syllable_spans():
        if who == singer:
            syllables.append([(m + _VOICE_OCTAVE, n) for m, n in notes])
        else:
            syllables.append([(None, length)])
    if singer == SOLO:
        return render_voice(syllables, TOTAL, "ah", vibrato=0.004, breath=0.20, seed=104729)
    return render_voice(syllables, TOTAL, "oo", vibrato=0.0015, breath=0.30, seed=7919)


# Each part is brought to this peak (of full scale) after it is made.
PEAKS = {"backing": 0.45, "melody": 0.30, "vocal1": 0.55, "vocal2": 0.50}


def render_parts():
    return scaled(
        {
            "backing": render_backing(),
            "melody": render_melody(),
            "vocal1": render_singer(SOLO),
            "vocal2": render_singer(CREW),
        },
        PEAKS,
    )


# --- the words and the project ------------------------------------------------------------------


# What the two singers are called, and the marker a line of each is written with: right after the
# line's time tag, before any word tag (`[00:05.40]V2: <00:05.40>Leave ...`). The lyric reader
# (libs/lyrics) reads the marker as the line's singer and the names as the two singers' names.
SINGER_NAMES = {SOLO: "Shantyman", CREW: "Crew"}
SINGER_MARKERS = {SOLO: "V1:", CREW: "V2:"}


def lrc(words):
    """The lyrics file: one line per sung line, each marked with its singer; `words` times every
    syllable, else only lines. A time is a whole number of hundredths, since an eighth note is
    30 of them."""
    rows = [
        "[ti:Leave Her, Johnny]",
        "[ar:Traditional]",
        "[al:iclforge karaoke example]",
        f"[v1:{SINGER_NAMES[SOLO]}]",
        f"[v2:{SINGER_NAMES[CREW]}]",
        "[by:tools/generators/gen_karaoke_duet_example.py]",
        f"[length:{TOTAL // RATE // 60:02d}:{TOTAL // RATE % 60:02d}]",
    ]
    eighths = 0
    for singer, line in LINES:
        row = stamp(eighths * EIGHTH * 100 // RATE, "line") + SINGER_MARKERS[singer] + " "
        for text, space, notes in line:
            if words:
                row += stamp(eighths * EIGHTH * 100 // RATE, "word")
            row += text + (" " if space else "")
            eighths += sum(n for _, n in notes)
        rows.append(row.rstrip())
    # The last line ends where the music does.
    rows.append(stamp(SCORE_EIGHTHS * EIGHTH * 100 // RATE, "line"))
    return "\n".join(rows) + "\n"


def project_json():
    """The Forge GUI project: the four parts, and nothing of how they are laid out. The Karaoke
    flow works the channel plan out from which parts there are, and so does `forge project`
    (Annex C's Table C.2.1: the backing left and right, the melody guide in the centre, the
    vocals where the surrounds would be), with the service set to bsmod 7."""
    document = {
        "format": "iclforge-project",
        "version": 1,
        "title": "Leave Her, Johnny (karaoke duet example)",
        "flow": "karaoke",
        "sources": [
            {"path": PART_FILES["backing"], "part": "backing"},
            {"path": PART_FILES["melody"], "part": "melody"},
            {"path": PART_FILES["vocal1"], "part": "vocal1"},
            {"path": PART_FILES["vocal2"], "part": "vocal2"},
        ],
        "encode": {
            "codec": "ac3",
            "container": "elementary",
            "bitrateKbps": BITRATE_KBPS,
        },
        "karaoke": {"input": "parts", "listening": "aware", "vocals": "both"},
    }
    return json.dumps(document, indent=2) + "\n"


def forge_command(forge, out):
    """The `forge encode` line of the README, run from the example folder: what `forge project`
    runs for the project above, written out."""
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
    for name, channels in render_parts().items():
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
    # The lyrics and the project are text: a checkout may have turned LF into CRLF.
    return data.replace(b"\r\n", b"\n") if name.endswith((".lrc", ".forgeproj")) else data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT,
                        help="the example folder (default: examples/karaoke/leave-her-johnny)")
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
