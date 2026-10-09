"""Unit tests for project_graph.py, the table the CI planners and check_layering.py share.

stdlib `unittest`: this runs in _static.yml's static job, which installs nothing.
"""

import io
import json
import sys
import tempfile
import unittest
import unittest.mock
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import project_graph

ROWS = {
    "lanes": ["core", "linux", "esp"],
    "projects": {
        "base": {"kind": "library", "path": "libs/base", "may_use": [], "lanes": ["core"]},
        "codec": {
            "kind": "library",
            "path": "libs/codec",
            "may_use": ["base"],
            "lanes": ["core"],
        },
        "codec-extra": {
            "kind": "library",
            "path": "libs/codec-extra",
            "may_use": ["base"],
            "lanes": ["core"],
        },
        "player": {
            "kind": "app",
            "path": "apps/player",
            "may_use": ["codec"],
            "lanes": ["linux"],
        },
        "board": {
            "kind": "firmware",
            "path": "firmware/board",
            "may_use": ["base"],
            "lanes": ["esp"],
            "ships": ["base"],
        },
        "tests": {"kind": "tests", "path": "tests", "may_use": [], "lanes": ["core"]},
    },
    "exceptions": [
        {
            "from": "codec",
            "to": "board",
            "paths": ["libs/codec/tests/"],
            "to_paths": ["firmware/board/include/"],
            "why": "the codec's tests use the board's host-portable headers",
        },
        {"from": "player", "to": "board", "why": "no files named: any file of the board"},
    ],
}


class Graph(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        path = Path(self._tmp.name) / "projects.json"
        path.write_text(json.dumps(ROWS), encoding="utf-8")
        self.table = project_graph.load_table(path)

    def test_a_path_belongs_to_the_project_with_the_longest_path_above_it(self) -> None:
        self.assertEqual(self.table.project_of("libs/codec/src/x.cpp").name, "codec")
        self.assertEqual(self.table.project_of("libs/codec").name, "codec")

    def test_a_sibling_whose_name_starts_the_same_is_not_the_project(self) -> None:
        self.assertEqual(self.table.project_of("libs/codec-extra/x.cpp").name, "codec-extra")
        self.assertIsNone(self.table.project_of("libs/codecs/x.cpp"))
        self.assertIsNone(self.table.project_of("README.md"))

    def test_the_lanes_ships_and_files_are_read(self) -> None:
        self.assertEqual(self.table.lanes, ("core", "linux", "esp"))
        self.assertEqual(self.table.projects["board"].ships, ("base",))
        self.assertEqual(self.table.exceptions[0].to_paths, ("firmware/board/include/",))

    def test_tests_and_fuzz_directories_are_consumers(self) -> None:
        self.assertTrue(self.table.is_consumer("libs/codec/tests/t.cpp"))
        self.assertTrue(self.table.is_consumer("libs/codec/fuzz/f.cpp"))
        self.assertTrue(self.table.is_consumer("tests/support/s.cpp"))
        self.assertFalse(self.table.is_consumer("libs/codec/src/x.cpp"))

    def test_dependents_are_transitive_and_follow_excused_edges(self) -> None:
        self.assertEqual(self.table.dependents("base"), {"codec", "codec-extra", "player", "board"})
        self.assertEqual(self.table.dependents("codec"), {"player"})
        # codec's tests include the board's headers: the board's change is the codec's, then
        # the player's through it.
        self.assertEqual(self.table.dependents("board"), {"codec", "player"})

    def test_a_file_an_excused_edge_names_is_reached_from_the_includer(self) -> None:
        names = [p.name for p in self.table.reached_from("firmware/board/include/ring.hpp")]
        self.assertEqual(names, ["codec"])
        self.assertEqual(self.table.reached_from("firmware/board/src/main.c"), [])
        # an exception that names no files reaches none in particular
        self.assertEqual(self.table.reached_from("libs/base/x.hpp"), [])

    def test_affected_is_the_project_its_users_and_those_reached(self) -> None:
        self.assertEqual(self.table.affected("libs/codec/src/x.cpp"), {"codec", "player"})
        self.assertEqual(
            self.table.affected("firmware/board/include/ring.hpp"), {"board", "codec", "player"}
        )

    def test_affected_cannot_say_for_an_unknown_path_or_the_test_support(self) -> None:
        self.assertIsNone(self.table.affected("somewhere/else.cpp"))
        self.assertIsNone(self.table.affected("tests/support/s.cpp"))

    def test_the_command_line_prints_the_projects_and_the_lanes(self) -> None:
        for argv, want in (
            (["affected", "libs/codec/src/x.cpp"], "codec,player"),
            (["lanes", "libs/codec/src/x.cpp"], "core,linux"),
        ):
            out = io.StringIO()
            with (
                redirect_stdout(out),
                unittest.mock.patch.object(project_graph, "load_table", return_value=self.table),
            ):
                code = project_graph.main(["project_graph.py", *argv])
            self.assertEqual((code, out.getvalue().strip()), (0, want))


class RealTable(unittest.TestCase):
    """Facts of tools/checks/projects.json that the planners rely on."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.table = project_graph.load_table()

    def test_every_project_names_lanes_from_the_tables_own_list(self) -> None:
        for project in self.table.projects.values():
            with self.subTest(project=project.name):
                self.assertTrue(project.lanes)
                self.assertTrue(set(project.lanes) <= set(self.table.lanes))

    def test_a_change_to_a_codec_blind_library_reaches_every_product(self) -> None:
        reached = self.table.affected("libs/base/include/iclforge/base/bits.hpp")
        self.assertTrue({"ac3", "ac4", "forge", "hearth", "crucible", "python", "rust"} <= reached)

    def test_a_change_to_the_iab_library_does_not_reach_the_bindings_or_the_firmware(self) -> None:
        reached = self.table.affected("libs/iab/src/parser.cpp")
        self.assertIn("forge", reached)
        self.assertFalse({"python", "rust", "js", "esp-idf", "baremetal"} & reached)

    def test_a_change_to_a_program_reaches_only_itself(self) -> None:
        # no app uses another, and no library uses an app: check_layering.py holds the table to it
        self.assertEqual(self.table.affected("apps/hearth/ui/src/main.cpp"), {"hearth"})


if __name__ == "__main__":
    unittest.main()
