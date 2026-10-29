"""The streams and containers forge writes, for the checks that hand them to Dolby's tools.

Shared by check_dolby_mp4_boxes.py, check_dolby_demux_roundtrip.py, check_dolby_cenc.py and
dolby_validator_bundle.py. Everything here is made by forge from signals it synthesises or from
the committed object fixture; nothing comes from Dolby's kit.
"""

from __future__ import annotations

import math
import os
import shutil
import struct
import subprocess
import wave
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
JOC_FIXTURE = REPO / "testdata" / "object-fixture" / "dee_joc_514.ec3"

# MP4 sample-entry name for each stream kind, and the extension of an elementary stream.
ENTRY_OF = {"ac3": "ac-3", "eac3": "ec-3", "joc": "ec-3", "ac4": "ac-4"}
SUFFIX_OF = {"ac3": ".ac3", "eac3": ".ec3", "joc": ".ec3", "ac4": ".ac4"}
ES_SUFFIXES = (".ac3", ".ec3", ".ac4")


@dataclass(frozen=True)
class Stream:
    """A clear elementary stream written by forge."""

    name: str
    path: Path

    @property
    def entry(self) -> str:
        return ENTRY_OF[self.name]


def default_cli() -> str | None:
    return os.environ.get("ICLFORGE_CLI") or shutil.which("forge")


def run(argv: list[object], *, timeout: float = 600.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in argv], capture_output=True, text=True, timeout=timeout, check=False
    )


def write_stereo_wav(path: Path, seconds: float = 3.0, rate: int = 48000) -> None:
    """Two different tones, so a swapped or dropped channel cannot pass for the right one."""
    frames = bytearray()
    for i in range(int(seconds * rate)):
        left = int(12000 * math.sin(2 * math.pi * 440 * i / rate))
        right = int(12000 * math.sin(2 * math.pi * 660 * i / rate))
        frames += struct.pack("<hh", left, right)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))


def make_streams(cli: str, work: Path) -> tuple[list[Stream], list[str]]:
    """AC-3, E-AC-3, E-AC-3 JOC and AC-4 streams. Returns the streams and any notes."""
    work.mkdir(parents=True, exist_ok=True)
    streams: list[Stream] = []
    notes: list[str] = []

    def add(name: str, argv: list[object], path: Path) -> None:
        done = run([cli, *argv])
        if done.returncode == 0 and path.exists():
            streams.append(Stream(name, path))
        else:
            notes.append(
                f"{name}: forge {argv[0]} failed ({done.returncode}): {done.stderr.strip()[-120:]}"
            )

    add("ac3", ["sine", work / "ac3.ac3", 3, 192], work / "ac3.ac3")
    add("eac3", ["eac3-sine", work / "eac3.ec3", 3, 192], work / "eac3.ec3")
    if JOC_FIXTURE.exists():
        shutil.copy(JOC_FIXTURE, work / "joc.ec3")
        streams.append(Stream("joc", work / "joc.ec3"))
    else:
        add("joc", ["atmos", work / "joc.ec3", 3, 640], work / "joc.ec3")
    wav = work / "stereo.wav"
    write_stereo_wav(wav)
    add("ac4", ["ac4-encode", wav, work / "ac4.ac4", 96], work / "ac4.ac4")
    return streams, notes


def make_containers(cli: str, stream: Stream, work: Path) -> dict[str, Path]:
    """The containers forge writes for one stream: mp4, the fmp4 directory (init segment, media
    segments, HLS playlists, DASH manifest) and the two TS profiles (AC-4 has DVB only)."""
    found: dict[str, Path] = {}
    work.mkdir(parents=True, exist_ok=True)
    mp4 = work / f"{stream.name}.mp4"
    if run([cli, "mp4", stream.path, mp4]).returncode == 0:
        found["mp4"] = mp4
    frag_dir = work / f"{stream.name}_fmp4"
    if run([cli, "fmp4", stream.path, frag_dir]).returncode == 0:
        for key, name in (
            ("fmp4_init", "init.mp4"),
            ("dash", "manifest.mpd"),
            ("hls_media", "audio.m3u8"),
            ("hls_master", "master.m3u8"),
        ):
            if (frag_dir / name).exists():
                found[key] = frag_dir / name
    for profile in ("dvb", "atsc"):
        if stream.name == "ac4" and profile == "atsc":
            continue
        ts = work / f"{stream.name}_{profile}.ts"
        if run([cli, "ts", stream.path, ts, profile]).returncode == 0:
            found[f"ts_{profile}"] = ts
    return found


# --- elementary-stream comparison --------------------------------------------------------------


def ac4_frames(data: bytes) -> list[bytes] | None:
    """The raw AC-4 frames of an AC-4 simple-transport stream (ETSI TS 103 190-1 Annex G):
    sync word 0xAC40, or 0xAC41 when a 16-bit CRC follows the frame; a 16-bit length, or 0xFFFF
    and a 24-bit length. None when the bytes are not such a stream."""
    frames: list[bytes] = []
    pos = 0
    while pos + 4 <= len(data):
        sync = struct.unpack(">H", data[pos : pos + 2])[0]
        if sync not in (0xAC40, 0xAC41):
            return None
        length = struct.unpack(">H", data[pos + 2 : pos + 4])[0]
        header = 4
        if length == 0xFFFF:
            if pos + 7 > len(data):
                return None
            length = int.from_bytes(data[pos + 4 : pos + 7], "big")
            header = 7
        end = pos + header + length + (2 if sync == 0xAC41 else 0)
        if end > len(data):
            return None
        frames.append(data[pos + header : pos + header + length])
        pos = end
    return frames if pos == len(data) else None


def same_stream(kind: str, got: bytes, expected: bytes) -> bool:
    """AC-3 and E-AC-3 streams are compared byte for byte. AC-4 streams are compared frame by
    frame, because two writers may frame the same raw frames differently (sync word, length
    form, CRC) and both be right."""
    if kind == "ac4":
        a, b = ac4_frames(got), ac4_frames(expected)
        return a is not None and a == b
    return got == expected
