# TrueHD and DD+ parity: the library

!!! note "Status as of 2026-10-29: an audit and a plan. Nothing here is built."
    The owner wants Dolby TrueHD and Dolby Digital Plus (DD+, E-AC-3; AC-3 and the JOC/Atmos layer ride with it) to be equal
    first-class parts of the repository. This page covers the library surface, everything below the applications:
    `libs/ac3` against `libs/truehd`, the C API, the bindings, the container code, the test data, the checks and CI lanes, and
    the documentation pages. The applications (`forge`, `forge-gui`, `crucible`, `hearth`) are audited on their own pages.
    Round 1 changes no code. Round 2 is the owner's to decide after reading section 6.

Names used below. **DD+** is E-AC-3 and its AC-3 core. **TH-ERRATA n** is open question n of
[libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md); **CT-ERRATA n** is open question n of
[libs/containers/ERRATA.md](../libs/containers/ERRATA.md); **Qn** is a question in section 6 of this page. Row IDs are a prefix and a number: Co (the codec libraries), Me (metadata),
At (Atmos), Ca (C API), Bi (bindings), Cn (containers and carriage), Ci (data, checks and CI), Do (documentation). The TrueHD
plan is [planning/truehd.md](truehd.md); what ships is in [docs/library/capabilities.md](../docs/library/capabilities.md).

## 1. Basis

Commit `523cf5dc3a3d5996fa5d8d3f66df8c5e5b3b6989` (branch `integ`, audited in a worktree of branch `parity-lib`). The
owner's TrueHD specification is not in the tree, so no row below says what that specification contains; where a row turns on
it, the row says "I could not tell" or points to a question.

**What was run** (the evidence column names these by their short tag):

| Tag | What |
|---|---|
| `ctest` | `ctest -N -L <label>` and `ctest --show-only=json-v1` in the existing build of this commit (4,049 tests; `ICLFORGE_BUILD_TRUEHD` on; ADM, GUI, Hearth, Python and fuzzers off). Counts are Catch2 test cases, whose size differs from file to file, so they show where tests exist and do not compare effort. No test body was run: every agent shares that build's scratch directory. |
| `abi` | A script that reads every `ICLFORGE_C_EXPORT` function in `libs/capi/include/iclforge_c/iclforge.h` and compares it with `tools/ci/abi-allowlist/libiclforge_c.so.txt`: 366 declared, 366 listed, no difference. The shared-library check itself (`tools/ci/check_abi_symbols.py` against a built `.so`) was not run: there is no shared build here. |
| `matrix-cov` | `python3 tools/checks/check_matrix_coverage.py --cli <forge>`: all eight checks pass (41 commands, 18 AC-4 commands, 4 AC-3 and 8 E-AC-3 layouts, 9 tool tokens, 2 Atmos modes, VBR and ABR). |
| `fuzz-list` | A script listing every `libs/*/fuzz/fuzz_*.cpp` against `BASE_TARGETS` in `tools/fuzz/run.sh` and counting each harness's seeds and regressions. |
| `probe` | `forge probe` (text, `detail=blocks`, `json=1`) on `testdata/truehd/truehd_m51_s24.thd` and `testdata/external-baseline/eac3-51-256/dee.ec3`. |
| `noexc` | The four `libs/truehd` sources `decoder.cpp`, `major_sync.cpp`, `syntax.cpp` and `encoder.cpp` compiled with their build flags plus `-fsyntax-only -fno-exceptions`: all exit 0. |
| `grep` | A named `grep` or `wc` over the tree; the count is quoted. |
| read only | I opened the file and did not run anything. |

**Not run:** any test body, libFuzzer (the tree builds with GCC only here), the shared-library ABI check, the Python, Rust and
JavaScript builds, the ADM bridge (it needs `ICLFORGE_BUILD_ADM`, off in this build), and every hardware path.

**Label counts per codec** (`ctest -N -L`, one label per row; the same test can carry several labels):

| Label | Tests | Where they are |
|---|---|---|
| `ac3` | 1,030 | 1,026 in `iclforge-ac3-tests`, 4 in `iclforge-containers-tests` |
| `eac3` | 238 | 194 ac3, 17 capi, 14 app-media, 9 forge-cli, 4 containers |
| `joc` / `oamd` / `atmos` | 25 / 32 / 68 | `atmos`: 36 ac3, 29 forge-cli, 3 capi |
| `signing` / `hmac` | 26 / 1 | |
| `karaoke` | 84 | all codecs |
| `truehd` | 138 | 85 in `iclforge-truehd-tests`, 26 containers, 18 capi, 9 forge-cli |
| `ac4` | 771 | for scale |
| `capi` | 102 | the whole C API test binary |

## 2. The matrix

One row per capability DD+ has in this area. Gap: **none**, **partial**, **missing**, or **n/a** with the reason. Size is the
ROADMAP's scale: S an afternoon, M a day or two, L a focused week, XL several PRs. "Blocked by" is an owner question
(TH-ERRATA, CT-ERRATA or Qn), another row, or nothing. The TrueHD cells say "no oracle" where no independent decoder reads
the feature, as `libs/truehd/ERRATA.md` does.

### 2.1 The codec libraries (`libs/ac3`, `libs/truehd`)

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| Co1 | Bitstream syntax, read and written | `libs/ac3/src/core`, `libs/ac3/src/io/elementary.cpp`, `libs/ac3/src/emdf` | `libs/truehd/src/core/syntax.cpp`, `libs/truehd/src/io/access_unit.cpp`, `libs/truehd/src/io/elementary.cpp`, `libs/truehd/src/io/major_sync.cpp` | none | - | - | `ctest`: `ac3` 1,030; `truehd` 138 (85 in the library's own binary) |
| Co2 | Decoder tool coverage | Every Annex E tool, to 7.1.4 (`docs/library/capabilities.md`, Decoding) | Substreams 0 to 2 and a generic 16-channel substream 3; noise generators 0 and 1 decoded; heavy DRC not read (`libs/truehd/include/iclforge/truehd/decoder/decoder.hpp`) | partial | M | TH-ERRATA 10 | read only |
| Co3 | Decoder oracle depth | FFmpeg strict decode, Dolby Reference Player, DEE streams (`testdata/external-baseline/`: 7 E-AC-3, 2 AC-3), 8 pinned third-party samples (`tools/checks/verify_fate_interop.py`) | FFmpeg strict decode on 12 fixtures and 12 hand-built streams (`testdata/truehd/`), the Python transcription `tools/references/truehd_syntax.py`; DEE legs not scored; no third-party commercial stream | partial | M | Q1, Q2 | `grep`: `ls testdata/external-baseline` 7 `eac3-`, 2 `ac3-`; `SAMPLES` in `verify_fate_interop.py`: 3 AC-3, 5 E-AC-3, 0 TrueHD |
| Co4 | Encoder breadth | AC-3 CBR; E-AC-3 CBR, VBR, ABR; 1+1 to 7.1.4; Annex E tools (`libs/ac3/src/encoder`) | 1 to 8 channels, 44.1 to 192 kHz, 16/20/24 bits, downmix presentations, FIR to order 8 (`libs/truehd/src/encoder`). IIR, LSB bypass, negative output shift, heavy DRC, noise and MLP are refused with a reason | partial | L | TH-ERRATA 7, 8, 10, 11; none of them has an oracle | `grep`: `EncoderConfig::kind` comment "only kTrueHd is written"; capabilities.md TrueHD table |
| Co5 | Encoder efficiency measured in CI | `tools/ci/quality_race.py` against FFmpeg and DEE; trend in `docs/quality-trend.md` | `tools/checks/verify_truehd_encode_interop.py` records the size beside FFmpeg's (0.95 times, per planning/truehd.md); `--max-size-ratio` is passed by no workflow or script | partial | S | nothing | `grep -rn max-size-ratio` over the tree: only the script that defines it |
| Co6 | Encode-time self-check | `libs/ac3/src/verify` (mirror, selfcheck; 34 cases) | `EncoderConfig::verify` decodes every unit and compares | none | - | - | read only |
| Co7 | Lost data and concealment | `ConcealmentPolicy` repeat-fade, mute, bed-only (`decoder.hpp`); `libs/ac3/tests/decoder/test_concealment.cpp`, 9 cases | `Decoder::lost_data()` and the next restart header; until then no samples and no silence, so the PCM is shorter than the stream | partial | M | Q9 | read only (`decoder.hpp` comment on `Strictness`) |
| Co8 | Diagnostic events | `DiagnosticSink` (`decoder/diagnostics.hpp`), 7 cases | `DecoderCounters`, seven counts; no callback | partial | S | nothing | read only |
| Co9 | Per-block syntax trace and dump | `FrameSyntax` (`decoder/syntax_trace.hpp`), `verify/trace_export.hpp`, `forge probe detail=blocks` | None in the library; the Python transcription is the only second reader of the fields. `forge probe detail=blocks` on a TrueHD file prints the same 7 lines as without it | missing | M | nothing | `probe`: outputs identical |
| Co10 | Stream description (`probe`) | `iclforge::ac3::io::Prober`, versioned JSON (`io/probe.hpp`); 90 leaf fields in the JSON | `probe_stream` and `StreamInfo` (`io/elementary.hpp`); 18 leaf fields, none of them dialnorm, DRC or mix level | partial | M | Me1 | `probe` |
| Co11 | Scan, timing, duration, seek | `ScannedStream`, `access_unit_timing`, `stream_duration_*`, `access_unit_at_sample` and `_at_seconds` (`io/elementary.hpp`) | `scan_stream` (`io/scan.hpp`, 2026-10-31): `ScannedStream` with units, samples, padding, duration, skipped and leading bytes, the restarts, `unit_at_sample`, `restart_for_sample`, `sample_at_seconds`; 8 cases on every fixture. The restart interval is the seek granule (section 3). Hearth's own index (`truehd_index.hpp`) and `forge cut` do not use it yet | partial | S | WP2 for Hearth's index | `ctest`: `[scan]` |
| Co12 | Chunked (streaming) input | `AccessUnitAccumulator` (`io/stream_accumulator.hpp`), 8 cases | `ElementaryReader` takes one `std::span` of the whole stream | missing | S | nothing | read only |
| Co13 | Metadata rewrite in place (`forge metadata`, `normalize`) | `io/metadata_edit.hpp`: dialnorm, compr, bsmod, dsurmod, CRC re-stamp; 19 cases | None. `major_sync_crc` exists (`core/crc.hpp`), so a rewrite is possible | missing | M | Me1; TH-ERRATA 2 for a major sync with the 16-channel extension | `grep`: no `edit` in `libs/truehd/include` |
| Co14 | Cut and join on unit boundaries | Any access unit (`forge cut`, `cat`; `extract_programme`) | No library primitive; a cut has to start at a unit that carries a major sync (`starts_with_major_sync`) | missing | S | Co11 | read only |
| Co15 | WAV channel order and mask | `io/wav.cpp` (`ac3_layout_for`, `wav_channel_order`), 21 cases | `forge decode` writes the stream's channel order; mask to channel assignment is this work's own reading | partial | S | TH-ERRATA 16 | read only |
| Co16 | Loudness and QC against the stream's own value | `meta/loudness.cpp`, `meta/qc.cpp`: BS.1770-4, EBU R 128, A/85, Netflix presets, compared with embedded dialnorm and compr | The meters are codec-blind and `forge loudness` runs on TrueHD in the matrix; QC refuses TrueHD; the embedded value is not exposed | partial | M | Me1; TH-ERRATA 17 | `grep` `run_codec_matrix.sh` lines 1467 and 1544 |
| Co17 | Build variants and small targets | Decode scalars fixed32, float32, float64; encode float32, float64; minimal profile without exceptions; AVX2 and NEON; ESP32 and bare-metal builds (`libs/ac3/variants`, `minimal.cmake`) | The decoder has no floating point and no `throw`; the four sources pass `-fno-exceptions`. No profile target, and no firmware project lists `truehd` | n/a until Q8 is answered | - | Q8 | `noexc`: 4 of 4 exit 0; `grep -c "double\|float" libs/truehd/src/decoder/decoder.cpp` 0 |

### 2.2 Metadata and the flags a TrueHD user would expect

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| Me1 | Dialogue normalisation and the channel-meaning fields | Read, written (`dialnorm=`, measured), edited, normalised, applied by the output stage, shown by `probe`, in the C API (`iclforge_decoded_*_dialnorm`); `ctest -L dialnorm` 10 | `ChannelMeaning` in `io/major_sync.hpp`: dialogue norm and mix level for 2, 6, 8 and 16 channels, source formats, start-up gains. Parsed and written; `Decoder::major_sync()` returns it; `EncoderConfig::channel_meaning` writes it (zero by default). Used nowhere else: no C API field, no CLI option, not in `probe`, not applied, not edited | partial | M | nothing (exposure); Q3 (applying) | `grep -rn channel_meaning apps libs/capi bindings`: 0 hits; `probe` JSON: 0 matches for dialnorm, drc, mix_level |
| Me2 | Dynamic range control | `dynrng`, `compr`, five encoder profiles, `drc_scale`, `drc_boost_scale`, `heavy_compression`, line and RF modes; `libs/ac3/tests/meta/test_drc.cpp` 35 cases, `test_dynamic_range.cpp` 7; `ctest -L drc` 40 | The per-unit DRC word (`drc_gain_update`, `drc_time_update`) and the start-up gains are read and carried, never applied; the encoder writes none; heavy DRC is not in the restart header the library reads. The word has no oracle | partial | L | TH-ERRATA 10; Q3, Q4 | `ctest`; `libs/truehd/ERRATA.md` entry 18 text |
| Me3 | Downmix coefficients and levels | `cmixlev`, `surmixlev`, Lo/Ro, Lt/Rt, preferred downmix, LFE mix; folded by the decoder and checked against FFmpeg `-ac 2` (capabilities.md, Decoding); `ctest -L downmix` 13 | The lower presentations are substreams of their own; the encoder writes them (`DownmixMode::kStandard`, `DownmixGains`, `stereo_mode`); the decoder picks one (`Presentation`) and folds nothing; the mix levels are not read out. "Nothing checks the downmix itself" (capabilities.md) | partial | L | Q3; TH-ERRATA 13 | read only |
| Me4 | Output stage (opt-in, off by default) | `OutputStage`, `OutputConfig` (`decoder/output.hpp`): dialnorm, fold, LFE mix, line and RF modes, Lt/Rt phase shift; `test_output_stage.cpp` 47 cases | None. The neutral parts (fold matrices, dialnorm gain, RF protection) live in `libs/ac3`, which `truehd` may not use (`tools/checks/projects.json`) | missing | L | Q3, Q4; Me1, Me2, Me3 | `grep` `projects.json`: `truehd` may use `base` only |
| Me5 | Dual mono (1+1) | `acmod` 0, `dialnorm2`, `compr2`, `dynrng2`; encoder layout `1+1`; `ctest` names: 22 cases in `iclforge-ac3-tests` | The library, its ERRATA and capabilities.md name nothing like it. The specification is not in the tree | n/a pending Q5 | - | Q5 | `grep -ci dual libs/truehd`: 0 |
| Me6 | Programme mixing and associated services | `mixmdate` (`meta/mixing.cpp`, 20 cases), `AssociatedServiceMixer` (`decoder/associated_service.*`, 17 cases), up to 8 independent substreams, `forge decode associated=`; `ctest -L associated` 37 | The 2, 6 and 8 channel presentations are alternatives built on one another. Nothing in the library names an associated service | n/a pending Q5 | - | Q5 | `grep -ci associated libs/truehd`: 0 |
| Me7 | Karaoke (Annex C) | `OutputConfig::karaoke`, `is_karaoke_programme`, `test_eac3_karaoke.cpp` 10 cases; `ctest -L karaoke` 84 (all codecs) | Nothing in the library | n/a pending Q5 | - | Q5 | `grep -ci karaoke libs/truehd`: 0 |
| Me8 | ADM/BWF export | `forge decode in.ec3 out.wav objects_dir adm_out` and `iclforge::adm::write_bw64` (`libs/adm`); not built in the audited tree | None. A bed-only ADM from a TrueHD bed needs no Atmos work; objects wait on the Atmos rows | missing | M (bed only) | At4, At5 for objects; Q14 | read only (`ICLFORGE_BUILD_ADM` off) |
| Me9 | Service and production metadata | `bsmod`, `dsurmod`, `dsurexmod`, headphone flag, copyright, language, time code (`meta/bsi.cpp`, 36 cases) | `stereo_mode` and the 6 and 8 channel source formats are in `ChannelMeaning`; the repository's TrueHD documents name no other | partial | S | Me1 | read only |

### 2.3 The Atmos layer, row by row

DD+ carries objects as joint object coding (JOC) in an EMDF container, with object audio metadata (OAMD, ETSI TS 103 420).
TrueHD Atmos is on hold behind the owner's open question **TH-ERRATA 1** (what the evolution payload holds on the wire). Every
TrueHD cell below is therefore "not built", and every row is blocked by TH-ERRATA 1; none is planned around. The sizes are
estimates for when the question is answered, not commitments.

| # | Layer | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| At1 | Object audio metadata | `libs/objects` (`oamd.hpp`, `emdf.hpp`); `ctest -L oamd` 32 | Not built; `iclforge::objects` is meant to be reused (planning/truehd.md, T2b) | missing | L | TH-ERRATA 1 | `ctest` |
| At2 | Carrier inside the stream | EMDF frame layout (`libs/ac3/src/emdf`, `test_emdf.cpp` 11 cases) | The extra data after the last substream, carried as opaque bytes (`DecodedAccessUnit::extra_data`; `hb_extra_data.thd`) | missing | L | TH-ERRATA 1, 3, 19 | read only |
| At3 | Object coding | JOC matrices (`libs/ac3/src/oba/joc.cpp`); `ctest -L joc` 25 | The sixteen-channel substream decodes as 16 generic channels; what they mean is not interpreted. FFmpeg refuses its restart sync | missing | XL (with At4, At5) | TH-ERRATA 1, 2 | `ctest`; planning/truehd.md, Oracles |
| At4 | Encoder: bed plus objects | `AtmosEncoder` (`oba/atmos.cpp`), `forge atmos*`, throughput gate in `tests/performance/test_performance.cpp` | None. `EncoderConfig::extra_data` is a hook that frames bytes and never looks inside them | missing | XL | TH-ERRATA 1 | read only |
| At5 | Decoder: objects out | `DecodedAccessUnit::object_audio`, `object_metadata`, `skip_object_reconstruction` (`decoder.hpp`) | None | missing | L | At1, At3 | read only |
| At6 | Channel-based immersive bed | `forge atmos-cbi`, `BedProgram`; `test_atmos_cbi.cpp` 4 cases | The bed is substreams 0 to 2, up to 8 channels. Which immersive bed layouts the format defines: I could not tell | missing | L | TH-ERRATA 1; Q13 | read only |
| At7 | Authenticity tag | `iclforge::ac3::signing`: HMAC-SHA-256 per frame, sign, verify, keyring, gate (`signing/emdf_atmos_signer.hpp`); `ctest -L signing` 26; `fuzz_signing_verify` with 5 seeds and 3 regressions | Not built. The evolution container's signing scope is unconfirmed; `iclforge::base::crypto` (codec-neutral) is available | missing | L | TH-ERRATA 1 | `ctest`; `fuzz-list`; docs/concepts/object-signing.md |
| At8 | Strip objects to a plain bed | `io/object_strip.cpp`, `forge strip-objects`; 7 cases | None | missing | M | At2 | `ctest` |
| At9 | Authoring bridges and scene tools | ADM to JOC (`libs/adm`), IAB to JOC (`libs/iab/include/iclforge/iab/ac3iab.hpp`), JOC to ADM, scene files, motion, OSC (`libs/objects`, codec-neutral) | None; the scene tools would be shared | missing | M | At4 | read only |
| At10 | Signalling in containers | MP4 `dec3` Atmos extension, HLS `CHANNELS="n/JOC"`, DASH (`test_fmp4.cpp`, 25 cases) | `has_object_substream` is read. What MP4, Matroska or TS carry for TrueHD Atmos: nothing in `libs/containers/ERRATA.md`; I could not tell | missing | M | TH-ERRATA 1; Q13 | read only |
| At11 | Reporting in `probe` | OAMD/JOC complexity index, object and bed configuration | `object_substream`, a boolean | partial | S | At2 | `probe` |
| At12 | C API and bindings for objects | 8 `iclforge_atmos_*` functions and the object accessors; Python `test_eac3_atmos.py`; Rust `bindings/rust/iclforge/src/atmos.rs` | None | missing | M | At4, At5 | `abi`; read only |
| At13 | Oracles | FFmpeg reads the bed; Dolby's parser reports `atmos=true`; DEE JOC fixture (`test_dee_joc_fixture.cpp`, 6 cases) | None. `gen_dee_gold.py` has `obj-thd-atmos-*` legs marked `refusal_ok`; whether DEE accepts them for TrueHD is not known | missing | S (once At4 exists) | TH-ERRATA 1, At4 | read only |

### 2.4 The C API (`libs/capi`)

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| Ca1 | Decode and encode entry points | `iclforge_decoder_*` (7), `iclforge_eac3_*` (27), `iclforge_atmos_*` (8), `iclforge_encoder_*` (7) | `iclforge_truehd_*` (43): decoder, scan, encoder, encoded units | none | - | - | `abi`: 366 declared, 366 in the allowlist |
| Ca2 | Metadata accessors on a decoded unit | About 80 `iclforge_decoded_*` functions: dialnorm, compr, dynrng, bsmod, layout, objects | `iclforge_truehd_decoded_*`: 9 accessors (rate, channels, samples, substream, restarted, padding, pcm, extra data, channel name) and a destroy; `iclforge_truehd_stream_info_t` has 10 fields; no dialnorm, mix level or DRC | partial | S | Me1 | `abi`; read only |
| Ca3 | Scan, timing, seek | 44 `iclforge_scanned_stream_*`, including `access_unit_at_sample` | 5 `iclforge_truehd_units_*`: offsets, sizes, skipped bytes; 15 `iclforge_truehd_scanned_stream_*` (2026-10-31): rate, units, samples, padding, duration, skipped bytes, restarts, `restart_for_sample`, `sample_at_seconds` | done | - | - | `abi`; `[capi][truehd]` |
| Ca4 | Encoder configuration | `iclforge_eac3_frame_metadata_init` and the frame configuration carry the metadata | `iclforge_truehd_encoder_config_t` has no channel meaning (Me1), no experimental tools, no extra-data source | partial | S | Me1 | read only |
| Ca5 | Metering and QC | `iclforge_loudness_meter_*` (15), `iclforge_level_meter_*` (11), `iclforge_qc_*` (4): codec-blind, usable on TrueHD PCM | same functions | none | - | - | `abi` |
| Ca6 | ABI allowlists | `libiclforge_c.so.txt` 366 lines; `libiclforge_ac3.so.txt` 410 | `libiclforge_truehd.so.txt` 50 lines; `libiclforge_c.so.txt` includes the 43 | none | - | - | `abi` (header against allowlist only) |
| Ca7 | Examples | C: `capi_encode_decode.c`, `capi_encode_eac3.c`; C++: `decode_stream`, `encode_eac3`, `metadata`, `stream_edit`, `mux_mp4`, `mux_mkv`, `mux_ts`, `mux_fmp4` and more | C: `capi_decode_truehd.c`, `capi_encode_truehd.c`. No C++ example for decode, encode, containers or stream tools | partial | M | nothing | `ctest`: `example.capi_decode_truehd` and `example.capi_encode_truehd` exist; `ls examples` |
| Ca8 | Installed package and packaging | `iclforge-ac3.pc`, `ac4` feature in the vcpkg port and the Conan recipe, an install consumer for the C API and for AC-4 (`tools/checks/check_install_consumer.sh`) | `iclforge-truehd.pc` installs; no vcpkg or Conan feature (a comment in `cmake/InstallLibrary.cmake` says "until a program that ships links it"; `forge` links it now); the install consumer names no TrueHD; `iclforge-c.pc` describes "AC-3, E-AC-3 and AC-4" and its static requires omit `iclforge-truehd` | partial | S | Q6 | `grep -c -i truehd tools/checks/check_install_consumer.sh`: 0; `packaging/`: 0 hits; `pkgconfig/iclforge-c.pc` read |
| Ca9 | C API documentation | `docs/library/c-api.md` | A TrueHD section and an encoding section; 29 of the 43 function names appear (the same count is 59 of 188 for the AC-3 side) | none | - | - | `grep` over `c-api.md` |

### 2.5 The bindings

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| Bi1 | Python | `bindings/python/src/iclforge_ext/bindings.cpp` over the C++ API, optional modules for AC-4, containers, lyrics, signing; 119 test functions, 18 for AC-4; `docs/library/python-api.md` | Nothing. The `python` row in `projects.json` does not list `truehd` | missing | L | Q7 | `grep -rli "truehd\|mlp" bindings/python`: none |
| Bi2 | JavaScript and WebAssembly | `bindings/js/src` push decoder, `ac4.ts`; 104 test calls, 20 for AC-4; the WebAssembly modules are built in `apps/demos/wasm` | Nothing. `demo-wasm` and `js` do not list `truehd` | missing | L | Q7 | `grep`: none |
| Bi3 | Rust | `bindings/rust/iclforge/src`: `ac3`, `eac3`, `atmos`, `ac4`, `meter`, `stream`; 91 tests, 13 for AC-4; `docs/library/rust-api.md` | `iclforge-sys` generates raw declarations from the header with bindgen, so `iclforge_truehd_*` is reachable unsafely when the C library is built with TrueHD (default on). The safe crate has no TrueHD module | partial | M | Q7 | read only (`build.rs`, `lib.rs`); not built |

### 2.6 Containers and carriage (`libs/containers`, `libs/audio`)

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| Cn1 | MP4 | `ac-3` and `ec-3` with `dac3`, `dec3` built from the stream (`libs/ac3/src/io/dec3.cpp`), reader and seekable reader; `ctest -L mp4` 60 | `mlpa` and `dmlp` (`mp4.hpp`), reader; `test_mp4_truehd.cpp` 9 cases. The layout is FFmpeg's only | none | - | CT-ERRATA 1 | `ctest` |
| Cn2 | Matroska | `A_AC3`, `A_EAC3`; `ctest -L matroska` 30 | `A_TRUEHD`; `test_matroska_truehd.cpp` 5 cases. Layout is FFmpeg's only | none | - | CT-ERRATA 6 | `ctest` |
| Cn3 | MPEG-TS | DVB (EN 300 468 Annex D) and ATSC descriptors, `mainid`, `asvc`; `ctest -L mpegts` 41 | Blu-ray `stream_type` 0x83 with the `HDMV` registration; 188-byte packets written, 188 and 192 read; no M2TS writer; DVB and ATSC refused | partial | M | CT-ERRATA 4, 5, 8 | `ctest`; read only |
| Cn4 | IEC 61937 bursts | AC-3 and E-AC-3, read and written, byte-exact with FFmpeg's `spdif` and checked against Microsoft's document; `ctest -L iec61937` 61 | MAT (data type 22): written byte-exact with FFmpeg's `spdif`; reading has no oracle because FFmpeg cannot read MAT; the last partial burst is padded where FFmpeg drops it; the WAV carrier is provisional; `ctest -L mat` 6 | partial | S | CT-ERRATA 2, 3, 7 | `ctest`; `libs/containers/ERRATA.md` M1 to M7 |
| Cn5 | Passthrough output to a receiver | `iclforge::audio::PassthroughSink`: AC-3, E-AC-3 (and AC-4) on WASAPI, ALSA, PipeWire, CoreAudio and Android; hardware-confirmed on Windows and a Raspberry Pi | `BitstreamFormat` has no TrueHD entry (`libs/audio/include/iclforge/audio/passthrough.hpp`); `SinkAudioCapabilities` has `ac3` and `eac3` only; the ELD parser files MAT under "other" (`eld_proc.hpp`) | missing | XL | CT-ERRATA 2; a receiver; Q10 | read only |
| Cn6 | Fragmented MP4, HLS, DASH | `mp4/fragment.cpp`, `hls.cpp`, `dash.cpp`; `test_fmp4.cpp` 25 cases | `fragment.cpp` accepts an `mlpa` track and FFmpeg reads the result; the HLS and DASH helpers name no TrueHD; `forge fmp4` refuses it | n/a pending Q12 (no TrueHD profile in the repository's sources) | - | Q12 | `grep` `fragment.cpp` lines 356 to 359; `run_codec_matrix.sh` line 1544 |
| Cn7 | Protected content | `enca` tracks refused with scheme and key id; Matroska `ContentEncryption` and scrambled TS refused | The same readers; no TrueHD protected fixture | none | - | - | read only |
| Cn8 | Identifying a raw buffer | `ac3::io::scan` (`kNotAC3`), `iclforge_stream_bsid`; container magic in `apps/shared/media/src/container_input.cpp` | `truehd::probe_stream`, `starts_with_major_sync`; no single call names the codec of a buffer, for either family | none | - | - | read only |
| Cn9 | Demuxer fuzz seeds | `fuzz_mp4_demux`: 2 of 9 seeds are named for DD+ (`dolby-ddp-*`); `fuzz_matroska_demux` and `fuzz_mpegts_demux`: 0 of 1 | `fuzz_mp4_demux` 2 of 9, `fuzz_matroska_demux` 1 of 1, `fuzz_mpegts_demux` 1 of 1, `fuzz_mat_unwrap` 1 | partial (DD+ lacks seeds in two harnesses) | S | nothing | `fuzz-list` |

### 2.7 Test data, checks and CI lanes

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| Ci1 | Committed golden masters | `testdata/external-baseline/`; `testdata/bitstream-hashes.json` pins; `drc_goldens.hpp` | `testdata/truehd/`: 12 FFmpeg-made fixtures of 0.25 s each and 12 hand-built streams. No DEE-made TrueHD stream; DEE's licence ends 2026-11-06 | partial | M | Q1 | `grep` `testdata/truehd/manifest.json`: `seconds` 0.25 |
| Ci2 | Generators | `gen_external_baseline.py`, `gen_conformance_vectors.py`, `gen_programme_fixtures.py`, `gen_dee_gold.py` | `gen_truehd_ffmpeg.py`; `gen_dee_gold.py`, `truehd` group | none | - | - | read only |
| Ci3 | Third-party corpus | 8 pinned samples, run on a schedule (`.github/workflows/interop.yml`) | None | missing | S | Q2 | `grep`: `SAMPLES` has no TrueHD |
| Ci4 | Codec matrix | `tools/ci/run_codec_matrix.sh`: 203 `run` lines in the AC-3 and E-AC-3 sections (lines 169 to 1178), each also held to FFmpeg's strict decode where FFmpeg can read it | Two sections, lines 1398 to 1683: 32 `run` lines, FFmpeg bit-exact checks in both directions | none | - | - | a script counting `run` lines under each section header |
| Ci5 | Matrix coverage check | `check_matrix_coverage.py` enforces every command, layout, tool token, Atmos mode and VBR/ABR, and, for AC-4, that every command naming an AC-4 file is run on one | No TrueHD rule. 10 usage rows name a `.thd` or `.mlp` file (`truehd-encode`, `decode`, `levels`, `loudness`, `spdif`, `unspdif`, `mkv`, `mp4`, `ts`, `demux`) and the matrix runs all 10 today; nothing makes it keep doing so. `truehd-encode`'s rates, widths and channel counts are not enumerated | partial | S | nothing | `matrix-cov`: passes; read only for the matrix |
| Ci6 | FFmpeg legs in CI | Gold-reference gate, matrix, quality race, differential fuzz, scheduled third-party samples | Five steps in `_ci-core.yml` (decode, hand-built, encoder, encoder input space, containers) and the gold-reference lines 603 to 633 of `verify_gold_reference.sh` | none | - | - | read only |
| Ci7 | libFuzzer harnesses run in CI | `BASE_TARGETS` in `tools/fuzz/run.sh` lists 23; all DD+ harnesses are in it; the two differential harnesses run in their own job | `fuzz_truehd_parse`, `fuzz_truehd_decode`, `fuzz_truehd_encode` and `fuzz_mat_unwrap` are built and in no run list, so `fuzz-regress`, `fuzz-short` and `fuzz-nightly` never start them. `libs/truehd/fuzz/` has no `seeds/` or `regressions/` | missing | S | nothing | `fuzz-list`: 4 harnesses, `in_run_list=False`, 0 seeds for the 3 TrueHD ones |
| Ci8 | Differential fuzz against FFmpeg | `fuzz_differential_ac3_decode`, `fuzz_differential_eac3_decode`, `differential_oracle.hpp` | None; a lossless decode makes the comparison exact | missing | M | Ci7 | `fuzz-list` |
| Ci9 | Encoder input-space fuzz | `fuzz_encoder_space.py`, `fuzz_eac3_encoder_space.py` | `fuzz_truehd_encoder_space.py`: regressions per PR, 900 s nightly budget | none | - | - | read only (`fuzz.yml` line 532) |
| Ci10 | Sanitizer lanes | The ASan and UBSan leg runs the codec matrix; the TSan preset runs the `concurrency` label | The matrix's TrueHD sections run on the sanitizer leg; no TrueHD test carries `concurrency` | none | - | - | `ctest`: 107 `concurrency` tests, 0 with the `truehd` label; read only (`.github/workflows/_ci-linux.yml`) |
| Ci11 | Coverage gate | `tools/checks/coverage_report.sh`: 13 library rows and 4 application rows with floors | `libs/truehd` is in neither the `--filter` pattern (line 240) nor the floors | missing | S | nothing | `grep libs/truehd tools/checks/coverage_report.sh`: no hit |
| Ci12 | Performance and memory gates | `iclforge-perf`: 6 DD+ cases and 4 AC-4; `bench_encoder`, `bench_memory`, `kernel_bench`; merge-queue comparison and trend files | No TrueHD case in any of them | missing | M | nothing | `ctest`: `iclforge-perf` 10 tests; `grep -ci truehd tests/performance`: 0 |
| Ci13 | Cross-platform reproducibility | `bitstream-hashes.json`: 98 pins over x86_64-sse2, aarch64-neon and generic, 19 encoder streams per tier at most | The decoder has no floating point, so its PCM does not depend on the platform. The encoder uses `double` for predictor choice (`coding.cpp`, 26 lines), so its bytes may differ by compiler; not pinned | partial | S | Q11 | `grep -c "double\|float"`: decoder.cpp 0, coding.cpp 26 |
| Ci14 | Layering and namespaces | `projects.json` rows for `ac3`, `ac4`; `namespaces.json` | `truehd` is in both. Projects that would need `truehd` in `may_use` for the rows above: `python`, `demo-wasm`, `js`, `sink`, `hearth`, `crucible`, `app-media`, `app-project`, `esp-idf`, `baremetal` (today only `capi` and `forge` list it) | n/a: each edit rides with its feature row | - | the feature rows | `python3` over `projects.json` |
| Ci15 | Threat model | `docs/threat-model.md`: a trust-boundary table with the fuzz harness of each parser; a resource-limits section for AC-3, E-AC-3 and AC-4 | No TrueHD row, and no limits section | missing | M | Ci7 | `grep -ci truehd docs/threat-model.md`: 0 |

### 2.8 Documentation

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| Do1 | `capabilities.md` and `verification.md` | Full | A TrueHD table (12 rows) and a scoring section; 9 and 32 mentions | none | - | - | `grep -ci` |
| Do2 | `development-status.md` | Rows per feature | TrueHD block present; line 216 says the carriage (MAT, IEC 61937) is phase T2c, but MAT is built; line 419 says "eleven questions", `libs/truehd/ERRATA.md` has 19 | partial | S | nothing | read only |
| Do3 | ROADMAP and object-signing page | Rows current | `ROADMAP.md` lines 106 to 111 ("Authenticity note (for that branch)") and `docs/concepts/object-signing.md` line 243 still name `feature/truehd-atmos-support` as the home of the TrueHD seam, which planning/truehd.md calls superseded | partial | S | nothing | `grep -n "feature/truehd-atmos-support"` |
| Do4 | Library pages that name the codec | `decoding.md`, `metadata.md`, `spatial-and-atmos.md`, `file-io.md`, `quality.md`, `examples.md`, `header-map.md`, `python-api.md`, `rust-api.md`, `channel-plans-and-routing.md`, `performance-quality.md`, `quality-trend.md`, `threat-model.md` all describe DD+ | The same 13 pages have no mention of TrueHD; `docs-snippets/generated/application-capabilities.md` comes from `docs/assets/data/support-catalogue.json`, which has none either | partial | M | rides with the feature rows | `grep -ci`: 0 on each |
| Do5 | CHANGELOG and README | Current | Current (`CHANGELOG.md` has TrueHD entries under Unreleased; `README.md` line 40 says TrueHD is in the C interface only) | none | - | - | read only |

### 2.9 Counts

80 rows. Size is blank ("no work") for rows with no gap and for the n/a rows.

| Gap | S | M | L | XL | No work | Rows |
|---|---|---|---|---|---|---|
| none | 0 | 0 | 0 | 0 | 17 | 17 |
| partial | 14 | 11 | 3 | 0 | 0 | 28 |
| missing | 6 | 12 | 8 | 3 | 0 | 29 |
| n/a (pending an owner answer) | 0 | 0 | 0 | 0 | 6 | 6 |
| **All** | 20 | 23 | 11 | 3 | 23 | 80 |

12 of the 29 missing rows are Atmos rows (At11 is partial); each is blocked by TH-ERRATA 1, directly or through another Atmos row.

## 3. What TrueHD has that DD+ never needed

These are the library's, in this area, with where each already shows.

- **Lossless verification.** The codec is lossless, so every correctness oracle is equality and none has an SNR floor. The decoder checks each
  restart interval against the next restart header (`DecoderCounters::lossless_check_failures`), strictly or leniently, and the
  encoder can decode what it writes (`EncoderConfig::verify`). `tools/checks/score_truehd_decode.py` and the FFmpeg legs
  (Ci6) are built on it. DD+ has SNR floors instead.
- **Restart-interval seek granularity.** Random access is at a restart header; the encoder default is 128 units, 107 ms at
  48 kHz. A decoder joined mid-stream, or after lost data, produces nothing until the next one. DD+'s seek returns an access
  unit (`access_unit_at_sample`); TrueHD's has to return the restart header before the sample. The library has no seek call
  yet (Co11), and a cut has to start on a unit with a major sync (Co14).
- **MAT carriage (IEC 61937 data type 22).** 24 access units to a 61,440-byte burst, an eight-channel PCM carrier in a WAV, and
  a start, middle and end code read off FFmpeg's output. DD+ uses plain bursts. The burst code is built (Cn4); the output
  path to a receiver is not (Cn5).
- **High and variable bit rates.** A 15-bit peak-rate field, 18 Mbit/s for Blu-ray, a FIFO and a 75 ms limit the encoder
  enforces (`EncoderStats::max_unit_rate_bps`). Muxers, the transport-stream writer and any network sink have to size
  buffers for it. AC-3's table stops at 640 kbit/s (capabilities.md, Encoding).
- **24-bit integer samples and 192 kHz.** The decoder returns interleaved `int32` PCM, 24 bits wide, where DD+ returns planar
  floating point. The C API, the WAV writer and any binding take integer PCM because of that. Rates run to
  192 kHz and channel counts to 16 (substream 3).
- **Presentations as nested substreams.** The 2, 6 and 8 channel presentations are built on one another and a decode reads
  as many as it needs (`Presentation`). DD+ selects a programme among independent substreams.
- **An exact sample count.** An end-of-stream marker counts the padding in the last unit (`padding_samples`), so a decode can
  be gapless without an edit list.
- **A decoder with no floating point.** Its output cannot differ by platform (Ci13), and there are no scalar variants to build
  or compare (Co17).
- **Two stream types.** MLP (DVD-Audio) is read by the same library; it is decoded and carried, not encoded.

## 4. A proposed definition of "first class" for the library

The owner can approve it or change it. A capability counts as done when every box on its row holds.

1. **Every DD+ capability in the matrix has a TrueHD counterpart, or a reason it does not apply that the owner has accepted.**
   The "n/a" rows (Me5 to Me7, Cn6, Co17) are decided by Q5, Q12 and Q8.
2. **Every C++ capability has a C API function, and Rust (over the C API) and Python (over the C++ API) expose it**, or an entry
   in a short list of exceptions with its reason. JavaScript follows Q7.
3. **Metadata is read, reported and writable, and applying it is opt-in.** Dialnorm, mix levels, DRC words and presentations
   reach `probe`, the C API and the bindings; an output stage applies them only when asked, as DD+'s does.
4. **Every feature has an oracle or a label.** FFmpeg's strict decode where it can read the feature, a second reader (the
   Python transcription) otherwise, and the words "no oracle" in capabilities.md where there is none.
5. **Every CI lane that checks DD+ checks TrueHD, or the exemption is written beside the lane:** unit tests under the
   `truehd` label; the codec matrix with a coverage rule like the AC-4 one; libFuzzer harnesses in the run list with seeds and
   regressions; the differential harness; encoder-space fuzz; sanitizers; a coverage floor for `libs/truehd`; speed and memory
   cases; the install consumer; the ABI allowlist.
6. **The data is the same kind:** golden masters made by the reference encoder and by FFmpeg, each with its source and hash;
   a third-party sample set if the owner permits it (Q2).
7. **The packages carry it.** The vcpkg port and the Conan recipe have a `truehd` feature if Q6 says so; the `.pc` files and
   the install consumer agree with the CMake export.
8. **The documentation names TrueHD wherever it names DD+ for the same capability**, and the threat model lists its parsers.
9. **The Atmos rows (At1 to At13) are built in the order section 5 gives, after TH-ERRATA 1 is answered.** Until then they are
   listed as blocked and not worked around.

## 5. The plan

Phases are in dependency order. Sizes are sums of the rows; a phase is a sequence of small PRs, not one. "Needs no decision"
means the work can start now; "waits" names the question.

| Phase | Rows | What | Size | Decision |
|---|---|---|---|---|
| 0. Close the gates | Ci5, Ci7, Ci11, Ci12 (first cases), Co5, Cn9, Ca8 (the `.pc` and the install consumer), Do2, Do3 | Add the four TrueHD harnesses to `BASE_TARGETS` with the 12 hand-built streams and the fixtures as seeds. Add `libs/truehd` to the coverage filter and floors (the first hosted run sets the number). Add the TrueHD rule to `check_matrix_coverage.py`. Add TrueHD cases to `iclforge-perf`. Pass `--max-size-ratio` in CI. Seed the DD+ gaps in the Matroska and MPEG-TS harnesses. Fix the stale lines | M | Needs no decision |
| 1. Surface the metadata | Me1, Me9, Co10, Ca2, Ca4 | Report `ChannelMeaning` in `probe`, the JSON, and the C API; write it from the C API and the encoder options. Add `libs/truehd/README.md` and capabilities.md rows. Where a field has no oracle, say so | M | Needs no decision |
| 2. Stream-level API | Co11, Co12, Co13, Co14, Ca3 | A scanned-stream type with timing and duration, seek to the restart header before a sample, a chunked accumulator, cut and join at restarts, a metadata rewrite that re-stamps the major sync CRC | L | Needs no decision; Co13 follows Phase 1 |
| 3. Bindings | Bi3, Bi1, Bi2, Ca7, Do4 | Rust safe wrapper over the C API first, then Python over the C++ API (as AC-4 does), then JavaScript; the C++ examples; the pages in Do4 | L to XL | Rust and Python can start; JavaScript waits for Q7 |
| 4. Oracle depth | Co3, Ci1, Ci3, Ci8 | Score and keep DEE's TrueHD legs; a differential harness against FFmpeg; third-party samples | M | Waits for Q1 (time-limited) and Q2 |
| 5. Output stage | Me2, Me3, Me4, Co7, Co16, Ci15 | Opt-in dialnorm, DRC and fold for TrueHD; concealment option; QC against the embedded value; the threat-model rows and limits | XL | Waits for Q3, Q4, Q9 |
| 6. Carriage | Cn3, Cn4, Cn5 | Remaining container items and the passthrough path | XL | Waits for CT-ERRATA 2, 3, 4, 7, 8, a receiver, Q10 |
| 7. Packaging | Ca8 (feature), Ci13 | vcpkg and Conan feature; the decode-hash gate | S | Waits for Q6, Q11 |
| 8. Atmos | At1 to At13 | In this order once TH-ERRATA 1 is answered: carrier (At2), metadata (At1), signing (At7), decode of the 16 channels as objects (At3, At5), encoder (At4), bed (At6), strip (At8), bridges (At9), signalling (At10), reporting (At11), C API and bindings (At12), oracle (At13) | XL | Blocked by TH-ERRATA 1; Q13 |

**What proves each phase done.** Phase 0: the four harnesses appear in `tools/fuzz/run.sh`'s list and `fuzz-regress` replays
their seeds; `coverage_report.sh` prints a `libs/truehd` row; `check_matrix_coverage.py` fails when a TrueHD command is
dropped from the matrix (shown by removing one in a local copy); `iclforge-perf` has TrueHD cases. Phase 1: a hand-built
stream with non-zero channel-meaning fields is read through the C API and `forge probe json=1`, and the Python transcription
agrees (the second reader, as there is no FFmpeg oracle). Phase 2: decoding from the restart header before sample N gives the
tail of the full decode; the accumulator gives the same units for every chunking. Phase 3: each binding decodes and encodes
the fixtures to FFmpeg's hashes; the pages are linked from the index. Phase 4: `score_truehd_decode.py` passes on the DEE set;
the differential harness runs one nightly budget clean. Phase 5: with the stage off, output is bit-identical to today's; with
it on, a test per field against a worked example. Phases 6 to 8: the checks of the rows they close.

**Risks.** A harness that has never run may find crashes in the TrueHD reader and decoder: that is the point, and it makes
Phase 0 larger. A coverage floor set from a first run is only as good as the tests then. The output stage (Phase 5) has no
oracle for DRC or for the fold and shares code with `libs/ac3`, which `truehd` may not link: the choice between moving that
code into `iclforge::base` and keeping a separate stage is Q3. Phase 4 has a date: DEE's licence ends on 2026-11-06.

## 6. Open questions for the owner

Each has a recommendation. None is settled here. The owner's existing questions (TH-ERRATA 1 to 19, CT-ERRATA 1 to 8) are not
repeated; they are named where a row waits on them.

1. **DEE's TrueHD streams, before 2026-11-06.** The `thd-` legs of `tools/generators/gen_dee_gold.py` can be made and scored only
   on the owner's machine, and the licence ends in eight days. The decision of 2026-09-26 makes them test inputs and keeps them
   off the tree. May the repository keep (a) only the manifest and PCM digests, or (b) a small subset of the streams as
   `testdata/external-baseline/` keeps DD+ ones? *Recommendation:* run and score them now; commit the manifest and digests (a);
   ask again about (b) with the sizes in hand.
2. **Third-party TrueHD streams as a decode oracle.** The DD+ side checks eight commercial excerpts a night
   (`tools/checks/verify_fate_interop.py`). The ROADMAP rules out "TrueHD interoperability by black-box analysis of Dolby
   streams". Decoding a commercial stream and comparing the result with FFmpeg's reads nothing of how it was made. Is that
   allowed? *Recommendation:* ask the owner to rule; build nothing until then.
3. **Where a TrueHD output stage lives, and whether it is in scope.** DD+'s `OutputStage` (dialnorm, fold, line and RF modes)
   is in `libs/ac3`; `truehd` links only `base`. Options: (a) move the neutral parts into `iclforge::base` and let both codecs
   use them; (b) a separate stage in `libs/truehd`; (c) report the metadata and leave applying it to the application.
   *Recommendation:* (c) in Phase 1 for all fields, then (a) if the owner wants one fold for both codecs. This touches
   `iclforge::base`, which every library uses.
4. **Applying the standard DRC word and the start-up gains.** Both are read and have no oracle (FFmpeg returns no gain for the
   hand-built stream that carries the word). DD+'s decoder leaves DRC off by default (`drc_scale` 0). Is the word's gain
   curve in the specification complete enough to implement from, and may it ship unchecked and labelled "no oracle", as the
   16-channel substream does? *Recommendation:* yes to both, default off, opt-in.
5. **Dual mono, associated services and karaoke.** Nothing in the library names them for TrueHD, and the specification is not
   in the tree. Does the specification define any of them? *Recommendation:* record them as not applicable in
   capabilities.md unless the owner says otherwise.
6. **Packaging.** Should the vcpkg port and the Conan recipe gain a `truehd` feature, off by default, as `ac4` has? The CMake
   comment says it was left out until a program that ships links it; `forge` does. *Recommendation:* yes, with an install
   consumer for it.
7. **Bindings: scope and order.** *Recommendation:* Rust over the C API, then Python over the C++ API as AC-4 is, both before
   Phase 5. JavaScript and WebAssembly only if a browser use is wanted; a lossless 7.1 stream may be too large for one. Which
   do you want?
8. **TrueHD on small targets.** Should TrueHD go into the minimum-footprint profile, the ESP-IDF component or the bare-metal
   probe? The decoder is integer-only and has no exceptions, so it could. *Recommendation:* no for now. Which codecs a sink decodes is the
   Hearth audit's to settle, and a TrueHD stream's declared peak is 18 Mbit/s.
9. **Lost data in a TrueHD decode.** Today a lost unit gives no samples until the next restart header, so the output is
   shorter than the source. Should there be an opt-in that fills the gap with silence of the right length (DD+'s `kMute`
   keeps timing)? *Recommendation:* yes, off by default.
10. **MAT passthrough to a receiver.** `PassthroughSink` needs a TrueHD entry and a backend path on each platform, and a
    receiver that takes MAT. The MAT layout itself waits on CT-ERRATA 2 and 3. Plan it now or later? *Recommendation:* later,
    after CT-ERRATA 2 is answered and a receiver is available.
11. **Encoder byte reproducibility.** DD+ pins its encoder's bytes per platform. The TrueHD encoder's bytes can differ by
    compiler, but the decode of any of them is the source. Pin the bytes, or hold the contract to the decode? *Recommendation:*
    the decode, plus a hash of the decoded fixtures per platform.
12. **HLS, DASH and CMAF for TrueHD.** The repository's sources give no profile for it, and `forge fmp4` refuses it.
    *Recommendation:* declare it out of scope in capabilities.md.
13. **The Atmos rows.** They wait on TH-ERRATA 1. Two things should be added to it: what MP4, Matroska and TS carry to signal
    TrueHD Atmos (CT-ERRATA is silent), and which immersive bed layouts the format defines (At6). *Recommendation:* add both to
    the specification's owner's list; plan nothing.
14. **ADM/BWF export of a TrueHD bed.** A bed-only ADM file needs no Atmos work (Me8). *Recommendation:* wait, and build it
    with the object export so the two share one writer.

## Findings outside the matrix

- `libs/capi/README.md` is generated from `projects.json` and names `include/iclforge/capi/` and a `fuzz/` directory; the tree
  has `include/iclforge_c/` and no fuzz directory in that library.
- `libs/audio/src/backend/alsa/eld_proc.hpp` says MAT/TrueHD "is not a format this codebase produces or consumes". MAT is
  produced now (Cn4).
