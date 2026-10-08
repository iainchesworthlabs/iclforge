#!/usr/bin/env python3
"""Is every change to C and C++ since BASE a change of paths only?

For each file git reports as modified or renamed (BASE against the working tree), the old and the
new text are compared with comments and #include lines removed, white space between tokens dropped
(a reflow is layout) and every path the move plan changed spelled the new way in the old text. Two
texts that are then equal differ only in comments, in includes, in layout and in paths. What is not
equal is printed, cut short, for a person to read.

    c7_pathonly.py --base <rev> [--root <worktree>]
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

LIBS = "ac3|ac4|adm|audio|base|capi|containers|dsp|iab|objects|render|sendspin"
EXT = (".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".hh", ".hxx", ".inl", ".mm", ".hpp.in", ".h.in")

# old spelling -> new spelling, applied to the OLD text (longest and most specific first)
RULES = [
    (re.compile(r"\bsrc/sendspin/third_party/time-filter\b"), "external/time-filter"),
    (re.compile(rf"\bsrc/({LIBS})/"), r"libs/\1/"),
    (re.compile(rf"\btests/({LIBS})/"), r"libs/\1/tests/"),
    (re.compile(r"\btests/(platform|crt)/"), r"tests/support/\1/"),
    (re.compile(r"\btests/(sanitized\.hpp|ac4_stream_kinds\.hpp)"), r"tests/support/\1"),
    (re.compile(r"\btests/audio/alsa_null_device\.hpp"), "tests/support/alsa_null_device.hpp"),
    (re.compile(r"\bfuzz/fuzz_([a-z0-9_]+)"), r"libs/@FUZZ@/fuzz/fuzz_\1"),
]

TOKEN = re.compile(r'(//[^\n]*|/\*.*?\*/)|("(?:\\.|[^"\\\n])*")', re.S)
INCLUDE = re.compile(r"^[ \t]*#[ \t]*include[^\n]*$", re.M)


def strip(text: str) -> list[str]:
    """The code of a C++ text with comments and #include lines removed, as one line.

    Layout is not meaning: the lines are joined and the white space between tokens dropped, and two
    string literals side by side (a clang-format wrap of a long one) are one. What a person is shown
    when two texts differ is that one line, cut short.
    """

    def keep_strings(m: re.Match) -> str:
        return m.group(2) or ""

    # one left-to-right pass, so that a comment that holds a quote is a comment, and a string that
    # holds // is a string
    text = TOKEN.sub(keep_strings, text)
    text = INCLUDE.sub("", text)
    text = re.sub(r'"\s+"', "", text)
    return [re.sub(r"\s+", "", text)]


def spell(text: str) -> str:
    for rx, rep in RULES:
        text = rx.sub(rep, text)
    return text


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--root", default=".")
    ap.add_argument("--show", type=int, default=40)
    a = ap.parse_args()
    root = Path(a.root)

    def git(*args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(root), *args], capture_output=True, check=True, text=True
        ).stdout

    status = git("diff", "-M", "--name-status", a.base).splitlines()
    same = 0
    flagged = []
    compared = 0
    for line in status:
        parts = line.split("\t")
        code = parts[0]
        if code[0] not in "MR":
            continue
        old, new = (parts[1], parts[-1])
        if not new.endswith(EXT):
            continue
        new_path = root / new
        if not new_path.is_file():
            continue
        compared += 1
        old_text = git("show", f"{a.base}:{old}")
        new_text = new_path.read_text(encoding="utf-8", errors="replace")
        o, n = strip(spell(old_text)), strip(new_text)
        # the fuzz placeholder: any library
        o = [re.sub(r"libs/@FUZZ@/", "libs/<x>/", x) for x in o]
        n = [re.sub(rf"libs/(?:{LIBS})/fuzz/", "libs/<x>/fuzz/", x) for x in n]
        if o == n:
            same += 1
        else:
            flagged.append((old, new, o, n))
    print(
        f"{compared} C/C++ files changed or renamed with edits; {same} differ in comments, "
        f"includes, layout and paths only; {len(flagged)} to read"
    )
    for old, new, o, n in flagged[: a.show]:
        print(f"\n--- {old} -> {new}")
        so, sn = set(o), set(n)
        for x in [x for x in o if x not in sn][:6]:
            print("  -", x[:170])
        for x in [x for x in n if x not in so][:6]:
            print("  +", x[:170])
    return 0


if __name__ == "__main__":
    sys.exit(main())
