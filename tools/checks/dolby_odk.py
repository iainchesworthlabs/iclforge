#!/usr/bin/env python3
"""Dolby's Online Delivery Kit test tools as optional, local-only oracles.

Dolby publishes, free of charge on ott.dolby.com, kits for delivering Dolby Digital Plus and
Dolby AC-4 online. The AC-4 kit's `Test_Tools/` holds two native command-line programs that
read what this project writes: `streamdemuxer` (demultiplexes MP4, fragmented MP4, DASH, HLS and
MPEG-2 TS, and decrypts common-encryption files given clear keys) and `mp4drop` (dumps the boxes
of an ISO-BMFF file as XML). An independent reader written by the format's owner is the
strongest outside check an MP4 or DASH writer can get, so the checks in `tools/checks/` that
start `check_dolby_` run them when they are present and skip when they are not.

What this module does and does not do.

- It finds the tools through the environment variable ICLFORGE_DOLBY_ODK_TOOLS, a directory that
  holds `streamdemuxer` and `mp4drop` directly, or in the kit's own layout
  (`<dir>/streamdemuxer/Linux64/streamdemuxer`), or in a `Linux64`/`Mac`/`Win64` folder.
- It runs a tool the careful way, because the program is Dolby's, not this project's: an empty
  environment (`env -i` semantics), no network where `unshare -n` works, a temporary working
  directory, a time limit and a cap on the output it keeps.
- `python3 tools/checks/dolby_odk.py fetch <cache-dir>` is the one opt-in step that touches the
  network. It reads the two tool archives out of the AC-4 kit's zip on ott.dolby.com with HTTP range
  requests (the kit is 1.1 GB; the two archives are 1.3 MB), checks each against the SHA-256 recorded
  here, and extracts the member for this platform only. Nothing is fetched by a test or by CI.
- Nothing from the kit is committed: not the programs, not the archives, not Dolby's streams, not
  XML dumps of Dolby's files. The kit is Dolby's and this project does not redistribute it. The
  project has not read the kit's licence terms in full; whether CI may fetch it is the owner's
  decision (docs/verification.md, "Dolby's Online Delivery Kit tools").

Exit status of the command line: 0 for success, 1 for an error, 77 when `where` finds no tools (the
convention automake uses for a skipped test).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import platform
import shutil
import ssl
import subprocess
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

ENV_VAR = "ICLFORGE_DOLBY_ODK_TOOLS"
SKIP_EXIT = 77

KIT_URL = "https://ott.dolby.com/OnDelKits/AC-4/Dolby_AC-4_Online_Delivery_Kit_1.5.zip"
KIT_PREFIX = "Dolby_AC-4_Online_Delivery_Kit_1.5/Test_Tools/"
# Recorded on 2026-10-29 from the kit published at KIT_URL. A different digest means Dolby
# changed the kit (a newer tool, perhaps), not that anything is broken: look before updating.
TOOL_ARCHIVES = {
    "streamdemuxer": "9b8fbecd74d6d3ebe4ec90114a3a436ff47bdb60e987e5321ca3433272b7f284",
    "mp4drop": "9740b75f63d2be3548287dc923d777078d91f29635183d5eb386e83b71a9f596",
}
TOOL_NAMES = tuple(TOOL_ARCHIVES)

MAX_OUTPUT_BYTES = 256 * 1024 * 1024  # an mp4drop dump of a long file is tens of megabytes
DEFAULT_TIMEOUT_S = 300.0
MAX_XML_BYTES = 64 * 1024 * 1024


def platform_folder() -> str:
    """The kit's folder name for this machine, or '' when the kit has no build for it."""
    system = platform.system()
    machine = platform.machine().lower()
    if system == "Linux" and machine in ("x86_64", "amd64"):
        return "Linux64"
    if system == "Windows" and machine in ("amd64", "x86_64"):
        return "Win64"
    if system == "Darwin":
        return "Mac"
    return ""


def tool_filename(name: str) -> str:
    return name + (".exe" if platform.system() == "Windows" else "")


def find_tool(name: str, root: str | os.PathLike[str] | None = None) -> Path | None:
    """Where `name` is under the tools directory, or None."""
    base = root if root is not None else os.environ.get(ENV_VAR, "")
    if not base:
        return None
    base_path = Path(base)
    folder = platform_folder()
    exe = tool_filename(name)
    candidates = [base_path / exe]
    if folder:
        candidates += [base_path / folder / exe, base_path / name / folder / exe]
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def skip_reason(*names: str, root: str | os.PathLike[str] | None = None) -> str | None:
    """A one-line reason to skip, or None when every named tool is present."""
    base = root if root is not None else os.environ.get(ENV_VAR, "")
    if not base:
        return f"{ENV_VAR} is not set (see tools/checks/dolby_odk.py fetch)"
    for name in names or TOOL_NAMES:
        if find_tool(name, root) is None:
            return f"{name} not found under {base} for {platform_folder() or platform.system()}"
    return None


@dataclass
class ToolResult:
    returncode: int
    stdout: bytes
    stderr: bytes
    truncated: bool = False
    timed_out: bool = False
    sandboxed: bool = False
    files: list[Path] = field(default_factory=list)

    @property
    def text(self) -> str:
        return self.stdout.decode("latin-1", errors="replace")

    @property
    def err_text(self) -> str:
        return self.stderr.decode("latin-1", errors="replace")


def _unshare_works() -> bool:
    unshare = shutil.which("unshare")
    if unshare is None:
        return False
    try:
        return subprocess.run([unshare, "-n", "true"], capture_output=True, timeout=10).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def _read_capped(path: Path, limit: int) -> tuple[bytes, bool]:
    with path.open("rb") as handle:
        data = handle.read(limit + 1)
    return data[:limit], len(data) > limit


def run_tool(
    name: str,
    args: list[str | os.PathLike[str]],
    *,
    root: str | os.PathLike[str] | None = None,
    cwd: str | os.PathLike[str] | None = None,
    timeout: float = DEFAULT_TIMEOUT_S,
    max_output: int = MAX_OUTPUT_BYTES,
) -> ToolResult:
    """Run one of Dolby's tools with an empty environment, no network (when possible) and limits.

    `cwd` is where the tool runs and writes relative paths; it defaults to a fresh temporary
    directory that is removed afterwards (and then the tool can only be given absolute paths).
    """
    exe = find_tool(name, root)
    if exe is None:
        raise FileNotFoundError(f"{name} is not installed under {ENV_VAR}")
    argv = [str(exe), *(str(a) for a in args)]
    sandboxed = _unshare_works()
    if sandboxed:
        argv = [shutil.which("unshare") or "unshare", "-n", *argv]
    own_dir = None
    if cwd is None:
        own_dir = tempfile.TemporaryDirectory(prefix="dolby-odk-")
        cwd = own_dir.name
    scratch = Path(tempfile.mkdtemp(prefix="dolby-odk-io-"))
    out_path, err_path = scratch / "stdout", scratch / "stderr"
    env = {"PATH": "/usr/bin:/bin", "LC_ALL": "C"}
    timed_out = False
    try:
        with out_path.open("wb") as out, err_path.open("wb") as err:
            try:
                proc = subprocess.run(
                    argv, stdin=subprocess.DEVNULL, stdout=out, stderr=err, cwd=cwd, env=env,
                    timeout=timeout, check=False,
                )
                code = proc.returncode
            except subprocess.TimeoutExpired:
                code, timed_out = -1, True
        stdout, cut_out = _read_capped(out_path, max_output)
        stderr, cut_err = _read_capped(err_path, max_output)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
        if own_dir is not None:
            own_dir.cleanup()
    return ToolResult(code, stdout, stderr, cut_out or cut_err, timed_out, sandboxed)


# --- mp4drop -------------------------------------------------------------------------------------


@dataclass
class Box:
    """One box of an mp4drop dump: its four-character name, size, scalar fields and children."""

    kind: str
    size: int
    fields: dict[str, list[str]] = field(default_factory=dict)
    children: list["Box"] = field(default_factory=list)

    def find(self, kind: str) -> "Box | None":
        for child in self.children:
            if child.kind == kind:
                return child
        return None

    def find_all(self, kind: str) -> list["Box"]:
        return [c for c in self.children if c.kind == kind]

    def walk(self, path: str = ""):
        here = f"{path}/{self.kind}" if path else self.kind
        yield here, self
        for child in self.children:
            yield from child.walk(here)

    def value(self, name: str, default: str | None = None) -> str | None:
        values = self.fields.get(name)
        return values[0] if values else default

    def int_value(self, name: str) -> int | None:
        text = self.value(name)
        if text is None:
            return None
        try:
            return int(text, 0)
        except ValueError:
            return None


def _box_from_element(element: ET.Element) -> Box:
    size_text = element.attrib.get("size", "0")
    try:
        size = int(size_text)
    except ValueError:
        size = 0
    box = Box(element.tag.strip(), size)
    for child in element:
        if "size" in child.attrib:
            box.children.append(_box_from_element(child))
        else:
            box.fields.setdefault(child.tag, []).append((child.text or "").strip())
    return box


def parse_mp4drop_xml(text: str) -> list[Box]:
    """The top-level boxes of an mp4drop XML dump (`-m 1`).

    Dolby's tool declares ISO-8859-1 and writes box names as element names, including names that
    are not valid XML element names for some brands; a dump that does not parse raises ValueError.
    """
    if len(text) > MAX_XML_BYTES:
        raise ValueError("mp4drop dump is larger than the parser accepts")
    try:
        root = ET.fromstring(text.encode("latin-1", errors="replace"))
    except ET.ParseError as error:
        raise ValueError(f"mp4drop output is not well-formed XML: {error}") from error
    return [_box_from_element(child) for child in root if "size" in child.attrib]


def mp4drop_boxes(path: str | os.PathLike[str], *, root: str | os.PathLike[str] | None = None,
                  timeout: float = DEFAULT_TIMEOUT_S) -> list[Box]:
    result = run_tool("mp4drop", [Path(path).resolve(), "-m", "1"], root=root, timeout=timeout)
    if result.timed_out or result.truncated:
        raise RuntimeError("mp4drop timed out or produced more output than the limit")
    if result.returncode != 0:
        raise RuntimeError(f"mp4drop exited {result.returncode}: {result.err_text.strip()[:200]}")
    return parse_mp4drop_xml(result.text)


def audio_sample_entries(boxes: list[Box]) -> list[tuple[int, Box]]:
    """(track index, sample-entry box) for every audio sample entry in the dump."""
    found: list[tuple[int, Box]] = []
    track_index = 0
    for top in boxes:
        if top.kind != "moov":
            continue
        for trak in top.find_all("trak"):
            track_index += 1
            for path, box in trak.walk():
                if path.endswith("/stsd") or path == "stsd":
                    for entry in box.children:
                        if entry.value("channelcount") is not None:
                            found.append((track_index, entry))
    return found


# --- streamdemuxer -------------------------------------------------------------------------------


def streamdemuxer(
    input_path: str | os.PathLike[str],
    out_dir: str | os.PathLike[str],
    *,
    keys: dict[str, str] | None = None,
    track_id: int | None = None,
    root: str | os.PathLike[str] | None = None,
    timeout: float = DEFAULT_TIMEOUT_S,
) -> ToolResult:
    """Demultiplex with Dolby's tool. `keys` maps 32-hex key ids to 32-hex keys.

    The result's `files` lists what the tool wrote under `out_dir`, in path order.
    """
    out = Path(out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    args: list[str | os.PathLike[str]] = ["-i", Path(input_path).resolve(), "-o", out]
    if keys:
        args += ["--decryption-keys", ",".join(f"{k}:{v}" for k, v in keys.items())]
    if track_id is not None:
        args += ["--track-id", str(track_id)]
    result = run_tool("streamdemuxer", args, root=root, timeout=timeout)
    result.files = sorted(p for p in out.rglob("*") if p.is_file())
    return result


# --- reporting -----------------------------------------------------------------------------------

PASS, DISAGREE, SKIP, ERROR, NOTE = "pass", "disagree", "skip", "error", "note"


@dataclass
class Result:
    """One comparison. `disagree` and `error` fail a check; `note` records a difference that is
    not a defect (two writers that differ and are both right); `skip` says why a leg did not run."""

    subject: str
    status: str
    detail: str = ""


def summarise(results: list[Result]) -> dict[str, int]:
    counts = {PASS: 0, DISAGREE: 0, SKIP: 0, ERROR: 0, NOTE: 0}
    for result in results:
        counts[result.status] += 1
    return counts


def print_report(title: str, results: list[Result]) -> int:
    """Print every non-passing result and a count line. Returns the process exit status: 1 when
    anything disagreed or failed, 0 otherwise."""
    print(f"== {title}")
    for result in results:
        if result.status != PASS:
            print(f"  {result.status.upper():8} {result.subject}: {result.detail}")
    counts = summarise(results)
    print(
        f"  {counts[PASS]} pass, {counts[DISAGREE]} disagree, {counts[ERROR]} error, "
        f"{counts[NOTE]} note, {counts[SKIP]} skip"
    )
    return 1 if counts[DISAGREE] or counts[ERROR] else 0


# --- fetch ---------------------------------------------------------------------------------------


class _HttpRangeFile(io.RawIOBase):
    """A read-only seekable view of a remote file, fetched in ranges, so a zip's central
    directory and two small members can be read without downloading the 1.1 GB kit."""

    def __init__(self, url: str, context: ssl.SSLContext):
        self._url, self._context, self._pos = url, context, 0
        head = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(head, context=context, timeout=60) as response:  # noqa: S310
            self._size = int(response.headers["Content-Length"])

    def seekable(self) -> bool:
        return True

    def readable(self) -> bool:
        return True

    def tell(self) -> int:
        return self._pos

    def seek(self, offset: int, whence: int = 0) -> int:
        if whence == 0:
            self._pos = offset
        elif whence == 1:
            self._pos += offset
        else:
            self._pos = self._size + offset
        return self._pos

    def read(self, size: int = -1) -> bytes:
        if size < 0 or self._pos + size > self._size:
            size = self._size - self._pos
        if size <= 0:
            return b""
        request = urllib.request.Request(
            self._url, headers={"Range": f"bytes={self._pos}-{self._pos + size - 1}"}
        )
        with urllib.request.urlopen(request, context=self._context, timeout=120) as response:  # noqa: S310
            data = response.read()
        self._pos += len(data)
        return data

    def readinto(self, buffer) -> int:  # type: ignore[override]
        data = self.read(len(buffer))
        buffer[: len(data)] = data
        return len(data)


def _safe_member(name: str) -> bool:
    return not (name.startswith("/") or ".." in Path(name).parts or "\\" in name)


def extract_platform_member(archive: bytes, tool: str, destination: Path) -> Path:
    """Write the platform's binary of `tool` from its archive; refuses unsafe member names."""
    folder = platform_folder()
    if not folder:
        raise RuntimeError(f"the kit has no {tool} for {platform.system()} {platform.machine()}")
    wanted = f"{tool}/{folder}/{tool_filename(tool)}"
    with zipfile.ZipFile(io.BytesIO(archive)) as zf:
        for info in zf.infolist():
            if not _safe_member(info.filename):
                raise RuntimeError(f"unsafe member name in the {tool} archive: {info.filename!r}")
        if wanted not in zf.namelist():
            raise RuntimeError(f"{wanted} is not in the {tool} archive")
        target = destination / tool_filename(tool)
        target.write_bytes(zf.read(wanted))
        target.chmod(0o755)
    return target


def fetch(cache_dir: str | os.PathLike[str], *, url: str = KIT_URL) -> list[Path]:
    """Download the two tool archives from the kit, verify them and extract this platform's
    binaries into cache_dir. Returns the extracted paths."""
    context = ssl.create_default_context()
    destination = Path(cache_dir)
    destination.mkdir(parents=True, exist_ok=True)
    remote = io.BufferedReader(_HttpRangeFile(url, context), buffer_size=1 << 20)
    extracted: list[Path] = []
    with zipfile.ZipFile(remote) as kit:
        for tool, expected in TOOL_ARCHIVES.items():
            member = f"{KIT_PREFIX}{tool}.zip"
            data = kit.read(member)
            digest = hashlib.sha256(data).hexdigest()
            if digest != expected:
                raise RuntimeError(
                    f"{member}: sha256 {digest} differs from the recorded {expected}; "
                    "Dolby may have changed the kit. Look at it, then update TOOL_ARCHIVES."
                )
            extracted.append(extract_platform_member(data, tool, destination))
    return extracted


NOTICE = (
    "These programs are Dolby's, from the free Online Delivery Kit. This project does not "
    "redistribute them and does not commit them or anything made with them; keep them local."
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    fetch_parser = sub.add_parser("fetch", help="download streamdemuxer and mp4drop into a directory")
    fetch_parser.add_argument("cache_dir")
    sub.add_parser("where", help="print where the tools are, or why they are not found")
    args = parser.parse_args(argv)
    if args.command == "fetch":
        try:
            paths = fetch(args.cache_dir)
        except (OSError, RuntimeError, zipfile.BadZipFile, KeyError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
        for path in paths:
            print(path)
        print(f"set {ENV_VAR}={Path(args.cache_dir).resolve()}")
        print(NOTICE)
        return 0
    reason = skip_reason()
    if reason:
        print(f"skip: {reason}")
        return SKIP_EXIT
    for name in TOOL_NAMES:
        print(f"{name}: {find_tool(name)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
