"""Stand-ins for a Blu-ray soundtrack, built from the committed fixtures.

They are what verify_truehd_local_streams.py --self-test runs on.

A real disc's stream cannot be committed or made here. What can be made is the shape of the files
the check will meet, so that its first run on real ones does not find a plain bug:

  * a BDAV .m2ts (192-byte packets with the four-byte arrival header), a Matroska file and an MP4,
    all written by FFmpeg's muxers from FFmpeg's own TrueHD fixtures, and a transport stream and a
    raw stream written by forge;
  * transport streams in which AC-3 frames share the stream with the TrueHD access units, written
    here (FFmpeg's muxer does not write a core). Their layout is a GUESS at what a disc does:
    the AC-3 frame in a PES packet of its own, and the AC-3 frame at the front of the PES packet
    that carries the access units after it. libs/containers/ERRATA.md (C9) says what is and is not
    known; the stand-ins show only that the reader copes with an AC-3 frame at any whole-frame
    boundary, which is all it assumes. The expected samples for these are the fixture's own.

`self_test` is the entry point; truehd_standins.build_* are used by the unit tests.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "testdata" / "truehd"
AC3_FRAME_BYTES = 2560  # 640 kbit/s at 48 kHz, FFmpeg's AC-3 encoder: 1536 samples, 32 ms
UNITS_PER_FRAME = 1536 / 40  # access units of 40 samples to one AC-3 frame


def run(command: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, check=False)


# --- the muxers FFmpeg and forge provide --------------------------------------------------------


def mux_with_ffmpeg(source: Path, out: Path) -> bool:
    flags = {
        ".m2ts": ["-f", "mpegts", "-mpegts_m2ts_mode", "1"],
        ".mkv": ["-f", "matroska"],
        ".mp4": ["-strict", "-2", "-f", "mp4"],
    }[out.suffix]
    result = run(
        [
            *["ffmpeg", "-hide_banner", "-v", "error", "-y", "-f", "truehd", "-i", str(source)],
            *["-c", "copy", *flags, str(out)],
        ]
    )
    return result.returncode == 0 and out.exists()


# --- a transport stream with AC-3 frames in it ---------------------------------------------------


def crc32_mpeg(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = (
                ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
                if crc & 0x80000000
                else (crc << 1) & 0xFFFFFFFF
            )
    return crc


def section(table_id: int, body: bytes) -> bytes:
    length = len(body) + 4
    head = bytes([table_id, 0xB0 | (length >> 8), length & 0xFF])
    data = head + body
    return data + crc32_mpeg(data).to_bytes(4, "big")


class TransportWriter:
    """Writes one program with one stream_type 0x83 stream, in 188-byte packets or the 192-byte
    BDAV packets of a disc (a four-byte arrival header in front of each)."""

    PMT_PID, ES_PID = 0x100, 0x1100

    def __init__(self, bdav: bool):
        self.bdav = bdav
        self.out = bytearray()
        self.counters: dict[int, int] = {}
        self.arrival = 0

    def packet(self, pid: int, payload: bytes, start: bool) -> None:
        assert len(payload) <= 184
        count = self.counters.get(pid, 0)
        self.counters[pid] = (count + 1) & 0xF
        pad = 184 - len(payload)
        header = bytes([0x47, (0x40 if start else 0) | (pid >> 8), pid & 0xFF])
        if pad:
            adaptation = bytes([pad - 1]) + (b"\x00" + b"\xff" * (pad - 2) if pad > 1 else b"")
            header += bytes([0x30 | count]) + adaptation
        else:
            header += bytes([0x10 | count])
        if self.bdav:
            self.out += (self.arrival & 0x3FFFFFFF).to_bytes(4, "big")
            self.arrival += 1000
        self.out += header + payload

    def psi(self, pid: int, data: bytes) -> None:
        self.packet(pid, b"\x00" + data + b"\xff" * (183 - len(data)), True)

    def start(self) -> None:
        pat = section(
            0,
            (1).to_bytes(2, "big")
            + b"\xc1\x00\x00"
            + (1).to_bytes(2, "big")
            + bytes([0xE0 | (self.PMT_PID >> 8), self.PMT_PID & 0xFF]),
        )
        registration = b"\x05\x04HDMV"  # the program-level registration descriptor
        es = bytes([0x83, 0xE0 | (self.ES_PID >> 8), self.ES_PID & 0xFF, 0xF0, 0x00])
        pmt = section(
            2,
            (1).to_bytes(2, "big")
            + b"\xc1\x00\x00"
            + bytes([0xE0 | (self.ES_PID >> 8), self.ES_PID & 0xFF, 0xF0, len(registration)])
            + registration
            + es,
        )
        self.psi(0, pat)
        self.psi(self.PMT_PID, pmt)

    def pes(self, payload: bytes, pts: int) -> None:
        stamp = bytes(
            [
                0x21 | ((pts >> 29) & 0x0E),
                (pts >> 22) & 0xFF,
                0x01 | ((pts >> 14) & 0xFE),
                (pts >> 7) & 0xFF,
                0x01 | ((pts << 1) & 0xFE),
            ]
        )
        header = b"\x00\x00\x01\xbd" + (3 + len(stamp) + len(payload)).to_bytes(2, "big")
        data = header + b"\x84\x80" + bytes([len(stamp)]) + stamp + payload
        for at in range(0, len(data), 184):
            self.packet(self.ES_PID, data[at : at + 184], at == 0)


def access_units(stream: bytes) -> list[bytes]:
    units, at = [], 0
    while at + 4 <= len(stream):
        length = (int.from_bytes(stream[at : at + 2], "big") & 0xFFF) * 2
        if length < 4 or at + length > len(stream):
            break
        units.append(stream[at : at + length])
        at += length
    return units


def ac3_core(source: Path) -> list[bytes] | None:
    """AC-3 frames of the stream's audio from FFmpeg's encoder, 640 kbit/s, 48 kHz."""
    result = run(
        [
            *["ffmpeg", "-hide_banner", "-v", "error", "-f", "truehd", "-i", str(source)],
            *["-c:a", "ac3", "-b:a", "640k", "-ar", "48000", "-f", "ac3", "-"],
        ]
    )
    data = result.stdout
    if result.returncode != 0 or len(data) % AC3_FRAME_BYTES or data[:2] != b"\x0b\x77":
        return None
    return [data[i : i + AC3_FRAME_BYTES] for i in range(0, len(data), AC3_FRAME_BYTES)]


def build_core_transport(source: Path, out: Path, layout: str) -> bool:
    """A BDAV transport stream of the fixture's access units and AC-3 frames of the same audio.
    layout "separate": each AC-3 frame in a PES packet of its own, the access units after it in the
    next; "joined": the AC-3 frame at the front of the PES packet carrying those access units."""
    core = ac3_core(source)
    if core is None:
        return False
    units = access_units(source.read_bytes())
    writer = TransportWriter(bdav=True)
    writer.start()
    boundary = [round(n * UNITS_PER_FRAME) for n in range(len(core) + 1)]
    boundary[-1] = len(units)
    for n, frame in enumerate(core):
        group = b"".join(units[boundary[n] : boundary[n + 1]])
        pts = 90000 + n * 2880
        if layout == "separate":
            writer.pes(frame, pts)
            if group:
                writer.pes(group, pts)
        else:
            writer.pes(frame + group, pts)
    if boundary[len(core)] < len(units):  # access units after the last frame's span
        writer.pes(b"".join(units[boundary[len(core)] :]), 90000 + len(core) * 2880)
    out.write_bytes(bytes(writer.out))
    return True


# --- the self-test -----------------------------------------------------------------------------


def manifest_entry(name: str) -> dict:
    manifest = json.loads((FIXTURES / "manifest.json").read_text())
    return next(f for f in manifest["fixtures"] if f["name"] == name)


def forge_pcm_digest(forge: str, path: Path, scratch: Path) -> tuple[str | None, str]:
    wav = scratch / "standin.wav"
    result = run([forge, "decode", str(path), str(wav)])
    if result.returncode != 0 or not wav.exists():
        return None, (result.stdout + result.stderr).decode(errors="replace").strip()[-300:]
    data = wav.read_bytes()
    wav.unlink()
    at = data.index(b"data") + 8
    body = data[at:]
    words = bytearray(len(body) // 3 * 4)
    words[1::4], words[2::4], words[3::4] = body[0::3], body[1::3], body[2::3]
    return hashlib.sha256(bytes(words)).hexdigest(), ""


def self_test(args, check_directory, render) -> int:
    forge = args.forge
    if not forge or not Path(forge).exists():
        print("SKIP: no forge binary (set ICLFORGE_CLI or pass --forge)")
        return 0
    if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
        print("SKIP: ffmpeg and ffprobe are needed on PATH")
        return 0
    failures = 0
    with tempfile.TemporaryDirectory(prefix="truehd-standins-") as name:
        base = Path(name)
        stand = base / "stand-ins"
        stand.mkdir()
        source = FIXTURES / "truehd_m51_s24.thd"
        for suffix in (".m2ts", ".mkv", ".mp4"):
            if not mux_with_ffmpeg(source, stand / f"m51{suffix}"):
                print(f"FAIL: FFmpeg could not write m51{suffix}")
                failures += 1
        shutil.copy(source, stand / "m51.thd")
        shutil.copy(FIXTURES / "truehd_st96_s24.thd", stand / "st96.thd")
        if run([forge, "ts", str(source), str(stand / "m51_forge.ts")]).returncode != 0:
            print("FAIL: forge could not write a transport stream")
            failures += 1
        rows, _census = check_directory(stand, forge, args)
        print(render(rows))
        failures += sum(row.failed for row in rows)
        wrong = [r.name for r in rows if r.verdict != "identical"]
        if wrong:
            print(f"FAIL: not identical: {wrong}")
            failures += 1
        if len(rows) < 6:
            print(f"FAIL: {len(rows)} tracks found, 6 expected")
            failures += 1

        # AC-3 frames in the stream: forge's decode must be the fixture's samples.
        expected = manifest_entry("truehd_m51_s24")["pcm_sha256"]
        for layout in ("separate", "joined"):
            path = base / f"core_{layout}.m2ts"
            if not build_core_transport(source, path, layout):
                print(
                    "SKIP: FFmpeg's AC-3 encoder is not available; the core stand-ins are not built"
                )
                break
            digest, why = forge_pcm_digest(forge, path, base)
            probe = run([forge, "probe", str(path), "json=1"])
            core = (
                json.loads(probe.stdout)["truehd"]["ac3_core_frames"]
                if probe.returncode == 0
                else -1
            )
            good = digest == expected and core > 0
            print(
                f"{'ok  ' if good else 'FAIL'} AC-3 frames in the stream, {layout}: "
                f"decode {'equals' if digest == expected else 'differs from'} the fixture's, "
                f"{core} AC-3 frame(s) set aside {why}"
            )
            failures += not good
    print("stand-ins: " + ("all good" if not failures else f"{failures} problem(s)"))
    return 1 if failures else 0
