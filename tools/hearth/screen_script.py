#!/usr/bin/env python3
"""Screen scripts: taps on the Hearth sink's screen, written once, run on the host and on a board.

    python tools/hearth/screen_script.py check SCRIPT...
    python tools/hearth/screen_script.py run   SCRIPT... --preview PANEL_PREVIEW [--out DIR]
    python tools/hearth/screen_script.py probe --board HOST --preview PANEL_PREVIEW --script SCRIPT
    python tools/hearth/screen_script.py run   SCRIPT... --preview PANEL_PREVIEW --board HOST \
        --lease D:/ac3bld/board-kit.lock --session ID [--restore]

A script (firmware/hearth-sink/preview/scripts/*.screen, planning/screen-test-harness.md) is a few
lines of what a person does at the glass and what they should then see:

    board p4-rev3-kit           # header: the BoardCaps variant (board_variants.hpp), glass, scene
    display 1024 600
    scene lyrics
    tap "Preferences"           # a button, found by what it says; scrolled to by finger if need be
    tap "+" in "Brightness"     # ... in the Preferences row of that name
    expect pref brightness = 90 # GET /preferences: the value the board holds
    expect shown "90%"          # a label on the screen says exactly this (text: contains it)
    tap-at 500 300              # a raw touch; swipe X Y1 Y2: a finger dragged along a column
    wait 1000                   # milliseconds of the board's clock
    shot after-plus             # a picture, into --out

Backends. The *preview* (`panel_preview --drive`: the firmware's own screen code on LVGL's software
renderer, firmware/hearth-sink/preview/preview.cpp) runs every step. A *board* is run in step with
it: each step runs on the preview, the touches it made (the answer's `events`) are made on the board
through POST :8081/panel/tap, and what the board's GET /preferences says is held to what the
preview's says after every step. `expect pref` is read from the board; `expect shown` and `text`
are read from the preview (they test the screen code, not the glass).

A board is written to, so `--board` with `run` needs the board's lease (D:/ac3bld/BOARD-LOCKS.md):
`--lease` names the lock file and `--session` the id that lock must carry. `probe` only reads
(GET /preferences, GET /panel.bmp) and needs none. Standard library only, as the CI container's
Python has nothing else.
"""

from __future__ import annotations

import argparse
import contextlib
import http.client
import json
import os
import queue
import shlex
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

SCRIPTS = Path(__file__).resolve().parents[2] / "firmware" / "hearth-sink" / "preview" / "scripts"
HEADERS = ("board", "display", "scene")
EXPECTS = ("pref", "shown", "not-shown", "text", "no-text")
SCENES = ("lyrics", "idle")


class ScriptError(ValueError):
    """A script that is not one: the file and line come first in the message."""


class StepError(RuntimeError):
    """A step that could not be done or whose expectation did not hold."""


@dataclass(frozen=True)
class Step:
    line: int
    verb: str
    args: tuple[str, ...]
    source: str


@dataclass
class Script:
    name: str
    board: str = "p4-rev3-kit"
    width: int = 1024
    height: int = 600
    scene: str = "lyrics"
    steps: list[Step] = field(default_factory=list)


def _int(name: str, line: int, word: str) -> int:
    try:
        return int(word)
    except ValueError:
        raise ScriptError(f"{name}:{line}: {word!r} is not a whole number") from None


def parse(text: str, name: str = "<script>") -> Script:
    """The script a text is, or ScriptError saying which line is wrong."""
    script = Script(name)
    for number, raw in enumerate(text.splitlines(), start=1):
        try:
            words = shlex.split(raw, comments=True)
        except ValueError as error:
            raise ScriptError(f"{name}:{number}: {error}") from None
        if not words:
            continue
        verb, args = words[0], tuple(words[1:])
        here = f"{name}:{number}: {verb}"
        if verb in HEADERS:
            if script.steps:
                raise ScriptError(f"{here} comes after a step; the header is first")
            if verb == "board" and len(args) == 1:
                script.board = args[0]
            elif verb == "display" and len(args) == 2:
                script.width, script.height = (
                    _int(name, number, args[0]),
                    _int(name, number, args[1]),
                )
            elif verb == "scene" and len(args) == 1 and args[0] in SCENES:
                script.scene = args[0]
            else:
                raise ScriptError(
                    f"{here}: wrong arguments (board NAME | display W H | scene {'|'.join(SCENES)})"
                )
            continue
        if verb == "tap" and len(args) == 1:
            pass
        elif verb == "tap" and len(args) == 3 and args[1] == "in":
            args = (args[0], args[2])
        elif verb == "tap-at" and len(args) == 2:
            _int(name, number, args[0]), _int(name, number, args[1])
        elif verb == "swipe" and len(args) == 3:
            for word in args:
                _int(name, number, word)
        elif verb == "wait" and len(args) == 1:
            _int(name, number, args[0])
        elif verb == "shot" and len(args) == 1:
            pass
        elif verb == "expect" and len(args) == 4 and args[0] == "pref" and args[2] == "=":
            args = (args[0], args[1], args[3])
        elif verb == "expect" and len(args) == 2 and args[0] in EXPECTS[1:]:
            pass
        else:
            raise ScriptError(
                f"{here}: not a step (tap TEXT [in ROW] | tap-at X Y | swipe X Y1 Y2 | wait MS | "
                f"shot NAME | "
                f"expect pref ID = VALUE | expect {'|'.join(EXPECTS[1:])} TEXT)"
            )
        script.steps.append(Step(number, verb, args, raw.strip()))
    return script


def load(path: Path) -> Script:
    return parse(path.read_text(encoding="utf-8"), path.name)


# --- backends ------------------------------------------------------------------------------------


class Backend(Protocol):
    """What a step is done with. The preview and a board in step with it both are one."""

    def setup(self, script: Script) -> None: ...
    def act(self, step: Step) -> None: ...
    def prefs(self) -> dict[str, Any]: ...
    def shown(self, text: str, contains: bool) -> bool: ...
    def shot(self, name: str, out: Path) -> None: ...
    def close(self) -> None: ...


def quote(word: str) -> str:
    return '"' + word.replace("\\", "\\\\").replace('"', '\\"') + '"'


class PreviewBackend:
    """`panel_preview --drive`: a command a line on stdin, a line of JSON back (preview.cpp)."""

    def __init__(self, command: list[str], timeout: float = 60.0) -> None:
        self.timeout = timeout
        self.events: list[list[Any]] = []
        self.process = subprocess.Popen(
            [*command, "--drive"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self._lines.put(line)
        self._lines.put(None)

    def call(self, *words: str) -> dict[str, Any]:
        assert self.process.stdin is not None
        try:
            self.process.stdin.write(" ".join(quote(w) for w in words) + "\n")
            self.process.stdin.flush()
            line = self._lines.get(timeout=self.timeout)
        except (BrokenPipeError, queue.Empty):
            self.process.kill()
            raise StepError("the preview did not answer") from None
        if line is None:
            raise StepError("the preview ended")
        answer = json.loads(line)
        self.events = answer.get("events", [])
        if not answer.get("ok"):
            raise StepError(answer.get("error", "the preview refused"))
        return answer

    def setup(self, script: Script) -> None:
        self.call("init", script.board, str(script.width), str(script.height), script.scene)

    def act(self, step: Step) -> None:
        if step.verb == "tap":
            self.call("tap", *step.args)
        elif step.verb in ("tap-at", "swipe", "wait"):
            self.call(step.verb, *step.args)
        else:
            raise StepError(f"{step.verb} is not an action")

    def prefs(self) -> dict[str, Any]:
        return self.call("prefs")["prefs"]

    def shown(self, text: str, contains: bool) -> bool:
        return bool(self.call("shown", text, *(["contains"] if contains else []))["value"])

    def shot(self, name: str, out: Path) -> None:
        self.call("shot", str(out / f"{name}.bmp"))

    def close(self) -> None:
        with contextlib.suppress(Exception):
            assert self.process.stdin is not None
            self.process.stdin.write("quit\n")
            self.process.stdin.close()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
        self._reader.join(timeout=5)
        if self.process.stdout is not None:
            self.process.stdout.close()


def values_of(listing: dict[str, Any]) -> dict[str, str]:
    return {p["id"]: p["value"] for p in listing["preferences"]}


class BoardBackend:
    """A board run in step with a preview: the preview says where the finger goes."""

    SIMPLE = ("choice", "stepper", "toggle")

    def __init__(
        self,
        host: str,
        shadow: PreviewBackend,
        port: int = 80,
        panel_port: int = 8081,
        settle: float = 0.4,
    ):
        self.host, self.port, self.panel_port, self.settle = host, port, panel_port, settle
        self.shadow = shadow
        self.before: dict[str, Any] = {}

    def request(
        self, method: str, port: int, path: str, body: bytes | None = None, read: int | None = None
    ) -> bytes:
        connection = http.client.HTTPConnection(self.host, port, timeout=15)
        try:
            connection.request(method, path, body=body)
            response = connection.getresponse()
            data = response.read() if read is None else response.read(read)
            if response.status != 200:
                raise StepError(
                    f"{method} :{port}{path} answered {response.status}: {data[:120]!r}"
                )
            return data
        except OSError as error:
            raise StepError(f"{method} :{port}{path}: {error}") from None
        finally:
            connection.close()

    def glass(self) -> tuple[int, int]:
        """The size of the picture on the board, from the head of GET /panel.bmp."""
        head = self.request("GET", self.panel_port, "/panel.bmp", read=26)
        if len(head) < 26 or head[:2] != b"BM":
            raise StepError("GET /panel.bmp did not answer with a BMP")
        return struct.unpack_from("<ii", head, 18)

    def setup(self, script: Script) -> None:
        self.shadow.setup(script)
        board = json.loads(self.request("GET", self.port, "/preferences"))
        preview = self.shadow.prefs()
        if board["profile"] != preview["profile"]:
            raise StepError(
                f"the board is {board['profile']!r} and the script's {script.board} "
                f"is {preview['profile']!r}"
            )
        if self.glass() != (script.width, script.height):
            raise StepError(
                f"the board's glass is {self.glass()} and the script's "
                f"is {(script.width, script.height)}"
            )
        # A screen that is dark, or already on a sheet, takes the first touch as something else
        # (a touch wakes it): the script starts from the home screen, lit.
        state = json.loads(self.request("GET", self.panel_port, "/panel"))
        for key, why in (
            ("dimmed", "dimmed: a touch would only wake it"),
            ("prefs_view", "already on Preferences"),
            ("dev_view", "already on the developer view"),
        ):
            if state.get(key):
                raise StepError(f"the board's screen is {why}; start from the home screen, lit")
        self.before = board

    def prefs(self) -> dict[str, Any]:
        return json.loads(self.request("GET", self.port, "/preferences"))

    def act(self, step: Step) -> None:
        self.shadow.act(step)
        if step.verb == "wait":
            time.sleep(int(step.args[0]) / 1000)
        for event in self.shadow.events:
            if event[0] == "tap":
                self.request("POST", self.panel_port, f"/panel/tap?x={event[1]}&y={event[2]}")
            else:
                raise StepError(
                    f"the board's :{self.panel_port} has no {event[0]} route yet "
                    "(planning/screen-test-harness.md, slice 2)"
                )
        time.sleep(self.settle)
        board, preview = values_of(self.prefs()), values_of(self.shadow.prefs())
        differ = sorted(i for i in preview if board.get(i) != preview[i])
        if differ:
            raise StepError(
                "after this step the board and the preview differ: "
                + ", ".join(
                    f"{i} is {board.get(i)!r} on the board, {preview[i]!r} on the preview"
                    for i in differ
                )
            )

    def shown(self, text: str, contains: bool) -> bool:
        return self.shadow.shown(text, contains)

    def shot(self, name: str, out: Path) -> None:
        (out / f"{name}.board.bmp").write_bytes(self.request("GET", self.panel_port, "/panel.bmp"))
        self.shadow.shot(name + ".preview", out)

    def restore(self) -> list[str]:
        """Puts back what the script changed by each preference's route; says what it could not."""
        left = []
        now = {p["id"]: p for p in self.prefs()["preferences"]}
        for was in self.before["preferences"]:
            if now[was["id"]]["value"] == was["value"]:
                continue
            if was["kind"] in self.SIMPLE and was["route"]:
                self.request("PUT", self.port, was["route"], body=was["value"].encode())
            else:
                left.append(
                    f"{was['id']} ({was['kind']}) was {was['value']!r} "
                    f"and is {now[was['id']]['value']!r}"
                )
        return left

    def close(self) -> None:
        self.shadow.close()


# --- running -------------------------------------------------------------------------------------


def run(script: Script, backend: Backend, out: Path | None = None) -> None:
    """Does every step in order; the first that fails raises StepError naming its line."""
    out = out or Path(".")
    backend.setup(script)
    for step in script.steps:
        try:
            if step.verb in ("tap", "tap-at", "swipe", "wait"):
                backend.act(step)
            elif step.verb == "shot":
                backend.shot(step.args[0], out)
            elif step.args[0] == "pref":
                entries = {p["id"]: p for p in backend.prefs()["preferences"]}
                if step.args[1] not in entries:
                    raise StepError(f"no preference is called {step.args[1]!r}")
                said = entries[step.args[1]]["value"]
                if said != step.args[2]:
                    raise StepError(f"{step.args[1]} is {said!r}, not {step.args[2]!r}")
            else:
                kind, text = step.args
                want = kind in ("shown", "text")
                if backend.shown(text, kind in ("text", "no-text")) != want:
                    raise StepError(f"{text!r} is {'not ' if want else ''}on the screen")
        except StepError as error:
            raise StepError(f"{script.name}:{step.line}: {step.source}  ->  {error}") from None


def check_lease(path: Path, session: str) -> None:
    """The board's lock file must exist and carry this session (D:/ac3bld/BOARD-LOCKS.md)."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise StepError(f"no lease: {error}") from None
    holder = next((line.partition("=")[2] for line in lines if line.startswith("session=")), "")
    if not session or session not in holder:
        raise StepError(f"{path} is held by {holder!r}, not by {session!r}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "run"):
        one = sub.add_parser(name)
        one.add_argument(
            "scripts", nargs="*", type=Path, help="default: every script in preview/scripts"
        )
    probe = sub.add_parser("probe", help="read a board and the preview, touch nothing")
    probe.add_argument("--script", type=Path, required=True)
    for one in (sub.choices["run"], probe):
        one.add_argument(
            "--preview",
            default=os.environ.get("ICLFORGE_PANEL_PREVIEW"),
            help="the panel_preview binary",
        )
        one.add_argument(
            "--board", help="a board's address: the same steps, on it, in step with the preview"
        )
        one.add_argument("--panel-port", type=int, default=8081)
    sub.choices["run"].add_argument("--out", type=Path, default=Path("screen-script-out"))
    sub.choices["run"].add_argument("--lease", type=Path, help="the board's lock file")
    sub.choices["run"].add_argument("--session", default="")
    sub.choices["run"].add_argument(
        "--restore", action="store_true", help="put back the preferences the script changed"
    )
    args = parser.parse_args(argv)

    if args.command == "probe":
        script = load(args.script)
        shadow = PreviewBackend([args.preview])
        board = BoardBackend(args.board, shadow, panel_port=args.panel_port)
        try:
            board.setup(script)
            board_values, preview_values = values_of(board.prefs()), values_of(shadow.prefs())
            same = sum(board_values.get(i) == v for i, v in preview_values.items())
            print(
                f"{script.board}: glass {board.glass()}, "
                f"{same} of {len(preview_values)} values equal"
            )
            for i, v in preview_values.items():
                if board_values.get(i) != v:
                    print(f"  {i}: board {board_values.get(i)!r}, preview {v!r}")
        finally:
            shadow.close()
        return 0

    paths = args.scripts or sorted(SCRIPTS.glob("*.screen"))
    try:
        scripts = [load(p) for p in paths]
    except ScriptError as error:
        print(error, file=sys.stderr)
        return 2
    if args.command == "check":
        for script in scripts:
            print(
                f"{script.name}: {len(script.steps)} steps for {script.board} "
                f"at {script.width}x{script.height}"
            )
        return 0
    if not args.preview:
        print(
            "--preview (or ICLFORGE_PANEL_PREVIEW) names the panel_preview binary", file=sys.stderr
        )
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    failed = 0
    for script in scripts:
        shadow = PreviewBackend([args.preview])
        backend: Backend = shadow
        try:
            if args.board:
                check_lease(args.lease or Path(), args.session)
                backend = BoardBackend(args.board, shadow, panel_port=args.panel_port)
            run(script, backend, args.out)
            print(f"PASS {script.name}")
        except StepError as error:
            failed += 1
            print(f"FAIL {error}")
        finally:
            if args.restore and isinstance(backend, BoardBackend):
                for note in backend.restore():
                    print(f"  left changed: {note}")
            shadow.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
