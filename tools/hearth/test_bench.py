"""Unit tests for bench.py, the bench's table, locks and redaction (planning/esp32-board-bench.md).

stdlib unittest, as ci.yml's script-lint job runs it. No board, no serial port and no network:
the table is held to the profiles file, the locks are files in a temporary directory, and the
redaction is held to values that exist only in these tests.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import contextlib
import datetime
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench
import sink_profile

SSID = "Testnet-Alpha-9"
PASSWORD = "correct-horse-battery"

# The five lock files the rule of 2026-10-11 names.
RULE_LOCKS = {
    "p4-board.lock",
    "p4-board2.lock",
    "s3-board-156.lock",
    "s3-board-45.lock",
    "c6-board.lock",
}


def fragment_text() -> str:
    return (
        'CONFIG_ICLFORGE_EXAMPLE_WIFI_SSID="' + SSID + '"\n'
        'CONFIG_ICLFORGE_EXAMPLE_WIFI_PASSWORD="' + PASSWORD + '"\n'
    )


def temp_bench(root: Path, *, with_fragment: bool = True) -> Path:
    """A bench.json beside temporary locks, builds and network fragment; returns its path."""
    data = json.loads(bench.BENCH_FILE.read_text("utf-8"))
    data["locks"] = str(root / "locks")
    data["builds"] = str(root / "builds")
    data["wifi_fragment"] = str(root / "wifi.sdkconfig")
    (root / "locks").mkdir(exist_ok=True)
    (root / "builds").mkdir(exist_ok=True)
    if with_fragment:
        (root / "wifi.sdkconfig").write_text(fragment_text(), "latin-1")
    path = root / "bench.json"
    path.write_text(json.dumps(data), "utf-8")
    return path


def run_main(argv: list[str], environment: dict[str, str] | None = None) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with (
        mock.patch.dict(os.environ, environment or {}),
        contextlib.redirect_stdout(out),
        contextlib.redirect_stderr(err),
    ):
        code = bench.main(argv)
    return code, out.getvalue(), err.getvalue()


class Table(unittest.TestCase):
    """bench.json against the profiles file and the booking rule."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.table = bench.load(bench.BENCH_FILE)
        cls.profiles = sink_profile.load()

    def test_there_are_five_boards_with_the_rule_s_five_locks(self) -> None:
        self.assertEqual(len(self.table.boards), 5)
        self.assertEqual({b.lock.name for b in self.table.boards.values()}, RULE_LOCKS)

    def test_names_addresses_ports_locks_and_mac_tails_are_each_unique(self) -> None:
        for attribute in ("name", "host", "port", "lock", "mac_tail"):
            values = [getattr(b, attribute) for b in self.table.boards.values()]
            self.assertEqual(len(values), len(set(values)), attribute)

    def test_a_mac_tail_is_six_hex_digits_and_a_chip_is_one_this_firmware_builds_for(self) -> None:
        for board in self.table.boards.values():
            self.assertRegex(board.mac_tail, r"^[0-9a-f]{6}$", board.name)
            self.assertIn(board.chip, {"esp32s3", "esp32c6", "esp32p4"}, board.name)

    def test_an_s3_lock_is_named_for_its_address(self) -> None:
        for board in self.table.boards.values():
            if board.name.startswith("S3"):
                suffix = board.host.rsplit(".", 1)[1]
                self.assertEqual(board.lock.name, f"s3-board-{suffix}.lock", board.name)

    def test_every_board_a_profile_names_is_on_the_bench_with_the_profile_s_chip(self) -> None:
        for ident, spec in self.profiles["profiles"].items():
            for name in spec.get("boards", []):
                board = self.table.board(name)
                self.assertEqual(board.chip, spec["target"], f"{ident}/{name}")

    def test_a_board_that_is_not_on_the_bench_is_refused_by_name(self) -> None:
        with self.assertRaises(bench.BenchError) as raised:
            self.table.board("S3c")
        self.assertIn("S3a", str(raised.exception))

    def test_the_p4s_are_marked_for_the_restart_fault_and_the_p4b_takes_no_ota(self) -> None:
        for name in ("P4a", "P4b"):
            self.assertTrue(self.table.board(name).restart_kills_c6_link, name)
        self.assertFalse(self.table.board("P4b").ota)
        self.assertTrue(self.table.board("P4a").ota)
        self.assertEqual(self.table.board("P4b").after_flash, "watchdog-reset")

    def test_the_process_page_has_a_row_for_every_board_as_the_file_has_it(self) -> None:
        page = (bench.HERE.parents[1] / "planning" / "esp32-board-bench.md").read_text("utf-8")
        rows = {
            line.split("|")[1].strip(): line for line in page.splitlines() if line.startswith("| ")
        }
        for board in self.table.boards.values():
            row = rows.get(board.name)
            self.assertIsNotNone(row, f"{board.name} has no row in the page's table")
            for needed in (board.host, board.port, board.lock.name):
                self.assertIn(needed, row, f"{board.name}: {needed}")

    def test_no_network_value_is_in_the_file(self) -> None:
        text = bench.BENCH_FILE.read_text("utf-8").lower()
        for word in ("password=", '"ssid"', "psk"):
            self.assertNotIn(word, text)

    def test_another_machine_s_file_is_named_by_the_environment(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = temp_bench(Path(tmp))
            with mock.patch.dict(os.environ, {bench.ENVIRONMENT: str(path)}):
                other = bench.load()
            self.assertEqual(other.locks, Path(tmp) / "locks")

    def test_a_file_of_another_format_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bench.json"
            path.write_text('{"format": 2}', "utf-8")
            with self.assertRaises(bench.BenchError):
                bench.load(path)


class Locks(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lock = Path(self.tmp.name) / "board.lock"

    def test_a_lock_is_one_line_in_the_format_every_session_uses(self) -> None:
        line = bench.take_lock(self.lock, "16d036", "play the suite", 30, "board suite")
        fields = bench.parse_lock(line)
        self.assertEqual(fields["session"], "16d036")
        self.assertEqual(fields["what"], "board suite")
        self.assertEqual(fields["purpose"], "play the suite")
        self.assertRegex(fields["start"], r"^\d{4}-\d\d-\d\dT\d\d:\d\dZ$")
        self.assertRegex(fields["expected_end"], r"^\d{4}-\d\d-\d\dT\d\d:\d\dZ$")
        self.assertEqual(self.lock.read_text("utf-8"), line + "\n")

    def test_a_lock_that_exists_is_not_taken_over_even_by_its_own_session(self) -> None:
        bench.take_lock(self.lock, "16d036", "first", 30)
        with self.assertRaises(bench.BenchError):
            bench.take_lock(self.lock, "16d036", "second", 30)

    def test_hold_reuses_a_lock_that_is_this_session_s_and_leaves_it(self) -> None:
        bench.take_lock(self.lock, "16d036", "put an image on it", 30)
        line, took = bench.hold_lock(self.lock, "16d036", "suite", 30)
        self.assertFalse(took)
        self.assertIn("put an image on it", line)
        with self.assertRaises(bench.BenchError):
            bench.hold_lock(self.lock, "8996e5", "suite", 30)

    def test_a_session_id_that_is_a_prefix_of_another_does_not_hold_its_lock(self) -> None:
        bench.take_lock(self.lock, "16d036ab", "x", 30)
        with self.assertRaises(bench.BenchError):
            bench.release_lock(self.lock, "16d036")

    def test_release_removes_only_this_session_s_lock(self) -> None:
        bench.take_lock(self.lock, "16d036", "x", 30)
        with self.assertRaises(bench.BenchError):
            bench.release_lock(self.lock, "8996e5")
        self.assertTrue(self.lock.exists())
        self.assertEqual(bench.release_lock(self.lock, "16d036"), "released")
        self.assertFalse(self.lock.exists())
        self.assertEqual(bench.release_lock(self.lock, "16d036"), "no lock file")

    def test_require_lock_says_why_a_board_is_not_to_be_touched(self) -> None:
        board = bench.Board(
            "S3b",
            {"chip": "esp32s3", "host": "h", "port": "p", "lock": "s3.lock", "mac_tail": "000000"},
            Path(self.tmp.name),
        )
        with self.assertRaises(bench.BenchError) as none:
            bench.require_lock(board, "16d036")
        self.assertIn("not booked", str(none.exception))
        bench.take_lock(board.lock, "877783", "matrix", 30)
        with self.assertRaises(bench.BenchError) as other:
            bench.require_lock(board, "16d036")
        self.assertIn("another session", str(other.exception))
        bench.release_lock(board.lock, "877783")
        bench.take_lock(board.lock, "16d036", "mine", 30)
        self.assertIn("mine", bench.require_lock(board, "16d036"))

    def test_overdue_is_minutes_past_the_expected_end_and_never_a_reason_to_take_over(self) -> None:
        now = datetime.datetime(2026, 10, 12, 6, 0, tzinfo=datetime.timezone.utc)
        late = {"expected_end": "2026-10-12T05:35Z"}
        self.assertEqual(bench.overdue_minutes(late, now), 25)
        self.assertIsNone(bench.overdue_minutes({"expected_end": "2026-10-12T06:30Z"}, now))
        self.assertIsNone(bench.overdue_minutes({"expected_end": "soon"}, now))
        self.assertIsNone(bench.overdue_minutes({}, now))


class Status(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = {bench.ENVIRONMENT: str(temp_bench(self.root))}

    def test_boards_prints_the_table(self) -> None:
        code, out, _ = run_main(["boards"], self.env)
        self.assertEqual(code, 0)
        for name in ("S3a", "S3b", "C6", "P4a", "P4b"):
            self.assertIn(name, out)
        self.assertIn("no OTA", out)

    def test_status_reads_all_five_locks_and_flags_an_overdue_one(self) -> None:
        (self.root / "locks" / "p4-board.lock").write_text(
            "session=32ab95 (P4 restart) | purpose=link | start=2026-10-12T03:22Z | "
            "expected_end=2026-10-12T05:35Z\n",
            "utf-8",
        )
        bench.take_lock(self.root / "locks" / "s3-board-45.lock", "16d036", "suite", 600)
        code, out, _ = run_main(["status"], self.env)
        self.assertEqual(code, 0)
        self.assertIn("P4a  booked by 32ab95 (P4 restart): link", out)
        self.assertIn("OVERDUE", out)
        self.assertIn("do not take the board over", out)
        self.assertIn("S3b  booked by 16d036", out)
        self.assertIn("3 of 5 free", out)
        self.assertIn("ListAgents", out)

    def test_lock_take_show_release_by_board_name(self) -> None:
        argv = ["lock", "take", "S3b", "--session", "16d036", "--purpose", "baseline"]
        code, out, _ = run_main(argv, self.env)
        self.assertEqual(code, 0)
        self.assertIn("session=16d036 (bench)", out)
        self.assertTrue((self.root / "locks" / "s3-board-45.lock").is_file())
        code, _, err = run_main(argv, self.env)
        self.assertEqual(code, 1)
        self.assertIn("is booked", err)
        code, out, _ = run_main(["lock", "show", "S3b"], self.env)
        self.assertIn("16d036", out)
        code, out, _ = run_main(["lock", "release", "S3b", "--session", "16d036"], self.env)
        self.assertEqual(out.strip(), "released")

    def test_a_lock_without_a_session_or_a_purpose_is_refused(self) -> None:
        code, _, err = run_main(["lock", "take", "S3b"], self.env)
        self.assertEqual(code, 1)
        self.assertIn("--session", err)
        code, _, err = run_main(["lock", "take", "S3b", "--session", "16d036"], self.env)
        self.assertEqual(code, 1)
        self.assertIn("--purpose", err)

    def test_probe_reads_a_board_s_state_and_survives_one_that_does_not_answer(self) -> None:
        def get(host: str, path: str, timeout: float = 8.0):
            if host.endswith(".45"):
                return 200, {}, b'{"state": "stopped", "network": {"ssid": "x"}}'
            raise bench.BenchError(f"{host}: unreachable")

        with mock.patch.object(bench, "http_get", get):
            code, out, _ = run_main(["status", "--probe"], self.env)
        self.assertEqual(code, 0)
        self.assertIn("/status stopped", out)
        self.assertIn("unreachable", out)
        self.assertNotIn("ssid", out)


class Masking(unittest.TestCase):
    def setUp(self) -> None:
        self.values = {"<wifi-ssid>": SSID, "<wifi-password>": PASSWORD}

    def test_the_fragment_s_values_are_read_by_key_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "wifi.sdkconfig"
            path.write_text(fragment_text(), "latin-1")
            self.assertEqual(bench.wifi_values(path), self.values)
            self.assertEqual(bench.wifi_values(Path(tmp) / "absent"), {})

    def test_a_console_line_loses_the_ssid_wherever_it_is(self) -> None:
        line = f"I (644) wifi:connected with {SSID}, aid = 5, channel 1"
        masked = bench.mask_line(line, self.values)
        self.assertNotIn(SSID, masked)
        self.assertIn("<wifi-ssid>", masked)

    def test_the_value_is_found_json_escaped_percent_escaped_and_as_hex(self) -> None:
        value = 'a"b\\c'
        found = bench.variants(value)
        self.assertIn('a\\"b\\\\c', found)
        self.assertIn("%61%22%62%5C%63", found)
        self.assertIn("61 22 62 5c 63", found)
        self.assertIn("6122625c63", found)
        text = "/connect?ssid=" + "".join(f"%{b:02X}" for b in SSID.encode())
        self.assertNotIn("%", bench.mask_line(text, self.values).replace("%s", ""))

    def test_the_access_point_s_address_is_masked(self) -> None:
        line = "connected with x, aid = 5, bssid = 9a:2a:6f:44:82:46, channel 1"
        self.assertEqual(
            bench.mask_line(line, {}), "connected with x, aid = 5, bssid = <bssid>, channel 1"
        )
        ba = "<ba-add>idx:0 (ifx:0, 9a:2a:6f:44:82:46), tid:0"
        self.assertNotIn("9a:2a", bench.mask_line(ba, {}))

    def test_a_line_with_a_pairing_token_is_dropped_whole(self) -> None:
        line = "sendspin: pairing token AAAA-BBBB-CCCC"
        self.assertEqual(bench.mask_line(line, {}), "<line with a pairing token or key dropped>")

    def test_a_keyed_value_is_redacted_whether_or_not_it_was_in_the_fragment(self) -> None:
        masked = bench.mask_line("provision: ssid=SomeoneElsesNet password:hunter2", {})
        self.assertNotIn("SomeoneElsesNet", masked)
        self.assertNotIn("hunter2", masked)

    def test_an_ordinary_line_is_left_alone(self) -> None:
        line = "sink.writes=5628 sink.underruns=0 sink.dry_ms=0"
        self.assertEqual(bench.mask_line(line, self.values), line)

    def test_redact_replaces_by_key_and_never_reads_the_value(self) -> None:
        document = {
            "state": "stopped",
            "network": {"ssid": SSID, "bssid": "aa", "address": "192.168.1.45"},
            "list": [{"password": PASSWORD, "kept": 1}],
            "client_token": "t",
        }
        redacted = bench.redact(document)
        self.assertEqual(redacted["state"], "stopped")
        self.assertEqual(redacted["network"]["ssid"], "<redacted>")
        self.assertEqual(redacted["network"]["address"], "192.168.1.45")
        self.assertEqual(redacted["list"][0], {"password": "<redacted>", "kept": 1})
        self.assertEqual(redacted["client_token"], "<redacted>")
        self.assertNotIn(SSID, json.dumps(redacted))


class Commands(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = {bench.ENVIRONMENT: str(temp_bench(self.root))}

    def test_log_prints_a_board_s_console_without_its_network(self) -> None:
        body = (
            f"I (644) wifi:connected with {SSID}, aid = 5\nsendspin: pairing token X\nline 3\n"
        ).encode()
        with mock.patch.object(bench, "http_get", return_value=(200, {}, body)):
            code, out, err = run_main(["log", "S3b"], self.env)
        self.assertEqual(code, 0)
        self.assertNotIn(SSID, out + err)
        self.assertIn("<wifi-ssid>", out)
        self.assertIn("line 3", out)
        self.assertNotIn("pairing token X", out)

    def test_log_tail_is_the_last_lines(self) -> None:
        body = "".join(f"line {n}\n" for n in range(10)).encode()
        with mock.patch.object(bench, "http_get", return_value=(200, {}, body)):
            _, out, _ = run_main(["log", "S3b", "--tail", "2"], self.env)
        self.assertEqual(out.split("\n")[:2], ["line 8", "line 9"])

    def test_snapshot_redacts_by_key(self) -> None:
        body = json.dumps({"state": "stopped", "network": {"ssid": SSID}}).encode()
        with mock.patch.object(bench, "http_get", return_value=(200, {}, body)):
            code, out, _ = run_main(["snapshot", "S3b"], self.env)
        self.assertEqual(code, 0)
        self.assertNotIn(SSID, out)
        self.assertIn("<redacted>", out)

    def test_scrub_check_counts_and_never_prints_the_value(self) -> None:
        leaky = self.root / "boot.log"
        leaky.write_text(f"connected with {SSID}\npassword {PASSWORD}\n", "utf-8")
        clean = self.root / "clean.log"
        clean.write_text("nothing here\n", "utf-8")
        code, out, err = run_main(["scrub", "--check", str(leaky), str(clean)], self.env)
        self.assertEqual(code, 1)
        self.assertIn("2 occurrence(s)", out)
        self.assertIn("0 occurrence(s)", out)
        self.assertNotIn(SSID, out + err)
        self.assertNotIn(PASSWORD, out + err)
        code, _, _ = run_main(["scrub", "--check", str(clean)], self.env)
        self.assertEqual(code, 0)

    def test_scrub_mask_rewrites_the_file_in_place(self) -> None:
        leaky = self.root / "boot.log"
        leaky.write_text(f"connected with {SSID}\n", "utf-8")
        code, out, _ = run_main(["scrub", "--mask", str(leaky)], self.env)
        self.assertEqual(code, 0)
        self.assertIn("masked", out)
        self.assertEqual(leaky.read_text("utf-8"), "connected with <wifi-ssid>\n")

    def test_scrub_shape_gives_the_classes_and_not_the_value(self) -> None:
        code, out, _ = run_main(["scrub", "--shape"], self.env)
        self.assertEqual(code, 0)
        self.assertIn(f"'length': {len(SSID)}", out)
        self.assertNotIn(SSID, out)
        self.assertNotIn(PASSWORD, out)

    def test_scrub_with_no_fragment_says_so_rather_than_passing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            env = {bench.ENVIRONMENT: str(temp_bench(Path(tmp), with_fragment=False))}
            target = Path(tmp) / "x.log"
            target.write_text("x", "utf-8")
            code, _, err = run_main(["scrub", "--check", str(target)], env)
        self.assertEqual(code, 1)
        self.assertIn("not readable", err)

    def test_console_without_pyserial_says_how_to_get_it(self) -> None:
        with mock.patch.dict(sys.modules, {"serial": None}):
            code, _, err = run_main(["console", "S3b", "--seconds", "1"], self.env)
        self.assertEqual(code, 1)
        self.assertIn("pyserial", err)


if __name__ == "__main__":
    unittest.main()
