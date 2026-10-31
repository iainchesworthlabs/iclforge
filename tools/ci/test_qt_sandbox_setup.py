"""Unit tests for tools/dev/qt-sandbox-setup.sh, the script that builds the Qt applications on a
Linux machine with no Qt.

What runs here is what needs no network and no compiler: the script reads its Qt version and
aqtinstall commit from .github/toolchain-versions.json (so a bump there is the only edit), refuses
a prefix inside the repository, prints a configure line that names what the three windows need, and
writes the package files Hearth's configure reads. The last of those downloads three files unless
ICLFORGE_QT_SANDBOX_MIRROR names a directory that holds them (the script's --mirror), so it
runs only then; the coverage-qt job runs the whole script, downloads and mbedTLS build included.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "dev" / "qt-sandbox-setup.sh"
MIRROR = os.environ.get("ICLFORGE_QT_SANDBOX_MIRROR")


def bash() -> str | None:
    """A bash that runs scripts; on Windows the first one on PATH may be the WSL launcher."""
    for candidate in (shutil.which("bash"), r"C:\Program Files\Git\bin\bash.exe"):
        if not candidate:
            continue
        probe = subprocess.run(
            [candidate, "-c", "echo ok"], capture_output=True, text=True, check=False
        )
        if probe.returncode == 0 and probe.stdout.strip() == "ok":
            return candidate
    return None


BASH = bash()


def python3_works() -> bool:
    """Whether `python3` runs inside that bash, which is where the script calls it (on Windows
    the one on PATH can be the Store's stub)."""
    if not BASH:
        return False
    return (
        subprocess.run([BASH, "-c", "python3 -c pass"], capture_output=True, check=False).returncode
        == 0
    )


@unittest.skipUnless(BASH and python3_works(), "needs bash and a python3 that runs")
class QtSandboxSetup(unittest.TestCase):
    def run_script(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [BASH, str(SCRIPT), *args], capture_output=True, text=True, cwd=ROOT, check=False
        )

    def test_the_script_parses(self):
        self.assertEqual(subprocess.run([BASH, "-n", str(SCRIPT)], check=False).returncode, 0)

    def test_the_qt_version_comes_from_the_pin_file(self):
        pins = json.loads((ROOT / ".github" / "toolchain-versions.json").read_text("utf-8"))
        with tempfile.TemporaryDirectory() as prefix:
            out = self.run_script("--print-only", prefix)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn(f"/kit/{pins['qt']}/gcc_64", out.stdout.replace("\\", "/"))

    def test_the_configure_line_builds_the_three_windows_with_pipewire(self):
        with tempfile.TemporaryDirectory() as prefix:
            line = self.run_script("--print-only", prefix).stdout
        for flag in (
            "-DICLFORGE_BUILD_GUI=ON",
            "-DICLFORGE_BUILD_CRUCIBLE=ON",
            "-DICLFORGE_BUILD_HEARTH=ON",
            "-DICLFORGE_WITH_ALSA=OFF",
            "-DICLFORGE_WITH_PIPEWIRE=ON",
            "-DICLFORGE_MDNS_INCLUDE_DIR=",
            "-DVCPKG_INSTALLED_DIR=",
            "-Wno-error=old-style-cast",
        ):
            with self.subTest(flag=flag):
                self.assertIn(flag, line)
        self.assertIn("PIPEWIRE_DEBUG=0", line)
        self.assertIn("QT_QPA_PLATFORM=offscreen", line)

    def test_without_hearth_the_line_turns_it_off_and_names_no_prefix(self):
        with tempfile.TemporaryDirectory() as prefix:
            line = self.run_script("--print-only", "--no-hearth", prefix).stdout
        self.assertIn("-DICLFORGE_BUILD_HEARTH=OFF", line)
        self.assertNotIn("CMAKE_PREFIX_PATH", line)

    def test_a_prefix_inside_the_repository_is_refused(self):
        out = self.run_script(
            "--print-only", str(ROOT / "build" / "qt-prefix-that-should-not-exist")
        )
        self.assertEqual(out.returncode, 2)
        self.assertIn("outside the repository", out.stderr)
        self.assertFalse((ROOT / "build" / "qt-prefix-that-should-not-exist").exists())

    def test_the_repository_patch_the_script_applies_is_there(self):
        self.assertTrue(
            (
                ROOT / "cmake" / "vcpkg" / "ports" / "cpp-httplib" / "wait-inside-a-frame.patch"
            ).is_file()
        )

    @unittest.skipUnless(MIRROR, "set ICLFORGE_QT_SANDBOX_MIRROR to a directory with the downloads")
    def test_the_hearth_prefix_holds_what_the_configure_reads(self):
        with tempfile.TemporaryDirectory() as prefix:
            out = self.run_script("--no-apt", "--no-qt", "--no-mbedtls", "--mirror", MIRROR, prefix)
            self.assertEqual(out.returncode, 0, out.stderr)
            hearth = Path(prefix) / "hearth"
            for relative in (
                "include/httplib.h",
                "include/mdns.h",
                "lib/cmake/httplib/httplibConfig.cmake",
                "lib/cmake/FLAC/FLACConfig.cmake",
                "lib/cmake/Opus/OpusConfig.cmake",
                "no-vcpkg-toolchain.cmake",
            ):
                with self.subTest(file=relative):
                    self.assertTrue((hearth / relative).is_file())
            share = hearth / "vcpkg_installed" / "x64-linux-gcc" / "share"
            for port in ("cpp-httplib", "mdns", "libflac", "opus"):
                with self.subTest(port=port):
                    self.assertTrue((share / port / "copyright").is_file())
                    spdx = json.loads((share / port / "vcpkg.spdx.json").read_text("utf-8"))
                    self.assertIn("versionInfo", spdx["packages"][0])
            # The repository's patch was applied to the header (it adds `in_message`).
            self.assertIn("in_message", (hearth / "include" / "httplib.h").read_text("utf-8"))

    @unittest.skipUnless(MIRROR, "set ICLFORGE_QT_SANDBOX_MIRROR to a directory with the downloads")
    def test_a_download_that_does_not_match_its_checksum_stops_the_script(self):
        with tempfile.TemporaryDirectory() as mirror, tempfile.TemporaryDirectory() as prefix:
            for item in Path(MIRROR).iterdir():
                shutil.copy(item, mirror)
            Path(mirror, "mdns.h").write_text("not the pinned header\n", encoding="utf-8")
            out = self.run_script("--no-apt", "--no-qt", "--no-mbedtls", "--mirror", mirror, prefix)
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("pinned SHA-256", out.stderr)


if __name__ == "__main__":
    unittest.main()
