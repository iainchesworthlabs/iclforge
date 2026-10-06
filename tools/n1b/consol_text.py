"""The text rewrites of the consolidation (planning/consolidation.md, stages C0 to C3), as tables.

    consol_text.py --root <worktree> --stage c0|c1|c2|c3 [--dry-run] [--report <file>]

Each stage is a list of rules, a rule a regular expression, its replacement and the files it reads
(C and C++ sources by default; a rule can widen that to the build files, scripts or pages). The
moves and the include spellings of a stage are n1b_apply.py's (with the stage's plan from
consoldef.py); this pass is what is left that a table can say: a macro, a namespace, an export
header that is no longer generated. It reads the tracked files of the worktree, leaves the history
alone (KEEP: the changelog, the plans, the scripts of the migrations and the byte-exact trees), and
a second run changes nothing.
"""

from __future__ import annotations

import argparse
import re
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from n1b_lib import CPP_EXT, DEFAULT_ROOT, Repo

KEEP = (
    "CHANGELOG.md",
    "planning/",
    "tools/n1b/",
    "tools/checks/layering_debt/",
    "tests/golden/",
    "fuzz/seeds/",
    "fuzz/regressions/",
    "packaging/winget/manifests/",
)

CMAKE = ("CMakeLists.txt", ".cmake", ".cmake.in")


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: str
    replacement: str
    # which files: "cpp" (C and C++ sources), "cmake", or "text" (every tracked text file)
    kinds: tuple[str, ...] = ("cpp",)
    flags: int = 0
    compiled: re.Pattern = field(init=False, repr=False, compare=False)

    def __post_init__(self) -> None:
        object.__setattr__(self, "compiled", re.compile(self.pattern, self.flags))


# --- C0 ---------------------------------------------------------------------------------------
# The AC-4 core's profiling variants re-declared iclforge::base's zone_enter and zone_leave behind
# a marker of their own; the markers are base's now.
C0 = [
    Rule(
        "ac4-profiling-header",
        r'(#\s*include\s*[<"])iclforge/ac4core/detail/profiling\.hpp([>"])',
        r"\1iclforge/base/detail/profiling.hpp\2",
    ),
    Rule("ac4-zone-marker", r"\bAC4_ZONE_SCOPED_N\b", "ICLFORGE_ZONE_SCOPED_N"),
]

# --- C1 ---------------------------------------------------------------------------------------
# AC-4's four libraries are one. What names one of the three that go, in a build file, a script, a
# page or a comment: its CMake target and alias, its file, its pkg-config name, its export macros.
# The include spellings and the C++ export macros are consol_apply.py's; the AC-4 core, which had
# no ABI, no headers and no export of its own beyond its archive, is a person's where a sentence
# says what it was (its target in the tests' links becomes the merged library's).
_TEXT = ("text",)
C1 = [
    Rule(
        "alias-variant",
        r"\biclforge::ac4(?:dec|enc)_(static|shared|objects)\b",
        r"iclforge::ac4_\1",
        _TEXT,
    ),
    Rule("alias", r"\biclforge::ac4(?:dec|enc|core)\b(?![_:])", "iclforge::ac4", _TEXT),
    Rule(
        "raw-target",
        r"\biclforge_ac4(?:dec|enc)_(static|shared|objects)\b",
        r"iclforge_ac4_\1",
        _TEXT,
    ),
    Rule(
        "file",
        r"\blibiclforge_ac4(?:dec|enc)(_static\.a|\.so|\.a|\.dylib|\.dll|\.lib)",
        r"libiclforge_ac4\1",
        _TEXT,
    ),
    Rule("pkg-config", r"\biclforge-ac4(?:dec|enc)\b(?![-\w])", "iclforge-ac4", _TEXT),
    Rule(
        "macro",
        r"\bICLFORGE_AC4(?:DEC|ENC)_(EXPORT|STATIC_DEFINE|BUILDING_SHARED)\b",
        r"ICLFORGE_AC4_\1",
        _TEXT,
    ),
    # A list that named the libraries one by one names the one library once: `iclforge::ac4,
    # iclforge::ac4 and iclforge::ac4`, `iclforge::ac4/iclforge::ac4`.
    Rule(
        "collapse",
        r"(`?\biclforge::ac4(?:_static|_shared)?\b`?)(?:(?:,? and |, | or |/)\1(?![\w:]))+",
        r"\1",
        _TEXT,
    ),
]

STAGES: dict[str, list[Rule]] = {"c0": C0, "c1": C1}


def kind_of(path: str) -> set[str]:
    kinds = {"text"}
    if Path(path).suffix in CPP_EXT or path.endswith((".hpp.in", ".h.in")):
        kinds.add("cpp")
    if path.endswith(CMAKE):
        kinds.add("cmake")
    return kinds


def rewrite(text: str, rules: list[Rule], kinds: set[str], counts: Counter) -> str:
    for rule in rules:
        if not kinds.intersection(rule.kinds):
            continue
        text, n = rule.compiled.subn(rule.replacement, text)
        counts[rule.name] += n
    return text


def run(root: Path, stage: str, dry_run: bool, report: Path | None) -> Counter:
    repo = Repo(str(root))
    rules = STAGES[stage]
    counts: Counter = Counter()
    changed: list[str] = []
    for f in repo.files:
        if f.startswith(KEEP):
            continue
        path = root / f
        try:
            text = path.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        out = rewrite(text, rules, kind_of(f), counts)
        if out != text:
            changed.append(f)
            if not dry_run:
                path.write_bytes(out.encode("utf-8"))
    print(
        f"{stage}: {len(changed)} files, "
        + ", ".join(f"{k} {v}" for k, v in sorted(counts.items()) if v)
    )
    if report:
        report.write_text("\n".join(changed) + "\n", encoding="utf-8")
    return counts


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--stage", required=True, choices=sorted(STAGES))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", type=Path, default=None)
    a = ap.parse_args()
    run(Path(a.root), a.stage, a.dry_run, a.report)


if __name__ == "__main__":
    main()
