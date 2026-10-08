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
    "c4n": {},
    "c4": {},
    "c5": {},
    "c6": {},
    "c7-1": {},
}

# --- C4's names -------------------------------------------------------------------------------
# Before C4, the user's decision 12: the tests' files and helpers that a library which merged still
# names drop its prefix, since their directory says it (tests/ac4/decoder/test_ac4dec_drc.cpp is
# tests/ac4/decoder/test_drc.cpp, as tests/ac3/decoder/test_decoder.cpp is), and the inspector's
# say what of the table of contents they test.
C4N_EXACT = {
    "tests/ac4/core/test_ac4.cpp": "tests/ac4/core/test_toc.cpp",
    "tests/ac4/core/ac4_toc_writer.hpp": "tests/ac4/core/toc_writer.hpp",
    "tools/checks/install_consumer/consumer_ac4enc.cpp":
        "tools/checks/install_consumer/consumer_ac4_encoder.cpp",
}

C4N_RULES: list[tuple[re.Pattern, str]] = [
    (re.compile(r"^tests/ac4/(core|decoder|encoder|io)/test_ac4(?:core|dec|enc)?_(.+)$"),
     r"tests/ac4/\1/test_\2"),
    (re.compile(r"^tests/ac4/decoder/ac4dec_(.+)$"), r"tests/ac4/decoder/\1"),
]


def c4n_new(path: str) -> str | None:
    if path in C4N_EXACT:
        return C4N_EXACT[path]
    for rx, repl in C4N_RULES:
        if rx.match(path):
            return rx.sub(repl, path)
    return None


# --- C4 ---------------------------------------------------------------------------------------
# What object audio metadata means and the object and ISF rendering are the codec's object area,
# as AC-3's are src/ac3/src/oba/ (planning/consolidation.md (d)). The rest of C4 merges copies into
# one, which is by hand.
C4_EXACT = {
    f"src/ac4/src/decoder/pcm/{name}": f"src/ac4/src/oba/{name}"
    for name in ("objects.hpp", "objects.cpp", "isf.hpp", "isf.cpp")
}


def c4_new(path: str) -> str | None:
    return C4_EXACT.get(path)


# --- C5 ---------------------------------------------------------------------------------------
# AC-4's transforms, QMF banks and sample rate converter, and the tables they read, are dsp's
# (decision 14): src/dsp/src/tiered/, namespace iclforge::dsp::tiered, the kernels written against a
# scalar tier, beside dsp's own. The QMF tables move whole: QWIN is the banks' window, and Annex
# D's file holds A-SPX's noise table beside it, which its generator writes together.
C5_TABLES = ("qmf_tables.hpp", "qmf_tables.cpp", "qmf_tables_fixed.hpp", "qmf_twiddles.hpp",
             "transform_tables.hpp")
C5_TESTS = ("dsp", "dsp_exact", "resampler", "transform_tables", "portable_math")
C5_RULES: list[tuple[re.Pattern, str]] = [
    (re.compile(r"^src/ac4/src/core/dsp/(.+)$"), r"src/dsp/src/tiered/\1"),
    (re.compile(rf"^src/ac4/src/core/tables/({'|'.join(map(re.escape, C5_TABLES))})$"),
     r"src/dsp/src/tiered/tables/\1"),
    (
        re.compile(rf"^tests/ac4/core/test_({'|'.join(C5_TESTS)})\.cpp$"),
        r"tests/dsp/tiered/test_\1.cpp",
    ),
]


def c5_new(path: str) -> str | None:
    for rx, repl in C5_RULES:
        if rx.match(path):
            return rx.sub(repl, path)
    return None


# --- C6 ---------------------------------------------------------------------------------------
# What ac3 held that is not AC-3's is base's: the family's version, WAV reading and writing
# (decision 16) and the level and loudness meters (decision 17). Each file moves whole and is then
# cut by hand: AC-3's mappings, its acmod and channel-map forms of the meters and the aliases are
# new files at the old paths.
C6_EXACT = {
    "src/ac3/include/iclforge/ac3/version.hpp.in": "src/base/include/iclforge/base/version.hpp.in",
    "src/ac3/src/version.cpp": "src/base/src/version.cpp",
    "src/ac3/include/iclforge/ac3/io/wav.hpp": "src/base/include/iclforge/base/wav.hpp",
    "src/ac3/include/iclforge/ac3/analysis/levels.hpp": "src/base/include/iclforge/base/levels.hpp",
    "src/ac3/include/iclforge/ac3/meta/loudness.hpp": "src/base/include/iclforge/base/loudness.hpp",
    "src/ac3/src/analysis/levels.cpp": "src/base/src/levels.cpp",
    "src/ac3/src/meta/loudness.cpp": "src/base/src/loudness.cpp",
} | {
    f"src/ac3/src/io/{name}": f"src/base/src/{name}"
    for name in ("wav.cpp", "wav_format.cpp", "wav_format.hpp", "wav_stream_reader.cpp",
                 "wav_stream_writer.cpp")
}


def c6_new(path: str) -> str | None:
    return C6_EXACT.get(path)


# --- C7-1 (planning/monorepo.md) ------------------------------------------------------------------
# The libraries are projects of their own under libs/: each with its tests and its fuzz targets
# beside it (decisions 1 and 3), the cross-project test helpers in tests/support (the test-support
# library), the vendored time filter in external/ (decision 14), the fuzz scripts in tools/fuzz.
C7_LIBS = ("ac3", "ac4", "adm", "audio", "base", "capi", "containers", "dsp", "iab", "objects",
           "render", "sendspin")
C7_FUZZ_LIB = {
    "ac3_decode": "ac3", "differential_ac3_decode": "ac3", "differential_eac3_decode": "ac3",
    "eac3_decode": "ac3", "scan": "ac3", "signing_verify": "ac3", "joc_parse": "ac3",
    "ac4_decode": "ac4", "ac4_encode": "ac4", "ac4_parse": "ac4", "adm_parse": "adm",
    "iab_parse": "iab", "iamf_parse": "containers", "iec61937_unwrap": "containers",
    "matroska_demux": "containers", "mp4_demux": "containers", "mpegts_demux": "containers",
    "emdf_parse": "objects", "oamd_parse": "objects", "osc_parse": "objects",
    "sendspin_frames": "sendspin", "sendspin_handshake": "sendspin", "sendspin_json": "sendspin",
    "sendspin_messages": "sendspin", "wav_read": "base",
}
C7_FUZZ_SHARED = {"crc_mutator.hpp": "ac3", "differential_oracle.hpp": "ac3"}
C7_SUPPORT = ("tests/platform/", "tests/crt/", "tests/sanitized.hpp", "tests/ac4_stream_kinds.hpp",
              "tests/audio/alsa_null_device.hpp")


def c7_1_new(path: str) -> str | None:
    p = path.split("/")
    if path.startswith("src/sendspin/third_party/time-filter/"):
        return "external/time-filter/" + path[len("src/sendspin/third_party/time-filter/"):]
    if p[0] == "src" and len(p) > 2 and p[1] in C7_LIBS:
        return "libs/" + "/".join(p[1:])
    for s in C7_SUPPORT:
        if path == s or (s.endswith("/") and path.startswith(s)):
            rest = path[len("tests/"):]
            return "tests/support/" + (rest.split("/", 1)[1] if rest.startswith("audio/") else rest)
    if p[0] == "tests" and len(p) > 2 and p[1] in C7_LIBS:
        return f"libs/{p[1]}/tests/" + "/".join(p[2:])
    if p[0] == "fuzz":
        if len(p) == 2:
            m = re.match(r"fuzz_(.+)\.cpp$", p[1])
            if m and m.group(1) in C7_FUZZ_LIB:
                return f"libs/{C7_FUZZ_LIB[m.group(1)]}/fuzz/{p[1]}"
            if p[1] in C7_FUZZ_SHARED:
                return f"libs/{C7_FUZZ_SHARED[p[1]]}/fuzz/{p[1]}"
            if p[1] == "CMakeLists.txt":
                return "cmake/IclforgeFuzz.cmake"
            return f"tools/fuzz/{p[1]}"
        if p[1] in ("seeds", "regressions") and len(p) > 3:
            lib = C7_FUZZ_LIB.get(p[2].removeprefix("fuzz_"))
            if lib:
                return f"libs/{lib}/fuzz/{p[1]}/" + "/".join(p[2:])
    return None


STAGES = {"c1": c1_new, "c2": c2_new, "c3": c3_new, "c4n": c4n_new, "c4": c4_new, "c5": c5_new,
          "c6": c6_new, "c7-1": c7_1_new}
REMOVED = {"c1": C1_REMOVED, "c2": C2_REMOVED, "c3": C3_REMOVED, "c4n": (), "c4": (), "c5": (),
           "c6": (), "c7-1": ()}

# The libraries each stage merges, old -> new: what a target, an export macro, an export header, a
# pkg-config name or an ABI allowlist follows (consol_apply.py, export_diff.py --map,
# abi_compare.py).
LIBRARY_MAP = {
    "c1": {"ac4": "ac4", "ac4core": "ac4", "ac4dec": "ac4", "ac4enc": "ac4"},
    "c2": {"arithmetic": "base", "admbridge": "adm", "signing": "ac3"},
    "c3": {c: "containers" for c in CONTAINERS},
    "c4n": {},
    "c4": {},
    "c5": {},
    "c6": {},
    "c7-1": {},
}

# The libraries a stage divides, old -> every library its files went to: signing's key, hash and
# MAC are base's and its signer ac3's, so the exports of signing, ac3 and base are compared as one
# group (export_diff.py, abi_compare.py). C6 gives part of ac3 to base.
SPLITS = {"c1": {}, "c2": {"signing": ("ac3", "base")}, "c3": {}, "c4n": {}, "c4": {}, "c5": {},
          "c6": {"ac3": ("ac3", "base")}, "c7-1": {}}

# The names C6 gives base: what was iclforge::ac3::io's of WAV, iclforge::ac3::analysis's and
# iclforge::ac3::meta's of the meters but their acmod and channel-map constructors, and the version.
C6_WAV = ("WavData", "WavError", "WavPcm16StreamWriter", "WavStreamReader", "WavStreamWriter",
          "read_wav", "write_wav_f32", "write_wav_pcm16_raw")
C6_LEVELS = ("ChannelLevel", "ChannelSummary", "MeterBallistics", "SoundfieldVector", "to_dbfs",
             "kFloorDb", "kFullScale", "meter_fraction")
C6_METER_MEMBERS = {
    "LevelMeter": ("process", "process_interleaved", "levels", "summary", "channel_count",
                   "sample_rate", "reset", "advance", "~LevelMeter", "operator="),
    "LoudnessMeter": ("push", "integrated_lkfs", "momentary_lkfs", "short_term_lkfs",
                      "loudness_range", "true_peak_dbtp", "channel_count", "push_block",
                      "push_true_peak", "~LoudnessMeter", "operator="),
}

# The names a stage moves to another namespace, as the exports spell them: the old namespace and,
# for each name declared in it, the new one (consol_text.py's tables).
def renamed_namespace(stage: str, name: str, unit: str = "") -> str:
    """`name`, a symbol or a line of IR as the old tree spelled it, as the stage spells it. A name
    no table holds (one in an anonymous namespace) goes where the unit it is defined in went."""
    if stage == "c3":
        return re.sub(
            rf"\biclforge::({'|'.join(CONTAINERS)})::", r"iclforge::containers::\1::", name
        )
    if stage == "c6":
        return c6_renamed(name)
    if stage == "c5":
        # AC-4's kernels and the tables that moved with them are dsp's; a mangled name no demangler
        # reads (a local lambda) has its components rewritten, and its substitutions read alike by
        # the comparison's "names no demangler reads"
        moved = (
            "kQwin|kQwinQ30|kAspxNoise|kAspxNoiseQ24|kCosQuadrant|kFftRoots\\d+|kPreTwiddle\\d+"
            "|kKbdLeft\\d+"
        )
        name = re.sub(r"\biclforge::ac4::detail::dsp::", "iclforge::dsp::tiered::", name)
        name = re.sub(rf"\biclforge::ac4::detail::tables::({moved})\b",
                      r"iclforge::dsp::tiered::tables::\1", name)
        return name.replace("8iclforge3ac46detail3dsp", "8iclforge3dsp6tiered")
    if stage == "c4":
        # the trace and the speakers are base's, iclforge::ac4's names for them aliases
        return re.sub(r"\biclforge::ac4::(SyntaxRecord|SyntaxSink|SyntaxTrace|Speaker)\b",
                      r"iclforge::base::\1", name)
    if stage != "c2":
        return name
    import consol_text

    name = re.sub(r"\biclforge::admbridge::", "iclforge::adm::", name)
    # a mangled name no demangler reads (a requires-clause, a local lambda): admbridge -> adm keeps
    # the namespace's depth, so the substitutions after it are unchanged
    name = name.replace("8iclforge9admbridge", "8iclforge3adm")
    unit_namespace = (
        "iclforge::base::crypto::" if unit.startswith("src/base/") else "iclforge::ac3::signing::"
    )

    def where(m: re.Match[str]) -> str:
        n = m.group(1)
        if n in consol_text.C2_CRYPTO:
            return "iclforge::base::crypto::" + n
        if n in consol_text.C2_SIGNER:
            return "iclforge::ac3::signing::" + n
        return unit_namespace + n

    return re.sub(r"\biclforge::signing::(\w+|\(anonymous namespace\))", where, name)


def c6_renamed(name: str) -> str:
    name = re.sub(rf"\biclforge::ac3::io::({'|'.join(C6_WAV)})\b", r"iclforge::base::\1", name)
    name = name.replace("iclforge::ac3::io::describe(iclforge::base::WavError)",
                        "iclforge::base::describe(iclforge::base::WavError)")
    name = re.sub(rf"\biclforge::ac3::analysis::({'|'.join(C6_LEVELS)})\b", r"iclforge::base::\1",
                  name)
    for cls, ns in (("LevelMeter", "analysis"), ("LoudnessMeter", "meta")):
        members = "|".join(re.escape(m) for m in C6_METER_MEMBERS[cls])
        name = re.sub(rf"\biclforge::ac3::{ns}::{cls}::({members})(?=\()",
                      rf"iclforge::base::{cls}::\1", name)
        # the move constructor and assignment take the class they belong to, now base's
        name = re.sub(rf"(iclforge::base::{cls}::(?:operator=)\()iclforge::ac3::{ns}::{cls}&&",
                      rf"\1iclforge::base::{cls}&&", name)
        name = name.replace(f"iclforge::ac3::{ns}::{cls}::{cls}(iclforge::ac3::{ns}::{cls}&&)",
                            f"iclforge::base::{cls}::{cls}(iclforge::base::{cls}&&)")
        name = name.replace(f"iclforge::ac3::{ns}::{cls}::Impl", f"iclforge::base::{cls}::Impl")
    name = re.sub(r"\biclforge::ac3::(version_details)\b", r"iclforge::base::\1", name)
    # objects' namespaces nest under iclforge::objects (decision 19); a mangled name no demangler
    # reads has its components rewritten, its substitutions read alike by the comparison
    name = re.sub(r"\biclforge::(oba|emdf)::", r"iclforge::objects::\1::", name)
    return name.replace("8iclforge3oba", "8iclforge7objects3oba").replace(
        "8iclforge4emdf", "8iclforge7objects4emdf")


def moves(stage: str, files: list[str]) -> dict[str, str]:
    """Old path -> new path for every tracked file the stage moves."""
    fn = STAGES[stage]
    out = {}
    for f in files:
        new = fn(f)
        if new and new != f:
            out[f] = new
    return out
