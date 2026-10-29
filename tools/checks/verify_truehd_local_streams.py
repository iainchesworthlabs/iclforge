#!/usr/bin/env python3
"""Holds forge's TrueHD decode to FFmpeg's, sample for sample, on streams kept outside the tree.

The owner's own Blu-ray soundtracks (BDAV .m2ts, MPEG-TS, Matroska from a ripper, MP4, or a raw
.thd/.mlp) are kept on their machine and never committed. Point ICLFORGE_TRUEHD_STREAMS at a
directory of them and ICLFORGE_CLI (or --forge) at a forge binary; with no directory this prints
SKIP and exits 0, as the other gated checks do (check_ac4_kit_streams.py, score_truehd_decode.py).

For every TrueHD or MLP track in every file:

  * the container is detected from its bytes and FFmpeg's demuxer extracts the track (-c copy);
  * FFmpeg's strict decode (-err_detect crccheck+bitstream+buffer+explode -xerror) and
    `forge decode` of the extracted stream are compared sample for sample, channel by channel
    (FFmpeg returns the channels of a 7 or 8 channel stream in its own order, so channels are
    matched by their content, and the order is reported). The codec is lossless: any
    difference is a defect in one of the two decoders. FFmpeg only warns of a failed lossless
    check, so its warnings are counted and reported too;
  * `forge decode` of the file as it is (the container reader and the AC-3 core handling) is held
    to the same samples, when the file is not larger than --max-container-mib and forge's first
    stream in it is this track;
  * what the stream is is reported: container, sample rate, channel layout, duration, access
    units, restart headers, lossless-check failures of each decoder, mean and peak rate, the
    effective sample width, whether an AC-3 core shares the stream, whether extra data (evolution
    frames) or an object substream (Atmos) is present, and anything the library refused.

Nothing derived from the content is written anywhere but a scratch directory that is removed at
the end, and the output is metadata: names, counts and rates, never samples or bytes. --anonymous
replaces the file names with numbers.

    ICLFORGE_TRUEHD_STREAMS=/path/to/discs ICLFORGE_CLI=build/bin/forge \\
        python3 tools/checks/verify_truehd_local_streams.py [--anonymous] [--report table.txt]
    python3 tools/checks/verify_truehd_local_streams.py --self-test --forge build/bin/forge

--self-test builds stand-ins from the committed fixtures with FFmpeg's muxers (a BDAV .m2ts, a
transport stream, Matroska, MP4, a raw stream, and transport streams with AC-3 frames shared with
the access units) and runs the check on them, so that the first run on real files does not meet a
fault the stand-ins show. It cannot show how real discs lay a stream out (libs/containers/ERRATA.md,
C9).

Exit status 1 when a track differs, a decoder refuses what the other reads, or a lossless check
fails. Needs ffmpeg and ffprobe on PATH.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

ENV_DIR = "ICLFORGE_TRUEHD_STREAMS"
ENV_CLI = "ICLFORGE_CLI"
STRICT = ["-v", "warning", "-xerror", "-err_detect", "crccheck+bitstream+buffer+explode"]
CHUNK_FRAMES = 1 << 16
EXTENSIONS = {".m2ts", ".mts", ".ts", ".mkv", ".mka", ".mp4", ".m4a", ".thd", ".mlp", ".truehd"}
# FFmpeg returns the channels of these layouts in the order of its own layout bits.
EXPECTED_FFMPEG_ORDER = {7: [0, 1, 2, 5, 6, 3, 4], 8: [0, 1, 2, 3, 6, 7, 4, 5]}


# --- containers --------------------------------------------------------------------------------


def detect_container(head: bytes) -> str:
    """The container from the first bytes of a file: matroska, mp4, mpegts (188 or 192) or raw."""
    if head[:4] == b"\x1a\x45\xdf\xa3":
        return "matroska"
    if head[4:8] in (b"ftyp", b"moov", b"styp", b"mdat", b"free", b"wide"):
        return "mp4"
    for stride, offset in ((192, 4), (188, 0), (204, 0)):
        if all(
            len(head) > offset + stride * k and head[offset + stride * k] == 0x47 for k in range(4)
        ):
            return "mpegts" if stride != 192 else "m2ts"
    if head[4:7] == b"\xf8\x72\x6f" or head[:2] == b"\x0b\x77":
        return "raw"
    return "unknown"


# --- running programs --------------------------------------------------------------------------


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, check=False, **kwargs)


def ffprobe_streams(path: Path) -> list[dict]:
    result = run(
        ["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)]
    )
    if result.returncode != 0:
        return []
    return json.loads(result.stdout).get("streams", [])


def extract(path: Path, index: int, out: Path) -> tuple[bool, str]:
    result = run(
        [
            *["ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(path), "-map", f"0:{index}"],
            *["-c", "copy", "-f", "truehd", str(out)],
        ]
    )
    return result.returncode == 0 and out.exists(), result.stderr.decode(errors="replace")


# --- sample streams ----------------------------------------------------------------------------


@dataclass
class Channels:
    """Per-channel digests of a stream of interleaved 32-bit words, fed in chunks of whole frames.
    Each channel's four byte planes are hashed separately, so the digest does not depend on the
    chunking and two decodes compare without either being held in memory."""

    channels: int
    hashes: list = field(default_factory=list)  # [channel][plane] -> a hash of that byte plane
    frames: int = 0
    low_bits_or: int = 0  # the OR of every sample's low byte of the 24 (bits 8 to 15 of the word)

    def __post_init__(self):
        self.hashes = [[hashlib.sha256() for _ in range(4)] for _ in range(self.channels)]

    def add(self, words: bytes) -> None:
        width = self.channels * 4
        if len(words) % width:
            raise ValueError("a partial frame")
        self.frames += len(words) // width
        for c in range(self.channels):
            for plane in range(4):
                data = bytes(words[c * 4 + plane :: width])
                self.hashes[c][plane].update(data)
                if plane == 1:
                    for value in set(data):
                        self.low_bits_or |= value

    def digests(self) -> list[str]:
        return [
            hashlib.sha256(b"".join(plane.digest() for plane in planes)).hexdigest()
            for planes in self.hashes
        ]


def read_chunks(stream, width: int):
    while True:
        data = stream.read(width * CHUNK_FRAMES)
        if not data:
            return
        # A pipe may return a short read in the middle of a frame.
        while len(data) % width:
            more = stream.read(width - len(data) % width)
            if not more:
                raise ValueError("a partial frame at the end")
            data += more
        yield data


def hash_ffmpeg(es: Path, channels: int) -> tuple[Channels | None, str, int]:
    """(digests, FFmpeg's warnings and errors, exit status) of its strict decode of the stream."""
    process = subprocess.Popen(
        ["ffmpeg", "-hide_banner", *STRICT, "-f", "truehd", "-i", str(es), "-f", "s32le", "-"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    digests = Channels(channels)
    stderr = []
    # stderr is read after stdout ends; a decode that writes enough warnings to fill the pipe
    # would stall, so it is drained by a thread.
    import threading  # noqa: PLC0415

    thread = threading.Thread(target=lambda: stderr.append(process.stderr.read()), daemon=True)
    thread.start()
    try:
        for chunk in read_chunks(process.stdout, channels * 4):
            digests.add(chunk)
    except ValueError:
        pass
    process.stdout.close()
    status = process.wait()
    thread.join()
    return digests, (stderr[0] if stderr else b"").decode(errors="replace"), status


def hash_wav(path: Path) -> Channels | None:
    """Digests of a 24-bit PCM WAV's samples as 32-bit words (the value shifted left by 8)."""
    with path.open("rb") as f:
        head = f.read(12)
        if head[:4] not in (b"RIFF", b"RF64") or head[8:12] != b"WAVE":
            return None
        channels = bits = 0
        while True:
            header = f.read(8)
            if len(header) < 8:
                return None
            tag, size = header[:4], int.from_bytes(header[4:], "little")
            if tag == b"fmt ":
                fmt = f.read(size + (size & 1))
                channels = int.from_bytes(fmt[2:4], "little")
                bits = int.from_bytes(fmt[14:16], "little")
            elif tag == b"data":
                break
            else:
                f.seek(size + (size & 1), os.SEEK_CUR)
        if bits != 24 or channels == 0:
            return None
        digests = Channels(channels)
        for chunk in read_chunks(f, channels * 3):
            words = bytearray(len(chunk) // 3 * 4)
            words[1::4], words[2::4], words[3::4] = chunk[0::3], chunk[1::3], chunk[2::3]
            digests.add(bytes(words))
        return digests


def match_channels(ffmpeg: list[str], forge: list[str]) -> list[int] | None:
    """For each FFmpeg channel, the forge channel with the same samples, or None if the two
    decodes do not hold the same set of channels. Equal channels (silence) match in order."""
    if len(ffmpeg) != len(forge):
        return None
    free = list(range(len(forge)))
    order = []
    for digest in ffmpeg:
        for slot in free:
            if forge[slot] == digest:
                order.append(slot)
                free.remove(slot)
                break
        else:
            return None
    return order


# --- the stream's own facts --------------------------------------------------------------------


def unit_sizes(es: Path):
    """The length in bytes of each access unit, from the two-byte length words (TH spec 3.3)."""
    with es.open("rb") as f:
        while True:
            head = f.read(2)
            if len(head) < 2:
                return
            length = (int.from_bytes(head, "big") & 0xFFF) * 2
            if length < 4:
                return
            body = f.read(length - 2)
            if len(body) < length - 2:
                return
            yield length


def rates(es: Path, units_per_second: float) -> tuple[float, float]:
    """(mean, peak) of the stream's rate in Mbit/s; the peak is the highest over a sliding window
    of one second of access units (the whole stream when it is shorter)."""
    window = max(1, round(units_per_second))
    recent: deque[int] = deque()
    total = in_window = peak = count = 0
    for size in unit_sizes(es):
        total += size
        count += 1
        recent.append(size)
        in_window += size
        if len(recent) > window:
            in_window -= recent.popleft()
        peak = max(peak, in_window)
    seconds = count / units_per_second if units_per_second else 0
    mean = total * 8 / seconds / 1e6 if seconds else 0.0
    peak_seconds = min(len(recent), window) / units_per_second if units_per_second else 0
    return mean, (peak * 8 / peak_seconds / 1e6 if peak_seconds else 0.0)


def forge_probe(forge: str, path: Path) -> dict | None:
    result = run([forge, "probe", str(path), "json=1"])
    try:
        return json.loads(result.stdout)
    except ValueError:
        return None


FORGE_DECODE_LINES = {
    "format": re.compile(r"^(TrueHD|MLP) (\d+) Hz, (\d+) channel\(s\)(?: \(([^)]*)\))?"),
    "units": re.compile(r"(\d+) access unit\(s\) read, (\d+) decoded, (\d+) sample"),
    "restarts": re.compile(
        r"(\d+) restart header\(s\); lossless check: (\d+) interval\(s\) ok, (\d+) failed"
    ),
    "checks": re.compile(r"other checks .*failed: (\d+)"),
}


def forge_decode(forge: str, source: Path, wav: Path, lenient: bool = False):
    command = [forge, "decode", str(source), str(wav)] + (
        ["truehd-check=lenient"] if lenient else []
    )
    result = run(command)
    text = (result.stdout + b"\n" + result.stderr).decode(errors="replace")
    facts: dict = {"status": result.returncode, "errors": []}
    for line in text.splitlines():
        text_line = line.strip()
        for key, pattern in FORGE_DECODE_LINES.items():
            match = pattern.search(text_line)
            if match:
                facts[key] = match.groups()
        if text_line.startswith("error:") or "do not read as one" in text_line:
            facts["errors"].append(re.sub(r"^error: [^:]*: ", "error: ", text_line)[:160])
        if text_line.startswith("note:") and "AC-3" in text_line:
            facts["note"] = text_line
    return facts


# --- one track ---------------------------------------------------------------------------------


@dataclass
class Row:
    name: str
    container: str = ""
    track: str = ""
    verdict: str = ""
    cells: dict = field(default_factory=dict)
    notes: list = field(default_factory=list)
    failed: bool = False


def lossless_warnings(text: str) -> int:
    return len(re.findall(r"Lossless check failed", text))


def check_track(
    forge: str, path: Path, stream: dict, ordinal: int, kind: str, scratch: Path, label: str, args
) -> Row:
    row = Row(name=label, container=kind, track=f"{ordinal} (pid {stream.get('id', '?')})")
    channels = int(stream.get("channels") or 0)
    es = scratch / "track.thd"
    ok, error = extract(path, int(stream["index"]), es)
    if not ok:
        row.failed = True
        row.verdict = "FFmpeg could not extract the track"
        row.notes.append(error.strip().splitlines()[-1][:120] if error.strip() else "")
        return row
    probe = forge_probe(forge, es)
    truehd = (probe or {}).get("truehd")
    if not truehd:
        row.failed = True
        row.verdict = "forge cannot read the extracted stream"
        return row
    rate = truehd["sample_rate"]
    per_unit = truehd["samples_per_access_unit"]
    mean, peak = rates(es, rate / per_unit)
    row.cells.update(
        rate=f"{rate / 1000:g} kHz",
        duration=f"{truehd['access_units'] * per_unit / rate:.1f} s",
        units=truehd["access_units"],
        major_syncs=truehd["major_syncs"],
        mbps=f"{mean:.2f} / {peak:.2f}",
        substreams=truehd["substreams"],
        evolution=truehd.get("extra_data_units", 0),
        objects="yes" if truehd["object_substream"] else "no",
        refused=truehd["unreadable_units"] + truehd["skipped_bytes"] + truehd["segment_failures"],
    )
    # FFmpeg's decode.
    ffmpeg_digests, ffmpeg_log, ffmpeg_status = hash_ffmpeg(es, channels or 2)
    ffmpeg_lossless = lossless_warnings(ffmpeg_log)
    row.cells["ffmpeg_lossless"] = ffmpeg_lossless
    # forge's decode of the extracted stream; strict first, lenient to count what strict stops at.
    wav = scratch / "forge.wav"
    forge_facts = forge_decode(forge, es, wav)
    strict_failed = forge_facts["status"] != 0
    if strict_failed:
        row.notes.append("forge strict: " + ("; ".join(forge_facts["errors"][:1]) or "failed"))
        wav.unlink(missing_ok=True)
        forge_facts = forge_decode(forge, es, wav, lenient=True)
    forge_digests = hash_wav(wav) if wav.exists() else None
    wav.unlink(missing_ok=True)
    if "format" in forge_facts:
        _codec, _rate, count, layout = forge_facts["format"]
        row.cells["layout"] = f"{count} ch" + (f" ({layout})" if layout else "")
    if "restarts" in forge_facts:
        restarts, ok_intervals, failed_intervals = (int(x) for x in forge_facts["restarts"])
        row.cells["restarts"] = restarts
        row.cells["forge_lossless"] = f"{ok_intervals} ok / {failed_intervals} failed"
    else:
        failed_intervals = 0
    if forge_digests is None:
        row.failed = True
        row.verdict = "forge wrote no usable decode"
    elif ffmpeg_status != 0 and not ffmpeg_digests.frames:
        row.failed = True
        row.verdict = "FFmpeg refused the stream"
        row.notes.append((ffmpeg_log.strip().splitlines() or [""])[-1][:120])
    else:
        order = match_channels(ffmpeg_digests.digests(), forge_digests.digests())
        same_length = ffmpeg_digests.frames == forge_digests.frames
        if order is not None and same_length:
            row.verdict = "identical"
            if order != list(range(len(order))):
                note = f"FFmpeg channel order {order}"
                if EXPECTED_FFMPEG_ORDER.get(len(order)) == order:
                    note += " (its layout order, ERRATA 33)"
                row.notes.append(note)
        else:
            row.failed = True
            row.verdict = "DIFFERENT"
            row.notes.append(
                f"samples a channel: FFmpeg {ffmpeg_digests.frames}, forge {forge_digests.frames}"
                + ("" if order is not None else "; the channels' samples differ")
            )
        # Effective width: the lowest set bit of any sample, in the 24 bit word.
        low = ffmpeg_digests.low_bits_or
        row.cells["width"] = f"{24 - ((low & -low).bit_length() - 1) if low else 0} bit"
    if strict_failed or failed_intervals or ffmpeg_lossless or ffmpeg_status != 0:
        row.failed = True
    if ffmpeg_lossless:
        row.notes.append(f"FFmpeg: {ffmpeg_lossless} lossless check warning(s)")
    if ffmpeg_status != 0 and ffmpeg_digests.frames:
        row.notes.append(
            f"FFmpeg stopped (exit {ffmpeg_status}) after {ffmpeg_digests.frames} samples"
        )
    # The file as it is.
    core = "-"
    size_mib = path.stat().st_size / (1 << 20)
    if True:
        if size_mib <= args.max_container_mib:
            container_probe = forge_probe(forge, path)
            if container_probe is None:
                row.failed = True
                row.notes.append("forge probe json=1 of the file is not valid JSON")
            first = container_probe is not None and container_probe.get("truehd") is not None
            core = str((container_probe or {}).get("truehd", {}).get("ac3_core_frames", "-"))
            if first and ordinal == 0:
                wav2 = scratch / "container.wav"
                facts = forge_decode(forge, path, wav2, lenient=strict_failed)
                digests = hash_wav(wav2) if wav2.exists() else None
                wav2.unlink(missing_ok=True)
                if digests is None or facts["status"] != 0:
                    row.failed = True
                    row.notes.append(
                        "forge on the file: " + ("; ".join(facts["errors"][:1]) or "no decode")
                    )
                    row.cells["file_leg"] = "FAILED"
                elif forge_digests is not None and digests.digests() == forge_digests.digests():
                    row.cells["file_leg"] = "identical"
                else:
                    row.failed = True
                    row.cells["file_leg"] = "DIFFERENT"
                    row.notes.append("forge on the file differs from forge on the extracted stream")
                if facts.get("note"):
                    row.notes.append(facts["note"].replace("note: ", "")[:140])
            else:
                row.cells["file_leg"] = "skipped (forge's first stream here is not this track)"
        else:
            row.cells["file_leg"] = f"skipped (over {args.max_container_mib} MiB)"
    row.cells["ac3_core"] = core
    return row


AC3_KBPS = (32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640)


def classify_payload(body: bytes) -> str:
    """A for a PES payload that is one AC-3 frame, M for one that starts with an AC-3 frame and
    goes on, T for any other (access units), ? for an empty one. Only the 48 kHz frame sizes are
    known here; another rate's frame is A."""
    if not body:
        return "?"
    if body[:2] != b"\x0b\x77" or len(body) < 5:
        return "T"
    code = body[4] & 0x3F
    if body[4] >> 6 == 0 and code >> 1 < len(AC3_KBPS):
        return "A" if len(body) == AC3_KBPS[code >> 1] * 4 else "M"
    return "A"


def census_ts(path: Path, limit: int = 4000) -> str:
    """For a transport stream, how its elementary streams are laid out, as a line of letters:
    the PES packets of the first TrueHD PID in order (T access units, A AC-3 frames, M both, ?
    neither), with the PES header's stream_id and extension flags. Structure only, no bytes."""
    data = path.read_bytes()[: 192 * limit * 4]
    stride, offset = (192, 4) if data[4:5] == b"\x47" else (188, 0)
    pmt_pids, pids = set(), {}
    order: dict[int, list[bytearray]] = {}
    for at in range(0, len(data) - stride + 1, stride):
        pkt = data[at + offset : at + offset + 188]
        if pkt[0] != 0x47:
            continue
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        start = bool(pkt[1] & 0x40)
        body = 4
        if pkt[3] & 0x20:
            body += 1 + pkt[4]
        if not pkt[3] & 0x10:
            continue
        payload = pkt[body:]
        if pid == 0 and start:
            section = payload[1 + payload[0] :]
            for i in range(8, 3 + (((section[1] & 0xF) << 8) | section[2]) - 4, 4):
                if ((section[i] << 8) | section[i + 1]) != 0:
                    pmt_pids.add(((section[i + 2] & 0x1F) << 8) | section[i + 3])
        elif pid in pmt_pids and start and pid not in pids:
            section = payload[1 + payload[0] :]
            length = 3 + (((section[1] & 0xF) << 8) | section[2])
            info = ((section[10] & 0xF) << 8) | section[11]
            i = 12 + info
            while i + 5 <= length - 4:
                es_pid = ((section[i + 1] & 0x1F) << 8) | section[i + 2]
                pids[es_pid] = section[i]
                i += 5 + (((section[i + 3] & 0xF) << 8) | section[i + 4])
        elif pids.get(pid) == 0x83:
            if start:
                order.setdefault(pid, []).append(bytearray())
            if pid in order:
                order[pid][-1] += payload
    lines = [
        "PMT streams: " + ", ".join(f"pid {p:#x} type {t:#x}" for p, t in sorted(pids.items()))
    ]
    for pid, packets in order.items():
        letters, kinds = [], set()
        for packet in packets[:200]:
            if packet[:3] != b"\x00\x00\x01" or len(packet) < 9:
                letters.append("?")
                continue
            header = 9 + packet[8]
            kinds.add((packet[3], packet[7], packet[8]))
            body = bytes(packet[header:])
            letters.append(classify_payload(body))
        lines.append(f"pid {pid:#x}: first {len(letters)} PES packets: {''.join(letters)}")
        lines.append(
            "  PES (stream_id, flags2, header_data_length): "
            + ", ".join(f"({a:#x},{b:#x},{c})" for a, b, c in sorted(kinds)[:6])
        )
    return "\n".join(lines)


# --- the directory -----------------------------------------------------------------------------


COLUMNS = [
    ("file", "name"),
    ("container", "container"),
    ("track", "track"),
    ("rate", "rate"),
    ("layout", "layout"),
    ("width", "width"),
    ("duration", "duration"),
    ("units", "units"),
    ("restarts", "restarts"),
    ("Mbit/s mean / peak", "mbps"),
    ("AC-3 core frames", "ac3_core"),
    ("evolution units", "evolution"),
    ("objects", "objects"),
    ("forge lossless", "forge_lossless"),
    ("ffmpeg lossless warnings", "ffmpeg_lossless"),
    ("refused", "refused"),
    ("file leg", "file_leg"),
    ("forge = ffmpeg", "verdict"),
]


def render(rows: list[Row]) -> str:
    table = [[title for title, _ in COLUMNS]]
    for row in rows:
        values = []
        for _title, key in COLUMNS:
            if key in ("name", "container", "track", "verdict"):
                values.append(str(getattr(row, key)))
            else:
                values.append(str(row.cells.get(key, "")))
        table.append(values)
    widths = [max(len(r[i]) for r in table) for i in range(len(COLUMNS))]
    lines = ["  ".join(cell.ljust(widths[i]) for i, cell in enumerate(r)).rstrip() for r in table]
    lines.insert(1, "  ".join("-" * w for w in widths))
    for row in rows:
        for note in row.notes:
            if note:
                lines.append(f"  {row.name} track {row.track}: {note}")
    return "\n".join(lines)


def check_directory(directory: Path, forge: str, args) -> tuple[list[Row], list[str]]:
    rows, census = [], []
    files = sorted(p for p in directory.iterdir() if p.is_file() and p.suffix.lower() in EXTENSIONS)
    with tempfile.TemporaryDirectory(prefix="truehd-local-") as scratch_name:
        scratch = Path(scratch_name)
        for number, path in enumerate(files, 1):
            label = f"file-{number}" if args.anonymous else path.name
            head = path.open("rb").read(4096)
            kind = detect_container(head)
            streams = [s for s in ffprobe_streams(path) if s.get("codec_name") in ("truehd", "mlp")]
            if not streams:
                row = Row(name=label, container=kind, verdict="no TrueHD or MLP track")
                row.notes.append(
                    "ffprobe lists none" if kind != "unknown" else "not a container FFmpeg reads"
                )
                rows.append(row)
                continue
            if kind in ("mpegts", "m2ts"):
                census.append(f"{label}:\n" + census_ts(path))
            for ordinal, stream in enumerate(streams):
                track_scratch = scratch / f"{number}-{ordinal}"
                track_scratch.mkdir()
                rows.append(
                    check_track(forge, path, stream, ordinal, kind, track_scratch, label, args)
                )
                shutil.rmtree(track_scratch, ignore_errors=True)
    return rows, census


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dir", default=os.environ.get(ENV_DIR, ""), help=f"default ${ENV_DIR}")
    parser.add_argument(
        "--forge",
        default=os.environ.get(ENV_CLI, "") or shutil.which("forge") or "",
        help=f"default ${ENV_CLI}",
    )
    parser.add_argument(
        "--max-container-mib", type=int, default=2048, help="the file leg's size limit"
    )
    parser.add_argument(
        "--anonymous", action="store_true", help="number the files instead of naming them"
    )
    parser.add_argument("--report", help="also write the table to this file")
    parser.add_argument(
        "--self-test", action="store_true", help="check stand-ins built from the fixtures"
    )
    args = parser.parse_args(argv)

    if args.self_test:
        import truehd_standins  # noqa: PLC0415

        return truehd_standins.self_test(args, check_directory, render)
    if not args.dir or not Path(args.dir).is_dir():
        print(f"SKIP: set {ENV_DIR} to a directory of TrueHD tracks (.m2ts, .ts, .mkv, .mp4, .thd)")
        return 0
    if not args.forge or not Path(args.forge).exists():
        print(f"SKIP: no forge binary (set {ENV_CLI} or pass --forge)")
        return 0
    if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
        print("SKIP: ffmpeg and ffprobe are needed on PATH")
        return 0
    rows, census = check_directory(Path(args.dir), args.forge, args)
    text = render(rows)
    if census:
        text += "\n\nTransport stream layout (structure only):\n" + "\n".join(census)
    bad = sum(row.failed for row in rows)
    text += (
        f"\n\n{len(rows)} track(s), {bad} with a difference, a refusal or a failed lossless check"
    )
    print(text)
    if args.report:
        Path(args.report).write_text(text + "\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.exit(main())
