# TrueHD

A TrueHD and MLP library, `iclforge::truehd` (`libs/truehd`): the bitstream, a lossless decoder, then an encoder, the
Atmos substream and its object metadata, and the front ends. This page is the plan. Status is in
[ROADMAP.md](../ROADMAP.md); what ships is in [docs/library/capabilities.md](../docs/library/capabilities.md) once it lands.

As of 2026-10-29: phase T1 is built and its checks pass (the bitstream layer, the decoder, `forge decode` and `forge probe`; bit-exact with
FFmpeg on its fixtures, the rest held to hand-built streams, which FFmpeg's decoder reads too except the 16-channel substream). The
decoder follows TH spec v3.9 where it answers the open questions (noise generator 1 and its table, the output shift by matrix channel,
the lossless check, the filter history across a restart, the extra data's check nibble, the terminators, `peak_data_rate`, the
channel assignment bitmask, the restart syncs of §7.2), scored on the owner's Blu-ray and on the 13 DEE fixtures of
`testdata/truehd-dee` (11 pass, 2 at 96 kHz unexplained, none fail). Phase T2 waits on the owner's answers to the questions below and in
[libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md).

## Sources, and what they are

- **The specification.** The owner supplied *Dolby TrueHD (MLP) Reference Specification* (versions 3.0 to 3.9, 2026-10-28 and
  29; v3.9 is the one used: v3.7 supplied the noise generator 1 table and answered many of the questions below, v3.9 reads §7.2's restart
  syncs and §19.1's apodising filters differently) and says parts of it derive from analysis of the Dolby Encoding Engine (DEE) and of
  FFmpeg, and takes responsibility for that. It is not in the tree. Code and docs cite it as "TH spec §n" (section) or "TH spec App. X".
  It contradicts itself and the streams in places and is silent in others: every such place is a numbered entry in
  [libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md), and the ones that need the owner are listed first there.
- **Streams from FFmpeg's encoder.** FFmpeg's `truehd` and `mlp` encoders write streams the project can decode, and its decoder
  is the independent check: the codec is lossless, so a decode either equals the source sample for sample or is wrong. FFmpeg's
  source is not read here (the rule AC-4's oracles follow); its output is.
- **Streams from DEE.** The owner decided on 2026-09-26 that these are test inputs only: decoded, compared with their known
  source and passed through, with no analysis of how Dolby's encoder made them. Nothing in this plan or in `libs/truehd`
  reads one to learn the format. `tools/generators/gen_dee_gold.py` makes the set, which stays on the owner's machine apart from the 13
  streams committed in `testdata/truehd-dee` (the owner's decision of 2026-10-29), which the scorer reads.
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
| T2b | Atmos: the sixteen-channel substream, the evolution frame, object audio metadata (ETSI TS 103 420, `iclforge::objects`), and an optional operator-keyed check of the frame in the style of [object signing](../docs/concepts/object-signing.md). The read side (the extra data into plain structures, the evolution frame under two readings, `forge probe`'s report) is built and provisional, from TH spec v3.6; the write side is planned in [truehd-atmos.md](truehd-atmos.md) and waits on the owner's answers about the container and the time base (ERRATA.md, "Atmos, spec v3.6", questions 22 to 39). | Read side done (provisional); write side not started |
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
   LSB bypass. Hand-built streams carry noise generators 0 and 1 (at 48, 96 and 192 kHz), an IIR filter and its states, and LSB bypass
   (under either restart sync), and FFmpeg's decoder reads them to the same samples; this library's encoder writes 7.1, which FFmpeg also decodes. The Atmos substream (restart sync 0x31EC) is refused by FFmpeg, so it has no
   external oracle.
2. **`tools/references/truehd_syntax.py`**, an independent transcription of the syntax and of the inverse filters and
   matrices, compared with the C++ decoder on the committed fixtures and on the hand-built streams. It is the second reader
   of the features FFmpeg's decoder does not read (the extra data, the Atmos substream).
3. **DEE's streams** of known sources, scored by `tools/checks/score_truehd_decode.py` on the owner's machine (the full set of 131 is
   not committed; 13 of its streams are, in `testdata/truehd-dee`, for scoring only: nothing reads one to learn the format). DEE's licence
   ends on 2026-11-06 and is not renewed, so the set had to be made before then. The legs worth
   having, all in `gen_dee_gold.py`'s `truehd` group: 2ch, 6ch and 8ch at 48 and 96 kHz and 16 and 24 bits from tones, music,
   noise, sweeps and film; the 8ch-with-downmix-substreams and independent-2ch presentations; each 2ch format; DRC per
   presentation; dialogue normalisation values; and `--optimize-data-rate`. A TrueHD Atmos leg would be the only external
   check of the sixteen-channel substream, and `--presentation atmos` with an ADM master is how `gen_dee_gold.py --masters`
   asks for one (DEE refuses it for the AC-3 family; whether it refuses it for TrueHD is not known).
4. **Hand-built streams** from the Python writer, for the features no encoder here writes. FFmpeg's decoder reads most of them
   (`tools/checks/verify_truehd_interop.py --handbuilt`), and its behaviour decided readings the specification left open
   (ERRATA questions 5, 6 and 18, and entries 21 and 44; v3.9 has since confirmed most of them).
5. **The owner's Blu-ray**, analysed by the owner on the owner's machine (`planning/truehd-disc-findings.md`): an Atmos 7.1 stream of
   four substreams whose lossless checks, with the 2, 6 and 8 channel presentations, are the strongest check of the decoder on a
   stream a Dolby encoder wrote.

## Decisions for the owner

These are also the first section of ERRATA.md, with the evidence; ERRATA.md has more that T1 found. TH spec v3.9 answered several of them
(generator 1's noise table and the LSB bypass, the lossless check's channel shift, negative output shifts, IIR state, the extra data's
length and check nibble, `extra_channel_meaning_length`'s unit, the terminators, `peak_data_rate`, the channel assignment bitmask); each says
so in ERRATA.md, with the provisional marks the owner asked to keep until the DEE set has scored them. Still open: the block data CRC, where heavy
DRC sits on the wire, the width of `zero_samples`, where the extension's length nibble sits, which way `ch_assign` is worded, the details of
noise generator 1 (question 43), the PACE layering, and the payload of the evolution frame.

1. What the evolution payload holds on the wire (TH spec §16.3, §16.6). Since v3.4 it is an outer Thrift container with the typed
   payload entries (the object audio metadata) and a nested `ContentSignature`, and §16.3.3 lists the outer field IDs, the digest scope
   and per-frame or per-stream signing as unconfirmed on the specification's side too. T1 carries the extra data as opaque bytes and
   T2b writes nothing until the owner has these. v3.6 adds a second description of the same bytes (a bit-level container, §15.4);
   ERRATA.md question 22 asks which real streams carry, and `forge probe` on a real Atmos track answers it.
2. The unit of `extra_channel_meaning_length` is answered (TH spec v3.9 §14.2: `length + 1` words, and the owner's Blu-ray has 1); where
   its nibble sits against the first word is question 44. The decoder finds the size by the CRC, trying the sizes the length gives first.
3. The terminators of TH spec §16.5 (`0xD234E`, `0x348D3`, `0x1234`) are answered: DEE can write them, Dolby's Blu-ray encoder writes
   none (planning/truehd-disc-findings.md), the decoder accepts streams with or without them, and this encoder writes none.
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
| Coverage | `libs/truehd` had no row; it has 84 line and 74 branch, under 87.1 and 79.7 measured on its own tests. `libs/base` takes the crypto under its existing floor. The three Qt applications (`apps/forge/gui`, `apps/crucible/ui`, `apps/hearth/ui`) have rows of their own since 2026-10-31, gated by the nightly `coverage-qt` job; the floors (40 line, 25 branch) are unmeasured and provisional until its first hosted run. |
| Speed trend and real-time guard | Covered by the performance task (`planning/truehd-parity-performance.md`): the four workloads `truehd_stereo_encode`, `truehd_51_encode`, `truehd_stereo_decode` and `truehd_51_decode` in `iclforge-bench`, `iclforge-membench` and the guards of `iclforge-perf`, and the kernels in `iclforge-kernelbench`. The badge, card and tile tables list them. Each executable's TrueHD code is a file of its own with a stand-in for `ICLFORGE_BUILD_TRUEHD=OFF`, which skips the guards and adds no row. |
| Shared-library pass and ABI gate | Were broken: five exported-by-header functions of `libs/truehd` were not exported, so `forge` and the TrueHD tests did not link. Fixed; the export allowlists name the crypto, signing and MP4 reader functions. |
| Minimum-footprint presets, ESP-IDF component, minimal AC-4 archive | Were broken: configure stopped at the root's guard, and the minimal AC-4 archive carried the signing source. Fixed, with tests. |
| Packages | The vcpkg port and the Conan recipe offer `truehd` (off by default, like `ac4`). CMake install and the CPack library component are generic. The Python wheel builds the library without using it. |
| Windows MSVC, clang-cl, macOS | Not run here. Read: five sources lacked the per-file `/wd4996` the project uses for `getenv` and `fopen` (added). The new library sources compile with Clang 18 at MSVC's constexpr step limit and `-Wconversion -Wsign-conversion` clean. Nothing assumes GCC or Linux. |
| CodeQL, MSVC analysis | Covered: both build the full preset. |
| clang-tidy | Not covered, and not for AC-4 either: `static-analysis.yml` lists `ac3`, `objects`, `render`, `base`, `dsp`, parts of `containers` and the CLI. `libs/ac4` and `libs/truehd` are in none of them. |
| Fuzz regress, short and nightly | Covered (2026-10-29). `tools/fuzz/run.sh` lists the TrueHD reader, decoder and encoder and the MAT unwrapper, with seeds made from `testdata/truehd`, a dictionary, and a mutator that puts the stream's checks right; `fuzz_differential_truehd_decode` holds the library's strict decode to FFmpeg's sample for sample in `fuzz-differential` (steps that cannot fail the job yet) and `fuzz-nightly`. `tools/checks/check_fuzz_harnesses.py` holds the lists in the static job. The signing and protected-MP4 harnesses have grown seeds. Ten-minute runs of each under Clang 18 found three things, all in `libs/truehd/ERRATA.md` (question 40, entry 42 with question 41, question 42); `tools/fuzz/README.md` has the table. |

Open, for the owner:

1. Should the vcpkg port and the Conan recipe offer `truehd` (done, off by default), or pin it off until a program that ships needs it?
2. Should TrueHD be a column of the README badges and the performance cards? It is now; accuracy and memory read "no data" for it, as AC-4's did.
3. `ICLFORGE_BUILD_TRUEHD=OFF` with tests on is supported by the CMake files (the C API's absent file and its test, and the stand-ins of the four performance executables) and built by no lane. Build it in
   one, or refuse it in the root as `ICLFORGE_BUILD_AC4=OFF` is?
4. The AC-4 and TrueHD libraries are outside clang-tidy's list. Add them (a first nightly with findings), or leave them to CodeQL and Sonar?
5. Forge writes the TrueHD WAV through `std::ofstream` with a narrow path, as the rest of the CLI writes its outputs; Windows reads a narrow path in the ANSI code page, so a non-ASCII path needs a wide one. Is that a fix for all of the CLI's outputs, or for none?

Open from the fuzzing task, for the owner (the first three are `libs/truehd/ERRATA.md` questions 40 to 42, with their evidence):

6. An MLP stream that declares 16-bit words but codes a bit below them: the library returns the 24-bit value, FFmpeg 16 bits (question 40).
7. A `ch_assign` that repeats a value: refused, where FFmpeg gives the unassigned output matrix channel 0 (question 41; the direction of the field is entry 42, found by the same harness and fixed).
8. A major sync that changes the channel assignment words: the library resets the filter histories, FFmpeg keeps them (question 42, a corner of question 18).
9. `fuzz_iamf_parse` is built and has no seeds and no place in the run list, like the TrueHD harnesses were. `check_fuzz_harnesses.py` names it as a known gap. Give it seeds (the IAMF examples make streams) and a place in `BASE_TARGETS`?
10. The TrueHD differential harness is in the required `fuzz-differential` job in steps that cannot fail it, because it has not run against the FFmpeg of the runners (8.0.1). Promote it after a clean week, or leave it to the nightly job?
11. Should a stream with a cyclic `ch_assign` join `testdata/truehd/handbuilt`? The decoder's tests build one, and FFmpeg's strict decode agrees with it, but the fixtures are the fixtures task's.

Open from WP7, library surfaces and the remaining CI lanes (2026-10-31; `planning/work-packages/WP7-library-surfaces-and-ci.md`; the numbers are its block of questions):

110. The three floors of `coverage_report.sh -q` (`apps/forge/gui/src`, `apps/crucible/ui/src`, `apps/hearth/ui/src`, 40 line and 25 branch each) are unmeasured: no Linux run was possible where they were set. Recommended: dispatch the `coverage-qt` job once on the branch, read the three figures from its log, and set each floor four to six points under, as every other row was.
111. Is `coverage-qt` nightly only (as built) or also in the run after a merge? It installs Qt and builds three windows instrumented. Recommended: nightly first, then the run after a merge once its time is known; the same for `truehd-off`.
112. Hearth's index over a file read in windows (`apps/hearth/engine/src/truehd_index.hpp`) and `forge cut` do not use the library's `scan_stream` yet. The scan takes the whole stream in memory; Hearth reads through a `ByteSource`. Recommended: give the library a chunked reader first (audit row Co12), then move Hearth's index onto it with WP2's agreement, so that one implementation is left; do not change `apps/shared/media` until then.
113. Only `libs/truehd` joined the nightly clang-tidy scope. Under LLVM 22.1.2 (the pin) its sources had no report except four missing includes in the AVX2 kernel (`internal/avx2/coded_bits_avx2.cpp`, the performance package's), which stays out of the scope until it adds `<cstdint>` and `<span>`. `libs/ac4/src` gave 1,275 reports in 74 files (1,196 `misc-include-cleaner`, 47 `bugprone-unchecked-optional-access`, 10 `performance-move-const-arg`, 9 `performance-inefficient-vector-operation`, 13 others), and putting it in would turn the nightly red at once. Recommended: fix the includes with `clang-tidy -fix` in a change of its own, look at the 47 optional accesses by hand, then add `libs/ac4`.
114. The TrueHD differential fuzz harness is still in steps that cannot fail the job. A clean week against FFmpeg 8.0.1 cannot be shown from here. Recommended: promote it after the first week of green `fuzz-differential` runs on main, in a change of its own.
115. `fuzz_iamf_parse` needs seeds and a place in `BASE_TARGETS`, and a stream with a cyclic `ch_assign` needs a `handbuilt` fixture. Neither could be made without a writer that produces the bytes (a small C++ generator for IAMF; `tools/references/truehd_writer.py` for the stream, whose manifest records FFmpeg 6.1.1 and would be regenerated under 8.0.1). Recommended: a generator under `tools/generators` for the IAMF seeds, and regenerate the handbuilt manifest only for the new entry, leaving the other 26 as they are.
116. The ABI allowlists of `containers` and `ac4` differ from the libraries (audit note). The comparison needs a Linux shared build (`nm -D`), which this session did not have. Recommended: run `tools/ci/check_abi_symbols.py` on a CI-matching build, report the diff, and fix the cause (a missing export macro or a leaked template) before touching the list.
117. On main since 2026-10-30 the Linux and macOS LLVM legs fail one case each, "the instrument sees a key left in a freed block", and the Windows LLVM leg failed to build TrueHD (fixed on `chore/truehd-ci-housekeeping`). The first is not TrueHD's; it sits with the crypto work of PR #1699. Recommended: that session reads it.
118. The Python binding exposes decode and scan, not the TrueHD encoder, the Atmos layer or the metadata edit. Recommended: bind the encoder next with the same tests, then Rust, then WebAssembly; the encoder's configuration is the only part that needs a decision (the channel meaning, Me1).
119. Wide paths for non-ASCII output on Windows (`std::ofstream` with a narrow path) remains a CLI-wide decision, not TrueHD's; recommended: ask, do not change in one command.


## Not in scope here

Encrypted or signed content (the AC-4 workstream's signing machinery is the intended carrier for TH spec §22; it is not
used here), the bed/object renderer, 44.1 kHz-family Atmos (the specification allows 48 kHz only), and anything that
analyses a Dolby-made stream.
