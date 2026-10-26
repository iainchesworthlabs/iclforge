"""Unit tests for suite_sendspin.py and the board suite's Sendspin rows.

stdlib unittest. The board and the test server are stand-ins: the board answers what the C6
answered on 2026-10-12 (the `sendspin.stream=` line is one it printed), and the server is a
process stub that writes the report the real one writes. Nothing here touches a network.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from typing import Any, ClassVar
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import board_suite as bs
import suite_sendspin as ss

# A line the C6 printed at the end of a 30 s AC-3 stream (2026-10-12).
STREAM_LINE = (
    "sendspin.stream=bursts bursts=938 late=0 dropped=0 invalid=0 underruns=0 resyncs=0 "
    "silence_frames=12537 skipped_frames=10 dropped_frames=14 repeated_frames=31 "
    "worst_error_us=-362 burst_us=21111 sink_us=10674 ring_high=35260 heap_free=52560"
)
EAC3_LINE = STREAM_LINE.replace("burst_us=21111", "burst_us=24217").replace(
    "heap_free=52560", "heap_free=2616"
)


class StubBoard:
    """What suite_sendspin asks of a board."""

    def __init__(
        self,
        log_text: str = STREAM_LINE + "\n",
        codes: list[str] | None = None,
        table: dict[str, str] | None = None,
        log_offsets: tuple[int, int] = (100, 400),
    ) -> None:
        self.host = "192.168.1.117"
        self.log_text = log_text
        self.codes = list(codes if codes is not None else ["", "482913"])
        self.table = dict(table or {"HEARTHID": "Hearth live test"})
        self.log_offsets = log_offsets
        self.forgotten: list[str] = []
        self.after = {"decode_stack_free": 10904, "paired": 1}
        self._calls = 0

    def sendspin(self) -> dict[str, Any]:
        code = self.codes[min(self._calls, len(self.codes) - 1)]
        self._calls += 1
        return {"pairing_code": code, **self.after}

    def pairing(self) -> dict[str, Any]:
        return {
            "capacity": 8,
            "servers": [
                {"server_id": sid, "name": name, "connected": False}
                for sid, name in self.table.items()
            ],
        }

    def forget(self, server_id: str) -> int:
        self.forgotten.append(server_id)
        self.table.pop(server_id, None)
        return 200

    def log(self, start: int | None) -> tuple[str, int | None]:
        if start is None:
            return "", self.log_offsets[0]
        return self.log_text, self.log_offsets[1]


class FakeProcess:
    def __init__(self, code: int | None) -> None:
        self.code = code
        self.killed = False

    def wait(self, timeout: float | None = None) -> int:
        if self.code is None:
            raise subprocess.TimeoutExpired("hearth-testserver", timeout or 0)
        return self.code

    def kill(self) -> None:
        self.killed = True


def report(
    failure: str | None = None,
    player_failure: str | None = None,
    sent: int = 938,
    **counters: Any,
) -> dict[str, Any]:
    """The report the real server writes: a run-level failure is the top-level `result`
    ("failed: C6 ran out of audio 1 time(s)", main.cpp finish()), and a player's own `failure`
    is only for one that was displaced, closed or did not connect."""
    return {
        "server": {"name": ss.SERVER_NAME, "id": "SUITEID"},
        "result": "pass" if failure is None else f"failed: {failure}",
        "programme": {"codec": "AC-3", "bursts": sent},
        "players": [
            {
                "label": "C6",
                "paired_by": "code",
                "failure": player_failure,
                "counters": {
                    "bursts_played": 938,
                    "underruns": 0,
                    "late_chunks": 0,
                    "dropped_chunks": 0,
                    "invalid_chunks": 0,
                    **counters,
                },
            }
        ],
    }


def fake_popen(data: dict[str, Any] | None, code: int | None = 0, seen: list | None = None):
    """A Popen that writes `data` to the --report path it was given, and records the argv and
    the pairing codes the relay had written by the time it was asked to wait."""

    def popen(argv: list[str], **kwargs: Any) -> FakeProcess:
        entry: dict[str, Any] = {"argv": list(argv)}
        if seen is not None:
            seen.append(entry)
        if data is not None:
            Path(argv[argv.index("--report") + 1]).write_text(json.dumps(data), "utf-8")
        process = FakeProcess(code)
        wait = process.wait

        def waiting(timeout: float | None = None) -> int:
            if "--code-log" in argv:
                time.sleep(0.15)  # the relay's tick is 10 ms
                log = Path(argv[argv.index("--code-log") + 1])
                entry["codes"] = log.read_text("utf-8") if log.is_file() else ""
            return wait(timeout)

        process.wait = waiting  # type: ignore[method-assign]
        return process

    return popen


SERVER = Path("hearth-testserver")
STATE = Path("state-dir")
PROGRAMME = Path("ffmpeg.ac3")


class Parsing(unittest.TestCase):
    def test_the_totals_line_gives_its_counters_and_its_kind(self) -> None:
        got = ss.parse_stream_end("noise\n" + STREAM_LINE + "\nsendspin.rms[0]=81651\n")
        assert got is not None
        self.assertEqual(got["kind"], "bursts")
        self.assertEqual(got["bursts"], 938)
        self.assertEqual(got["burst_us"], 21111)
        self.assertEqual(got["worst_error_us"], -362)
        self.assertEqual(got["heap_free"], 52560)
        self.assertNotIn("sendspin.stream", got)

    def test_the_last_totals_line_wins_and_none_is_none(self) -> None:
        text = STREAM_LINE + "\n" + EAC3_LINE + "\n"
        got = ss.parse_stream_end(text)
        assert got is not None
        self.assertEqual(got["burst_us"], 24217)
        self.assertIsNone(ss.parse_stream_end("sendspin.progress chunks=125 late=0\n"))

    def test_a_pcm_stream_is_its_own_kind(self) -> None:
        got = ss.parse_stream_end(STREAM_LINE.replace("=bursts bursts=", "=pcm bursts="))
        assert got is not None
        self.assertEqual(got["kind"], "pcm")

    def test_the_pairing_table_is_ids_and_names(self) -> None:
        table = StubBoard(table={"A": "Hearth live test", "B": ss.SERVER_NAME}).pairing()
        ids = ss.pairing_ids(table)
        self.assertEqual(ids, {"A": "Hearth live test", "B": ss.SERVER_NAME})
        self.assertEqual(ss.ours_in(ids), ["B"])

    def test_digits_of_a_code(self) -> None:
        self.assertEqual(ss.digits_of("482-913"), "482913")
        self.assertEqual(ss.digits_of(""), "")


class Command(unittest.TestCase):
    def argv(self, **kwargs: Any) -> list[str]:
        return ss.command(
            SERVER, STATE, PROGRAMME, Path("r.json"), Path("c.txt"), "192.168.1.117", 80, "C6", 60,
            **kwargs,
        )  # fmt: skip

    def test_the_server_dials_the_board_with_the_suite_s_identity_and_name(self) -> None:
        argv = self.argv()
        self.assertEqual(argv[argv.index("--player") + 1], "ws://192.168.1.117:8928/sendspin")
        self.assertEqual(argv[argv.index("--state") + 1], str(STATE))
        self.assertEqual(argv[argv.index("--name") + 1], ss.SERVER_NAME)
        self.assertEqual(argv[argv.index("--play") + 1], str(PROGRAMME))

    def test_a_short_file_is_played_over_and_over_for_so_long(self) -> None:
        argv = self.argv(seconds=20)
        self.assertEqual(argv[argv.index("--seconds") + 1], "20")
        self.assertNotIn("--seconds", self.argv())

    def test_a_tone_is_played_as_pcm_for_so_long_and_names_no_file(self) -> None:
        argv = self.argv(seconds=90, tone=True)
        self.assertIn("--pcm", argv)
        self.assertIn("--tone", argv)
        self.assertEqual(argv[argv.index("--seconds") + 1], "90")
        self.assertNotIn("--play", argv)
        self.assertNotIn("--pcm", self.argv())

    def test_a_board_that_holds_the_record_is_not_asked_to_pair_and_the_board_is_not_polled(
        self,
    ) -> None:
        self.assertIn("--code-log", self.argv(use_code=True))
        self.assertNotIn("--code-log", self.argv(use_code=False))
        # No GET /status a second while the board plays: its HTTP server answers from the same
        # heap the C6's E-AC-3 decoder has 2.6 KB of.
        self.assertNotIn("--status", self.argv())
        self.assertIn("--status", self.argv(poll_status=True))


class Relay(unittest.TestCase):
    def run_relay(self, codes: list[str], stale: str = "", seconds: float = 0.3) -> str:
        board = StubBoard(codes=codes)
        stop = threading.Event()
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "codes.txt"
            path.touch()
            thread = threading.Thread(
                target=ss.relay_codes, args=(board, path, stop, 0.01, stale, seconds)
            )
            thread.start()
            time.sleep(0.12)
            stop.set()
            thread.join(2)
            return path.read_text("utf-8")

    def test_a_code_is_written_as_the_console_prints_it_and_again_while_shown(self) -> None:
        text = self.run_relay(["", "", "482913"])
        self.assertIn("sendspin: PAIRING CODE 482-913\n", text)
        self.assertGreater(text.count("PAIRING CODE"), 1)

    def test_the_code_a_board_kept_from_before_the_run_is_not_written(self) -> None:
        self.assertEqual(self.run_relay(["304185"], stale="304185"), "")

    def test_a_short_value_is_not_a_code(self) -> None:
        self.assertEqual(self.run_relay(["12"]), "")


class Playing(unittest.TestCase):
    def play(self, board: StubBoard, popen: Any, **kwargs: Any) -> ss.SendspinRun:
        return ss.run_row(
            board, SERVER, STATE, PROGRAMME, "C6", popen=popen, relay_every=0.01, log_wait_s=0.0,
            **kwargs,
        )  # fmt: skip

    def test_a_played_row_has_the_servers_report_and_the_boards_totals(self) -> None:
        board = StubBoard()
        seen: list[dict[str, Any]] = []
        run = self.play(board, fake_popen(report(), 0, seen))
        self.assertEqual(run.exit_code, 0)
        self.assertEqual(run.server_id, "SUITEID")
        assert run.stream is not None
        self.assertEqual(run.stream["bursts"], 938)
        self.assertEqual(run.after["decode_stack_free"], 10904)
        self.assertEqual(ss.judge(run, {"units": 938}), [])
        # the relay put the code the board showed in the file the server reads
        self.assertIn("PAIRING CODE 482-913", seen[0]["codes"])

    def test_without_the_code_log_there_is_no_relay(self) -> None:
        board = StubBoard()
        seen: list[dict[str, Any]] = []
        self.play(board, fake_popen(report(), 0, seen), use_code=False)
        self.assertNotIn("--code-log", seen[0]["argv"])
        self.assertNotIn("codes", seen[0])

    def test_a_run_that_does_not_end_is_killed_and_said(self) -> None:
        process = FakeProcess(None)
        run = self.play(StubBoard(), lambda argv, **kw: process, timeout_s=0.01)
        self.assertTrue(process.killed)
        self.assertIsNone(run.exit_code)
        self.assertTrue(any("did not end" in f for f in ss.judge(run, {})))

    def test_a_console_shorter_than_where_it_was_is_a_reboot(self) -> None:
        board = StubBoard(log_offsets=(500, 120))
        run = self.play(board, fake_popen(report(), 0))
        self.assertTrue(run.rebooted)
        self.assertTrue(any("rebooted" in f for f in ss.judge(run, {"units": 938})))

    def test_a_server_that_cannot_be_started_is_an_error_not_a_finding(self) -> None:
        def broken(argv: list[str], **kwargs: Any) -> Any:
            raise OSError("no such file")

        with self.assertRaises(ss.SendspinError):
            self.play(StubBoard(), broken)


class Judging(unittest.TestCase):
    def run_of(
        self,
        *,
        code: int = 0,
        line: str | None = STREAM_LINE,
        failure: str | None = None,
        player_failure: str | None = None,
        **rep: Any,
    ) -> ss.SendspinRun:
        board = StubBoard(log_text=(line or "") + "\n")
        return ss.run_row(
            board, SERVER, STATE, PROGRAMME, "C6",
            popen=fake_popen(report(failure, player_failure, **rep), code),
            relay_every=0.01, log_wait_s=0.0,
        )  # fmt: skip

    def test_a_clean_row_has_no_findings(self) -> None:
        self.assertEqual(ss.judge(self.run_of(), {"units": 938}), [])

    def test_exit_three_names_what_the_player_said(self) -> None:
        run = ss.SendspinRun(exit_code=3, report=report(failure="C6 ran out of audio 3 time(s)"))
        findings = ss.judge(run, {})
        self.assertEqual(len(findings), 1)
        self.assertIn("went away", findings[0])
        self.assertIn("ran out of audio", findings[0])

    def test_exit_two_is_a_board_that_did_not_pair(self) -> None:
        findings = ss.judge(ss.SendspinRun(exit_code=2), {})
        self.assertIn("did not connect, pair or take its settings", findings[0])

    def test_no_totals_line_is_a_finding(self) -> None:
        findings = ss.judge(self.run_of(line=None), {"units": 938})
        self.assertTrue(any("no sendspin.stream= line" in f for f in findings))

    def test_late_dropped_invalid_and_underruns_are_glitches_not_findings(self) -> None:
        # 938 sent: 935 played, 2 late, 1 dropped
        line = STREAM_LINE.replace("bursts=938", "bursts=935")
        line = line.replace("late=0", "late=2").replace("dropped=0", "dropped=1")
        line = line.replace("invalid=0", "invalid=4").replace("underruns=0", "underruns=3")
        line = line.replace("resyncs=0", "resyncs=2")
        run = self.run_of(line=line)
        self.assertEqual(ss.judge(run, {"units": 938}), [])
        text = " | ".join(ss.glitches(run))
        for part in (
            "2 late chunk", "1 dropped chunk", "4 invalid chunk", "3 sink underrun",
            "2 resynchronisation",
        ):  # fmt: skip
            self.assertIn(part, text)
        self.assertEqual(ss.glitches(self.run_of()), [])

    def test_a_stream_that_ran_the_board_dry_and_played_through_is_a_glitch(self) -> None:
        # exit 3 with its failure "ran out of audio" and every burst played
        run = self.run_of(code=3, failure="C6 ran out of audio 1 time(s)")
        self.assertEqual(ss.judge(run, {"units": 938}), [])

    def test_the_real_report_has_the_runs_failure_in_result_not_in_the_player(self) -> None:
        # 2026-10-12, found by a real run on the C6: main.cpp's finish() writes the run's failure
        # as the top-level `result`, and the player's `failure` stays null for it.
        data = report("C6 ran out of audio 1 time(s)")
        self.assertEqual(data["result"], "failed: C6 ran out of audio 1 time(s)")
        self.assertIsNone(data["players"][0]["failure"])
        run = self.run_of(code=3, failure="C6 ran out of audio 1 time(s)")
        self.assertEqual(run.failure(), "C6 ran out of audio 1 time(s)")
        self.assertEqual(ss.judge(run, {"units": 938}), [])

    def test_a_pass_has_no_failure(self) -> None:
        self.assertEqual(self.run_of().failure(), "")

    def test_a_players_own_failure_is_never_a_glitch(self) -> None:
        run = self.run_of(
            code=3,
            player_failure="was displaced by another server",
            failure="C6 ran out of audio 1 time(s)",
        )
        findings = ss.judge(run, {"units": 938})
        self.assertEqual(len(findings), 1)
        self.assertIn("displaced", findings[0])

    def test_late_chunks_the_server_names_are_a_glitch_when_every_burst_was_played(self) -> None:
        text = "C6 did not play every burst: 2 late, 0 dropped with no room for them"
        line = STREAM_LINE.replace("bursts=938", "bursts=936").replace("late=0", "late=2")
        run = self.run_of(code=3, line=line, failure=text)
        self.assertEqual(ss.judge(run, {"units": 938}), [])
        self.assertEqual(ss.glitches(run), ["2 late chunk(s)"])

    def test_an_unaccounted_burst_is_a_finding_even_when_the_count_is_right(self) -> None:
        run = self.run_of(code=3, failure="C6 did not account for every burst sent")
        findings = ss.judge(run, {"units": 938})
        self.assertEqual(len(findings), 1)
        self.assertIn("did not account for every burst", findings[0])

    def test_a_stream_that_stopped_short_is_a_finding_whatever_it_says(self) -> None:
        line = STREAM_LINE.replace("bursts=938", "bursts=170")
        run = self.run_of(code=3, line=line, failure="C6 ran out of audio 8 time(s)")
        self.assertIn("170 bursts played, expected 938", ss.judge(run, {"units": 938}))
        gone = self.run_of(code=3, line=None, failure="C6 went away")
        self.assertTrue(any("went away" in f for f in ss.judge(gone, {"units": 938})))

    def test_a_looped_stream_is_held_to_the_bursts_the_server_sent(self) -> None:
        # a three-frame AC-4 file played for 20 s: how many bursts that is, is the report's
        spec = {"units": 3, "sendspin_seconds": 20}
        board = StubBoard(log_text=STREAM_LINE.replace("bursts=938", "bursts=470") + "\n")
        run = ss.run_row(
            board, SERVER, STATE, PROGRAMME, "S3a",
            popen=fake_popen(report(sent=470), 0), relay_every=0.01, log_wait_s=0.0,
            seconds=20,
        )  # fmt: skip
        self.assertEqual(ss.judge(run, spec), [])
        short = StubBoard(log_text=STREAM_LINE.replace("bursts=938", "bursts=460") + "\n")
        run = ss.run_row(
            short, SERVER, STATE, PROGRAMME, "S3a",
            popen=fake_popen(report(sent=470), 0), relay_every=0.01, log_wait_s=0.0,
            seconds=20,
        )  # fmt: skip
        self.assertIn("460 bursts played, expected 470", ss.judge(run, spec))

    def test_late_bursts_are_accounted_for_and_a_lost_one_is_not(self) -> None:
        # P4a, 2026-10-12: 932 played and 6 late of 938 sent, the server's own "did not play every
        # burst: 6 late": a glitch, not a stream that lost data.
        line = STREAM_LINE.replace("bursts=938", "bursts=932").replace("late=0", "late=6")
        run = self.run_of(
            code=3, line=line, failure="B did not play every burst: 6 late, 0 dropped"
        )
        self.assertEqual(ss.judge(run, {"units": 938}), [])
        self.assertEqual(ss.accounted(run.stream or {}), 938)
        # two of them neither played nor late: lost
        lost = STREAM_LINE.replace("bursts=938", "bursts=930").replace("late=0", "late=6")
        findings = ss.judge(self.run_of(line=lost), {"units": 938})
        self.assertIn("930 bursts played, expected 938 (6 late, 0 dropped)", findings)

    def test_every_burst_of_the_stream_is_played(self) -> None:
        line = STREAM_LINE.replace("bursts=938", "bursts=900")
        findings = ss.judge(self.run_of(line=line), {"units": 938})
        self.assertIn("900 bursts played, expected 938", findings)

    def test_a_pcm_stream_is_not_the_rows_stream(self) -> None:
        line = STREAM_LINE.replace("=bursts bursts=", "=pcm bursts=")
        findings = ss.judge(self.run_of(line=line), {"units": 938})
        self.assertTrue(any("not bursts" in f for f in findings))


class Figures(unittest.TestCase):
    def test_the_decoders_time_is_the_bursts_over_its_period(self) -> None:
        run = ss.SendspinRun(exit_code=0, stream=ss.parse_stream_end(STREAM_LINE))
        figures = ss.figures(run, 32000)
        self.assertEqual(figures["decoder_permille"], 660)  # 21,111 us of 32,000
        self.assertEqual(figures["frames"], 938)
        self.assertEqual(figures["play_heap_free"], 52560)
        self.assertEqual(figures["ring_high"], 35260)
        self.assertEqual(figures["worst_error_us"], -362)

    def test_the_stack_is_the_reports_least_where_the_server_polled_else_the_boards_after(
        self,
    ) -> None:
        run = ss.SendspinRun(
            stream=ss.parse_stream_end(STREAM_LINE), after={"decode_stack_free": 10904}
        )
        self.assertEqual(ss.figures(run, 32000)["decode_stack_free"], 10904)
        run.report = {"players": [{"status": {"decode_stack_free": 11144}}]}
        self.assertEqual(ss.figures(run, 32000)["decode_stack_free"], 11144)


class Table(unittest.TestCase):
    def play(self, board: StubBoard, popens: list[Any]) -> tuple[ss.SendspinRun, ss.Pairing]:
        pairing = ss.Pairing(board)
        iterator = iter(popens)

        def popen(argv: list[str], **kw: Any) -> Any:
            return next(iterator)(argv, **kw)

        run = ss.play_row(
            board, SERVER, STATE, PROGRAMME, "C6", pairing,
            popen=popen, relay_every=0.01, log_wait_s=0.0,
        )  # fmt: skip
        return run, pairing

    def test_a_board_without_the_record_is_paired_by_a_code_and_the_record_is_forgotten(
        self,
    ) -> None:
        board = StubBoard()
        seen: list[dict[str, Any]] = []
        run, pairing = self.play(board, [fake_popen(report(), 0, seen)])
        self.assertIn("--code-log", seen[0]["argv"])
        self.assertEqual(pairing.server_id, "SUITEID")
        board.table["SUITEID"] = ss.SERVER_NAME  # the board stored it while the server played
        self.assertIn("forgot", pairing.close())
        self.assertEqual(board.forgotten, ["SUITEID"])
        self.assertEqual(run.notes, [])

    def test_a_board_that_holds_the_record_is_played_by_it(self) -> None:
        board = StubBoard(table={"HEARTHID": "Hearth live test", "OLD": ss.SERVER_NAME})
        seen: list[dict[str, Any]] = []
        self.play(board, [fake_popen(report(), 0, seen)])
        self.assertNotIn("--code-log", seen[0]["argv"])
        self.assertEqual(seen[0]["argv"][seen[0]["argv"].index("--timeout") + 1], "25")

    def test_a_record_the_board_does_not_take_is_forgotten_and_the_row_played_by_a_code(
        self,
    ) -> None:
        board = StubBoard(table={"OLD": ss.SERVER_NAME})
        first: list[dict[str, Any]] = []
        second: list[dict[str, Any]] = []
        run, _ = self.play(board, [fake_popen(None, 2, first), fake_popen(report(), 0, second)])
        self.assertNotIn("--code-log", first[0]["argv"])
        self.assertIn("--code-log", second[0]["argv"])
        self.assertEqual(board.forgotten, ["OLD"])
        self.assertEqual(run.exit_code, 0)
        self.assertEqual(len(run.notes), 1)
        self.assertEqual(ss.judge(run, {"units": 938}), [])  # a note, not a finding

    def test_the_owner_s_records_are_never_forgotten(self) -> None:
        board = StubBoard(table={"HEARTHID": "Hearth live test", "MA": "Music Assistant"})
        pairing = ss.Pairing(board)
        pairing.server_id = "HEARTHID"
        # present before the run, and not the suite's name: kept
        self.assertIn("was there before", pairing.close())
        self.assertEqual(board.forgotten, [])

    def test_a_leftover_of_an_interrupted_run_is_the_suite_s_by_its_name_and_goes(self) -> None:
        board = StubBoard(table={"HEARTHID": "Hearth live test", "LEFT": ss.SERVER_NAME})
        pairing = ss.Pairing(board)
        pairing.server_id = "LEFT"
        self.assertIn("forgot", pairing.close())
        self.assertEqual(board.forgotten, ["LEFT"])

    def test_keep_pairing_leaves_the_record_and_an_unknown_id_forgets_nothing(self) -> None:
        board = StubBoard()
        pairing = ss.Pairing(board, keep=True)
        pairing.server_id = "SUITEID"
        board.table["SUITEID"] = ss.SERVER_NAME
        self.assertIn("kept", pairing.close())
        self.assertEqual(board.forgotten, [])
        self.assertIn("never learned", ss.Pairing(StubBoard()).close())


class Locating(unittest.TestCase):
    def test_the_explicit_path_then_the_environment_then_the_bench(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            binary = root / "suite-testserver" / "bin" / "hearth-testserver.exe"
            binary.parent.mkdir(parents=True)
            binary.write_text("", "utf-8")
            other = root / "elsewhere.exe"
            other.write_text("", "utf-8")
            with mock.patch.dict("os.environ", {}, clear=False):
                self.assertEqual(ss.find_testserver(str(other), root), other)
                self.assertEqual(ss.find_testserver(None, root), binary)
            with mock.patch.dict("os.environ", {"ICLFORGE_TESTSERVER": str(other)}):
                self.assertEqual(ss.find_testserver(None, root), other)

    def test_none_found_says_how_to_get_one(self) -> None:
        with (
            tempfile.TemporaryDirectory() as scratch,
            mock.patch.dict("os.environ", {"ICLFORGE_TESTSERVER": ""}),
            self.assertRaises(ss.SendspinError) as caught,
        ):
            ss.find_testserver(None, Path(scratch))
        self.assertIn("--testserver", str(caught.exception))

    def test_the_state_is_beside_the_locks(self) -> None:
        self.assertEqual(
            ss.default_state(Path("D:/ac3bld")), Path("D:/ac3bld/suite-sendspin-state")
        )


class InTheSuite(unittest.TestCase):
    """What board_suite does with a row that is played over Sendspin."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.suite = bs.load_suite()

    def row(self, id_: str = "ac3-2.0-sendspin") -> dict[str, Any]:
        for row in bs.plan(self.suite, "c6-16mb"):
            if row["id"] == id_:
                return row
        raise AssertionError(id_)

    def context(self, board: StubBoard, popen: Any = None) -> bs.SendspinContext:
        return bs.SendspinContext(
            SERVER, STATE, ss.Pairing(board), "C6", 30.0,
            popen=popen, relay_every=0.01, log_wait_s=0.0,
        )  # fmt: skip

    def test_the_c6_s_list_plays_ac3_and_then_e_ac3_over_sendspin(self) -> None:
        ids = [r["id"] for r in bs.plan(self.suite, "c6-16mb")]
        rows = {r["id"]: r.get("via", "play") for r in bs.plan(self.suite, "c6-16mb")}
        self.assertEqual(rows["ac3-2.0-sendspin"], "sendspin")
        self.assertEqual(rows["eac3-2.0-sendspin"], "sendspin")
        self.assertEqual(rows["ac3-2.0-refused"], "play")
        # As a user plays them: the E-AC-3 stream after the AC-3 one, in the same boot, which is
        # where its Wi-Fi receive buffers used to fail to allocate (planning/esp32-platform-
        # profiles.md, 'Sendspin rows').
        self.assertLess(ids.index("ac3-2.0-sendspin"), ids.index("eac3-2.0-sendspin"))

    def test_a_row_without_the_test_server_is_refused_with_how_to_get_one(self) -> None:
        result, _ = bs.run_sendspin_play(StubBoard(), self.row(), self.suite, None, None)  # type: ignore[arg-type]
        self.assertEqual(result.state, "refused")
        self.assertIn("--testserver", result.findings[0])

    def test_a_played_row_has_the_play_figures_and_the_sendspin_ones(self) -> None:
        board = StubBoard()
        result, _ = bs.run_sendspin_play(
            board,
            self.row(),
            self.suite,
            self.context(board, fake_popen(report(), 0)),
            None,  # type: ignore[arg-type]
        )
        self.assertEqual(result.findings, [])
        self.assertEqual(result.state, "finished")
        figures = result.figures()
        self.assertEqual(figures["frames"], 938)
        self.assertEqual(figures["units"], 938)
        self.assertEqual(figures["decoder_permille"], 660)
        self.assertNotIn("underruns", figures)  # a /play figure, and any is a regression
        self.assertEqual(figures["stream_underruns"], 0)
        self.assertEqual((figures["plays"], figures["glitched_plays"]), (1, 0))
        self.assertEqual(figures["decode_stack_free"], 10904)
        self.assertEqual(figures["via"], "sendspin")
        self.assertEqual(figures["play_heap_free"], 52560)

    def test_a_row_with_seconds_asks_the_server_to_loop_it(self) -> None:
        board = StubBoard(log_text=STREAM_LINE.replace("bursts=938", "bursts=470") + "\n")
        seen: list[dict[str, Any]] = []
        row = {
            "id": "ac4-20-music-sendspin",
            "stream": "ac4-20-music",
            "via": "sendspin",
            "stream_spec": {
                "file": "libs/ac4/fuzz/seeds/fuzz_ac4_parse/ac4-20-music-192-3frames.ac4",
                "codec": "AC-4",
                "units": 3,
                "sendspin_seconds": 20,
                "period_us": 42667,
            },
        }
        result, _ = bs.run_sendspin_play(
            board,
            row,
            self.suite,
            self.context(board, fake_popen(report(sent=470), 0, seen)),
            None,  # type: ignore[arg-type]
        )
        argv = seen[0]["argv"]
        self.assertEqual(argv[argv.index("--seconds") + 1], "20")
        self.assertEqual(result.findings, [])
        self.assertEqual(result.lines.units, 470)
        # 21,111 us of a burst against the stream's own 42,667 us period
        self.assertEqual(result.decoder_permille, round(21111 * 1000 / 42667))

    def test_a_row_that_rebooted_the_board_is_failed(self) -> None:
        board = StubBoard(log_offsets=(500, 120))
        result, _ = bs.run_sendspin_play(
            board,
            self.row(),
            self.suite,
            self.context(board, fake_popen(report(), 3)),
            None,  # type: ignore[arg-type]
        )
        self.assertEqual(result.state, "failed")
        self.assertTrue(result.rebooted)
        self.assertTrue(result.findings)

    def test_repeated_plays_keep_the_worst_counters_and_the_least_heap(self) -> None:
        def result(heap: int, resyncs: int, burst: int) -> bs.PlayResult:
            out = bs.PlayResult(id="x", stream="ac3-2.0", expect="pass", state="finished")
            out.decoder_permille = round(burst / 32)
            out.extra = {"play_heap_free": heap, "resyncs": resyncs, "burst_us": burst, "late": 0}
            return out

        runs = [result(26000, 0, 21000), result(2600, 3, 24000), result(40000, 0, 22000)]
        for run, got in zip(runs, (936, 938, 938), strict=True):
            run.frames = got
            run.extra["via"] = "sendspin"
        median = bs._median(runs)
        self.assertEqual(median.frames, 938)  # not the first play's, which lost two to late
        self.assertEqual(median.extra["play_heap_free"], 2600)
        self.assertEqual(median.extra["resyncs"], 3)
        self.assertEqual(median.extra["burst_us"], 22000)


class Comparing(unittest.TestCase):
    BASE: ClassVar[dict[str, Any]] = {
        "profile": "c6-16mb",
        "boot": {},
        "plays": {
            "eac3-2.0-sendspin": {
                "state": "finished",
                "frames": 938,
                "units": 938,
                "decoder_permille": 757,
                "decode_stack_free": 10904,
                "underruns": 0,
                "resyncs": 0,
                "play_heap_free": 2616,
            },
            "ac3-2.0-sendspin": {
                "state": "finished",
                "frames": 938,
                "units": 938,
                "decoder_permille": 660,
                "decode_stack_free": 11144,
                "underruns": 0,
                "resyncs": 0,
                "play_heap_free": 52560,
            },
        },
    }
    TOLERANCE: ClassVar[dict[str, Any]] = {"play_heap_free_bytes": {"eac3-2.0-sendspin": 1024}}

    def run_with(self, play: str, **changes: Any) -> list[str]:
        run = json.loads(json.dumps(self.BASE))
        run["plays"][play].update(changes)
        regressions, _ = bs.compare(run, self.BASE, self.TOLERANCE)
        return regressions

    def test_the_baseline_holds_to_itself(self) -> None:
        self.assertEqual(bs.compare(self.BASE, self.BASE, self.TOLERANCE), ([], []))

    def test_plays_that_glitched_are_held_to_the_baseline_s_count_and_one_more(self) -> None:
        base = json.loads(json.dumps(self.BASE))
        base["plays"]["ac3-2.0-sendspin"].update(
            {"via": "sendspin", "plays": 5, "glitched_plays": 1}
        )
        run = json.loads(json.dumps(base))
        run["plays"]["ac3-2.0-sendspin"]["glitched_plays"] = 2
        self.assertEqual(bs.compare(run, base, {})[0], [])
        run["plays"]["ac3-2.0-sendspin"]["glitched_plays"] = 3
        found, _ = bs.compare(run, base, {})
        self.assertTrue(any("3 of 5 plays glitched, the baseline's 1 of 5" in f for f in found))
        found, _ = bs.compare(run, base, {"glitched_plays": 0})
        self.assertTrue(found)

    def test_the_glitch_allowance_can_be_named_by_row(self) -> None:
        base = json.loads(json.dumps(self.BASE))
        for play in ("ac3-2.0-sendspin", "eac3-2.0-sendspin"):
            base["plays"][play].update({"via": "sendspin", "plays": 5, "glitched_plays": 2})
        run = json.loads(json.dumps(base))
        for play in ("ac3-2.0-sendspin", "eac3-2.0-sendspin"):
            run["plays"][play]["glitched_plays"] = 4
        tolerance = {"glitched_plays": {"eac3-2.0-sendspin": 2}}
        found, _ = bs.compare(run, base, tolerance)
        # E-AC-3 may have two more (4 of 5 is inside); AC-3 has the default one more (4 is not)
        self.assertEqual(len(found), 1)
        self.assertIn("ac3-2.0-sendspin: 4 of 5 plays glitched", found[0])

    def test_a_sendspin_row_s_stream_underruns_are_not_a_plays_underruns(self) -> None:
        run = json.loads(json.dumps(self.BASE))
        run["plays"]["ac3-2.0-sendspin"].update(
            {"via": "sendspin", "underruns": 3, "plays": 5, "glitched_plays": 0}
        )
        self.assertEqual(bs.compare(run, self.BASE, {})[0], [])

    def test_the_heap_a_row_has_a_tolerance_for_is_held_and_one_without_is_not(self) -> None:
        # 2,616 down to 1,500 is past the 1,024 the E-AC-3 row is allowed
        self.assertTrue(
            any(
                "least internal heap" in f
                for f in self.run_with("eac3-2.0-sendspin", play_heap_free=1500)
            )
        )
        # inside it
        self.assertEqual(self.run_with("eac3-2.0-sendspin", play_heap_free=1700), [])
        # the AC-3 row has no tolerance named: its heap reads 26 to 52 KB from play to play
        self.assertEqual(self.run_with("ac3-2.0-sendspin", play_heap_free=26688), [])

    def test_the_burst_time_is_held_as_the_decoders_time_is(self) -> None:
        found = self.run_with("eac3-2.0-sendspin", decoder_permille=900)
        self.assertTrue(any("decoder takes 900 permille" in f for f in found))

    def test_a_tolerance_for_every_row_is_one_number(self) -> None:
        run = json.loads(json.dumps(self.BASE))
        run["plays"]["ac3-2.0-sendspin"]["play_heap_free"] = 100
        found, _ = bs.compare(run, self.BASE, {"play_heap_free_bytes": 1024})
        self.assertTrue(any("ac3-2.0-sendspin" in f and "heap" in f for f in found))


class OnTheBoard(unittest.TestCase):
    def test_a_bare_forget_is_not_sent(self) -> None:
        board = bs.Board("127.0.0.1")
        for empty in ("", "   "):
            with self.assertRaises(bs.SuiteError):
                board.forget(empty)


if __name__ == "__main__":
    unittest.main()
