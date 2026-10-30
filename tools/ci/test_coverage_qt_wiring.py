"""Unit tests for how the Qt applications' coverage is wired: the -q mode of
tools/checks/coverage_report.sh, the presets it runs on, and the coverage-qt job that calls them.

stdlib only, like the other suites in tools/ci. The script is run against a stand-in `gcovr` that
records its arguments, so what is checked is which trees the -q mode measures and gates and what
it does when one has no data, not gcovr's arithmetic.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "checks" / "coverage_report.sh"
QT_TREES = ("apps/forge/gui/src", "apps/crucible/ui/src", "apps/hearth/ui/src")


def bash() -> str | None:
    for candidate in (shutil.which("bash"), r"C:\Program Files\Git\bin\bash.exe"):
        if candidate and subprocess.run([candidate, "-c", "true"], capture_output=True).returncode == 0:
            return candidate
    return None


BASH = bash()


def text(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def preset(kind: str, name: str) -> dict:
    presets = json.loads(text("CMakePresets.json"))
    for entry in presets[kind]:
        if entry["name"] == name:
            return entry
    raise AssertionError(f"no {kind} {name!r} in CMakePresets.json")


class PresetsAndLabels(unittest.TestCase):
    def test_the_qt_presets_follow_the_plain_coverage_ones(self):
        configure = preset("configurePresets", "config-linux-gcc-coverage-qt")
        self.assertEqual(configure["inherits"], "config-linux-gcc-coverage")
        self.assertEqual(configure["cacheVariables"]["ICLFORGE_BUILD_GUI"], "ON")
        self.assertEqual(configure["cacheVariables"]["ICLFORGE_BUILD_CRUCIBLE"], "ON")
        # A Qt kit is the one thing this preset adds, and it must not turn a library off.
        self.assertNotIn("ICLFORGE_BUILD_HEARTH", configure["cacheVariables"])
        self.assertEqual(
            preset("buildPresets", "build-linux-gcc-coverage-qt")["configurePreset"],
            "config-linux-gcc-coverage-qt",
        )
        test = preset("testPresets", "test-linux-gcc-coverage-qt")
        self.assertEqual(test["configurePreset"], "config-linux-gcc-coverage-qt")
        self.assertEqual(test["environment"]["QT_QPA_PLATFORM"], "offscreen")
        self.assertEqual(test["environment"]["PIPEWIRE_DEBUG"], "0")

    def test_the_test_preset_selects_the_labels_the_window_suites_carry(self):
        regex = re.compile(preset("testPresets", "test-linux-gcc-coverage-qt")["filter"]["include"]["label"])
        declared = {
            "apps/forge/gui/tests/qml.cmake": "gui",
            "apps/crucible/ui/tests/CMakeLists.txt": "crucible-ui",
            "apps/hearth/ui/tests/CMakeLists.txt": "hearth-ui",
            "apps/forge/gui/tests/CMakeLists.txt": "forge-gui",
            "apps/crucible/engine/tests/CMakeLists.txt": "crucible",
            "apps/hearth/engine/tests/CMakeLists.txt": "hearth",
        }
        for relative, label in declared.items():
            with self.subTest(file=relative):
                self.assertRegex(text(relative), rf'(LABELS "{label}"|PROJECT {label} )')
                self.assertTrue(regex.search(label), f"{label} is not selected")
        # The library suites the plain coverage job runs are not pulled in.
        for label in ("ac3", "truehd", "capi", "Performance"):
            with self.subTest(label=label):
                self.assertFalse(regex.search(label))

    def test_every_window_test_binary_that_recompiles_a_controller_links_coverage(self):
        """Linking an instrumented library gives a target the gcov runtime, not instrumentation of
        its own sources (cmake/Coverage.cmake), so a qmltests target without iclforge::coverage
        reports no data for the controllers it recompiles."""
        for relative, target in (
            ("apps/forge/gui/tests/qml.cmake", "forge_gui_qmltests"),
            ("apps/crucible/ui/tests/CMakeLists.txt", "crucible_qmltests"),
            ("apps/hearth/ui/tests/CMakeLists.txt", "hearth_qmltests"),
        ):
            with self.subTest(target=target):
                body = text(relative)
                links = re.findall(rf"target_link_libraries\({target}\b.*?\)\s*\n", body, re.S)
                self.assertTrue(links, f"no target_link_libraries({target} ...)")
                self.assertTrue(any("iclforge::coverage" in link for link in links))


@unittest.skipUnless(BASH, "needs bash")
class QtOnlyMode(unittest.TestCase):
    """coverage_report.sh -q, run against a gcovr that records what it was asked."""

    def run_report(self, trace_paths: list[str], *flags: str):
        work = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, work, True)
        build = work / "build"
        build.mkdir()
        (build / "coverage.json").write_text(
            json.dumps({"files": [{"file_name": f"{p}/x.cpp"} for p in trace_paths]}),
            encoding="utf-8",
        )
        log = work / "gcovr.log"
        fake = work / "bin"
        fake.mkdir()
        gcovr = fake / "gcovr"
        gcovr.write_text(
            "#!/usr/bin/env bash\n"
            'printf "%s\\n" "$*" >> "' + log.as_posix() + '"\n'
            "exit 0\n",
            encoding="utf-8",
        )
        gcovr.chmod(gcovr.stat().st_mode | stat.S_IEXEC)
        env = dict(os.environ, PATH=f"{fake.as_posix()}{os.pathsep}{os.environ['PATH']}")
        out = subprocess.run(
            [BASH, str(SCRIPT), *flags, "-b", build.as_posix()],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        return out, (log.read_text(encoding="utf-8") if log.exists() else "")

    def test_it_measures_and_gates_the_three_trees_and_nothing_else(self):
        out, calls = self.run_report(list(QT_TREES), "-q")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        extraction = calls.splitlines()[0]
        for tree in QT_TREES:
            with self.subTest(tree=tree):
                self.assertIn(f"--filter {tree}/.*", extraction)
                self.assertIn(f"== {tree} (gate: line >=", out.stdout)
        for library in ("libs/", "apps/forge/cli", "apps/hearth/(engine"):
            with self.subTest(library=library):
                self.assertNotIn(library, extraction)
        self.assertNotIn("per command", out.stdout)
        self.assertEqual(len([c for c in calls.splitlines() if "--fail-under-line" in c]), len(QT_TREES))

    def test_a_tree_with_no_data_fails_the_run(self):
        out, _ = self.run_report(list(QT_TREES[:2]), "-q")
        self.assertEqual(out.returncode, 1)
        self.assertIn(f"no data for {QT_TREES[2]}", out.stdout + out.stderr)

    def test_the_plain_run_does_not_gate_the_qt_trees(self):
        paths = [
            line.split()[0]
            for line in re.search(r'(?s)\ncomponents="\n(.*?)"\n', text("tools/checks/coverage_report.sh")).group(1).splitlines()
            if line.strip()
        ]
        self.assertTrue(paths)
        for tree in QT_TREES:
            with self.subTest(tree=tree):
                self.assertNotIn(tree, paths)


class TheJob(unittest.TestCase):
    def test_the_job_runs_the_qt_presets_and_the_qt_gate(self):
        core = text(".github/workflows/_ci-core.yml")
        job = re.search(r"(?ms)^  coverage-qt:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", core).group(1)
        for needle in (
            "cmake --preset config-linux-gcc-coverage-qt",
            "cmake --build --preset build-linux-gcc-coverage-qt",
            "ctest --preset test-linux-gcc-coverage-qt",
            "coverage_report.sh -q",
            "qtshadertools",
            "qtquick3d",
            "inputs.qt_version",
            "inputs.aqt_source",
            "tools/dev/qt-sandbox-setup.sh",
        ):
            with self.subTest(needle=needle):
                self.assertIn(needle, job)

    def test_the_result_reaches_ci_status_and_the_pins_reach_the_job(self):
        ci = text(".github/workflows/ci.yml")
        self.assertIn("R_COVERAGE_QT: ${{ needs.core.outputs.coverage_qt_result }}", ci)
        self.assertIn('"coverage-qt:$R_COVERAGE_QT"', ci)
        self.assertIn("qt_version: ${{ needs.toolchain-versions.outputs.qt_version }}", ci)
        self.assertIn("aqt_source: ${{ needs.toolchain-versions.outputs.aqt_source }}", ci)
        self.assertIn("coverage_qt_result:", text(".github/workflows/_ci-core.yml"))


if __name__ == "__main__":
    unittest.main()
