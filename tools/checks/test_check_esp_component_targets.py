"""Unit tests for check_esp_component_targets.py, the ESP-IDF component's link-target check.

stdlib `unittest`, for the reason the script is stdlib-only: this runs in _static.yml's static job.

The cases are the rules the script's header states, and the incident it exists for. The small
trees are in memory: a component, a root and two libraries, written the way the real ones are, so
that each rule can be broken on its own. The real tree is held to the same check, and two of its
files are put back the way they were before the pull request that fixed the incident, to show
that the check fails on them (the root added libs/lyrics only for a build with neither
minimum-footprint profile, and the sink's player linked iclforge::lyrics; pull request #1344 made
the root add it for a sink too).
"""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_esp_component_targets as cect

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = "firmware/esp-idf/comp"
COMPONENT_FILE = f"{COMPONENT}/CMakeLists.txt"

# The component of the small trees: a sink option turns the player-only half on, the profile is
# always minimal, and the root is pulled in by path as the real component does.
COMPONENT_TEXT = """\
set(MINIMAL ON CACHE BOOL "" FORCE)
if(CONFIG_SINK)
    set(PLAYER_ONLY ON CACHE BOOL "" FORCE)
endif()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../.." build)
target_link_libraries(${COMPONENT_LIB} PUBLIC iclforge::base)
if(CONFIG_SINK)
    target_link_libraries(${COMPONENT_LIB} PUBLIC iclforge::lyrics)
endif()
"""
LYRICS_LINK_LINE = 8

ROOT_TEXT = """\
add_subdirectory(libs/base)
if((NOT MINIMAL) OR PLAYER_ONLY)
    add_subdirectory(libs/lyrics)
endif()
"""

BASE_TEXT = "add_library(iclforge::base ALIAS iclforge_base)\n"
LYRICS_TEXT = """\
if(MINIMAL AND NOT PLAYER_ONLY)
    return()
endif()
add_library(iclforge::lyrics ALIAS iclforge_lyrics)
"""


def tree(changes: dict[str, str] | None = None) -> cect.Tree:
    """The small tree, with the files named by `changes` (path -> text) replaced or added."""
    files = {
        COMPONENT_FILE: COMPONENT_TEXT,
        "CMakeLists.txt": ROOT_TEXT,
        "libs/base/CMakeLists.txt": BASE_TEXT,
        "libs/lyrics/CMakeLists.txt": LYRICS_TEXT,
    }
    files.update(changes or {})
    return cect.Tree(files=files)


def findings(t: cect.Tree) -> list[cect.Finding]:
    return cect.check(t, COMPONENT)[0]


def evaluate(text: str, **variables: str) -> bool:
    """The truth of one if() condition, with the named variables set."""
    interp = cect.Interpreter(cect.Tree(files={}), "/repo/c/CMakeLists.txt", {})
    command = cect.tokenize(f"if({text})")[0]
    scope = cect.Scope(dict(variables))
    return interp.condition(interp.arguments(command, scope), scope, "test:1")


class Tokenizer(unittest.TestCase):
    def test_comments_quoted_arguments_and_nesting_are_read(self) -> None:
        commands = cect.tokenize(
            "# a comment ( with ) parens\r\n"
            'set(A "x ) # y" CACHE STRING "doc")   # trailing\r\n'
            "#[[ a bracket\n comment ]]\n"
            "if((A OR B) AND NOT C)\n"
            "endif()\n"
        )
        self.assertEqual([c.name for c in commands], ["set", "if", "endif"])
        self.assertEqual(commands[0].line, 2)
        self.assertEqual(commands[0].args[1].text, "x ) # y")
        self.assertTrue(commands[0].args[1].quoted)
        self.assertEqual(commands[1].line, 5)
        self.assertEqual(
            [a.text for a in commands[1].args], ["(", "A", "OR", "B", ")", "AND", "NOT", "C"])

    def test_a_command_not_closed_is_a_parse_error(self) -> None:
        with self.assertRaises(cect.ParseError):
            cect.tokenize("set(A\n")


class Conditions(unittest.TestCase):
    def test_and_binds_tighter_than_or_and_not_tighter_than_and(self) -> None:
        self.assertTrue(evaluate("A OR B AND C", A="ON", B="OFF", C="OFF"))
        self.assertFalse(evaluate("(A OR B) AND C", A="ON", B="OFF", C="OFF"))
        self.assertTrue(evaluate("NOT A AND B", A="OFF", B="ON"))
        self.assertFalse(evaluate("NOT (A AND B)", A="ON", B="ON"))

    def test_constants_variables_and_strings(self) -> None:
        self.assertTrue(evaluate("CONFIG_X", CONFIG_X="y"))
        self.assertFalse(evaluate("CONFIG_X"))  # not defined
        self.assertFalse(evaluate("X", X="0"))
        self.assertFalse(evaluate("X", X="X-NOTFOUND"))
        self.assertTrue(evaluate("X", X="some text"))
        self.assertFalse(evaluate('"X"', X="ON"))  # a quoted argument is not a variable name
        self.assertTrue(evaluate("ON"))
        self.assertTrue(evaluate("2"))

    def test_comparisons(self) -> None:
        self.assertTrue(evaluate('P STREQUAL "decoder"', P="decoder"))
        self.assertFalse(evaluate("P STREQUAL decoder", P="encoder"))
        self.assertTrue(evaluate('P MATCHES "^dec"', P="decoder"))
        self.assertTrue(evaluate("V VERSION_LESS 3.10", V="3.9.1"))
        self.assertTrue(evaluate("N GREATER 2", N="10"))
        self.assertTrue(evaluate("A IN_LIST L", A="b", L="a;b;c"))
        self.assertTrue(evaluate("NOT DEFINED NOPE"))
        self.assertTrue(evaluate("DEFINED X", X=""))

    def test_a_condition_it_cannot_read_stops_the_run_with_its_place(self) -> None:
        with self.assertRaises(cect.Unsupported) as caught:
            evaluate("A FROBNICATE B", A="1", B="2")
        self.assertIn("test:1", str(caught.exception))


class SmallTrees(unittest.TestCase):
    def test_a_clean_tree_has_no_finding(self) -> None:
        found, runs = cect.check(tree(), COMPONENT)
        self.assertEqual(found, [])
        self.assertEqual(len(runs), 2)  # CONFIG_SINK set or not
        self.assertEqual(
            sorted({link.target for run in runs for link in run.links}),
            ["iclforge::base", "iclforge::lyrics"])

    def test_a_root_guard_that_leaves_the_directory_out_is_found(self) -> None:
        # The incident: the root adds libs/lyrics for a build with no minimum-footprint profile,
        # and the component's profile is one.
        root = (
            "add_subdirectory(libs/base)\n"
            "if(NOT MINIMAL)\n"
            "    add_subdirectory(libs/lyrics)\n"
            "endif()\n")
        found = findings(tree({"CMakeLists.txt": root}))
        self.assertEqual(len(found), 1)
        finding = found[0]
        self.assertEqual(finding.file, COMPONENT_FILE)
        self.assertEqual(finding.line, LYRICS_LINK_LINE)
        for expected in (
            "iclforge::lyrics",
            "CONFIG_SINK=y",
            "libs/lyrics/CMakeLists.txt:4 defines it, and that file is not run",
            "CMakeLists.txt:3 would run libs/lyrics, but `if(NOT MINIMAL)` is false (MINIMAL=ON)",
        ):
            self.assertIn(expected, finding.message)

    def test_a_return_in_the_library_before_its_definition_is_found(self) -> None:
        lyrics = "if(MINIMAL)\n    return()\nendif()\nadd_library(iclforge::lyrics ALIAS l)\n"
        found = findings(tree({"libs/lyrics/CMakeLists.txt": lyrics}))
        self.assertEqual(len(found), 1)
        self.assertIn(
            "libs/lyrics/CMakeLists.txt:2 returns first, under `if(MINIMAL)` (MINIMAL=ON)",
            found[0].message)
        self.assertIn("reaches that file but not the line", found[0].message)

    def test_a_definition_in_an_included_file_counts_when_the_include_runs(self) -> None:
        text = (
            "if(MINIMAL)\n"
            '    include("${CMAKE_CURRENT_SOURCE_DIR}/minimal.cmake")\n'
            "    return()\n"
            "endif()\n")
        included = "add_library(iclforge::lyrics ALIAS m)\n"
        self.assertEqual(findings(tree({
            "libs/lyrics/CMakeLists.txt": text, "libs/lyrics/minimal.cmake": included})), [])
        # The same include, behind a condition the component's options do not satisfy.
        found = findings(tree({
            "libs/lyrics/CMakeLists.txt": text.replace("if(MINIMAL)", "if(NOT MINIMAL)"),
            "libs/lyrics/minimal.cmake": included}))
        self.assertEqual(len(found), 1)
        self.assertIn("minimal.cmake:1 defines it, and that file is not run", found[0].message)

    def test_what_iclforge_add_library_makes_follows_the_minimal_profile(self) -> None:
        lyrics = "iclforge_add_library(more)\nadd_library(iclforge::lyrics ALIAS l)\n"

        def component(profile: str, target: str) -> str:
            return (f'set(ICLFORGE_MINIMAL_DECODER {profile} CACHE BOOL "" FORCE)\n'
                    + COMPONENT_TEXT.replace("iclforge::lyrics", target))

        def found(profile: str, target: str) -> list[str]:
            changes = {COMPONENT_FILE: component(profile, target),
                       "libs/lyrics/CMakeLists.txt": lyrics}
            return [f.message.split(" ")[0] for f in findings(tree(changes))]

        # With the profile on the helper makes iclforge::more and iclforge::more_static and no
        # iclforge::more_shared: a link to the shared one is a finding, to the others is not.
        self.assertEqual(found("ON", "iclforge::more_shared"), ["iclforge::more_shared"])
        self.assertEqual(found("ON", "iclforge::more_static"), [])
        self.assertEqual(found("ON", "iclforge::more"), [])
        # With it off the helper makes the shared one too.
        self.assertEqual(found("OFF", "iclforge::more_shared"), [])

    def test_a_link_that_no_combination_reaches_is_a_finding(self) -> None:
        # Every combination with CONFIG_SINK set stops in the root, so the link is never seen.
        root = (
            "if(PLAYER_ONLY)\n"
            '    message(FATAL_ERROR "a sink needs the whole player")\n'
            "endif()\n"
            "add_subdirectory(libs/base)\n"
            "add_subdirectory(libs/lyrics)\n")
        found = findings(tree({"CMakeLists.txt": root}))
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].line, LYRICS_LINK_LINE)
        self.assertIn("no combination of the component's options gets as far", found[0].message)
        self.assertIn("a sink needs the whole player", found[0].message)

    def test_a_combination_the_build_refuses_is_not_a_finding(self) -> None:
        # CONFIG_OTHER is refused outright; the combinations without it are held to the links.
        component = COMPONENT_TEXT + "if(CONFIG_OTHER)\n    set(UNUSED ON)\nendif()\n"
        root = 'if(CONFIG_OTHER)\n    message(FATAL_ERROR "refused")\nendif()\n' + ROOT_TEXT
        found, runs = cect.check(
            tree({COMPONENT_FILE: component, "CMakeLists.txt": root}), COMPONENT)
        self.assertEqual(found, [])
        self.assertEqual(len(runs), 4)
        self.assertEqual(sum(1 for run in runs if run.aborted), 2)

    def test_option_defaults_and_forced_cache_values(self) -> None:
        root = (
            'option(WITH_LYRICS "x" ON)\n'
            "add_subdirectory(libs/base)\n"
            "if(WITH_LYRICS)\n    add_subdirectory(libs/lyrics)\nendif()\n")
        lyrics = "add_library(iclforge::lyrics ALIAS l)\n"
        self.assertEqual(findings(tree({
            "CMakeLists.txt": root, "libs/lyrics/CMakeLists.txt": lyrics})), [])
        # The component forces it off first: option() leaves a cache value alone.
        forced = 'set(WITH_LYRICS OFF CACHE BOOL "" FORCE)\n' + COMPONENT_TEXT
        found = findings(tree({
            "CMakeLists.txt": root, "libs/lyrics/CMakeLists.txt": lyrics, COMPONENT_FILE: forced}))
        self.assertEqual(len(found), 1)
        self.assertIn("(WITH_LYRICS=OFF)", found[0].message)

    def test_elseif_and_else_branches_run_one_at_a_time(self) -> None:
        root = (
            "add_subdirectory(libs/base)\n"
            "if(PLAYER_ONLY AND MINIMAL)\n"
            "    add_subdirectory(libs/lyrics)\n"
            "elseif(MINIMAL)\n"
            '    message(FATAL_ERROR "not here")\n'
            "else()\n"
            '    message(FATAL_ERROR "nor here")\n'
            "endif()\n")
        # CONFIG_SINK unset: PLAYER_ONLY is unset, so the elseif refuses the combination; with it
        # set the first branch runs and the link is held to its definition.
        found, runs = cect.check(tree({"CMakeLists.txt": root}), COMPONENT)
        self.assertEqual(found, [])
        stopped = {run.assignment.get("CONFIG_SINK"): bool(run.aborted) for run in runs}
        self.assertEqual(stopped, {"y": False, None: True})

    def test_a_variable_set_in_a_directory_stays_there_and_the_cache_does_not(self) -> None:
        root = (
            "add_subdirectory(libs/base)\n"
            "add_subdirectory(libs/lyrics)\n"
            "if(LEAKED)\n    message(FATAL_ERROR \"a directory's variable reached its parent\")\n"
            "endif()\n"
            "if(NOT CACHED)\n"
            '    message(FATAL_ERROR "a cache variable did not reach the parent")\n'
            "endif()\n")
        lyrics = ('set(LEAKED ON)\nset(CACHED ON CACHE BOOL "" FORCE)\n'
                  "add_library(iclforge::lyrics ALIAS l)\n")
        found, runs = cect.check(
            tree({"CMakeLists.txt": root, "libs/lyrics/CMakeLists.txt": lyrics}), COMPONENT)
        self.assertEqual(found, [])
        self.assertFalse(any(run.aborted for run in runs))

    def test_a_condition_the_interpreter_cannot_read_is_reported_with_its_place(self) -> None:
        root = "if(MINIMAL FROBNICATE PLAYER_ONLY)\nendif()\n" + ROOT_TEXT
        with self.assertRaises(cect.Unsupported) as caught:
            cect.check(tree({"CMakeLists.txt": root}), COMPONENT)
        self.assertIn("CMakeLists.txt:1", str(caught.exception))

    def test_the_command_line_has_a_usage_error_for_a_missing_component(self) -> None:
        err = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stderr(err):
            self.assertEqual(cect.main(["--root", tmp]), 2)
        self.assertIn("no firmware/esp-idf/iclforge/CMakeLists.txt", err.getvalue())

    def test_the_command_line_reports_a_finding_as_an_annotation_and_exits_one(self) -> None:
        files = {
            COMPONENT_FILE: COMPONENT_TEXT,
            "CMakeLists.txt": "add_subdirectory(libs/base)\n",
            "libs/base/CMakeLists.txt": BASE_TEXT,
            "libs/lyrics/CMakeLists.txt": LYRICS_TEXT,
        }
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp:
            for name, text in files.items():
                path = Path(tmp) / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            with contextlib.redirect_stdout(out):
                status = cect.main(["--root", tmp, "--component", COMPONENT])
        self.assertEqual(status, 1)
        self.assertIn(
            f"::error file={COMPONENT_FILE},line={LYRICS_LINK_LINE}::"
            "check_esp_component_targets: iclforge::lyrics is linked here when CONFIG_SINK=y",
            out.getvalue())


class RealTree(unittest.TestCase):
    """The tree this file is in, held to the check, and put back as it was before #1344."""

    component = cect.COMPONENT
    component_file = f"{cect.COMPONENT}/CMakeLists.txt"

    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_every_link_of_the_component_is_defined_in_every_combination(self) -> None:
        found, runs = cect.check(cect.Tree(root=ROOT), self.component)
        self.assertEqual([f.message for f in found], [])
        configured = [run for run in runs if not run.aborted]
        # The check has seen the sink and the AC-4 shape, and not nothing: the libraries the
        # sink links are defined in each run that links them.
        linked = {link.target for run in configured for link in run.links}
        self.assertTrue(
            {"iclforge::ac3_minimal", "iclforge::sendspin", "iclforge::lyrics",
             "iclforge::ac4_static"} <= linked, linked)
        sink = [run for run in configured if run.assignment.get("CONFIG_ICLFORGE_SENDSPIN") == "y"]
        self.assertTrue(sink)
        for run in sink:
            self.assertIn("iclforge::lyrics", run.targets)
            self.assertIn("iclforge::sendspin", run.targets)

    def test_the_encoder_profile_with_ac4_is_refused_by_the_root_and_not_a_finding(self) -> None:
        _, runs = cect.check(cect.Tree(root=ROOT), self.component)
        refused = [run for run in runs
                   if run.aborted
                   and run.assignment.get("ICLFORGE_ESP_PROFILE") == "encoder"
                   and run.assignment.get("CONFIG_ICLFORGE_AC4") == "y"]
        self.assertTrue(refused)
        self.assertIn("ICLFORGE_MINIMAL_DECODER=ON", refused[0].aborted.message)

    def test_the_root_guard_before_1344_is_a_finding_on_the_lyrics_link(self) -> None:
        text = self.read("CMakeLists.txt")
        fixed = ("if((NOT ICLFORGE_MINIMAL_DECODER AND NOT ICLFORGE_MINIMAL_ENCODER) "
                 "OR ICLFORGE_SENDSPIN_PLAYER_ONLY)")
        self.assertIn(fixed, text, "the root's guard for libs/lyrics has moved: update this test")
        broken = text.replace(
            fixed, "if(NOT ICLFORGE_MINIMAL_DECODER AND NOT ICLFORGE_MINIMAL_ENCODER)")
        found, _ = cect.check(
            cect.Tree(root=ROOT, overrides={"CMakeLists.txt": broken}), self.component)
        self.assertEqual(len(found), 1, [f.message for f in found])
        finding = found[0]
        self.assertEqual(finding.file, self.component_file)
        for expected in (
            "iclforge::lyrics",
            "CONFIG_ICLFORGE_SENDSPIN=y",
            "libs/lyrics/CMakeLists.txt",
            "`if(NOT ICLFORGE_MINIMAL_DECODER AND NOT ICLFORGE_MINIMAL_ENCODER)` is false "
            "(ICLFORGE_MINIMAL_DECODER=ON, ICLFORGE_MINIMAL_ENCODER=OFF)",
        ):
            self.assertIn(expected, finding.message)
        # The line is the component's link, not the root's guard.
        line = self.read(self.component_file).splitlines()[finding.line - 1]
        self.assertIn("iclforge::lyrics", line)

    def test_the_lyrics_directorys_own_return_without_the_player_is_a_finding(self) -> None:
        text = self.read("libs/lyrics/CMakeLists.txt")
        fixed = ("if((ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER) "
                 "AND NOT ICLFORGE_SENDSPIN_PLAYER_ONLY)")
        self.assertIn(fixed, text, "libs/lyrics's guard has moved: update this test")
        broken = text.replace(fixed, "if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)")
        found, _ = cect.check(
            cect.Tree(root=ROOT, overrides={"libs/lyrics/CMakeLists.txt": broken}), self.component)
        self.assertEqual(len(found), 1, [f.message for f in found])
        self.assertIn("returns first, under `if(ICLFORGE_MINIMAL_DECODER OR "
                      "ICLFORGE_MINIMAL_ENCODER)`", found[0].message)

    def test_a_link_to_a_target_nothing_defines_is_a_finding(self) -> None:
        text = self.read(self.component_file)
        link = "target_link_libraries(${COMPONENT_LIB} PUBLIC iclforge::lyrics)"
        self.assertIn(link, text)
        broken = text.replace(link, link.replace("iclforge::lyrics", "iclforge::lyric_reader"))
        found, _ = cect.check(
            cect.Tree(root=ROOT, overrides={self.component_file: broken}), self.component)
        self.assertEqual(len(found), 1, [f.message for f in found])
        self.assertIn("nothing in the tree's CMake files defines iclforge::lyric_reader",
                      found[0].message)

    def test_the_component_helper_file_that_is_not_run_defines_no_target(self) -> None:
        _, runs = cect.check(cect.Tree(root=ROOT), self.component)
        skipped = {path for run in runs for path in run.not_run}
        self.assertEqual(skipped, {f"{cect.VROOT}/{self.component}/opus.cmake"})
        for path in skipped:
            self.assertNotRegex(
                self.read(cect.Tree.rel(path)), r"(add_library|iclforge_add_library)\(\s*iclforge",
                path)

    def test_the_script_runs_clean_from_the_command_line(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = cect.main(["--root", str(ROOT)])
        self.assertEqual(status, 0, out.getvalue())
        self.assertIn("every iclforge:: target it links is defined in each", out.getvalue())


if __name__ == "__main__":
    unittest.main()
