#!/usr/bin/env python3
"""The test binary of the monolith, named in the comments of the products' files.

    c7_testnames.py [--root <worktree>] [--dry-run]

`iclforge-tests` was the one Catch2 binary of every test (planning/monorepo.md, (e)); since C7-1 it is
the umbrella target that builds the per-project binaries, and since C7-2 the products' tests are
beside the programs. A comment under apps/ that says a thing "is held by iclforge-tests", "rides
iclforge-tests" or "compiles into iclforge-tests" means the binary of the program it is in:

    apps/forge/cli                            iclforge-forge-cli-tests
    apps/forge/gui                            iclforge-forge-gui-tests
    apps/shared/media, apps/shared/theme      iclforge-app-media-tests
    apps/shared/preferences                   iclforge-settings-tests
    apps/hearth                               iclforge-hearth-tests
    apps/crucible                             iclforge-crucible-tests

A quoted `"iclforge-tests"` is a string the program or its test reads (the Qt Quick harness sets it as
the organisation name) and is left, and so is a file outside apps/.

A bare `tests/CMakeLists.txt` in a comment meant the one build file of every test; the file that
holds the thing the comment is about is the tests/ CMakeLists.txt beside the project (a library's, a
program's, tests/support's). Forge GUI's Qt Quick suites are `apps/forge/gui/tests/qml.cmake` since
C7-2 (a Catch2 binary has the CMakeLists.txt), so every mention of the old name of that file is
respelled; in Hearth's and Crucible's windows a bare mention is their Qt Quick suites' file, and
three test files that sit in a window's tests/ name the engine's.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

BINARIES = (
    ("apps/forge/cli/", "iclforge-forge-cli-tests"),
    ("apps/forge/gui/", "iclforge-forge-gui-tests"),
    ("apps/shared/media/", "iclforge-app-media-tests"),
    ("apps/shared/theme/", "iclforge-app-media-tests"),
    ("apps/shared/preferences/", "iclforge-settings-tests"),
    ("apps/hearth/", "iclforge-hearth-tests"),
    ("apps/crucible/", "iclforge-crucible-tests"),
)
NAME = re.compile(r"(?<![\"'A-Za-z0-9_-])iclforge-tests(?![\"'A-Za-z0-9_-])")
BARE = re.compile(r"(?<![A-Za-z0-9_./-])tests/CMakeLists\.txt")
GUI_QML = re.compile(r"(?<![A-Za-z0-9_-])apps/forge/gui/tests/CMakeLists\.txt")
# The tests/CMakeLists.txt a bare mention means, by the file that makes it (first prefix that fits).
TESTS_FILE = (
    ("apps/hearth/ui/tests/test_hearth_controller.cpp", "apps/hearth/engine/tests/CMakeLists.txt"),
    ("apps/crucible/ui/tests/test_desktop_entries.cpp", "apps/crucible/engine/tests/CMakeLists.txt"),
    ("apps/crucible/ui/tests/test_translations.cpp", "apps/crucible/engine/tests/CMakeLists.txt"),
    ("apps/crucible/CMakeLists.txt", "apps/crucible/engine/tests/CMakeLists.txt"),
    ("apps/crucible/engine/", "apps/crucible/engine/tests/CMakeLists.txt"),
    ("apps/crucible/ui/", "apps/crucible/ui/tests/CMakeLists.txt"),
    ("apps/forge/cli/", "apps/forge/cli/tests/CMakeLists.txt"),
    ("apps/forge/gui/", "apps/forge/gui/tests/qml.cmake"),
    ("apps/shared/media/", "apps/shared/media/tests/CMakeLists.txt"),
    ("apps/hearth/ui/", "apps/hearth/ui/tests/CMakeLists.txt"),
    ("apps/hearth/", "apps/hearth/engine/tests/CMakeLists.txt"),
    ("tests/support/", "tests/support/CMakeLists.txt"),
)
LIBRARY = re.compile(r"^libs/([^/]+)/")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=".")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    root = Path(a.root)
    files = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True
    ).stdout.decode("utf-8", "surrogateescape").split("\0")
    changed = mentions = 0
    for f in files:
        if f.startswith(("tools/n1b/", "planning/consolidation.md", "planning/monorepo.md")):
            continue
        binary = next((b for p, b in BINARIES if f.startswith(p)), None)
        own_tests = next((t for p, t in TESTS_FILE if f.startswith(p)), None)
        if (m := LIBRARY.match(f)) is not None:
            own_tests = f"libs/{m.group(1)}/tests/CMakeLists.txt"
        if binary is None and own_tests is None and not f.endswith((".md", ".yml", ".sh", ".ps1")):
            continue
        path = root / f
        try:
            text = path.read_bytes().decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        new, n = (NAME.subn(binary, text) if binary else (text, 0))
        new, k = GUI_QML.subn("apps/forge/gui/tests/qml.cmake", new)
        n += k
        if own_tests:
            new, k = BARE.subn(own_tests, new)
            n += k
        if n:
            changed += 1
            mentions += n
            if not a.dry_run:
                path.write_bytes(new.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {changed} files, {mentions} mentions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
