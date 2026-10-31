"""Unit tests for board_suite.py, the board suite (planning/esp32-platform-profiles.md).

stdlib unittest, as ci.yml's script-lint job runs it. The board is a stand-in: an HTTP server
that answers GET /hardware, GET /status, GET /log and POST /play with what the real boards
printed on 2026-10-12, so the suite's reading of a play is held to those lines.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import argparse
import contextlib
import http.server
import io
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.request
from pathlib import Path
from typing import ClassVar
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench
import board_suite as bs
import sink_profile
import suite_sendspin as ss
from test_bench import temp_bench

NETWORK_NAME = "TheHomeNetwork-5GHz"


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
        # the slots the sink opened for the play; None is what the layout needs (SLOTS)
        self.slots = kwargs.get("slots")


# The slots a layout needs, for the layouts the rows name (planning/karaoke-p4x-panel.md): a name is
# its ring, its heights and its LFE feeds, and 9.1.6 is the sixteen a P4X's TDM frame carries.
SLOTS = {"2.0": 2, "5.1": 6, "9.1.6": 16}


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
        self.layout = "2.0"
        self.layout_puts: list[str] = []  # every PUT /layout, in order
        self.refused_layouts: set[str] = set()  # the layouts PUT /layout answers 409 to
        # the board's words for each: iclforge/layout_options.hpp's sentence, which is also GET
        # /layout-options's reason for it
        self.refusal_reasons: dict[str, str] = {}
        # GET /layout-options's body; None is a firmware without the route (a 404)
        self.layout_options: dict | None = None
        # what PUT /layout says after the ok for a layout it takes with a limit on it
        self.layout_notes: dict[str, str] = {}
        self.restarts = 0
        self.log = bytearray(
            b"boot line\n"
            b"budget: when=boot net=1 internal_free=86231 largest=31744 ask_ring=0 "
            b"ask_decode_stack=0 ask_fetch_stack=8192 ask_total=8192 ask_biggest=8192 "
            b"headroom=23552 floor=0 verdict=ok\n"
        )
        self.boot_log = bytes(self.log)
        self.boot_logs: list[bytes] | None = None  # one per restart, in turn
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
                        "layout": stand_in.layout,
                        "network": {"kind": "wifi", "ssid": NETWORK_NAME, "address": "192.168.1.9"},
                        **stand_in.status_extra,
                    }
                    self._send(200, json.dumps(body).encode())
                elif path == "/log":
                    start = int(query.partition("=")[2]) if query.startswith("from=") else 0
                    # a reply is at most 4 KB, as the board's is (kLogReplyBytes)
                    data = (
                        bytes(stand_in.log[start : start + 4096])
                        if start <= len(stand_in.log)
                        else b""
                    )
                    nxt = start + len(data) if start <= len(stand_in.log) else len(stand_in.log)
                    self._send(200, data, {"X-Log-Next": str(nxt)})
                elif path == "/layout":
                    self._send(200, stand_in.layout.encode())
                elif path == "/layout-options" and stand_in.layout_options is not None:
                    self._send(200, json.dumps(stand_in.layout_options).encode())
                else:
                    self._send(404, b"")

            def do_PUT(self) -> None:
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length).decode().strip()
                if self.path != "/layout":
                    self._send(404, b"")
                    return
                stand_in.layout_puts.append(body)
                if body in stand_in.refused_layouts:
                    reason = stand_in.refusal_reasons.get(body, "more slots than this sink has")
                    self._send(409, (reason + "\n").encode())
                    return
                stand_in.layout = body
                note = stand_in.layout_notes.get(body)
                said = "ok; takes effect at the next play" + (f". Note: {note}" if note else "")
                self._send(200, (said + "\n").encode())

            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length).decode()
                if self.path == "/play":
                    stand_in.play(body)
                    self._send(202, b"accepted\n")
                elif self.path == "/restart":
                    stand_in.restarts += 1
                    stand_in.state = "stopped"
                    cycle = stand_in.boot_logs
                    boot = cycle[(stand_in.restarts - 1) % len(cycle)] if cycle else None
                    stand_in.log = bytearray(boot or stand_in.boot_log)
                    self._send(200, b"restarting\n")
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
                "stream": {
                    "codec": "AC-3",
                    "slots": behaviour.slots or SLOTS.get(self.layout, 2),
                    "layout": self.layout,
                },
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
                f"stream.decode_stack_free={behaviour.stack_free} stream.audio_ms=30016 "
                "stream.wall_ms=30130"
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


def budget_log(free: int, largest: int, verdict: str = "ok", floor: int = 0) -> bytes:
    return (
        f"boot line\nbudget: when=boot net=1 internal_free={free} largest={largest} ask_ring=0 "
        "ask_decode_stack=0 ask_fetch_stack=8192 ask_total=8192 ask_biggest=8192 "
        f"headroom={largest - 8192} floor={floor} verdict={verdict}\n"
    ).encode()


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
        # Its HTTP rows: a row played over Sendspin is the board's product path, and plays.
        for profile in ("c6-16mb", "c6-4mb"):
            for row in self.suite["profiles"][profile]["plays"]:
                if row.get("via") == "sendspin":
                    self.assertIsNone(row.get("expect"), f"{profile}/{row['id']}")
                    continue
                self.assertEqual(row.get("expect"), "refused-memory", f"{profile}/{row['id']}")

    def test_a_row_is_played_over_http_or_over_sendspin_and_nothing_else(self) -> None:
        for profile, spec in self.suite["profiles"].items():
            for row in spec["plays"]:
                self.assertIn(
                    row.get("via", "play"), ("play", "sendspin"), f"{profile}/{row['id']}"
                )

    def test_a_row_on_a_layout_says_how_many_slots_it_opens_for(self) -> None:
        for profile, spec in self.suite["profiles"].items():
            for row in spec["plays"]:
                if row.get("layout"):
                    self.assertIsInstance(row.get("slots"), int, f"{profile}/{row['id']}")
                    self.assertNotIn("via", row, f"{profile}/{row['id']}: a layout is a /play's")

    def test_a_row_on_a_layout_is_played_where_the_board_carries_it_and_refused_where_it_does_not(
        self,
    ) -> None:
        # The suite asks the board (GET /layout-options) and does not guess from the profile: a
        # chip below v3.0, which has no I2S clock for a frame past two channels, is the board's
        # own answer, not a reason to keep the row out of its list. What a row is where the board
        # cannot carry its layout is the row's `if_unavailable` (the default is the refusal).
        for profile, spec in self.suite["profiles"].items():
            for row in spec["plays"]:
                self.assertIn(
                    row.get("if_unavailable", "refuse"),
                    bs.UNAVAILABLE_POLICIES,
                    f"{profile}/{row['id']}",
                )
                if row.get("if_unavailable"):
                    self.assertTrue(row.get("layout"), f"{profile}/{row['id']}")

    def test_a_sendspin_row_s_stream_says_how_many_bursts_it_is_or_how_long_it_loops(self) -> None:
        # The bursts the board accounted for are held to it, as a /play's units are: a file
        # states its count, and a short one that the server plays over and over says for how
        # long (the server's report then says how many it sent) and what its burst period is.
        for profile, spec in self.suite["profiles"].items():
            for row in spec["plays"]:
                if row.get("via") != "sendspin":
                    continue
                stream = self.suite["streams"][row["stream"]]
                where = f"{profile}/{row['id']}"
                if stream.get("sendspin_seconds"):
                    self.assertIsInstance(stream["sendspin_seconds"], int, where)
                    self.assertIsInstance(stream.get("period_us"), int, where)
                else:
                    self.assertIsInstance(
                        stream.get("sendspin_units", stream.get("units")), int, where
                    )

    def test_a_committed_baseline_is_for_a_profile_and_holds_exactly_its_rows(self) -> None:
        for path in sorted(bs.BASELINES.glob("*.json")):
            record = json.loads(path.read_text("utf-8"))
            profile = record["profile"]
            self.assertEqual(path.stem, profile, path.name)
            self.assertIn(profile, self.suite["profiles"], path.name)
            rows = {row["id"]: row for row in self.suite["profiles"][profile]["plays"]}
            self.assertEqual(set(record["plays"]), set(rows), path.name)
            self.assertEqual(record["findings"], {}, path.name)
            self.assertRegex(record["profile_hash"], r"^[0-9a-f]{12}$", path.name)
            self.assertTrue(record["version"], path.name)
            for row_id, figures in record["plays"].items():
                if rows[row_id].get("expect") is None:
                    self.assertEqual(figures["state"], "finished", f"{path.name}/{row_id}")
                    if figures.get("via") == "sendspin":
                        # over Sendspin a play may glitch; the baseline says how many did
                        self.assertLessEqual(
                            figures["glitched_plays"], figures["plays"], f"{path.name}/{row_id}"
                        )
                    else:
                        self.assertEqual(figures["underruns"], 0, f"{path.name}/{row_id}")
            # --record refuses a run whose log holds no boot line, so a baseline has the figures
            # the compare's boot ratchet reads.
            for key in ("internal_free", "largest"):
                self.assertIsInstance(record["boot"].get(key), int, f"{path.name}: boot {key}")

    def test_a_baseline_is_under_the_profiles_hash_or_the_pr_that_moved_it_accepts_it(self) -> None:
        """A baseline carries the hash of the profile it was recorded under, and the hash moves
        whenever the profile is edited. A PR that edits a profile has to *decide* about every
        baseline it moves, here and not on `main`: a locked value that changed carries a re-record,
        and a key added at the value it already had keeps the baseline and says so, against the
        profile's new hash, in suite.json (`baseline_accepted`). The acceptance names the hash it
        was made against, so the next edit of the profile asks again."""
        data = sink_profile.load()
        for path in sorted(bs.BASELINES.glob("*.json")):
            record = json.loads(path.read_text("utf-8"))
            profile = record["profile"]
            current = sink_profile.profile_hash(data, profile)
            accepted = self.suite["profiles"][profile].get("baseline_accepted")
            if record["profile_hash"] == current:
                self.assertIsNone(
                    accepted,
                    f"{profile}: baseline_accepted is dead: the baseline is under the profile's "
                    "hash, so the acceptance is removed",
                )
                continue
            how = (
                f"{profile}: the profile is {current} and its baseline was recorded under "
                f"{record['profile_hash']}. A locked value that changed carries its re-record "
                "(board_suite.py run --record); a key added at the value it already had keeps the "
                f"baseline: set profiles.{profile}.baseline_accepted in suite.json to "
                f'{{"profile_hash": "{current}", "why": "..."}} '
                "(planning/esp32-platform-profiles.md, 'Changing a profile')"
            )
            self.assertIsNotNone(accepted, how)
            self.assertEqual(accepted.get("profile_hash"), current, how)
            self.assertTrue(str(accepted.get("why", "")).strip(), how)

    def test_a_pin_layout_belongs_to_a_stream_that_has_a_pin(self) -> None:
        for name, spec in self.suite["streams"].items():
            if "pin_layout" in spec:
                self.assertIn("pin", spec, name)
                self.assertEqual(spec["codec"], "AC-4", name)

    def test_a_profile_s_own_tolerance_names_only_figures_the_suite_holds(self) -> None:
        allowed = {
            "relative",
            "absolute_permille",
            "stack_free_bytes",
            "headroom_ms",
            "largest_block_bytes",
            "internal_free_bytes",
            "play_heap_free_bytes",
            "play_heap_free_note",
            "glitched_plays",
            "note",
        }
        for profile, spec in self.suite["profiles"].items():
            self.assertLessEqual(set(spec.get("tolerance", {})), allowed, profile)

    def test_the_c6_has_room_for_its_measured_boot_jitter_and_the_s3_keeps_its_own(self) -> None:
        c6 = bs.tolerance_for(self.suite, "c6-16mb")
        s3 = bs.tolerance_for(self.suite, "s3-n16r8")
        # 96,256 to 88,064 was seen: 8,192 bytes
        self.assertGreaterEqual(c6["largest_block_bytes"], 8192)
        self.assertEqual(s3["largest_block_bytes"], 1024)
        self.assertEqual(c6["absolute_permille"], s3["absolute_permille"])

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
            + "sink.dma_ms=64",
            "stream.units=938 stream.held=0 stream.resync_bytes=0 stream.sink=i2s "
            + "stream.decode_stack_free=18260 stream.audio_ms=30016 stream.wall_ms=30130",
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
        if "only" not in kwargs:
            # the /play rows the stand-in has a behaviour for (every profile but the C6's has AC-4
            # rows now, which only the tests that name them play): a Sendspin row is played by the
            # test server (test_suite_sendspin.py)
            kwargs["only"] = [
                row["id"]
                for row in bs.plan(self.suite, profile)
                if row.get("via", "play") == "play"
                and Path(self.suite["streams"][row["stream"]]["file"]).name in stand_in.behaviours
            ]
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

    def test_a_row_on_a_layout_sets_it_for_the_play_and_puts_the_board_s_own_back(self) -> None:
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79)})
        results = self.run_rows(stand_in, "p4-rev3", only=["ac3-5.1-tdm16"])
        self.assertEqual(results[0].findings, [])
        self.assertEqual(results[0].state, "finished")
        self.assertEqual(stand_in.layout_puts, ["9.1.6", "2.0"])
        self.assertEqual(stand_in.layout, "2.0")

    def test_a_board_that_opens_for_fewer_slots_than_the_row_says_is_a_finding(self) -> None:
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79, slots=2)})
        results = self.run_rows(stand_in, "p4-rev3", only=["ac3-5.1-tdm16"])
        self.assertTrue(
            any("rendered onto 2 slots, expected 16" in f for f in results[0].findings),
            results[0].findings,
        )
        self.assertEqual(stand_in.layout, "2.0", "the layout is put back after a failing play too")

    def test_a_layout_the_board_refuses_is_a_refusal_and_nothing_is_played(self) -> None:
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79)})
        stand_in.refused_layouts = {"9.1.6"}
        results = self.run_rows(stand_in, "p4-rev3", only=["ac3-5.1-tdm16"])
        self.assertEqual(results[0].state, "refused")
        self.assertTrue(any("PUT /layout 9.1.6 answered 409" in f for f in results[0].findings))
        self.assertEqual(stand_in.plays, [])
        self.assertEqual(stand_in.layout_puts, ["9.1.6"], "nothing to put back: it was refused")

    # --- a row declares what it needs; the board says what it carries (GET /layout-options) -----

    CHIP = "This chip (v1.3) has no clock for multi-channel output, so only 1.0 and 2.0 can play."

    def listing(self, **verdicts: str) -> dict:
        """GET /layout-options for a board, as the firmware writes it: `verdicts` maps a layout to
        its verdict (supported, limits or unavailable), the reason for the last two the CHIP words
        unless given as "verdict:reason"."""
        options = []
        for layout, said in verdicts.items():
            verdict, _, reason = said.partition(":")
            entry = {"layout": layout.replace("_", "."), "verdict": verdict, "fix": "none"}
            if verdict != "supported":
                entry["reason"] = reason or self.CHIP
            options.append(entry)
        return {"layout": "2.0", "slots": 2, "profile": "p4-rev1", "options": options}

    def test_a_layout_the_board_carries_is_played_whatever_the_profile(self) -> None:
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79)})
        listing = self.listing(**{"9_1_6": "supported"})
        results = self.run_rows(stand_in, "p4-rev3", only=["ac3-5.1-tdm16"], layouts=listing)
        self.assertEqual(results[0].findings, [])
        self.assertEqual(results[0].state, "finished")
        self.assertEqual(len(stand_in.plays), 1)

    def test_a_layout_the_board_says_it_cannot_carry_is_refused_and_the_refusal_is_the_pass(
        self,
    ) -> None:
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79)})
        stand_in.refused_layouts = {"9.1.6"}
        stand_in.refusal_reasons = {"9.1.6": self.CHIP}
        listing = self.listing(**{"9_1_6": "unavailable"})
        results = self.run_rows(stand_in, "p4-rev3", only=["ac3-5.1-tdm16"], layouts=listing)
        self.assertEqual(results[0].findings, [])
        self.assertEqual(results[0].state, "refused-layout")
        self.assertEqual(results[0].why, self.CHIP)
        self.assertTrue(
            any("is refused, as the board's listing says" in n for n in results[0].notes)
        )
        self.assertEqual(stand_in.plays, [], "nothing is played: there is no layout to play on")
        self.assertEqual(stand_in.layout_puts, ["9.1.6"])
        self.assertEqual(stand_in.layout, "2.0")
        # What the record keeps of it: the state and the board's words, no play's figures.
        self.assertEqual(
            results[0].figures(), {"state": "refused-layout", "frames": None, "why": self.CHIP}
        )

    def test_a_refusal_in_other_words_than_the_listing_s_is_a_finding(self) -> None:
        # The route and the listing are one verdict: a board that says two things about one layout
        # has two sources.
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79)})
        stand_in.refused_layouts = {"9.1.6"}
        stand_in.refusal_reasons = {"9.1.6": "more slots than this sink has"}
        listing = self.listing(**{"9_1_6": "unavailable"})
        results = self.run_rows(stand_in, "p4-rev3", only=["ac3-5.1-tdm16"], layouts=listing)
        self.assertTrue(
            any(
                "was refused with 'more slots than this sink has'" in f for f in results[0].findings
            ),
            results[0].findings,
        )

    def test_a_board_that_takes_a_layout_it_lists_as_unavailable_is_a_finding_and_is_put_back(
        self,
    ) -> None:
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79)})
        listing = self.listing(**{"9_1_6": "unavailable"})  # and the stand-in takes it
        results = self.run_rows(stand_in, "p4-rev3", only=["ac3-5.1-tdm16"], layouts=listing)
        self.assertTrue(any("answered 200, expected 409" in f for f in results[0].findings))
        self.assertEqual(stand_in.layout, "2.0", "the board's own layout is put back")
        self.assertEqual(stand_in.layout_puts, ["9.1.6", "2.0"])
        self.assertEqual(stand_in.plays, [])

    def test_a_row_that_says_skip_is_left_out_with_the_reason_and_nothing_is_sent(self) -> None:
        suite = json.loads(json.dumps(self.suite))
        suite["profiles"]["p4-rev3"]["plays"][2]["if_unavailable"] = "skip"
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79)})
        listing = self.listing(**{"9_1_6": "unavailable"})
        board = bs.Board("127.0.0.1", stand_in.port, 5.0)
        results = bs.run_suite(
            board,
            "p4-rev3",
            suite,
            "http://127.0.0.1:1",
            self.pins,
            only=["ac3-5.1-tdm16"],
            sleep=0.01,
            timeout=5.0,
            layouts=listing,
        )
        self.assertEqual(results[0].findings, [])
        self.assertEqual(results[0].state, "skipped")
        self.assertEqual(results[0].why, self.CHIP)
        self.assertTrue(any(self.CHIP in n for n in results[0].notes))
        self.assertEqual(stand_in.layout_puts, [], "a skipped row does not touch the board")
        self.assertEqual(stand_in.plays, [])

    def test_a_layout_carried_with_a_limit_is_played_and_the_limit_is_noted(self) -> None:
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79)})
        limit = "Not verified: the widest layout measured on this kind of board is 2 slots."
        stand_in.layout_notes = {"9.1.6": limit}
        listing = self.listing(**{"9_1_6": "limits:" + limit})
        results = self.run_rows(stand_in, "p4-rev3", only=["ac3-5.1-tdm16"], layouts=listing)
        self.assertEqual(results[0].findings, [])
        self.assertEqual(results[0].state, "finished")
        self.assertTrue(any("is listed with a limit" in n and limit in n for n in results[0].notes))

    def test_a_layout_the_listing_does_not_have_is_tried_as_written(self) -> None:
        # A firmware with no GET /layout-options (layouts is None), and a layout outside the
        # catalog, are played as before: the suite does not invent a verdict.
        stand_in = self.board("p4-rev3", {"ffmpeg.ac3": Behaviour(units=79)})
        for listing in (None, self.listing(**{"5_1": "supported"})):
            stand_in.plays.clear()
            results = self.run_rows(stand_in, "p4-rev3", only=["ac3-5.1-tdm16"], layouts=listing)
            self.assertEqual(results[0].findings, [])
            self.assertEqual(len(stand_in.plays), 1)

    def test_the_fold_is_a_5_1_stream_on_the_2_0_layout_and_needs_no_tdm(self) -> None:
        # On a chip with no clock for a TDM frame the fold is still testable: a row with no layout
        # plays on the board's own 2.0. (The row that set 5.1 to test it is the capability row.)
        stand_in = self.board("p4-rev1", {"ffmpeg.ac3": Behaviour(units=79, slots=2)})
        row = {
            "id": "ac3-5.1-fold",
            "stream": "ac3-5.1",
            "stream_spec": self.suite["streams"]["ac3-5.1"],
        }
        board = bs.Board("127.0.0.1", stand_in.port, 5.0)
        played, _ = bs.run_play(
            board,
            row,
            "http://127.0.0.1:1",
            1,
            None,
            self.suite,
            self.pins,
            5.0,
            0.01,
            layouts=self.listing(**{"5_1": "unavailable"}),
        )
        self.assertEqual(played.findings, [])
        self.assertEqual(played.state, "finished")
        self.assertEqual(stand_in.layout_puts, [], "no layout is set: the board plays on its own")

    def test_the_listing_is_described_in_a_line(self) -> None:
        listing = self.listing(**{"2_0": "supported", "5_1": "limits:x", "7_1": "unavailable"})
        self.assertEqual(
            bs.describe_layouts(listing),
            "layouts (2 slots): supported 2.0; with limits 5.1; unavailable 7.1",
        )
        self.assertIn("no GET /layout-options", bs.describe_layouts(None))

    def test_a_row_that_says_what_to_do_must_say_it_in_words_the_suite_knows(self) -> None:
        suite = json.loads(json.dumps(self.suite))
        suite["profiles"]["p4-rev3"]["plays"][2]["if_unavailable"] = "shrug"
        with self.assertRaisesRegex(bs.SuiteError, "if_unavailable 'shrug'"):
            bs.plan(suite, "p4-rev3")
        suite["profiles"]["p4-rev3"]["plays"][0]["if_unavailable"] = "skip"
        suite["profiles"]["p4-rev3"]["plays"][2].pop("if_unavailable")
        with self.assertRaisesRegex(bs.SuiteError, "names none"):
            bs.plan(suite, "p4-rev3")

    def test_the_layout_options_route_is_read_as_the_firmware_writes_it(self) -> None:
        stand_in = self.board("p4-rev3", {})
        board = bs.Board("127.0.0.1", stand_in.port, 5.0)
        self.assertIsNone(board.layout_options(), "no route is a 404, which is None")
        stand_in.layout_options = self.listing(**{"2_0": "supported"})
        self.assertEqual(board.layout_options()["options"][0]["layout"], "2.0")
        stand_in.layout_options = {"options": "none"}
        with self.assertRaisesRegex(bs.SuiteError, "no list of options"):
            board.layout_options()

    def test_a_baseline_row_the_board_no_longer_carries_says_why_in_the_boards_words(self) -> None:
        baseline = {"profile": "p4-rev3", "plays": {"ac3-5.1-tdm16": {"state": "finished"}}}
        run = {
            "profile": "p4-rev3",
            "plays": {"ac3-5.1-tdm16": {"state": "refused-layout", "why": self.CHIP}},
        }
        regressions, _ = bs.compare(run, baseline, {})
        self.assertEqual(
            regressions,
            [f"ac3-5.1-tdm16: state refused-layout, baseline finished ({self.CHIP})"],
        )

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
        # The /play rows: the C6's Sendspin rows are played by the test server, not here.
        rows = ["ac3-2.0-refused", "eac3-2.0-refused"]
        refusing = {name: Behaviour(refuse=True) for name in ("ffmpeg.ac3", "ffmpeg.ec3")}
        results = self.run_rows(self.board("c6-16mb", refusing), "c6-16mb", only=rows)
        self.assertEqual([r.id for r in results], rows)
        for r in results:
            self.assertEqual(r.findings, [], r.id)
            self.assertEqual((r.state, r.why), ("failed", "memory"))
        playing = self.board("c6-16mb", standard())
        results = self.run_rows(playing, "c6-16mb", only=rows)
        self.assertTrue(any("expected a refusal for memory" in f for f in results[0].findings))

    def test_an_ac4_play_needs_the_hosts_pcm_hash(self) -> None:
        pin = self.pins["ac4_20_music"]
        good = {"ac4-20-music-192-3frames.ac4": Behaviour(units=3, pcm_hash=pin)}
        results = self.run_rows(
            self.board("s3-n16r8", {**standard(), **good}, **{}),
            "s3-n16r8",
            only=["ac4-20-music"],
        )
        self.assertEqual(results[0].findings, [])
        self.assertEqual(results[0].decoder_permille, 480)
        wrong = {"ac4-20-music-192-3frames.ac4": Behaviour(units=3, pcm_hash="0123456789abcdef")}
        results = self.run_rows(
            self.board("s3-n16r8", {**standard(), **wrong}),
            "s3-n16r8",
            only=["ac4-20-music"],
        )
        self.assertTrue(any("is not the host's" in f for f in results[0].findings))
        zero = {"ac4-20-music-192-3frames.ac4": Behaviour(units=3, pcm_hash="0" * 16)}
        results = self.run_rows(
            self.board("s3-n16r8", {**standard(), **zero}),
            "s3-n16r8",
            only=["ac4-20-music"],
        )
        self.assertTrue(any("AC4_PCM_HASH=1" in f for f in results[0].findings))

    def test_repeating_a_row_holds_its_median_and_its_worst(self) -> None:
        stand_in = self.board("s3-n16r8", standard())
        results = self.run_rows(stand_in, "s3-n16r8", repeat=3)
        self.assertEqual(len(stand_in.plays), 6)
        self.assertEqual(len(results), 2)
        self.assertEqual(len(set(stand_in.plays)), 6, "each play has a location of its own")


class Boot(unittest.TestCase):
    def test_the_last_budget_line_of_the_log_is_the_boot_figure(self) -> None:
        stand_in = StandIn("s3-n16r8", standard())
        try:
            boot = bs.boot_budget(bs.Board("127.0.0.1", stand_in.port))
        finally:
            stand_in.close()
        self.assertEqual(
            boot,
            {
                "when": "boot",
                "net": 1,
                "internal_free": 86231,
                "largest": 31744,
                "ask_total": 8192,
                "ask_biggest": 8192,
                "headroom": 23552,
                "verdict": "ok",
                "floor": 0,
            },
        )

    def test_a_log_that_has_rotated_the_line_out_gives_nothing(self) -> None:
        stand_in = StandIn("s3-n16r8", standard())
        try:
            stand_in.log = bytearray(b"sendspin: [1] connected\n")
            self.assertEqual(bs.boot_budget(bs.Board("127.0.0.1", stand_in.port)), {})
        finally:
            stand_in.close()

    def test_a_run_records_the_boot_line_and_the_samples_of_a_repeated_row(self) -> None:
        stand_in = StandIn("s3-n16r8", standard())
        try:
            board = bs.Board("127.0.0.1", stand_in.port, 5.0)
            boot = bs.boot_budget(board)
            results = bs.run_suite(
                board,
                "s3-n16r8",
                bs.load_suite(),
                "http://127.0.0.1:1",
                {},
                repeat=3,
                only=["ac3-2.0", "eac3-2.0"],
                sleep=0.01,
                timeout=5.0,
            )
            hardware = board.hardware()
        finally:
            stand_in.close()
        record = bs.run_record("s3-n16r8", hardware, results, boot, "S3b")
        self.assertEqual(record["board"], "S3b")
        self.assertEqual(record["boot"]["largest"], 31744)
        self.assertEqual(record["plays"]["ac3-2.0"]["decoder_permille_samples"], [223, 223, 223])


class BaselineNote(unittest.TestCase):
    """A baseline recorded under another hash than the run's image carries says so."""

    def test_the_same_hash_has_no_note(self) -> None:
        run = {"profile_hash": "0fb46709d4bb"}
        self.assertIsNone(bs.baseline_note(run, {"profile_hash": "0fb46709d4bb"}))

    def test_another_hash_says_both_and_that_the_figures_are_as_recorded(self) -> None:
        note = bs.baseline_note({"profile_hash": "6e5a68ebbb57"}, {"profile_hash": "0fb46709d4bb"})
        assert note is not None
        self.assertIn("0fb46709d4bb", note)
        self.assertIn("6e5a68ebbb57", note)
        self.assertIn("compared as they were recorded", note)
        self.assertTrue(note.startswith("note:"))

    def test_a_hash_that_is_not_known_on_either_side_is_not_a_difference(self) -> None:
        self.assertIsNone(bs.baseline_note({"profile_hash": "x"}, {}))
        self.assertIsNone(bs.baseline_note({}, {"profile_hash": "x"}))
        self.assertIsNone(bs.baseline_note({"profile_hash": ""}, {"profile_hash": "x"}))

    def test_the_compare_command_prints_it_and_the_exit_code_is_the_figures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = {
                "profile": "s3-n16r8",
                "profile_hash": "c4b8d369cf36",
                "boot": {},
                "plays": {"ac3-2.0": {"state": "finished", "decoder_permille": 123, "units": 938}},
            }
            run = {
                "profile": "s3-n16r8",
                "profile_hash": "0fb46709d4bb",
                "boot": {},
                "plays": {"ac3-2.0": {"state": "finished", "decoder_permille": 124, "units": 938}},
            }
            (Path(tmp) / "base.json").write_text(json.dumps(base), "utf-8")
            (Path(tmp) / "run.json").write_text(json.dumps(run), "utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = bs.main(
                    [
                        "compare",
                        "--run",
                        str(Path(tmp) / "run.json"),
                        "--baseline",
                        str(Path(tmp) / "base.json"),
                    ]
                )
        self.assertEqual(code, 0)
        self.assertIn("recorded under profile hash c4b8d369cf36", out.getvalue())
        self.assertIn("the run holds to the baseline", out.getvalue())

    def test_the_note_does_not_hide_a_regression(self) -> None:
        base = {
            "profile_hash": "c4b8d369cf36",
            "profile": "s3-n16r8",
            "plays": {"ac3-2.0": {"state": "finished", "decoder_permille": 100, "units": 938}},
        }
        run = {
            "profile_hash": "0fb46709d4bb",
            "profile": "s3-n16r8",
            "plays": {"ac3-2.0": {"state": "finished", "decoder_permille": 200, "units": 938}},
        }
        regressions, _ = bs.compare(run, base, {})
        self.assertTrue(regressions)
        self.assertIsNotNone(bs.baseline_note(run, base))


class FoldedPin(unittest.TestCase):
    """A pin is the host's hash of a stream in its own layout: a fold is not it."""

    ROW: ClassVar[dict] = {
        "id": "ac4-51-music",
        "stream": "ac4-51-music",
        "stream_spec": {
            "codec": "AC-4",
            "units": 3,
            "pin": "ac4_51_music",
            "pin_layout": "5.1",
        },
    }
    PINS: ClassVar[dict[str, str]] = {"ac4_51_music": "6d36d7d2e8a070e9"}

    def judge(self, layout: str, pcm_hash: str) -> bs.PlayResult:
        result = bs.PlayResult(id="ac4-51-music", stream="ac4-51-music", expect="pass")
        result.state = "finished"
        result.lines = bs.parse_play_lines(
            [
                "sink.writes=24 sink.underruns=0 sink.dry_ms=0 sink.min_headroom_ms=5 "
                + "sink.dma_ms=64",
                "stream.units=3 stream.held=0 stream.resync_bytes=0 stream.sink=i2s_wide "
                + "stream.decode_stack_free=17020 stream.audio_ms=128 stream.wall_ms=17",
                f"ac4.lap=1 frames=3 decoder_permille=604 worst_frame_us=33624 pcm_hash={pcm_hash}",
                "result=pass",
            ]
        )
        bs._judge(
            result, {"state": "finished", "layout": layout}, self.ROW["stream_spec"], self.PINS
        )
        return result

    def test_the_stream_folded_to_2_0_is_not_held_to_the_5_1_pin(self) -> None:
        result = self.judge("2.0", "7162259f3e596916")
        self.assertEqual(result.findings, [])
        self.assertTrue(result.folded)
        self.assertEqual(result.figures()["layout"], "2.0")
        self.assertEqual(result.figures()["pcm_hash"], "7162259f3e596916")

    def test_the_stream_played_in_its_own_layout_is_held_to_the_pin(self) -> None:
        self.assertEqual(self.judge("5.1", "6d36d7d2e8a070e9").findings, [])
        wrong = self.judge("5.1", "7162259f3e596916")
        self.assertTrue(any("is not the host's" in f for f in wrong.findings))
        self.assertFalse(wrong.folded)

    def test_a_fold_with_no_hash_at_all_is_still_a_finding(self) -> None:
        result = self.judge("2.0", "0000000000000000")
        self.assertTrue(any("no PCM hash" in f for f in result.findings))

    def test_a_row_with_no_pin_layout_is_held_to_its_pin_whatever_the_layout(self) -> None:
        spec = {"codec": "AC-4", "pin": "ac4_51_music"}
        result = bs.PlayResult(id="x", stream="x", expect="pass")
        result.state = "finished"
        result.lines = bs.parse_play_lines(
            ["ac4.lap=1 frames=3 decoder_permille=1 pcm_hash=7162259f3e596916", "result=pass"]
        )
        bs._judge(result, {"state": "finished", "layout": "2.0"}, spec, self.PINS)
        self.assertTrue(any("is not the host's" in f for f in result.findings))

    def test_the_folded_row_s_own_hash_is_what_the_baseline_holds_it_to(self) -> None:
        base = {
            "profile": "p4-rev1",
            "plays": {
                "ac4-51-music": {
                    "state": "finished",
                    "units": 3,
                    "pcm_hash": "7162259f3e596916",
                    "layout": "2.0",
                }
            },
        }
        same = {
            "profile": "p4-rev1",
            "plays": {
                "ac4-51-music": {
                    "state": "finished",
                    "units": 3,
                    "pcm_hash": "7162259f3e596916",
                    "layout": "2.0",
                }
            },
        }
        moved = {
            "profile": "p4-rev1",
            "plays": {
                "ac4-51-music": {
                    "state": "finished",
                    "units": 3,
                    "pcm_hash": "0123456789abcdef",
                    "layout": "2.0",
                }
            },
        }
        self.assertEqual(bs.compare(same, base, {})[0], [])
        regressions, _ = bs.compare(moved, base, {})
        self.assertTrue(any("PCM hash" in line for line in regressions))


class LogPaging(unittest.TestCase):
    """GET /log answers 4 KB at a time; a read follows X-Log-Next to the end."""

    def test_a_whole_log_read_follows_the_pages_and_ends_at_the_real_end(self) -> None:
        noisy = b"".join(b"hosted: line %05d of the boot\n" % n for n in range(400))  # ~12.8 KB
        stand_in = StandIn("p4-rev1", standard())
        try:
            stand_in.log = bytearray(noisy + budget_log(292211, 221184))
            board = bs.Board("127.0.0.1", stand_in.port)
            text, following = board.log(None)
            boot = bs.boot_budget(board)
        finally:
            stand_in.close()
        self.assertEqual(len(text.encode()), len(noisy) + len(budget_log(292211, 221184)))
        self.assertEqual(following, len(noisy) + len(budget_log(292211, 221184)))
        self.assertEqual((boot["internal_free"], boot["largest"]), (292211, 221184))

    def test_a_read_from_a_mark_follows_the_pages_too(self) -> None:
        stand_in = StandIn("p4-rev1", standard())
        try:
            stand_in.log = bytearray(b"x" * 10000)
            board = bs.Board("127.0.0.1", stand_in.port)
            text, following = board.log(1000)
        finally:
            stand_in.close()
        self.assertEqual((len(text), following), (9000, 10000))

    def test_a_log_that_is_shorter_than_the_mark_is_returned_for_the_caller_to_read_as_a_restart(
        self,
    ) -> None:
        stand_in = StandIn("p4-rev1", standard())
        try:
            stand_in.log = bytearray(b"ESP-ROM:esp32p4\n")
            board = bs.Board("127.0.0.1", stand_in.port)
            text, following = board.log(20000)
        finally:
            stand_in.close()
        self.assertEqual((text, following), ("", 16))

    def test_a_run_whose_log_is_longer_than_a_page_is_marked_at_its_end(self) -> None:
        # The first read of a whole log used to mark the end of its first 4 KB, and the play
        # then read the rest of the old log, with its old result= line, as its own.
        stand_in = StandIn("s3-n16r8", standard())
        try:
            stand_in.log = bytearray(
                b"".join(
                    b"old play: sink.writes=1 sink.underruns=9 sink.dry_ms=9 "
                    b"sink.min_headroom_ms=0 sink.dma_ms=1\n"
                    b"result=fail\n"
                    for _ in range(200)
                )
            )
            board = bs.Board("127.0.0.1", stand_in.port, 5.0)
            results = bs.run_suite(
                board,
                "s3-n16r8",
                bs.load_suite(),
                "http://127.0.0.1:1",
                {},
                repeat=1,
                sleep=0.01,
                timeout=5.0,
                only=["ac3-2.0", "eac3-2.0"],
            )
        finally:
            stand_in.close()
        for result in results:
            self.assertEqual(result.findings, [], result.id)
            self.assertEqual(result.lines.underruns, 0, result.id)


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

    def test_the_boot_line_is_ratcheted_against_erosion(self) -> None:
        tolerance = {**self.TOLERANCE, "largest_block_bytes": 1024, "internal_free_bytes": 4096}
        base = {
            **self.figures(),
            "boot": {"largest": 31744, "internal_free": 86231, "verdict": "ok"},
        }
        same = {
            **self.figures(),
            "boot": {"largest": 31744, "internal_free": 86000, "verdict": "ok"},
        }
        self.assertEqual(bs.compare(same, base, tolerance), ([], []))
        eroded = {
            **self.figures(),
            "boot": {"largest": 30000, "internal_free": 70000, "verdict": "ok"},
        }
        regressions, _ = bs.compare(eroded, base, tolerance)
        self.assertEqual(len(regressions), 2)
        self.assertIn("largest internal block is 30,000", regressions[0])
        short = {
            **self.figures(),
            "boot": {"largest": 31744, "internal_free": 86231, "verdict": "short-block"},
        }
        self.assertIn("verdict short-block", bs.compare(short, base, tolerance)[0][0])
        none = self.figures()
        self.assertIn("no budget line", bs.compare(none, base, tolerance)[0][0])

    def test_a_missing_play_or_another_profile_is_a_regression(self) -> None:
        run = {"profile": "c6-16mb", "plays": {}}
        regressions, _ = bs.compare(run, self.figures(), self.TOLERANCE)
        self.assertEqual(len(regressions), 2)


class BenchBoards(unittest.TestCase):
    """--board against the bench's table: the lock and the address are the board's own."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        path = temp_bench(Path(self.tmp.name))
        patch = mock.patch.dict(os.environ, {bench.ENVIRONMENT: str(path)})
        patch.start()
        self.addCleanup(patch.stop)

    @staticmethod
    def args(**more: object) -> argparse.Namespace:
        base = {
            "board": "",
            "host": None,
            "lock": None,
            "restart": False,
            "accept_restart_fault": False,
            "reset_by": "http",
        }
        return argparse.Namespace(**{**base, **more})

    def test_a_board_of_the_bench_gives_the_address_and_the_lock(self) -> None:
        args = self.args(board="S3b")
        bs.resolve_board(args)
        self.assertEqual(args.host, "192.168.1.45")
        self.assertEqual(Path(args.lock).name, "s3-board-45.lock")
        self.assertEqual(Path(args.lock).parent, Path(self.tmp.name) / "locks")

    def test_an_address_that_is_not_the_board_s_is_refused(self) -> None:
        with self.assertRaises(bs.SuiteError) as raised:
            bs.resolve_board(self.args(board="S3b", host="192.168.1.156"))
        self.assertIn("192.168.1.45", str(raised.exception))

    def test_another_board_s_lock_is_refused(self) -> None:
        with self.assertRaises(bs.SuiteError) as raised:
            bs.resolve_board(self.args(board="S3b", lock="D:/ac3bld/s3-board-156.lock"))
        self.assertIn("s3-board-45.lock", str(raised.exception))

    def test_the_board_s_own_address_and_lock_given_by_hand_are_accepted(self) -> None:
        args = self.args(board="C6", host="192.168.1.117", lock="D:/ac3bld/c6-board.lock")
        bs.resolve_board(args)
        self.assertEqual(args.host, "192.168.1.117")
        self.assertEqual(args.lock, "D:/ac3bld/c6-board.lock")

    def test_a_p4_is_not_restarted_unless_the_run_accepts_the_fault(self) -> None:
        for name in ("P4a", "P4b"):
            with self.assertRaises(bs.SuiteError) as raised:
                bs.resolve_board(self.args(board=name, restart=True))
            self.assertIn("C6 link", str(raised.exception))
        accepted = self.args(board="P4a", restart=True, accept_restart_fault=True)
        bs.resolve_board(accepted)
        self.assertEqual(accepted.host, "192.168.1.99")

    def test_a_p4_may_be_reset_by_esptool_where_its_http_restart_is_refused(self) -> None:
        args = self.args(board="P4a", restart=True, reset_by="esptool")
        bs.resolve_board(args)
        self.assertEqual(args.host, "192.168.1.99")

    def test_an_s3_is_restarted_without_a_word(self) -> None:
        args = self.args(board="S3a", restart=True)
        bs.resolve_board(args)
        self.assertEqual(args.host, "192.168.1.156")

    def test_a_label_that_is_not_on_the_bench_is_only_a_label(self) -> None:
        args = self.args(board="the stand-in", host="127.0.0.1", lock="x.lock")
        bs.resolve_board(args)
        self.assertEqual((args.host, args.lock), ("127.0.0.1", "x.lock"))

    def test_no_board_leaves_the_arguments_as_they_were(self) -> None:
        args = self.args(host="127.0.0.1")
        bs.resolve_board(args)
        self.assertEqual(args.host, "127.0.0.1")
        self.assertIsNone(args.lock)


class ResetBy(unittest.TestCase):
    """--reset-by esptool: a P4's boots, taken by a watchdog reset and not by POST /restart."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        path = temp_bench(self.root)
        data = json.loads(path.read_text("utf-8"))
        data["boards"]["P4a"]["host"] = "127.0.0.1"  # the stand-in
        path.write_text(json.dumps(data), "utf-8")
        patch = mock.patch.dict(os.environ, {bench.ENVIRONMENT: str(path)})
        patch.start()
        self.addCleanup(patch.stop)
        self.stand_in = StandIn("p4-rev1", standard())
        self.addCleanup(self.stand_in.close)
        self.saved = bs.BASELINES
        bs.BASELINES = self.root / "baselines"
        self.addCleanup(setattr, bs, "BASELINES", self.saved)
        self.lock = self.root / "locks" / "p4-board.lock"
        self.out = self.root / "run.json"
        self.resets: list[str] = []

    def reset_through_the_stand_in(self, table, board, run, probe=None, wait=90.0):
        """What esptool's reset does to the board, as the stand-in's restart."""
        self.resets.append(board.name)
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.stand_in.port}/restart", method="POST", data=b""
        )
        urllib.request.urlopen(request, timeout=5).read()
        return {"version": "v0.10.0-beta.1-4175-g79d2e7618-"}

    def run_cli(self, *extra: str) -> tuple[int, str, str]:
        argv = [
            "run",
            "--profile",
            "p4-rev1",
            "--board",
            "P4a",
            "--host",
            "127.0.0.1",
            "--port",
            str(self.stand_in.port),
            "--lock",
            str(self.lock),
            "--session",
            "16d036",
            "--serve-host",
            "127.0.0.1",
            "--out",
            str(self.out),
            "--play-timeout",
            "10",
            # the /play rows: the Sendspin rows need the test server, which these do not have
            "--only",
            "ac3-2.0",
            "--only",
            "eac3-2.0",
            *extra,
        ]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bs.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_the_boots_are_taken_by_the_reset_and_the_worst_is_held(self) -> None:
        self.stand_in.boot_logs = [budget_log(112500, 96256), budget_log(108988, 94208)]
        with mock.patch.object(bs.bench_flash, "reset_board", self.reset_through_the_stand_in):
            code, out, err = self.run_cli("--restart", "--reset-by", "esptool", "--boots", "2")
        self.assertEqual((code, err), (0, ""), out)
        self.assertEqual(self.resets, ["P4a", "P4a"])
        record = json.loads(self.out.read_text("utf-8"))
        self.assertEqual(
            (record["boot"]["internal_free"], record["boot"]["largest"]), (108988, 94208)
        )

    def test_the_http_restart_of_a_p4_is_still_refused_before_anything_is_sent(self) -> None:
        with mock.patch.object(bs.bench_flash, "reset_board", self.reset_through_the_stand_in):
            code, _, err = self.run_cli("--restart")
        self.assertEqual(code, 2)
        self.assertIn("--reset-by esptool", err)
        self.assertEqual((self.resets, self.stand_in.restarts), ([], 0))
        self.assertFalse(self.lock.exists())

    def test_esptool_is_a_way_to_restart_and_so_needs_restart(self) -> None:
        code, _, err = self.run_cli("--reset-by", "esptool")
        self.assertEqual(code, 2)
        self.assertIn("needs --restart", err)

    def test_it_needs_the_board_to_be_one_of_the_bench(self) -> None:
        args = [
            "run",
            "--profile",
            "p4-rev1",
            "--board",
            "a stand-in",
            "--host",
            "127.0.0.1",
            "--port",
            str(self.stand_in.port),
            "--lock",
            str(self.lock),
            "--session",
            "16d036",
            "--restart",
            "--reset-by",
            "esptool",
        ]
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            code = bs.main(args)
        self.assertEqual(code, 2)
        self.assertIn("board of bench.json", err.getvalue())

    def test_a_board_that_does_not_answer_after_the_reset_is_an_error_and_the_lock_goes(
        self,
    ) -> None:
        with mock.patch.object(bs.bench_flash, "reset_board", lambda *a, **k: None):
            code, _, err = self.run_cli("--restart", "--reset-by", "esptool")
        self.assertEqual(code, 2)
        self.assertIn("did not answer after the reset", err)
        self.assertFalse(self.lock.exists())


class WorstBoot(unittest.TestCase):
    def test_it_is_the_boot_with_the_smallest_largest_block(self) -> None:
        samples = [
            {"largest": 96256, "internal_free": 112000},
            {"largest": 88064, "internal_free": 112600},
            {"largest": 94208, "internal_free": 108988},
        ]
        self.assertEqual(bs.worst_boot(samples)["largest"], 88064)

    def test_a_tie_goes_to_the_least_free_ram(self) -> None:
        samples = [
            {"largest": 31744, "internal_free": 102539},
            {"largest": 31744, "internal_free": 85923},
            {"largest": 31744, "internal_free": 86231},
        ]
        self.assertEqual(bs.worst_boot(samples)["internal_free"], 85923)

    def test_a_boot_with_no_line_is_not_a_boot(self) -> None:
        self.assertEqual(bs.worst_boot([{}, {"largest": 1, "internal_free": 2}, {}])["largest"], 1)
        self.assertEqual(bs.worst_boot([{}, {}]), {})
        self.assertEqual(bs.worst_boot([]), {})


class Tolerance(unittest.TestCase):
    SUITE: ClassVar[dict] = {
        "tolerance": {"relative": 0.05, "largest_block_bytes": 1024, "note": "the suite's"},
        "profiles": {
            "wide": {"tolerance": {"largest_block_bytes": 12288, "note": "why"}},
            "plain": {},
        },
    }

    def test_a_profile_s_own_values_go_over_the_suite_s_and_its_note_is_not_one(self) -> None:
        wide = bs.tolerance_for(self.SUITE, "wide")
        self.assertEqual(wide["largest_block_bytes"], 12288)
        self.assertEqual(wide["relative"], 0.05)
        self.assertEqual(wide["note"], "the suite's")

    def test_a_profile_with_none_or_no_row_has_the_suite_s(self) -> None:
        for profile in ("plain", "absent", None):
            self.assertEqual(bs.tolerance_for(self.SUITE, profile)["largest_block_bytes"], 1024)

    def test_a_c6_boot_one_step_below_the_common_mode_is_not_a_regression_on_the_c6(self) -> None:
        baseline = {
            "profile": "c6-16mb",
            "boot": {"largest": 96256, "internal_free": 112500, "verdict": "below-floor"},
            "plays": {},
        }
        run = {
            "profile": "c6-16mb",
            "boot": {"largest": 88064, "internal_free": 112500, "verdict": "below-floor"},
            "plays": {},
        }
        suite = bs.load_suite()
        regressions, _ = bs.compare(run, baseline, bs.tolerance_for(suite, "c6-16mb"))
        self.assertEqual(regressions, [])
        # the S3's 1,024 bytes would call the same boot a regression
        regressions, _ = bs.compare(run, baseline, bs.tolerance_for(suite, "s3-n16r8"))
        self.assertTrue(any("largest internal block" in line for line in regressions))
        # and a loss the C6's allowance does not cover still is one
        run["boot"]["largest"] = 65536
        regressions, _ = bs.compare(run, baseline, bs.tolerance_for(suite, "c6-16mb"))
        self.assertTrue(any("largest internal block" in line for line in regressions))


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
            # the /play rows: the Sendspin rows need the test server, which these do not have
            "--only",
            "ac3-2.0",
            "--only",
            "eac3-2.0",
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
        self.assertNotIn(NETWORK_NAME, out + err + self.out.read_text("utf-8"))
        self.assertNotIn("192.168.1.9", self.out.read_text("utf-8"))

    def test_streams_are_played_from_a_server_that_is_already_running_when_one_is_named(
        self,
    ) -> None:
        argv = list(self.run_args("--lock", str(self.lock), "--session", "16d036"))
        at = argv.index("--serve-host")
        del argv[at : at + 2]  # the suite starts none when it is given one
        argv += ["--fixtures-url", "http://127.0.0.1:9/"]
        code, out, err = self.run_cli(*argv)
        self.assertEqual((code, err), (0, ""), out)
        self.assertEqual(len(self.stand_in.plays), 2)
        for url in self.stand_in.plays:
            self.assertTrue(url.startswith("http://127.0.0.1:9/testdata/"), url)
            self.assertNotIn("//testdata", url)

    def test_serving_its_own_and_naming_a_server_are_one_or_the_other(self) -> None:
        argv = self.run_args("--lock", str(self.lock), "--session", "16d036")
        with self.assertRaises(SystemExit):
            self.run_cli(*argv, "--fixtures-url", "http://127.0.0.1:9")
        self.assertEqual(self.stand_in.plays, [])

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

    def test_restart_puts_the_boot_line_back_in_the_log_before_a_recording(self) -> None:
        # A board that has played has rotated its boot lines out of a small log.
        self.stand_in.log = bytearray(b"sendspin: [1] connected\n")
        code, out, _ = self.run_cli(
            *self.run_args("--lock", str(self.lock), "--session", "16d036", "--record")
        )
        self.assertEqual(code, 0)
        self.assertIn("not recorded: the log holds no boot budget line", out)
        self.assertFalse((bs.BASELINES / "s3-n16r8.json").exists())
        self.assertEqual(self.stand_in.restarts, 0)
        code, out, _ = self.run_cli(
            *self.run_args("--lock", str(self.lock), "--session", "16d036", "--record", "--restart")
        )
        self.assertEqual(code, 0, out)
        self.assertEqual(self.stand_in.restarts, 1)
        baseline = json.loads((bs.BASELINES / "s3-n16r8.json").read_text("utf-8"))
        self.assertEqual(baseline["boot"]["largest"], 31744)

    def test_boots_restarts_that_many_times_and_the_record_holds_the_worst(self) -> None:
        self.stand_in.boot_logs = [
            budget_log(86231, 31744),
            budget_log(102539, 31744),
            budget_log(85923, 31744),
        ]
        code, out, _ = self.run_cli(
            *self.run_args(
                "--lock", str(self.lock), "--session", "16d036", "--restart", "--boots", "3"
            )
        )
        self.assertEqual(code, 0, out)
        self.assertEqual(self.stand_in.restarts, 3)
        self.assertIn("restarting (3 of 3)", out)
        record = json.loads(self.out.read_text("utf-8"))
        self.assertEqual(record["boot"]["internal_free"], 85923)
        self.assertEqual(
            record["boot_samples"],
            {"internal_free": [86231, 102539, 85923], "largest": [31744, 31744, 31744]},
        )

    def test_the_worst_boot_is_the_one_a_baseline_records(self) -> None:
        self.stand_in.boot_logs = [budget_log(112500, 96256), budget_log(108988, 94208)]
        code, out, _ = self.run_cli(
            *self.run_args(
                "--lock",
                str(self.lock),
                "--session",
                "16d036",
                "--restart",
                "--boots",
                "2",
                "--record",
            )
        )
        self.assertEqual(code, 0, out)
        baseline = json.loads((bs.BASELINES / "s3-n16r8.json").read_text("utf-8"))
        self.assertEqual(
            (baseline["boot"]["internal_free"], baseline["boot"]["largest"]), (108988, 94208)
        )

    def test_one_boot_has_no_samples_to_keep(self) -> None:
        code, out, _ = self.run_cli(
            *self.run_args("--lock", str(self.lock), "--session", "16d036", "--restart")
        )
        self.assertEqual(code, 0, out)
        self.assertNotIn("boot_samples", json.loads(self.out.read_text("utf-8")))

    def test_boots_without_restart_or_below_one_is_refused_before_the_board_is_touched(
        self,
    ) -> None:
        for extra in (("--boots", "3"), ("--restart", "--boots", "0")):
            code, _, err = self.run_cli(
                *self.run_args("--lock", str(self.lock), "--session", "16d036", *extra)
            )
            self.assertEqual(code, 2, extra)
            self.assertIn("--boots", err)
        self.assertEqual((self.stand_in.restarts, self.stand_in.plays), (0, []))
        self.assertFalse(self.lock.exists())

    def c6_stand_in(self) -> None:
        self.stand_in.hardware["profile"] = "c6-16mb"
        self.stand_in.hardware["profile_hash"] = sink_profile.profile_hash(
            sink_profile.load(), "c6-16mb"
        )

    def test_a_sendspin_row_is_played_by_the_test_server_and_the_pairing_is_tidied(self) -> None:
        self.c6_stand_in()
        closed: list[str] = []

        class Tidy:
            def __init__(self, board, keep=False) -> None:
                self.keep = keep

            def close(self) -> str:
                closed.append("closed")
                return "pairing: forgot the suite's record"

        stream = (
            "sendspin.stream=bursts bursts=938 late=0 dropped=0 invalid=0 underruns=0 resyncs=0 "
            "worst_error_us=-362 burst_us=21111 sink_us=10674 ring_high=35260 heap_free=52560"
        )
        canned = ss.SendspinRun(
            exit_code=0,
            stream=ss.parse_stream_end(stream),
            after={"decode_stack_free": 10904},
            report={"players": [{"counters": {}}]},
            server_id="SUITEID",
        )
        with (
            mock.patch.object(ss, "find_testserver", return_value=Path("hearth-testserver")),
            mock.patch.object(ss, "Pairing", Tidy),
            mock.patch.object(ss, "play_row", return_value=canned) as played,
        ):
            code, out, err = self.run_cli(
                *self.run_args(
                    "--lock", str(self.lock), "--session", "16d036",
                    "--profile", "c6-16mb", "--only", "ac3-2.0-sendspin",
                )
            )  # fmt: skip
        self.assertEqual((code, err), (0, ""), out)
        self.assertEqual(played.call_count, 1)
        self.assertTrue(str(played.call_args.args[3]).endswith("ffmpeg.ac3"))
        self.assertEqual(closed, ["closed"])
        self.assertIn("pairing: forgot", out)
        figures = json.loads(self.out.read_text("utf-8"))["plays"]["ac3-2.0-sendspin"]
        self.assertEqual(figures["via"], "sendspin")
        self.assertEqual(figures["decoder_permille"], 660)
        self.assertEqual(figures["play_heap_free"], 52560)
        self.assertFalse(self.lock.exists())

    def test_no_test_server_is_refused_before_the_board_is_booked_or_touched(self) -> None:
        self.c6_stand_in()
        with mock.patch.object(
            ss,
            "find_testserver",
            side_effect=ss.SendspinError("no hearth-testserver: --testserver"),
        ):
            code, _, err = self.run_cli(
                *self.run_args(
                    "--lock", str(self.lock), "--session", "16d036",
                    "--profile", "c6-16mb", "--only", "ac3-2.0-sendspin", "--restart",
                )
            )  # fmt: skip
        self.assertEqual(code, 2)
        self.assertIn("--testserver", err)
        self.assertFalse(self.lock.exists())
        self.assertEqual((self.stand_in.restarts, self.stand_in.plays), (0, []))

    def test_a_board_that_is_another_image_is_refused_before_it_is_restarted(self) -> None:
        # Eight restarts of a board the suite then refuses are eight restarts for nothing.
        self.stand_in.hardware["profile"] = ""
        code, _, err = self.run_cli(
            *self.run_args(
                "--lock", str(self.lock), "--session", "16d036", "--restart", "--boots", "8"
            )
        )
        self.assertEqual(code, 2)
        self.assertIn("no profile", err)
        self.assertEqual((self.stand_in.restarts, self.stand_in.plays), (0, []))
        self.assertFalse(self.lock.exists())

    def test_a_line_read_half_printed_is_not_a_figure(self) -> None:
        found = bs.parse_play_lines(
            [
                "sink.writes=5628 sink.underruns=0 sink.dry_ms=0 sink.min_headroom_ms=5 "
                + "sink.dma_ms=64",
                "sink.writes=5628 sink.underruns=3 sink.dry_",
                "stream.units=938 stream.held=0 stream.resync_bytes=0 stream.sink=i2s "
                + "stream.decode_stack_free=18260 stream.audio_ms=30016 stream.wall_ms=30130",
                "stream.units=1 stream.held=0 stream.resy",
            ]
        )
        self.assertEqual((found.underruns, found.units, found.decode_stack_free), (0, 938, 18260))

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
