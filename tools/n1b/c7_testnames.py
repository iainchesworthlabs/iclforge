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
        binary = next((b for p, b in BINARIES if f.startswith(p)), None)
        if binary is None:
            continue
        path = root / f
        try:
            text = path.read_bytes().decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        new, n = NAME.subn(binary, text)
        if n:
            changed += 1
            mentions += n
            if not a.dry_run:
                path.write_bytes(new.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {changed} files, {mentions} mentions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
