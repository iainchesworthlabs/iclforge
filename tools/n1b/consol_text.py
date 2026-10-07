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

import consol_apply
import n1b_docs
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
    # what a build exported, which check_abi_symbols.py --update writes again after a stage
    "tools/ci/abi-allowlist/",
    # the namespace checker's own fixtures, which name libraries of their own (`mp4`)
    "tools/checks/test_check_namespaces.py",
)

CMAKE = ("CMakeLists.txt", ".cmake", ".cmake.in")


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: str
    replacement: object  # a string with group references, or a function of the match
    # which files: "cpp" (C and C++ sources), "cmake", or "text" (every tracked text file)
    kinds: tuple[str, ...] = ("cpp",)
    flags: int = 0
    # whether the plans under planning/ are read too (not this consolidation's own page nor the
    # layout study's two, which name the paths before and after on purpose): a path or a header a
    # plan names follows the tree, as N1B's path pass had it do
    plans: bool = False
    # the files the rule alone reads, by prefix, KEEP's among them (empty: every file of its kinds)
    files: tuple[str, ...] = ()
    # in a C or C++ source, whether the rule reads its string literals and its comments: a name in a
    # string is what a program prints, which C0 to C3 do not change
    strings: bool = True
    comments: bool = True
    # a file this matches is left alone by the rule (a namespace alias that keeps a relative name)
    unless: str = ""
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
        r"\bICLFORGE_AC4(?:DEC|ENC)_"
        r"((?:DEPRECATED_)?(?:NO_)?EXPORT|DEPRECATED|STATIC_DEFINE|BUILDING_SHARED)\b",
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
        "scalar-dir",
        r"\bsrc/ac4(?:core|/src/core)/variants/scalar-(double|float|fixed)\b",
        lambda m: "src/ac4/variants/decode-scalar-"
        + {"double": "float64", "float": "float32", "fixed": "fixed32"}[m.group(1)],
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
        r"\bsrc/ac4dec\b(?![\w-])(?!/[\w.*{])",
        "src/ac4/src/decoder",
        _TEXT,
        plans=True,
    ),
    Rule(
        "dir-enc",
        r"\bsrc/ac4enc\b(?![\w-])(?!/[\w.*{])",
        "src/ac4/src/encoder",
        _TEXT,
        plans=True,
    ),
    # The streams and digests under tests/golden/ac4dec moved with the library's tests (decision
    # 10).
    Rule(
        "golden-dir",
        r"\btests/golden/ac4dec\b(?![\w-])",
        "tests/golden/ac4",
        _TEXT,
        plans=True,
    ),
    # check_ac4_decode_scalar_snr.py keys its floors by each stream's path from the repository's
    # root
    Rule(
        "golden-dir-pins",
        r"\btests/golden/ac4dec/",
        "tests/golden/ac4/",
        _TEXT,
        files=("tests/golden/ac4/scalar-agreement",),
    ),
    # A test that reaches the streams from the external baseline's directory, `/ ".." / "ac4dec" /`
    Rule(
        "golden-path-part",
        r'(/\s*"\.\."\s*/\s*)"ac4dec"(\s*/)',
        r'\1"ac4"\2',
    ),
    # A comment that still spells an AC-4 header as it was before N1B (`ac4enc/encoder.hpp`): N1B's
    # header map, followed to the spelling of today (n1b_docs.new_header).
    Rule(
        "n1b-header",
        r"(?<![A-Za-z0-9_./-])("
        + "|".join(
            re.escape(k)
            for k in sorted(n1b_docs.HEADER_MAP, key=len, reverse=True)
            if k.startswith(("ac4core/", "ac4dec/", "ac4enc/"))
        )
        + r")(?![A-Za-z0-9_])",
        lambda m: n1b_docs.new_header(m.group(1)),
        _TEXT,
    ),
    # The encoder's errata, folded into the decoder's page, link the decoder's entries on the page
    # they are on.
    Rule(
        "errata-self",
        r"\.\./ac4dec/ERRATA\.md#",
        "#",
        _TEXT,
        files=("src/ac4/ERRATA.md",),
    ),
    # A syntax digest names its stream by the path from tests/golden/ (ac4_syntax.stream_label), and
    # the constructed streams moved with tests/golden/ac4dec: the generator writes the new path.
    Rule(
        "digest-stream",
        r"^(# ac4-syntax-digest/1 )\.\./ac4dec/",
        r"\1../ac4/",
        _TEXT,
        re.MULTILINE,
        files=("tests/golden/ac4/",),
    ),
]
# --- C2 ---------------------------------------------------------------------------------------
# arithmetic joins base, admbridge joins adm, and signing is divided: the operator's key, SHA-256
# and HMAC-SHA-256 to iclforge::base::crypto, the EMDF Atmos signer to iclforge::ac3::signing. A
# qualified name follows its declaration; a string literal keeps what it prints (the CLI's messages
# when ADM is not built name iclforge::admbridge); the Catch2 tags and the environment variables
# (ICLFORGE_SIGNING_KEY) keep their names.
C2_CRYPTO = (
    "SigningKey",
    "KeyErrorKind",
    "KeyLoadError",
    "load_signing_key",
    "decode_signing_key",
    "hmac_sha256",
    "sha256",
    "Sha256",
)
C2_SIGNER = (
    "sign_atmos_stream",
    "sign_atmos_frame",
    "has_authenticity_tag",
    "VerifyResult",
    "VerifySummary",
    "verify_atmos_stream",
    "verify_atmos_frame",
)
_CRYPTO = "|".join(C2_CRYPTO)
_SIGNER = "|".join(C2_SIGNER)
_SIGNER_FILES = ("src/ac3/include/iclforge/ac3/signing/", "src/ac3/src/signing/")
_CRYPTO_FILES = ("src/base/include/iclforge/base/crypto/", "src/base/src/crypto/")
C2 = [
    Rule(
        "crypto-name",
        rf"\biclforge::signing::({_CRYPTO})\b",
        r"iclforge::base::crypto::\1",
        _TEXT,
        strings=False,
    ),
    Rule(
        "signer-name",
        rf"\biclforge::signing::({_SIGNER})\b",
        r"iclforge::ac3::signing::\1",
        _TEXT,
        strings=False,
    ),
    Rule(
        "signer-relative",
        rf"(?<![\w:])signing::({_SIGNER})\b",
        r"ac3::signing::\1",
        strings=False,
    ),
    Rule(
        "crypto-namespace",
        r"\bnamespace iclforge::signing\b",
        "namespace iclforge::base::crypto",
        files=_CRYPTO_FILES,
    ),
    Rule(
        "signer-namespace",
        r"\bnamespace iclforge::signing\b",
        "namespace iclforge::ac3::signing",
        files=_SIGNER_FILES,
    ),
    # the signer, in iclforge::ac3::signing now, names the key and the MAC by base's namespace
    Rule(
        "signer-crypto",
        r"(?<![\w:.>/])(SigningKey|hmac_sha256)\b",
        r"base::crypto::\1",
        files=_SIGNER_FILES,
        strings=False,
        comments=False,
    ),
    Rule("bridge-name", r"\biclforge::admbridge::", "iclforge::adm::", _TEXT, strings=False),
    Rule("bridge-relative", r"(?<![\w:])admbridge::(?=\w)", "adm::", _TEXT, strings=False),
    Rule("bridge-namespace", r"\bnamespace iclforge::admbridge\b", "namespace iclforge::adm"),
    # what names a library that goes, in a build file, a script or a page
    Rule(
        "alias-variant",
        r"\biclforge::(signing|admbridge)_(static|shared|objects)\b",
        lambda m: f"iclforge::{C2_INTO[m.group(1)]}_{m.group(2)}",
        _TEXT,
        strings=False,
    ),
    Rule(
        "raw-target",
        r"\biclforge_(signing|admbridge)_(static|shared|objects)\b",
        lambda m: f"iclforge_{C2_INTO[m.group(1)]}_{m.group(2)}",
        _TEXT,
        strings=False,
    ),
    Rule(
        "alias",
        r"\biclforge::(signing|admbridge)\b(?![_:])",
        lambda m: f"iclforge::{C2_INTO[m.group(1)]}",
        ("cmake",),
    ),
    Rule(
        "file",
        r"\blibiclforge_(signing|admbridge)(_static\.a|\.so|\.a|\.dylib|\.dll|\.lib)",
        lambda m: f"libiclforge_{C2_INTO[m.group(1)]}{m.group(2)}",
        _TEXT,
        strings=False,
    ),
    Rule(
        "pkg-config",
        r"\biclforge-(signing|admbridge)\b(?![-\w])",
        lambda m: f"iclforge-{C2_INTO[m.group(1)]}",
        _TEXT,
        strings=False,
    ),
    Rule(
        "macro",
        r"\bICLFORGE_ADMBRIDGE_((?:DEPRECATED_)?(?:NO_)?EXPORT|DEPRECATED|STATIC_DEFINE"
        r"|BUILDING_SHARED)\b",
        r"ICLFORGE_ADM_\1",
        _TEXT,
    ),
    Rule(
        "collapse",
        r"(`?\biclforge::(?:ac3|adm|base)(?:_static|_shared)?\b`?)"
        r"(?:(?:,? and |, | or |/)\1(?![\w:]))+",
        r"\1",
        _TEXT,
    ),
    Rule(
        "dedupe",
        r"(\biclforge::(?:ac3|adm|base)(?:_static|_shared)?)"
        r"(?:[ \t]*\n[ \t]*\1(?![\w:])|[ \t]+\1(?![\w:]))+",
        r"\1",
        ("cmake",),
    ),
]
# A library that goes, and the one a link to it becomes: the signer's users link the codec, which
# links base. arithmetic's links are include paths, and are base's by hand.
C2_INTO = {"signing": "ac3", "admbridge": "adm"}

# The directories of arithmetic and admbridge, named bare in a comment or a page, are base's and
# adm's; arithmetic's header-only target is base's (iclforge::base_arithmetic). signing's went two
# ways, and a sentence that names it is a person's.
C2_PROSE = [
    # the SIMD directories, as today's tree has them and as comments from before N1B spell them
    Rule(
        "arch-header",
        r"\bsrc/arithmetic/arch/(\*|<arch>|\w+)/ac3/internal/arch/simd\.hpp\b",
        r"src/base/variants/arch-\1/iclforge/base/detail/simd.hpp",
        _TEXT,
        plans=True,
    ),
    Rule(
        "arch-dirs",
        r"\bsrc/arithmetic/arch/\{([\w,]+)\}/",
        lambda m: "src/base/variants/{"
        + ",".join("arch-" + x for x in m.group(1).split(","))
        + "}/",
        _TEXT,
        plans=True,
    ),
    Rule(
        "arch-root",
        r"\bsrc/arithmetic/(?:arch|variants)\b/?",
        lambda m: "src/base/variants" + ("/" if m.group(0).endswith("/") else ""),
        _TEXT,
        plans=True,
    ),
    Rule(
        "build-file",
        r"\bsrc/(arithmetic|admbridge)/CMakeLists\.txt\b",
        lambda m: f"src/{ {'arithmetic': 'base', 'admbridge': 'adm'}[m.group(1)] }/CMakeLists.txt",
        _TEXT,
        plans=True,
    ),
    Rule(
        "dir",
        r"\bsrc/(arithmetic|admbridge)\b(?![\w-])(?!/[\w.*{])",
        lambda m: f"src/{ {'arithmetic': 'base', 'admbridge': 'adm'}[m.group(1)] }",
        _TEXT,
        plans=True,
    ),
    Rule(
        "arithmetic-target",
        r"\biclforge::arithmetic\b",
        "iclforge::base_arithmetic",
        _TEXT,
        strings=False,
    ),
    # A page or a comment that names the bridge's library names adm's, and one that names the
    # signer's names the namespace it is in now; what a sentence says of linking it is a person's.
    Rule(
        "bridge-library",
        r"\biclforge::admbridge\b(?![_:])",
        "iclforge::adm",
        _TEXT,
        strings=False,
    ),
    Rule(
        "signer-library",
        r"\biclforge::signing\b(?![_:])",
        "iclforge::ac3::signing",
        _TEXT,
        strings=False,
    ),
    Rule(
        "collapse-again",
        r"(`?\biclforge::adm\b`?)(?:(?:,? and |, | or |/)\1(?![\w:]))+",
        r"\1",
        _TEXT,
    ),
]

# --- C3 ---------------------------------------------------------------------------------------
# mp4, mpegts, matroska, iamf and iec61937 are one library, iclforge::containers, and their
# namespaces nest under it (decision 6): iclforge::mp4 is iclforge::containers::mp4. A name in a
# C++ string keeps what it prints; the options ICLFORGE_BUILD_MP4 and the rest, and the vcpkg and
# Conan features, keep their names.
C3_PARTS = ("mp4", "mpegts", "matroska", "iamf", "iec61937")
_C3_PART = "|".join(C3_PARTS)
C3 = [
    Rule(
        "namespace-qualified",
        rf"\biclforge::({_C3_PART})::",
        r"iclforge::containers::\1::",
        _TEXT,
        strings=False,
    ),
    Rule(
        "namespace-declared",
        rf"\bnamespace iclforge::({_C3_PART})\b",
        r"namespace iclforge::containers::\1",
    ),
    # `using namespace iclforge::mp4;`, `namespace iamf = iclforge::iamf;`, a namespace's closing
    # comment, and a page that names the part
    Rule(
        "namespace-bare",
        rf"\biclforge::({_C3_PART})\b(?![_:])",
        r"iclforge::containers::\1",
        ("cpp",),
        strings=False,
    ),
]
# A name written from inside another iclforge namespace (`iec61937::kBurstBytes` in iclforge::audio)
# nests too, unless the file declares an alias of that name.
C3 += [
    Rule(
        f"relative-{part}",
        rf"(?<![\w:]){part}::(?=\w)",
        f"containers::{part}::",
        strings=False,
        unless=rf"\bnamespace\s+{part}\s*=",
    )
    for part in C3_PARTS
]
C3 += [
    Rule(
        "alias-variant",
        rf"\biclforge::({_C3_PART})_(static|shared|objects)\b",
        r"iclforge::containers_\2",
        _TEXT,
        strings=False,
    ),
    Rule(
        "raw-target",
        rf"\biclforge_({_C3_PART})_(static|shared|objects)\b",
        r"iclforge_containers_\2",
        _TEXT,
        strings=False,
    ),
    Rule("alias", rf"\biclforge::({_C3_PART})\b(?![_:])", "iclforge::containers", ("cmake",)),
    Rule(
        "file",
        rf"\blibiclforge_({_C3_PART})(_static\.a|\.so|\.a|\.dylib|\.dll|\.lib)",
        r"libiclforge_containers\2",
        _TEXT,
        strings=False,
    ),
    Rule(
        "pkg-config",
        rf"\biclforge-({_C3_PART})\b(?![-\w])",
        "iclforge-containers",
        _TEXT,
        strings=False,
    ),
    Rule(
        "macro",
        r"\bICLFORGE_(?:MP4|MPEGTS|MATROSKA|IAMF|IEC61937)_"
        r"((?:DEPRECATED_)?(?:NO_)?EXPORT|DEPRECATED|STATIC_DEFINE|BUILDING_SHARED)\b",
        r"ICLFORGE_CONTAINERS_\1",
        _TEXT,
    ),
    Rule(
        "collapse",
        r"(`?\biclforge::containers(?:_static|_shared)?\b`?)(?:(?:,? and |, | or |/)\1(?![\w:]))+",
        r"\1",
        _TEXT,
    ),
    Rule(
        "dedupe",
        r"(\biclforge::containers(?:_static|_shared)?)"
        r"(?:[ \t]*\n[ \t]*\1(?![\w:])|[ \t]+\1(?![\w:]))+",
        r"\1",
        ("cmake",),
    ),
]
# A page or a comment that names one of the five as a library names its part of the one; their
# directories, named bare, are where their files went; a link to a heading that names a part's
# namespace follows the heading's new anchor.
C3_PROSE = [
    Rule(
        "anchor",
        rf"(\]\([^)\s#]*#)([\w-]*iclforge(?:{_C3_PART})[\w-]*)(\))",
        lambda m: m.group(1)
        + re.sub(rf"iclforge({_C3_PART})", r"iclforgecontainers\1", m.group(2))
        + m.group(3),
        _TEXT,
    ),
    Rule(
        "part-library",
        rf"\biclforge::({_C3_PART})\b(?![_:])",
        r"iclforge::containers::\1",
        _TEXT,
        strings=False,
    ),
    Rule(
        "build-file",
        rf"\bsrc/({_C3_PART})/CMakeLists\.txt\b",
        "src/containers/CMakeLists.txt",
        _TEXT,
        plans=True,
    ),
    # not a path inside another (`src/containers/src/mp4`, which holds `src/mp4`)
    Rule(
        "dir",
        rf"(?<![\w/.-])src/({_C3_PART})\b(?![\w-])(?!/[\w.*{{])(?!\.\w)",
        r"src/containers/src/\1",
        _TEXT,
        plans=True,
    ),
    Rule(
        "test-dir",
        rf"(?<![\w/.-])tests/({_C3_PART})\b(?![\w-])(?!/[\w.*{{])(?!\.\w)",
        r"tests/containers/\1",
        _TEXT,
        plans=True,
    ),
    # what the first run of the two rules above made of a path already inside src/containers
    Rule(
        "nested-dir",
        r"\bsrc/containers/src/containers/src/",
        "src/containers/src/",
        _TEXT,
        plans=True,
    ),
    Rule(
        "nested-test-dir",
        r"\btests/containers/containers/",
        "tests/containers/",
        _TEXT,
        plans=True,
    ),
]

# --- C4's names -------------------------------------------------------------------------------
# Decision 12: what still says a library that merged. A Catch2 tag names the codec and the area, as
# the directory does ([ac4dec] is [ac4][decoder]; a run of tags that has [ac4] already keeps one);
# the tests' environment variables and compile definitions name the codec, with the direction where
# the decoder's and the encoder's are the same variable; the scalar tier's macro takes the project's
# prefix; the install check's encoder program its file's new name.
C4N_TAGS = {
    "ac4dec": ("ac4", "decoder"),
    "ac4enc": ("ac4", "encoder"),
    "ac4core": ("ac4", "core"),
    "admbridge": ("adm", "bridge"),
}


def _c4n_tags(m: re.Match[str]) -> str:
    out: list[str] = []
    for tag in re.findall(r"\[([^\]]*)\]", m.group(0)):
        for t in C4N_TAGS.get(tag, (tag,)):
            if t not in out:
                out.append(t)
    return "".join(f"[{t}]" for t in out)


_C4N_OLD_TAG = "|".join(C4N_TAGS)


def _c4n_basenames() -> dict[str, str]:
    """The file names C4's names stage gave, old -> new, from consoldef.py's moves: a page or a
    comment that names a test file without its directory, which the path pass does not read."""
    import consoldef

    olds = [*consoldef.C4N_EXACT, *_C4N_EXAMPLES]
    return {Path(o).name: Path(consoldef.c4n_new(o) or o).name for o in olds}


# Names of each shape, for _c4n_basenames(): the rules map the rest the same way.
_C4N_EXAMPLES = (
    "tests/ac4/decoder/ac4dec_bits.hpp",
    "tests/ac4/decoder/ac4dec_constructed.hpp",
    "tests/ac4/decoder/ac4dec_constructed.cpp",
    "tests/ac4/decoder/ac4dec_hsf.hpp",
    "tests/ac4/decoder/ac4dec_hsf.cpp",
    "tests/ac4/decoder/ac4dec_mux.hpp",
    "tests/ac4/decoder/ac4dec_mux.cpp",
    "tests/ac4/decoder/ac4dec_objects.hpp",
    "tests/ac4/decoder/ac4dec_objects.cpp",
    "tests/ac4/decoder/ac4dec_printed_matrices.hpp",
    "tests/ac4/decoder/ac4dec_units.hpp",
    "tests/ac4/core/test_ac4_presentation_configs.cpp",
    "tests/ac4/core/test_ac4_toc_syntax.cpp",
    "tests/ac4/io/test_ac4_splitter.cpp",
)


def _c4n_bare(m: re.Match[str]) -> str:
    name = m.group(0)
    known = _c4n_basenames()
    if name in known:
        return known[name]
    return re.sub(r"^test_ac4(?:dec|enc|core)_", "test_", name)


C4N = [
    # a test file or a helper named without its directory
    Rule(
        "bare-name",
        r"(?<![\w/.-])(?:test_ac4(?:dec|enc|core)_\w+\.cpp|test_ac4(?:_\w+)?\.cpp"
        r"|ac4dec_(?:bits|constructed|hsf|mux|objects|printed_matrices|units)\.[ch]pp"
        r"|ac4_toc_writer\.hpp|consumer_ac4enc\.cpp)",
        _c4n_bare,
        _TEXT,
        plans=True,
    ),
    Rule("bare-brace", r"(?<![\w/.-])test_ac4(?:dec|enc|core)_\{", "test_{", _TEXT, plans=True),
    # the tests' own namespaces
    Rule("test-namespace", r"\bac4dec_test\b", "ac4_decoder_test", ("cpp",)),
    Rule("units-namespace", r"\bac4dec_units\b", "ac4_units", ("cpp",)),
    Rule(
        "tags",
        rf"(?:\[[\w.!-]+\])*\[(?:{_C4N_OLD_TAG})\](?:\[[\w.!-]+\])*",
        _c4n_tags,
        _TEXT,
        plans=True,
    ),
    Rule("write-env", r"\bAC4(DEC|ENC)_WRITE_(\w+)", lambda m: "AC4_"
         + ("DECODER" if m.group(1) == "DEC" else "ENCODER") + "_WRITE_" + m.group(2), _TEXT,
         plans=True),
    Rule("env", r"\bAC4DEC_(GOLDEN_DIR|STREAM_DIR|API_STREAM_DIR|AJOC_STREAM|TRACE_DIR)\b",
         r"AC4_\1", _TEXT, plans=True),
    Rule("double-macro", r"\bAC4CORE_ALSO_AT_DOUBLE\b", "ICLFORGE_AC4_ALSO_AT_DOUBLE", _TEXT,
         plans=True),
    Rule("consumer", r"\b(pc_)?consumer_ac4enc\b", r"\1consumer_ac4_encoder", _TEXT),
    Rule("consumer-list", r"\bac4enc_consumers\b", "ac4_encoder_consumers", _TEXT),
    Rule("adm-scratch", r"\badmbridge_(write|zones|divergence)_", r"adm_bridge_\1_", ("cpp",)),
]

# --- C4 ---------------------------------------------------------------------------------------
# One bit reader and one writer, iclforge::BitReader and iclforge::BitWriter (src/base), for AC-4's
# and base's: AC-4's code includes base's headers and calls the one name each operation has. The
# reader's position and overflow flag are base's names (bit_position(), overflowed()), its peek is
# peek(); the writer's count is base's bit_count(), which AC-3's bit counters share, and both
# align with align(). The inspector's reader and the DSI writer, which are not these classes, are
# rewritten by hand (their files are left alone), and so are the copies of the old classes that
# tests/base/test_bit_io.cpp holds the new ones to.
_C4_AC4 = ("src/ac4/", "tests/ac4/", "fuzz/")
_C4_HAND = r"class (?:Reader|DsiWriter) \{|namespace old_base \{"
C4 = [
    Rule(
        "reader-include",
        r'(#\s*include\s*")core/bit_reader\.hpp(")',
        r"\1iclforge/base/bitreader.hpp\2",
    ),
    Rule(
        "writer-include",
        r'(#\s*include\s*")core/bit_writer\.hpp(")',
        r"\1iclforge/base/bitwriter.hpp\2",
    ),
    # AC-4's writer counted with bit_position(), its reader with position(): the first is the
    # writer's bit_count(), and only then does the second take bit_position()
    Rule("writer-count", r"\.bit_position\(\)", ".bit_count()", files=_C4_AC4, unless=_C4_HAND,
         strings=False, comments=False),
    Rule("reader-position", r"\.position\(\)", ".bit_position()", files=_C4_AC4,
         unless=_C4_HAND, strings=False, comments=False),
    Rule("reader-overflow", r"\.overflow\(\)", ".overflowed()", files=_C4_AC4, unless=_C4_HAND,
         strings=False, comments=False),
    Rule("reader-peek", r"\.peek_raw\(", ".peek(", files=_C4_AC4, strings=False, comments=False),
    Rule("writer-codeword", r"\b(\w+)\.write_codeword\(", r"write_codeword(\1, ", files=_C4_AC4,
         strings=False, comments=False),
    Rule("writer-align", r"\.byte_align\(\)", ".align()", unless=_C4_HAND, strings=False,
         comments=False),
]

# C4's rules above translate one API into another (AC-4's reader's bit_position() is the writer's
# bit_count(), and the reader's position() becomes bit_position()), so a second run would rename
# what the first wrote. They run once, on a tree that still has AC-4's own reader, which C4's hand
# commit deletes; on any other tree the stage changes nothing.
ONCE = {"c4": "src/ac4/src/core/bit_reader.hpp"}

# base's header-only target carries the bit reader and writer, the CRC, the trace and the speakers
# now, beside the arithmetic: what it is is base's headers.
C4B = [
    Rule("base-headers", r"\biclforge(::|_)base_arithmetic\b", r"iclforge\1base_headers", _TEXT),
    # the reader's records are record() and record_element(): Qt defines `emit` as a macro
    Rule("reader-record", r"\.emit(_element)?\(", r".record\1(", files=_C4_AC4, strings=False,
         comments=False),
    # what AC-4's code and tests named in its own namespace is iclforge's
    Rule(
        "qualified",
        r"\b(?:iclforge::ac4::)?detail::(BitReader|BitWriter|variable_bits_width)\b",
        r"iclforge::\1",
        files=_C4_AC4,
    ),
]

# --- C5 ---------------------------------------------------------------------------------------
# AC-4's kernels are dsp's (decision 14): iclforge::ac4::detail::dsp is iclforge::dsp::tiered, and
# the tables that moved with them iclforge::dsp::tiered::tables. Inside iclforge::ac4::detail a bare
# `dsp::` named AC-4's own namespace; it names iclforge::dsp now, so it gains `tiered::`, except in
# a file whose `dsp` is a namespace alias (which the qualified rule rewrites). A moved table is
# spelled in full wherever AC-4 named it, since AC-4's `tables` keeps the rest. The moved kernels
# take their scalar from dsp's tier header.
_C5_MOVED = ("src/dsp/src/tiered/", "tests/dsp/tiered/")
_C5_USERS = ("src/ac4/", "tests/ac4/", "tests/dsp/tiered/", "tests/performance/",
             "src/dsp/src/tiered/")
_C5_TABLES = (
    r"kQwin|kQwinQ30|kAspxNoise|kAspxNoiseQ24|kCosQuadrant|kFftRoots\d+|kPreTwiddle\d+|kKbdLeft\d+"
)
C5 = [
    Rule(
        "tables-namespace",
        r"\bnamespace iclforge::ac4::detail::tables\b",
        "namespace iclforge::dsp::tiered::tables",
        files=("src/dsp/src/tiered/tables/",),
    ),
    Rule(
        "namespace",
        r"\bnamespace iclforge::ac4::detail::dsp\b",
        "namespace iclforge::dsp::tiered",
        files=_C5_MOVED,
    ),
    Rule("qualified", r"\biclforge::ac4::detail::dsp\b", "iclforge::dsp::tiered"),
    # from inside iclforge::ac4, `detail::dsp::` and `ac4::detail::dsp::`
    Rule("partly-qualified", r"(?<![\w:])(?:ac4::)?detail::dsp::", "dsp::tiered::", files=_C5_USERS,
         strings=False),
    Rule("table", rf"(?:\biclforge::ac4::detail::|(?<![\w:]))tables::({_C5_TABLES})\b",
         r"iclforge::dsp::tiered::tables::\1", files=_C5_USERS),
    Rule("bare", r"(?<![\w:])dsp::(?!tiered\b)", "dsp::tiered::", files=_C5_USERS,
         unless=r"namespace dsp = ", strings=False),
    Rule("real", r'(#\s*include\s*")iclforge/ac4/detail/real\.hpp(")',
         r"\1iclforge/dsp/tiered/real.hpp\2", files=_C5_MOVED),
    Rule("double-macro", r"\bICLFORGE_AC4_ALSO_AT_DOUBLE\b", "ICLFORGE_DSP_ALSO_AT_DOUBLE",
         files=_C5_MOVED),
]

STAGES: dict[str, list[Rule]] = {
    "c0": C0,
    "c1": C1 + C1_PROSE,
    "c2": C2 + C2_PROSE,
    "c3": C3 + C3_PROSE,
    "c4n": C4N,
    "c4": C4,
    "c4b": C4B,
    "c5": C5,
}


def kind_of(path: str) -> set[str]:
    kinds = {"text"}
    if Path(path).suffix in CPP_EXT or path.endswith((".hpp.in", ".h.in")):
        kinds.add("cpp")
    if path.endswith(CMAKE):
        kinds.add("cmake")
    return kinds


HELD_PLANS = KEEP[1:4]

_RAW_OPEN = re.compile(r'(?:u8|[uUL])?R"([^ ()\\\t\n]{0,16})\(')


def cpp_segments(text: str) -> list[tuple[str, str]]:
    """A C or C++ source as runs of code, comments and string or character literals (raw strings
    too), so that a rule can leave a literal or a comment alone. A digit separator (`1'000`) is
    code."""
    out: list[tuple[str, str]] = []
    i, start, n = 0, 0, len(text)

    def flush(upto: int) -> None:
        if upto > start:
            out.append(("code", text[start:upto]))

    while i < n:
        c = text[i]
        if c == "/" and text.startswith("//", i):
            flush(i)
            j = text.find("\n", i)
            j = n if j == -1 else j
            out.append(("comment", text[i:j]))
            i = start = j
        elif c == "/" and text.startswith("/*", i):
            flush(i)
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append(("comment", text[i:j]))
            i = start = j
        elif (
            c in "uULR"
            and (m := _RAW_OPEN.match(text, i))
            and (i == 0 or not text[i - 1].isalnum())
        ):
            flush(i)
            close = ")" + m.group(1) + '"'
            j = text.find(close, m.end())
            j = n if j == -1 else j + len(close)
            out.append(("string", text[i:j]))
            i = start = j
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            flush(i)
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(("string", text[i:j]))
            i = start = j
        else:
            i += 1
    flush(n)
    return out


def rewrite(
    text: str,
    rules: list[Rule],
    kinds: set[str],
    counts: Counter,
    plan_page: bool = False,
    path: str = "",
) -> str:
    for rule in rules:
        if not kinds.intersection(rule.kinds) or (plan_page and not rule.plans):
            continue
        if rule.files and not path.startswith(rule.files):
            continue
        if rule.unless and re.search(rule.unless, text):
            continue
        if "cpp" in kinds and not (rule.strings and rule.comments):
            out = []
            for kind, chunk in cpp_segments(text):
                if kind == "code" or (kind == "string" and rule.strings) or (
                    kind == "comment" and rule.comments
                ):
                    chunk, n = rule.compiled.subn(rule.replacement, chunk)
                    counts[rule.name] += n
                out.append(chunk)
            text = "".join(out)
            continue
        text, n = rule.compiled.subn(rule.replacement, text)
        counts[rule.name] += n
    return text


def spelling_rules(plan: Path) -> list[Rule]:
    """The include spellings a stage changed (its plan's map, and the cut's), wherever a page or a
    comment names one: n1b_docs.py's `header` rule for the consolidation."""
    made = json.loads(plan.read_text(encoding="utf-8"))
    spellings = dict(made.get("spellings", {}))
    spellings.update(consol_apply.spellings_of(made.get("moves", {})))
    spellings.update(CUT_SPELLINGS.get(made["stage"], {}))
    if not spellings:
        return []
    alternation = "|".join(re.escape(old) for old in sorted(spellings, key=len, reverse=True))
    return [
        Rule(
            "header",
            r"(?<![\w/])(" + alternation + r")(?![\w/])",
            lambda m: spellings[m.group(1)],
            _TEXT,
            plans=True,
        )
    ]


def run(
    root: Path, stage: str, dry_run: bool, report: Path | None, plan: Path | None = None
) -> Counter:
    counts: Counter = Counter()
    if stage in ONCE and not (root / ONCE[stage]).exists():
        print(f"{stage}: already applied ({ONCE[stage]} is gone); nothing to do")
        return counts
    repo = Repo(str(root))
    rules = STAGES[stage] + (spelling_rules(plan) if plan else [])
    changed: list[str] = []
    for f in repo.files:
        plan_page = f.startswith("planning/")
        kept = f.startswith(KEEP) and not (plan_page and not f.startswith(HELD_PLANS))
        file_rules = [r for r in rules if r.files and f.startswith(r.files)] if kept else rules
        if not file_rules:
            continue
        path = root / f
        try:
            text = path.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        out = rewrite(text, file_rules, kind_of(f), counts, plan_page, f)
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
