"""The Test step of .github/actions/build-leg/action.yml runs a failing parallel phase's tests
again, one at a time, and passes the phase when they pass alone. These tests hold that repeat to
the tests that failed, without running ctest: the shell functions are cut out of the action and
run under bash against ctest output copied from real runs, with a stand-in for `ctest` itself.

What this exists to hold down. The repeat used `ctest --rerun-failed`, which reads test NUMBERS
from Testing/Temporary/LastTestsFailed.log and looks them up in the full test list.
`ctest -E <regex>` with no -R takes the tests it matches out of the list before numbering it, and
the first phase runs `-E _qml_tests_`, so its #535 was the full list's #588: the repeat ran another
test, saw it pass and turned a red leg green. On the main run of PR #1286 (Linux GCC) a real
failure of the translation catalogues went through as "bitstream: a join the packer cannot make
whole bursts of reopens instead", and the same happened on Windows MSVC, macOS, Linux LLVM and
arm64. To see it with ctest itself: a CTestTestfile.cmake with three `*_qml_tests_*` tests ahead
of `t_a`, a failing `t_b` and `t_c`. `ctest -E _qml_tests_` calls `t_b` #2 and writes `2:t_b` to
LastTestsFailed.log, and `ctest --rerun-failed` then runs the full list's #2, `app_qml_tests_two`,
which passes.

The repeat is now by name (`--tests-from-file`), and the leg fails unless the tests the repeat
started are exactly the tests that failed. The cases below are the ways that could go wrong.

stdlib `unittest`, run by _static.yml's "Oracle unit tests" step like the rest of tools/ci.

Run: python3 -m unittest discover -s tools/ci -p 'test_build_leg_ctest_rerun.py'
"""

import os
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ACTION = Path(__file__).resolve().parents[2] / ".github" / "actions" / "build-leg" / "action.yml"

FUNCTIONS = ("failed_test_names", "failed_test_count", "started_test_names", "run_failed_alone")

# The tail of a first pass as ctest 4.2.3 printed it on the Linux GCC leg of PR #1286's run: the
# failed list, a list of tests that did not run above it, and each failure's labels after its
# status.
FIRST_PASS = (
    "99% tests passed, 2 tests failed out of 3701\n"
    "\n"
    "Label Time Summary:\n"
    "ac3                   =  15.56 sec*proc (994 tests)\n"
    "\n"
    "Total Test time (real) = 105.98 sec\n"
    "\n"
    "The following tests did not run:\n"
    "\t579 - hearth ac4: the engine plays the streams of AC4_API_STREAM_DIR (Skipped)\n"
    "\t2821 - Chromium's A-JOC stream decodes in full and core decoding (Skipped)\n"
    "\n"
    "The following tests FAILED:\n"
    "\t535 - engine: a sink that places fewer objects than the stream declares is sent "
    "PCM decoded here (Failed) group hearth iclforge websocket\n"
    "\t1037 - gui translation catalogues carry no unfinished or dead entry (Failed) "
    "crucible gui translations\n"
    "Errors while running CTest\n"
)
FIRST_PASS_NAMES = [
    "engine: a sink that places fewer objects than the stream declares is sent PCM decoded here",
    "gui translation catalogues carry no unfinished or dead entry",
]


def find_bash() -> str | None:
    """A bash this test can run. `bash` on a Windows PATH is usually the WSL launcher."""
    if os.name != "nt":
        return shutil.which("bash")
    git = Path(os.environ.get("PROGRAMFILES", r"C:\Program Files")) / "Git" / "bin" / "bash.exe"
    return str(git) if git.exists() else None


BASH = find_bash()


def function_source(name: str) -> str:
    text = ACTION.read_text(encoding="utf-8")
    match = re.search(rf"^( +){name}\(\) \{{\n.*?^\1\}}\n", text, re.DOTALL | re.MULTILINE)
    assert match, f"{name}() is not in {ACTION.name} any more"
    return textwrap.dedent(match.group(0))


# A stand-in for ctest. Without --tests-from-file it is the first pass, which prints $FIRST_PASS and
# exits 8. With it, $STUB_MODE says what the repeat does:
#   pass   runs the tests the file lists, and they pass
#   fail   runs them, and the first one fails
#   other  ignores the file and runs two other tests, which pass (what --rerun-failed did)
#   fewer  runs the first test the file lists and not the rest
#   extra  runs the listed tests and one more
# --rerun-failed is refused outright: nothing may ask for it again.
STUB = r"""
ctest() {
  local file="" n=0 name
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --tests-from-file) file="$2"; shift ;;
      --rerun-failed) echo "ctest: --rerun-failed asked for" >&2; return 99 ;;
    esac
    shift
  done
  if [ -z "$file" ]; then
    cat "$FIRST_PASS"
    return 8
  fi
  case "$STUB_MODE" in
    other)
      printf 'Test project /build\n'
      printf '    Start 535: bitstream: a join the packer cannot make whole bursts\n'
      printf '1/2 Test #535: bitstream: a join the packer cannot ..   Passed    0.01 sec\n'
      printf '    Start 1037: a zero-channel tap folds to silence and adds nothing to the bed\n'
      printf '2/2 Test #1037: a zero-channel tap folds to silence ..   Passed    0.00 sec\n'
      printf '\n100%% tests passed, 0 tests failed out of 2\n'
      return 0 ;;
  esac
  printf 'Test project /build\n'
  while IFS= read -r name; do
    n=$((n + 1))
    printf '    Start %s: %s\n' "$n" "$name"
    if [ "$STUB_MODE" = fail ] && [ "$n" -eq 1 ]; then
      printf '1/1 Test #%s: %s ..***Failed    0.01 sec\n' "$n" "$name"
    elif [ "$STUB_MODE" != fewer ] || [ "$n" -eq 1 ]; then
      printf '%s/%s Test #%s: %s ..   Passed    0.01 sec\n' "$n" "$n" "$n" "$name"
    fi
    if [ "$STUB_MODE" = fewer ]; then
      break
    fi
  done < "$file"
  if [ "$STUB_MODE" = extra ]; then
    printf '    Start 99: an unrelated test that was never asked for\n'
  fi
  if [ "$STUB_MODE" = fail ]; then
    printf '\n50%% tests passed, 1 tests failed out of 2\n\nThe following tests FAILED:\n'
    printf '\t  1 - still failing alone (Failed)\nErrors while running CTest\n'
    return 8
  fi
  printf '\n100%% tests passed, 0 tests failed out of %s\n' "$n"
  return 0
}
"""


@unittest.skipUnless(BASH, "needs a POSIX bash")
class ShellCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def bash(self, script: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        full = dict(os.environ)
        full.update(env or {})
        return subprocess.run(
            [BASH, "-c", "set -euo pipefail\n" + script],
            capture_output=True,
            text=True,
            check=False,
            env=full,
        )

    def write(self, name: str, text: str) -> Path:
        path = self.dir / name
        path.write_bytes(text.encode("utf-8"))
        return path

    def functions(self, *names: str) -> str:
        return "".join(function_source(name) for name in names)


class OutputParsing(ShellCase):
    def read(self, function: str, text: str) -> list[str]:
        log = self.write("run.log", text)
        done = self.bash(self.functions(function) + f'{function} < "{log.as_posix()}"\n')
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout.splitlines()

    def test_the_failed_names_come_out_whole_without_labels_or_the_skipped_ones(self) -> None:
        self.assertEqual(self.read("failed_test_names", FIRST_PASS), FIRST_PASS_NAMES)

    def test_a_name_with_parentheses_of_its_own_is_not_cut(self) -> None:
        text = (
            "The following tests FAILED:\n"
            "\t  4 - NetworkSinks (paired) plans a group (Failed) hearth group\n"
            "\t  9 - x (Failed) y (Timeout)\n"
            "\t 10 - a name that is all labels' business (SEGFAULT) a b c\n"
            "\t 11 - never started (Not Run)\n"
            "Errors while running CTest\n"
        )
        self.assertEqual(
            self.read("failed_test_names", text),
            [
                "NetworkSinks (paired) plans a group",
                "x (Failed) y",
                "a name that is all labels' business",
                "never started",
            ],
        )

    def test_a_windows_ctest_that_ends_its_lines_with_carriage_returns(self) -> None:
        text = FIRST_PASS.replace("\n", "\r\n")
        self.assertEqual(self.read("failed_test_names", text), FIRST_PASS_NAMES)
        self.assertEqual(self.read("failed_test_count", text), ["2"])

    def test_no_failed_list_is_no_names(self) -> None:
        self.assertEqual(self.read("failed_test_names", "100% tests passed, 0 tests failed\n"), [])

    def test_a_test_output_that_looks_like_a_list_line_outside_the_list_is_not_read(self) -> None:
        text = "\t7 - an echo from a test (Failed)\n" + FIRST_PASS
        self.assertEqual(self.read("failed_test_names", text), FIRST_PASS_NAMES)

    def test_the_failed_count_is_the_one_ctest_printed(self) -> None:
        self.assertEqual(self.read("failed_test_count", FIRST_PASS), ["2"])
        self.assertEqual(
            self.read("failed_test_count", "1% tests passed, 1 tests failed out of 9"), ["1"]
        )
        self.assertEqual(self.read("failed_test_count", "100% tests passed out of 1\n"), [])

    def test_the_started_names_are_exact(self) -> None:
        text = (
            "Test project /build\n"
            "    Start  535: engine: a sink that places fewer objects ... than it declares\n"
            "1/2 Test #535: engine: a sink that places ..   Passed    0.01 sec\n"
            "    Start 1037: gui translation catalogues carry no unfinished or dead entry\r\n"
        )
        self.assertEqual(
            self.read("started_test_names", text),
            [
                "engine: a sink that places fewer objects ... than it declares",
                "gui translation catalogues carry no unfinished or dead entry",
            ],
        )


class RunFailedAlone(ShellCase):
    def run_it(self, mode: str, first: str = FIRST_PASS) -> subprocess.CompletedProcess:
        first_log = self.write("first.log", first)
        script = (
            self.functions(*FUNCTIONS)
            + STUB
            + 'log="$RUNNER_TEMP/ctest.log"\n'
            + 'failed_names="$RUNNER_TEMP/ctest-failed-names.txt"\n'
            + f'rc=0\nrun_failed_alone "{first_log.as_posix()}" "$RUNNER_TEMP/again.log" || rc=$?\n'
            + 'echo "rc=$rc"\n'
        )
        return self.bash(
            script,
            {
                "RUNNER_TEMP": self.dir.as_posix(),
                "LEG_PRESET": "linux-gcc",
                "FIRST_PASS": (self.dir / "first.log").as_posix(),
                "STUB_MODE": mode,
            },
        )

    def test_the_tests_that_failed_are_the_tests_run_again(self) -> None:
        done = self.run_it("pass")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("rc=0", done.stdout)
        for name in FIRST_PASS_NAMES:
            self.assertIn(f": {name}\n", done.stdout)
        self.assertNotIn("::error", done.stdout)
        listed = (self.dir / "ctest-failed-names.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual(listed, sorted(FIRST_PASS_NAMES))

    def test_a_repeat_that_ran_other_tests_is_not_a_pass(self) -> None:
        # What --rerun-failed did on every leg: two other tests, both green.
        done = self.run_it("other")
        self.assertNotIn("rc=0", done.stdout)
        self.assertIn(
            "::error title=ctest::The run that was to repeat the failed tests", done.stdout
        )
        self.assertIn("Not run again: " + ";".join(sorted(FIRST_PASS_NAMES)), done.stdout)
        self.assertIn("Run, and did not fail: a zero-channel tap", done.stdout)

    def test_a_repeat_that_ran_fewer_tests_is_not_a_pass(self) -> None:
        done = self.run_it("fewer")
        self.assertNotIn("rc=0", done.stdout)
        self.assertIn("Not run again: " + sorted(FIRST_PASS_NAMES)[1], done.stdout)

    def test_a_repeat_that_ran_more_tests_is_not_a_pass(self) -> None:
        done = self.run_it("extra")
        self.assertNotIn("rc=0", done.stdout)
        self.assertIn(
            "Run, and did not fail: an unrelated test that was never asked for", done.stdout
        )

    def test_a_test_that_fails_alone_keeps_ctests_status(self) -> None:
        done = self.run_it("fail")
        self.assertIn("rc=8", done.stdout)
        self.assertNotIn("::error", done.stdout)

    def test_names_that_do_not_add_up_to_ctests_count_are_not_run(self) -> None:
        cut = FIRST_PASS.replace("2 tests failed", "3 tests failed")
        done = self.run_it("pass", cut)
        self.assertIn("rc=1", done.stdout)
        self.assertIn("ctest counted 3 failed and its output names 2", done.stdout)
        self.assertNotIn("Start", done.stdout)

    def test_a_failure_with_no_list_is_not_run_on_a_guess(self) -> None:
        done = self.run_it("pass", "No tests were found!!!\nErrors while running CTest\n")
        self.assertIn("rc=1", done.stdout)
        self.assertIn("::error title=ctest::Not running the failed tests again", done.stdout)
        self.assertNotIn("Start", done.stdout)

    def test_a_windows_log_repeats_the_same_tests(self) -> None:
        done = self.run_it("pass", FIRST_PASS.replace("\n", "\r\n"))
        self.assertIn("rc=0", done.stdout)
        for name in FIRST_PASS_NAMES:
            self.assertIn(f": {name}\n", done.stdout)


class RunPhase(ShellCase):
    def run_it(self, mode: str, at_once: int = 4) -> tuple[subprocess.CompletedProcess, str]:
        self.write("first.log", FIRST_PASS)
        script = (
            self.functions(*FUNCTIONS, "run_phase")
            + STUB
            + 'log="$RUNNER_TEMP/ctest.log"\n'
            + 'failed_log="$RUNNER_TEMP/ctest-failed.log"\n'
            + 'phase_log="$RUNNER_TEMP/ctest-phase.log"\n'
            + 'again_log="$RUNNER_TEMP/ctest-again.log"\n'
            + 'failed_names="$RUNNER_TEMP/ctest-failed-names.txt"\n'
            + ': > "$log"; : > "$failed_log"\n'
            + f"rc=0\nrun_phase {at_once} retry -LE Performance -E '_qml_tests_' || rc=$?\n"
            + 'echo "rc=$rc"\n'
        )
        done = self.bash(
            script,
            {
                "RUNNER_TEMP": self.dir.as_posix(),
                "LEG_PRESET": "linux-gcc",
                "FIRST_PASS": (self.dir / "first.log").as_posix(),
                "STUB_MODE": mode,
            },
        )
        failed = (self.dir / "ctest-failed.log").read_text(encoding="utf-8")
        return done, failed

    def test_a_pass_alone_passes_the_phase_and_names_the_tests_in_a_warning(self) -> None:
        done, failed = self.run_it("pass")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("rc=0", done.stdout)
        self.assertIn(
            "::warning title=ctest::Failed under -j4 and passed when run alone: ", done.stdout
        )
        self.assertIn(";".join(sorted(FIRST_PASS_NAMES)), done.stdout)
        self.assertEqual(failed, "")

    def test_a_repeat_of_other_tests_fails_the_phase_and_keeps_the_first_failures(self) -> None:
        done, failed = self.run_it("other")
        self.assertNotIn("rc=0", done.stdout)
        self.assertNotIn("::warning", done.stdout)
        self.assertIn("The following tests FAILED:", failed)
        self.assertIn("gui translation catalogues carry no unfinished or dead entry", failed)

    def test_a_failure_alone_keeps_the_repeats_own_list(self) -> None:
        done, failed = self.run_it("fail")
        self.assertIn("rc=8", done.stdout)
        self.assertIn("still failing alone", failed)
        # The first pass's own list line is not kept beside it.
        self.assertNotIn("1037 - gui translation catalogues", failed)

    def test_a_phase_that_runs_one_test_at_a_time_has_no_repeat(self) -> None:
        done, failed = self.run_it("pass", at_once=1)
        self.assertIn("rc=8", done.stdout)
        self.assertNotIn("Still failing", done.stdout)
        self.assertIn("gui translation catalogues carry no unfinished or dead entry", failed)


class ActionText(unittest.TestCase):
    def test_nothing_asks_ctest_to_rerun_failed_by_number(self) -> None:
        commands = [
            line
            for line in ACTION.read_text(encoding="utf-8").splitlines()
            if not line.lstrip().startswith("#")
        ]
        for line in commands:
            self.assertNotIn("--rerun-failed", line)


if __name__ == "__main__":
    unittest.main()
