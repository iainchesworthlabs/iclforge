#!/usr/bin/env python3
"""Fail when first-party C++ uses the standard library's <format> or <print>.

The tree formats with {fmt}: `fmt::format`, `fmt::print`, `fmt::println` and the cmake target
`iclforge::fmt` (CONTRIBUTING.md, "Code conventions"; cmake/Fmt.cmake). NDK r26's bundled libc++
has no usable <format>, and nothing in the Android build passes -fexperimental-library. A desktop
standard library ships <format>, so a `std::format` compiles on every desktop leg and breaks only
where an NDK build reaches the file, which for the Hearth take recorder and the forge CLI, the last
two files that had one, is nowhere: neither is in an Android build. The conventions paragraph alone
did not stop it coming back. This check is what does, in the static job, in seconds, on every pull
request.

What it reads: every tracked C++ file (*.cpp *.hpp *.h *.inl *.mm and the other usual suffixes)
under libs/ apps/ tests/ examples/ tools/ bindings/, with the inside of every comment and string
literal blanked, so that a comment explaining the rule, or a diagnostic that names std::format,
does not count. What it fails on:

  - `#include <format>` and `#include <print>`;
  - `std::format`, `std::format_to`, `std::format_to_n`, `std::formatted_size`, `std::vformat`,
    `std::formatter`, `std::format_error`, `std::make_format_args` and the rest of the names
    <format> declares (`std::format*`, `std::vformat*`, `std::wformat*`, `std::basic_format_*`,
    `std::make_format_args`, `std::make_wformat_args`, `std::range_format*`, `std::runtime_format`),
    and `std::print`, `std::println`, `std::vprint_unicode`, `std::vprint_nonunicode`.

`std::printf` and the rest of <cstdio> are not <print> and are not flagged.

NOT read: firmware/ (the ESP-IDF components and boards are built by idf.py with that toolchain's
standard library and use neither {fmt} nor <format>, so the NDK gap the rule answers to is not
theirs) and external/ (vendored code, not ours to edit). tests/ and tools/ are read: a test that
compiles on a desktop and not on the NDK is the same failure.

There is no waiver list. A file that has to name the standard facility in code, a probe that checks
the NDK still lacks it for instance, is an edit to this script in review, where it is seen.

Stdlib-only, run from _static.yml's static job and runnable the same way locally:

    python3 tools/checks/check_no_std_format.py [--root <repo>]

Its own suite is test_check_no_std_format.py, which runs in the Oracle unit tests step.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# The comment and literal blanker the other tree checks use (offsets and newlines are kept).
from check_namespaces import mask  # noqa: E402

TREES = ("libs", "apps", "tests", "examples", "tools", "bindings")
SUFFIXES = (
    ".cpp",
    ".cc",
    ".cxx",
    ".c++",
    ".cppm",
    ".ixx",
    ".hpp",
    ".hh",
    ".hxx",
    ".h",
    ".ipp",
    ".inl",
    ".mm",
    ".hpp.in",
    ".h.in",
)
# Only matters when the tree is no git repository (a tracked file is never one of these).
SKIPPED_DIRECTORIES = frozenset(
    {".git", "external", "node_modules", "vcpkg_installed", "_deps", "build", "CMakeFiles"}
)

# <format> and <print>, however the directive is spaced.
_INCLUDE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*<[ \t]*(format|print)[ \t]*>", re.M)
# The names of std that come from <format> or <print>. `std::format\w*` takes in format_to,
# format_to_n, formatted_size, formatter, format_error, format_args, format_string, format_context
# and format_parse_context; `printf` is not `print`, which the trailing \b keeps out.
_NAME = re.compile(
    r"(?<![\w])std[ \t]*::[ \t]*"
    r"(?P<name>format\w*|vformat\w*|wformat\w*|basic_format\w*|make_format_args|make_wformat_args"
    r"|range_format\w*|runtime_format|vprint_unicode|vprint_nonunicode|println|print)\b"
)

GUIDANCE = (
    "This project formats with {fmt}, never the standard library's <format>/<print>:\n"
    "  - include <fmt/format.h> for fmt::format and fmt::format_to, <fmt/base.h> for fmt::print and\n"
    "    fmt::println, <fmt/chrono.h> to format a std::chrono time_point or duration, <fmt/ranges.h>\n"
    "    for a range, and link the target iclforge::fmt;\n"
    "  - the format-spec syntax is the same, so the call usually changes in its prefix alone, but\n"
    "    compare the output where a floating-point value is written with a bare {}: 100000.0 is\n"
    "    \"1e+05\" under std::format and \"100000\" under fmt::format.\n"
    "Why: NDK r26's libc++ has no usable <format>, and a desktop build compiles std::format without\n"
    "a word, so it breaks only where an NDK build reaches the file. See CONTRIBUTING.md (Code\n"
    "conventions) and cmake/Fmt.cmake."
)


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    what: str
    text: str


def scan(text: str) -> list[tuple[int, str]]:
    """The (1-based line, what) of each use in `text`, comments and literals not counted."""
    masked = mask(text)
    found: list[tuple[int, str]] = []
    for m in _INCLUDE.finditer(masked):
        found.append((masked.count("\n", 0, m.start()) + 1, f"#include <{m.group(1)}>"))
    for m in _NAME.finditer(masked):
        found.append((masked.count("\n", 0, m.start()) + 1, f"std::{m.group('name')}"))
    return sorted(found)


def is_cpp(path: str) -> bool:
    return path.endswith(SUFFIXES)


def tracked(root: Path) -> list[str]:
    """The C++ files under the trees: what git tracks, or what is there when it is no repository."""
    try:
        # A root that is a directory inside some other repository is no repository of its own: a
        # test's temporary tree under a checkout would otherwise be answered with that checkout's
        # (empty) listing for these paths.
        prefix = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--show-prefix"],
            capture_output=True,
            check=True,
        ).stdout.decode("utf-8").strip()
        if prefix:
            raise FileNotFoundError(prefix)
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--", *TREES],
            capture_output=True,
            check=True,
        ).stdout.decode("utf-8", "surrogateescape")
        files = [f for f in out.split("\0") if f and (root / f).is_file()]
    except (subprocess.CalledProcessError, FileNotFoundError):
        files = []
        for tree in TREES:
            for p in (root / tree).rglob("*"):
                rel = p.relative_to(root)
                if p.is_file() and not SKIPPED_DIRECTORIES.intersection(rel.parts[:-1]):
                    files.append(rel.as_posix())
    return sorted(f for f in files if is_cpp(f))


def check(root: Path, files: list[str]) -> list[Finding]:
    findings: list[Finding] = []
    for f in files:
        text = (root / f).read_bytes().decode("utf-8", "replace")
        lines = text.splitlines()
        for number, what in scan(text):
            # An include is its own line; for a name, the line says what it was used for.
            shown = ""
            if not what.startswith("#include") and number <= len(lines):
                shown = lines[number - 1].strip()
            findings.append(Finding(f, number, what, shown))
    return findings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", default=str(HERE.parents[1]), help="the repository")
    a = ap.parse_args(argv)
    root = Path(a.root)
    files = tracked(root)
    findings = check(root, files)
    for x in findings:
        print(f"FAIL: {x.path}:{x.line}: {x.what}" + (f": {x.text}" if x.text else ""))
    print(f"{len(files)} C++ files under {', '.join(TREES)}: {len(findings)} uses of <format>/<print>")
    if findings:
        print()
        print(GUIDANCE)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
