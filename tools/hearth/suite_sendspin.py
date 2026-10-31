#!/usr/bin/env python3
"""A board suite row played over Sendspin, which is the C6's only audio path.

The C6 refuses an HTTP play on purpose (`CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES`, #1395), so
its `/play` rows are the refusal and nothing holds the audio it is sold for. This plays a row the
way the product does: the repository's `hearth-testserver` dials the board, pairs it, and sends
the programme as bursts over `_iclforge_player@v1`; the board prints a `sendspin.stream=` line when
the stream ends (firmware/esp-idf/iclforge/src/burst_player.cpp) and the suite reads that line the
way it reads a `/play`'s `result=` line.

Three things the harness owns, because each has cost a session a board:

* **One identity.** The server keeps its key and its pairing records in a state directory
  (default `<bench locks>/suite-sendspin-state`). A fresh directory is a new identity, which is a
  new pairing record on the board each run: eight of them once pushed every other server off a
  C6. The directory is kept, so the board holds one record of the suite's.
* **The code.** An unpaired board shows a pairing code in `GET /status`
  (`sendspin.pairing_code`); the server reads one from a file written after it asks. The suite
  relays the code, and appends it again while it is shown, because the server measures the file
  just after asking and a code written a moment before would be missed.
* **Leaving the table as found.** The board pairs up to eight servers and a suite run adds one.
  At the end the suite POSTs `forget <server_id>` for its own record, never a bare `forget`,
  which drops the owner's Hearth. `--keep-pairing` leaves it.

A server of the owner's (Music Assistant) holds the board while it is paired and connected; the
test server pairs at the dial and displaces it for the length of the play, and the owner's server
reconnects afterwards.

Standard library only.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

# What the server's run-level result says of a stream that ended with gaps in it: a glitch of a
# stream that played through, not a board that went away. It writes these as the report's
# top-level `result` ("failed: C6 ran out of audio 1 time(s)"); players[0].failure is only for a
# player that was displaced, closed or did not connect. Whether every burst was played is the
# burst count's to say, which `judge` checks after.
SOFT_FAILURE = re.compile(r"ran out of audio|did not play every burst|found invalid chunks")

SENDSPIN_PORT = 8928
SERVER_NAME = "ICL Forge board suite"
# How long the pairing code is relayed for: pairing takes about three seconds.
RELAY_SECONDS = 25.0

# What GET /status's `sendspin` object is read for, by name. The network object is never one.
SENDSPIN_FIELDS = (
    "server",
    "server_id",
    "paired",
    "connections",
    "pairing_code",
    "playing",
    "bursts",
    "underruns",
    "late",
    "dropped",
    "invalid",
    "resyncs",
    "worst_error_us",
    "burst_us",
    "worst_burst_us",
    "decode_stack_free",
    "server_stack_free",
)

# `sendspin.stream=` line: its counters are the last of the stream, the burst time a mean.
STREAM_MARK = "sendspin.stream="
_NUMBER = re.compile(r"\b([a-z_]+)=(-?\d+)\b")
_KIND = re.compile(r"sendspin\.stream=(\w+)")


class SendspinError(Exception):
    pass


class BoardLike(Protocol):
    host: str

    def sendspin(self) -> dict[str, Any]: ...

    def pairing(self) -> dict[str, Any]: ...

    def forget(self, server_id: str) -> int: ...

    def log(self, start: int | None) -> tuple[str, int | None]: ...


def parse_stream_end(text: str) -> dict[str, Any] | None:
    """The last `sendspin.stream=` line of `text` as its fields: `kind` ('bursts' or 'pcm') and the
    counters by name. None where the log holds none."""
    found: dict[str, Any] | None = None
    for raw in text.splitlines():
        if STREAM_MARK not in raw:
            continue
        line = raw[raw.index(STREAM_MARK) :]
        kind = _KIND.match(line)
        values: dict[str, Any] = {name: int(number) for name, number in _NUMBER.findall(line)}
        values["kind"] = kind.group(1) if kind else ""
        found = values
    return found


def pairing_ids(table: dict[str, Any]) -> dict[str, str]:
    """GET /pairing's servers as {server_id: name}."""
    return {
        str(server.get("server_id")): str(server.get("name") or "")
        for server in table.get("servers", [])
        if server.get("server_id")
    }


def default_state(locks: Path) -> Path:
    return locks / "suite-sendspin-state"


def find_testserver(explicit: str | None, locks: Path) -> Path:
    """The test server binary: --testserver, then ICLFORGE_TESTSERVER, then the bench's build."""
    candidates = [explicit, os.environ.get("ICLFORGE_TESTSERVER")]
    for name in ("hearth-testserver.exe", "hearth-testserver"):
        candidates.append(str(locks / "suite-testserver" / "bin" / name))
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    raise SendspinError(
        "no hearth-testserver to play a Sendspin row with: pass --testserver, set "
        "ICLFORGE_TESTSERVER, or build it (planning/esp32-board-bench.md, 'Sendspin rows')"
    )


@dataclass
class SendspinRun:
    exit_code: int | None = None
    report: dict[str, Any] | None = None
    stream: dict[str, Any] | None = None
    server_id: str | None = None
    server_stderr: str = ""
    seconds: float = 0.0
    findings: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    after: dict[str, Any] = field(default_factory=dict)
    rebooted: bool = False

    def player(self) -> dict[str, Any]:
        players = (self.report or {}).get("players") or []
        return players[0] if players else {}

    def failure(self) -> str:
        """Why the server says the run failed: the player's own failure (it was displaced, or
        did not connect), else the run's `result` when that is not a pass."""
        own = str(self.player().get("failure") or "")
        if own:
            return own
        result = str((self.report or {}).get("result") or "")
        return "" if result in ("", "pass") else result.removeprefix("failed: ")


def command(
    testserver: Path,
    state: Path,
    programme: Path,
    report: Path,
    code_log: Path,
    host: str,
    status_port: int,
    label: str,
    timeout_s: int,
    port: int = SENDSPIN_PORT,
    use_code: bool = True,
    poll_status: bool = False,
    seconds: int | None = None,
    tone: bool = False,
) -> list[str]:
    argv = [
        str(testserver),
        "--state",
        str(state),
        "--name",
        SERVER_NAME,
        "--report",
        str(report),
        "--timeout",
        str(timeout_s),
        "--player",
        f"ws://{host}:{port}/sendspin",
        "--label",
        label,
    ]
    if tone:
        # two tones, as PCM over player@v1, for `seconds`: the stereo PCM path beside the coded one
        argv += ["--pcm", "--tone"]
    else:
        argv += ["--play", str(programme)]
    if seconds:
        # a three-frame file is not a stream: the server plays it over and over for this long
        argv += ["--seconds", str(seconds)]
    if poll_status:
        argv += [
            "--status",
            f"http://{host}:{status_port}/status" if status_port != 80 else f"http://{host}/status",
        ]
    # The server asks a board to pair at the dial whenever it has a --code-log, which a board that
    # holds the server's record answers by showing a code that nobody enters: so the file is
    # only given to a board that does not hold the record.
    if use_code:
        argv += ["--code-log", str(code_log)]
    return argv


def digits_of(code: str) -> str:
    return re.sub(r"\D", "", code or "")


def relay_codes(
    board: BoardLike,
    path: Path,
    stop: threading.Event,
    every: float = 0.4,
    stale: str = "",
    seconds: float = RELAY_SECONDS,
) -> None:
    """Writes the pairing code the board shows into `path`, as its console prints it, and again
    each tick while it is shown (the server measures the file after asking). A board keeps the
    last code it showed in /status after a pairing that failed: `stale` is the one shown before
    the run, and is not a code for this one. It stops after `seconds`: a board with little heap
    left is not polled for the length of a play (its HTTP server answers from the same heap)."""
    deadline = time.monotonic() + seconds
    while not stop.wait(every) and time.monotonic() < deadline:
        try:
            digits = digits_of(str(board.sendspin().get("pairing_code") or ""))
        except Exception:  # a board between two answers is not a failed run
            continue
        if len(digits) < 6 or digits == stale:
            continue
        shown = f"{digits[:3]}-{digits[3:]}" if len(digits) == 6 else digits
        with path.open("a", encoding="utf-8") as out:
            out.write(f"sendspin: PAIRING CODE {shown}\n")


def run_row(
    board: BoardLike,
    testserver: Path,
    state: Path,
    programme: Path,
    label: str,
    *,
    status_port: int = 80,
    timeout_s: float = 150.0,
    popen: Callable[..., Any] = subprocess.Popen,
    relay_every: float = 0.4,
    use_code: bool = True,
    connect_s: int = 60,
    log_wait_s: float = 8.0,
    seconds: int | None = None,
    tone: bool = False,
) -> SendspinRun:
    """One programme sent to the board by the test server, and what came of it. The server's exit
    status and report, and the board's own totals line from its log, are what it returns."""
    result = SendspinRun()
    _, mark = board.log(None)
    try:
        stale = digits_of(str(board.sendspin().get("pairing_code") or ""))
    except Exception:
        stale = ""
    with tempfile.TemporaryDirectory(prefix="suite-sendspin-") as scratch:
        work = Path(scratch)
        report, code_log, errors = work / "report.json", work / "codes.txt", work / "stderr.txt"
        code_log.touch()
        argv = command(
            testserver, state, programme, report, code_log, board.host, status_port, label,
            connect_s,
            use_code=use_code,
            seconds=seconds,
            tone=tone,
        )  # fmt: skip
        stop = threading.Event()
        relay = threading.Thread(
            target=relay_codes, args=(board, code_log, stop, relay_every, stale), daemon=True
        )
        started = time.monotonic()
        with errors.open("wb") as err:
            try:
                process = popen(argv, stdout=subprocess.DEVNULL, stderr=err)
            except OSError as error:
                raise SendspinError(f"{testserver} could not be started: {error}") from error
            if use_code:
                relay.start()
            try:
                result.exit_code = process.wait(timeout=timeout_s)
            except subprocess.TimeoutExpired:
                process.kill()
                result.exit_code = None
                result.findings.append(f"the test server did not end in {timeout_s:.0f} s")
            finally:
                stop.set()
                if use_code:
                    relay.join(timeout=5)
        result.seconds = time.monotonic() - started
        result.server_stderr = errors.read_text("utf-8", "replace")[-1500:]
        if report.is_file():
            try:
                result.report = json.loads(report.read_text("utf-8"))
            except ValueError as error:
                result.findings.append(f"the test server's report is not JSON: {error}")
    result.server_id = ((result.report or {}).get("server") or {}).get("id")
    try:
        result.after = board.sendspin()
    except Exception:  # a board that rebooted may not answer yet; the log says so
        result.after = {}
    # The totals line prints a moment after the server sees the stream end.
    deadline = time.monotonic() + log_wait_s
    text = ""
    offset = mark
    while True:
        chunk, following = board.log(offset)
        if following is not None and offset is not None and following < offset:
            result.rebooted = True  # the log is shorter than where it was: a new console
            chunk, following = board.log(None)
            text = chunk
            offset = following
        else:
            text += chunk
            offset = following if following is not None else offset
        result.stream = parse_stream_end(text)
        if result.stream is not None or time.monotonic() >= deadline:
            break
        time.sleep(0.4)
    return result


def judge(run: SendspinRun, spec: dict[str, Any]) -> list[str]:
    """What is wrong with the run, as findings; empty when it played the programme through. A
    stream that played through and ran the board dry is not one: it is a glitch, which
    `glitches` names and the suite holds as a count of plays."""
    findings = list(run.findings)
    if run.rebooted:
        findings.append("the board rebooted during the play (its console restarted)")
    if run.exit_code not in (0, None):
        failure = run.failure()
        # A player's own failure is never a glitch; the run's result is one when it names only
        # gaps in a stream the board's own totals line then confirms.
        glitch_only = (
            run.exit_code == 3
            and run.stream is not None
            and not run.player().get("failure")
            and SOFT_FAILURE.search(failure)
        )
        if not glitch_only:
            why = {
                1: "the command line or the file could not be used",
                2: "the board did not connect, pair or take its settings",
                3: "the board went away, or reported an error, during the play",
            }.get(run.exit_code, "unknown")
            findings.append(
                f"the test server exited {run.exit_code} ({why})"
                + (f": {failure}" if failure else "")
            )
            return findings
    if run.exit_code is None:
        return findings
    if run.stream is None:
        findings.append("the board printed no sendspin.stream= line when the stream ended")
        return findings
    stream = run.stream
    if stream.get("kind") != "bursts":
        findings.append(f"the stream was {stream.get('kind')!r}, not bursts")
    expected = spec.get("sendspin_units", spec.get("units"))
    if spec.get("sendspin_seconds"):
        # a file played over and over: how many bursts that is, is the server's to say
        expected = ((run.report or {}).get("programme") or {}).get("bursts")
    if expected is not None and accounted(stream) != expected:
        late, dropped = stream.get("late", 0), stream.get("dropped", 0)
        findings.append(
            f"{stream.get('bursts')} bursts played, expected {expected}"
            + (f" ({late} late, {dropped} dropped)" if late or dropped else "")
        )
    return findings


def accounted(stream: dict[str, Any]) -> int:
    """The bursts of a stream the board accounts for: played, or arrived too late to play, or
    dropped for want of room (the server's own test: it fails a run that does not account for
    every burst it sent). A late burst is a glitch and is counted as one; a burst that is none
    of these is a stream that lost data."""
    return int(stream.get("bursts", 0)) + int(stream.get("late", 0)) + int(stream.get("dropped", 0))


def glitches(run: SendspinRun) -> list[str]:
    """What a stream that played through had wrong with it, by the board's own totals: late,
    dropped or invalid chunks, sink underruns, resynchronisations. The C6 does this on about one
    30 s AC-3 stream in seven, so a row holds how many of its plays did, not that none did."""
    stream = run.stream or {}
    found = []
    for name, label in (
        ("late", "late chunk(s)"),
        ("dropped", "dropped chunk(s)"),
        ("invalid", "invalid chunk(s)"),
        ("underruns", "sink underrun(s)"),
        ("resyncs", "resynchronisation(s)"),
    ):
        if stream.get(name):
            found.append(f"{stream[name]} {label}")
    return found


def figures(run: SendspinRun, period_us: int) -> dict[str, Any]:
    """The row's figures, by the names the suite's baselines and `compare` use where the
    /play rows have one: decoder_permille is the board's mean time to decode a burst over the
    burst's duration."""
    out: dict[str, Any] = {}
    stream = run.stream or {}
    if "bursts" in stream:
        out["frames"] = stream["bursts"]
    if stream.get("burst_us") is not None and period_us:
        out["decoder_permille"] = round(stream["burst_us"] * 1000 / period_us)
        out["burst_us"] = stream["burst_us"]
    status = run.player().get("status") or {}
    stack = status.get("decode_stack_free")
    if stack is None:
        stack = run.after.get("decode_stack_free")
    if stack is not None:
        out["decode_stack_free"] = stack
    for name in ("underruns", "late", "dropped", "invalid", "resyncs", "worst_error_us"):
        if name in stream:
            out[name] = stream[name]
    if "heap_free" in stream:
        out["play_heap_free"] = stream["heap_free"]
    if "ring_high" in stream:
        out["ring_high"] = stream["ring_high"]
    return out


def ours_in(table: dict[str, str]) -> list[str]:
    """The ids in a pairing table ({id: name}) that are the suite's record, by the name the suite's
    server gives."""
    return [server_id for server_id, name in table.items() if name == SERVER_NAME]


def play_row(
    board: BoardLike,
    testserver: Path,
    state: Path,
    programme: Path,
    label: str,
    pairing: Pairing,
    *,
    status_port: int = 80,
    timeout_s: float = 150.0,
    popen: Callable[..., Any] = subprocess.Popen,
    relay_every: float = 0.4,
    log_wait_s: float = 8.0,
    seconds: int | None = None,
    tone: bool = False,
) -> SendspinRun:
    """One row: by the record where the board holds the suite's, by a code where it does not. A
    record the board does not take (the server's own copy was lost) is forgotten and the row is
    played again by a code, which is noted and is not a finding."""
    common: dict[str, Any] = {
        "status_port": status_port,
        "timeout_s": timeout_s,
        "popen": popen,
        "relay_every": relay_every,
        "log_wait_s": log_wait_s,
        "seconds": seconds,
        "tone": tone,
    }
    held = ours_in(pairing_ids(board.pairing()))
    run = run_row(
        board, testserver, state, programme, label, use_code=not held,
        connect_s=25 if held else 60, **common,
    )  # fmt: skip
    pairing.learned(run)
    if held and run.exit_code == 2:
        for server_id in ours_in(pairing_ids(board.pairing())):
            board.forget(server_id)
        run = run_row(board, testserver, state, programme, label, use_code=True, **common)
        run.notes.append("the board did not take the suite's stored record; paired again by a code")
        pairing.learned(run)
    return run


class Pairing:
    """The suite's record in the board's pairing table, from before the run to its end."""

    def __init__(self, board: BoardLike, keep: bool = False) -> None:
        self.board = board
        self.keep = keep
        self.before: dict[str, str] = pairing_ids(board.pairing())
        self.server_id: str | None = None

    def learned(self, run: SendspinRun) -> None:
        if run.server_id and not self.server_id:
            self.server_id = run.server_id

    def close(self) -> str:
        """Forgets the suite's own record unless it was kept or was there before the run, and says
        what it did. Never a bare `forget`: that drops the owner's Hearth."""
        if self.server_id is None:
            return "pairing: the suite's server id was never learned: nothing to forget"
        now = pairing_ids(self.board.pairing())
        if self.server_id not in now:
            return "pairing: the board holds no record of the suite's"
        ours = self.server_id not in self.before or now.get(self.server_id) == SERVER_NAME
        if self.keep:
            return f"pairing: kept the record of {self.server_id[:8]}… (--keep-pairing)"
        if not ours:
            return f"pairing: the record of {self.server_id[:8]}… was there before: kept"
        code = self.board.forget(self.server_id)
        return f"pairing: forgot the suite's record {self.server_id[:8]}… (POST answered {code})"


def main() -> int:  # pragma: no cover - the module is a library; this is a probe
    print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
