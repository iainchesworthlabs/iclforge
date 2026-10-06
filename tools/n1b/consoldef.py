"""The consolidation's moves as data (planning/consolidation.md, appendices A and B).

Each stage is a function old_path -> new_path (None when the file stays), in the manner of
layoutdef.py's layouts, so that n1b_apply.py's include rewrite, n1b_cmake.py and n1b_paths.py can
be pointed at it (consol_apply.py). The spelling a `#include` uses to reach a file is
layoutdef.spelling_of(), which needs nothing of a layout: the path below `include/`, or below a
variant directory.

  c1  AC-4 one library: src/ac4, src/ac4core, src/ac4dec and src/ac4enc into src/ac4, laid out by
      AC-3's areas (core, io, decoder, encoder); tests/ac4*, and tests/golden/ac4dec, into
      tests/ac4 and tests/golden/ac4 (the user's decision 10: the tests mirror src/).
  c2  src/arithmetic into src/base; src/admbridge into src/adm; src/signing split, its key, SHA-256
      and HMAC to src/base (crypto/) and its EMDF signer to src/ac3 (signing/).
  c3  src/mp4, mpegts, matroska, iamf and iec61937 into src/containers, one directory each.

The cuts that come before C1's moves (ac4.hpp into three headers, the decoder's and the encoder's
headers into four and two, ac4.cpp into three units) are ac4_cuts.py's; the files they make are
named here by the paths they have after the cut, in today's layout.
"""

from __future__ import annotations

import re

# --- C1 ---------------------------------------------------------------------------------------

# After ac4_cuts.py: the inspector's header and unit in three, by what each declares. The unit's
# table-of-contents part keeps ac4.cpp's name through the cut, so that this move keeps its history.
C1_EXACT = {
    "src/ac4/include/iclforge/ac4/toc.hpp": "src/ac4/include/iclforge/ac4/core/toc.hpp",
    "src/ac4/include/iclforge/ac4/syntax.hpp": "src/ac4/include/iclforge/ac4/core/syntax.hpp",
    "src/ac4/include/iclforge/ac4/elementary.hpp": "src/ac4/include/iclforge/ac4/io/elementary.hpp",
    "src/ac4/include/iclforge/ac4/carriage.hpp": "src/ac4/include/iclforge/ac4/io/carriage.hpp",
    "src/ac4/src/ac4.cpp": "src/ac4/src/core/toc.cpp",
    "src/ac4/src/elementary.cpp": "src/ac4/src/io/elementary.cpp",
    "src/ac4/src/carriage.cpp": "src/ac4/src/io/carriage.cpp",
    "src/ac4dec/ERRATA.md": "src/ac4/ERRATA.md",
    "src/ac4dec/src/bit_reader.hpp": "src/ac4/src/core/bit_reader.hpp",
    "src/ac4enc/src/bit_writer.hpp": "src/ac4/src/core/bit_writer.hpp",
    "src/ac4enc/src/bit_writer.cpp": "src/ac4/src/core/bit_writer.cpp",
    # the inspector's tests: the splitter is io's, the rest the table of contents'
    "tests/ac4/test_ac4_splitter.cpp": "tests/ac4/io/test_ac4_splitter.cpp",
}

# The AC-4 core's scalar directories take the tier's names every codec uses.
C1_SCALAR = {"double": "float64", "float": "float32", "fixed": "fixed32"}

C1_RULES: list[tuple[re.Pattern, str]] = [
    (re.compile(r"^src/ac4core/include/iclforge/ac4core/(.+)$"), r"src/ac4/src/core/\1"),
    (re.compile(r"^src/ac4core/src/(.+)$"), r"src/ac4/src/core/\1"),
    (
        re.compile(r"^src/ac4dec/include/iclforge/ac4dec/(.+\.hpp)$"),
        r"src/ac4/include/iclforge/ac4/decoder/\1",
    ),
    (re.compile(r"^src/ac4dec/src/(.+)$"), r"src/ac4/src/decoder/\1"),
    (
        re.compile(r"^src/ac4enc/include/iclforge/ac4enc/(.+\.hpp)$"),
        r"src/ac4/include/iclforge/ac4/encoder/\1",
    ),
    (re.compile(r"^src/ac4enc/src/(.+)$"), r"src/ac4/src/encoder/\1"),
    (re.compile(r"^tests/ac4/([^/]+)$"), r"tests/ac4/core/\1"),
    (re.compile(r"^tests/ac4core/(.+)$"), r"tests/ac4/core/\1"),
    (re.compile(r"^tests/ac4dec/(.+)$"), r"tests/ac4/decoder/\1"),
    (re.compile(r"^tests/ac4enc/(.+)$"), r"tests/ac4/encoder/\1"),
    (re.compile(r"^tests/golden/ac4dec/(.+)$"), r"tests/golden/ac4/\1"),
]

# Written by hand, not moved: the build files of the merged library, the encoder's errata (a
# section of src/ac4/ERRATA.md), the core's build file.
C1_REMOVED = (
    "src/ac4core/CMakeLists.txt",
    "src/ac4dec/CMakeLists.txt",
    "src/ac4enc/CMakeLists.txt",
    "src/ac4enc/ERRATA.md",
)

_SCALAR_VARIANT = re.compile(
    r"^src/ac4core/variants/scalar-(?P<c>double|float|fixed)/iclforge/ac4core/detail/(?P<f>[^/]+)$"
)


def c1_new(path: str) -> str | None:
    if path in C1_REMOVED:
        return None
    if path in C1_EXACT:
        return C1_EXACT[path]
    if path.startswith(("src/ac4/", "tests/ac4/")) and path.count("/") > 2:
        # already in the merged layout (a second run), or a directory C1 leaves
        if not re.match(r"^tests/ac4/[^/]+$", path):
            return None
    m = _SCALAR_VARIANT.match(path)
    if m:
        tier = C1_SCALAR[m.group("c")]
        return f"src/ac4/variants/decode-scalar-{tier}/iclforge/ac4/detail/{m.group('f')}"
    for rx, repl in C1_RULES:
        if rx.match(path):
            return rx.sub(repl, path)
    return None


# --- C2 ---------------------------------------------------------------------------------------

C2_EXACT = {
    "src/signing/include/iclforge/signing/signing_key.hpp": (
        "src/base/include/iclforge/base/crypto/signing_key.hpp"
    ),
    "src/signing/src/signing_key.cpp": "src/base/src/crypto/signing_key.cpp",
    "src/signing/src/sha256.hpp": "src/base/include/iclforge/base/crypto/sha256.hpp",
    "src/signing/src/sha256.cpp": "src/base/src/crypto/sha256.cpp",
    "src/signing/src/hmac_sha256.hpp": "src/base/include/iclforge/base/crypto/hmac_sha256.hpp",
    "src/signing/src/hmac_sha256.cpp": "src/base/src/crypto/hmac_sha256.cpp",
    "src/signing/include/iclforge/signing/emdf_atmos_signer.hpp": (
        "src/ac3/include/iclforge/ac3/signing/emdf_atmos_signer.hpp"
    ),
    "src/signing/src/emdf_atmos_signer.cpp": "src/ac3/src/signing/emdf_atmos_signer.cpp",
    "src/admbridge/ERRATA.md": "src/adm/ERRATA.md",
    # the primitives' and the key's tests, cut from tests/signing/test_signing.cpp (c2_cuts.py)
    "tests/signing/test_crypto.cpp": "tests/base/test_crypto.cpp",
}

C2_RULES: list[tuple[re.Pattern, str]] = [
    (
        re.compile(r"^src/arithmetic/include/iclforge/arithmetic/(.+)$"),
        r"src/base/include/iclforge/base/arithmetic/\1",
    ),
    (
        re.compile(r"^src/arithmetic/variants/(arch-[^/]+)/iclforge/arithmetic/detail/(.+)$"),
        r"src/base/variants/\1/iclforge/base/detail/\2",
    ),
    (
        re.compile(r"^src/admbridge/include/iclforge/admbridge/(.+)$"),
        r"src/adm/include/iclforge/adm/\1",
    ),
    (re.compile(r"^src/admbridge/src/(.+)$"), r"src/adm/src/\1"),
    (re.compile(r"^tests/admbridge/(.+)$"), r"tests/adm/\1"),
    (re.compile(r"^tests/signing/(.+)$"), r"tests/ac3/signing/\1"),
]

C2_REMOVED = (
    "src/arithmetic/CMakeLists.txt",
    "src/signing/CMakeLists.txt",
    "src/admbridge/CMakeLists.txt",
)


def c2_new(path: str) -> str | None:
    if path in C2_REMOVED:
        return None
    if path in C2_EXACT:
        return C2_EXACT[path]
    for rx, repl in C2_RULES:
        if rx.match(path):
            return rx.sub(repl, path)
    return None


# --- C3 ---------------------------------------------------------------------------------------

CONTAINERS = ("mp4", "mpegts", "matroska", "iamf", "iec61937")
_C3 = "|".join(CONTAINERS)
C3_RULES: list[tuple[re.Pattern, str]] = [
    (
        re.compile(rf"^src/({_C3})/include/iclforge/\1/(.+)$"),
        r"src/containers/include/iclforge/containers/\1/\2",
    ),
    (re.compile(rf"^src/({_C3})/src/(.+)$"), r"src/containers/src/\1/\2"),
    (re.compile(rf"^src/({_C3})/(ERRATA\.md|README\.md)$"), r"src/containers/\1/\2"),
    (re.compile(rf"^tests/({_C3})/(.+)$"), r"tests/containers/\1/\2"),
]

C3_REMOVED = tuple(f"src/{c}/CMakeLists.txt" for c in CONTAINERS)


def c3_new(path: str) -> str | None:
    if path in C3_REMOVED:
        return None
    for rx, repl in C3_RULES:
        if rx.match(path):
            return rx.sub(repl, path)
    return None


# Files that are not moved but folded into another by hand, which the pages and comments that name
# them follow as they follow a move (consol_paths.py).
FOLDED = {
    "c1": {
        "src/ac4enc/ERRATA.md": "src/ac4/ERRATA.md",
        # the header the cut divided: the table of contents' keeps its file comment
        "src/ac4/include/iclforge/ac4/ac4.hpp": "src/ac4/include/iclforge/ac4/core/toc.hpp",
    },
    "c2": {},
    "c3": {},
}

STAGES = {"c1": c1_new, "c2": c2_new, "c3": c3_new}
REMOVED = {"c1": C1_REMOVED, "c2": C2_REMOVED, "c3": C3_REMOVED}

# The libraries each stage merges, old -> new: what a target, an export macro, an export header, a
# pkg-config name or an ABI allowlist follows (consol_apply.py, export_diff.py --map,
# abi_compare.py).
LIBRARY_MAP = {
    "c1": {"ac4": "ac4", "ac4core": "ac4", "ac4dec": "ac4", "ac4enc": "ac4"},
    "c2": {"arithmetic": "base", "admbridge": "adm", "signing": "ac3"},
    "c3": {c: "containers" for c in CONTAINERS},
}


def moves(stage: str, files: list[str]) -> dict[str, str]:
    """Old path -> new path for every tracked file the stage moves."""
    fn = STAGES[stage]
    out = {}
    for f in files:
        new = fn(f)
        if new and new != f:
            out[f] = new
    return out
