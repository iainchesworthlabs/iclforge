# One shape for every codec, and fewer libraries (a proposal)

!!! note "Status as of 2026-10-06: C0 and C1 run and proved; C2 and C3 wait on the user; decisions 1 to 5, 7 and 10 taken"
    Asked for by the user on 2026-10-06: "the AC3 codec and the AC4 codec structures are completely
    different. There's also duplication from inside codecs to common stuff", "the ac3 approach is the
    preferred approach", and "should there be 25 libraries? Is it worth consolidating some?". This page
    reads the tree as it stood on `main` that day. It follows [layout.md](layout.md) (N1B), which put the
    codecs side by side and left the duplicated DSP, the codec-blind vocabulary and the shape of AC-4 for
    later ([layout.md (j)](layout.md#j-what-stays-out-and-follow-on-ideas)). The user took decisions 1 to 5
    below on 2026-10-06, and 7 and 10 before C1; 6, 8 and 9 are open. C0 and C1 ran on the local
    branches `chore/src-consolidation-c0` and `-c1` and changed nothing a build outputs; C1's cut moves
    the instruction counts of the bare-metal AC-4 probe by up to 68 parts per million, which the user is
    asked about before C2 ([what the runs found](#what-the-runs-found-that-the-plan-did-not)); nothing
    is pushed.

## In brief

- **One shape for a codec, AC-3's.** A codec is one library, `src/<codec>/`, laid out by area
  (`core`, `decoder`, `encoder`, `io`, `meta`, `oba`, `signing`, `verify`), with a `minimal.cmake` that builds
  the decode-only or encode-only archive from a narrower list of the same sources and with `variants/`
  directories that carry the same names in every codec. AC-4's four libraries (`ac4`, `ac4core`,
  `ac4dec`, `ac4enc`) become one, `iclforge::ac4`.
- **What is not a codec's lives once.** Bit I/O, the syntax trace, CRC-16, the escaped-integer codings,
  the speaker vocabulary, profiling, the crypto primitives, the FFT, the QMF bank and the resampler go to
  the codec-blind libraries, and AC-4 links them as AC-3 does. The AC-4 rows of
  `tools/checks/layering.json` gain `base`, `dsp`, `objects` and `render`.
- **22 libraries become 12.** AC-4's four are one; `mp4`, `mpegts`, `matroska`, `iamf` and `iec61937` are
  one, `iclforge::containers`; `arithmetic` joins `base`; `admbridge` joins `adm`; `signing` is split, its
  primitives to `base` and its EMDF signer to `ac3`, so that AC-4's own signer arrives in `ac4` later. The
  ABI allowlists go from 16 to 9.
- **Stages, each proved.** C0 to C3 move and rename and change no output byte, with the proof N1B used
  (`tools/n1b/baseline.py`, `export_diff.py`, `abi_compare.py`, `ir_compare.py`). C4 removes the copies
  that do not touch arithmetic. C5, one FFT, one QMF bank and one resampler, moves the last bits of the
  `double` output and is scored again, as [decision 25 of ac4.md](ac4.md#decisions-of-2026-09-25) did.
  C6 takes the codec-blind code out of `ac3` and narrows the public headers.

There are 22 directories under `src/`, not 25; the count the question gave was an estimate.

## (a) What is there

The 22 libraries and the libraries each may include (`tools/checks/layering.json`):

| library | may include | what it is |
|---|---|---|
| `arithmetic` | | header-only: `Fixed32`, `MantExp`, the float functions, the SIMD seam |
| `base` | | bit I/O, the speaker vocabulary, the CPU probe, the profiling hooks |
| `dsp` | `arithmetic` | the 512-point DFT, the 64-band QMF bank of TS 103 420, the offline resampler, biquads |
| `objects` | `base`, `dsp` | the object model, OAMD of TS 103 420, the EMDF container |
| `render` | `base`, `objects` | layouts, routing, the bed and object renderer, the panner |
| `iec61937` | | burst packing and detection |
| `ac3` | `base`, `dsp`, `objects`, `render`, `arithmetic` | AC-3, E-AC-3 and Atmos in E-AC-3 |
| `ac4` | | the inspector: sync frames, TOC, presentations, carriage |
| `ac4core` | `arithmetic` | AC-4's tables and transforms, a hidden static library |
| `ac4dec`, `ac4enc` | `ac4`, `ac4core` | the AC-4 decoder and encoder |
| `mp4`, `mpegts`, `matroska`, `iamf`, `iab`, `adm` | | containers and object formats, codec-blind |
| `admbridge` | `adm`, `iab`, `objects` | maps ADM and IAB onto the object model |
| `signing` | `ac3` | SHA-256, HMAC, the runtime key, the EMDF Atmos signer |
| `audio` | `base`, `render`, `iec61937`, `objects` | platform audio backends; not installed |
| `sendspin` | | the Sendspin protocol for Hearth; not installed |
| `capi` | `ac3`, `ac4`, `ac4dec`, `ac4enc` | the C11 surface |

**The two codecs.** `src/ac3` is one target (`iclforge_add_library(ac3 ...)`) of about 123 files: 50
public headers under `include/iclforge/ac3/{analysis,core,decoder,emdf,encoder,io,meta,oba,quality,verify}/`,
the sources under `src/` in the same areas, and seven variant headers selected by directory
(`decode-scalar-{float64,float32,fixed32}`, `encode-scalar-{float64,float32}`, `profile-{full,minimal}`).
`minimal.cmake` builds `iclforge::ac3_minimal`, decode-only or encode-only, from a list of the library's
sources and of `base`, `dsp`, `objects` and `render`'s.

AC-4 is four libraries of 186 files: `ac4` (4), `ac4core` (59), `ac4dec` (72), `ac4enc` (51). Their public
types share one namespace, `iclforge::ac4`, under three header roots (`iclforge/ac4/ac4.hpp`,
`iclforge/ac4dec/decoder.hpp`, `iclforge/ac4enc/encoder.hpp`, the last two of about 820 and 830 lines);
`ac4core`'s 34 headers are under `include/` but are not installed, and the decoder and the encoder compile
against them through `$<COMPILE_ONLY:iclforge::ac4core>`. The decoder is laid out by direction
(`src/syntax/` reads bits, `src/pcm/` reconstructs), the encoder by tool (`asf/`, `aspx/`, `acpl/`,
`ajcc/`, `ajoc/`, `oamd/`, `frame/`), and the core by kind (`dsp/`, `tables/`, a directory per tool's
shared kernel). The decoder does not read the TOC again: it calls `iclforge::ac4::parse_raw_frame()`
(`src/ac4dec/src/decoder.cpp`, `Decoder::Impl::collect`).

**Why AC-4 is shaped so.** [Decision 15 of ac4.md](ac4.md#decisions-for-the-encoder-and-the-applications)
weighed "(c) one AC-4 library holding both directions, as `ac3::forge` holds AC-3's and E-AC-3's" and
took the split, because (c) "puts the encoder into every decoder-only build unless an option splits it
again". [Decision 7](ac4.md#decisions) took "the decoder carries its own" DSP, because Forge's FFT took
powers of two only and its QMF bank had another window and phase. Since then AC-3's minimal profile has
become the option that splits a build again (`ICLFORGE_MINIMAL_DECODER`, `ICLFORGE_MINIMAL_ENCODER`, one
`if` at the top of `src/ac3/CMakeLists.txt`), and [decision 25](ac4.md#decisions-of-2026-09-25) and
[decision 31](ac4.md#decisions-of-2026-09-25) already brought AC-4 to AC-3's three scalars and its
arithmetic.

## (b) What is wrong with it

1. **Two shapes for one kind of thing.** A reader who knows where AC-3's DRC, its bit allocation or its
   object syntax are does not find AC-4's by the same path, and the next codec (the TrueHD branch,
   `src/truehd`) has two patterns to choose from.
2. **Copies of what is not a codec's.** AC-4 may not include `base`, `dsp`, `objects` or `render`, so it
   carries its own; the containers carry theirs for the same reason:
   - *Bit readers and writers*: seven. `base` (`bitreader.hpp`, `bitwriter.hpp`), the inspector
     (`src/ac4/src/ac4.cpp`, `Reader`), the decoder (`src/ac4dec/src/bit_reader.hpp`), the encoder
     (`src/ac4enc/src/bit_writer.hpp`), `iab` (`src/iab/src/bitreader.hpp`, `bitwriter.hpp`), `mp4`
     (`BitCursor` in `src/mp4/src/reader.cpp`) and `iec61937` (`TocBits` in `src/iec61937/src/iec61937.cpp`).
     What the decoder's and the encoder's add is the syntax trace (`SyntaxRecord`, `SyntaxSink`,
     `src/ac4/include/iclforge/ac4/syntax.hpp`), which needs no codec.
   - *`variable_bits` and its kin*: seven loops, in `objects` (`emdf.cpp`, `oamd.cpp`), the inspector, the
     decoder's reader, the encoder's writer, `iec61937` and `iab` (`put_plex`). They are two codings, not
     seven: EMDF's (TS 102 366 Annex H) and AC-4's (TS 103 190-1, 4.2.2), which differ in the offset
     between groups.
   - *CRC-16, polynomial 0x8005*: `src/ac3/include/iclforge/ac3/core/crc16.hpp`, `src/ac4/src/ac4.cpp`
     and `src/signing/src/emdf_atmos_signer.cpp`.
   - *Profiling*: `src/ac4core/variants/profiling-*` re-declares `base`'s `zone_enter` and `zone_leave`.
   - *The scalar option*: `ICLFORGE_DECODE_SCALAR` is declared in `src/ac3/CMakeLists.txt` and again in
     `src/ac4core/CMakeLists.txt`, with directories spelled `float64` in one and `double` in the other.
   - *Speakers*: `iclforge::ac4::Speaker` beside `iclforge::base::Location`.
   - *The ISO-BMFF writer*: `put_u64`, `put_box`, `put_fullbox` in both `src/mp4/src/isobmff_detail.hpp`
     and `src/iamf/src/isobmff_detail.hpp`.
   - *AC-4's DRC curve and its K-weighting*: written twice, nearly line for line, in
     `src/ac4dec/src/pcm/drc.cpp` and `src/ac4enc/src/frame/drc_gains.cpp`.
   - *Transforms*: `src/ac4core/src/dsp/{fft,mdct,qmf,resampler,kbd,synthesis}.cpp` beside
     `src/dsp/src/{fft,qmf,resampler}.cpp` and `src/ac3/src/core/mdct.cpp`. These are not copies: the QMF
     prototypes differ (TS 103 420's clean-room window, AC-4's `QWIN`), AC-4's FFT is mixed-radix
     (2^a · 3^b · 5^c) where `dsp`'s is 512 points, and the resamplers do different jobs. One
     implementation of each takes a parameter and a numerical proof (C5).
3. **What is the same domain and is not a copy.** OAMD is two standards (TS 103 420 for E-AC-3, TS 103 190-2
   6.2.8 for AC-4), and so are the DRC wire formats and the normative downmix and rendering tables (AC-3
   7.8, AC-4 Part 2 5.10.2). Those stay in their codec. What they can share is the geometry (a position, a
   gain curve) and the renderer that is not normative.
4. **Twelve libraries write their CMake by hand.** `base`, `dsp`, `objects`, `render`, `iec61937` and `ac3`
   use `iclforge_add_library()` (`cmake/IclforgeLibrary.cmake`); `ac4`, `ac4dec`, `ac4enc`, `mp4`,
   `mpegts`, `matroska`, `iamf`, `iab`, `adm`, `admbridge`, `signing` and `capi` repeat the OBJECT, static
   and shared targets, the aliases and the export header.
5. **Small libraries that are one thing.** `iec61937` is two files; `mp4`, `mpegts`, `matroska` and `iamf`
   are 5 to 15 files each, link nothing, and share helpers they cannot share. `admbridge` is opt-in
   behind Boost with `adm` and is never built without it. `arithmetic` exists apart from `base` so that
   `ac4core` could link it without `base`.
6. **The public surfaces disagree.** `ac3` installs its tables (`core/tables.hpp`, `bitalloc_tables.hpp`,
   `aht_tables.hpp`) as public headers; AC-4 installs two headers of about 820 and 830 lines. `ac3` holds the
   family's build identity (`src/ac3/src/version.cpp`) and three codec-blind areas that take AC-3's
   `Acmod` and `SampleRate` (`io/wav*`, `meta/loudness`, `analysis/levels`;
   [layout.md decision 5](layout.md#i-decisions)). `objects` declares into `iclforge::oba` and
   `iclforge::emdf` rather than `iclforge::objects`.

## (c) Principles

1. A codec is one library, `src/<codec>/`, target `iclforge::<codec>`, namespace `iclforge::<codec>`.
2. Inside it, the areas are AC-3's: `core` (syntax both directions read and write, tables, kernels),
   `decoder`, `encoder`, `io` (framing, scanning, carriage), `meta` (metadata semantics both directions
   use), `oba` (objects), `signing`, `verify` (traces and self-checks), `quality`. A codec has the areas it
   needs and no others.
3. A decode-only or encode-only build is a source list in `minimal.cmake`, not a library. The archive is
   static, so a full build that links one direction pays for the other only in its shared library, as AC-3
   does today.
4. The reader and the writer of a codec's syntax stay separate files in `decoder/` and `encoder/`. Merging
   the libraries does not merge the transcriptions that [ac4.md's validation](ac4.md#where-the-libraries-live)
   rests on.
5. What needs no codec is in `base`, `dsp`, `objects` or `render`, once, and every codec links it.
6. `variants/<axis>-<choice>/` has the same axes and spellings in every codec.
7. Every library is made by `iclforge_add_library()`.
8. A library exists because something builds or installs it alone: an option, a third-party dependency,
   an ABI, a consumer that must not link the rest. A directory of files is not a reason.

## (d) The target

### The libraries

| library | absorbs | may include | why it is alone |
|---|---|---|---|
| `base` | `arithmetic`; `signing`'s key, SHA-256 and HMAC | | the leaf every library links |
| `dsp` | `ac4core`'s FFT, MDCT, QMF, resampler, KBD (C5) | `base` | signal processing no codec owns |
| `objects` | | `base`, `dsp` | the object model the renderer and both codecs share |
| `render` | | `base`, `objects` | linked by `audio` and the programs without a codec |
| `ac3` | `signing`'s EMDF signer | `base`, `dsp`, `objects`, `render` | a codec |
| `ac4` | `ac4`, `ac4core`, `ac4dec`, `ac4enc` | `base`, `dsp`, `objects`, `render` | a codec |
| `containers` | `mp4`, `mpegts`, `matroska`, `iamf`, `iec61937` | `base` | codec-blind carriage |
| `iab` | | `base` | an object format, used by `adm` and the CLI alone |
| `adm` | `admbridge` | `iab`, `objects` | opt-in, Boost and libadm |
| `capi` | | `ac3`, `ac4` | the C ABI |
| `audio` | | `base`, `render`, `containers`, `objects` | platform backends; not installed |
| `sendspin` | | | Hearth's protocol, mbedTLS, FLAC, Opus; not installed |

`ac3` links `iec61937` today (`DEPENDS ... iec61937` in `src/ac3/CMakeLists.txt`) and includes none of it;
the link goes, so that `ac3` does not come to link `containers`.

Considered and not proposed: `base`, `dsp`, `objects` and `render` as one library. They are the
codec-blind layer that `audio`, Hearth and Crucible link without a codec, and the layering check between
them is what keeps a codec's vocabulary out of it. Moving `audio` and `sendspin` out of `src/` (they are
not installed) is tidiness and changes no count; it is left.

### AC-4, laid out as AC-3

At the end of C4 (C1 builds the first four areas; `meta` and `oba` are filled in C4, `signing` when AC-4's
signer is built):

```text
src/ac4/
  CMakeLists.txt        iclforge_add_library(ac4 ... DEPENDS base dsp objects render)
  minimal.cmake         the decode-only archive (ICLFORGE_MINIMAL_AC4 today)
  ERRATA.md             the decoder's and the encoder's, as two sections
  include/iclforge/ac4/
    core/               toc.hpp (Toc, the presentation types, parse_raw_frame), syntax.hpp
    io/                 elementary.hpp (scan, SyncFrameSplitter), carriage.hpp (build_dac4, rfc6381)
    decoder/            decoder.hpp, config.hpp, frame.hpp, presentation.hpp
    encoder/            encoder.hpp, config.hpp
  src/
    core/               the TOC reader, tables/, huffman_codebook.hpp, aspx/ acpl/ ajcc/ ajoc/ kernels
    io/                 the scanner and the splitter, dac4 and the codec string
    decoder/            decoder.cpp, presentations.cpp, huffman.cpp, syntax/, pcm/
    encoder/            encoder.cpp, asf/, aspx/, acpl/, ajcc/, ajoc/, oamd/, frame/
    meta/               the DRC curve both directions use, dialogue enhancement's gains, loudness
    oba/                object metadata's semantics and the object and ISF rendering
    signing/            (when AC-4's signer is built)
    verify/             the encoder's re-parse of what it wrote (encoder.cpp's self-check)
  variants/
    decode-scalar-{float64,float32,fixed32}/iclforge/ac4/detail/real.hpp
    profile-{full,minimal}/iclforge/ac4/detail/profile.hpp
```

The core's headers are private (`src/core/`), as `ac4core`'s are not installed today; AC-3's tables
follow in C6. `src/ac4/src/core/dsp/` exists from C1 to C5 and is empty after C5. The encoder has no
minimum-footprint archive, as it has none today ([decision 34](ac4.md#decisions-of-2026-09-25)).

### Signing, a part of each codec

What is signed and how is the codec's: AC-3's EMDF Atmos signer walks an E-AC-3 frame's EMDF layout
(`iclforge/ac3/emdf/frame_layout.hpp`) and computes an HMAC over its authenticated region; AC-4's, when
it is built, signs what TS 103 190 carries, by its own algorithm. What the two share has no codec: the
operator's key, loaded at run time and zeroed on destruction (`SigningKey`, `load_signing_key`), and the
hash and MAC (SHA-256 from FIPS 180-4, HMAC from RFC 2104). So:

- `src/base/include/iclforge/base/crypto/{signing_key,sha256,hmac_sha256}.hpp`, namespace
  `iclforge::base::crypto`, from `src/signing/include/iclforge/signing/signing_key.hpp` and
  `src/signing/src/{sha256,hmac_sha256}.hpp`.
- `src/ac3/include/iclforge/ac3/signing/emdf_atmos_signer.hpp`, namespace `iclforge::ac3::signing`, from
  `src/signing/include/iclforge/signing/emdf_atmos_signer.hpp`.
- `src/ac4/include/iclforge/ac4/signing/` when AC-4's signer is built, linking the same primitives.

`src/signing/CMakeLists.txt` kept the signer out of `ac3` "so the codec still links nothing to
encode/decode". That holds: no encoder or decoder source calls the signer, `minimal.cmake` lists none of
its files, and the crypto is in `base`, which has no dependency. What changes is that
`libiclforge_ac3.so` carries the signer's code.

## (e) Stages

Each stage is one pull request, or a few, on a freeze of `main` for the files it moves, in the manner of
N1B ([tools/n1b/README.md](../tools/n1b/README.md)): the moves in a commit of their own (`git mv` alone,
every rename `R100`), then the include spellings, then the build files, then what is done by hand. The
scripts of N1B are the starting point: `layoutdef.py` as the plan, `n1b_apply.py`, `n1b_cmake.py` and
`n1b_paths.py` for the moves, the includes, the CMake and the paths in text, and `baseline.py` for the
proof. They live in `tools/n1b/` and are written for that layout; C1 to C3 need a plan of their own and
the passes pointed at it.

**C0, the build made uniform (no file moves).**
- `ac4`, `ac4dec`, `ac4enc`, `mp4`, `mpegts`, `matroska`, `iamf`, `iab`, `adm`, `admbridge`, `signing` and
  `capi` made by `iclforge_add_library()`, which gains what they need: no shared target in the minimal
  profile, a source list per option, `COMPILE_ONLY` dependencies.
- `ICLFORGE_DECODE_SCALAR` declared once, at the root, with one spelling of its directories
  (`float64`, `float32`, `fixed32`).
- `ac4core`'s profiling variants removed; its zone markers include `base`'s header (the functions are
  already the same).
- Proof: `baseline.py compare`, all four kinds identical.

**C1, AC-4 one library.**
- First, the cuts (as N1B's S1, in today's layout): `ac4.hpp` split into `toc.hpp`, `elementary.hpp` and
  `carriage.hpp`; `decoder.hpp` into four headers and `encoder.hpp` into two, by what each type is. Each
  cut is a header move and the tests prove it.
- Then the moves of [appendix A](#appendix-a-the-moves-of-c1). The namespace is `iclforge::ac4` already, so
  no symbol is renamed; the export macros `ICLFORGE_AC4DEC_EXPORT` and `ICLFORGE_AC4ENC_EXPORT` become
  `ICLFORGE_AC4_EXPORT`.
- `src/ac4/minimal.cmake` replaces the root's `ICLFORGE_MINIMAL_AC4` branch, which adds three
  subdirectories today.
- `layering.json`: one `ac4` row, `["base", "dsp", "objects", "render"]`; `capi` becomes
  `["ac3", "ac4"]`.
- What names a library: `cmake/InstallLibrary.cmake`, `cmake/iclforgeConfig.cmake.in`, `cmake/PkgConfig.cmake`,
  the vcpkg port and the Conan recipe, `tests/CMakeLists.txt`, `fuzz/CMakeLists.txt`, `examples/`, the
  programs' CMake, `python/`, `rust/iclforge-sys/build.rs`, `apps/wasm`, `apps/baremetal`, the ESP-IDF
  component (`CONFIG_ICLFORGE_AC4` builds the `ac4` minimal archive) and
  `tools/packaging/pack_esp_component.py`, `tools/checks/install_consumer/`, the coverage floors
  (`tools/checks/coverage_report.sh`), the change planner's path prefixes, `sonar-project.properties`.
- ABI: `libiclforge_ac4.so.txt` is the union of the three files it replaces (`export_diff.py --map` with a
  C1 map; `abi_compare.py`).
- Proof: hashes and the CLI corpus identical; every exported name of the three libraries exported by one;
  `check_layering.py`, `check_namespaces.py`, `check_pages.py`; the AC-4 probe's image on the Cortex-M3
  leg within noise; ESP-IDF pack `--verify`.

**C2, the small merges.**
- `arithmetic` into `base` ([appendix B](#appendix-b-the-moves-of-c2-and-c3)): its headers declare into
  `iclforge::internal` and `iclforge::arithmetic`, which `base` already shares (`iclforge::internal::cpu`),
  so the lock (`check_namespaces.py`) has one owner for them afterwards; the SIMD resolution moves from
  `src/arithmetic/CMakeLists.txt` to `src/base/CMakeLists.txt`; `STAGED_TREES` loses `src/arithmetic`.
- `admbridge` into `adm`: namespace `iclforge::admbridge` to `iclforge::adm`, headers `iclforge/admbridge/`
  to `iclforge/adm/`.
- `signing` split as (d) says; `fuzz_signing_verify`, `examples/object_signing.cpp`,
  `apps/crucible/engine/signing_hook.cpp` and `docs/concepts/object-signing.md` follow.
- Proof as C1, with the namespace rewrites passed to `export_diff.py` and `abi_compare.py`.

**C3, the containers.**
- The moves of [appendix B](#appendix-b-the-moves-of-c2-and-c3).
- `ICLFORGE_BUILD_MP4`, `ICLFORGE_BUILD_MPEGTS`, `ICLFORGE_BUILD_MATROSKA` and `ICLFORGE_BUILD_IAMF`
  keep their names and defaults, and select sources of the one library; the vcpkg features keep theirs.
- `ac3`'s link to `iec61937` removed; `audio`'s row names `containers`.
- Proof as C1.

**C4, one copy of each primitive, no arithmetic touched.**
- One bit reader and one writer in `base`, with the syntax trace as an optional sink: the trace types
  (`SyntaxRecord`, `SyntaxSink`, `SyntaxTrace`) move to `base`, `iclforge::ac4` keeps aliases, and a
  reader with no sink costs the branch it costs today. The inspector's, the decoder's, the encoder's,
  `iab`'s, `mp4`'s and `iec61937`'s readers and writers go.
- The two `variable_bits` codings in `base`, named for their standards.
- CRC-16 in `base`, with AC-3's GF(2) solver staying in `ac3`.
- `iclforge::ac4::Speaker` replaced by `iclforge::base::Location`.
- The ISO-BMFF box writer once in `containers`.
- AC-4's DRC curve and K-weighting once, in `src/ac4/src/meta/`; dialogue enhancement's shared values
  beside it; OAMD's semantics and object rendering to `src/ac4/src/oba/`.
- Speed: the base reader refills a 64-bit cache a byte at a time and AC-4's reloads 8 bytes every 4, so
  the merge request carries the speed and heap comparison the merge queue runs, and the ESP32-P4 AC-4
  timings (docs/platforms/bare-metal/esp32-p4.md) are taken again before and after.
- Proof as C1; any change in the hashes stops the stage.

**C5, one FFT, one QMF bank, one resampler (output moves).**
- One kernel at a time, each its own pull request: the mixed-radix FFT in `dsp`, with `dft512` its
  512-point case; the QMF bank with its prototype window as a parameter (TS 103 420's for JOC, `QWIN` for
  AC-4), using the kernels decision 25 made for every tier; the resampler's design and polyphase engine
  once, with the offline converter and AC-4's fixed-ratio tables as two configurations. AC-3's MDCT keeps
  its AVX2 path and takes its FFT from `dsp`.
- The cost decision 25 named: the `double` output moves in its last bits. Each kernel is scored again:
  the gold-reference gate, the SNR floors, the quality trend, `tests/golden/ac4dec/scalar-agreement*.json`,
  `tests/golden/ac4-probe-pcm-hashes.json`, the pinned bitstream hashes (regenerated with the reason
  recorded), and the ESP32 timings, none of which may get slower; AC-4 5.1.4 on the P4 runs at 1.57 to
  1.90 of real time already.
- DEE's licence ends on 2026-11-06 ([decision 23](ac4.md#decisions-for-the-encoder-and-the-applications)).
  Scoring against the DEE streams already in the tree needs no licence; a kernel whose proof would need a
  new DEE encode is done before then or not at all.

**C6, what `ac3` holds that is not AC-3's, and the public surface.**
- `io/wav*`, `meta/loudness` and `analysis/levels` to `base` or `dsp`, taking `base::Layout` and an
  integer rate where they take `Acmod` and `SampleRate` (about 50 files,
  [layout.md decision 5(b)](layout.md#i-decisions)); then AC-4's programs use them without going through
  AC-3's types.
- The family's version (`version.cpp`, `version.hpp.in`) to `base`.
- Headers installed by `FILE_SET HEADERS`, so a `detail/` or table header stays out of the package; AC-3's
  tables move behind it.
- `objects`' namespaces to `iclforge::objects` (`iclforge::oba` and `iclforge::emdf` today), with aliases
  for a release.

### Proof per stage

| proof | C0 | C1 | C2 | C3 | C4 | C5 | C6 |
|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| MSVC `/W4 /WX`, every default target | x | x | x | x | x | x | x |
| clang-cl over the changed units; GCC 16 and Clang 22 `-Werror` | x | x | x | x | x | x | x |
| the whole `iclforge-tests` | x | x | x | x | x | x | x |
| pinned bitstream hashes, CLI corpus identical (`baseline.py compare`) | x | x | x | x | x | | x |
| scored again: gold reference, SNR floors, quality trend, scalar agreement | | | | | | x | |
| exported names per library (`export_diff.py`, `abi_compare.py`) | x | x | x | x | x | x | x |
| `ir_compare.py` over the moved units | | x | x | x | | | |
| `check_layering.py`, `check_namespaces.py`, `check_pages.py` | x | x | x | x | x | x | x |
| ESP-IDF pack `--verify`; bare-metal probes; ESP32 timings | | x | x | | x | x | |
| Python, Rust, WASM tests (`ci.yml` dispatch) | | x | x | x | | | x |
| `check_doc_paths.py`, `mkdocs build --strict` | x | x | x | x | x | x | x |

## (f) What this reverses

- [Decision 15 of ac4.md](ac4.md#decisions-for-the-encoder-and-the-applications) took (a), four libraries;
  this takes its (c), one, with the minimal profile as the option it said would be needed.
- [Decision 7 of ac4.md](ac4.md#decisions) took (a), AC-4's own DSP; C5 takes its (b), one DSP library,
  with the cost decision 7 named: `ac3`'s minimal profile, the ESP-IDF component and the ABI gate change.
- [layout.md (j)](layout.md#j-what-stays-out-and-follow-on-ideas) listed "splitting `ac3` the way AC-4 is
  split" as a follow-on; it is the other way round now, and the line is struck there when C1 lands.
- `src/signing/CMakeLists.txt`'s reason for a target of its own, answered in (d).

When a stage lands, `ac4.md` and `layout.md` say so in their status blocks, and `ROADMAP.md` carries a row.

## How C0 to C3 are run

The stages run as N1B's did ([tools/n1b/README.md](../tools/n1b/README.md)), one local branch each, each
made from the one before: `chore/src-consolidation-c0` from the branch this page was written on, then
`-c1`, `-c2`, `-c3`. Within a stage the commits come in N1B's order, each script in a commit of its own
before the commit it makes: the cuts (C1 only), the moves alone (`git mv`, every rename `R100`), the
include spellings, the build files and the paths in text, then what is done by hand.

**The passes.** N1B's scripts are pointed at a plan of their own:

| script | what it does here |
|---|---|
| `consoldef.py` (new) | the moves of appendices A and B as data, one function per stage (`c1_new`, `c2_new`, `c3_new`), as `layoutdef.py` holds L2; the libraries each stage merges (`LIBRARY_MAP`) |
| `ac4_cuts.py` (new) | C1's cuts in today's layout: `ac4.hpp` into `toc.hpp`, `elementary.hpp` and `carriage.hpp`, the decoder's header into four and the encoder's into two, by a table of where each declaration goes; `ac4.cpp` into three units; each consumer includes what it uses |
| `consol_apply.py` (new) | `n1b_apply.py`'s planner and include rewrite with a stage's moves: `git mv`, the include spellings, the export headers and macros of the libraries that merge |
| `consol_text.py` (new) | the rewrites a table can say: C0's profiling markers; a stage's export macros, namespaces and CMake target names |
| `n1b_cmake.py`, `n1b_paths.py` | the moved paths in the build files and in every other text, from the stage's plan |
| `baseline.py` | the record of a tree before and after; it gains an ELF reader for the exports (`nm -D`) and an `install` kind, every file `cmake --install` lays down and the bytes of the text a consumer reads |
| `flags_diff.py` (new) | the flags each unit compiles with, and each archive and link step, of two configured trees (`compile_commands.json`, `ninja -t commands`): a C0 that changes nothing a compiler sees shows nothing, minutes before a build |
| `export_diff.py`, `abi_compare.py` | the exports per library, with a stage's library map (C1, C2, C3) and namespace rewrites (C2, C3) |
| `ir_compare.py` | the moved units' IR, old tree against new (C1 to C3) |

The CMake of a merged library (`src/ac4/CMakeLists.txt` and `minimal.cmake`, `src/containers/CMakeLists.txt`),
the install rules, the package config, the ports, the bindings, the pages and the status of the plans are
by hand, in the stage's last commits.

**The proof, on this machine.** Linux (WSL2), GCC 16 and Clang 22, the presets `config-linux-gcc` and
`config-linux-llvm` (Release, `-Werror`) and `config-linux-llvm-shared` (Debug, `BUILD_SHARED_LIBS=ON`),
each with `ICLFORGE_BUILD_ADM=ON` and the `adm` and `hearth` features, so that every library is built.
Before a stage, its parent is built in a worktree of its own (`build/wt/<stage>-before`) and recorded
(`baseline.py record`); after it, the stage's tree is built, the whole ctest runs, and the two records are
compared: the pinned hashes and the CLI corpus per compiler, the exports of every shared library with the
stage's map, the installed tree. Then `check_layering.py`, `check_namespaces.py`, `check_pages.py`,
`check_doc_paths.py` and `tools/ci/precheck.py`, and the bare-metal probes under QEMU (`arm-none-eabi-gcc`
and `qemu-system-arm` are on this machine): the AC-3 decoder, the encoder, and AC-4 in float, with the stage
timers, and in fixed point, each with `--icount`, so that the instruction count of every fixture is
compared as well as its PCM. What cannot run here is recorded with each stage: MSVC and clang-cl, macOS,
ESP-IDF (`pack_esp_component.py --verify` needs `idf.py`), and the CI dispatch.

A stage that shows a difference stops, and the user is told before the next begins.

## What the runs found that the plan did not

### C0, 2026-10-06 (`chore/src-consolidation-c0`)

The commits, after the plan's own: two fixes to `main` the baseline needed (below); the scripts
(`baseline.py`'s ELF and install records, `flags_diff.py`, `consol_text.py`); the scripted commit
(`consol_text.py --stage c0`: 6 files, 6 includes and 8 markers); the build files by hand (21 files,
+485 −1,599: the twelve libraries' `CMakeLists.txt`, `cmake/IclforgeLibrary.cmake`,
`cmake/InstallLibrary.cmake`, the root, `src/ac3`, `src/base`, `src/ac4core`, the two removed variant
headers and the layering table).

| | the plan | the run |
|---|---|---|
| libraries made by hand | 12 | 12, now `iclforge_add_library()`; `ac4core` stays a plain archive until C1 removes it |
| `iclforge_add_library()` gains | a minimal profile, a source list per option, `COMPILE_ONLY` | the minimal profile, `EMBEDS`, `LINK_PUBLIC`, `STEM`, `EXPORT_HEADER`, and three options that keep the old exports exactly (`LINK_PRIVATE_FIRST`, `BUILD_TREE_DEPENDS`, `NO_C4251_SUPPRESSION`); a source list per option is a list the caller builds (`src/capi`) |
| install blocks | not named | `InstallLibrary.cmake`'s twelve blocks become `iclforge_install_library()` calls, which gains `SHARED_ONLY`, `EXPORT_SET`, `REQUIRES`, `STATIC_REQUIRES`, `GENERATED_HEADERS` |
| `ICLFORGE_DECODE_SCALAR` | at the root, one spelling of its directories | at the root, with `ICLFORGE_DECODE_SCALAR_TIER` (`float64`, `float32`, `fixed32`); `ac4core`'s directories keep their names until C1 moves them (a move in C0 would not be "no file moves") |
| `ac4core`'s profiling variants | removed, `base`'s header | removed; the core takes `base`'s directory by variable (`ICLFORGE_PROFILING_INCLUDE_DIR`), since the minimal profile builds no `iclforge::base`; `layering.json` lets `ac4core` and `ac4dec` include `base` |

**The proof.** Identical, in every kind, on GCC 16 (`config-linux-gcc`), Clang 22 (`config-linux-llvm`)
and the shared tree (`config-linux-llvm-shared`), each with ADM on: the pinned hashes (both modes, the
gate passing), the 44 commands of the CLI corpus per compiler, the exports of all 18 shared libraries
(`abi_compare.py --map identity`: every library −0 +0), the public headers, and the installed tree
(253 files: the package config, 17 export sets and 18 `.pc` files byte for byte). `flags_diff.py`
found every unit compiled with the same flags (822, 822 and 789 units) and every archive and link step
the same (87, 62 and 60), and the same of the bare-metal AC-4 tree (`config-arm-none-eabi-minimal-ac4`,
87 and 5), once AC-4's profiling directory, which C0 changes on purpose, is set aside. The five
bare-metal probes under QEMU with `-icount` (the AC-3 and E-AC-3 decoder, the encoder, and AC-4 in
float, with the stage timers and in fixed point) print the same PCM hash, instruction count, heap,
stack and image size for every fixture. The whole ctest of the GCC and the Clang trees, 3,452 tests
each: all pass but five that skip themselves (streams named by environment variables), as on the
parent.
`check_layering.py` (240 edges), `check_namespaces.py` (191 headers), `check_pages.py` and
`check_doc_paths.py` (6,230 paths) pass; so does `precheck.py` but for the patch attribution
(below).

**What `main` needed first.** The parent did not build with Clang 22 `-Werror`: `src/iamf/src/iamf.cpp`
promoted two floats to double implicitly (#1188) and `tests/iab/test_mxf_writer.cpp` had an unused
item UL (#1187). Each is a commit at the start of the branch, before the baseline, and changes no
output. Three more things of `main`'s are recorded and left: the shared tree's `iclforge-tests` does
not link (`iclforge::iab::DlcAudio::normalized()` is not exported; `shared_libs` is a nightly leg), the
fixed-point AC-4 probe's decode takes 22,712 bytes of stack against a ceiling of 21,500, and the ABI
allowlists of `adm` and `admbridge` are 36 names behind the libraries (CI's shared leg builds no ADM).

Hazards the plan did not name:

- **What a hand-written library exported.** The helper's first draft compiled and linked everything
  the same, and the installed export sets still differed in six files: the AC-4 decoder and encoder
  took the inspector's compile requirements in the build tree only, four libraries set no C4251
  suppression, and `iclforge::signing` linked the codec first and as an ordinary link. Only the
  `install` record, which the plan's four kinds do not include, showed it; three options keep them.
- **`libiclforge_signing.so` carries a copy of the codec.** Its objects link the bare `iclforge::ac3`,
  which is the static archive in a static build, so the shared signer embeds what it calls of the
  codec beside its link to `libiclforge_ac3.so`. Kept as it was; C2 moves the signer into `ac3`.
- **Tracy and AC-4.** Under `ICLFORGE_ENABLE_TRACY` the AC-4 core's markers now make Tracy zones,
  where its own variant made nothing. No Tracy build was run.
- **The machine.** vcpkg wants to write `~/vcpkg`, outside the sandbox: its buildtrees, downloads and
  packages go under `build/vcpkg`, and the trees share one `vcpkg_installed` (`VCPKG_INSTALLED_DIR`).
  Catch2's test discovery writes into `XDG_RUNTIME_DIR` and three tests write under `HOME`; both point
  into `build/tmp` for the builds and the ctest. A fresh build directory clones libadm and libbw64,
  which makes a configure take six to seven minutes. The commits carry the machine's identity, not the
  one `precheck.py`'s attribution check expects, so a push needs them re-attributed first.

Not run here: MSVC and clang-cl, macOS, the ESP-IDF pack (`compote` and `idf.py` are not installed;
the packer's `stage()` was run instead, and stages the same files but the two variant headers C0
removes), the CI dispatch, `ruff` (the sandbox will not install it) and the platform-macro check
(`pwsh`).

### C1, 2026-10-06 (`chore/src-consolidation-c1`)

The commits, after C0 merged: the cuts' script (four commits) and the cut (106 files, +3,005 −2,817);
the passes' scripts; the moves alone (393 renames, every one `R100`); the include spellings (296 files);
the build files and the paths in text (245 files), then nine smaller passes of `consol_text.py`, each
after the commit of its rule; the hand-written commit (74 files, +1,698 −1,844); the ABI allowlist; and
the fixes the proof found (below). 45 commits in all, with this page's and `tools/n1b/README.md`'s; 24 of them are the scripts'.

| | the plan | the run |
|---|---|---|
| AC-4's libraries | four become `iclforge::ac4` | one: `src/ac4/CMakeLists.txt` builds the inspector, the decoder, the encoder and the core as `iclforge::ac4`, and `src/ac4/minimal.cmake` its decode-only archive; the three build files are gone |
| public headers (decision 7) | split in C1 | `ac4.hpp` into `core/toc.hpp`, `io/elementary.hpp` and `io/carriage.hpp`; the decoder's header into `decoder/{config,frame,presentation,decoder}.hpp`, the encoder's into `encoder/{config,encoder}.hpp`; `syntax.hpp` to `core/`; the core's 34 headers private (`src/ac4/src/core/`) |
| `ac4.cpp` | not named | divided as its header was: `src/core/toc.cpp`, `src/io/elementary.cpp`, `src/io/carriage.cpp` |
| the golden directory (decision 10) | `tests/golden/ac4dec` to `tests/golden/ac4` | moved, and the tests under `tests/ac4/{core,decoder,encoder,io}`; the test files keep their names (`test_ac4dec_*.cpp`) and their Catch2 tags (`[ac4dec]`, which `ctest -L` and the pages select by) |
| the encoder's errata | not named | folded into `src/ac4/ERRATA.md` under "The encoder"; its 67 links to the decoder's entries are links within the page |
| install | not named | 253 files become 247: two shared libraries (and their versioned names), three archives, three `.pc` files and two export headers fewer, and ten headers by area in place of four; the package config makes no `iclforge::ac4dec` or `iclforge::ac4enc` |
| ABI allowlists | 16 to 9 over C1 to C3 | 16 to 14 |
| coverage floor | not named | `src/ac4` at 88/80, the lowest of the three it replaces (93/88, 88/80, 88/80); not measured (below) |

**The proof.** Against C0's tree, on GCC 16, Clang 22 and the shared tree, each with ADM on:

- **Identical:** the pinned hashes (both modes, both compilers); the 44 commands of the CLI corpus but
  five, whose output names the stream they read (`probe json=1` prints `"file":
  "tests/golden/ac4/constructed/..."`), and which are identical once the path is read as the old one;
  the exports (`export_diff.py --map c1`: `libiclforge_ac4.so` exports 1,154 names, exactly the union of
  the three, and every other library −0 +0, 3,268 names in all); the allowlists (`abi_compare.py --map
  c1`: the one AC-4 file holds the 60 names the three held); the public headers (`check-moves --pure` on
  the move commit: 199 headers, none lost or edited; after the stage 192 of C0's 193 are where the plan
  sends them, the other the header the cut divided).
- **The whole ctest:** 3,452 tests on GCC and on Clang, all pass but the five that skip themselves, as
  on C0. The shared tree builds every library, and its `iclforge-tests` fails to link as on `main`.
- **The IR** (`ir_compare.py --plan --names none`, 538 units of the Clang tree, compiled at `-O0`): 524
  identical, one the same but for an assertion's white space, eleven that differ only in what a build
  writes (the commit, the branch and the "dirty" line of `version.cpp`, a Qt resource's time stamp, the
  vcpkg path the two trees share), and `ac4.cpp` against `toc.cpp`. The three units the cut made define
  the 1,258 functions `ac4.cpp` did; 179 of its 182 own functions have the same IR, and the other three
  the same source and the same IR but for the order of their stack slots.
- **How each unit compiles and links** (`flags_diff.py --moves`): 822 units become 824 (the cut's two),
  and what changes is what the merge says: the two `STATIC_DEFINE`s and generated directories of the
  libraries that went leave their consumers, the core's units take the library's define and its scalar
  directory, and the programs link the one archive. `libiclforge_ac4.so` is linked from the objects the
  three were.
- **The bare-metal probes** (QEMU, `-icount`): the AC-3 decoder and the encoder print the same PCM hash,
  instruction count, heap, stack and image size for every fixture. The three AC-4 probes print the same
  PCM hash, heap, stack and allocations for all six fixtures, and two things that are not output bytes
  move: the image is 80 to 112 bytes smaller, and the instructions per frame of three or four fixtures
  move by 1,000 or 2,000 (at most 68 parts per million, at the probe's resolution of 1,000). Of the 87
  objects of the AC-4 tree, 85 disassemble the same; `decoder.cpp`'s sections come in another order, and
  the rest is the cut: GCC optimises `parse_raw_frame`, which the decoder calls every frame, in a unit
  of 17,959 bytes in place of one of 30,965, and inlines and clones other helpers than it did. The
  fixed-point probe fails its stack ceiling at 22,712 bytes, as on `main`.
- `check_layering.py` (19 libraries, 152 edges), `check_namespaces.py` (163 public headers in 18
  libraries), `check_pages.py`, `check_doc_paths.py` (6,201 paths) and the unit tests of `tools/n1b`,
  `tools/checks` and `tools/ci` pass; `precheck.py` fails only the patch attribution, as on C0. The
  fuzz tree configures, instruments all 80 AC-4 units and builds the three AC-4 harnesses, which run
  their seeds clean. The ESP-IDF packer's `stage()` stages the same files outside AC-4 and AC-4's
  decoder without the encoder (477 files become 483: the headers by area, the cut's units).

The bare-metal difference is the one the user is asked about before C2: the plan said C1 changes no
output byte, and it does not, but its cut changes the code GCC makes of the inspector's unit, which
the instruction counts and the image size see.

Hazards the plan did not name:

- **Paths that are data.** `consol_text.py` leaves `tests/golden/` alone, as N1B's passes did, and three
  kinds of path inside it moved with the directory: the stream a syntax digest names (51 files; the test
  failed on each), the keys of the scalar-agreement floors (`check_ac4_decode_scalar_snr.py` keys them by
  path from the root; 114 keys), and two tests that build the path from parts (`".." / "ac4dec" /
  "presentations"`). Each is a rule of its own, scoped to the files it is for.
- **Proof tools that read the tree.** `cli_bytes.py` globbed `tests/golden/ac4dec/constructed`, so on
  C1's tree the corpus had no AC-4 commands at all; the comparison lists a missing command, which is how
  it showed. It reads either directory now.
- **Spellings that are not includes.** `tools/generators/gen_ac4_tables.py` writes the tables' includes
  as strings; the core's headers became private, and their public spellings are in no include the
  spelling pass reads. The text pass follows every moved public header to its private spelling now.
  The comments that spelt an AC-4 header as it was before N1B (`ac4enc/encoder.hpp`) follow N1B's map.
- **A configuration no preset builds.** `fuzz/CMakeLists.txt` instrumented `iclforge_ac4core`, a
  target that no longer exists; no tree here configures the fuzzers, so only a configure of one found
  it.
- **N1B's own header map.** `n1b_docs.py`'s map is held to what N1B's layout gives and to the headers of
  the tree, which a later stage makes disagree. The map stays as derived; `LATER_SPELLINGS` follows a
  header a later stage moved (C0's profiling header, C1's five).
- **Names that stay.** The macro `AC4CORE_ALSO_AT_DOUBLE`, the test files' prefixes and the Catch2 tags
  name the core, the decoder and the encoder, which are areas of one library now; renaming them changes
  no output but the tags are what `ctest -L ac4dec` and the pages select by, so they are left for a
  decision of their own.
- **The machine.** The check scripts' tests make git repositories under `TMPDIR`, which must not be
  inside the worktree (`build/tmp` is); they run with `TMPDIR=/tmp`. A fuzzer writes what it finds into
  the first corpus directory it is given, which is the committed seeds.

Not run here: as for C0, and the coverage run (`gcovr` is not installed), so the floor of `src/ac4` is
the lowest of the three it replaces rather than a measurement.

## (g) Decisions

### Taken on 2026-10-06

1. **The shape of a codec.** **Taken: AC-3's**, one library by area, for AC-4 and for any later codec.
2. **The containers.** (a) **`mp4`, `mpegts`, `matroska`, `iamf` and `iec61937` as one library**;
   (b) the same without `iec61937`, which `audio` links; (c) as now. **Taken: (a).**
3. **`arithmetic`.** **Taken: into `base`.**
4. **`admbridge`.** **Taken: into `adm`.**
5. **Signing.** **Taken, on the user's words:** signing is a part of each codec, since what is signed and
   the algorithm differ ("signing should probably be in ac3 with ac4 having its own signing"). AC-3's
   signer goes into `ac3` now and AC-4's into `ac4` when it is built; the key, SHA-256 and HMAC, which
   both use, go to `base` (d).
7. **AC-4's public headers.** **Taken: (a)**, split in C1.
10. **The golden directories.** **Taken, on the user's words:** "tests structure should ideally map
    directly to src structure so move/consolidate/rename as necessary". `tests/golden/ac4dec/` becomes
    `tests/golden/ac4/` in C1, and a golden directory named for a library that merges follows it in C2
    and C3.

### Open

6. **The containers' namespace.** (a) **`iclforge::containers::mp4` and so on** (recommended: one rule for
   path, target and namespace, as `iclforge::ac3::io` is; headers `iclforge/containers/mp4/mp4.hpp`);
   (b) keep `iclforge::mp4` and its kin inside the one library, a recorded exception to the lock. Cost of
   (a): every use of the five namespaces in the programs, the tests and the bindings, done by a pass as
   N1B's S3 was.
8. **The order.** (a) **C0, C1, C2, C3 as one freeze, then C4, C5, C6 as they are ready** (recommended:
   the mechanical stages are disruptive in the same files and are cheapest together); (b) each stage
   alone; (c) C1 first, before C0.
9. **C5 at all.** (a) **proceed, kernel by kernel, with re-scoring** (recommended); (b) stop after C4 and
   keep two QMF banks and two FFTs, recorded as accepted, as decision 25(b) priced it.

## Appendix A: the moves of C1

From today's paths, file counts in brackets. A directory moves whole unless a line says otherwise.

| from | to |
|---|---|
| `src/ac4/include/iclforge/ac4/ac4.hpp` | `src/ac4/include/iclforge/ac4/core/toc.hpp`, `io/elementary.hpp`, `io/carriage.hpp` (the cut) |
| `src/ac4/include/iclforge/ac4/syntax.hpp` | `src/ac4/include/iclforge/ac4/core/syntax.hpp` (to `base` in C4) |
| `src/ac4/src/ac4.cpp` | `src/ac4/src/core/toc.cpp`, `src/ac4/src/io/elementary.cpp`, `src/ac4/src/io/carriage.cpp` (the cut) |
| `src/ac4core/include/iclforge/ac4core/{tables,aspx,acpl,ajcc,ajoc}/` [16] | `src/ac4/src/core/{tables,aspx,acpl,ajcc,ajoc}/` |
| `src/ac4core/include/iclforge/ac4core/huffman_codebook.hpp` | `src/ac4/src/core/huffman_codebook.hpp` |
| `src/ac4core/include/iclforge/ac4core/dsp/` [18] | `src/ac4/src/core/dsp/` (to `src/dsp` in C5) |
| `src/ac4core/src/{tables,aspx,acpl,ajcc,ajoc,dsp}/` [19] | `src/ac4/src/core/{tables,aspx,acpl,ajcc,ajoc,dsp}/` |
| `src/ac4core/variants/scalar-{double,float,fixed}/iclforge/ac4core/detail/real.hpp` | `src/ac4/variants/decode-scalar-{float64,float32,fixed32}/iclforge/ac4/detail/real.hpp` |
| `src/ac4core/variants/profiling-{none,stage_timers}/` | removed in C0 (`base`'s) |
| `src/ac4core/CMakeLists.txt` | removed; its scalar, `-O3`, constexpr-limit and `-Wdouble-promotion` blocks go to `src/ac4/CMakeLists.txt` and `minimal.cmake` |
| `src/ac4dec/include/iclforge/ac4dec/decoder.hpp` | `src/ac4/include/iclforge/ac4/decoder/{decoder,config,frame,presentation}.hpp` (the cut) |
| `src/ac4dec/src/{decoder.cpp,presentations.hpp,presentations.cpp,huffman.hpp,huffman.cpp}` | `src/ac4/src/decoder/` |
| `src/ac4dec/src/bit_reader.hpp` | `src/ac4/src/core/bit_reader.hpp` (to `base` in C4) |
| `src/ac4dec/src/syntax/` [24] | `src/ac4/src/decoder/syntax/` |
| `src/ac4dec/src/pcm/` [39] | `src/ac4/src/decoder/pcm/` (`drc`, `de`, `objects`, `isf` lifted to `meta/` and `oba/` in C4) |
| `src/ac4enc/include/iclforge/ac4enc/encoder.hpp` | `src/ac4/include/iclforge/ac4/encoder/{encoder,config}.hpp` (the cut) |
| `src/ac4enc/src/encoder.cpp` | `src/ac4/src/encoder/encoder.cpp` |
| `src/ac4enc/src/bit_writer.{hpp,cpp}` | `src/ac4/src/core/bit_writer.{hpp,cpp}` (to `base` in C4) |
| `src/ac4enc/src/{asf,aspx,acpl,ajcc,ajoc,oamd,frame}/` [45] | `src/ac4/src/encoder/{asf,aspx,acpl,ajcc,ajoc,oamd,frame}/` (`frame/drc_gains`, `frame/dialogue` lifted to `meta/` in C4) |
| `src/ac4dec/ERRATA.md`, `src/ac4enc/ERRATA.md` | `src/ac4/ERRATA.md`, two sections; the pages that link them follow |
| `src/ac4dec/CMakeLists.txt`, `src/ac4enc/CMakeLists.txt` | removed; `src/ac4/CMakeLists.txt` and `src/ac4/minimal.cmake` |
| `tests/ac4/` [5] | `tests/ac4/core/`, `tests/ac4/io/` |
| `tests/ac4core/` | `tests/ac4/core/` |
| `tests/ac4dec/` | `tests/ac4/decoder/` |
| `tests/ac4enc/` | `tests/ac4/encoder/` |
| `tools/ci/abi-allowlist/libiclforge_{ac4,ac4dec,ac4enc}.so.txt` | `libiclforge_ac4.so.txt`, regenerated |

The include spellings that change: `iclforge/ac4/ac4.hpp`, `iclforge/ac4dec/decoder.hpp`,
`iclforge/ac4enc/encoder.hpp` (public), and every `iclforge/ac4core/...` (private, inside the library and
its tests). The fuzz targets (`fuzz_ac4_parse`, `fuzz_ac4_decode`, `fuzz_ac4_encode`) stay where they are
and link `iclforge::ac4`.

## Appendix B: the moves of C2 and C3

| from | to |
|---|---|
| `src/arithmetic/include/iclforge/arithmetic/{fixed32,mant_exp,scalar_math}.hpp` | `src/base/include/iclforge/base/arithmetic/` |
| `src/arithmetic/variants/arch-{generic,x86_64,aarch64}/iclforge/arithmetic/detail/simd.hpp` | `src/base/variants/arch-{generic,x86_64,aarch64}/iclforge/base/detail/simd.hpp` |
| `src/arithmetic/CMakeLists.txt` | removed; the SIMD resolution to `src/base/CMakeLists.txt` |
| `src/signing/include/iclforge/signing/signing_key.hpp`, `src/signing/src/signing_key.cpp` | `src/base/include/iclforge/base/crypto/signing_key.hpp`, `src/base/src/crypto/signing_key.cpp` |
| `src/signing/src/{sha256,hmac_sha256}.{hpp,cpp}` | `src/base/include/iclforge/base/crypto/{sha256,hmac_sha256}.hpp`, `src/base/src/crypto/` |
| `src/signing/include/iclforge/signing/emdf_atmos_signer.hpp`, `src/signing/src/emdf_atmos_signer.cpp` | `src/ac3/include/iclforge/ac3/signing/`, `src/ac3/src/signing/` |
| `src/signing/CMakeLists.txt` | removed |
| `tests/signing/` | `tests/ac3/signing/` (the signer), `tests/base/` (the primitives) |
| `src/admbridge/include/iclforge/admbridge/{bridge,iab_bridge,coordinates}.hpp` | `src/adm/include/iclforge/adm/` |
| `src/admbridge/src/{bridge,iab_bridge,coordinates}.cpp`, `src/admbridge/ERRATA.md` | `src/adm/src/`, `src/adm/ERRATA.md` |
| `tests/admbridge/` | `tests/adm/` |
| `src/{mp4,mpegts,matroska,iamf,iec61937}/include/iclforge/<name>/` | `src/containers/include/iclforge/containers/<name>/` |
| `src/{mp4,mpegts,matroska,iamf,iec61937}/src/` | `src/containers/src/<name>/` |
| `src/{mp4,mpegts,matroska,iamf,iec61937}/CMakeLists.txt` | removed; `src/containers/CMakeLists.txt` |
| `tests/{mp4,mpegts,matroska,iamf,iec61937}/` | `tests/containers/<name>/` |
| `tools/ci/abi-allowlist/libiclforge_{mp4,mpegts,matroska,iamf,iec61937}.so.txt` | `libiclforge_containers.so.txt` |
| `tools/ci/abi-allowlist/libiclforge_signing.so.txt` | removed; its names in `libiclforge_ac3.so.txt` and `libiclforge_base.so.txt` |

After C3 the ABI allowlists are `base`, `dsp`, `objects`, `render`, `ac3`, `ac4`, `containers`, `iab` and
`c`: nine, from sixteen.
