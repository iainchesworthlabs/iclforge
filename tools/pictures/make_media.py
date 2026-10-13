#!/usr/bin/env python3
"""Make the files the documentation pictures are taken with.

A picture of a player needs something playing, and a picture is only regenerable if what was
playing is. Everything here is computed: sines with slow envelopes, a scene of moving objects,
and the two streams `forge` encodes from them. The encoders are deterministic, so the same
commit gives the same bytes, and take_pictures.py checks nothing more than that the files are
there. Nothing is recorded and nothing is anybody's music.

    python3 tools/pictures/make_media.py --forge <build>/bin/forge --out <folder>

What it writes, and which pictures use it (tools/pictures/pictures.json names them as
`{media}/<file>`):

    harbour-51.wav        30 s of 5.1, the source of the two streams below. It is silent from
                          0.8 s to 2.4 s: `hearth --seek` starts the item to show a position, and
                          a capture parked in that window plays nothing on the speakers of the
                          machine it runs on.
    scene.txt             five objects crossing the room, for the Atmos stream.
    night-garden.ec3      E-AC-3 with Atmos objects: Hearth's Play, Media, Speakers and Decoder
                          pages, and the application Crucible's room lists.
    harbour.ac4           AC-4 5.1: Hearth's AC-4 decoder page.
    roundtrip-51.wav      one second of 5.1 and of stereo sines: the Forge GUI's source.
    roundtrip-stereo.wav
    tone-stereo-30.wav    a quiet tone a browser plays, so that Crucible's room has a second
                          application in it. Forty seconds, to outlast the run; the name is the
                          one the pictures were first taken with.
    demo-signing.key      32 random bytes, so that Crucible's Signing card says a key is loaded
                          and shows nobody's real one. Not reproducible, and need not be.
    takes/                four stand-in takes for Hearth's Microphone card to list: the
                          descriptions are real, the audio beside each is a FLAC header and no
                          sound, since the card reads the descriptions and opens no audio.

Standard library only, apart from the `forge` it is given.
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import os
import struct
import subprocess
import sys
import wave
from pathlib import Path

RATE = 48000


def _write_wav(path: Path, channels: int, seconds: float, sample) -> None:
    """`sample(t, channel)` in [-1, 1] for each frame, as 16-bit PCM."""
    frames = bytearray()
    for n in range(round(RATE * seconds)):
        t = n / RATE
        for channel in range(channels):
            value = max(-1.0, min(1.0, sample(t, channel)))
            frames += struct.pack("<h", int(value * 32767))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(frames))


def _gate(t: float) -> float:
    """Digital silence from 0.8 s to 2.4 s, with a fifth of a second's fade each side."""
    if 0.8 <= t <= 2.4:
        return 0.0
    if 0.6 < t < 0.8:
        return (0.8 - t) / 0.2
    if 2.4 < t < 2.6:
        return (t - 2.4) / 0.2
    return 1.0


def harbour(path: Path) -> None:
    """Six channels in WAV order (L R C LFE Ls Rs), each a sine under its own slow swell."""
    freqs = (220.0, 330.0, 277.2, 55.0, 392.0, 440.0)
    level = (0.30, 0.28, 0.22, 0.35, 0.12, 0.14)
    swell = (0.23, 0.31, 0.17, 0.11, 0.41, 0.37)

    def sample(t: float, c: int) -> float:
        envelope = 0.55 + 0.45 * math.sin(2 * math.pi * swell[c] * t + c)
        return _gate(t) * level[c] * envelope * math.sin(2 * math.pi * freqs[c] * t)

    _write_wav(path, 6, 30, sample)


def scene(path: Path) -> None:
    """`forge atmos-encode ... objects <scene>`: object, time, x, y, z, gain, LFE send.

    Each object crosses the room in a straight line over the thirty seconds. Object 3 is the LFE
    channel and keeps its default.
    """
    paths = (
        (0.1, 0.1, 0.0, 0.9, 0.2, 0.6),
        (0.9, 0.1, 0.0, 0.1, 0.8, 0.9),
        (0.5, 0.0, 0.2, 0.5, 1.0, 1.0),
        None,
        (0.1, 0.9, 0.5, 0.9, 0.9, 0.2),
        (0.9, 0.9, 0.8, 0.2, 0.3, 0.4),
    )
    rows = ["# object time_s x y z gain lfe_send"]
    for index, line in enumerate(paths):
        if line is None:
            continue
        x0, y0, z0, x1, y1, z1 = line
        for step in range(7):
            a = step / 6
            rows.append(
                f"{index} {5.0 * step:.1f} {x0 + (x1 - x0) * a:.3f} {y0 + (y1 - y0) * a:.3f} "
                f"{z0 + (z1 - z0) * a:.3f} 1.0 0.0")
    path.write_text("\n".join(rows) + "\n", encoding="ascii", newline="\n")


def roundtrip(path: Path, freqs: tuple[float, ...], db: float) -> None:
    """One second of a steady sine a channel, at `db` dBFS: what the Forge GUI's pictures load."""
    amplitude = 10 ** (db / 20)
    _write_wav(path, len(freqs), 1, lambda t, c: amplitude * math.sin(2 * math.pi * freqs[c] * t))


def tone(path: Path) -> None:
    """A quiet 330 Hz on both channels, swelling twice a second, for a browser to play."""
    def sample(t: float, _channel: int) -> float:
        swell = 0.6 + 0.4 * math.sin(2 * math.pi * 0.5 * t)
        return 0.05 * swell * math.sin(2 * math.pi * 330 * t)

    _write_wav(path, 2, 40, sample)


# The takes the Microphone card lists: the name Hearth gives a take (the item's title and when),
# how long it ran and where in the item it began.
TAKES = (
    ("Auld Lang Syne", "2026-10-11", "193045", 16.4, 0),
    ("Auld Lang Syne", "2026-10-11", "194210", 12.1, 4180),
    ("Leave Her, Johnny", "2026-10-12", "201512", 19.0, 0),
    ("Auld Lang Syne", "2026-10-12", "203307", 16.5, 0),
)


def takes(folder: Path) -> None:
    """Stand-in takes: each description is one Hearth writes (`iclforge-take-1`), and the file
    beside it is the four bytes a FLAC file begins with. Hearth lists takes from the descriptions
    and opens no audio to do it."""
    folder.mkdir(parents=True, exist_ok=True)
    for title, day, clock, seconds, start_ms in TAKES:
        stem = f"{title} - {day} {clock}"
        (folder / f"{stem}.flac").write_bytes(b"fLaC")
        description = {
            "format": "iclforge-take-1",
            "audio": f"{stem}.flac",
            "seconds": seconds,
            "created_utc": f"{day}T{clock[0:2]}:{clock[2:4]}:{clock[4:6]}Z",
            "item": {"title": title},
            "item_start_ms": start_ms,
        }
        (folder / f"{stem}.take.json").write_text(
            json.dumps(description, indent=2) + "\n", encoding="utf-8", newline="\n")


def _forge(forge: Path, *args: str | Path) -> None:
    """Runs an encode, and shows what it said only when it failed: its progress is not news."""
    done = subprocess.run([str(forge), *map(str, args)], check=False, capture_output=True,
                          text=True, errors="replace")
    if done.returncode != 0:
        sys.stderr.write(done.stdout + done.stderr)
        raise SystemExit(f"forge {args[0]} failed with exit code {done.returncode}")


def make(out: Path, forge: Path | None) -> list[Path]:
    """Everything, into `out`; the streams only when there is a `forge` to encode them. A file
    that is there already is left: the sources take a minute of Python to compute."""
    out.mkdir(parents=True, exist_ok=True)
    made: list[Path] = []

    def want(name: str) -> Path | None:
        path = out / name
        made.append(path)
        return None if path.exists() else path

    if path := want("harbour-51.wav"):
        harbour(path)
    if path := want("scene.txt"):
        scene(path)
    if path := want("roundtrip-51.wav"):
        roundtrip(path, (440.0, 554.37, 659.25, 110.0, 329.63, 392.0), -5.5)
    if path := want("roundtrip-stereo.wav"):
        roundtrip(path, (440.0, 554.37), -6.0)
    if path := want("tone-stereo-30.wav"):
        tone(path)
    if path := want("demo-signing.key"):
        path.write_text(base64.b64encode(os.urandom(32)).decode("ascii"), encoding="ascii")
    takes(out / "takes")
    made.append(out / "takes")
    if forge is not None:
        if path := want("night-garden.ec3"):
            _forge(forge, "atmos-encode", out / "harbour-51.wav", path, "768", "objects",
                   out / "scene.txt")
        if path := want("harbour.ac4"):
            _forge(forge, "ac4-encode", out / "harbour-51.wav", path, "192")
    return made


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True, help="the folder the files go in")
    parser.add_argument("--forge", type=Path,
                        help="the forge of the build the pictures are taken from; without it "
                             "the two streams are not made")
    args = parser.parse_args(argv[1:])
    if args.forge is not None and not args.forge.is_file():
        print(f"no forge at {args.forge}", file=sys.stderr)
        return 2
    for path in make(args.out.resolve(), args.forge):
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
