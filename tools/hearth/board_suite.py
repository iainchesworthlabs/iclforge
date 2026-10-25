#!/usr/bin/env python3
"""The board suite: play a profile's fixed list on a booked board and hold it to a baseline.

    python tools/hearth/board_suite.py plan   --profile s3-n16r8
    python tools/hearth/board_suite.py run    --profile s3-n16r8 --board S3b \\
        --session 16d036 --purpose baseline [--restart] [--record]
    python tools/hearth/board_suite.py compare --run board-run.json \\
        --baseline baselines/s3-n16r8.json
    python tools/hearth/board_suite.py lock   take|release|show --file LOCK --session ID

What it does (planning/esp32-platform-profiles.md, "The board suite"), and stops at the first
refusal:

1. **Booking.** `run` takes the lock file itself, atomically, and refuses with no lock and
   `--session`, or when the file exists: the rule is that a board nobody has booked is not one
   to run on (planning/esp32-board-bench.md). `--board` names a board of bench.json, which gives
   the address and the lock and refuses a pair that is not its own. It prints what a script
   cannot check (the other sessions, a running idf.py or esptool) as a checklist first, and
   releases the lock when it ends, whatever happened, if it took it.
2. **Identity.** GET /hardware must say the profile and the hash of its current definition
   (`sink_profile.py identity`), and GET /status `stopped`. A board running another profile, an
   older definition or an image built with neither is refused: its figures are not this
   profile's.
3. **Plays.** The repository's fixture streams are served from this machine and each row of the
   profile's list (firmware/hearth-sink/suite.json) is played over POST /play, polled to its end,
   and its lines read from GET /log. A row with `"via": "sendspin"` is played the way the C6's
   product path is: the repository's `hearth-testserver` pairs the board and sends the stream,
   and the board's `sendspin.stream=` line is read (tools/hearth/suite_sendspin.py).
4. **Asserts**, per row: finished and not failed, every unit, no resynchronised byte, no sink
   underrun, no failed allocation, no reboot, and for AC-4 the PCM hash the host pins
   (testdata/ac4-probe-pcm-hashes.json). A row that expects the C6's refusal expects state
   failed, why "memory", and no reboot. A row that names a `layout` needs the board to carry it,
   and the suite asks the board (GET /layout-options, the firmware's one verdict over its
   capabilities, which PUT /layout applies too): where the board carries the layout the row is
   played; where it does not, `if_unavailable` says what the row is: "refuse" (the default) has
   PUT /layout answer 409 with the board's own reason, which is the row's pass, and "skip" leaves
   the row out with the reason stated. A layout carried with a limit is played and the limit
   noted. Neither is a play, so neither has figures beyond its state and reason.
5. **Figures**, per row: the decoder's time per frame (`us_per_frame` less the sink's and the
   renderer's, over the frame's 32 ms; an AC-4 play's own `decoder_permille`), the decode stack
   left, the sink queue's least headroom. On an I2S sink `realtime_permille` is not one of them:
   it includes the wait for the DAC and reads 981 to 992 for any stereo play.
6. **Baseline.** `--record` writes baselines/<profile>.json from a clean run; `--compare` holds a
   run to it with the suite's tolerance, the profile's own over it, and prints an improvement
   as "re-baseline". A board's boot figures vary from boot to boot: `--restart --boots N`
   restarts N times and holds the worst.

Nothing the board says about its network is read or kept: /status carries the SSID and this
takes the fields it wants by name.

Standard library only, as the CI container's Python has nothing else.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import http.client
import http.server
import json
import re
import socket
import socketserver
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field
from functools import partial
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench
import bench_flash
import sink_profile
import suite_sendspin

REPO = Path(__file__).resolve().parents[2]
PROJECT = REPO / "firmware" / "hearth-sink"
SUITE = PROJECT / "suite.json"
BASELINES = PROJECT / "baselines"
PINS = REPO / "testdata" / "ac4-probe-pcm-hashes.json"

FORMAT = 1

# The most a board's GET /log answers with in one reply (firmware/esp-idf/iclforge/src/
# control.cpp, kLogReplyBytes), and how many replies one read follows (a 256 KB ring).
LOG_REPLY_BYTES = 4096
MAX_LOG_PAGES = 64

# The fields of GET /status the suite reads, by name. The network object is never one.
STATUS_FIELDS = (
    "state",
    "location",
    "frames",
    "finished",
    "failed",
    "why",
    "error",
    "us_per_frame",
    "render_us_per_frame",
    "sink_us_per_frame",
    "worst_frame_us",
    "ring_low",
    "resync_bytes",
    "layout",
    "stream",
)

CHECKLIST = (
    "the suite cannot see another session. Before it runs, by hand:",
    "  - read every lock file (python tools/hearth/bench.py status)",
    "  - ListAgents: is a peer building or testing near this board?",
    "  - no idf.py, esptool, ota.py or capture of this board is running",
    "  - the board's /status is stopped (the suite checks that one)",
)

# The booking is the bench's (bench.py): the suite and the flash tool take the same lines.
SuiteError = bench.BenchError


# --- booking --------------------------------------------------------------------------


def take_lock(path: Path, session: str, purpose: str, minutes: int) -> str:
    """The booking line, written atomically: a lock that exists is not taken over."""
    return bench.take_lock(path, session, purpose, minutes, "board suite")


def hold_lock(path: Path, session: str, purpose: str, minutes: int) -> tuple[str, bool]:
    """The booking for a run: the lock taken, or, where the file is already this session's (it
    booked the board to do something first, such as put an image on it), that line, which the
    run then leaves for its owner to release. Returns the line and whether the run took it."""
    return bench.hold_lock(path, session, purpose, minutes, "board suite")


release_lock = bench.release_lock


def resolve_board(args: argparse.Namespace) -> None:
    """Fills --host and --lock from the bench's row for --board, and refuses a pair that
    disagrees with it: a lock that is not the board's is the mistake the rule exists for. A
    --board that is not on the bench is only a label for the record."""
    if not args.board:
        return
    try:
        row = bench.load().boards.get(args.board)
    except bench.BenchError:
        row = None
    if row is None:
        return
    if args.host and args.host != row.host:
        raise SuiteError(f"--board {row.name} is at {row.host}, not {args.host}")
    if args.lock and Path(args.lock).name != row.lock.name:
        raise SuiteError(f"--board {row.name} is guarded by {row.lock.name}, not {args.lock}")
    if (
        args.restart
        and row.restart_kills_c6_link
        and args.reset_by != "esptool"
        and not args.accept_restart_fault
    ):
        raise SuiteError(
            f"{row.name}: a restart can leave a P4 with no Wi-Fi (its C6 link), and --restart "
            "restarts it by POST /restart; restart it with --reset-by esptool (a watchdog reset), "
            "or pass --accept-restart-fault (planning/esp32-board-bench.md has the recovery)"
        )
    args.host = args.host or row.host
    args.lock = args.lock or str(row.lock)


# --- the board ------------------------------------------------------------------------


class Board:
    """A hearth_sink board's control surface, as far as the suite reads it."""

    def __init__(self, host: str, port: int = 80, timeout: float = 10.0) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout

    def _request(
        self, method: str, path: str, body: bytes | None = None
    ) -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        try:
            connection.request(method, path, body=body)
            response = connection.getresponse()
            data = response.read()
            return response.status, {k.lower(): v for k, v in response.getheaders()}, data
        except (OSError, http.client.HTTPException) as error:
            raise SuiteError(f"{self.host}: {method} {path}: {error}") from error
        finally:
            connection.close()

    def json(self, path: str) -> dict[str, Any]:
        status, _, data = self._request("GET", path)
        if status != 200:
            raise SuiteError(f"{self.host}: GET {path} answered {status}")
        try:
            return json.loads(data.decode("utf-8", "replace"))
        except ValueError as error:
            raise SuiteError(f"{self.host}: GET {path} is not JSON: {error}") from error

    def hardware(self) -> dict[str, Any]:
        return self.json("/hardware")

    def status(self) -> dict[str, Any]:
        """/status's fields the suite reads, by name; the network object is not among them."""
        raw = self.json("/status")
        return {key: raw.get(key) for key in STATUS_FIELDS}

    def play(self, url: str) -> int:
        return self._request("POST", "/play", url.encode("utf-8"))[0]

    def layout(self) -> str:
        """GET /layout: the output layout the next play renders onto, as text."""
        status, _, data = self._request("GET", "/layout")
        if status != 200:
            raise SuiteError(f"{self.host}: GET /layout answered {status}")
        return data.decode("utf-8", "replace").strip()

    def set_layout(self, name: str) -> int:
        """PUT /layout: the status code (200, or 409 for a layout the board cannot carry)."""
        return self.put_layout(name)[0]

    def put_layout(self, name: str) -> tuple[int, str]:
        """PUT /layout: the status code and the board's words. A layout the board cannot carry is a
        409 whose body is the reason (iclforge/layout_options.hpp), the sentence GET /layout-options
        lists for it; one it takes with a limit is a 200 that says the limit after the ok."""
        status, _, data = self._request("PUT", "/layout", name.encode("utf-8"))
        return status, data.decode("utf-8", "replace").strip()

    def layout_options(self) -> dict[str, Any] | None:
        """GET /layout-options: what the board can be set to, layout by layout, and why not, as the
        firmware's one verdict over its capabilities (iclforge/layout_options.hpp in libs/device).
        None on a firmware that has no such route (a 404), which a row is then played as before."""
        status, _, data = self._request("GET", "/layout-options")
        if status == 404:
            return None
        if status != 200:
            raise SuiteError(f"{self.host}: GET /layout-options answered {status}")
        try:
            options = json.loads(data.decode("utf-8", "replace"))
        except ValueError as error:
            raise SuiteError(f"{self.host}: GET /layout-options is not JSON: {error}") from error
        if not isinstance(options, dict) or not isinstance(options.get("options"), list):
            raise SuiteError(f"{self.host}: GET /layout-options has no list of options")
        return options

    def sendspin(self) -> dict[str, Any]:
        """/status's `sendspin` object, the fields the suite reads by name (the network object is
        another one and is not read)."""
        raw = self.json("/status").get("sendspin") or {}
        return {key: raw.get(key) for key in suite_sendspin.SENDSPIN_FIELDS}

    def pairing(self) -> dict[str, Any]:
        """GET /pairing: the servers the board is paired with (their ids and names)."""
        return self.json("/pairing")

    def forget(self, server_id: str) -> int:
        """POST /pairing `forget <server_id>`: that one server's record. Never a bare `forget`,
        which the board reads as every record, the owner's Hearth among them."""
        if not server_id or not server_id.strip():
            raise SuiteError("forget needs a server id: a bare forget drops every pairing record")
        return self._request("POST", "/pairing", f"forget {server_id.strip()}".encode())[0]

    def restart(self, wait: float = 120.0, sleep: float = 2.0) -> None:
        """POST /restart, then wait for the board to answer again and to be stopped."""
        self._request("POST", "/restart")
        time.sleep(sleep)
        self.wait_stopped(wait, sleep)

    def wait_stopped(self, wait: float = 120.0, sleep: float = 2.0) -> None:
        """Wait for the board to answer and to be stopped."""
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            try:
                if self.json("/status").get("state") == "stopped":
                    return
            except SuiteError:
                pass
            time.sleep(sleep)
        raise SuiteError(f"{self.host} did not come back from its restart in {wait:.0f} s")

    def stop(self) -> None:
        with contextlib.suppress(SuiteError):
            self._request("POST", "/stop")

    def log(self, start: int | None) -> tuple[str, int | None]:
        """The console lines after byte `start` (all the ring holds for None) to the end of the
        log, and the offset after. One reply is at most LOG_REPLY_BYTES (control.cpp's
        kLogReplyBytes): a client follows X-Log-Next for the rest, and a whole-log read that did
        not left its mark in the middle of a 16 KB ring and the P4's boot line, past the first
        4 KB of a noisy boot, unread."""
        data = b""
        cursor = start
        following_number: int | None = None
        for _ in range(MAX_LOG_PAGES):
            path = "/log" if cursor is None else f"/log?from={cursor}"
            status, headers, page = self._request("GET", path)
            if status != 200:
                raise SuiteError(f"{self.host}: GET {path} answered {status}")
            following = headers.get("x-log-next", "")
            following_number = int(following) if following.isdigit() else None
            data += page
            if following_number is None or len(page) < LOG_REPLY_BYTES:
                break
            if cursor is not None and following_number <= cursor:
                break  # the log is shorter than where it was: the caller reads that as a restart
            cursor = following_number
        return data.decode("utf-8", "replace"), following_number


# --- reading a play -------------------------------------------------------------------

_FIELD = re.compile(r"([A-Za-z_.\[\]0-9]+)=(-?[0-9A-Za-z_.:/-]+)")


def fields(line: str) -> dict[str, str]:
    return dict(_FIELD.findall(line))


@dataclass
class PlayLines:
    """What a play's console lines say."""

    verdict: str | None = None
    units: int | None = None
    resync_bytes: int | None = None
    decode_stack_free: int | None = None
    underruns: int | None = None
    dry_ms: int | None = None
    min_headroom_ms: int | None = None
    failed_allocations: int = 0
    ac4_decoder_permille: int | None = None
    ac4_pcm_hash: str | None = None
    heap_internal_free: int | None = None
    heap_largest: int | None = None
    refused: str | None = None


def parse_play_lines(lines: list[str]) -> PlayLines:
    """The facts of one play from its console lines, the last of each kind winning."""
    found = PlayLines()
    for raw in lines:
        line = raw.strip()
        if line.startswith("result="):
            found.verdict = line.partition("=")[2].strip()
        elif "could not allocate" in line:
            found.failed_allocations += 1
        elif line.startswith("sink.writes=") and "sink.dma_ms=" in line:
            values = fields(line)
            found.underruns = _int(values.get("sink.underruns"))
            found.dry_ms = _int(values.get("sink.dry_ms"))
            found.min_headroom_ms = _int(values.get("sink.min_headroom_ms"))
        elif line.startswith("stream.units=") and "stream.wall_ms=" in line:
            values = fields(line)
            found.units = _int(values.get("stream.units"))
            found.resync_bytes = _int(values.get("stream.resync_bytes"))
            found.decode_stack_free = _int(values.get("stream.decode_stack_free"))
        elif line.startswith("ac4.") and "pcm_hash=" in line:
            values = fields(line)
            found.ac4_decoder_permille = _int(values.get("decoder_permille"))
            found.ac4_pcm_hash = values.get("pcm_hash")
        elif line.startswith("heap: internal free"):
            match = re.match(r"heap: internal free (\d+) \(largest block (\d+)\)", line)
            if match:
                found.heap_internal_free = int(match.group(1))
                found.heap_largest = int(match.group(2))
        elif line.startswith("player: a play here needs"):
            found.refused = line
    return found


def _int(text: str | None) -> int | None:
    try:
        return int(text) if text is not None else None
    except ValueError:
        return None


def decoder_permille(status: dict[str, Any], frame_us: int) -> int | None:
    """The decoder's time for a frame over the frame's duration, in permille: `us_per_frame`
    less what the sink's write and the renderer took, which on a paced sink includes the wait
    for the DAC. None where the play had no frames."""
    frames = status.get("frames") or 0
    total = status.get("us_per_frame")
    if not frames or total is None:
        return None
    others = (status.get("render_us_per_frame") or 0) + (status.get("sink_us_per_frame") or 0)
    return max(0, round((total - others) * 1000 / frame_us))


# --- the run --------------------------------------------------------------------------


@dataclass
class PlayResult:
    id: str
    stream: str
    expect: str
    state: str | None = None
    why: str | None = None
    frames: int | None = None
    decoder_permille: int | None = None
    lines: PlayLines = field(default_factory=PlayLines)
    samples: list[int] = field(default_factory=list)
    rebooted: bool = False
    findings: list[str] = field(default_factory=list)
    folded: bool = False
    layout: str | None = None
    # What a Sendspin row records beyond the /play figures (suite_sendspin.figures), and notes
    # that are printed and are not findings.
    extra: dict[str, Any] = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)

    def figures(self) -> dict[str, Any]:
        out: dict[str, Any] = {"state": self.state, "frames": self.frames}
        if self.why:
            out["why"] = self.why
        if self.folded:
            # the hash is of what the decoder folded the stream to, held to the baseline's own
            out["layout"] = self.layout
        if len(self.samples) > 1:
            out["decoder_permille_samples"] = list(self.samples)
        for name, value in (
            ("decoder_permille", self.decoder_permille),
            ("units", self.lines.units),
            ("decode_stack_free", self.lines.decode_stack_free),
            ("underruns", self.lines.underruns),
            ("dry_ms", self.lines.dry_ms),
            ("min_headroom_ms", self.lines.min_headroom_ms),
            ("resync_bytes", self.lines.resync_bytes),
            ("pcm_hash", self.lines.ac4_pcm_hash),
        ):
            if value is not None:
                out[name] = value
        for name, value in self.extra.items():
            if value is not None:
                out[name] = value
        return out


def load_suite(path: Path = SUITE) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text("utf-8"))
    except (OSError, ValueError) as error:
        raise SuiteError(f"{path} cannot be read: {error}") from error
    if data.get("format") != FORMAT:
        raise SuiteError(f"{path} is format {data.get('format')}, this reads {FORMAT}")
    return data


def plan(
    suite: dict[str, Any], profile: str, only: list[str] | None = None
) -> list[dict[str, Any]]:
    rows = suite.get("profiles", {}).get(profile)
    if rows is None:
        known = ", ".join(sorted(suite.get("profiles", {})))
        raise SuiteError(f"the suite has no list for profile '{profile}' (known: {known})")
    out = []
    for row in rows["plays"]:
        if only and row["id"] not in only:
            continue
        stream = suite["streams"].get(row["stream"])
        if stream is None:
            raise SuiteError(
                f"play {row['id']} names stream '{row['stream']}', which is not listed"
            )
        policy = row.get("if_unavailable")
        if policy is not None and policy not in UNAVAILABLE_POLICIES:
            raise SuiteError(
                f"play {row['id']} says if_unavailable '{policy}': it is "
                + " or ".join(UNAVAILABLE_POLICIES)
            )
        if policy is not None and not row.get("layout"):
            raise SuiteError(
                f"play {row['id']} says what to do if its layout is unavailable and names none"
            )
        out.append({**row, "stream_spec": stream})
    return out


# --- what a board can carry -----------------------------------------------------------------

# A row that names a `layout` needs the board to carry it. The board says which layouts it can
# (GET /layout-options: the firmware's one verdict over its capabilities, the same one PUT /layout
# applies), and the suite does not guess from the profile: a row runs where the board carries its
# layout, and where the board does not, `if_unavailable` says what the row is: "refuse" (the
# default) expects PUT /layout to answer 409 with the board's own reason, and that refusal is the
# row's pass, since a board that is told a layout it cannot carry is meant to say no; "skip" leaves
# the row out, with the reason stated. A layout the board carries with a limit is played, and the
# limit is a note.
UNAVAILABLE_POLICIES = ("refuse", "skip")


def layout_entry(options: dict[str, Any] | None, layout: str) -> dict[str, Any] | None:
    """The board's listing for one layout, or None when it has none: a firmware without
    GET /layout-options, or a layout that the listing's catalog does not carry."""
    for entry in (options or {}).get("options", []):
        if isinstance(entry, dict) and entry.get("layout") == layout:
            return entry
    return None


def describe_layouts(options: dict[str, Any] | None) -> str:
    """The board's listing in one line: what it carries, what it carries with a limit, what it
    does not."""
    if options is None:
        return "layouts: the board has no GET /layout-options; every row is played as written"
    groups: dict[str, list[str]] = {"supported": [], "limits": [], "unavailable": []}
    for entry in options.get("options", []):
        if isinstance(entry, dict):
            groups.setdefault(str(entry.get("verdict")), []).append(str(entry.get("layout")))
    return (
        f"layouts ({options.get('slots')} slots): supported {' '.join(groups['supported']) or '-'}"
        f"; with limits {' '.join(groups['limits']) or '-'}"
        f"; unavailable {' '.join(groups['unavailable']) or '-'}"
    )


def layout_outcome(
    board: Board, row: dict[str, Any], layout: str, options: dict[str, Any] | None
) -> PlayResult | None:
    """What a row on a layout is when the board says it cannot carry that layout, or None when
    the row is to be played: the board lists it as carried (with or without a limit), or lists
    nothing about it."""
    entry = layout_entry(options, layout)
    if entry is None or entry.get("verdict") != "unavailable":
        return None
    reason = str(entry.get("reason") or "")
    result = PlayResult(id=row["id"], stream=row["stream"], expect=row.get("expect", "pass"))
    result.why = reason or None
    if row.get("if_unavailable", "refuse") == "skip":
        result.state = "skipped"
        result.notes.append(f"skipped: this board cannot carry {layout}: {reason}")
        return result
    previous = board.layout()
    code, body = board.put_layout(layout)
    if code == 200:
        # It took a layout it listed as unavailable: the finding below, and the board's own back.
        board.set_layout(previous)
    result.state = "refused-layout"
    if code != 409:
        result.findings.append(
            f"PUT /layout {layout} answered {code}, expected 409: the board's own listing says it "
            f"cannot carry it ({reason})"
        )
    elif body != reason:
        result.findings.append(
            f"PUT /layout {layout} was refused with {body!r}, the board's listing gives {reason!r}"
        )
    else:
        result.notes.append(f"{layout} is refused, as the board's listing says: {reason}")
    return result


def lan_address_for(host: str) -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((host, 80))
        return str(probe.getsockname()[0])


class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args: Any, **kwargs: Any) -> None:
        return


class FixtureServer(http.server.ThreadingHTTPServer):
    def server_bind(self) -> None:
        # HTTPServer's own looks the host's name up, which can take a second a server.
        socketserver.TCPServer.server_bind(self)
        self.server_name = "localhost"
        self.server_port = self.server_address[1]


def serve_fixtures(directory: Path, address: str) -> FixtureServer:
    server = FixtureServer((address, 0), partial(_Quiet, directory=str(directory)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def check_identity(board: Board, profile: str, profiles: Path | None = None) -> dict[str, Any]:
    """The board's hardware report, if it is this profile at this definition."""
    data = sink_profile.load(profiles) if profiles else sink_profile.load()
    try:
        wanted = sink_profile.profile_hash(data, profile)
    except sink_profile.ProfileError as error:
        raise SuiteError(str(error)) from error
    hardware = board.hardware()
    says = hardware.get("profile") or ""
    if says != profile:
        raise SuiteError(
            f"the board says it is '{says or '(no profile)'}', not {profile}: build it with "
            f"`sink_profile.py identity {profile} --cmake`; a scratch build is not a baseline"
        )
    if hardware.get("profile_hash") != wanted:
        raise SuiteError(
            f"the board carries {profile} at hash {hardware.get('profile_hash') or '(none)'} and "
            f"the file's definition is {wanted}: it was built from another one"
        )
    return hardware


def boot_budget(board: Board) -> dict[str, Any]:
    """The last `budget:` line of the board's console, as its fields, or {} where the log no
    longer holds one (a board that has played has rotated its boot lines out of a small log: the
    C6's holds 2 KB, which a restart a moment before still has it in)."""
    text, _ = board.log(None)
    found: dict[str, str] = {}
    for line in text.splitlines():
        if line.strip().startswith("budget: when="):
            found = fields(line.strip().partition("budget:")[2])
    keep = ("when", "net", "internal_free", "largest", "ask_total", "ask_biggest", "headroom")
    boot: dict[str, Any] = {key: _number(found[key]) for key in keep if key in found}
    if "verdict" in found:
        boot["verdict"] = found["verdict"]
    if "floor" in found:
        boot["floor"] = _number(found["floor"])
    return boot


def boot_budget_wait(board: Board, seconds: float = 12.0, sleep: float = 0.5) -> dict[str, Any]:
    """boot_budget after a restart, which can answer before the line has been printed."""
    deadline = time.monotonic() + seconds
    while True:
        boot = boot_budget(board)
        if boot or time.monotonic() >= deadline:
            return boot
        time.sleep(sleep)


def restart_board(args: argparse.Namespace, board: Board) -> None:
    """One restart of the board, by the method the run asked for: POST /restart, or a system
    reset by esptool's watchdog reset, which does not start the fault a P4's restart can."""
    if args.reset_by != "esptool":
        board.restart()
        return
    table = bench.load()
    row = table.boards[args.board]
    hardware = bench_flash.reset_board(table, row, bench_flash.make_runner(table))
    if hardware is None:
        raise SuiteError(f"{row.name} did not answer after the reset")
    board.wait_stopped()


def worst_boot(samples: list[dict[str, Any]]) -> dict[str, Any]:
    """The boot that left the least: the smallest largest block, then the least free RAM, of the
    samples that have a budget line. A board's boot figures are not one number: the S3's free RAM
    reads 16 KB higher when the idle task has freed the player's start stack first, and the C6's
    largest block was 96,256 bytes on fourteen of seventeen boots and 94,208, 92,160 and 88,064
    on the others. A baseline of the worst of N boots is the floor the board is known to start
    from, and a run that starts higher than it is not a regression."""
    usable = [sample for sample in samples if sample]
    if not usable:
        return {}
    return min(
        usable,
        key=lambda sample: (
            sample.get("largest", sys.maxsize),
            sample.get("internal_free", sys.maxsize),
        ),
    )


def tolerance_for(suite: dict[str, Any], profile: str | None) -> dict[str, Any]:
    """The suite's tolerance, with the profile's own on top of it: a profile whose boot figures
    jitter by more than the S3's does (the C6's largest block, by 8 KB) says so in its row, with
    the spread it was measured from."""
    merged = dict(suite.get("tolerance", {}))
    own = (suite.get("profiles", {}).get(profile or "") or {}).get("tolerance", {})
    merged.update({key: value for key, value in own.items() if key != "note"})
    return merged


def _number(text: str) -> Any:
    try:
        return int(text)
    except ValueError:
        return text


@dataclass
class SendspinContext:
    """What a Sendspin row needs: the test server, its state directory (one identity, so one
    pairing record on the board), the board's pairing as the run found it, and the label the
    server's report gives the board."""

    testserver: Path
    state: Path
    pairing: suite_sendspin.Pairing
    label: str = "board"
    timeout: float = 180.0
    status_port: int = 80
    # How the server is started and how long the harness waits for the board's totals line: the
    # defaults are the real ones, and a test sets them.
    popen: Any = None
    relay_every: float = 0.4
    log_wait_s: float = 8.0


# The extras a Sendspin row's baseline keeps, by how repeated plays of it are combined.
_EXTRA_MAX = ("late", "dropped", "invalid", "resyncs", "stream_underruns", "ring_high")
_EXTRA_SUM = ("plays", "glitched_plays")
_EXTRA_MIN = ("play_heap_free",)


def run_sendspin_play(
    board: Board,
    row: dict[str, Any],
    suite: dict[str, Any],
    context: SendspinContext | None,
    mark: int | None,
) -> tuple[PlayResult, int | None]:
    """One row played over Sendspin: the test server sends the stream, the board's totals line is
    read. The figures are the /play rows' where there is one (frames, decoder_permille,
    decode_stack_free, units, underruns) and the Sendspin ones beside them."""
    spec = row["stream_spec"]
    result = PlayResult(id=row["id"], stream=row["stream"], expect=row.get("expect", "pass"))
    if context is None:
        result.state = "refused"
        result.findings.append(
            "a Sendspin row needs the test server: --testserver, or ICLFORGE_TESTSERVER, or a "
            "build of it in the bench's suite-testserver directory"
        )
        return result, mark
    options: dict[str, Any] = {
        "status_port": context.status_port,
        "timeout_s": context.timeout,
        "relay_every": context.relay_every,
        "log_wait_s": context.log_wait_s,
        "seconds": spec.get("sendspin_seconds"),
    }
    if context.popen is not None:
        options["popen"] = context.popen
    try:
        run = suite_sendspin.play_row(
            board,
            context.testserver,
            context.state,
            REPO / spec["file"],
            context.label,
            context.pairing,
            **options,
        )
    except suite_sendspin.SendspinError as error:
        result.state = "refused"
        result.findings.append(str(error))
        return result, mark
    hard = suite_sendspin.judge(run, spec)
    result.findings.extend(hard)
    result.notes.extend(run.notes)
    result.rebooted = run.rebooted
    glitched = suite_sendspin.glitches(run) if not hard else []
    for line in glitched:
        result.notes.append(f"a glitch in the play: {line}")
    played = run.stream is not None and not run.rebooted and not hard
    result.state = "finished" if played else "failed"
    period = int(spec.get("period_us", suite.get("frame_us", 32000)))
    figures = suite_sendspin.figures(run, period)
    result.frames = figures.pop("frames", None)
    # A play's units held to the stream's are the bursts accounted for: played, or late, or
    # dropped (a late one is a glitch of the play and is counted as one).
    result.lines.units = suite_sendspin.accounted(run.stream) if run.stream else None
    result.decoder_permille = figures.pop("decoder_permille", None)
    result.lines.decode_stack_free = figures.pop("decode_stack_free", None)
    # Not `underruns`: that figure is a /play's, and any is a regression there. Over Sendspin a
    # play's glitches are counted in plays (glitched_plays of plays), as the board's own totals
    # of the worst play are kept beside them.
    figures["stream_underruns"] = figures.pop("underruns", None)
    result.extra = {
        "via": "sendspin",
        "plays": 1,
        "glitched_plays": 1 if glitched else 0,
        **figures,
    }
    # The log is read from where the row's stream started, so a /play row after this one starts
    # from the end of what it said.
    _, mark = board.log(mark)
    return result, mark


def run_play(
    board: Board,
    row: dict[str, Any],
    base_url: str,
    counter: int,
    mark: int | None,
    suite: dict[str, Any],
    pins: dict[str, str],
    timeout: float,
    sleep: float,
    sendspin: SendspinContext | None = None,
    layouts: dict[str, Any] | None = None,
) -> tuple[PlayResult, int | None]:
    """One row, played; returns its result and the log offset to carry on from. `layouts` is the
    board's own listing (GET /layout-options): a row on a layout the board cannot carry is not
    played, and is what its `if_unavailable` says (layout_outcome)."""
    if row.get("via") == "sendspin":
        return run_sendspin_play(board, row, suite, sendspin, mark)
    layout = row.get("layout")
    if not layout:
        return _play_http(board, row, base_url, counter, mark, suite, pins, timeout, sleep)
    outcome = layout_outcome(board, row, layout, layouts)
    if outcome is not None:
        return outcome, mark
    # A row on another layout (the P4X's full TDM frame, where the board's own is 2.0): set for the
    # play and put back after it, so that the next row is played as it would have been without it.
    previous = board.layout()
    code, body = board.put_layout(layout)
    if code != 200:
        refused = PlayResult(id=row["id"], stream=row["stream"], expect=row.get("expect", "pass"))
        refused.state = "refused"
        refused.findings.append(
            f"PUT /layout {layout} answered {code}, expected 200" + (f": {body}" if body else "")
        )
        return refused, mark
    try:
        played, mark = _play_http(board, row, base_url, counter, mark, suite, pins, timeout, sleep)
    finally:
        board.set_layout(previous)
    # A layout the board carries with a limit on it is played, and the limit is said.
    entry = layout_entry(layouts, layout)
    if entry is not None and entry.get("verdict") == "limits" and entry.get("reason"):
        played.notes.append(f"{layout} is listed with a limit: {entry['reason']}")
    return played, mark


def _play_http(
    board: Board,
    row: dict[str, Any],
    base_url: str,
    counter: int,
    mark: int | None,
    suite: dict[str, Any],
    pins: dict[str, str],
    timeout: float,
    sleep: float,
) -> tuple[PlayResult, int | None]:
    """One row over POST /play: the location is the play's own, the verdict is read from the log."""
    spec = row["stream_spec"]
    result = PlayResult(id=row["id"], stream=row["stream"], expect=row.get("expect", "pass"))
    # Advance the mark past anything printed between plays.
    _, mark = board.log(mark)
    url = f"{base_url}/{spec['file']}?run={counter}"
    code = board.play(url)
    if code != 202:
        result.state = "refused"
        result.findings.append(f"POST /play answered {code}, expected 202")
        return result, mark
    # The location is the play's own (the run counter makes it unique), so a stale state of the
    # play before is not read as this one's.
    deadline = time.monotonic() + timeout
    status: dict[str, Any] = {}
    while time.monotonic() < deadline:
        time.sleep(sleep)
        status = board.status()
        if str(status.get("location") or "").endswith(f"run={counter}") and status.get("state") in (
            "finished",
            "failed",
        ):
            break
    else:
        result.state = status.get("state")
        result.findings.append(f"the play did not end in {timeout:.0f} s (state {result.state})")
        return result, mark
    result.state = status.get("state")
    result.why = status.get("why") or None
    result.frames = status.get("frames")
    result.decoder_permille = decoder_permille(status, int(suite.get("frame_us", 32000)))

    # The verdict line prints just after the state flips: read the log until it is there.
    text = ""
    verdict_deadline = time.monotonic() + 15
    offset: int | None = mark
    while time.monotonic() < verdict_deadline:
        chunk, following = board.log(offset)
        if following is not None and offset is not None and following < offset:
            # The log is shorter than where it was: the board restarted, and its console is a
            # new one. Read what it has said since from the start; a play that took the board
            # down has no verdict line to wait for.
            result.rebooted = True
            chunk, following = board.log(None)
            text = chunk
            offset = following
            break
        text += chunk
        offset = following if following is not None else offset
        if "result=" in text or (result.expect == "refused-memory" and "refused" in text):
            break
        time.sleep(sleep)
    result.lines = parse_play_lines(text.splitlines())
    _judge(result, status, spec, pins, row.get("slots"))
    return result, offset


def _judge(
    result: PlayResult,
    status: dict[str, Any],
    spec: dict[str, Any],
    pins: dict[str, str],
    expected_slots: int | None = None,
) -> None:
    lines = result.lines
    if result.rebooted:
        result.findings.append("the board rebooted during the play (its console restarted)")
    if lines.failed_allocations:
        result.findings.append(f"{lines.failed_allocations} failed allocation(s) in the play")
    if result.expect == "refused-memory":
        if result.state != "failed" or result.why != "memory":
            result.findings.append(
                f"expected a refusal for memory, got state {result.state} why {result.why!r}"
            )
        return
    if result.state != "finished" or status.get("failed"):
        result.findings.append(f"the play did not finish: state {result.state}, why {result.why!r}")
        return
    if expected_slots is not None:
        # The sink opened for this many slots: a TDM frame past two channels, which the board with
        # no clock for it cannot open (the P4 below v3.0), shows as a play on fewer or none.
        played_slots = (status.get("stream") or {}).get("slots")
        if played_slots != expected_slots:
            result.findings.append(
                f"the play was rendered onto {played_slots} slots, expected {expected_slots}"
            )
    if lines.verdict != "pass":
        result.findings.append(f"the verdict line says result={lines.verdict}")
    if spec.get("units") is not None and lines.units != spec["units"]:
        result.findings.append(f"{lines.units} units, expected {spec['units']}")
    if lines.resync_bytes:
        result.findings.append(f"{lines.resync_bytes} resynchronised bytes")
    if lines.underruns:
        result.findings.append(
            f"{lines.underruns} sink underrun(s), {lines.dry_ms} ms dry, least headroom "
            f"{lines.min_headroom_ms} ms"
        )
    pin = spec.get("pin")
    if pin:
        expected = pins.get(pin)
        got = lines.ac4_pcm_hash
        # The pin is the host's hash of the stream in its own layout. A play on another layout
        # (a 5.1 stream folded to the 2.0 an I2S sink with two outputs is given, which a sink
        # that cannot open six cannot be asked otherwise) is not that PCM: no host oracle, and
        # the hash is the row's own, held to the baseline's.
        played = str(status.get("layout") or "")
        pin_layout = spec.get("pin_layout")
        folded = bool(pin_layout and played and played != pin_layout)
        if expected is None:
            result.findings.append(f"no pin named {pin} in {PINS.name}")
        elif got is None or set(got) == {"0"}:
            result.findings.append(
                "no PCM hash on the AC-4 line: the image was built without "
                "CONFIG_ICLFORGE_EXAMPLE_AC4_PCM_HASH=1 (the `measure` overlay)"
            )
        elif folded:
            result.folded = True
            result.layout = played
        elif got.lower() != expected.lower():
            result.findings.append(f"PCM hash {got} is not the host's {expected} ({pin})")
        if lines.ac4_decoder_permille is not None:
            result.decoder_permille = lines.ac4_decoder_permille


def run_suite(
    board: Board,
    profile: str,
    suite: dict[str, Any],
    base_url: str,
    pins: dict[str, str],
    only: list[str] | None = None,
    repeat: int = 1,
    timeout: float = 120.0,
    sleep: float = 0.4,
    sendspin: SendspinContext | None = None,
    layouts: dict[str, Any] | None = None,
) -> list[PlayResult]:
    rows = plan(suite, profile, only)
    mark: int | None = None
    _, mark = board.log(None)
    results: list[PlayResult] = []
    counter = 0
    for row in rows:
        runs: list[PlayResult] = []
        for _ in range(max(1, repeat)):
            counter += 1
            played, mark = run_play(
                board, row, base_url, counter, mark, suite, pins, timeout, sleep, sendspin, layouts
            )
            runs.append(played)
            if played.findings and played.state in (None, "refused"):
                break
        results.append(_median(runs))
    return results


def _median(runs: list[PlayResult]) -> PlayResult:
    """One result for a row played more than once: the worst findings and the median figures."""
    if len(runs) == 1:
        return runs[0]
    chosen = runs[0]
    times = [run.decoder_permille for run in runs if run.decoder_permille is not None]
    if times:
        chosen.decoder_permille = int(statistics.median(times))
        chosen.samples = list(times)
    stacks = [r.lines.decode_stack_free for r in runs if r.lines.decode_stack_free is not None]
    if stacks:
        chosen.lines.decode_stack_free = min(stacks)
    heads = [r.lines.min_headroom_ms for r in runs if r.lines.min_headroom_ms is not None]
    if heads:
        chosen.lines.min_headroom_ms = min(heads)
    for name in _EXTRA_MAX + _EXTRA_MIN:
        values = [r.extra[name] for r in runs if r.extra.get(name) is not None]
        if values:
            chosen.extra[name] = max(values) if name in _EXTRA_MAX else min(values)
    for name in _EXTRA_SUM:
        values = [r.extra[name] for r in runs if r.extra.get(name) is not None]
        if values:
            chosen.extra[name] = sum(values)
    if all(r.extra.get("via") == "sendspin" for r in runs):
        # the bursts a play got to the sink: the first play may be a glitched one
        got = [r.frames for r in runs if r.frames is not None]
        if got:
            chosen.frames = max(got)
    worst = [r.extra["worst_error_us"] for r in runs if r.extra.get("worst_error_us") is not None]
    if worst:
        chosen.extra["worst_error_us"] = max(worst, key=abs)
    burst = [r.extra["burst_us"] for r in runs if r.extra.get("burst_us") is not None]
    if burst:
        chosen.extra["burst_us"] = int(statistics.median(burst))
    for run in runs[1:]:
        for finding in run.findings:
            if finding not in chosen.findings:
                chosen.findings.append(finding)
        for note in run.notes:
            if note not in chosen.notes:
                chosen.notes.append(note)
        chosen.rebooted = chosen.rebooted or run.rebooted
        if run.lines.underruns:
            chosen.lines.underruns = max(chosen.lines.underruns or 0, run.lines.underruns)
    return chosen


# --- baseline -------------------------------------------------------------------------


def run_record(
    profile: str,
    hardware: dict[str, Any],
    results: list[PlayResult],
    boot: dict[str, Any] | None,
    board: str = "",
    boots: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    record = {
        "format": FORMAT,
        "profile": profile,
        "profile_hash": hardware.get("profile_hash"),
        "version": hardware.get("version"),
        "chip": hardware.get("chip"),
        "board": board,
        "recorded": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%MZ"),
        "boot": boot or {},
        "plays": {r.id: r.figures() for r in results},
        "findings": {r.id: r.findings for r in results if r.findings},
    }
    samples = [b for b in (boots or []) if b]
    if len(samples) > 1:
        # `boot` is the worst of these; they are kept so that the spread can be read
        record["boot_samples"] = {
            "internal_free": [b.get("internal_free") for b in samples],
            "largest": [b.get("largest") for b in samples],
        }
    return record


def baseline_note(run: dict[str, Any], baseline: dict[str, Any]) -> str | None:
    """One line when the baseline was recorded under another profile hash than the run's image
    carries, and None when they are the same or either is not known. The hash is the profile's
    fragments and locked values, so a different one is a different definition, and the baseline's
    figures are still the ones recorded: `compare` holds the run to them as they are, and this says
    which definition they were taken under. A profile gains a locked key at the value it already
    had in effect (a new switch left off) without a figure moving; the baselines are then kept,
    and this is the note that goes with them (planning/esp32-platform-profiles.md, "Changing a
    profile")."""
    recorded, carried = baseline.get("profile_hash"), run.get("profile_hash")
    if not recorded or not carried or recorded == carried:
        return None
    return (
        f"note: the baseline was recorded under profile hash {recorded} and this run's image "
        f"carries {carried}: the definition has changed since (git log -p "
        "firmware/hearth-sink/profiles.json), and the figures are compared as they were recorded"
    )


def compare(
    run: dict[str, Any], baseline: dict[str, Any], tolerance: dict[str, Any]
) -> tuple[list[str], list[str]]:
    """What a run did against a baseline: regressions, and improvements to re-baseline."""
    regressions: list[str] = []
    improvements: list[str] = []
    if run.get("profile") != baseline.get("profile"):
        regressions.append(
            f"the run is profile {run.get('profile')}, the baseline's is {baseline.get('profile')}"
        )
    relative = float(tolerance.get("relative", 0.05))
    absolute = int(tolerance.get("absolute_permille", 20))
    stack_slack = int(tolerance.get("stack_free_bytes", 512))
    headroom_slack = int(tolerance.get("headroom_ms", 2))
    # The boot line: what a play would ask of internal RAM, and what is left to meet it with.
    base_boot, now_boot = baseline.get("boot") or {}, run.get("boot") or {}
    if base_boot and not now_boot:
        regressions.append("boot: the run has no budget line, the baseline has one")
    elif base_boot:
        if now_boot.get("verdict") != base_boot.get("verdict"):
            regressions.append(
                f"boot: verdict {now_boot.get('verdict')}, "
                f"the baseline's {base_boot.get('verdict')}"
            )
        for key, slack_key, label in (
            ("largest", "largest_block_bytes", "largest internal block"),
            ("internal_free", "internal_free_bytes", "internal RAM free"),
        ):
            base_value, now_value = base_boot.get(key), now_boot.get(key)
            slack = int(tolerance.get(slack_key, 0))
            if (
                isinstance(base_value, int)
                and isinstance(now_value, int)
                and now_value < base_value - slack
            ):
                regressions.append(
                    f"boot: {label} is {now_value:,} bytes, the baseline's {base_value:,}"
                )
    for play, base in baseline.get("plays", {}).items():
        now = run.get("plays", {}).get(play)
        if now is None:
            regressions.append(f"{play}: not in the run")
            continue
        if now.get("state") != base.get("state"):
            # A row the board no longer carries the layout of says why, in the board's words.
            why = now.get("why") if now.get("state") in ("skipped", "refused-layout") else None
            regressions.append(
                f"{play}: state {now.get('state')}, baseline {base.get('state')}"
                + (f" ({why})" if why else "")
            )
            continue
        base_time, now_time = base.get("decoder_permille"), now.get("decoder_permille")
        if base_time is not None and now_time is not None:
            margin = max(base_time * relative, absolute)
            if now_time > base_time + margin:
                regressions.append(
                    f"{play}: the decoder takes {now_time} permille of a frame, the baseline's "
                    f"{base_time} (allowed to {base_time + margin:.0f})"
                )
            elif now_time < base_time - margin:
                improvements.append(
                    f"{play}: {now_time} permille, the baseline's {base_time}: re-baseline"
                )
        if (
            base.get("decode_stack_free") is not None
            and now.get("decode_stack_free") is not None
            and now["decode_stack_free"] < base["decode_stack_free"] - stack_slack
        ):
            regressions.append(
                f"{play}: the decode stack has {now['decode_stack_free']} bytes free, the "
                f"baseline's {base['decode_stack_free']}"
            )
        if (
            base.get("min_headroom_ms") is not None
            and now.get("min_headroom_ms") is not None
            and now["min_headroom_ms"] < base["min_headroom_ms"] - headroom_slack
        ):
            regressions.append(
                f"{play}: the sink queue's least headroom is {now['min_headroom_ms']} ms, the "
                f"baseline's {base['min_headroom_ms']}"
            )
        if now.get("via") == "sendspin":
            # Plays that glitched, of the plays made: the C6 runs a 30 s AC-3 stream dry in about
            # one in seven, so none is not the figure. One more than the baseline had is allowed
            # (the tolerance's glitched_plays), which five plays a row make a real margin.
            # one number for every row, or a number by row id (one more for the rest)
            configured = tolerance.get("glitched_plays", 1)
            allowed = int(configured.get(play, 1) if isinstance(configured, dict) else configured)
            now_glitched = int(now.get("glitched_plays") or 0)
            base_glitched = int(base.get("glitched_plays") or 0)
            if now_glitched > base_glitched + allowed:
                regressions.append(
                    f"{play}: {now_glitched} of {now.get('plays') or '?'} plays glitched, the "
                    f"baseline's {base_glitched} of {base.get('plays') or '?'} (allowed "
                    f"{base_glitched + allowed})"
                )
        elif now.get("underruns"):
            regressions.append(f"{play}: {now['underruns']} sink underrun(s)")
        # The least internal heap while a Sendspin stream played. A row has a tolerance for it
        # where the figure is a stable one (the C6's E-AC-3 plays with about 2.6 KB spare, and a
        # play that takes any of that is the regression); an AC-3 row's reads 26 to 52 KB from one
        # play to the next and is recorded without one.
        heap_slack = tolerance.get("play_heap_free_bytes")
        slack_for = heap_slack.get(play) if isinstance(heap_slack, dict) else heap_slack
        if (
            slack_for is not None
            and isinstance(base.get("play_heap_free"), int)
            and isinstance(now.get("play_heap_free"), int)
            and now["play_heap_free"] < base["play_heap_free"] - int(slack_for)
        ):
            regressions.append(
                f"{play}: the least internal heap while it played is {now['play_heap_free']:,} "
                f"bytes, the baseline's {base['play_heap_free']:,}"
            )
        if now.get("units") != base.get("units"):
            regressions.append(f"{play}: {now.get('units')} units, baseline {base.get('units')}")
        if base.get("pcm_hash") and now.get("pcm_hash") != base.get("pcm_hash"):
            regressions.append(
                f"{play}: PCM hash {now.get('pcm_hash')}, baseline {base['pcm_hash']}"
            )
    return regressions, improvements


# --- command line ---------------------------------------------------------------------


def _print_results(results: list[PlayResult]) -> None:
    for r in results:
        figures = r.figures()
        shown = ", ".join(f"{k}={v}" for k, v in figures.items())
        verdict = "FAIL" if r.findings else "ok"
        print(f"  {verdict:<4} {r.id}: {shown}")
        for finding in r.findings:
            print(f"         - {finding}")
        for note in r.notes:
            print(f"         note: {note}")


def cmd_run(args: argparse.Namespace) -> int:
    suite = load_suite()
    rows = plan(suite, args.profile, args.only)  # no list is refused before a lock is taken
    resolve_board(args)
    if args.reset_by == "esptool":
        if not args.restart:
            raise SuiteError("--reset-by esptool is the way --restart restarts: it needs --restart")
        if args.board not in bench.load().boards:
            raise SuiteError("--reset-by esptool needs --board to be a board of bench.json")
    if args.boots < 1:
        raise SuiteError("--boots is at least 1")
    if args.boots > 1 and not args.restart:
        raise SuiteError("--boots restarts the board that many times: it needs --restart")
    if not args.host:
        raise SuiteError("--host, or a --board that is on the bench, is required")
    if not args.lock or not args.session:
        raise SuiteError(
            "refused: --lock and --session are required. A board is booked through its lock file "
            "before anything runs on it (see tools/hearth/board_suite.py --help)"
        )
    # A Sendspin row's server is found after the arguments are checked and before the lock
    # is taken: not when the first row wants it.
    testserver: Path | None = None
    if any(row.get("via") == "sendspin" for row in rows):
        try:
            testserver = suite_sendspin.find_testserver(args.testserver, bench.load().locks)
        except suite_sendspin.SendspinError as error:
            raise SuiteError(str(error)) from error
    for line in CHECKLIST:
        print(line)
    took, ours = hold_lock(
        Path(args.lock), args.session, args.purpose or f"suite {args.profile}", args.minutes
    )
    print(f"booked: {took}" if ours else f"already booked by this session: {took}")
    server = None
    sendspin: SendspinContext | None = None
    try:
        board = Board(args.host, args.port, args.timeout)
        # Before any restart: a board that is another image is not worth eight of them.
        check_identity(board, args.profile, args.profiles)
        boots: list[dict[str, Any]] = []
        if args.restart:
            for number in range(1, args.boots + 1):
                print(
                    f"{args.host}: restarting ({number} of {args.boots}), so that its boot line "
                    "is the log's first"
                )
                restart_board(args, board)
                if number < args.boots:
                    boots.append(boot_budget_wait(board))
        hardware = check_identity(board, args.profile, args.profiles)
        before = board.status()
        if before.get("state") != "stopped":
            raise SuiteError(f"the board's /status is {before.get('state')!r}, not 'stopped'")
        # What the board says it can carry: a row on a layout it cannot is its refusal, not a play.
        layouts = board.layout_options() if any(row.get("layout") for row in rows) else None
        if any(row.get("layout") for row in rows):
            print(describe_layouts(layouts))
        print(
            f"{args.host}: {hardware.get('chip')} {hardware.get('version')}, "
            f"profile {args.profile} at {hardware.get('profile_hash')}"
        )
        boots.append(boot_budget_wait(board) if args.restart else boot_budget(board))
        boot = worst_boot(boots)
        if len(boots) > 1:
            print(f"boot: the worst of {len(boots)}: {boot}")
        if testserver is not None:
            locks = bench.load().locks
            sendspin = SendspinContext(
                testserver=testserver,
                state=Path(args.sendspin_state or suite_sendspin.default_state(locks)),
                pairing=suite_sendspin.Pairing(board, keep=args.keep_pairing),
                label=args.board or args.host,
                timeout=args.play_timeout + 60,
                status_port=args.port,
            )
            print(f"sendspin rows: the test server is {sendspin.testserver}")
        if args.fixtures_url:
            base_url = args.fixtures_url.rstrip("/")
        else:
            address = args.serve_host or lan_address_for(args.host)
            server = serve_fixtures(REPO, address)
            base_url = f"http://{address}:{server.server_address[1]}"
        pins = json.loads(PINS.read_text("utf-8")).get("hashes", {}) if PINS.is_file() else {}
        results = run_suite(
            board,
            args.profile,
            suite,
            base_url,
            pins,
            args.only,
            args.repeat,
            args.play_timeout,
            sendspin=sendspin,
            layouts=layouts,
        )
        board.stop()
        record = run_record(args.profile, hardware, results, boot, args.board, boots)
        _print_results(results)
        problems = [f for r in results for f in r.findings]
        if args.out != "-":
            Path(args.out).write_text(json.dumps(record, indent=2) + "\n", "utf-8")
            print(f"wrote {args.out}")
        regressions: list[str] = []
        if args.compare:
            baseline = json.loads(Path(args.compare).read_text("utf-8"))
            regressions, improvements = compare(
                record, baseline, tolerance_for(suite, args.profile)
            )
            for line in regressions:
                print(f"regression: {line}")
            for line in improvements:
                print(f"improved: {line}")
            note = baseline_note(record, baseline)
            if note:
                print(note)
        if args.record:
            if not boot:
                print("not recorded: the log holds no boot budget line; run with --restart")
            elif problems or regressions:
                print("not recorded: the run has findings")
            else:
                target = BASELINES / f"{args.profile}.json"
                BASELINES.mkdir(exist_ok=True)
                target.write_text(json.dumps(record, indent=2) + "\n", "utf-8")
                print(f"recorded {target}")
        return 1 if (problems or regressions) else 0
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
        if sendspin is not None:
            try:
                print(sendspin.pairing.close())
            except SuiteError as error:
                print(f"pairing: could not tidy the board's pairing table: {error}")
        if ours:
            print(f"lock: {release_lock(Path(args.lock), args.session)}")
        else:
            print("lock: left booked, as it was found")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    show = commands.add_parser("plan", help="a profile's play list")
    show.add_argument("--profile", required=True)

    run = commands.add_parser("run", help="play a profile's list on a booked board")
    run.add_argument("--profile", required=True)
    run.add_argument("--host", help="the board's address (the bench's, with --board)")
    run.add_argument("--port", type=int, default=80)
    run.add_argument("--lock", help="the board's lock file: taken, and released at the end")
    run.add_argument("--session", help="your session id, as the lock line carries it")
    run.add_argument("--purpose", default="")
    run.add_argument(
        "--board",
        default="",
        help="a board of tools/hearth/bench.json, which gives --host and --lock and refuses a "
        "pair that is not its own; any other text is a label kept in the record",
    )
    run.add_argument(
        "--restart",
        action="store_true",
        help="POST /restart first, so that the boot line is in the log "
        "(refused for a P4 of the bench: its restart can kill the C6 link)",
    )
    run.add_argument(
        "--reset-by",
        choices=("http", "esptool"),
        default="http",
        help="how --restart restarts: POST /restart, or esptool's watchdog reset through the "
        "board's serial port (what a P4 needs, whose restart can leave its C6 link dead)",
    )
    run.add_argument(
        "--boots",
        type=int,
        default=1,
        help="with --restart, restart this many times and hold the worst boot line "
        "(a board's boot figures vary from boot to boot)",
    )
    run.add_argument(
        "--accept-restart-fault",
        action="store_true",
        help="restart a P4 anyway, ready to put it back by hand",
    )
    run.add_argument("--minutes", type=int, default=30)
    run.add_argument("--only", action="append", help="a play id; may be repeated")
    run.add_argument("--repeat", type=int, default=1, help="plays of each row; the median is held")
    run.add_argument("--timeout", type=float, default=10.0, help="one HTTP request, seconds")
    run.add_argument("--play-timeout", type=float, default=120.0, help="one play, seconds")
    serving = run.add_mutually_exclusive_group()
    serving.add_argument("--serve-host", help="the address to serve the streams on")
    serving.add_argument(
        "--fixtures-url",
        help="play the streams from a server that is already running, which serves the "
        "repository's paths (testdata/..., libs/...) from this URL, and start none: the suite's "
        "own serves the whole repository on the LAN, which a bench that wants only the streams "
        "it plays on offer can do without",
    )
    run.add_argument(
        "--testserver",
        help="hearth-testserver, for rows with via sendspin (else ICLFORGE_TESTSERVER, else the "
        "bench's suite-testserver/bin)",
    )
    run.add_argument(
        "--sendspin-state",
        help="the test server's state directory: its identity and its pairing records (default "
        "suite-sendspin-state beside the bench's locks; keep one, so that the board holds one "
        "record of the suite's)",
    )
    run.add_argument(
        "--keep-pairing",
        action="store_true",
        help="leave the suite's pairing record on the board (it is forgotten at the end)",
    )
    run.add_argument("--out", default="board-run.json", help="where to write the run; - for none")
    run.add_argument("--profiles", type=Path, help="a profiles file other than the repository's")
    group = run.add_mutually_exclusive_group()
    group.add_argument("--record", action="store_true", help="write baselines/<profile>.json")
    group.add_argument("--compare", metavar="BASELINE", help="hold the run to this baseline")

    cmp_ = commands.add_parser("compare", help="hold a recorded run to a baseline")
    cmp_.add_argument("--run", type=Path, required=True)
    cmp_.add_argument("--baseline", type=Path, required=True)

    lock = commands.add_parser("lock", help="take, release or show a board's lock file")
    lock.add_argument("action", choices=["take", "release", "show"])
    lock.add_argument("--file", type=Path, required=True)
    lock.add_argument("--session", default="")
    lock.add_argument("--purpose", default="")
    lock.add_argument("--minutes", type=int, default=60)

    args = parser.parse_args(argv)
    try:
        if args.command == "plan":
            suite = load_suite()
            for row in plan(suite, args.profile):
                spec = row["stream_spec"]
                print(
                    f"{row['id']}\t{spec['codec']}\t{spec['file']}\t"
                    f"{row.get('expect', 'pass')}\tunits={spec.get('units', '-')}"
                    f"\tpin={spec.get('pin', '-')}\tvia={row.get('via', 'play')}"
                )
            return 0
        if args.command == "run":
            return cmd_run(args)
        if args.command == "compare":
            suite = load_suite()
            recorded = json.loads(args.run.read_text("utf-8"))
            baseline = json.loads(args.baseline.read_text("utf-8"))
            regressions, improvements = compare(
                recorded, baseline, tolerance_for(suite, recorded.get("profile"))
            )
            for line in regressions:
                print(f"regression: {line}")
            for line in improvements:
                print(f"improved: {line}")
            note = baseline_note(recorded, baseline)
            if note:
                print(note)
            if not regressions:
                print("the run holds to the baseline")
            return 1 if regressions else 0
        if args.command == "lock":
            if args.action == "show":
                print(
                    args.file.read_text("utf-8").strip() if args.file.is_file() else "no lock file"
                )
            elif not args.session:
                raise SuiteError("--session is required")
            elif args.action == "take":
                print(take_lock(args.file, args.session, args.purpose, args.minutes))
            else:
                print(release_lock(args.file, args.session))
            return 0
    except (SuiteError, sink_profile.ProfileError) as error:
        print(f"board_suite: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
