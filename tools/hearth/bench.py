#!/usr/bin/env python3
"""The bench: the ESP32 boards, their locks, and reading them without a network value leaking.

    python tools/hearth/bench.py boards
    python tools/hearth/bench.py status [--probe]
    python tools/hearth/bench.py lock   take|release|show BOARD --session ID [--purpose TEXT]
    python tools/hearth/bench.py log    BOARD [--tail N]
    python tools/hearth/bench.py snapshot BOARD [BOARD ...]
    python tools/hearth/bench.py console BOARD [--seconds N] [--reset] [--until TEXT] [--out FILE]
    python tools/hearth/bench.py scrub  --check|--mask|--shape [FILE ...]

Everything here reads: nothing writes a board's flash (that is bench_flash.py, which holds the
same rules). The rules are the owner's of 2026-10-11, planning/esp32-board-bench.md has them in
full: a board nobody has booked is not one to use, every lock file is read before one is
taken, and a lock that is not this session's is left alone however old it is.

- **boards** and **status**: the table in bench.json, and for each board its lock (who, why, until
  when, and "overdue" past that, which is a reason to ask that session, not to take the board).
  `--probe` adds the board's GET /status state, a read.
- **lock**: takes a board's lock file atomically (the line every session uses), shows it, or
  releases it when it is this session's.
- **log**, **snapshot** and **console**: what a board says, with the network's SSID and password,
  the access point's address and the Sendspin pairing token taken out before anything is printed
  or written. GET /log carries the SSID in its `connected with` line; a script that prints it is
  one that leaks.
- **scrub**: counts or masks the network fragment's values in files, and never prints them.

Standard library only, except `console`, which needs pyserial.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import http.client
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
BENCH_FILE = HERE / "bench.json"
ENVIRONMENT = "ICLFORGE_BENCH"

# What GET /status says that is not about the network. The network object is never copied.
SECRET_WORDS = ("ssid", "bssid", "password", "psk", "token", "secret")


class BenchError(Exception):
    """Why a command will not run; main() prints it and exits 1."""


# --- the table -------------------------------------------------------------------------


class Board:
    """One row of bench.json, with the bench's lock directory."""

    def __init__(self, name: str, row: dict[str, Any], locks: Path) -> None:
        self.name = name
        self.chip: str = row["chip"]
        self.host: str = row["host"]
        self.port: str = row["port"]
        self.mac_tail: str = row["mac_tail"]
        self.lock: Path = locks / row["lock"]
        self.after_flash: str = row.get("after_flash", "hard-reset")
        self.ota: bool = bool(row.get("ota", True))
        self.restart_kills_c6_link: bool = bool(row.get("restart_kills_c6_link", False))
        self.log_bytes: int | None = row.get("log_bytes")
        self.note: str = row.get("note", "")


class Bench:
    def __init__(self, data: dict[str, Any]) -> None:
        self.data = data
        self.locks = Path(data["locks"])
        self.idf = Path(data["idf"])
        self.idf_tools = Path(data["idf_tools"])
        self.builds = Path(data["builds"])
        self.wifi_fragment = Path(data["wifi_fragment"])
        self.boards = {name: Board(name, row, self.locks) for name, row in data["boards"].items()}

    def board(self, name: str) -> Board:
        try:
            return self.boards[name]
        except KeyError:
            known = ", ".join(self.boards)
            raise BenchError(f"no board named {name!r} on the bench (it has {known})") from None


def load(path: Path | None = None) -> Bench:
    chosen = path or Path(os.environ.get(ENVIRONMENT) or BENCH_FILE)
    try:
        data = json.loads(chosen.read_text("utf-8"))
    except (OSError, ValueError) as error:
        raise BenchError(f"{chosen}: {error}") from error
    if data.get("format") != 1:
        raise BenchError(f"{chosen}: format {data.get('format')!r}, this reads 1")
    return Bench(data)


# --- booking ---------------------------------------------------------------------------


def _stamp(moment: datetime.datetime) -> str:
    return moment.strftime("%Y-%m-%dT%H:%MZ")


def _holds(line: str, session: str) -> bool:
    return f"session={session} " in line or f"session={session}|" in line


def take_lock(path: Path, session: str, purpose: str, minutes: int, what: str = "bench") -> str:
    """The booking line, written atomically: a lock that exists is not taken over."""
    now = datetime.datetime.now(datetime.timezone.utc)
    end = now + datetime.timedelta(minutes=minutes)
    line = (
        f"session={session} ({what}) | purpose={purpose} | start={_stamp(now)} | "
        f"expected_end={_stamp(end)}"
    )
    try:
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError as error:
        raise BenchError(f"{path} is booked: {read_lock(path) or '(unreadable)'}") from error
    except OSError as error:
        raise BenchError(f"{path} cannot be created: {error}") from error
    with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
        handle.write(line + "\n")
    return line


def read_lock(path: Path) -> str | None:
    """The lock's line, or None where there is no lock."""
    try:
        return path.read_text("utf-8", "replace").strip()
    except FileNotFoundError:
        return None
    except OSError:
        return "(unreadable)"


def hold_lock(
    path: Path, session: str, purpose: str, minutes: int, what: str = "bench"
) -> tuple[str, bool]:
    """The booking for a run: the lock taken, or, where the file is already this session's (it
    booked the board to do something first, such as put an image on it), that line, which the
    run then leaves for its owner to release. Returns the line and whether the run took it."""
    try:
        return take_lock(path, session, purpose, minutes, what), True
    except BenchError:
        held = read_lock(path) or ""
        if _holds(held, session):
            return held, False
        raise


def release_lock(path: Path, session: str) -> str:
    """Removes the lock only if it is this session's."""
    held = read_lock(path)
    if held is None:
        return "no lock file"
    if not _holds(held, session):
        raise BenchError(f"{path} is not this session's, and is left alone: {held}")
    path.unlink()
    return "released"


def require_lock(board: Board, session: str) -> str:
    """The line of a lock this session holds, or why the board is not to be touched."""
    held = read_lock(board.lock)
    if held is None:
        raise BenchError(
            f"{board.name} is not booked: take {board.lock} first (bench.py lock take {board.name})"
        )
    if not _holds(held, session):
        raise BenchError(f"{board.name} is booked by another session: {held}")
    return held


_LOCK_FIELDS = re.compile(r"(session|purpose|start|expected_end)=([^|]*)")


def parse_lock(line: str) -> dict[str, str]:
    """The fields of a booking line; the `(what)` after the session id is kept as `what`."""
    out = {key: value.strip() for key, value in _LOCK_FIELDS.findall(line)}
    session = out.get("session", "")
    match = re.match(r"(\S+)\s*\((.*)\)\s*$", session)
    if match:
        out["session"], out["what"] = match.group(1), match.group(2)
    return out


def overdue_minutes(fields: dict[str, str], now: datetime.datetime | None = None) -> int | None:
    """Minutes past expected_end, or None when it is not past or not readable."""
    text = fields.get("expected_end", "")
    try:
        end = datetime.datetime.strptime(text, "%Y-%m-%dT%H:%MZ").replace(
            tzinfo=datetime.timezone.utc
        )
    except ValueError:
        return None
    now = now or datetime.datetime.now(datetime.timezone.utc)
    late = int((now - end).total_seconds() // 60)
    return late if late > 0 else None


# --- reading a board over HTTP ---------------------------------------------------------


def http_get(host: str, path: str, timeout: float = 8.0) -> tuple[int, dict[str, str], bytes]:
    connection = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        connection.request("GET", path)
        response = connection.getresponse()
        return (
            response.status,
            {k.lower(): v for k, v in response.getheaders()},
            response.read(),
        )
    except (OSError, http.client.HTTPException) as error:
        raise BenchError(f"{host}: GET {path}: {error}") from error
    finally:
        connection.close()


def redact(value: Any) -> Any:
    """JSON with every key that looks like a network value replaced; the values are not read."""
    if isinstance(value, dict):
        return {
            key: ("<redacted>" if any(word in key.lower() for word in SECRET_WORDS) else redact(v))
            for key, v in value.items()
        }
    if isinstance(value, list):
        return [redact(v) for v in value]
    return value


# --- the network fragment's values: masked, counted, never printed ------------------------


def wifi_values(fragment: Path) -> dict[str, str]:
    """{'<wifi-ssid>': value, '<wifi-password>': value} from the fragment; {} where it is absent
    (a machine with no bench has none to leak)."""
    values: dict[str, str] = {}
    try:
        text = fragment.read_text("latin-1")
    except OSError:
        return values
    for line in text.splitlines():
        key, _, value = line.partition("=")
        if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        if key.endswith("_SSID"):
            values["<wifi-ssid>"] = value
        elif key.endswith("_PASSWORD"):
            values["<wifi-password>"] = value
    return values


def variants(value: str) -> set[str]:
    """The value as it may appear in text: raw, JSON-escaped, percent-escaped, as hex."""
    raw = value.encode("latin-1", "replace")
    found = {
        value,
        value.replace("\\", "\\\\").replace('"', '\\"'),
        "".join(f"%{b:02X}" for b in raw),
        " ".join(f"{b:02x}" for b in raw),
        raw.hex(),
    }
    return {v for v in found if v}


_TOKEN_LINE = re.compile(r"pairing token|psk|passkey", re.IGNORECASE)
_BSSID = re.compile(r"(bssid\s*=\s*|ifx:\d,\s*)(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")
_KEYED = re.compile(r"(ssid|password|psk)([=:])\s*\S+", re.IGNORECASE)


def mask_line(line: str, values: dict[str, str]) -> str:
    """One console line with the network's values, the access point's address and a pairing
    token taken out. A line that carries a token is dropped whole."""
    if _TOKEN_LINE.search(line):
        return "<line with a pairing token or key dropped>"
    for token, value in values.items():
        for variant in sorted(variants(value), key=len, reverse=True):
            line = line.replace(variant, token)
    line = _KEYED.sub(lambda m: f"{m.group(1)}{m.group(2)}<redacted>", line)
    return _BSSID.sub(lambda m: m.group(1) + "<bssid>", line)


def mask_text(text: str, values: dict[str, str]) -> str:
    return "\n".join(mask_line(line, values) for line in text.split("\n"))


def count_occurrences(text: str, values: dict[str, str]) -> int:
    return sum(text.count(v) for value in values.values() for v in variants(value))


# --- commands ---------------------------------------------------------------------------


def cmd_boards(bench: Bench, _args: argparse.Namespace) -> int:
    for board in bench.boards.values():
        extra = "" if board.ota else "  (no OTA: USB only)"
        print(
            f"{board.name:4} {board.chip:8} {board.host:15} {board.port:6} {board.lock.name}{extra}"
        )
    return 0


def cmd_status(bench: Bench, args: argparse.Namespace) -> int:
    free = 0
    for board in bench.boards.values():
        held = read_lock(board.lock)
        if held is None:
            line = "free"
            free += 1
        else:
            fields = parse_lock(held)
            late = overdue_minutes(fields)
            line = (
                f"booked by {fields.get('session', '?')} ({fields.get('what', '?')}): "
                f"{fields.get('purpose', '?')}; until {fields.get('expected_end', '?')}"
            )
            if late is not None:
                line += f"  OVERDUE by {late} min: ask that session, do not take the board over"
        state = ""
        if args.probe:
            try:
                _, _, body = http_get(board.host, "/status", timeout=4.0)
                state = f"  /status {json.loads(body).get('state')}"
            except (BenchError, ValueError) as error:
                state = f"  /status unreachable ({error})"
        print(f"{board.name:4} {line}{state}")
    print(
        f"{free} of {len(bench.boards)} free. The locks are not all there is: ListAgents, and "
        "a running idf.py, esptool, ota.py or capture, are not visible here."
    )
    return 0


def cmd_lock(bench: Bench, args: argparse.Namespace) -> int:
    board = bench.board(args.board)
    if args.action == "show":
        print(read_lock(board.lock) or f"{board.name}: no lock")
        return 0
    if not args.session:
        raise BenchError("--session is required: the lock names its owner")
    if args.action == "take":
        if not args.purpose:
            raise BenchError("--purpose is required: the next session reads it")
        print(take_lock(board.lock, args.session, args.purpose, args.minutes, args.what))
        return 0
    print(release_lock(board.lock, args.session))
    return 0


def cmd_log(bench: Bench, args: argparse.Namespace) -> int:
    board = bench.board(args.board)
    status, _, body = http_get(board.host, "/log")
    if status != 200:
        raise BenchError(f"{board.host}: GET /log answered {status}")
    text = body.decode("utf-8", "replace")
    lines = mask_text(text, wifi_values(bench.wifi_fragment)).splitlines()
    for line in lines[-args.tail :] if args.tail else lines:
        print(line)
    return 0


def cmd_snapshot(bench: Bench, args: argparse.Namespace) -> int:
    for name in args.boards:
        board = bench.board(name)
        print(f"=== {board.name} {board.host}")
        for path in ("/hardware", "/status"):
            try:
                status, _, body = http_get(board.host, path)
                print(f"--- GET {path} -> {status}")
                print(json.dumps(redact(json.loads(body)), indent=1))
            except (BenchError, ValueError) as error:
                print(f"--- GET {path}: {error}")
    return 0


def cmd_scrub(bench: Bench, args: argparse.Namespace) -> int:
    values = wifi_values(bench.wifi_fragment)
    if args.mode == "shape":
        for token, value in values.items():
            print(
                token,
                {
                    "length": len(value),
                    "plain-alnum-space-underscore-hyphen": bool(
                        re.fullmatch(r"[A-Za-z0-9 _-]*", value)
                    ),
                    "all-ascii": value.isascii(),
                },
            )
        return 0
    if not values:
        raise BenchError(f"no network values to look for: {bench.wifi_fragment} is not readable")
    total = 0
    for name in args.files:
        path = Path(name)
        text = path.read_bytes().decode("latin-1")
        hits = count_occurrences(text, values)
        total += hits
        if args.mode == "mask" and hits:
            masked = text
            for token, value in values.items():
                for variant in sorted(variants(value), key=len, reverse=True):
                    masked = masked.replace(variant, token)
            path.write_bytes(masked.encode("latin-1"))
        note = " masked" if args.mode == "mask" and hits else ""
        print(f"{path}: {hits} occurrence(s){note}")
    return 0 if args.mode == "mask" or total == 0 else 1


def rewrite_dtr(connection) -> None:
    """Write DTR with the value it already has. Windows' usbser.sys driver sends the line state
    only when DTR is written, so a change to RTS is not seen until it is."""
    state = connection.dtr
    connection.dtr = state


def cmd_console(bench: Bench, args: argparse.Namespace) -> int:
    board = bench.board(args.board)
    try:
        import serial  # noqa: PLC0415 - only this command needs pyserial
    except ImportError as error:
        raise BenchError("console needs pyserial: python -m pip install pyserial") from error
    values = wifi_values(bench.wifi_fragment)
    out = (
        open(args.out, "w", encoding="utf-8", newline="\n")  # noqa: SIM115 - closed below
        if args.out
        else sys.stdout
    )
    started = time.time()

    def emit(text: str) -> None:
        out.write(f"[{time.time() - started:7.2f}] {mask_line(text, values)}\n")
        out.flush()

    connection = None
    buffer = b""
    pulse_pending = args.reset
    opens = 0
    stop = False
    emit(f"{board.name} {board.port} for up to {args.seconds:g} s (reset={args.reset})")
    try:
        while time.time() - started < args.seconds and not stop:
            try:
                if connection is None:
                    # dtr and rts are set before open(): opening the port neither resets the
                    # chip nor puts it in download mode.
                    candidate = serial.Serial()
                    candidate.port = board.port
                    candidate.baudrate = 115200
                    candidate.dtr = False
                    candidate.rts = False
                    candidate.timeout = 0.2
                    candidate.open()
                    connection = candidate
                    opens += 1
                    emit(f"-- port open (#{opens})")
                    if pulse_pending:
                        pulse_pending = False
                        # idf.py monitor's sequence, with its usbser.sys workaround: Windows'
                        # driver sends the line state only when DTR is written.
                        rewrite_dtr(connection)
                        connection.dtr = False
                        connection.rts = True
                        rewrite_dtr(connection)
                        time.sleep(0.2)
                        connection.rts = False
                        rewrite_dtr(connection)
                        emit("-- RTS pulse sent (EN low for 0.2 s)")
                chunk = connection.read(512)
                if chunk:
                    buffer += chunk
                    while b"\n" in buffer:
                        raw, buffer = buffer.split(b"\n", 1)
                        text = raw.decode("utf-8", "replace").rstrip("\r")
                        emit(text)
                        if args.until and args.until in text:
                            stop = True
            except Exception:  # the port vanishes and comes back at a restart
                if connection is not None:
                    with contextlib.suppress(Exception):
                        connection.close()
                connection = None
                time.sleep(0.2)
        if buffer:
            emit(buffer.decode("utf-8", "replace"))
        emit("capture ended")
    finally:
        if connection is not None:
            connection.close()
        if args.out:
            out.close()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("boards", help="the table in bench.json").set_defaults(func=cmd_boards)

    status = sub.add_parser("status", help="every board's lock, and with --probe its /status")
    status.add_argument("--probe", action="store_true")
    status.set_defaults(func=cmd_status)

    lock = sub.add_parser("lock", help="take, show or release a board's lock")
    lock.add_argument("action", choices=("take", "show", "release"))
    lock.add_argument("board")
    lock.add_argument("--session")
    lock.add_argument("--purpose")
    lock.add_argument("--minutes", type=int, default=30)
    lock.add_argument("--what", default="bench", help="the words in brackets after the session")
    lock.set_defaults(func=cmd_lock)

    log = sub.add_parser("log", help="GET /log, with the network's values taken out")
    log.add_argument("board")
    log.add_argument("--tail", type=int, default=0, help="the last N lines (0 = all)")
    log.set_defaults(func=cmd_log)

    snapshot = sub.add_parser("snapshot", help="/hardware and /status, redacted")
    snapshot.add_argument("boards", nargs="+")
    snapshot.set_defaults(func=cmd_snapshot)

    console = sub.add_parser("console", help="the serial console, masked (needs pyserial)")
    console.add_argument("board")
    console.add_argument("--seconds", type=float, default=30.0)
    console.add_argument("--reset", action="store_true", help="pulse RTS once after opening")
    console.add_argument("--until", help="stop once a line contains this text")
    console.add_argument("--out", help="a file, in place of the terminal")
    console.set_defaults(func=cmd_console)

    scrub = sub.add_parser("scrub", help="count or mask the network's values in files")
    group = scrub.add_mutually_exclusive_group(required=True)
    for mode in ("check", "mask", "shape"):
        group.add_argument(f"--{mode}", dest="mode", action="store_const", const=mode)
    scrub.add_argument("files", nargs="*")
    scrub.set_defaults(func=cmd_scrub)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(load(), args))
    except BenchError as error:
        print(f"bench: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
