"""Stage C2 of planning/consolidation.md, first part: cut the signing tests by what each one tests.

    c2_cuts.py --root <worktree> [--dry-run]

`tests/signing/test_signing.cpp` tests two things that C2 puts in two libraries: the crypto
primitives and the operator's key (SHA-256, HMAC-SHA-256, `load_signing_key`, `decode_signing_key`),
which go to `iclforge::base`, and the EMDF Atmos signer, which goes to `iclforge::ac3`. The test
cases tagged `[sha256]`, `[hmac]` or `[key]` go to `tests/signing/test_crypto.cpp`, each other one
stays; a helper of the anonymous namespace goes with the cases that call it (to both files when both
do), and an include with the file that names what it declares. The cut is made in today's layout,
before the moves, so that the move commit stays pure; `consoldef.py` moves the new file to
`tests/base/`. `tests/CMakeLists.txt` lists the new file beside the old. Test names and tags are
kept, so ctest lists the same cases.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

from n1b_lib import DEFAULT_ROOT

SOURCE = "tests/signing/test_signing.cpp"
CRYPTO = "tests/signing/test_crypto.cpp"
CMAKE = "tests/CMakeLists.txt"
CRYPTO_TAGS = ("[sha256]", "[hmac]", "[key]")

# What a header declares, by the tokens that name it: an include goes to a file that uses one.
HEADER_TOKENS = {
    "<algorithm>": r"std::(?:ranges::)?(?:copy|fill|equal|find|count|all_of|any_of|none_of|min|max"
    r"|reverse|sort|transform|search|mismatch)\b",
    "<array>": r"std::array\b",
    "<cmath>": r"std::(?:sin|cos|abs|fabs|sqrt|pow|floor|ceil)\b",
    "<cstddef>": r"std::(?:byte|size_t|to_integer)\b",
    "<cstdint>": r"std::u?int\d+_t\b",
    "<filesystem>": r"std::filesystem\b|\bfs::",
    "<fstream>": r"std::[io]?fstream\b",
    "<numbers>": r"std::numbers\b",
    "<span>": r"std::span\b",
    "<string>": r"std::string\b|std::to_string\b",
    "<string_view>": r"std::string_view\b|\bstring_view\b",
    "<vector>": r"std::vector\b",
    '"platform/process.hpp"': r"\bprocess_id\b",
    '"iclforge/ac3/oba/atmos.hpp"': r"\b(?:AtmosEncoder|kSamplesPerFrame|ObjectPlacement)\b",
    '"iclforge/signing/emdf_atmos_signer.hpp"': r"\b(?:sign_atmos_\w+|verify_atmos_\w+"
    r"|has_authenticity_tag|VerifyResult|VerifySummary)\b",
    '"iclforge/signing/signing_key.hpp"': r"\b(?:SigningKey|load_signing_key|decode_signing_key"
    r"|KeyErrorKind|KeyLoadError)\b",
    '"hmac_sha256.hpp"': r"\bhmac_sha256\b",
    '"sha256.hpp"': r"\b(?:sha256|Sha256)\b",
}


@dataclass
class Block:
    """A piece of the file: a helper of the anonymous namespace, or a test case with the comment
    lines above it."""

    text: str
    name: str = ""


def balanced_blocks(body: str) -> list[Block]:
    """Top-level declarations of `body` (a definition ends where its braces balance), each with the
    comment lines directly above it."""
    out: list[Block] = []
    lines = body.splitlines()
    i = 0
    while i < len(lines):
        start = i
        while i < len(lines) and (not lines[i].strip() or lines[i].lstrip().startswith("//")):
            i += 1
        if i >= len(lines):
            break
        depth, seen = 0, False
        j = i
        while j < len(lines):
            code = re.sub(r'"(?:\\.|[^"\\])*"', '""', lines[j].split("//")[0])
            depth += code.count("{") - code.count("}")
            seen = seen or "{" in code
            if (seen and depth == 0) or (not seen and code.rstrip().endswith(";")):
                break
            j += 1
        text = "\n".join(lines[start : j + 1]).strip("\n")
        m = re.search(r"([A-Za-z_]\w*)\s*\(", lines[i])
        out.append(Block(text, m.group(1) if m else ""))
        i = j + 1
    return out


def case_head(text: str) -> str:
    """A test case's name and tags, which may wrap onto a second line."""
    m = re.search(r"TEST_CASE\((.*?)\)\s*\{", text, re.S)
    return m.group(1) if m else ""


def uses(text: str, name: str) -> bool:
    return re.search(rf"(?<![\w.]){re.escape(name)}\s*\(", text) is not None


def includes_for(text: str, includes: list[str]) -> list[str]:
    out = []
    for line in includes:
        m = re.match(r'#include\s+([<"][^>"]+[>"])', line)
        token = HEADER_TOKENS.get(m.group(1)) if m else None
        if token is None or re.search(token, text):
            out.append(line)
    return out


def cut(source: str) -> tuple[str, str]:
    """The signer's file and the primitives' file."""
    m = re.search(r"^namespace \{\n(.*?)^\}  // namespace\n", source, re.S | re.M)
    if not m:
        raise SystemExit("c2_cuts: the anonymous namespace of test_signing.cpp was not found")
    head, helpers_text, tail = source[: m.start()], m.group(1), source[m.end() :]
    helpers = balanced_blocks(helpers_text)
    cases = [b for b in balanced_blocks(tail) if "TEST_CASE(" in b.text]
    if not cases:
        raise SystemExit("c2_cuts: no TEST_CASE in test_signing.cpp")
    crypto_cases = [b for b in cases if any(t in case_head(b.text) for t in CRYPTO_TAGS)]
    signer_cases = [b for b in cases if b not in crypto_cases]

    def needed(case_blocks: list[Block]) -> list[Block]:
        text = "\n".join(b.text for b in case_blocks)
        keep: list[Block] = []
        changed = True
        while changed:
            changed = False
            for h in helpers:
                if h not in keep and h.name and uses(text, h.name):
                    keep.append(h)
                    text += "\n" + h.text
                    changed = True
        return [h for h in helpers if h in keep]

    include_lines = [line for line in head.splitlines() if line.startswith("#include")]

    def write(case_blocks: list[Block]) -> str:
        kept = needed(case_blocks)
        body = "\n\n".join(b.text for b in case_blocks)
        helper_text = "\n\n".join(h.text for h in kept)
        text = body + "\n" + helper_text
        wanted = set(includes_for(text, include_lines))
        out_head = []
        for line in head.splitlines():
            if line.startswith("#include") and line not in wanted:
                continue
            out_head.append(line)
        head_text = re.sub(r"\n{3,}", "\n\n", "\n".join(out_head))
        # a comment that introduced includes the cut left out goes with them
        head_text = re.sub(r"(?m)^(?://[^\n]*\n)+(?=\n|\Z)", "", head_text)
        parts = [head_text.rstrip("\n") + "\n"]
        if kept:
            parts.append("namespace {\n\n" + helper_text + "\n\n}  // namespace\n")
        parts.append(body + "\n")
        return "\n".join(parts)

    return write(signer_cases), write(crypto_cases)


def add_to_build(cmake: str) -> str:
    line = "    signing/test_signing.cpp\n"
    if "signing/test_crypto.cpp" in cmake:
        return cmake
    if line not in cmake:
        raise SystemExit("c2_cuts: tests/CMakeLists.txt does not list signing/test_signing.cpp")
    return cmake.replace(line, "    signing/test_crypto.cpp\n" + line, 1)


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    root = Path(a.root)
    if (root / CRYPTO).exists():
        print("c2_cuts: already cut")
        return
    signer, crypto = cut((root / SOURCE).read_text(encoding="utf-8"))
    cmake = add_to_build((root / CMAKE).read_text(encoding="utf-8"))
    print(
        f"c2_cuts: {SOURCE} keeps {signer.count('TEST_CASE(')} cases, "
        f"{CRYPTO} takes {crypto.count('TEST_CASE(')}"
    )
    if a.dry_run:
        return
    (root / SOURCE).write_text(signer, encoding="utf-8")
    (root / CRYPTO).write_text(crypto, encoding="utf-8")
    (root / CMAKE).write_text(cmake, encoding="utf-8")


if __name__ == "__main__":
    main()
