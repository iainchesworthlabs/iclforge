"""Unit tests for board_suite.py, the board suite (planning/esp32-platform-profiles.md).

stdlib unittest, as ci.yml's script-lint job runs it. The board is a stand-in: an HTTP server
that answers GET /hardware, GET /status, GET /log and POST /play with what the real boards
printed on 2026-10-12, so the suite's reading of a play is held to those lines.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import contextlib
import http.server
import io
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from typing import ClassVar

sys.path.insert(0, str(Path(__file__).resolve().parent))

import board_suite as bs
import sink_profile

SECRET = "TheHomeNetwork-5GHz"


class Behaviour:
    """What the stand-in does for a stream, by the file's name."""

    def __init__(self, **kwargs) -> None:
        self.units = kwargs.get("units", 938)
        self.underruns = kwargs.get("underruns", 0)
        self.stack_free = kwargs.get("stack_free", 18268)
        self.headroom = kwargs.get("headroom", 5)
        self.us_per_frame = kwargs.get("us_per_frame", 31449)
        self.render_us = kwargs.get("render_us", 300)
        self.sink_us = kwargs.get("sink_us", 24000)
        self.refuse = kwargs.get("refuse", False)
        self.reboot = kwargs.get("reboot", False)
        self.alloc_failures = kwargs.get("alloc_failures", 0)
        self.pcm_hash = kwargs.get("pcm_hash")
        self.decoder_permille = kwargs.get("decoder_permille", 480)
        self.resync = kwargs.get("resync", 0)
        self.verdict = kwargs.get("verdict", "pass")


class StandIn:
    def __init__(self, profile: str, behaviours: dict[str, Behaviour], **hardware) -> None:
        self.hardware = {
            "target": "esp32s3",
            "chip": "ESP32-S3",
            "version": "v0.10.0-beta.1-4176-g57282fb21-",
            "profile": profile,
            "profile_hash": hardware.pop("profile_hash", None)
            or (sink_profile.profile_hash(sink_profile.load(), profile) if profile else ""),
            **hardware,
        }
        self.behaviours = behaviours
        self.state = "stopped"
        self.location = ""
        self.frames = 0
        self.status_extra: dict = {}
        self.log = bytearray(b"boot line\nbudget: when=boot net=1 verdict=ok\n")
        self.plays: list[str] = []
        stand_in = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args, **kwargs) -> None:
                return

            def _send(self, code: int, body: bytes, headers: dict | None = None) -> None:
                self.send_response(code)
                for key, value in (headers or {}).items():
                    self.send_header(key, value)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self) -> None:
                path, _, query = self.path.partition("?")
                if path == "/hardware":
                    self._send(200, json.dumps(stand_in.hardware).encode())
                elif path == "/status":
                    body = {
                        "state": stand_in.state,
                        "location": stand_in.location,
                        "frames": stand_in.frames,
                        "finished": stand_in.state == "finished",
                        "failed": stand_in.state == "failed",
                        "network": {"kind": "wifi", "ssid": SECRET, "address": "192.168.1.9"},
                        **stand_in.status_extra,
                    }
                    self._send(200, json.dumps(body).encode())
                elif path == "/log":
                    start = int(query.partition("=")[2]) if query.startswith("from=") else 0
                    data = bytes(stand_in.log[start:]) if start <= len(stand_in.log) else b""
                    self._send(200, data, {"X-Log-Next": str(len(stand_in.log))})
                else:
                    self._send(404, b"")

            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length).decode()
                if self.path == "/play":
                    stand_in.play(body)
                    self._send(202, b"accepted\n")
                else:
                    stand_in.state = "stopped"
                    self._send(200, b"stopped\n")

        self.server = bs.FixtureServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def play(self, url: str) -> None:
        self.plays.append(url)
        name = url.split("?", maxsplit=1)[0].rsplit("/", 1)[-1]
        behaviour = self.behaviours[name]
        self.location = url
        if behaviour.reboot:
            self.log = bytearray(b"ESP-ROM:esp32s3\n")
        lines = []
        if behaviour.refuse:
            self.state = "failed"
            self.frames = 0
            self.status_extra = {"why": "memory", "error": 111112}
            lines.append(
                "player: a play here needs 134008 bytes of internal RAM free and 111112 are: "
                "refused"
            )
        else:
            self.state = "finished"
            self.frames = behaviour.units
            self.status_extra = {
                "why": "end of stream",
                "us_per_frame": behaviour.us_per_frame,
                "render_us_per_frame": behaviour.render_us,
                "sink_us_per_frame": behaviour.sink_us,
                "resync_bytes": behaviour.resync,
            }
            lines.append("heap: internal free 90931 (largest block 31744), psram free 6341784")
            for _ in range(behaviour.alloc_failures):
                lines.append("heap: heap_caps_malloc could not allocate 1682 bytes (caps 0x80c)")
            lines.append(
                f"sink.writes=5628 sink.underruns={behaviour.underruns} sink.dry_ms=0 "
                f"sink.min_headroom_ms={behaviour.headroom} sink.dma_ms=64"
            )
            lines.append(
                f"stream.units={behaviour.units} stream.held=0 "
                f"stream.resync_bytes={behaviour.resync} stream.sink=i2s "
                f"stream.decode_stack_free={behaviour.stack_free} stream.audio_ms=30016"
            )
            if behaviour.pcm_hash is not None:
                lines.append(
                    f"ac4.lap=1 frames=3 decoder_permille={behaviour.decoder_permille} "
                    f"worst_frame_us=1 pcm_hash={behaviour.pcm_hash}"
                )
            lines.append(f"result={behaviour.verdict}")
        self.log.extend(("\n".join(lines) + "\n").encode())

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


def standard(**overrides) -> dict[str, Behaviour]:
    return {
        "ffmpeg.ac3": Behaviour(**overrides),
        "ffmpeg.ec3": Behaviour(**overrides),
    }


class Suite(unittest.TestCase):
    """suite.json against the tree it describes."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.suite = bs.load_suite()
        cls.profiles = sink_profile.load()

    def test_every_stream_file_is_in_the_repository(self) -> None:
        for name, spec in self.suite["streams"].items():
            self.assertTrue((bs.REPO / spec["file"]).is_file(), f"{name}: {spec['file']}")

    def test_every_profile_has_a_list_and_no_list_is_for_a_profile_that_is_not(self) -> None:
        self.assertEqual(set(self.suite["profiles"]), set(self.profiles["profiles"]))

    def test_every_play_names_a_stream_and_ids_are_unique_in_a_profile(self) -> None:
        for profile, spec in self.suite["profiles"].items():
            ids = [row["id"] for row in spec["plays"]]
            self.assertEqual(len(ids), len(set(ids)), profile)
            for row in spec["plays"]:
                self.assertIn(row["stream"], self.suite["streams"], f"{profile}/{row['id']}")

    def test_every_ac4_pin_is_a_pin_the_host_holds(self) -> None:
        pins = json.loads(bs.PINS.read_text("utf-8"))["hashes"]
        for name, spec in self.suite["streams"].items():
            if spec["codec"] == "AC-4":
                self.assertIn(spec["pin"], pins, name)

    def test_a_profile_without_psram_http_plays_only_expects_a_refusal(self) -> None:
        for profile in ("c6-16mb", "c6-4mb"):
            for row in self.suite["profiles"][profile]["plays"]:
                self.assertEqual(row.get("expect"), "refused-memory", f"{profile}/{row['id']}")

    def test_plan_for_an_unknown_profile_is_refused(self) -> None:
        with self.assertRaises(bs.SuiteError):
            bs.plan(self.suite, "s3-n16r8-nothing")


class Booking(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.lock = Path(self.dir.name) / "board.lock"

    def tearDown(self) -> None:
        self.dir.cleanup()

    def test_a_lock_is_written_once_in_the_line_the_other_sessions_use(self) -> None:
        line = bs.take_lock(self.lock, "16d036", "baseline", 30)
        self.assertRegex(
            line,
            r"^session=16d036 \(board suite\) \| purpose=baseline \| "
            r"start=\d{4}-\d\d-\d\dT\d\d:\d\dZ \| expected_end=\d{4}-\d\d-\d\dT\d\d:\d\dZ$",
        )
        self.assertEqual(self.lock.read_text("utf-8").strip(), line)

    def test_a_booked_board_is_not_taken_over(self) -> None:
        self.lock.write_text("session=8996e5 (karaoke) | purpose=x\n", "utf-8")
        with self.assertRaisesRegex(bs.SuiteError, "is booked: session=8996e5"):
            bs.take_lock(self.lock, "16d036", "baseline", 30)
        self.assertIn("8996e5", self.lock.read_text("utf-8"))

    def test_a_lock_this_session_already_holds_is_used_and_not_taken(self) -> None:
        line = bs.take_lock(self.lock, "16d036", "put an image on it", 30)
        held, ours = bs.hold_lock(self.lock, "16d036", "suite", 30)
        self.assertEqual((held, ours), (line, False))
        self.lock.write_text("session=8996e5 (karaoke) | purpose=x\n", "utf-8")
        with self.assertRaises(bs.SuiteError):
            bs.hold_lock(self.lock, "16d036", "suite", 30)

    def test_only_the_session_that_took_it_releases_it(self) -> None:
        bs.take_lock(self.lock, "16d036", "baseline", 30)
        with self.assertRaisesRegex(bs.SuiteError, "not this session's"):
            bs.release_lock(self.lock, "8996e5")
        self.assertTrue(self.lock.exists())
        self.assertEqual(bs.release_lock(self.lock, "16d036"), "released")
        self.assertFalse(self.lock.exists())

    def test_a_session_id_that_is_a_prefix_of_another_does_not_release_it(self) -> None:
        self.lock.write_text("session=16d0366 (x) | purpose=y\n", "utf-8")
        with self.assertRaises(bs.SuiteError):
            bs.release_lock(self.lock, "16d036")


class Reading(unittest.TestCase):
    def test_a_plays_lines_become_its_facts(self) -> None:
        lines = [
            "heap: internal free 90931 (largest block 31744), psram free 6341784",
            "sink.writes=760 sink.underruns=1 sink.dry_ms=88 sink.min_headroom_ms=0 sink.dma_ms=64",
            "sink.writes=5628 sink.underruns=1 sink.dry_ms=88 sink.min_headroom_ms=0 "
            "sink.dma_ms=64",
            "stream.units=938 stream.held=0 stream.resync_bytes=0 stream.sink=i2s "
            "stream.decode_stack_free=18260 stream.audio_ms=30016",
            "result=pass",
        ]
        found = bs.parse_play_lines(lines)
        self.assertEqual(
            (found.verdict, found.units, found.underruns, found.dry_ms, found.min_headroom_ms),
            ("pass", 938, 1, 88, 0),
        )
        self.assertEqual((found.decode_stack_free, found.heap_largest), (18260, 31744))

    def test_the_decoders_time_leaves_out_the_sink_and_the_renderer(self) -> None:
        status = {
            "frames": 938,
            "us_per_frame": 31449,
            "render_us_per_frame": 300,
            "sink_us_per_frame": 24000,
        }
        self.assertEqual(bs.decoder_permille(status, 32000), 223)

    def test_realtime_permille_is_not_the_figure(self) -> None:
        """On an I2S sink every 2.0 play reads 981 to 992 because it includes the DAC's wait."""
        paced = {"frames": 938, "us_per_frame": 31449, "sink_us_per_frame": 0}
        self.assertEqual(bs.decoder_permille(paced, 32000), 983)
        fast = {"frames": 938, "us_per_frame": 31449, "sink_us_per_frame": 24000}
        self.assertLess(bs.decoder_permille(fast, 32000), 400)

    def test_no_frames_is_no_figure(self) -> None:
        self.assertIsNone(bs.decoder_permille({"frames": 0, "us_per_frame": 0}, 32000))

    def test_an_ac4_line_gives_its_own_hash_and_time(self) -> None:
        found = bs.parse_play_lines(
            ["ac4.lap=1 frames=3 decoder_permille=480 worst_frame_us=9 pcm_hash=f860e51f602ae753"]
        )
        self.assertEqual(
            (found.ac4_decoder_permille, found.ac4_pcm_hash), (480, "f860e51f602ae753")
        )


class Run(unittest.TestCase):
    """The suite against a stand-in board."""

    def setUp(self) -> None:
        self.suite = bs.load_suite()
        self.pins = json.loads(bs.PINS.read_text("utf-8"))["hashes"]
        self.opened: list[StandIn] = []

    def tearDown(self) -> None:
        for board in self.opened:
            board.close()

    def board(self, profile: str, behaviours: dict[str, Behaviour], **hardware) -> StandIn:
        stand_in = StandIn(profile, behaviours, **hardware)
        self.opened.append(stand_in)
        return stand_in

    def run_rows(self, stand_in: StandIn, profile: str, **kwargs):
        board = bs.Board("127.0.0.1", stand_in.port, 5.0)
        return bs.run_suite(
            board,
            profile,
            self.suite,
            "http://127.0.0.1:1",
            self.pins,
            sleep=0.01,
            timeout=5.0,
            **kwargs,
        )

    def test_a_clean_release_profile_passes_every_row(self) -> None:
        results = self.run_rows(self.board("s3-n16r8", standard()), "s3-n16r8")
        self.assertEqual([r.id for r in results], ["ac3-2.0", "eac3-2.0"])
        for r in results:
            self.assertEqual(r.findings, [], r.id)
            self.assertEqual(r.state, "finished")
            self.assertEqual(r.decoder_permille, 223)
            self.assertEqual(r.lines.decode_stack_free, 18268)

    def test_an_underrun_is_a_finding_with_its_figures(self) -> None:
        stand_in = self.board("s3-n16r8", {**standard(), "ffmpeg.ac3": Behaviour(underruns=1)})
        results = self.run_rows(stand_in, "s3-n16r8")
        self.assertEqual(results[1].findings, [])
        self.assertTrue(any("1 sink underrun" in f for f in results[0].findings))

    def test_a_play_that_is_not_every_unit_is_a_finding(self) -> None:
        stand_in = self.board("s3-n16r8", {**standard(), "ffmpeg.ec3": Behaviour(units=900)})
        results = self.run_rows(stand_in, "s3-n16r8")
        self.assertTrue(any("900 units, expected 938" in f for f in results[1].findings))

    def test_a_failed_allocation_in_a_play_is_a_finding(self) -> None:
        stand_in = self.board(
            "s3-n16r8", {**standard(), "ffmpeg.ac3": Behaviour(alloc_failures=129)}
        )
        results = self.run_rows(stand_in, "s3-n16r8")
        self.assertTrue(any("129 failed allocation" in f for f in results[0].findings))

    def test_a_reboot_in_a_play_is_a_finding(self) -> None:
        stand_in = self.board("s3-n16r8", {**standard(), "ffmpeg.ec3": Behaviour(reboot=True)})
        results = self.run_rows(stand_in, "s3-n16r8")
        self.assertTrue(results[1].rebooted)
        self.assertTrue(any("rebooted" in f for f in results[1].findings))

    def test_a_verdict_that_is_not_pass_is_a_finding(self) -> None:
        stand_in = self.board("s3-n16r8", {**standard(), "ffmpeg.ac3": Behaviour(verdict="fail")})
        results = self.run_rows(stand_in, "s3-n16r8")
        self.assertTrue(any("result=fail" in f for f in results[0].findings))

    def test_the_c6s_refusal_is_the_row_and_a_play_that_runs_is_the_finding(self) -> None:
        refusing = {name: Behaviour(refuse=True) for name in ("ffmpeg.ac3", "ffmpeg.ec3")}
        results = self.run_rows(self.board("c6-16mb", refusing), "c6-16mb")
        for r in results:
            self.assertEqual(r.findings, [], r.id)
            self.assertEqual((r.state, r.why), ("failed", "memory"))
        playing = self.board("c6-16mb", standard())
        results = self.run_rows(playing, "c6-16mb")
        self.assertTrue(any("expected a refusal for memory" in f for f in results[0].findings))

    def test_an_ac4_play_needs_the_hosts_pcm_hash(self) -> None:
        pin = self.pins["ac4_20_music"]
        good = {"ac4-20-music-192-3frames.ac4": Behaviour(units=3, pcm_hash=pin)}
        results = self.run_rows(
            self.board("s3-n16r8-ac4", {**standard(), **good}, **{}),
            "s3-n16r8-ac4",
            only=["ac4-20-music"],
        )
        self.assertEqual(results[0].findings, [])
        self.assertEqual(results[0].decoder_permille, 480)
        wrong = {"ac4-20-music-192-3frames.ac4": Behaviour(units=3, pcm_hash="0123456789abcdef")}
        results = self.run_rows(
            self.board("s3-n16r8-ac4", {**standard(), **wrong}),
            "s3-n16r8-ac4",
            only=["ac4-20-music"],
        )
        self.assertTrue(any("is not the host's" in f for f in results[0].findings))
        zero = {"ac4-20-music-192-3frames.ac4": Behaviour(units=3, pcm_hash="0" * 16)}
        results = self.run_rows(
            self.board("s3-n16r8-ac4", {**standard(), **zero}),
            "s3-n16r8-ac4",
            only=["ac4-20-music"],
        )
        self.assertTrue(any("AC4_PCM_HASH=1" in f for f in results[0].findings))

    def test_repeating_a_row_holds_its_median_and_its_worst(self) -> None:
        stand_in = self.board("s3-n16r8", standard())
        results = self.run_rows(stand_in, "s3-n16r8", repeat=3)
        self.assertEqual(len(stand_in.plays), 6)
        self.assertEqual(len(results), 2)
        self.assertEqual(len(set(stand_in.plays)), 6, "each play has a location of its own")


class Identity(unittest.TestCase):
    def setUp(self) -> None:
        self.opened: list[StandIn] = []

    def tearDown(self) -> None:
        for board in self.opened:
            board.close()

    def stand_in(self, profile: str, **hardware) -> StandIn:
        board = StandIn(profile, {}, **hardware)
        self.opened.append(board)
        return board

    def test_the_profile_at_its_current_definition_is_accepted(self) -> None:
        board = self.stand_in("s3-n16r8")
        hardware = bs.check_identity(bs.Board("127.0.0.1", board.port), "s3-n16r8")
        self.assertEqual(hardware["profile"], "s3-n16r8")

    def test_another_profile_is_refused(self) -> None:
        board = self.stand_in("c6-16mb")
        with self.assertRaisesRegex(bs.SuiteError, "says it is 'c6-16mb', not s3-n16r8"):
            bs.check_identity(bs.Board("127.0.0.1", board.port), "s3-n16r8")

    def test_a_scratch_build_with_no_profile_is_refused(self) -> None:
        board = self.stand_in("")
        with self.assertRaisesRegex(bs.SuiteError, "no profile"):
            bs.check_identity(bs.Board("127.0.0.1", board.port), "s3-n16r8")

    def test_an_older_definition_is_refused(self) -> None:
        board = self.stand_in("s3-n16r8", profile_hash="000000000000")
        with self.assertRaisesRegex(bs.SuiteError, "another one"):
            bs.check_identity(bs.Board("127.0.0.1", board.port), "s3-n16r8")


class Compare(unittest.TestCase):
    TOLERANCE: ClassVar[dict] = {
        "relative": 0.05,
        "absolute_permille": 20,
        "stack_free_bytes": 512,
        "headroom_ms": 2,
    }

    def figures(self, **kwargs) -> dict:
        play = {
            "state": "finished",
            "units": 938,
            "decoder_permille": 223,
            "decode_stack_free": 18268,
            "min_headroom_ms": 5,
            "underruns": 0,
        }
        play.update(kwargs)
        return {"profile": "s3-n16r8", "plays": {"ac3-2.0": play}}

    def test_inside_the_tolerance_is_clean(self) -> None:
        run = self.figures(decoder_permille=240, decode_stack_free=18000)
        self.assertEqual(bs.compare(run, self.figures(), self.TOLERANCE), ([], []))

    def test_a_slower_decoder_is_a_regression_by_the_larger_margin(self) -> None:
        # 223 + max(11.15, 20) = 243
        self.assertEqual(
            bs.compare(self.figures(decoder_permille=243), self.figures(), self.TOLERANCE)[0], []
        )
        slow = bs.compare(self.figures(decoder_permille=250), self.figures(), self.TOLERANCE)[0]
        self.assertEqual(len(slow), 1)
        self.assertIn("allowed to 243", slow[0])

    def test_a_faster_decoder_is_an_improvement_to_re_baseline(self) -> None:
        regressions, improvements = bs.compare(
            self.figures(decoder_permille=150), self.figures(), self.TOLERANCE
        )
        self.assertEqual(regressions, [])
        self.assertIn("re-baseline", improvements[0])

    def test_less_stack_or_headroom_than_the_slack_is_a_regression(self) -> None:
        run = self.figures(decode_stack_free=17000, min_headroom_ms=1)
        regressions, _ = bs.compare(run, self.figures(), self.TOLERANCE)
        self.assertEqual(len(regressions), 2)

    def test_an_underrun_or_a_different_unit_count_is_a_regression(self) -> None:
        run = self.figures(underruns=1, units=900)
        regressions, _ = bs.compare(run, self.figures(), self.TOLERANCE)
        self.assertEqual(len(regressions), 2)

    def test_a_missing_play_or_another_profile_is_a_regression(self) -> None:
        run = {"profile": "c6-16mb", "plays": {}}
        regressions, _ = bs.compare(run, self.figures(), self.TOLERANCE)
        self.assertEqual(len(regressions), 2)


class CommandLine(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.lock = self.root / "board.lock"
        self.out = self.root / "run.json"
        self.stand_in = StandIn("s3-n16r8", standard())
        self.saved = bs.BASELINES
        bs.BASELINES = self.root / "baselines"

    def tearDown(self) -> None:
        bs.BASELINES = self.saved
        self.stand_in.close()
        self.dir.cleanup()

    def run_cli(self, *argv: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bs.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def run_args(self, *extra: str) -> list[str]:
        return [
            "run",
            "--profile",
            "s3-n16r8",
            "--host",
            "127.0.0.1",
            "--port",
            str(self.stand_in.port),
            "--serve-host",
            "127.0.0.1",
            "--out",
            str(self.out),
            "--play-timeout",
            "10",
            *extra,
        ]

    def test_a_run_books_plays_writes_the_run_and_releases(self) -> None:
        code, out, err = self.run_cli(
            *self.run_args("--lock", str(self.lock), "--session", "16d036")
        )
        self.assertEqual((code, err), (0, ""), out)
        self.assertFalse(self.lock.exists(), "the lock is released")
        record = json.loads(self.out.read_text("utf-8"))
        self.assertEqual(record["profile"], "s3-n16r8")
        self.assertEqual(set(record["plays"]), {"ac3-2.0", "eac3-2.0"})
        self.assertEqual(record["plays"]["ac3-2.0"]["decoder_permille"], 223)

    def test_nothing_the_board_says_about_its_network_is_kept_or_printed(self) -> None:
        code, out, err = self.run_cli(
            *self.run_args("--lock", str(self.lock), "--session", "16d036")
        )
        self.assertEqual(code, 0)
        self.assertNotIn(SECRET, out + err + self.out.read_text("utf-8"))
        self.assertNotIn("192.168.1.9", self.out.read_text("utf-8"))

    def test_without_a_lock_and_a_session_it_refuses_before_touching_the_board(self) -> None:
        code, _, err = self.run_cli(*self.run_args())
        self.assertEqual(code, 2)
        self.assertIn("--lock and --session are required", err)
        self.assertEqual(self.stand_in.plays, [])

    def test_a_run_on_a_board_this_session_booked_leaves_it_booked(self) -> None:
        bs.take_lock(self.lock, "16d036", "put an image on it", 30)
        code, out, err = self.run_cli(
            *self.run_args("--lock", str(self.lock), "--session", "16d036")
        )
        self.assertEqual((code, err), (0, ""), out)
        self.assertIn("already booked by this session", out)
        self.assertIn("left booked", out)
        self.assertTrue(self.lock.exists())

    def test_a_booked_board_is_refused_and_the_other_session_keeps_its_lock(self) -> None:
        self.lock.write_text("session=8996e5 (karaoke) | purpose=x\n", "utf-8")
        code, _, err = self.run_cli(*self.run_args("--lock", str(self.lock), "--session", "16d036"))
        self.assertEqual(code, 2)
        self.assertIn("is booked", err)
        self.assertIn("8996e5", self.lock.read_text("utf-8"))
        self.assertEqual(self.stand_in.plays, [])

    def test_a_board_that_is_not_stopped_is_refused_and_the_lock_is_released(self) -> None:
        self.stand_in.state = "playing"
        code, _, err = self.run_cli(*self.run_args("--lock", str(self.lock), "--session", "16d036"))
        self.assertEqual(code, 2)
        self.assertIn("not 'stopped'", err)
        self.assertFalse(self.lock.exists())
        self.assertEqual(self.stand_in.plays, [])

    def test_a_board_of_another_profile_is_refused(self) -> None:
        other = StandIn("p4-rev1", standard())
        try:
            args = self.run_args("--lock", str(self.lock), "--session", "16d036")
            args[args.index("--port") + 1] = str(other.port)
            code, _, err = self.run_cli(*args)
        finally:
            other.close()
        self.assertEqual(code, 2)
        self.assertIn("not s3-n16r8", err)
        self.assertFalse(self.lock.exists())

    def test_record_writes_the_baseline_and_compare_holds_a_run_to_it(self) -> None:
        code, _, _ = self.run_cli(
            *self.run_args("--lock", str(self.lock), "--session", "16d036", "--record")
        )
        self.assertEqual(code, 0)
        baseline = bs.BASELINES / "s3-n16r8.json"
        self.assertTrue(baseline.is_file())
        code, out, _ = self.run_cli(
            *self.run_args(
                "--lock", str(self.lock), "--session", "16d036", "--compare", str(baseline)
            )
        )
        self.assertEqual(code, 0, out)
        # A slower decoder fails the comparison.
        self.stand_in.behaviours = {
            name: Behaviour(sink_us=10000) for name in ("ffmpeg.ac3", "ffmpeg.ec3")
        }
        code, out, _ = self.run_cli(
            *self.run_args(
                "--lock", str(self.lock), "--session", "16d036", "--compare", str(baseline)
            )
        )
        self.assertEqual(code, 1)
        self.assertIn("regression:", out)

    def test_a_run_with_findings_is_not_recorded(self) -> None:
        self.stand_in.behaviours = {
            name: Behaviour(underruns=2) for name in ("ffmpeg.ac3", "ffmpeg.ec3")
        }
        code, out, _ = self.run_cli(
            *self.run_args("--lock", str(self.lock), "--session", "16d036", "--record")
        )
        self.assertEqual(code, 1)
        self.assertIn("not recorded", out)
        self.assertFalse((bs.BASELINES / "s3-n16r8.json").exists())

    def test_plan_prints_the_rows(self) -> None:
        code, out, _ = self.run_cli("plan", "--profile", "c6-16mb")
        self.assertEqual(code, 0)
        self.assertIn("refused-memory", out)

    def test_lock_commands_take_show_and_release(self) -> None:
        code, out, _ = self.run_cli(
            "lock", "take", "--file", str(self.lock), "--session", "16d036", "--purpose", "x"
        )
        self.assertEqual(code, 0)
        code, out, _ = self.run_cli("lock", "show", "--file", str(self.lock))
        self.assertIn("session=16d036", out)
        code, out, _ = self.run_cli(
            "lock", "release", "--file", str(self.lock), "--session", "8996e5"
        )
        self.assertEqual(code, 2)
        code, out, _ = self.run_cli(
            "lock", "release", "--file", str(self.lock), "--session", "16d036"
        )
        self.assertEqual((code, out.strip()), (0, "released"))


if __name__ == "__main__":
    unittest.main()
