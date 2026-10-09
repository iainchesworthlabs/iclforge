"""Unit tests for check_layering.py, the dependency check over the projects of the tree.

stdlib `unittest`, for the reason the script is stdlib-only: this runs in _static.yml's static job.

Each test builds a small temporary tree (there is no git repository in it, so the script lists it by
walking it) with a table beside it, and runs the check over it, so the cases are the rules the
script's header states: an allowed include passes; a forbidden one fails with the file and the line;
a private header is found by the tail of its path and a quoted include by its own directory; a
system header is not the project's; a project's tests and fuzz targets may use any library but no
program; a link line is held to the table as an include is; a library may use only libraries, a
program no other program; an internal project is neither installed nor included by an installed
header; a named exception excuses its edge and fails when it excuses none; a known debt is reported
and does not fail, and a debt that is no longer there does.
"""

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_layering


def _write(root: Path, relative: str, text: str) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


BASE = {"kind": "library", "path": "libs/base", "may_use": []}
CODEC = {"kind": "library", "path": "libs/codec", "may_use": ["base"]}
PROGRAM = {"kind": "app", "path": "apps/prog", "may_use": ["base", "codec"]}


class LayeringCheck(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        _write(self.root, "libs/base/include/x/base/bits.hpp", "#pragma once\n")
        _write(
            self.root,
            "libs/codec/src/decode.cpp",
            '#include <vector>\n#include "x/base/bits.hpp"\nint main() { return 0; }\n',
        )

    def table(self, projects: dict, exceptions=None, install_files=None) -> Path:
        # A row names the CI lanes its tree lights; the ones that do not say get core's.
        rows = {name: {"lanes": ["core"], **row} for name, row in projects.items()}
        body: dict = {"lanes": ["core", "linux"], "projects": rows}
        if exceptions is not None:
            body["exceptions"] = exceptions
        if install_files is not None:
            body["install_files"] = install_files
        return _write(self.root, "projects.json", json.dumps(body))

    def run_check(self, projects: dict, exceptions=None, install_files=None, debt=None):
        argv = [
            "--root",
            str(self.root),
            "--table",
            str(self.table(projects, exceptions, install_files)),
        ]
        argv += ["--debt", str(self.root / (debt or "no-debt"))]
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = check_layering.main(argv)
        return code, buffer.getvalue()

    def test_allowed_include_passes(self) -> None:
        code, out = self.run_check({"base": BASE, "codec": CODEC})
        self.assertEqual(code, 0, out)
        self.assertIn("1 includes", out)

    def test_forbidden_include_fails_and_names_file_and_line(self) -> None:
        code, out = self.run_check({"base": BASE, "codec": {**CODEC, "may_use": []}})
        self.assertEqual(code, 1)
        self.assertIn("file=libs/codec/src/decode.cpp,line=2", out)
        self.assertIn("codec may not include base", out)

    def test_system_and_unresolved_includes_are_not_edges(self) -> None:
        _write(self.root, "libs/codec/src/other.cpp", '#include <string>\n#include "fmt/core.h"\n')
        code, out = self.run_check({"base": BASE, "codec": CODEC})
        self.assertEqual(code, 0, out)
        self.assertIn("1 includes", out)

    def test_private_header_is_found_by_the_tail_of_its_path(self) -> None:
        _write(self.root, "libs/base/src/detail/tables.hpp", "#pragma once\n")
        _write(self.root, "libs/codec/src/use.cpp", '#include "detail/tables.hpp"\n')
        code, out = self.run_check({"base": BASE, "codec": {**CODEC, "may_use": []}})
        self.assertEqual(code, 1)
        self.assertIn("libs/codec/src/use.cpp", out)

    def test_quoted_include_beside_the_file_stays_in_its_project(self) -> None:
        _write(self.root, "libs/codec/src/helper.hpp", "#pragma once\n")
        _write(self.root, "libs/codec/src/use.cpp", '#include "helper.hpp"\n')
        code, out = self.run_check({"base": BASE, "codec": CODEC})
        self.assertEqual(code, 0, out)

    def test_a_tree_the_table_lacks_fails(self) -> None:
        code, out = self.run_check({"base": BASE})
        self.assertEqual(code, 1)
        self.assertIn("libs/codec/ holds C/C++ or CMake files of no project", out)

    def test_a_project_with_no_files_fails(self) -> None:
        ghost = {"kind": "library", "path": "libs/ghost", "may_use": []}
        code, out = self.run_check({"base": BASE, "codec": CODEC, "ghost": ghost})
        self.assertEqual(code, 1)
        self.assertIn("nothing is filed under libs/ghost", out)

    def test_a_cycle_in_the_table_fails(self) -> None:
        code, out = self.run_check({"base": {**BASE, "may_use": ["codec"]}, "codec": CODEC})
        self.assertEqual(code, 1)
        self.assertIn("cycle in the table: base -> codec", out)

    def test_a_row_naming_an_unknown_project_fails(self) -> None:
        code, out = self.run_check(
            {"base": BASE, "codec": {**CODEC, "may_use": ["base", "nowhere"]}}
        )
        self.assertEqual(code, 1)
        self.assertIn("nowhere, which is not a project of the table", out)

    def test_a_library_may_use_only_libraries(self) -> None:
        _write(self.root, "apps/prog/src/main.cpp", "int main() {}\n")
        code, out = self.run_check(
            {"base": BASE, "codec": {**CODEC, "may_use": ["base", "prog"]}, "prog": PROGRAM}
        )
        self.assertEqual(code, 1)
        self.assertIn("codec (library) may use prog (app)", out)

    def test_a_program_may_not_use_another_program(self) -> None:
        _write(self.root, "apps/prog/src/main.cpp", "int main() {}\n")
        _write(self.root, "apps/other/src/main.cpp", "int main() {}\n")
        other = {"kind": "app", "path": "apps/other", "may_use": ["prog"]}
        code, out = self.run_check({"base": BASE, "codec": CODEC, "prog": PROGRAM, "other": other})
        self.assertEqual(code, 1)
        self.assertIn("other (app) may use prog (app)", out)

    def test_a_program_including_another_programs_header_fails(self) -> None:
        _write(self.root, "apps/prog/src/api.hpp", "#pragma once\n")
        _write(self.root, "apps/other/src/main.cpp", '#include "api.hpp"\n')
        other = {"kind": "app", "path": "apps/other", "may_use": []}
        code, out = self.run_check({"base": BASE, "codec": CODEC, "prog": PROGRAM, "other": other})
        self.assertEqual(code, 1)
        self.assertIn("other may not include prog", out)

    def test_tests_and_fuzz_may_use_any_library_but_no_program(self) -> None:
        _write(self.root, "libs/base/tests/test_bits.cpp", '#include "x/codec/decode.hpp"\n')
        _write(self.root, "libs/codec/include/x/codec/decode.hpp", "#pragma once\n")
        _write(self.root, "libs/base/fuzz/fuzz_bits.cpp", '#include "x/codec/decode.hpp"\n')
        code, out = self.run_check({"base": BASE, "codec": CODEC})
        self.assertEqual(code, 0, out)
        _write(self.root, "apps/prog/src/api.hpp", "#pragma once\n")
        _write(self.root, "libs/base/tests/test_prog.cpp", '#include "api.hpp"\n')
        code, out = self.run_check({"base": BASE, "codec": CODEC, "prog": PROGRAM})
        self.assertEqual(code, 1)
        self.assertIn("base may not include prog", out)

    def test_a_programs_tests_beside_it_are_its_consumers(self) -> None:
        _write(
            self.root,
            "apps/prog/cli/tests/test_it.cpp",
            '#include "x/base/bits.hpp"\n#include "x/codec/decode.hpp"\n',
        )
        _write(self.root, "libs/codec/include/x/codec/decode.hpp", "#pragma once\n")
        code, out = self.run_check(
            {"base": BASE, "codec": {**CODEC}, "prog": {**PROGRAM, "may_use": []}}
        )
        self.assertEqual(code, 0, out)

    def test_a_link_line_is_held_to_the_table(self) -> None:
        _write(
            self.root,
            "libs/base/CMakeLists.txt",
            "add_library(base_lib STATIC a.cpp)\nadd_library(x::base ALIAS base_lib)\n",
        )
        _write(
            self.root,
            "libs/codec/CMakeLists.txt",
            "add_library(codec_lib STATIC a.cpp)\n"
            "target_link_libraries(codec_lib PUBLIC x::base)\n",
        )
        code, out = self.run_check({"base": BASE, "codec": CODEC})
        self.assertEqual(code, 0, out)
        self.assertIn("1 link lines", out)
        code, out = self.run_check({"base": BASE, "codec": {**CODEC, "may_use": []}})
        self.assertEqual(code, 1)
        self.assertIn("codec may not link base (x::base)", out)

    def test_iclforge_add_library_depends_is_a_link_line(self) -> None:
        _write(self.root, "libs/base/CMakeLists.txt", "iclforge_add_library(base SOURCES a.cpp)\n")
        _write(
            self.root,
            "libs/codec/CMakeLists.txt",
            "iclforge_add_library(codec SOURCES a.cpp DEPENDS base)\n",
        )
        code, out = self.run_check({"base": BASE, "codec": {**CODEC, "may_use": []}})
        self.assertEqual(code, 1)
        self.assertIn("codec may not link base (iclforge::base)", out)

    def test_a_comment_is_not_a_link_line(self) -> None:
        _write(self.root, "libs/base/CMakeLists.txt", "add_library(base_lib STATIC a.cpp)\n")
        _write(
            self.root,
            "libs/codec/CMakeLists.txt",
            "# target_link_libraries(codec_lib PUBLIC base_lib)\n"
            "add_library(codec_lib STATIC a.cpp)\n",
        )
        code, out = self.run_check({"base": BASE, "codec": {**CODEC, "may_use": []}})
        self.assertEqual(code, 1, out)  # the include is still forbidden
        self.assertNotIn("link base", out)

    def test_an_exception_excuses_its_edge(self) -> None:
        exception = [
            {"from": "codec", "to": "base", "paths": ["libs/codec/src/"], "why": "for now"}
        ]
        code, out = self.run_check(
            {"base": BASE, "codec": {**CODEC, "may_use": []}}, exceptions=exception
        )
        self.assertEqual(code, 0, out)
        self.assertIn("1 excused by 1 exceptions", out)

    def test_an_exception_for_other_paths_does_not(self) -> None:
        exception = [{"from": "codec", "to": "base", "paths": ["libs/codec/tests/"], "why": "x"}]
        code, out = self.run_check(
            {"base": BASE, "codec": {**CODEC, "may_use": []}}, exceptions=exception
        )
        self.assertEqual(code, 1)
        self.assertIn("codec may not include base", out)
        self.assertIn("the exception codec -> base excuses no edge any more", out)

    def test_an_exception_that_excuses_nothing_fails(self) -> None:
        exception = [{"from": "codec", "to": "base", "why": "left over"}]
        code, out = self.run_check({"base": BASE, "codec": CODEC}, exceptions=exception)
        self.assertEqual(code, 1)
        self.assertIn("the exception codec -> base excuses no edge any more: delete it", out)

    def test_an_exception_gives_a_reason(self) -> None:
        exception = [{"from": "codec", "to": "base"}]
        code, out = self.run_check(
            {"base": BASE, "codec": {**CODEC, "may_use": []}}, exceptions=exception
        )
        self.assertEqual(code, 1)
        self.assertIn("the exception codec -> base gives no reason", out)

    def test_an_installed_header_including_an_internal_project_fails(self) -> None:
        _write(self.root, "libs/inner/include/x/inner/i.hpp", "#pragma once\n")
        _write(self.root, "libs/codec/include/x/codec/api.hpp", '#include "x/inner/i.hpp"\n')
        inner = {"kind": "library", "path": "libs/inner", "internal": True, "may_use": []}
        code, out = self.run_check(
            {"base": BASE, "inner": inner, "codec": {**CODEC, "may_use": ["base", "inner"]}}
        )
        self.assertEqual(code, 1)
        self.assertIn("an installed header of codec includes x/inner/i.hpp", out)
        # a private source may
        (self.root / "libs/codec/include/x/codec/api.hpp").unlink()
        _write(self.root, "libs/codec/src/use.cpp", '#include "x/inner/i.hpp"\n')
        code, out = self.run_check(
            {"base": BASE, "inner": inner, "codec": {**CODEC, "may_use": ["base", "inner"]}}
        )
        self.assertEqual(code, 0, out)

    def test_an_install_naming_an_internal_target_fails(self) -> None:
        _write(self.root, "libs/inner/CMakeLists.txt", "add_library(inner_lib STATIC a.cpp)\n")
        _write(self.root, "cmake/Install.cmake", "install(TARGETS codec_lib inner_lib)\n")
        inner = {"kind": "library", "path": "libs/inner", "internal": True, "may_use": []}
        code, out = self.run_check(
            {"base": BASE, "inner": inner, "codec": CODEC}, install_files=["cmake/Install.cmake"]
        )
        self.assertEqual(code, 1)
        self.assertIn("cmake/Install.cmake:1: install() names inner_lib", out)

    def test_an_app_library_is_internal(self) -> None:
        _write(self.root, "apps/shared/m/src/m.cpp", "int m() { return 0; }\n")
        shared = {"kind": "app-library", "path": "apps/shared/m", "may_use": []}
        code, out = self.run_check({"base": BASE, "codec": CODEC, "m": shared})
        self.assertEqual(code, 1)
        self.assertIn("m is an app-library and so internal", out)

    def test_known_debt_is_reported_and_does_not_fail(self) -> None:
        _write(
            self.root,
            "debt/c1.txt",
            "# a cut\n\nlibs/codec/src/decode.cpp x/base/bits.hpp  # codec -> base\n",
        )
        code, out = self.run_check({"base": BASE, "codec": {**CODEC, "may_use": []}}, debt="debt")
        self.assertEqual(code, 0, out)
        self.assertIn("1 known debts", out)

    def test_debt_that_is_no_longer_forbidden_fails(self) -> None:
        _write(self.root, "debt/c1.txt", "libs/codec/src/decode.cpp x/base/bits.hpp\n")
        code, out = self.run_check({"base": BASE, "codec": CODEC}, debt="debt")
        self.assertEqual(code, 1)
        self.assertIn("c1.txt lists libs/codec/src/decode.cpp including x/base/bits.hpp", out)

    def test_a_new_forbidden_include_is_not_covered_by_a_debt_for_another(self) -> None:
        _write(self.root, "debt/c1.txt", "libs/codec/src/decode.cpp x/base/bits.hpp\n")
        _write(self.root, "libs/codec/src/new.cpp", '#include "x/base/bits.hpp"\n')
        code, out = self.run_check({"base": BASE, "codec": {**CODEC, "may_use": []}}, debt="debt")
        self.assertEqual(code, 1)
        self.assertIn("libs/codec/src/new.cpp", out)

    def test_a_readme_beside_the_debt_files_is_not_read_as_one(self) -> None:
        _write(self.root, "debt/README.md", "# Known debts\n\nonly-one-field\n")
        code, out = self.run_check({"base": BASE, "codec": CODEC}, debt="debt")
        self.assertEqual(code, 0, out)
        self.assertIn("0 known debts", out)

    def test_malformed_debt_line_is_an_error(self) -> None:
        _write(self.root, "debt/c1.txt", "only-one-field\n")
        with self.assertRaises(ValueError):
            self.run_check({"base": BASE, "codec": {**CODEC, "may_use": []}}, debt="debt")

    def test_edges_flag_prints_the_edges_and_stops(self) -> None:
        argv = [
            "--root",
            str(self.root),
            "--table",
            str(self.table({"base": BASE, "codec": {**CODEC, "may_use": []}})),
        ]
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = check_layering.main([*argv, "--edges"])
        self.assertEqual(code, 0)
        self.assertEqual(buffer.getvalue().strip(), "codec -> base (include): 1")

    def test_an_empty_tree_fails_rather_than_passing_vacuously(self) -> None:
        with tempfile.TemporaryDirectory() as empty:
            table = _write(Path(empty), "projects.json", json.dumps({"projects": {"base": BASE}}))
            buffer = io.StringIO()
            with redirect_stdout(buffer):
                code = check_layering.main(["--root", empty, "--table", str(table)])
            self.assertEqual(code, 1)
            self.assertIn("nothing was checked", buffer.getvalue())

    def test_a_project_that_lights_no_lane_fails(self) -> None:
        code, out = self.run_check({"base": BASE, "codec": {**CODEC, "lanes": []}})
        self.assertEqual(code, 1)
        self.assertIn("codec lights no lane", out)

    def test_a_lane_the_table_does_not_list_fails(self) -> None:
        code, out = self.run_check({"base": {**BASE, "lanes": ["core", "mars"]}, "codec": CODEC})
        self.assertEqual(code, 1)
        self.assertIn("base names the lane mars", out)

    def test_a_firmware_project_may_use_the_firmware_library_it_is_built_on(self) -> None:
        _write(self.root, "fw/component/c.cpp", "int c() { return 0; }\n")
        _write(self.root, "fw/board/main.cpp", "int main() { return 0; }\n")
        component = {
            "kind": "firmware-library",
            "path": "fw/component",
            "may_use": ["base"],
            "ships": ["base"],
        }
        board = {"kind": "firmware", "path": "fw/board", "may_use": ["base", "component"]}
        code, out = self.run_check(
            {"base": BASE, "codec": CODEC, "component": component, "board": board}
        )
        self.assertEqual(code, 0, out)

    def test_only_firmware_uses_a_firmware_library_and_never_another_firmware(self) -> None:
        _write(self.root, "fw/component/c.cpp", "int c() { return 0; }\n")
        _write(self.root, "fw/board/main.cpp", "int main() { return 0; }\n")
        _write(self.root, "fw/other/main.cpp", "int main() { return 0; }\n")
        component = {"kind": "firmware-library", "path": "fw/component", "may_use": ["base"]}
        board = {"kind": "firmware", "path": "fw/board", "may_use": ["base"]}
        other = {"kind": "firmware", "path": "fw/other", "may_use": ["board"]}
        code, out = self.run_check(
            {
                "base": BASE,
                "codec": {**CODEC, "may_use": ["base", "component"]},
                "component": {**component, "may_use": ["base", "board"]},
                "board": board,
                "other": other,
            }
        )
        self.assertEqual(code, 1)
        self.assertIn("codec (library) may use component (firmware-library)", out)
        self.assertIn("component (firmware-library) may use board (firmware)", out)
        self.assertIn("other (firmware) may use board (firmware)", out)

    def test_only_firmware_ships_libraries_and_only_libraries(self) -> None:
        board = {"kind": "firmware", "path": "fw/board", "may_use": ["base"], "lanes": ["linux"]}
        _write(self.root, "fw/board/main.cpp", "int main() { return 0; }\n")
        code, out = self.run_check(
            {
                "base": {**BASE, "ships": ["base"]},
                "codec": CODEC,
                "board": {**board, "ships": ["codec", "board"]},
            }
        )
        self.assertEqual(code, 1)
        self.assertIn("base ships libraries, and only firmware does", out)
        self.assertIn("board ships board, which is not a library of the table", out)

    def test_a_table_with_no_lane_list_fails(self) -> None:
        path = self.table({"base": BASE, "codec": CODEC})
        body = json.loads(path.read_text(encoding="utf-8"))
        del body["lanes"]
        path.write_text(json.dumps(body), encoding="utf-8")
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = check_layering.main(
                ["--root", str(self.root), "--table", str(path), "--debt", str(self.root / "none")]
            )
        self.assertEqual(code, 1)
        self.assertIn("the table lists no lanes", buffer.getvalue())

    def test_an_exception_excuses_only_the_files_it_reaches(self) -> None:
        _write(self.root, "libs/base/src/detail/tables.hpp", "#pragma once\n")
        _write(self.root, "libs/base/src/other.hpp", "#pragma once\n")
        _write(
            self.root,
            "libs/codec/src/use.cpp",
            '#include "detail/tables.hpp"\n#include "other.hpp"\n',
        )
        reach = [
            {
                "from": "codec",
                "to": "base",
                "paths": ["libs/codec/src/"],
                "why": "x",
                "to_paths": ["libs/base/src/detail/"],
            }
        ]
        code, out = self.run_check(
            {"base": BASE, "codec": {**CODEC, "may_use": []}}, exceptions=reach
        )
        self.assertEqual(code, 1)
        self.assertIn("(other.hpp)", out)
        self.assertNotIn("(detail/tables.hpp)", out)

    def test_to_paths_must_be_in_the_target_and_hold_files(self) -> None:
        reach = [
            {
                "from": "codec",
                "to": "base",
                "why": "x",
                "paths": ["libs/codec/src/"],
                "to_paths": ["libs/codec/", "libs/base/nothing/"],
            }
        ]
        code, out = self.run_check(
            {"base": BASE, "codec": {**CODEC, "may_use": []}}, exceptions=reach
        )
        self.assertEqual(code, 1)
        self.assertIn("reaches libs/codec/, which is not in libs/base", out)
        self.assertIn("reaches libs/base/nothing/, which nothing is filed under", out)

    def test_a_reached_file_that_no_excused_include_names_fails(self) -> None:
        _write(self.root, "libs/base/src/detail/tables.hpp", "#pragma once\n")
        _write(self.root, "libs/base/src/gone.hpp", "#pragma once\n")
        _write(self.root, "libs/codec/src/use.cpp", '#include "detail/tables.hpp"\n')
        reach = [
            {
                "from": "codec",
                "to": "base",
                "paths": ["libs/codec/src/"],
                "why": "x",
                "to_paths": ["libs/base/src/detail/tables.hpp", "libs/base/src/gone.hpp"],
            }
        ]
        code, out = self.run_check(
            {"base": BASE, "codec": {**CODEC, "may_use": []}}, exceptions=reach
        )
        self.assertEqual(code, 1)
        self.assertIn(
            "reaches libs/base/src/gone.hpp, which no excused include names any more", out
        )
        self.assertNotIn("reaches libs/base/src/detail/tables.hpp", out)

    def test_an_include_of_a_private_header_of_another_project_fails(self) -> None:
        _write(self.root, "libs/base/src/detail/tables.hpp", "#pragma once\n")
        _write(self.root, "libs/codec/src/use.cpp", '#include "detail/tables.hpp"\n')
        code, out = self.run_check({"base": BASE, "codec": CODEC})
        self.assertEqual(code, 1)
        self.assertIn("file=libs/codec/src/use.cpp,line=1", out)
        self.assertIn("a header of base that is private to it", out)

    def test_a_directory_the_row_exposes_may_be_included(self) -> None:
        _write(self.root, "libs/base/src/detail/tables.hpp", "#pragma once\n")
        _write(self.root, "libs/codec/src/use.cpp", '#include "detail/tables.hpp"\n')
        shared = {**BASE, "exposes": ["libs/base/src/detail/"]}
        code, out = self.run_check({"base": shared, "codec": CODEC})
        self.assertEqual(code, 0, out)

    def test_a_private_include_can_be_a_known_debt_and_a_gone_one_fails(self) -> None:
        _write(self.root, "libs/base/src/detail/tables.hpp", "#pragma once\n")
        _write(self.root, "libs/codec/src/use.cpp", '#include "detail/tables.hpp"\n')
        _write(self.root, "debt/c1.txt", "libs/codec/src/use.cpp detail/tables.hpp\n")
        code, out = self.run_check({"base": BASE, "codec": CODEC}, debt="debt")
        self.assertEqual(code, 0, out)
        self.assertIn("1 known debts", out)
        _write(self.root, "libs/codec/src/use.cpp", "// no include any more\n")
        code, out = self.run_check({"base": BASE, "codec": CODEC}, debt="debt")
        self.assertEqual(code, 1)
        self.assertIn("no longer a forbidden or private include", out)

    def test_what_a_project_exposes_is_in_it_and_holds_files(self) -> None:
        shared = {**BASE, "exposes": ["libs/codec/src/", "libs/base/nothing/"]}
        code, out = self.run_check({"base": shared, "codec": CODEC})
        self.assertEqual(code, 1)
        self.assertIn("base exposes libs/codec/src/, which is not in libs/base", out)
        self.assertIn("base exposes libs/base/nothing/, which nothing is filed under", out)


class RealTable(unittest.TestCase):
    """The committed table is a table the check accepts, whatever the tree holds."""

    def test_table_is_loadable_acyclic_and_obeys_the_kinds(self) -> None:
        table = check_layering.load_table(check_layering.DEFAULT_TABLE)
        graph = {name: list(p.may_use) for name, p in table.projects.items()}
        self.assertFalse(check_layering.cycles(graph))
        for name, project in table.projects.items():
            self.assertIn(project.kind, check_layering.ALLOWED, name)
            self.assertNotIn(name, project.may_use)
            for used in project.may_use:
                self.assertIn(used, table.projects, f"{name} uses {used}")
                self.assertIn(
                    table.projects[used].kind,
                    check_layering.ALLOWED[project.kind],
                    f"{name} uses {used}",
                )

    def test_every_exception_names_projects_and_a_reason(self) -> None:
        table = check_layering.load_table(check_layering.DEFAULT_TABLE)
        for e in table.exceptions:
            self.assertIn(e.source, table.projects)
            self.assertIn(e.target, table.projects)
            self.assertTrue(e.why)


if __name__ == "__main__":
    unittest.main()
