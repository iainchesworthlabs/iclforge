"""Unit tests for screen_script.py, the screen scripts (planning/screen-test-harness.md).

stdlib unittest, as _static.yml's "Oracle unit tests" step runs it. The preview is a stand-in (a
Python program that speaks the `--drive` lines) and the board is a stand-in (two HTTP servers, the
control surface's and the panel's); the scripts of firmware/hearth-sink/preview/scripts are held to
the grammar and, when ICLFORGE_PANEL_PREVIEW names a built panel_preview, run on it.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import http.server
import io
import json
import os
import struct
import sys
import tempfile
import textwrap
import threading
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import Any, ClassVar

sys.path.insert(0, str(Path(__file__).resolve().parent))

import screen_script as ss

GOOD = textwrap.dedent(
    """\
    # a comment
    board p4-rev3-kit
    display 1024 600
    scene lyrics
    tap "Preferences"
    tap "+" in "Brightness"   # a row
    expect pref brightness = 90
    expect shown "90%"
    expect not-shown "Gone"
    expect text "9"
    swipe 6 500 100
    tap-at 10 20
    wait 250
    shot after
    """
)


def listing(brightness: str = "80", layout: str = "2.0") -> dict[str, Any]:
    """What GET /preferences says, with the fields the runner reads."""
    return {
        "profile": "p4-rev3",
        "display": "dsi",
        "preferences": [
            {"id": "brightness", "kind": "stepper", "route": "/brightness", "value": brightness},
            {"id": "speaker-layout", "kind": "choice", "route": "/layout", "value": layout},
            {"id": "time-zone", "kind": "sheet", "route": "/timezone", "value": "UTC"},
        ],
    }


class FakeBackend:
    """A screen that holds a brightness and steps it by 10 on a tap of + or -."""

    def __init__(self) -> None:
        self.brightness = 80
        self.text = {"Brightness", "80%"}
        self.acted: list[str] = []
        self.events: list[list[Any]] = []

    def setup(self, script: ss.Script) -> None:
        self.acted.append("setup " + script.board)

    def act(self, step: ss.Step) -> None:
        self.acted.append(step.source)
        self.events = [["tap", 1, 2]]
        if step.verb == "tap":
            if step.args[0] not in ("+", "-", "Preferences"):
                raise ss.StepError(f"no button {step.args[0]!r} is shown")
            self.brightness += {"+": 10, "-": -10}.get(step.args[0], 0)
            self.text = {"Brightness", f"{self.brightness}%"}

    def prefs(self) -> dict[str, Any]:
        return listing(str(self.brightness))

    def shown(self, text: str, contains: bool) -> bool:
        return any(text in t if contains else text == t for t in self.text)

    def shot(self, name: str, out: Path) -> None:
        self.acted.append("shot " + name)

    def close(self) -> None:
        pass


class ParseTests(unittest.TestCase):
    def test_a_script_is_its_header_and_its_steps(self) -> None:
        script = ss.parse(GOOD, "good")
        self.assertEqual(
            (script.board, script.width, script.height, script.scene),
            ("p4-rev3-kit", 1024, 600, "lyrics"),
        )
        verbs = [s.verb for s in script.steps]
        self.assertEqual(
            verbs,
            [
                "tap",
                "tap",
                "expect",
                "expect",
                "expect",
                "expect",
                "swipe",
                "tap-at",
                "wait",
                "shot",
            ],
        )
        self.assertEqual(script.steps[1].args, ("+", "Brightness"))
        self.assertEqual(script.steps[2].args, ("pref", "brightness", "90"))
        self.assertEqual(script.steps[1].line, 6)

    def test_a_script_with_no_header_is_the_kit_at_its_own_size(self) -> None:
        script = ss.parse('tap "Preferences"\n')
        self.assertEqual((script.board, script.width, script.height), ("p4-rev3-kit", 1024, 600))

    def test_each_mistake_says_which_line(self) -> None:
        cases = {
            'tap "a" in\n': "1:",
            "tap\n": "1:",
            "tap-at 1\n": "1:",
            "tap-at one 2\n": "not a whole number",
            "wait soon\n": "not a whole number",
            "expect pref brightness 90\n": "1:",
            "expect pref brightness =\n": "1:",
            "expect seen X\n": "1:",
            "double tap\n": "not a step",
            'tap "open\n': "1:",
            "board\n": "wrong arguments",
            "scene attic\n": "wrong arguments",
            'tap "a"\nboard p4-rev3-kit\n': "2: board comes after a step",
        }
        for text, expected in cases.items():
            with self.subTest(text=text), self.assertRaises(ss.ScriptError) as caught:
                ss.parse(text, "bad")
            self.assertIn("bad:", str(caught.exception))
            self.assertIn(expected, str(caught.exception))

    def test_a_label_can_hold_quotes_and_a_hash(self) -> None:
        script = ss.parse('tap "Say \\"hi\\" # now"\n')
        self.assertEqual(script.steps[0].args, ('Say "hi" # now',))


class RunTests(unittest.TestCase):
    def test_a_script_that_holds_passes(self) -> None:
        backend = FakeBackend()
        ss.run(
            ss.parse(GOOD.replace("expect text", "expect not-shown"), "good"), backend, Path(".")
        )
        self.assertEqual(backend.acted[0], "setup p4-rev3-kit")
        self.assertEqual(backend.acted[-1], "shot after")

    def test_the_first_failure_stops_the_run_and_names_its_line(self) -> None:
        backend = FakeBackend()
        script = ss.parse('tap "+"\nexpect pref brightness = 70\ntap "-"\n', "stops")
        with self.assertRaises(ss.StepError) as caught:
            ss.run(script, backend, Path("."))
        self.assertIn("stops:2: expect pref brightness = 70", str(caught.exception))
        self.assertIn("'90', not '70'", str(caught.exception))
        self.assertEqual(backend.acted, ["setup p4-rev3-kit", 'tap "+"'])

    def test_a_missing_button_fails_the_step_that_looked_for_it(self) -> None:
        with self.assertRaises(ss.StepError) as caught:
            ss.run(ss.parse('tap "Nowhere"\n', "x"), FakeBackend(), Path("."))
        self.assertIn("x:1:", str(caught.exception))

    def test_text_that_is_on_the_screen_and_should_not_be_fails(self) -> None:
        with self.assertRaises(ss.StepError):
            ss.run(ss.parse('expect not-shown "80%"\n', "x"), FakeBackend(), Path("."))
        with self.assertRaises(ss.StepError):
            ss.run(ss.parse('expect shown "8"\n', "x"), FakeBackend(), Path("."))
        ss.run(ss.parse('expect text "8"\nexpect no-text "zzz"\n', "x"), FakeBackend(), Path("."))

    def test_a_preference_that_does_not_exist_is_not_a_pass(self) -> None:
        with self.assertRaises(ss.StepError) as caught:
            ss.run(ss.parse("expect pref volume = 5\n", "x"), FakeBackend(), Path("."))
        self.assertIn("no preference is called 'volume'", str(caught.exception))


FAKE_DRIVER = """
import json, shlex, sys
state = {"brightness": 80}
for line in sys.stdin:
    words = shlex.split(line)
    verb = words[0]
    if verb == "quit":
        break
    if verb == "tap" and words[1] == "+":
        state["brightness"] += 10
        out = {"ok": True, "events": [["swipe", 6, 500, 100], ["tap", 11, 22]]}
    elif verb == "tap":
        out = {"ok": False, "error": "no button " + words[1] + " is shown", "events": []}
    elif verb == "prefs":
        out = {"ok": True, "events": [], "prefs": {"profile": "p4-rev3", "preferences": [
            {"id": "brightness", "kind": "stepper", "route": "/brightness",
             "value": str(state["brightness"])}]}}
    elif verb == "shown":
        out = {"ok": True, "events": [], "value": words[1] == 'Say "hi"'}
    else:
        out = {"ok": True, "events": []}
    print(json.dumps(out), flush=True)
"""


class PreviewBackendTests(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        path = Path(self.dir.name) / "driver.py"
        path.write_text(FAKE_DRIVER, encoding="utf-8")
        self.backend = ss.PreviewBackend([sys.executable, str(path)], timeout=20)
        self.addCleanup(self.backend.close)

    def test_a_step_is_a_line_out_and_a_line_of_json_back(self) -> None:
        script = ss.parse('tap "+" in "Brightness"\nexpect pref brightness = 90\n', "p")
        ss.run(script, self.backend, Path(self.dir.name))
        self.assertEqual(
            self.backend.events, []
        )  # the events of the last command, the read of the prefs

    def test_the_touches_a_step_made_come_back(self) -> None:
        self.backend.setup(ss.parse(""))
        self.backend.act(ss.parse('tap "+"\n').steps[0])
        self.assertEqual(self.backend.events, [["swipe", 6, 500, 100], ["tap", 11, 22]])

    def test_a_refusal_is_a_step_error_with_the_previews_words(self) -> None:
        with self.assertRaises(ss.StepError) as caught:
            ss.run(ss.parse('tap "Nope"\n', "p"), self.backend, Path(self.dir.name))
        self.assertIn("no button Nope is shown", str(caught.exception))

    def test_quotes_survive_the_trip(self) -> None:
        self.assertTrue(self.backend.shown('Say "hi"', False))


class Board:
    """A board's two HTTP servers: the control surface (GET /preferences, PUT) and :8081 (panel)."""

    def __init__(
        self,
        brightness: str = "80",
        glass: tuple[int, int] = (1024, 600),
        follows: bool = True,
        dimmed: bool = False,
    ) -> None:
        self.brightness, self.glass, self.follows, self.dimmed = brightness, glass, follows, dimmed
        self.taps: list[str] = []
        self.puts: list[tuple[str, str]] = []
        board = self

        class Main(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args: Any) -> None:
                pass

            def do_GET(self) -> None:
                body = json.dumps(listing(board.brightness)).encode()
                self.send_response(200)
                self.end_headers()
                self.wfile.write(body)

            def do_PUT(self) -> None:
                body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
                board.puts.append((self.path, body))
                if self.path == "/brightness":
                    board.brightness = body
                self.send_response(200)
                self.end_headers()

        class Panel(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args: Any) -> None:
                pass

            def do_GET(self) -> None:
                if self.path == "/panel":
                    state = {"dimmed": board.dimmed, "prefs_view": False, "dev_view": False}
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(json.dumps(state).encode())
                    return
                head = bytearray(54)
                head[:2] = b"BM"
                struct.pack_into("<ii", head, 18, *board.glass)
                self.send_response(200)
                self.end_headers()
                self.wfile.write(bytes(head))

            def do_POST(self) -> None:
                board.taps.append(self.path)
                if board.follows:
                    board.brightness = str(int(board.brightness) + 10)
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"tapped\n")

        self.servers = [http.server.ThreadingHTTPServer(("127.0.0.1", 0), h) for h in (Main, Panel)]
        for server in self.servers:
            threading.Thread(target=server.serve_forever, daemon=True).start()

    @property
    def ports(self) -> tuple[int, int]:
        return self.servers[0].server_address[1], self.servers[1].server_address[1]

    def stop(self) -> None:
        for server in self.servers:
            server.shutdown()
            server.server_close()


class StandInPreview(FakeBackend):
    """The preview a board is in step with: a tap of + puts a touch at (11, 22)."""

    def act(self, step: ss.Step) -> None:
        super().act(step)
        self.events = [["tap", 11, 22]] if step.args[:1] == ("+",) else []

    def setup(self, script: ss.Script) -> None:
        super().setup(script)


class BoardBackendTests(unittest.TestCase):
    def board(self, **kwargs: Any) -> tuple[Board, ss.BoardBackend]:
        board = Board(**kwargs)
        self.addCleanup(board.stop)
        main, panel = board.ports
        return board, ss.BoardBackend(
            "127.0.0.1", StandInPreview(), port=main, panel_port=panel, settle=0
        )

    def test_the_preview_decides_where_the_finger_goes_and_the_board_is_touched_there(self) -> None:
        board, backend = self.board()
        ss.run(ss.parse('tap "+"\nexpect pref brightness = 90\n', "b"), backend, Path("."))
        self.assertEqual(board.taps, ["/panel/tap?x=11&y=22"])

    def test_a_board_that_does_not_do_what_the_preview_did_fails_the_step(self) -> None:
        _, backend = self.board(follows=False)
        with self.assertRaises(ss.StepError) as caught:
            ss.run(ss.parse('tap "+"\n', "b"), backend, Path("."))
        self.assertIn("brightness is '80' on the board, '90' on the preview", str(caught.exception))

    def test_a_pref_is_read_from_the_board_not_the_preview(self) -> None:
        board, backend = self.board()
        backend.shadow.brightness = 50  # type: ignore[attr-defined]
        with self.assertRaises(ss.StepError):
            ss.run(ss.parse("expect pref brightness = 50\n", "b"), backend, Path("."))
        self.assertEqual(board.taps, [])

    def test_a_glass_of_another_size_is_refused_before_a_touch(self) -> None:
        board, backend = self.board(glass=(800, 480))
        with self.assertRaises(ss.StepError) as caught:
            ss.run(ss.parse('tap "+"\n', "b"), backend, Path("."))
        self.assertIn("glass", str(caught.exception))
        self.assertEqual(board.taps, [])

    def test_a_dark_screen_is_not_touched_for_it_would_only_wake(self) -> None:
        board, backend = self.board(dimmed=True)
        with self.assertRaises(ss.StepError) as caught:
            ss.run(ss.parse('tap "+"\n', "b"), backend, Path("."))
        self.assertIn("dimmed", str(caught.exception))
        self.assertEqual(board.taps, [])

    def test_a_swipe_is_not_made_up_for_a_route_the_board_lacks(self) -> None:
        _, backend = self.board()
        backend.shadow.act = lambda step: setattr(
            backend.shadow, "events", [["swipe", 6, 500, 100]]
        )  # type: ignore[method-assign]
        with self.assertRaises(ss.StepError) as caught:
            ss.run(ss.parse('tap "+"\n', "b"), backend, Path("."))
        self.assertIn("no swipe route", str(caught.exception))

    def test_restore_puts_back_what_a_route_can_and_names_what_it_cannot(self) -> None:
        board, backend = self.board()
        backend.setup(ss.parse(""))
        board.brightness = "100"
        self.assertEqual(backend.restore(), [])
        self.assertEqual(board.puts, [("/brightness", "80")])
        self.assertEqual(board.brightness, "80")

    def test_a_picture_from_each_is_kept(self) -> None:
        _, backend = self.board()
        with tempfile.TemporaryDirectory() as out:
            backend.shot("one", Path(out))
            self.assertTrue((Path(out) / "one.board.bmp").exists())


class LeaseTests(unittest.TestCase):
    def test_a_board_is_written_to_only_under_the_sessions_own_lease(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / "board-kit.lock"
            with self.assertRaises(ss.StepError):
                ss.check_lease(lock, "abc123")
            lock.write_text(
                "session=screen-tap-abc123 (a session)\naction=settings\n", encoding="utf-8"
            )
            ss.check_lease(lock, "abc123")
            with self.assertRaises(ss.StepError):
                ss.check_lease(lock, "other")
            with self.assertRaises(ss.StepError):
                ss.check_lease(lock, "")


class RepoScriptTests(unittest.TestCase):
    scripts: ClassVar[list[Path]] = sorted(ss.SCRIPTS.glob("*.screen"))

    def test_there_are_scripts_and_every_one_is_a_script(self) -> None:
        self.assertGreaterEqual(len(self.scripts), 3)
        for path in self.scripts:
            with self.subTest(script=path.name):
                self.assertGreater(len(ss.load(path).steps), 3)

    def test_the_scripts_name_preferences_the_registry_has(self) -> None:
        registry = (
            ss.SCRIPTS.parents[3] / "libs" / "device" / "include" / "iclforge" / "preferences.hpp"
        ).read_text(encoding="utf-8")
        for path in self.scripts:
            for step in ss.load(path).steps:
                if step.verb == "expect" and step.args[0] == "pref":
                    with self.subTest(script=path.name, line=step.line):
                        self.assertIn(f'.id = "{step.args[1]}"', registry)

    def test_check_says_what_it_read(self) -> None:
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(io.StringIO()):
            self.assertEqual(ss.main(["check"]), 0)
        self.assertIn("prefs-brightness.screen", out.getvalue())

    @unittest.skipUnless(
        os.environ.get("ICLFORGE_PANEL_PREVIEW"),
        "ICLFORGE_PANEL_PREVIEW names no built panel_preview",
    )
    def test_every_script_holds_on_the_real_screen_code(self) -> None:
        for path in self.scripts:
            with self.subTest(script=path.name), tempfile.TemporaryDirectory() as out:
                backend = ss.PreviewBackend([os.environ["ICLFORGE_PANEL_PREVIEW"]])
                try:
                    ss.run(ss.load(path), backend, Path(out))
                finally:
                    backend.close()


if __name__ == "__main__":
    unittest.main()
