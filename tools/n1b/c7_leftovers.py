#!/usr/bin/env python3
"""What still names a root that C7 moved?

The path pass rewrites a path it can find in a move plan. It cannot rewrite a root built from parts
(`${PROJECT_SOURCE_DIR}/src/dsp/variants/decode-scalar-${TIER}`, `REPO.join("src")`), a regular
expression over the old root, a glob, or a directory a build writes into. This lists them, per
file, for a person to read:

  lib-roots    `src/<library>` where <library> is one of the twelve (not `libs/<x>/src/<library>`,
               which is a directory of that name inside a library)
  bare-src     a bare `src/` or `./src`: a library's own sources, a binding's, a vendored project's,
               and the old root in prose
  var-src      `${...}/src/`: a root taken from a variable
  fuzz         the central `fuzz/` (a harness's own is libs/<lib>/fuzz)
  tests-lib    `tests/<library>/` (a library's tests are libs/<lib>/tests)
  third-party  `third_party/time-filter` (it is external/time-filter)

C7-2 (the products) adds:

  apps-roots   `apps/cli`, `apps/gui`, `apps/common`, `apps/notices`, `apps/windows`, `apps/linux`,
               `apps/android`, `apps/wasm` (they are apps/forge/{cli,gui}, apps/shared/media,
               notices/, apps/crucible/{windows,linux}, apps/demos/{android,wasm})
  tests-prod   `tests/cli`, `tests/gui`, `tests/hearth`, `tests/crucible` (each program's tests
               are beside it: apps/<product>/<program>/tests)
  prog-files   `apps/{hearth,crucible}/<program>/<file>` for a source, a header or a resource
               that now sits in the program's src/ or assets/, and `apps/crucible/translations`
  apps-parts   a product root built from parts: `"apps", "gui"`, `Path("apps") / "cli"`,
               `apps/${name}`

    c7_leftovers.py [--root <worktree>] [--kind K] [--exclude PREFIX ...] [--show N]

Nothing here fails a build: a hit is a line to read. The first run of C7-1 found, this way, the
`src/dsp/variants` include path that stopped AC-4 compiling, the build tree's `src/<lib>/generated`
directory the installed export header is copied from, the ESP-IDF component's `lib/src/ac3` probe
and the ABI step's `find .../src -name '*.so'`.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from collections import Counter, defaultdict
from pathlib import Path

LIBS = "ac3|ac4|adm|audio|base|capi|containers|dsp|iab|objects|render|sendspin"
START = r"(?:^|[\s\"'(=:,\[{}\|!])"
KINDS: dict[str, re.Pattern[str]] = {
    # not after a path component (`js/src/ac4.ts`, `libs/ac3/src/base`), not a file (`src/adm.cpp`)
    "lib-roots": re.compile(
        rf"(?<![A-Za-z0-9_.-])(?<![A-Za-z0-9_.-]/)src/(?:{LIBS})(?![A-Za-z0-9_-])(?!\.[a-z])"
    ),
    "bare-src": re.compile(START + r"\.?/?src(?:/|\b(?=[\"'*,\s)]|$))"),
    "var-src": re.compile(r"\}/src(?:/|\b)"),
    "fuzz": re.compile(START + r"\.?/?fuzz(?:/|\b(?=[\"'*,\s)]|$))|\}/fuzz(?:/|\b)"),
    "tests-lib": re.compile(START + rf"tests/(?:{LIBS})(?:/|\b)|\}}/tests/(?:{LIBS})(?:/|\b)"),
    "third-party": re.compile(r"third_party/time-filter"),
    "apps-roots": re.compile(
        r"(?<![A-Za-z0-9_.-])apps/(?:cli|gui|common|notices|windows|linux|android|wasm)"
        r"(?![A-Za-z0-9_-])"
    ),
    "tests-prod": re.compile(
        START + r"tests/(?:cli|gui|hearth|crucible)(?:/|\b)|\}/tests/(?:cli|gui|hearth|crucible)(?:/|\b)"
    ),
    "prog-files": re.compile(
        r"apps/(?:hearth|crucible)/(?:engine|ui|render|testsink|testserver|runner)/"
        r"(?!src/|tests/|assets/|platform/|packaging/)[A-Za-z0-9_.-]+\.(?:cpp|hpp|h|mm|qml|ts|svg|png)"
        r"|apps/crucible/translations"
    ),
    "apps-parts": re.compile(
        r"""["']apps["']\s*[,/]\s*["'](?:cli|gui|common|notices|windows|linux|android|wasm)["']"""
        r"|apps/\$\{|apps/\{"
    ),
}
BINARY = (
    ".ts",
    ".png",
    ".wav",
    ".ac3",
    ".ec3",
    ".ac4",
    ".bin",
    ".mp4",
    ".jpg",
    ".ico",
    ".svg",
    ".lock",
    ".json.gz",
)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", default=".")
    ap.add_argument("--kind", default="all", choices=["all", *KINDS])
    ap.add_argument(
        "--exclude", nargs="*", default=["planning/", "tools/n1b/", "docs/", "CHANGELOG.md"]
    )
    ap.add_argument("--show", type=int, default=80)
    a = ap.parse_args()
    root = Path(a.root)
    listed = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True
    ).stdout.decode("utf-8", "surrogateescape")
    hits: dict[str, list[tuple[str, int, str]]] = defaultdict(list)
    for f in listed.split("\0"):
        if not f or f.startswith(tuple(a.exclude)) or f.endswith(BINARY):
            continue
        p = root / f
        if not p.is_file():
            continue
        try:
            text = p.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for n, line in enumerate(text.splitlines(), 1):
            for kind, rx in KINDS.items():
                if a.kind not in ("all", kind):
                    continue
                m = rx.search(line)
                if m:
                    hits[kind].append((f, n, line.strip()[:160]))
    for kind, hs in hits.items():
        by = Counter(f for f, _, _ in hs)
        print(f"=== {kind}: {len(hs)} lines in {len(by)} files")
        for f, c in by.most_common(15):
            print(f"  {c:4d}  {f}")
    if a.kind != "all":
        for f, n, line in hits[a.kind][: a.show]:
            print(f"{f}:{n}: {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
