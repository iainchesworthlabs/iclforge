"""Unit tests for check_sonar_scope.py: sonar-project.properties held to the tree."""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_sonar_scope as C

FILES = [
    "libs/a/include/x/a.hpp",
    "libs/a/src/a.cpp",
    "libs/a/tests/test_a.cpp",
    "libs/a/fuzz/fuzz_a.cpp",
    "apps/p/src/main.cpp",
    "apps/p/tests/test_p.cpp",
    "bindings/py/tests/test_py.py",
    "bindings/py/src/mod.py",
    "tools/fuzz/driver.py",
    "tools/ci/run.py",
    "firmware/board/main.cpp",
]

CLEAN = r"""
sonar.sources=libs,apps,bindings/py,tools
sonar.tests=libs,apps,bindings/py/tests
sonar.test.inclusions=libs/*/tests/**,libs/*/fuzz/**,apps/*/tests/**,bindings/py/tests/**
sonar.exclusions=\
  libs/*/tests/**,\
  libs/*/fuzz/**,\
  apps/*/tests/**,\
  bindings/py/tests/**,\
  build/**
sonar.coverage.exclusions=tools/**
sonar.cpd.exclusions=libs/a/src/**/*.cpp
sonar.issue.ignore.multicriteria=e1
sonar.issue.ignore.multicriteria.e1.ruleKey=cpp:S2083
sonar.issue.ignore.multicriteria.e1.resourceKey=apps/p/src/main.cpp
"""


def without_py_tests(text: str) -> str:
    """The properties as if bindings/py/tests were not declared a tree of tests at all."""
    return (
        text.replace(",bindings/py/tests\n", "\n")
        .replace(",bindings/py/tests/**\n", "\n")
        .replace("  bindings/py/tests/**,\\\n", "")
    )


def run(text: str, files=FILES, on_purpose=None):
    return C.check(C.parse_properties(text), files, on_purpose or C.deliberate_trees(text))


class Parse(unittest.TestCase):
    def test_a_trailing_backslash_continues_a_value_and_comments_are_skipped(self):
        props = C.parse_properties("# note\nsonar.exclusions=a/**,\\\n  b/**,\\\n  c/**\nk=v\n")
        self.assertEqual(C.split_list(props["sonar.exclusions"]), ["a/**", "b/**", "c/**"])
        self.assertEqual(props["k"], "v")

    def test_the_deliberate_comment_is_read_from_the_raw_text(self):
        text = "# check_sonar_scope: tests-as-sources bindings/py/tests, apps/w/tests/\nk=v\n"
        self.assertEqual(C.deliberate_trees(text), ["bindings/py/tests", "apps/w/tests"])


class Globs(unittest.TestCase):
    def test_ant_style_patterns(self):
        cases = [
            ("libs/*/tests/**", "libs/a/tests/x/y.cpp", True),
            ("libs/*/tests/**", "libs/a/b/tests/y.cpp", False),
            ("**/node_modules/**", "a/node_modules/b/c.js", True),
            ("**/*.min.js", "x.min.js", True),
            ("apps/demos/wasm/**/*.cpp", "apps/demos/wasm/a.cpp", True),
            ("apps/demos/wasm/**/*.cpp", "apps/demos/wasm/a/b/c.cpp", True),
            ("libs/ac3/src/**/tables*.cpp", "libs/ac3/src/core/eac3_tables.cpp", False),
            ("firmware/baremetal/", "firmware/baremetal/a/b.cpp", True),
        ]
        for pattern, path, wanted in cases:
            with self.subTest(pattern=pattern, path=path):
                self.assertEqual(bool(C.glob_to_regex(pattern).match(path)), wanted)


class Scope(unittest.TestCase):
    def test_a_file_that_divides_the_tree_cleanly_passes(self):
        problems, counts = run(CLEAN)
        self.assertEqual(problems, [])
        # a.hpp, a.cpp, main.cpp, mod.py, driver.py, run.py are source; the four tests are tests.
        self.assertEqual(counts, {"source": 6, "test": 4, "excluded": 0})

    def test_a_root_the_tree_lacks_fails(self):
        problems, _ = run(CLEAN.replace("bindings/py,", "bindings/gone,"))
        self.assertIn(
            "sonar.sources names bindings/gone, which nothing in the tree is filed under", problems
        )

    def test_a_pattern_that_matches_no_file_fails_but_build_output_is_not_stale(self):
        problems, _ = run(CLEAN.replace("libs/a/src/**/*.cpp", "libs/a/src/**/tables*.cpp"))
        self.assertIn(
            "sonar.cpd.exclusions: libs/a/src/**/tables*.cpp matches no file of the tree", problems
        )
        self.assertFalse([p for p in problems if "build/**" in p])

    def test_an_issue_ignore_rule_for_a_file_that_moved_fails(self):
        problems, _ = run(CLEAN.replace("apps/p/src/main.cpp", "apps/p/src/old/main.cpp"))
        self.assertIn(
            "an issue-ignore resourceKey: apps/p/src/old/main.cpp matches no file of the tree",
            problems,
        )

    def test_a_test_that_is_also_scanned_as_source_fails(self):
        problems, _ = run(CLEAN.replace("  apps/*/tests/**,\\\n", ""))
        self.assertIn(
            "apps/p/tests/test_p.cpp is a test and is also scanned as source: "
            "sonar.exclusions lacks it",
            problems,
        )

    def test_a_test_no_inclusion_names_is_scanned_as_source_and_fails(self):
        problems, _ = run(without_py_tests(CLEAN))
        self.assertIn(
            "bindings/py/tests/test_py.py is under a tests/ or fuzz/ directory and is scanned "
            "as source",
            problems,
        )

    def test_a_tree_the_file_scans_as_source_on_purpose_passes_and_a_stale_one_fails(self):
        marker = "# check_sonar_scope: tests-as-sources bindings/py/tests\n"
        problems, _ = run(marker + without_py_tests(CLEAN))
        self.assertEqual(problems, [])
        problems, _ = run(marker.replace("py/tests", "gone/tests") + without_py_tests(CLEAN))
        self.assertIn(
            "tests-as-sources names bindings/gone/tests, which nothing in the tree is filed under",
            problems,
        )

    def test_a_fuzz_directory_of_tools_is_a_script_and_not_a_test(self):
        problems, _ = run(CLEAN)
        self.assertFalse([p for p in problems if "tools/fuzz" in p])

    def test_what_no_root_covers_is_listed(self):
        got = C.unscanned(C.parse_properties(CLEAN), FILES)
        self.assertEqual(got, ["firmware/board (1 files)"])


class RealTree(unittest.TestCase):
    def test_the_properties_of_the_real_tree_hold(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = C.main([])
        self.assertEqual(code, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
