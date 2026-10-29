# TrueHD

A TrueHD and MLP library, `iclforge::truehd` (`libs/truehd`): the bitstream, a lossless decoder, then an encoder, the
Atmos substream and its object metadata, and the front ends. This page is the plan. Status is in
[ROADMAP.md](../ROADMAP.md); what ships is in [docs/library/capabilities.md](../docs/library/capabilities.md) once it lands.

As of 2026-10-29: phase T1 is built and its checks pass (the bitstream layer, the decoder, `forge decode` and `forge probe`; bit-exact with
FFmpeg on its fixtures, the rest held to hand-built streams, which FFmpeg's decoder reads too except the 16-channel substream). Phase T2 waits on the owner's answers to the questions below and in
[libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md).

## Sources, and what they are

- **The specification.** The owner supplied *Dolby TrueHD (MLP) Reference Specification* (versions 3.0 to 3.5, 2026-10-28 and
  29; v3.5 is the one used) and says parts of it derive from analysis of the Dolby Encoding Engine (DEE) and of FFmpeg, and
  takes responsibility for that. It is not in the tree. Code and docs cite it as "TH spec §n" (section) or "TH spec App. X".
  It contradicts itself and the streams in places and is silent in others: every such place is a numbered entry in
  [libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md), and the ones that need the owner are listed first there.
- **Streams from FFmpeg's encoder.** FFmpeg's `truehd` and `mlp` encoders write streams the project can decode, and its decoder
  is the independent check: the codec is lossless, so a decode either equals the source sample for sample or is wrong. FFmpeg's
  source is not read here (the rule AC-4's oracles follow); its output is.
- **Streams from DEE.** The owner decided on 2026-09-26 that these are test inputs only: decoded, compared with their known
  source and passed through, with no analysis of how Dolby's encoder made them. Nothing in this plan or in `libs/truehd`
  reads one to learn the format. `tools/generators/gen_dee_gold.py` makes the set, which stays on the owner's machine.
- **Not used:** the branch `feature/truehd-atmos-support` (23 commits, 1,155 behind `main` on 2026-09-29). The ROADMAP says no
  TrueHD decoder reads its block layout and that it carries non-redistributable reference PDFs, so it is superseded by this
  library and nothing is read from it or ported. The ROADMAP row that points at it is corrected.

The clean-room rule in [CONTRIBUTING.md](../CONTRIBUTING.md) and the ROADMAP line that keeps TrueHD interoperability by
black-box analysis of Dolby streams out of scope are the owner's to amend; this plan changes neither.

## Names

The directory, the target and the namespace are `truehd` (`libs/truehd`, `iclforge::truehd`, option `ICLFORGE_BUILD_TRUEHD`),
the codec's name, as [layout.md](layout.md) decision 12 recommends. The ROADMAP's earlier `iclforge::mlp` /
`ICLFORGE_BUILD_MLP` is the container family's name and is not used. The library reads and writes both stream types: TrueHD
(`0xF8726FBA`) and MLP (`0xF8726FBB`, DVD-Audio).

## Phases

| Phase | What | State |
|---|---|---|
| T1 | The library skeleton wired into the build; the syntax layer (access units, major sync, substream directory, restart header, block header, filters, matrices, the CRCs and parities), both directions; an elementary stream reader; the lossless decoder for substreams 0 to 2 and a generic 16-channel substream 3 with its extra data carried as opaque bytes; an independent Python transcription; bit-exact interoperability with FFmpeg's encoder and decoder. | Done; noise generator 1 and heavy DRC are not decoded |
| T2a | The encoder: matrices, prediction filters, codebook and offset choice, restart cadence, the FIFO and `input_timing` rate control, the 2ch and 6ch downmix substreams, verification by decoding. | Done except the opt-in tools: bit-exact with FFmpeg's strict decode in 43 cases and a fuzzer; IIR, LSB bypass, a negative output shift and heavy DRC are not written, and nothing outside the library reads the downmix presentations. Streams are 0.95 times FFmpeg's size |
| T2b | Atmos: the sixteen-channel substream, the evolution frame, object audio metadata (ETSI TS 103 420, `iclforge::objects`), and an optional operator-keyed check of the frame in the style of [object signing](../docs/concepts/object-signing.md). Blocked on the owner's answers about the evolution payload (ERRATA.md, open questions). | Not started |
| T2c | Containers (MP4 `mlpa`, Matroska `A_TRUEHD`, MPEG-TS, IEC 61937 and MAT), the `forge` commands, C API and Python bindings, the FFmpeg legs of `run_codec_matrix.sh` and the gold-reference gate, and the DEE scoring harness. T1 adds the minimum `forge decode` and `forge probe` for `.thd` and `.mlp` that its interoperability check needs. | Done except the bindings: the containers (T2c1); `forge` on every command that reads an AC-3 family stream and `truehd-encode`, the C API (decode and encode), the matrix, the gold-reference gate, the workflows and `score_truehd_decode.py` with its instructions (T2c2). Python, JavaScript and Rust bindings: see below |

### Not done in T2c

- **Bindings.** `iclforge_truehd_*` is in the C API. Nothing wraps it in `bindings/python`, `bindings/rust` or the JavaScript/WebAssembly
  package; the Rust crate's build script lists no `libs/truehd` path. Each is a follow-up on the C API alone.
- **A `.forgeproj` for TrueHD.** The project file's commands (`apps/shared/project`) do not name `truehd-encode`.
- **MLP encoding.** The encoder writes TrueHD (`0xF8726FBA`); `forge truehd-encode` refuses an `.mlp` name.
- **Scoring against DEE.** `tools/checks/score_truehd_decode.py` has been run on streams this tree made, not on DEE's. It has to be run
  on the owner's machine before 2026-11-06 ([Scoring DEE's TrueHD streams](../docs/verification.md#scoring-dees-truehd-streams)).
- **Readings that are this work's own** and wait for the owner: the WAV channel mask as TrueHD channels, the speakers the loudness
  weights are keyed on (`libs/truehd/ERRATA.md`, open questions 16 and 17), and the MAT carrier's rate and channel count in a WAV
  (`libs/containers/ERRATA.md`, open question 2).

## Oracles

Ranked as in [CONTRIBUTING.md](../CONTRIBUTING.md#oracles), for this codec.

1. **FFmpeg's decoder**, strict (`-v error -xerror -err_detect crccheck+bitstream+buffer+explode`), on streams this library
   writes, and its encoder's streams through this library's decoder. Bit-exact in both directions. FFmpeg 6.1.1 here, 8.x in
   CI. FFmpeg's encoder writes 1 to 6 channels (stereo and 5.1 are the layouts used), 44.1 to 192 kHz, 16 and 24 bits, with
   one or two substreams. It does not write the Atmos substream, 7.1, noise generation, an IIR filter that is not trivial, or
   LSB bypass. Hand-built streams carry noise generation, an IIR filter and LSB bypass, and FFmpeg's decoder reads them to
   the same samples; this library's encoder writes 7.1, which FFmpeg also decodes. The Atmos substream (restart sync 0x31EC) is refused by FFmpeg, so it has no
   external oracle.
2. **`tools/references/truehd_syntax.py`**, an independent transcription of the syntax and of the inverse filters and
   matrices, compared with the C++ decoder on the committed fixtures and on the hand-built streams. It is the second reader
   of the features FFmpeg's decoder does not read (the extra data, the Atmos substream).
3. **DEE's streams** of known sources, scored by `tools/checks/score_truehd_decode.py` on the owner's machine (the set is not
   committed). DEE's licence ends on 2026-11-06 and is not renewed, so the set has to be made before then. The legs worth
   having, all in `gen_dee_gold.py`'s `truehd` group: 2ch, 6ch and 8ch at 48 and 96 kHz and 16 and 24 bits from tones, music,
   noise, sweeps and film; the 8ch-with-downmix-substreams and independent-2ch presentations; each 2ch format; DRC per
   presentation; dialogue normalisation values; and `--optimize-data-rate`. A TrueHD Atmos leg would be the only external
   check of the sixteen-channel substream, and `--presentation atmos` with an ADM master is how `gen_dee_gold.py --masters`
   asks for one (DEE refuses it for the AC-3 family; whether it refuses it for TrueHD is not known).
4. **Hand-built streams** from the Python writer, for the features no encoder here writes. FFmpeg's decoder reads most of them
   (`tools/checks/verify_truehd_interop.py --handbuilt`), and its behaviour decided four readings the specification leaves open
   (ERRATA questions 5, 6 and 18, and entry 21).

## Decisions for the owner

These are also the first section of ERRATA.md, with the evidence; ERRATA.md has seven more (generator 1's noise table, the lossless check's
channel shift, negative output shifts, IIR state, the block data CRC, heavy DRC, where a bypassed LSB comes from) that T1 found.

1. What the evolution payload holds on the wire (TH spec §16.3, §16.6). Since v3.4 it is an outer Thrift container with the typed
   payload entries (the object audio metadata) and a nested `ContentSignature`, and §16.3.3 lists the outer field IDs, the digest scope
   and per-frame or per-stream signing as unconfirmed on the specification's side too. T1 carries the extra data as opaque bytes and
   T2b writes nothing until the owner has these.
2. The unit and meaning of `extra_channel_meaning_length` and the size of a major sync that carries it (TH spec §14.2).
   The decoder finds the size by the CRC.
3. Whether the terminators of TH spec §16.5 (`0xD234E`, `0x348D3`, `0x1234`) are written. FFmpeg's encoder writes none, and
   the decoder accepts streams with or without them.
4. Which presentation a plain `forge decode` gives for a stream with the sixteen-channel substream.
   (T1: `best`, substreams 0 to 2; `truehd-presentation=all` reads substream 3.)
5. Whether a `forge` run on TrueHD or MLP refuses an option it does not apply or warns about it (ERRATA.md question 21). It
   refuses today, by name with a usage error, and warns only where the option cannot change the output or any verdict, which is the
   audit's recommendation (planning/truehd-parity-forge.md, question 7) and **provisional** until the owner answers.

## CI: what each lane holds for TrueHD, and what is open

Audited and closed on 2026-10-29 against the lanes that hold E-AC-3 (AC-4 where it is the closer comparison). "Covered" means a
lane builds or runs the TrueHD, crypto or signing code; the other words say what keeps it out.

| Lane | State |
|---|---|
| Static job (layering, namespaces, project pages, packaging, doc paths, Sonar scope, ruff, shellcheck, actionlint) | Covered. The packaging check and the ESP-IDF target check failed on this branch and are fixed (below). New: the fixture manifest check and the fixture feature check. |
| Pull-request gate: Linux GCC build, every ctest case, gold-reference gate | Covered. The gate decodes every manifest stream against FFmpeg's decode bit for bit and round-trips the encoder on the 5.1 reference. |
| Codec matrix and its coverage check | Covered. The decode legs follow the manifest (25 streams), the signing section runs every option of the usage block, and the coverage check now reads the TrueHD commands and options and the signature options. |
| FFmpeg Validate (nightly, `ci:deep`) | Covered: decode, hand-built streams, encoder, encoder-space regressions, containers, fixture features, Blu-ray layout stand-ins. FFmpeg is pinned to 8.0.1 here. The hand-built manifest was written against 6.1.1; for another major version the leg holds only the streams FFmpeg decodes to the whole signal and prints a notice, which cannot fail the job. Not run against 8.0.1 (not available to the agent). |
| ASan and UBSan leg | Covered: the full ctest run and the codec matrix, signing and TrueHD sections included. |
| TSan leg | Not applicable. It runs the cases tagged `concurrency`; E-AC-3 has none, and the TrueHD and crypto code has no mutable static or thread state. |
| Coverage | `libs/truehd` had no row; it has 84 line and 74 branch, under 87.1 and 79.7 measured on its own tests. `libs/base` takes the crypto under its existing floor. |
| Speed trend and real-time guard | Covered by the performance task (`planning/truehd-parity-performance.md`): the four workloads `truehd_stereo_encode`, `truehd_51_encode`, `truehd_stereo_decode` and `truehd_51_decode` in `iclforge-bench`, `iclforge-membench` and the guards of `iclforge-perf`, and the kernels in `iclforge-kernelbench`. The badge, card and tile tables list them. Each executable's TrueHD code is a file of its own with a stand-in for `ICLFORGE_BUILD_TRUEHD=OFF`, which skips the guards and adds no row. |
| Shared-library pass and ABI gate | Were broken: five exported-by-header functions of `libs/truehd` were not exported, so `forge` and the TrueHD tests did not link. Fixed; the export allowlists name the crypto, signing and MP4 reader functions. |
| Minimum-footprint presets, ESP-IDF component, minimal AC-4 archive | Were broken: configure stopped at the root's guard, and the minimal AC-4 archive carried the signing source. Fixed, with tests. |
| Packages | The vcpkg port and the Conan recipe offer `truehd` (off by default, like `ac4`). CMake install and the CPack library component are generic. The Python wheel builds the library without using it. |
| Windows MSVC, clang-cl, macOS | Not run here. Read: five sources lacked the per-file `/wd4996` the project uses for `getenv` and `fopen` (added). The new library sources compile with Clang 18 at MSVC's constexpr step limit and `-Wconversion -Wsign-conversion` clean. Nothing assumes GCC or Linux. |
| CodeQL, MSVC analysis | Covered: both build the full preset. |
| clang-tidy | Not covered, and not for AC-4 either: `static-analysis.yml` lists `ac3`, `objects`, `render`, `base`, `dsp`, parts of `containers` and the CLI. `libs/ac4` and `libs/truehd` are in none of them. |
| Fuzz regress, short and nightly | Not covered: `tools/fuzz/run.sh` lists no TrueHD harness (the three have no seeds yet). The fuzzing task's. |

Open, for the owner:

1. Should the vcpkg port and the Conan recipe offer `truehd` (done, off by default), or pin it off until a program that ships needs it?
2. Should TrueHD be a column of the README badges and the performance cards? It is now; accuracy and memory read "no data" for it, as AC-4's did.
3. `ICLFORGE_BUILD_TRUEHD=OFF` with tests on is supported by the CMake files (the C API's absent file and its test, and the stand-ins of the four performance executables) and built by no lane. Build it in
   one, or refuse it in the root as `ICLFORGE_BUILD_AC4=OFF` is?
4. The AC-4 and TrueHD libraries are outside clang-tidy's list. Add them (a first nightly with findings), or leave them to CodeQL and Sonar?
5. Forge writes the TrueHD WAV with `std::fopen`, which takes an ANSI path on Windows; a non-ASCII path needs `std::ofstream` or a wide path.

## Not in scope here

Encrypted or signed content (the AC-4 workstream's signing machinery is the intended carrier for TH spec §22; it is not
used here), the bed/object renderer, 44.1 kHz-family Atmos (the specification allows 48 kHz only), and anything that
analyses a Dolby-made stream.
