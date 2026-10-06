"""Stage C1 of planning/consolidation.md, first part: cut AC-4's three public headers and the
inspector's unit by what each declaration is, in today's layout (decision 7(a): the include
spellings change in C1 anyway, so a consumer changes once).

    ac4_cuts.py [--root <worktree>] [--dry-run]

  iclforge/ac4/ac4.hpp        -> iclforge/ac4/toc.hpp         Error, the table of contents and the
                                                              presentation types, parse_raw_frame()
                                 iclforge/ac4/elementary.hpp  SyncFrame, scan(), SyncFrameSplitter
                                 iclforge/ac4/carriage.hpp    dac4, the timing, the codec string,
                                                              the manifests
  iclforge/ac4dec/decoder.hpp -> decoder.hpp                  DecodeError's describe() aside,
                                                              the Decoder
                                 config.hpp                   what decode() is configured by
                                 frame.hpp                    what decode() returns
                                 presentation.hpp             what the decoder reports of a
                                                              presentation
  iclforge/ac4enc/encoder.hpp -> encoder.hpp                  the Encoder, sync_frame()
                                 config.hpp                   EncoderConfig and its parts
  src/ac4/src/ac4.cpp         -> toc.cpp, elementary.cpp, carriage.cpp

A header is read as its include block, its file comment and the top-level declarations of its one
namespace, each with the comments above it, and every declaration goes to the header the table
names (PIECES; a describe() overload by its argument). Nothing in a declaration is edited. Each new
header includes the standard headers its declarations name, the export header where it exports,
and the other headers whose names it uses (comments are not read for that), so that it compiles
alone. A consumer that included ac4.hpp includes the headers whose names it uses instead (the table
of contents' where it uses none); a consumer of the decoder's or the encoder's header keeps it, as
that header includes the others, and gains the inspector's headers it used through it.

Idempotent: a split whose first new file exists is skipped. Loud: a declaration the table does not
place, or a marker of the unit's cut that is not found, stops the run. The pages are not touched
(the header map and the library pages say where a declaration lives, which a person words).
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from cuts import Source, include_line, new_file, quoted_run, sorted_position
from n1b_lib import CPP_EXT, Repo, base_parser

AC4_INC = "src/ac4/include/iclforge/ac4"
DEC_INC = "src/ac4dec/include/iclforge/ac4dec"
ENC_INC = "src/ac4enc/include/iclforge/ac4enc"


def fail(message: str) -> None:
    sys.exit(f"ac4_cuts: {message}")


# --- what goes where ------------------------------------------------------------------------------


@dataclass
class Split:
    source: str  # the header that is cut, its repository path
    spelling: str  # how it is included
    main: str  # the piece that keeps the file's name and its file comment
    pieces: dict[str, str]  # piece name -> new header's spelling
    comments: dict[str, str]  # piece name -> the file comment a new header opens with
    place: dict[str, str] = field(default_factory=dict)  # declaration -> piece
    # whether the header that keeps the file's name includes the rest of its split, so that a
    # consumer that included it still has every declaration it had (the decoder's and the
    # encoder's; the inspector's header is gone, and its consumers include what they use)
    umbrella: bool = True


INSPECTOR = Split(
    source=f"{AC4_INC}/ac4.hpp",
    spelling="iclforge/ac4/ac4.hpp",
    main="toc",
    umbrella=False,
    pieces={
        "toc": "iclforge/ac4/toc.hpp",
        "elementary": "iclforge/ac4/elementary.hpp",
        "carriage": "iclforge/ac4/carriage.hpp",
    },
    comments={
        "elementary": (
            "// AC-4 in an elementary stream: Annex G.3.1's ac4_syncframe() and the walk\n"
            "// over them, whole (scan()) or as a stream arrives (SyncFrameSplitter). A sync\n"
            "// frame's raw_ac4_frame is what parse_raw_frame() (iclforge/ac4/toc.hpp) reads."
        ),
        "carriage": (
            "// AC-4 in a container or a manifest: what a muxer, a segmenter and a playlist\n"
            "// writer need of a stream, read off its parsed table of contents\n"
            "// (iclforge/ac4/toc.hpp), so that each stays codec-blind."
        ),
    },
    place={
        "Error": "toc",
        "describe(Error)": "toc",
        "SyncFrame": "elementary",
        "ScanResult": "elementary",
        "scan": "elementary",
        "kSplitterRecommendedBuffer": "elementary",
        "SyncFrameSplitter": "elementary",
        **{
            n: "toc"
            for n in (
                "ContentType OriginalContent ChannelSubstreamInfo ObjectKind ObjectEntry "
                "ObjectProperties OamdSubstreamInfo GainTool StereoDmxCoeff BedRenderInfo "
                "TrimConfig Trim Headphone OamdCommonData AjocSubstreamInfo ObjSubstreamInfo "
                "GroupSubstream SubstreamGroupInfo PresentationInfoV0 EmdfVersionKey "
                "AlternativeTarget AlternativeInfo PresentationInfoV1 Toc Substream RawFrame "
                "parse_raw_frame"
            ).split()
        },
        **{
            n: "carriage"
            for n in (
                "build_dac4 dac4_refusal cmaf_refusal samples_per_frame MediaTiming media_timing "
                "FrameRate frame_rate rfc6381_codec_string signalled_presentation "
                "ManifestDescriptor dash_channel_configuration dash_supplemental_properties "
                "presentation_channel_count configuration_difference"
            ).split()
        },
    },
)

DECODER = Split(
    source=f"{DEC_INC}/decoder.hpp",
    spelling="iclforge/ac4dec/decoder.hpp",
    main="decoder",
    pieces={
        "decoder": "iclforge/ac4dec/decoder.hpp",
        "config": "iclforge/ac4dec/config.hpp",
        "frame": "iclforge/ac4dec/frame.hpp",
        "presentation": "iclforge/ac4dec/presentation.hpp",
    },
    comments={
        "config": (
            "// What an AC-4 Decoder (iclforge/ac4dec/decoder.hpp) is configured by: the output\n"
            "// processing a system asks for, which presentation it decodes, what it does with a\n"
            "// frame that will not decode, and full or core decoding."
        ),
        "frame": (
            "// What an AC-4 Decoder (iclforge/ac4dec/decoder.hpp) returns for a frame: the\n"
            "// decoded channels and objects, the report of what the frame carries, the blocks a\n"
            "// streaming caller takes them in, and how a concealed frame was made."
        ),
        "presentation": (
            "// What an AC-4 Decoder (iclforge/ac4dec/decoder.hpp) reports of the presentation it\n"
            "// decodes: its members, and the loudness, DRC, dialogue enhancement and downmix\n"
            "// metadata the stream sends for it."
        ),
    },
    place={
        "DecodeError": "frame",
        "describe(DecodeError)": "frame",
        **{
            n: "config"
            for n in (
                "DownmixTarget describe(DownmixTarget) DrcMode describe(DrcMode) OutputConfig "
                "AssociatedType PresentationChoice select_presentation ConcealmentPolicy "
                "DecodingMode describe(DecodingMode) DecoderConfig"
            ).split()
        },
        **{
            n: "frame"
            for n in (
                "ConcealmentAction Concealment EmdfPayloadReport SubstreamReport FrameReport "
                "Speaker describe(Speaker) ObjectUpdate DecodedObject DecodedFrame kBlockSamples "
                "PcmBlock BlockSink FrameInfo"
            ).split()
        },
        **{
            n: "presentation"
            for n in (
                "SubstreamRole describe(SubstreamRole) PresentationMember PresentationInfo "
                "LoudnessInfo DrcModeInfo DrcInfo DialogueEnhancementInfo DownmixInfo "
                "PresentationMetadata"
            ).split()
        },
        "Decoder": "decoder",
    },
)

ENCODER = Split(
    source=f"{ENC_INC}/encoder.hpp",
    spelling="iclforge/ac4enc/encoder.hpp",
    main="encoder",
    pieces={
        "encoder": "iclforge/ac4enc/encoder.hpp",
        "config": "iclforge/ac4enc/config.hpp",
    },
    comments={
        "config": (
            "// What an AC-4 Encoder (iclforge/ac4enc/encoder.hpp) is configured by: the codec\n"
            "// and rate modes, the loudness, DRC, downmix and dialogue metadata it sends, the\n"
            "// objects, and the substreams and presentations it builds of them."
        ),
    },
    place={
        "EncodeError": "encoder",
        "describe(EncodeError)": "encoder",
        "EncodedFrame": "encoder",
        "Encoder": "encoder",
        "sync_frame": "encoder",
    },
)
# Everything else the encoder's header declares is its configuration.
ENCODER_DEFAULT = "config"

SPLITS = (INSPECTOR, DECODER, ENCODER)

# The headers that are not cut and that the pieces may need: name -> spelling.
FIXED = {"iclforge/ac4/syntax.hpp": ("SyntaxRecord", "SyntaxTrace", "SyntaxSink", "sink_of")}

STD = {
    "array": (r"std::array\b",),
    "concepts": (r"std::(same_as|convertible_to|invocable|derived_from|integral|floating_point)\b",),
    "cstddef": (r"std::(size_t|byte|ptrdiff_t|nullptr_t)\b",),
    "cstdint": (r"std::u?int(8|16|32|64)_t\b",),
    "expected": (r"std::(expected|unexpected)\b",),
    "functional": (r"std::function\b",),
    "memory": (r"std::(unique_ptr|shared_ptr|make_unique)\b",),
    "optional": (r"std::(optional|nullopt)\b",),
    "span": (r"std::span\b",),
    "string": (r"std::string\b(?!_view)",),
    "string_view": (r"std::string_view\b",),
    "type_traits": (r"std::(is_\w+|remove_\w+|decay_t|enable_if)\b",),
    "utility": (r"std::(pair|move|forward|exchange)\b",),
    "vector": (r"std::vector\b",),
}


# --- reading a header -----------------------------------------------------------------------------


@dataclass
class Decl:
    key: str
    text: str


@dataclass
class Header:
    system: list[str]
    project: list[str]
    comment: str
    namespace: str
    decls: list[Decl]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)


def depth_change(line: str) -> tuple[int, int]:
    code = strip_comments(line)
    return code.count("{") - code.count("}"), code.count("(") - code.count(")")


def decl_key(code: str) -> str:
    """The name a declaration declares, from its code: `struct X`, `X(` for a function."""
    flat = " ".join(code.split())
    for rx in (
        r"^enum class (\w+)",
        r"^(?:struct|class) (?:ICLFORGE_\w+_EXPORT )?(\w+)",
        r"^using (\w+) =",
        r"^inline constexpr [\w:<>, ]+? (\w+) =",
    ):
        m = re.match(rx, flat)
        if m:
            return m.group(1)
    m = re.match(r"^(?:template <[^>]*> )?(?:\[\[\w+\]\] )?[^(]*?\b(\w+)\(([^)]*)\)", flat)
    if not m:
        fail(f"cannot name the declaration {flat[:80]!r}")
    name, params = m.group(1), m.group(2)
    if name == "describe":
        return f"describe({params.split()[0]})"
    return name


def read_header(path: Path) -> Header:
    text = path.read_bytes().decode("utf-8").replace("\r\n", "\n")
    lines = text.split("\n")
    ns_at = next(i for i, ln in enumerate(lines) if re.match(r"^namespace [\w:]+ \{$", ln))
    namespace = lines[ns_at].split()[1]
    end_at = max(i for i, ln in enumerate(lines) if ln.startswith(f"}}  // namespace {namespace}"))
    system = [m.group(1) for ln in lines[:ns_at] if (m := re.match(r"#include <([^>]+)>", ln))]
    project = [m.group(1) for ln in lines[:ns_at] if (m := re.match(r'#include "([^"]+)"', ln))]
    last_include = max(i for i, ln in enumerate(lines[:ns_at]) if ln.startswith("#include"))
    comment = "\n".join(lines[last_include + 1 : ns_at]).strip("\n")
    decls: list[Decl] = []
    pending: list[str] = []
    current: list[str] = []
    braces = parens = 0
    for ln in lines[ns_at + 1 : end_at]:
        if not current:
            stripped = ln.strip()
            if not stripped or stripped.startswith("//"):
                pending.append(ln)
                continue
            if stripped.startswith("template"):
                current.append(ln)
                continue
        current.append(ln)
        b, p = depth_change(ln)
        braces += b
        parens += p
        code = strip_comments(ln).rstrip()
        if braces == 0 and parens == 0 and (code.endswith(";") or code.endswith("}")):
            if code.endswith("}") and not code.endswith("};"):
                # a function body: done at its closing brace
                pass
            head = "\n".join(c for c in current)
            key = decl_key(strip_comments(head))
            body = "\n".join(pending + current).strip("\n")
            decls.append(Decl(key, body))
            pending, current = [], []
    if current:
        fail(f"{path}: an unfinished declaration at the end of the namespace")
    return Header(system, project, comment, namespace, decls)


# --- writing the pieces ---------------------------------------------------------------------------


def names_used(code: str) -> set[str]:
    return set(re.findall(r"\b[A-Za-z_]\w*\b", strip_comments(code)))


def declared(decls: list[Decl]) -> set[str]:
    """The names a piece provides. An overload (`describe(Error)`) is not one: each piece declares
    the overloads of its own types, and a call of one names no other piece's declaration."""
    return {d.key for d in decls if "(" not in d.key}


def std_headers(code: str) -> list[str]:
    bare = strip_comments(code)
    return sorted(h for h, pats in STD.items() if any(re.search(p, bare) for p in pats))


def plan_split(split: Split, header: Header, default: str | None) -> dict[str, list[Decl]]:
    out: dict[str, list[Decl]] = {p: [] for p in split.pieces}
    for d in header.decls:
        piece = split.place.get(d.key, default)
        if piece is None:
            fail(f"{split.source}: the table does not place {d.key!r}")
        out[piece].append(d)
    return out


def write_pieces(
    root: Path, split: Split, header: Header, plan: dict[str, list[Decl]], others: dict[str, set]
) -> dict[str, set[str]]:
    """Write every piece; return each piece's spelling -> the names it declares."""
    inc_dir = Path(split.source).parent
    export = next(h for h in header.project if h.endswith("/export.hpp"))
    like = Source(root, split.source)
    names = {split.pieces[p]: declared(ds) for p, ds in plan.items()}
    universe = {**others, **names}
    for piece, decls in plan.items():
        body = "\n\n".join(d.text for d in decls)
        used = names_used(body)
        project = []
        for spelling, provided in universe.items():
            if spelling != split.pieces[piece] and used & provided:
                project.append(spelling)
        # the header that keeps the file's name includes the rest of its split, so that a consumer
        # that included it still has every declaration it had
        if piece == split.main and split.umbrella:
            project += [sp for p, sp in split.pieces.items() if p != piece]
        if re.search(r"\bICLFORGE_\w+_EXPORT\b", strip_comments(body)):
            project.append(export)
        comment = header.comment if piece == split.main else split.comments[piece]
        out = ["#pragma once", ""]
        system = std_headers(body)
        if system:
            out += [f"#include <{h}>" for h in system] + [""]
        if project:
            out += [include_line(h) for h in sorted(set(project))] + [""]
        out += [comment, "", f"namespace {header.namespace} {{", "", body, ""]
        out += [f"}}  // namespace {header.namespace}", ""]
        rel = (inc_dir / Path(split.pieces[piece]).name).as_posix()
        new_file(root, rel, "\n".join(out), like)
    return names


def includes_of(text: str) -> list[str]:
    return re.findall(r'^#include "([^"]+)"', text, flags=re.M)


def closure(start: str, edges: dict[str, list[str]]) -> set[str]:
    seen, todo = set(), [start]
    while todo:
        s = todo.pop()
        if s in seen:
            continue
        seen.add(s)
        todo.extend(edges.get(s, ()))
    return seen


def replace_include(text: str, old: str, news: list[str]) -> str:
    lines = text.split("\n")
    old_line = include_line(old)
    index = lines.index(old_line)
    first, last = quoted_run(lines, index)
    del lines[index]
    last -= 1
    for spelling in news:
        line = include_line(spelling)
        if line in lines:
            continue
        pos = sorted_position(lines, first, last, line)
        lines.insert(pos, line)
        last += 1
    return "\n".join(lines)


def add_includes(text: str, anchor: str, news: list[str]) -> str:
    lines = text.split("\n")
    index = lines.index(include_line(anchor))
    first, last = quoted_run(lines, index)
    for spelling in news:
        line = include_line(spelling)
        if line in lines:
            continue
        lines.insert(sorted_position(lines, first, last, line), line)
        last += 1
    return "\n".join(lines)


# --- the inspector's unit -------------------------------------------------------------------------

# Where ac4.cpp is cut, by the code that opens each part: the scanner from Annex G's banner to the
# anonymous namespace that opens the table of contents' parse, the carriage from its banner on. The
# rest, the table of contents' reader, stays in ac4.cpp, which C1's moves rename core/toc.cpp so that
# it keeps its history.
UNIT = "src/ac4/src/ac4.cpp"
UNIT_CUTS = (
    ("elementary", "// --- Annex G: AC-4 sync frame ---", "namespace {\n\n// --- §4.2.14.15 emdf_reserved"),
    ("carriage", "// --- Carriage (AC-4 bitstream inspector's separable slice) ---", None),
)


def cut_unit(root: Path) -> None:
    src = Source(root, UNIT)
    text = src.text
    a_head, a_end = UNIT_CUTS[0][1], UNIT_CUTS[0][2]
    c_head = UNIT_CUTS[1][1]
    a = text.find(a_head)
    b = text.find(a_end)
    c = text.find(c_head)
    end = text.rfind("}  // namespace iclforge::ac4")
    if min(a, b, c, end) < 0 or not a < b < c < end:
        fail("ac4.cpp: a marker of the cut is not where it was")
    # The scanner's part starts inside the reader's anonymous namespace (crc16 is in it): close it
    # before the cut and open one for the scanner.
    head = text[:a]
    elementary_body = "namespace {\n\n" + text[a:b].rstrip() + "\n"
    toc_text = head.rstrip() + "\n\n}  // namespace\n\n" + text[b:c].rstrip() + "\n\n" + text[end:]
    carriage_body = text[c:end].rstrip() + "\n"
    std_all = re.findall(r"^#include <([^>]+)>", text, flags=re.M)

    def unit(own: str, body: str) -> str:
        used = [h for h in std_all if any(re.search(p, strip_comments(body)) for p in STD.get(h, ()))]
        extra = [h for h in std_all if h not in STD and re.search(UNIT_STD_EXTRA[h], body)]
        system = sorted(set(used + extra))
        out = [include_line(own), ""]
        if system:
            out += [f"#include <{h}>" for h in system] + [""]
        out += ["namespace iclforge::ac4 {", "", body.rstrip("\n"), "", "}  // namespace iclforge::ac4", ""]
        return "\n".join(out)

    new_file(root, "src/ac4/src/elementary.cpp", unit("iclforge/ac4/elementary.hpp", elementary_body), src)
    new_file(root, "src/ac4/src/carriage.cpp", unit("iclforge/ac4/carriage.hpp", carriage_body), src)
    # The table of contents' unit keeps the file's history: it is ac4.cpp renamed, by the moves.
    toc = toc_text.replace('#include "iclforge/ac4/ac4.hpp"', '#include "iclforge/ac4/toc.hpp"', 1)
    src.text = toc
    src.save()


# What the standard headers the unit includes are used for, where STD does not say.
UNIT_STD_EXTRA = {
    "algorithm": r"std::(min|max|clamp|sort|find|any_of|all_of|none_of|count|copy|fill|ranges::)",
    "bit": r"std::(bit_cast|countl_zero|countr_zero|popcount|bit_width|has_single_bit)",
    "cstring": r"std::(memcpy|memcmp|memmove|memset)",
    "limits": r"std::numeric_limits",
    "numeric": r"std::(accumulate|gcd|lcm|iota|reduce)",
}


# --- consumers ------------------------------------------------------------------------------------


def rewrite_consumers(root: Path, pieces: dict[str, set[str]], edges: dict[str, list[str]]) -> int:
    repo = Repo(str(root))
    inspector_pieces = list(INSPECTOR.pieces.values())
    changed = 0
    for f in repo.files:
        if Path(f).suffix not in CPP_EXT or f.startswith("tools/n1b/") or not (root / f).exists():
            continue
        if f in {
            (Path(s.source).parent / Path(p).name).as_posix()
            for s in SPLITS
            for p in s.pieces.values()
        }:
            continue
        src = Source(root, f)
        text = src.text
        incs = includes_of(text)
        used = names_used(text)
        if INSPECTOR.spelling in incs:
            want = [s for s in inspector_pieces if used & pieces[s]] or [INSPECTOR.pieces["toc"]]
            text = replace_include(text, INSPECTOR.spelling, want)
        for split in (DECODER, ENCODER):
            if split.spelling in includes_of(text):
                reach = set()
                for inc in includes_of(text):
                    reach |= closure(inc, edges)
                missing = [s for s in inspector_pieces if used & pieces[s] and s not in reach]
                if missing:
                    text = add_includes(text, split.spelling, missing)
        if text != src.text:
            src.text = text
            src.save()
            changed += 1
    return changed


def run(root: Path, dry_run: bool) -> None:
    if (root / f"{AC4_INC}/toc.hpp").exists():
        print("ac4_cuts: the cut is made already")
        return
    headers = {s.source: read_header(root / s.source) for s in SPLITS}
    plans = {
        s.source: plan_split(s, headers[s.source], ENCODER_DEFAULT if s is ENCODER else None)
        for s in SPLITS
    }
    for s in SPLITS:
        print(s.source, {p: len(d) for p, d in plans[s.source].items()})
    if dry_run:
        return
    pieces: dict[str, set[str]] = {k: set(v) for k, v in FIXED.items()}
    for s in SPLITS:
        pieces.update(write_pieces(root, s, headers[s.source], plans[s.source], pieces))
    # the inspector's header is gone; its pieces replace it
    (root / INSPECTOR.source).unlink()
    edges = {}
    for s in SPLITS:
        for spelling in s.pieces.values():
            path = root / (Path(s.source).parent / Path(spelling).name)
            edges[spelling] = includes_of(path.read_text(encoding="utf-8"))
    cut_unit(root)
    n = rewrite_consumers(root, pieces, edges)
    print(f"ac4_cuts: wrote {sum(len(s.pieces) for s in SPLITS)} headers, 3 units; {n} consumers")


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    run(Path(a.root), a.dry_run)


if __name__ == "__main__":
    main()
