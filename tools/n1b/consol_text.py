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
import json
import re
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from n1b_lib import CPP_EXT, DEFAULT_ROOT, Repo

KEEP = (
    "CHANGELOG.md",
    "planning/consolidation.md",
    "planning/layout.md",
    "planning/layout-inventory.md",
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
    # whether the plans under planning/ are read too (not this consolidation's own page nor the
    # layout study's two, which name the paths before and after on purpose): a path or a header a
    # plan names follows the tree, as N1B's path pass had it do
    plans: bool = False
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
    # A build file that linked two or three of them links the one library once: the repeats, on
    # one line or on the lines that follow, go.
    Rule(
        "dedupe",
        r"(\biclforge::ac4(?:_static|_shared)?)(?:[ \t]*\n[ \t]*\1(?![\w:])|[ \t]+\1(?![\w:]))+",
        r"\1",
        ("cmake",),
    ),
]

# The directories of the three libraries that went, named bare in a comment or a page, where the
# path pass (consol_paths.py) reads whole paths of files a line at a time. A list of them is the one
# library's directory; a path a comment broke at a slash follows its file; each directory alone is
# where its files went (the core's to src/ac4/src/core, the decoder's and the encoder's to their
# areas), and its build file is the library's.
# The header the cut removed: a page that names it is pointed at the table of contents' header, the
# one that keeps its file comment; what it says of the declarations is a person's.
CUT_SPELLINGS = {"c1": {"iclforge/ac4/ac4.hpp": "iclforge/ac4/core/toc.hpp"}}

C1_PROSE = [
    # `src/ac4, src/ac4core, src/ac4dec and src/ac4enc`, written with code spans or not
    Rule(
        "dir-list",
        r"(`?)src/ac4(?:core|dec|enc)?\1(?:(?:,? and |, )\1src/ac4(?:core|dec|enc)?\b\1)+",
        r"\1src/ac4\1",
        _TEXT,
        plans=True,
    ),
    Rule(
        "errata-broken",
        r"src/ac4(?:dec|enc)/(\n[ \t]*(?://|#)[ \t]*)ERRATA\.md",
        r"src/ac4/\1ERRATA.md",
        _TEXT,
        plans=True,
    ),
    Rule(
        "build-file",
        r"\bsrc/ac4(?:core|dec|enc)/CMakeLists\.txt\b",
        "src/ac4/CMakeLists.txt",
        _TEXT,
        plans=True,
    ),
    Rule(
        "dir-core",
        r"\bsrc/ac4core(?:/include/iclforge/ac4core)?\b(?![\w-])",
        "src/ac4/src/core",
        _TEXT,
        plans=True,
    ),
    Rule(
        "dir-dec-src",
        r"\bsrc/ac4dec/src\b",
        "src/ac4/src/decoder",
        _TEXT,
        plans=True,
    ),
    Rule(
        "dir-enc-src",
        r"\bsrc/ac4enc/src\b",
        "src/ac4/src/encoder",
        _TEXT,
        plans=True,
    ),
    Rule(
        "dir-dec",
        r"\bsrc/ac4dec\b(?![\w-])(?!/)",
        "src/ac4/src/decoder",
        _TEXT,
        plans=True,
    ),
    Rule(
        "dir-enc",
        r"\bsrc/ac4enc\b(?![\w-])(?!/)",
        "src/ac4/src/encoder",
        _TEXT,
        plans=True,
    ),
]
STAGES: dict[str, list[Rule]] = {"c0": C0, "c1": C1 + C1_PROSE}


def kind_of(path: str) -> set[str]:
    kinds = {"text"}
    if Path(path).suffix in CPP_EXT or path.endswith((".hpp.in", ".h.in")):
        kinds.add("cpp")
    if path.endswith(CMAKE):
        kinds.add("cmake")
    return kinds


HELD_PLANS = KEEP[1:4]


def rewrite(
    text: str, rules: list[Rule], kinds: set[str], counts: Counter, plan_page: bool = False
) -> str:
    for rule in rules:
        if not kinds.intersection(rule.kinds) or (plan_page and not rule.plans):
            continue
        text, n = rule.compiled.subn(rule.replacement, text)
        counts[rule.name] += n
    return text


def spelling_rules(plan: Path) -> list[Rule]:
    """The include spellings a stage changed (its plan's map, and the cut's), wherever a page or a
    comment names one: n1b_docs.py's `header` rule for the consolidation."""
    spellings = dict(json.loads(plan.read_text(encoding="utf-8")).get("spellings", {}))
    spellings.update(CUT_SPELLINGS.get(json.loads(plan.read_text(encoding="utf-8"))["stage"], {}))
    rules = []
    for old in sorted(spellings, key=len, reverse=True):
        rules.append(
            Rule(
                "header",
                r"(?<![\w/])" + re.escape(old) + r"(?![\w/])",
                spellings[old].replace("\\", "\\\\"),
                _TEXT,
                plans=True,
            )
        )
    return rules


def run(
    root: Path, stage: str, dry_run: bool, report: Path | None, plan: Path | None = None
) -> Counter:
    repo = Repo(str(root))
    rules = STAGES[stage] + (spelling_rules(plan) if plan else [])
    counts: Counter = Counter()
    changed: list[str] = []
    for f in repo.files:
        plan_page = f.startswith("planning/")
        if f.startswith(KEEP) and not (plan_page and not f.startswith(HELD_PLANS)):
            continue
        path = root / f
        try:
            text = path.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        out = rewrite(text, rules, kind_of(f), counts, plan_page)
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
    ap.add_argument(
        "--plan", type=Path, default=None, help="the stage's plan: its include spellings follow too"
    )
    a = ap.parse_args()
    run(Path(a.root), a.stage, a.dry_run, a.report, a.plan)


if __name__ == "__main__":
    main()
