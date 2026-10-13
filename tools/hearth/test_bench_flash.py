"""Unit tests for bench_flash.py, the bench's writes (planning/esp32-board-bench.md).

stdlib unittest, as ci.yml's script-lint job runs it. No esptool, idf.py or board runs: the
runner is a recorder that answers with what esptool 5 and idf.py printed on the bench, so what
is held is the command lines (what would be written, to which port, after which check) and the
refusals (a board that is not booked, or not the one on the port, is written nothing).

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench
import bench_flash as bf
import sink_profile
from test_bench import NETWORK_KEY, SSID, temp_bench

S3_MAC_OUTPUT = """esptool v5.0
Connected to ESP32-S3 on COM16:
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
MAC:                28:84:85:47:b3:9c
"""

C6_MAC_OUTPUT = """Chip type:          ESP32-C6 (QFN40) (revision v0.1)
MAC:                ac:eb:e6:ff:fe:db:4c:40
BASE MAC:           ac:eb:e6:db:4c:40
MAC_EXT:            ff:fe
"""

# What a real build's flasher_args.json looks like, with the audio sample and the storage image
# that are in flash_files (D:\esp\build\iclforge\deploy-1010-esp32p4, 2026-10-12).
FLASHER_ARGS = {
    "flash_settings": {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
    "flash_files": {
        "0x2000": "bootloader/bootloader.bin",
        "0x8000": "partition_table/partition-table.bin",
        "0x10000": "ota_data_initial.bin",
        "0x20000": "iclforge_hearth_sink.bin",
        "0x830000": "stream/sample.ac3",
        "0x870000": "storage.bin",
    },
    "bootloader": {"offset": "0x2000", "file": "bootloader/bootloader.bin"},
    "partition-table": {"offset": "0x8000", "file": "partition_table/partition-table.bin"},
    "otadata": {"offset": "0x10000", "file": "ota_data_initial.bin"},
    "app": {"offset": "0x20000", "file": "iclforge_hearth_sink.bin"},
    "audio": {"offset": "0x830000", "file": "stream/sample.ac3"},
    "storage": {"offset": "0x870000", "file": "storage.bin"},
}


class Recorder:
    """A runner: records each (argv, cwd) and answers from `answers`, matched by a word."""

    def __init__(self, answers: dict[str, tuple[int, str]] | None = None) -> None:
        self.calls: list[tuple[list[str], Path | None, bool]] = []
        self.answers = answers or {}

    def __call__(
        self, argv: Sequence[str], cwd: Path | None = None, *, idf: bool = False
    ) -> tuple[int, str]:
        self.calls.append((list(argv), cwd, idf))
        joined = " ".join(argv)
        for word, answer in self.answers.items():
            if word in joined:
                return answer
        return 0, ""

    def commands(self, word: str) -> list[list[str]]:
        return [argv for argv, _, _ in self.calls if word in " ".join(argv)]

    def idf_flags(self, word: str) -> list[bool]:
        return [idf for argv, _, idf in self.calls if word in " ".join(argv)]


def namespace(**values: Any) -> argparse.Namespace:
    return argparse.Namespace(**values)


class Fixture(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.table = bench.load(temp_bench(self.root))
        self.output: list[str] = []
        self.build = self.root / "builds" / "t-s3-n16r8"
        self.build.mkdir(parents=True)
        (self.build / "flash_args").write_text("--flash-mode dio\n0x0 a.bin\n", "utf-8")
        (self.build / "flasher_args.json").write_text(json.dumps(FLASHER_ARGS), "utf-8")

    def out(self, text: str) -> None:
        self.output.append(text)

    def book(self, board: str, session: str = "16d036") -> None:
        bench.take_lock(self.table.board(board).lock, session, "test", 30)


class Environment(unittest.TestCase):
    def setUp(self) -> None:
        self.table = bench.load(bench.BENCH_FILE)

    def test_on_windows_the_command_runs_in_powershell_after_export_ps1(self) -> None:
        wrapped = bf.in_idf(self.table, ["idf.py", "-DX=a;b c", "it's"], windows=True)
        self.assertEqual(wrapped[1:3], ["-NoProfile", "-NonInteractive"])
        script = wrapped[-1]
        self.assertIn("IDF_TOOLS_PATH='D:/esp/tools'", script)
        self.assertIn(". 'D:/esp/esp-idf/export.ps1' *> $null", script)
        # one argument each, single-quoted, so the semicolons of SDKCONFIG_DEFAULTS are text
        self.assertIn("& 'idf.py' '-DX=a;b c' 'it''s'", script)

    def test_a_command_the_export_did_not_provide_is_127_and_never_a_silent_zero(self) -> None:
        script = bf.in_idf(self.table, ["idf.py", "build"], windows=True)[-1]
        self.assertIn("Get-Command 'idf.py'", script)
        self.assertIn("exit 127", script)
        # PowerShell leaves $LASTEXITCODE empty when nothing ran, and `exit` of that is 0
        self.assertIn("if ($null -eq $LASTEXITCODE) { exit 1 }; exit $LASTEXITCODE", script)

    def test_elsewhere_it_runs_in_bash_after_export_sh(self) -> None:
        wrapped = bf.in_idf(self.table, ["idf.py", "-DX=a;b c"], windows=False)
        self.assertEqual(wrapped[:2], ["bash", "-c"])
        self.assertIn("export.sh", wrapped[2])
        self.assertIn("exec idf.py '-DX=a;b c'", wrapped[2])

    def test_an_msys_shell_s_variables_are_not_passed_to_the_idf_environment(self) -> None:
        with mock.patch.dict(
            os.environ,
            {"MSYSTEM": "MINGW64", "MINGW_PREFIX": "/mingw64", "msys_x": "1", "KEEP": "yes"},
        ):
            clean = bf.clean_environment()
        self.assertEqual(clean["KEEP"], "yes")
        for gone in ("MSYSTEM", "MINGW_PREFIX", "msys_x"):
            self.assertNotIn(gone, clean)

    def test_the_real_runner_wraps_an_idf_command_and_leaves_another_alone(self) -> None:
        seen: list[tuple[list[str], Path | None, dict[str, str] | None]] = []

        def process(argv: Sequence[str], cwd: Path | None = None, env: Any = None):
            seen.append((list(argv), cwd, env))
            return 0, ""

        run = bf.make_runner(self.table)
        with (
            mock.patch.object(bf, "run_process", process),
            mock.patch.dict(os.environ, {"MSYSTEM": "MINGW64"}),
        ):
            run(["idf.py", "build"], None, idf=True)
            run(["git", "describe"], None)
        wrapped, plain = seen
        self.assertIn("idf.py", wrapped[0][-1])
        self.assertNotEqual(wrapped[0], ["idf.py", "build"])
        self.assertNotIn("MSYSTEM", wrapped[2] or {})
        self.assertEqual((wrapped[2] or {}).get("PYTHONUTF8"), "1")
        self.assertEqual(plain[0], ["git", "describe"])
        self.assertIsNone(plain[2])

    def test_a_program_that_is_not_there_is_127_with_its_name(self) -> None:
        code, text = bf.run_process(["definitely-not-a-program-iclforge"], None)
        self.assertEqual(code, 127)
        self.assertIn("definitely-not-a-program-iclforge", text)


class Reading(unittest.TestCase):
    def test_the_s3_and_the_p4_have_a_6_octet_mac_line(self) -> None:
        self.assertEqual(bf.read_mac(S3_MAC_OUTPUT), "28:84:85:47:b3:9c")

    def test_the_c6_s_8_octet_line_is_not_its_mac_the_base_mac_is(self) -> None:
        self.assertEqual(bf.read_mac(C6_MAC_OUTPUT), "ac:eb:e6:db:4c:40")

    def test_no_mac_is_none(self) -> None:
        self.assertIsNone(bf.read_mac("A fatal error occurred: Failed to connect"))

    def test_the_tail_is_the_last_three_octets(self) -> None:
        self.assertEqual(bf.mac_tail("28:84:85:47:b3:9c"), "47b39c")


class Plan(Fixture):
    def test_a_full_flash_is_the_builds_own_flash_args(self) -> None:
        self.assertEqual(bf.flash_plan(self.build, app_only=False), ["@flash_args"])

    def test_app_only_is_the_app_and_otadata_and_not_the_audio_or_the_storage(self) -> None:
        plan = bf.flash_plan(self.build, app_only=True)
        self.assertEqual(
            plan,
            [
                "--flash-mode",
                "dio",
                "--flash-freq",
                "80m",
                "--flash-size",
                "16MB",
                "0x10000",
                "ota_data_initial.bin",
                "0x20000",
                "iclforge_hearth_sink.bin",
            ],
        )
        for unwanted in ("bootloader", "partition", "0x830000", "storage"):
            self.assertNotIn(unwanted, " ".join(plan))

    def test_otadata_only_is_the_otadata_and_not_the_app_or_the_rest(self) -> None:
        plan = bf.flash_plan(self.build, app_only=False, otadata_only=True)
        self.assertEqual(
            plan,
            [
                "--flash-mode",
                "dio",
                "--flash-freq",
                "80m",
                "--flash-size",
                "16MB",
                "0x10000",
                "ota_data_initial.bin",
            ],
        )

    def test_a_build_with_no_otadata_section_is_refused_for_otadata_only(self) -> None:
        broken = {k: v for k, v in FLASHER_ARGS.items() if k != "otadata"}
        (self.build / "flasher_args.json").write_text(json.dumps(broken), "utf-8")
        with self.assertRaises(bench.BenchError):
            bf.flash_plan(self.build, app_only=False, otadata_only=True)

    def test_a_build_with_no_app_section_is_refused(self) -> None:
        broken = {k: v for k, v in FLASHER_ARGS.items() if k != "app"}
        (self.build / "flasher_args.json").write_text(json.dumps(broken), "utf-8")
        with self.assertRaises(bench.BenchError):
            bf.flash_plan(self.build, app_only=True)

    def test_a_directory_with_no_build_is_refused(self) -> None:
        with self.assertRaises(bench.BenchError):
            bf.flash_plan(self.root, app_only=False)


class Flash(Fixture):
    def args(self, board: str = "S3b", **more: Any) -> argparse.Namespace:
        base = {
            "board": board,
            "build_dir": str(self.build),
            "session": "16d036",
            "app_only": False,
            "otadata_only": False,
            "erase": False,
        }
        return namespace(**{**base, **more})

    def flash(self, run: Recorder, args: argparse.Namespace, answers: Any = None) -> int:
        probe = answers or (lambda host: {"project": "iclforge_hearth_sink", "version": "v1"})
        return bf.cmd_flash(self.table, args, run, self.out, probe)

    def test_a_board_nobody_has_booked_is_written_nothing_and_not_even_read(self) -> None:
        run = Recorder()
        with self.assertRaises(bench.BenchError) as raised:
            self.flash(run, self.args())
        self.assertIn("not booked", str(raised.exception))
        self.assertEqual(run.calls, [])

    def test_a_board_booked_by_another_session_is_written_nothing(self) -> None:
        self.book("S3b", "877783")
        run = Recorder()
        with self.assertRaises(bench.BenchError) as raised:
            self.flash(run, self.args())
        self.assertIn("another session", str(raised.exception))
        self.assertEqual(run.calls, [])

    def test_the_wrong_chip_on_the_port_is_read_and_not_written(self) -> None:
        self.book("S3a")
        run = Recorder({"read-mac": (0, S3_MAC_OUTPUT)})  # S3b's MAC on S3a's port
        with self.assertRaises(bench.BenchError) as raised:
            self.flash(run, self.args(board="S3a"))
        self.assertIn("is not S3a", str(raised.exception))
        self.assertEqual(len(run.commands("read-mac")), 1)
        self.assertEqual(run.commands("write-flash"), [])
        self.assertEqual(run.commands("erase-flash"), [])

    def test_no_mac_read_is_not_a_reason_to_write(self) -> None:
        self.book("S3b")
        run = Recorder({"read-mac": (2, "Failed to connect")})
        with self.assertRaises(bench.BenchError):
            self.flash(run, self.args())
        self.assertEqual(run.commands("write-flash"), [])

    def test_the_right_board_is_written_from_its_build_dir_and_waited_for(self) -> None:
        self.book("S3b")
        run = Recorder({"read-mac": (0, S3_MAC_OUTPUT)})
        self.assertEqual(self.flash(run, self.args()), 0)
        (written,) = run.commands("write-flash")
        self.assertEqual(written[:4], ["python", "-m", "esptool", "--chip"])
        # esptool is the IDF environment's, not the system Python's
        flags = run.idf_flags("read-mac") + run.idf_flags("write-flash")
        self.assertEqual(flags, [True, True])
        joined = " ".join(written)
        self.assertIn("--chip esp32s3 --port COM16 --baud 460800", joined)
        self.assertIn("--before default-reset --after hard-reset write-flash @flash_args", joined)
        cwd = next(c for argv, c, _ in run.calls if "write-flash" in " ".join(argv))
        self.assertEqual(cwd, self.build)
        self.assertTrue(
            any("answers; project iclforge_hearth_sink" in line for line in self.output)
        )
        self.assertTrue((self.build / "flash-S3b.log").is_file())

    def test_the_p4b_is_started_by_a_watchdog_reset_and_only_its_app_is_written(self) -> None:
        self.book("P4b")
        run = Recorder({"read-mac": (0, "MAC:                30:ed:a0:ea:18:56\n")})
        self.assertEqual(self.flash(run, self.args(board="P4b", app_only=True)), 0)
        (read,) = run.commands("read-mac")
        self.assertIn("--after watchdog-reset read-mac", " ".join(read))
        (written,) = run.commands("write-flash")
        joined = " ".join(written)
        self.assertIn("--chip esp32p4 --port COM13", joined)
        self.assertIn("--after watchdog-reset write-flash", joined)
        self.assertIn("0x10000 ota_data_initial.bin 0x20000 iclforge_hearth_sink.bin", joined)
        self.assertNotIn("@flash_args", joined)

    def test_otadata_only_writes_that_one_file_by_watchdog_reset_on_a_p4(self) -> None:
        self.book("P4a")
        run = Recorder({"read-mac": (0, "MAC:                30:ed:a0:ea:6d:66\n")})
        code = self.flash(run, self.args(board="P4a", otadata_only=True))
        self.assertEqual(code, 0)
        (written,) = run.commands("write-flash")
        joined = " ".join(written)
        self.assertIn("--after watchdog-reset write-flash", joined)
        self.assertTrue(joined.endswith("0x10000 ota_data_initial.bin"), joined)
        self.assertNotIn("iclforge_hearth_sink.bin", joined)

    def test_otadata_only_is_not_combined_with_the_app_or_an_erase(self) -> None:
        self.book("P4a")
        for extra in ({"app_only": True}, {"erase": True}):
            run = Recorder({"read-mac": (0, "MAC:                30:ed:a0:ea:6d:66\n")})
            with self.assertRaises(bench.BenchError):
                self.flash(run, self.args(board="P4a", otadata_only=True, **extra))
            self.assertEqual(run.commands("write-flash"), [])

    def test_erase_comes_before_the_write_and_says_what_it_takes(self) -> None:
        self.book("S3b")
        run = Recorder({"read-mac": (0, S3_MAC_OUTPUT)})
        self.assertEqual(self.flash(run, self.args(erase=True)), 0)
        kinds = [
            "erase" if "erase-flash" in " ".join(argv) else "write"
            for argv, _, _ in run.calls
            if "erase-flash" in " ".join(argv) or "write-flash" in " ".join(argv)
        ]
        self.assertEqual(kinds, ["erase", "write"])
        self.assertTrue(any("pairing" in line for line in self.output))

    def test_a_failed_erase_stops_before_the_write(self) -> None:
        self.book("S3b")
        run = Recorder({"read-mac": (0, S3_MAC_OUTPUT), "erase-flash": (2, "failed")})
        with self.assertRaises(bench.BenchError):
            self.flash(run, self.args(erase=True))
        self.assertEqual(run.commands("write-flash"), [])

    def test_a_verified_write_whose_port_left_with_the_reset_is_not_a_failed_one(self) -> None:
        self.book("P4a")
        left = (
            "Wrote 8192 bytes (31 compressed) at 0x00010000 in 0.1 seconds\n"
            "Hash of data verified.\n\nHard resetting with a watchdog...\n\n"
            "ERROR: A serial exception error occurred: Cannot configure port, something went "
            "wrong. Original message: OSError(22, 'The I/O operation has been aborted')\n"
        )
        run = Recorder(
            {"read-mac": (0, "MAC:                30:ed:a0:ea:6d:66\n"), "write-flash": (1, left)}
        )
        self.assertEqual(self.flash(run, self.args(board="P4a", otadata_only=True)), 0)
        self.assertTrue(any("port left with it" in line for line in self.output))

    def test_the_same_exit_without_a_verified_write_is_a_failure(self) -> None:
        self.book("P4a")
        run = Recorder(
            {
                "read-mac": (0, "MAC:                30:ed:a0:ea:6d:66\n"),
                "write-flash": (
                    1,
                    "Hard resetting with a watchdog...\nERROR: A serial exception error occurred\n",
                ),
            }
        )
        self.assertEqual(self.flash(run, self.args(board="P4a", otadata_only=True)), 1)

    def test_a_failed_write_is_a_failure_and_a_silent_board_is_one_too(self) -> None:
        self.book("S3b")
        failing = Recorder({"read-mac": (0, S3_MAC_OUTPUT), "write-flash": (2, "A fatal error")})
        self.assertEqual(self.flash(failing, self.args()), 1)
        silent = Recorder({"read-mac": (0, S3_MAC_OUTPUT)})
        code = bf.cmd_flash(self.table, self.args(), silent, self.out, lambda host: None, wait=0.0)
        self.assertEqual(code, 1)
        self.assertTrue(any("did not answer" in line for line in self.output))

    def test_the_log_is_masked_before_it_is_written(self) -> None:
        self.book("S3b")
        run = Recorder(
            {"read-mac": (0, S3_MAC_OUTPUT), "write-flash": (0, f"connected with {SSID}")}
        )
        self.flash(run, self.args())
        log = (self.build / "flash-S3b.log").read_text("utf-8")
        self.assertNotIn(SSID, log)
        self.assertIn("<wifi-ssid>", log)


class Build(Fixture):
    def setUp(self) -> None:
        super().setUp()
        # what idf.py leaves behind when it built
        (self.build / "project_description.json").write_text("{}", "utf-8")

    def args(self, **more: Any) -> argparse.Namespace:
        base = {
            "profile": "s3-n16r8",
            "tag": "t",
            "worktree": None,
            "overlay": [],
            "wifi": False,
            "no_verify": False,
            "configure_only": False,
        }
        return namespace(**{**base, **more})

    def idf_build(self, run: Recorder) -> list[str]:
        return next(
            argv for argv, _, idf in run.calls if idf and argv[0] == "idf.py" and "build" in argv
        )

    def test_the_build_is_the_profile_s_with_its_identity_in_the_idf_environment(self) -> None:
        run = Recorder({"describe": (0, "v0.10.0-4175-g79d2e7618\n")})
        code = bf.cmd_build(self.table, self.args(), run, self.out)
        self.assertEqual(code, 0)
        build = self.idf_build(run)
        data = sink_profile.load()
        digest = sink_profile.profile_hash(data, "s3-n16r8")
        defaults = sink_profile.defaults_string(data, "s3-n16r8")
        self.assertIn(f"-DSDKCONFIG_DEFAULTS={defaults}", build)
        self.assertIn("-DIDF_TARGET=esp32s3", build)
        self.assertIn("-DICLFORGE_PROFILE=s3-n16r8", build)
        self.assertIn(f"-DICLFORGE_PROFILE_HASH={digest}", build)
        self.assertIn("-DPROJECT_VER=v0.10.0-4175-g79d2e7618", build)
        self.assertIn(str(self.build), build)
        self.assertFalse(any("wifi.sdkconfig" in word for word in build))

    def test_it_is_then_sized_and_held_to_the_profile_like_ci_does(self) -> None:
        run = Recorder({"describe": (0, "v1\n"), "json2": (0, '{"layout": []}')})
        self.assertEqual(bf.cmd_build(self.table, self.args(), run, self.out), 0)
        (sized,) = run.commands("json2")
        # given its project: it is not run from inside it, as CI's step is
        self.assertEqual(sized[:2], ["idf.py", "-C"])
        self.assertEqual(run.idf_flags("json2"), [True])
        (verified,) = run.commands("sink_profile.py")
        self.assertIn("verify s3-n16r8 --build-dir", " ".join(verified))
        self.assertIn("--size-json", verified)
        self.assertEqual(run.idf_flags("sink_profile.py"), [False])
        self.assertTrue((self.build / "size.json").is_file())

    def test_wifi_appends_the_fragment_and_the_overlay_and_prints_neither_value(self) -> None:
        run = Recorder(
            {
                "describe": (0, "v1\n"),
                "-DIDF_TARGET": (0, f"using {SSID} {NETWORK_KEY}"),
                "verify": (0, f"ok {SSID}"),
            }
        )
        code = bf.cmd_build(self.table, self.args(wifi=True), run, self.out)
        self.assertEqual(code, 0)
        defaults = next(w for w in self.idf_build(run) if w.startswith("-DSDKCONFIG_DEFAULTS="))
        self.assertTrue(defaults.endswith("wifi.sdkconfig"))
        (verified,) = run.commands("sink_profile.py")
        self.assertIn("--overlay=wifi-credentials", verified)
        text = "\n".join(self.output)
        log = (self.root / "builds" / "t-s3-n16r8.log").read_text("utf-8")
        for secret in (SSID, NETWORK_KEY):
            self.assertNotIn(secret, text)
            self.assertNotIn(secret, log)
        self.assertIn("<network fragment>", text)

    def test_a_failed_build_is_reported_with_its_tail_and_is_not_verified(self) -> None:
        run = Recorder({"describe": (0, "v1\n"), "-DIDF_TARGET": (1, "ninja: build stopped\n")})
        code = bf.cmd_build(self.table, self.args(), run, self.out)
        self.assertEqual(code, 1)
        self.assertEqual(run.commands("sink_profile.py"), [])
        self.assertTrue(any("ninja: build stopped" in line for line in self.output))
        log = (self.root / "builds" / "t-s3-n16r8.log").read_text("utf-8")
        self.assertIn("BUILD_EXIT=1", log)

    def test_an_exit_of_zero_that_left_no_build_is_a_failed_build(self) -> None:
        (self.build / "project_description.json").unlink()
        run = Recorder({"describe": (0, "v1\n")})
        self.assertEqual(bf.cmd_build(self.table, self.args(), run, self.out), 1)
        self.assertEqual(run.commands("sink_profile.py"), [])
        self.assertTrue(any("left no build" in line for line in self.output))

    def test_a_guard_finding_is_a_failed_build(self) -> None:
        run = Recorder(
            {"describe": (0, "v1\n"), "sink_profile.py": (1, "error: DECODE_STACK_IN_PSRAM is 0")}
        )
        self.assertEqual(bf.cmd_build(self.table, self.args(), run, self.out), 1)

    def test_configure_only_reconfigures_and_holds_the_guard_without_sizing(self) -> None:
        run = Recorder({"describe": (0, "v1\n")})
        code = bf.cmd_build(self.table, self.args(configure_only=True), run, self.out)
        self.assertEqual(code, 0)
        configured = next(
            argv for argv, _, idf in run.calls if idf and argv[0] == "idf.py" and "-B" in argv
        )
        self.assertEqual(configured[-1], "reconfigure")
        self.assertNotIn("build", configured)
        self.assertEqual(run.commands("json2"), [], "no size of a configure")
        (verified,) = run.commands("sink_profile.py")
        self.assertNotIn("--size-json", verified)
        self.assertTrue(any(line.startswith("configured") for line in self.output))

    def test_no_verify_skips_the_size_and_the_guard(self) -> None:
        run = Recorder({"describe": (0, "v1\n")})
        self.assertEqual(bf.cmd_build(self.table, self.args(no_verify=True), run, self.out), 0)
        self.assertEqual(run.commands("json2"), [])
        self.assertEqual(run.commands("sink_profile.py"), [])

    def test_a_profile_that_is_not_in_the_file_is_refused(self) -> None:
        with self.assertRaises(sink_profile.ProfileError):
            bf.cmd_build(self.table, self.args(profile="s3-nothing"), Recorder(), self.out)

    def test_a_missing_network_fragment_is_refused_before_anything_runs(self) -> None:
        (self.root / "wifi.sdkconfig").unlink()
        run = Recorder()
        with self.assertRaises(bench.BenchError):
            bf.cmd_build(self.table, self.args(wifi=True), run, self.out)
        self.assertEqual(run.calls, [])


class Verify(Fixture):
    def setUp(self) -> None:
        super().setUp()
        (self.build / "project_description.json").write_text(
            json.dumps({"project_name": "iclforge_hearth_sink", "project_version": "v1"}), "utf-8"
        )

    def verify(self, hardware: dict[str, Any] | None) -> int:
        args = namespace(board="S3b", build_dir=str(self.build), profile=None)
        return bf.cmd_verify(self.table, args, Recorder(), self.out, lambda host: hardware)

    def test_a_board_that_runs_the_build_matches(self) -> None:
        self.assertEqual(self.verify({"project": "iclforge_hearth_sink", "version": "v1"}), 0)
        self.assertTrue(any("matches" in line for line in self.output))

    def test_another_version_differs(self) -> None:
        self.assertEqual(self.verify({"project": "iclforge_hearth_sink", "version": "v0"}), 1)
        self.assertTrue(any("DIFFERS" in line for line in self.output))

    def test_the_profile_is_held_to_the_file_s_hash(self) -> None:
        digest = sink_profile.profile_hash(sink_profile.load(), "s3-n16r8")
        board = {
            "project": "iclforge_hearth_sink",
            "version": "v1",
            "profile": "s3-n16r8",
            "profile_hash": digest,
        }
        self.assertEqual(self.verify_profile(board), 0)
        stale = {**board, "profile_hash": "000000000000"}
        self.assertEqual(self.verify_profile(stale), 1)

    def verify_profile(self, hardware: dict[str, Any]) -> int:
        args = namespace(board="S3b", build_dir=str(self.build), profile="s3-n16r8")
        return bf.cmd_verify(self.table, args, Recorder(), self.out, lambda host: hardware)

    def test_a_board_that_does_not_answer_is_an_error_not_a_match(self) -> None:
        with self.assertRaises(bench.BenchError):
            self.verify(None)


class Ota(Fixture):
    def args(self, board: str = "S3b", **more: Any) -> argparse.Namespace:
        base = {"board": board, "build_dir": str(self.build), "session": "16d036", "force": False}
        return namespace(**{**base, **more})

    def test_a_board_that_takes_no_ota_is_pointed_at_the_usb_flash(self) -> None:
        self.book("P4b")
        run = Recorder()
        with self.assertRaises(bench.BenchError) as raised:
            bf.cmd_ota(self.table, self.args("P4b"), run, self.out)
        self.assertIn("flash P4b --app-only", str(raised.exception))
        self.assertEqual(run.calls, [])

    def test_an_unbooked_board_is_pushed_nothing(self) -> None:
        run = Recorder()
        with self.assertRaises(bench.BenchError):
            bf.cmd_ota(self.table, self.args(), run, self.out)
        self.assertEqual(run.calls, [])

    def test_the_push_goes_to_the_table_s_address_and_its_exit_code_is_kept(self) -> None:
        self.book("S3b")
        run = Recorder({"ota.py": (2, f"192.168.1.45: rolled back ({SSID})")})
        code = bf.cmd_ota(self.table, self.args(force=True), run, self.out)
        self.assertEqual(code, 2)
        (pushed,) = run.commands("ota.py")
        self.assertEqual(
            pushed[-5:], ["--build-dir", str(self.build), "--host", "192.168.1.45", "--force"]
        )
        self.assertNotIn(SSID, "\n".join(self.output))


class Reset(Fixture):
    def args(self, board: str = "P4a") -> argparse.Namespace:
        return namespace(board=board, session="16d036")

    def test_a_board_nobody_has_booked_is_not_reset(self) -> None:
        run = Recorder()
        with self.assertRaises(bench.BenchError):
            bf.cmd_reset(self.table, self.args(), run, self.out)
        self.assertEqual(run.calls, [])

    def test_the_reset_is_a_watchdog_reset_in_the_idf_environment_and_waits_for_the_board(
        self,
    ) -> None:
        self.book("P4a")
        run = Recorder({"read-mac": (0, "MAC:                30:ed:a0:ea:6d:66\n")})
        answers = iter([None, None, {"version": "v1", "profile": "p4-rev1"}])
        code = bf.cmd_reset(
            self.table, self.args(), run, self.out, lambda host: next(answers), wait=30.0
        )
        self.assertEqual(code, 0)
        (reset,) = run.commands("read-mac")
        self.assertIn("--chip esp32p4 --port COM10", " ".join(reset))
        self.assertIn("--before default-reset --after watchdog-reset read-mac", " ".join(reset))
        self.assertEqual(run.idf_flags("read-mac"), [True])
        self.assertTrue(any("version v1 (profile p4-rev1)" in line for line in self.output))

    def test_a_board_that_does_not_come_back_is_a_failure(self) -> None:
        self.book("P4a")
        run = Recorder({"read-mac": (0, "MAC:                30:ed:a0:ea:6d:66\n")})
        code = bf.cmd_reset(self.table, self.args(), run, self.out, lambda host: None, wait=0.0)
        self.assertEqual(code, 1)
        self.assertTrue(any("did not answer" in line for line in self.output))

    def test_the_wrong_chip_on_the_port_is_an_error(self) -> None:
        self.book("P4a")
        run = Recorder({"read-mac": (0, S3_MAC_OUTPUT)})
        with self.assertRaises(bench.BenchError) as raised:
            bf.cmd_reset(self.table, self.args(), run, self.out, lambda host: {}, wait=0.0)
        self.assertIn("is not P4a", str(raised.exception))

    def test_no_mac_read_is_not_a_reset(self) -> None:
        self.book("P4a")
        run = Recorder({"read-mac": (2, "Failed to connect")})
        with self.assertRaises(bench.BenchError):
            bf.cmd_reset(self.table, self.args(), run, self.out, lambda host: {}, wait=0.0)

    def test_it_needs_a_session(self) -> None:
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            bf.build_parser().parse_args(["reset", "P4a"])


class CommandLine(Fixture):
    def test_an_unknown_board_is_a_one_line_error(self) -> None:
        err = _capture_stderr(
            lambda: bf.main(
                ["verify", "S3z", "--build-dir", str(self.build)], Recorder(), table=self.table
            )
        )
        self.assertEqual(err[0], 1)
        self.assertIn("no board named 'S3z'", err[1])

    def test_flash_requires_a_session(self) -> None:
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            bf.build_parser().parse_args(["flash", "S3b", "--build-dir", str(self.build)])


def _capture_stderr(call: Any) -> tuple[int, str]:
    buffer = io.StringIO()
    with contextlib.redirect_stderr(buffer):
        code = call()
    return code, buffer.getvalue()


if __name__ == "__main__":
    unittest.main()
