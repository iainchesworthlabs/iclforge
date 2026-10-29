"""Unit tests for check_fuzz_harnesses.py: a tree with a harness that is unbuilt, unlisted or
unseeded shows, and one that is whole does not."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_fuzz_harnesses as C

RUN_SH = """\
readonly BASE_TARGETS=(fuzz_alpha fuzz_beta
                       fuzz_gamma)
seed_source_for() {
    case "$1" in
        fuzz_differential_alpha) echo fuzz_alpha ;;
        fuzz_beta)               echo fuzz_alpha ;;
        *)                       echo "$1" ;;
    esac
}
"""
WORKFLOW = """\
        run: bash tools/fuzz/run.sh regress fuzz_differential_alpha
        run: bash tools/fuzz/run.sh run fuzz_differential_alpha
        run: bash tools/fuzz/run.sh regress fuzz_differential_alpha
        run: bash tools/fuzz/run.sh run fuzz_differential_alpha
"""


class Tree(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name)
        (self.repo / "tools" / "fuzz").mkdir(parents=True)
        (self.repo / ".github" / "workflows").mkdir(parents=True)
        self.fuzz = self.repo / "libs" / "lib" / "fuzz"
        self.fuzz.mkdir(parents=True)
        self.set_run_sh(RUN_SH)
        self.set_workflow(WORKFLOW)
        for name in ("fuzz_alpha", "fuzz_beta", "fuzz_gamma", "fuzz_differential_alpha"):
            self.add_harness(name)
        self.add_seed("fuzz_alpha")
        self.add_seed("fuzz_gamma")

    def set_run_sh(self, text):
        (self.repo / "tools" / "fuzz" / "run.sh").write_text(text, encoding="utf-8")

    def set_workflow(self, text):
        (self.repo / ".github" / "workflows" / "fuzz.yml").write_text(text, encoding="utf-8")

    def add_harness(self, name, register=True):
        (self.fuzz / f"{name}.cpp").write_text("", encoding="utf-8")
        if register:
            with (self.fuzz / "CMakeLists.txt").open("a", encoding="utf-8") as cmake:
                cmake.write(f"iclforge_add_fuzzer({name})\n")

    def add_seed(self, name, file="one"):
        directory = self.fuzz / "seeds" / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / file).write_bytes(b"x")

    def problems(self):
        return C.problems_in(self.repo, {}, {})[0]


class Harnesses(Tree):
    def test_a_whole_tree_has_no_problem(self):
        self.assertEqual(self.problems(), [])

    def test_a_harness_the_cmake_file_does_not_register_is_reported(self):
        self.add_harness("fuzz_delta", register=False)
        self.assertTrue(any("fuzz_delta" in p and "register" in p for p in self.problems()))

    def test_a_harness_in_no_run_list_is_reported(self):
        self.add_harness("fuzz_delta")
        self.add_seed("fuzz_delta")
        self.assertTrue(any("fuzz_delta is in no run list" in p for p in self.problems()))

    def test_a_listed_harness_with_no_seeds_is_reported(self):
        (self.fuzz / "seeds" / "fuzz_gamma" / "one").unlink()
        self.assertTrue(any("fuzz_gamma has no seeds" in p for p in self.problems()))

    def test_a_harness_that_shares_seeds_reads_its_sources(self):
        self.assertEqual(self.problems(), [])
        (self.fuzz / "seeds" / "fuzz_alpha" / "one").unlink()
        self.assertTrue(any("fuzz_beta has no seeds" in p for p in self.problems()))

    def test_a_list_entry_with_no_source_is_reported(self):
        self.set_run_sh(RUN_SH.replace("fuzz_gamma)", "fuzz_gamma fuzz_ghost)"))
        self.assertTrue(any("fuzz_ghost" in p for p in self.problems()))


class Exceptions(Tree):
    def test_an_exception_the_workflow_runs_is_accepted(self):
        self.add_harness("fuzz_opt")
        self.add_seed("fuzz_opt")
        self.set_workflow(WORKFLOW + "run: bash tools/fuzz/run.sh run fuzz_opt\n")
        self.assertEqual(C.problems_in(self.repo, {"fuzz_opt": "opt-in"}, {})[0], [])

    def test_an_exception_the_workflow_never_runs_is_reported(self):
        self.add_harness("fuzz_opt")
        self.add_seed("fuzz_opt")
        problems = C.problems_in(self.repo, {"fuzz_opt": "opt-in"}, {})[0]
        self.assertTrue(any("fuzz.yml never runs it" in p for p in problems))

    def test_an_exception_for_a_harness_that_is_gone_is_reported(self):
        problems = C.problems_in(self.repo, {"fuzz_gone": "opt-in"}, {})[0]
        self.assertTrue(any("stale" in p for p in problems))

    def test_a_known_gap_passes_and_is_noticed(self):
        self.add_harness("fuzz_delta")
        problems, notices = C.problems_in(self.repo, {}, {"fuzz_delta": "no seeds"})
        self.assertEqual(problems, [])
        self.assertEqual(notices, ["fuzz_delta: no seeds"])

    def test_a_known_gap_that_is_closed_is_reported(self):
        self.add_seed("fuzz_gamma")
        problems = C.problems_in(self.repo, {}, {"fuzz_gamma": "no seeds"})[0]
        self.assertTrue(any("is closed" in p for p in problems))


class Differential(Tree):
    def test_a_differential_harness_the_workflow_runs_once_is_reported(self):
        self.set_workflow(WORKFLOW.splitlines()[0] + "\n" + WORKFLOW.splitlines()[1] + "\n")
        problems = self.problems()
        self.assertTrue(any("`run.sh run` line" in p for p in problems))
        self.assertTrue(any("`run.sh regress` line" in p for p in problems))

    def test_a_differential_harness_in_the_default_list_is_reported(self):
        self.set_run_sh(RUN_SH.replace("fuzz_gamma)", "fuzz_gamma fuzz_differential_alpha)"))
        self.assertTrue(any("meant not to be" in p for p in self.problems()))


class Dictionaries(Tree):
    def dictionary(self, name, text):
        directory = self.fuzz / "dictionaries"
        directory.mkdir(exist_ok=True)
        (directory / f"{name}.dict").write_text(text, encoding="utf-8")

    def test_a_dictionary_in_libfuzzers_syntax_is_accepted(self):
        self.dictionary("fuzz_alpha", '# a comment\n\nsync="\\xF8\\x72"\n"plain"\n')
        self.assertEqual(self.problems(), [])

    def test_a_dictionary_line_that_is_not_syntax_is_reported(self):
        self.dictionary("fuzz_alpha", "sync = F8 72\n")
        self.assertTrue(any(":1: not libFuzzer syntax" in p for p in self.problems()))

    def test_a_dictionary_of_no_harness_is_reported(self):
        self.dictionary("fuzz_nowhere", '"x"\n')
        self.assertTrue(any("belongs to no harness" in p for p in self.problems()))


class RealTree(unittest.TestCase):
    def test_the_repository_has_no_problem(self):
        problems, _ = C.problems_in(C.REPO)
        self.assertEqual(problems, [])


if __name__ == "__main__":
    unittest.main()
