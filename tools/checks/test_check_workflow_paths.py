"""Unit tests for check_workflow_paths.py: path filters against the tree and the graph."""

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_workflow_paths as C
import project_graph


class Patterns(unittest.TestCase):
    def test_a_star_stays_in_its_directory_and_two_cross_them(self) -> None:
        self.assertTrue(C.glob_to_re("libs/*/x.cpp").match("libs/a/x.cpp"))
        self.assertFalse(C.glob_to_re("libs/*/x.cpp").match("libs/a/b/x.cpp"))
        self.assertTrue(C.glob_to_re("libs/ac3/**").match("libs/ac3/src/deep/x.cpp"))
        self.assertTrue(C.glob_to_re("**/x.cpp").match("x.cpp"))
        self.assertTrue(C.glob_to_re("**/x.cpp").match("a/b/x.cpp"))
        self.assertTrue(C.glob_to_re("a?.cpp").match("ab.cpp"))
        self.assertFalse(C.glob_to_re("a?.cpp").match("a/.cpp"))

    def test_the_items_of_every_filter_list_are_read_with_their_lines(self) -> None:
        text = (
            "on:\n"
            "  push:\n"
            "    paths:\n"
            '      - "libs/base/**"   # the library\n'
            "      - 'CMakeLists.txt'\n"
            "\n"
            "      # a comment between\n"
            "      - .github/workflows/x.yml\n"
            "    tags:\n"
            '      - "v*"\n'
            "  pull_request:\n"
            "    paths-ignore:\n"
            "      - docs/**\n"
        )
        self.assertEqual(
            C.filters_of(text),
            [
                (4, "paths", "libs/base/**"),
                (5, "paths", "CMakeLists.txt"),
                (8, "paths", ".github/workflows/x.yml"),
                (13, "paths-ignore", "docs/**"),
            ],
        )


TABLE = {
    "lanes": ["core", "esp"],
    "projects": {
        "base": {"kind": "library", "path": "libs/base", "may_use": [], "lanes": ["core"]},
        "dsp": {"kind": "library", "path": "libs/dsp", "may_use": ["base"], "lanes": ["core"]},
        "codec": {"kind": "library", "path": "libs/codec", "may_use": ["dsp"], "lanes": ["core"]},
        "tags": {"kind": "library", "path": "libs/tags", "may_use": ["base"], "lanes": ["core"]},
        "wheel": {
            "kind": "binding",
            "path": "bindings/wheel",
            "may_use": ["codec", "tags"],
            "lanes": ["core"],
        },
        "board": {
            "kind": "firmware",
            "path": "firmware/board",
            "may_use": ["codec", "tags"],
            "lanes": ["esp"],
            "ships": ["codec", "base"],
        },
    },
}


class Graph(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        path = self.root / "projects.json"
        path.write_text(json.dumps(TABLE), encoding="utf-8")
        self.table = project_graph.load_table(path)

    def test_a_project_is_built_from_the_libraries_it_uses_transitively(self) -> None:
        self.assertEqual(C.built_from(self.table, "wheel"), ["base", "codec", "dsp", "tags"])

    def test_a_firmware_project_is_built_from_what_it_ships(self) -> None:
        self.assertEqual(C.built_from(self.table, "board"), ["base", "codec"])


class Problems(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        (self.root / "projects.json").write_text(json.dumps(TABLE), encoding="utf-8")
        self.table = project_graph.load_table(self.root / "projects.json")
        (self.root / ".github" / "workflows").mkdir(parents=True)
        self.files = [
            "libs/base/a.cpp",
            "libs/dsp/a.cpp",
            "libs/codec/a.cpp",
            "libs/tags/a.cpp",
            "bindings/wheel/a.py",
            "firmware/board/a.c",
            "CMakeLists.txt",
            "cmake/x.cmake",
        ]

    def workflow(self, name: str, paths: list[str]) -> None:
        body = "on:\n  push:\n    paths:\n" + "".join(f'      - "{p}"\n' for p in paths)
        (self.root / ".github" / "workflows" / name).write_text(body, encoding="utf-8")

    def run_problems(self) -> list[str]:
        saved = C.BUILDERS
        C.BUILDERS = (C.Builder("wheels.yml", ("wheel",)),)
        self.addCleanup(setattr, C, "BUILDERS", saved)
        return C.problems(self.root, self.table, self.files)

    def test_a_filter_that_names_every_tree_and_matches_files_passes(self) -> None:
        self.workflow(
            "wheels.yml",
            [
                "bindings/wheel/**",
                "libs/base/**",
                "libs/dsp/**",
                "libs/codec/**",
                "libs/tags/**",
                "cmake/**",
                "CMakeLists.txt",
            ],
        )
        self.assertEqual(self.run_problems(), [])

    def test_a_pattern_that_matches_nothing_is_named_with_its_line(self) -> None:
        self.workflow(
            "wheels.yml",
            [
                "bindings/wheel/**",
                "libs/base/**",
                "libs/dsp/**",
                "libs/codec/**",
                "libs/tags/**",
                "cmake/**",
                "CMakeLists.txt",
                "src/moved/**",
            ],
        )
        got = self.run_problems()
        self.assertEqual(len(got), 1)
        self.assertIn(
            "wheels.yml:11: the paths pattern src/moved/** matches no tracked file", got[0]
        )

    def test_a_library_the_project_is_built_from_and_the_filter_lacks_is_named(self) -> None:
        self.workflow(
            "wheels.yml",
            ["bindings/wheel/**", "libs/base/**", "libs/dsp/**", "cmake/**", "CMakeLists.txt"],
        )
        got = "\n".join(self.run_problems())
        self.assertIn("no path filter for libs/codec/** (codec, which wheel is built from)", got)
        self.assertIn("no path filter for libs/tags/** (tags, which wheel is built from)", got)
        self.assertNotIn("libs/dsp/**", got)

    def test_the_build_files_are_required_of_a_cmake_project(self) -> None:
        self.workflow(
            "wheels.yml",
            ["bindings/wheel/**", "libs/base/**", "libs/dsp/**", "libs/codec/**", "libs/tags/**"],
        )
        got = "\n".join(self.run_problems())
        self.assertIn("no path filter for cmake/**", got)
        self.assertIn("no path filter for CMakeLists.txt", got)

    def test_a_builder_that_is_not_a_workflow_is_named(self) -> None:
        got = self.run_problems()
        self.assertEqual(got, [".github/workflows/wheels.yml is a builder and is not a workflow"])

    def test_the_command_line_reports_and_exits_nonzero(self) -> None:
        out = io.StringIO()
        with redirect_stdout(out):
            code = C.main(["--root", str(Path(__file__).resolve().parents[2])])
        self.assertIn("check_workflow_paths:", out.getvalue())
        self.assertEqual(code, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
