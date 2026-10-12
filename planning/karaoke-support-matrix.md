# Karaoke support matrix: AC-3 and E-AC-3

!!! note "Status as of 2026-10-12: read from `main` at `7f4979339` and from the peer sessions' unpushed work; every cell names the test or the command behind it"
    One row per capability, the AC-3 cell beside the E-AC-3 cell, and who builds each gap. It is the
    first deliverable of [the E-AC-3 programme](karaoke-eac3.md): the AC-3 column is the "make sure
    AC-3 is completely supported" half, and **no row is built twice**: where another session has
    the cell, its name is in the last column and this programme waits for its push. The E-AC-3
    column is read against [the findings](karaoke-eac3-findings.md).

Legend: **built** has a test or a command that passes today; **partial** says what is missing;
**gap** has no one; **n/a** is not a karaoke concern (the cell is the same whatever the stream).
"E-AC-3 plan" names the work item of [the plan](karaoke-eac3.md#work-items).

## The library and the command line

| Capability | AC-3 today | E-AC-3 today | Build, owner |
|---|---|---|---|
| **Decode: Annex C's aware reproduction** (L, C, R and the LFE) | **built**: `libs/ac3/tests/decoder/test_output_stage.cpp` (`karaoke: 3/2 is L M R V1 V2, ...`, `karaoke: a real karaoke stream decodes to L C R through the decoder`); `forge decode k.ac3 out.wav karaoke` | **gap**: `OutputStage` gets `levels.karaoke` only from `FrameDecoder`; `Eac3Decoder` never sets it, and `forge decode k.ec3 out.wav karaoke` warns "ignored" | E-AC-3: **E2** (this programme) |
| **Decode: the capable reproduction** (none, V1, V2, both) | **built**: `karaoke capable: ...` cases in the same file; `forge decode ... karaoke=none\|v1\|v2\|v1+v2` (`test_cli_encode_paths.cpp`, `decode karaoke=none\|v1\|v2\|v1+v2 is the karaoke-capable decoder's Table C.2.3`) | **gap**, with the above | **E2** |
| **Re-levelling** of M, V1, V2 (W10) | **built** in the library: `karaoke re-levelling: ...` (six cases); Hearth sliders; wire keys `karaoke_*_db`; **no CLI option** | **gap**, with the above | CLI option: Karaoke F (`karaoke-melody=` etc., their cell 2); E-AC-3: **E2** |
| The layout a reproduced frame comes out in | **built**: `ac3::output_mode()`, `karaoke: output_mode is the layout a real decoder's channels come out in` | **gap**: `Eac3Decoder::apply_output` sizes by `output_channel_count(config, acmod, lfe)` without the karaoke flag | **E2** |
| **Encode**: `bsmod=7` with a 3/0 to 3/2 layout | **built**: `forge encode ... bsmod=7`; `apps/forge/cli/tests/test_cli_encode_paths.cpp` `[karaoke]` | **partial**: `forge eac3-encode k.wav k.ec3 384 none <layout> bsmod=7` writes the stream (measured; `test_mp4.cpp` `dec3 box's asvc bit follows the karaoke/voice-over acmod split` builds one), but it writes no `mixmdate`, so a decoder takes -3 dB for the levels the AC-3 default carries as -6 | **E5** (`bsmod=7` writes `mixmeta` from `cmixlev`/`surmixlev`) |
| The 1+1 and mono refusal | **gap** (both accept `bsmod=7` on 1+1, which Annex A Table A5.2 and Annex G Table G.2 prohibit for a karaoke service) | **gap** (the same) | **E5**: a warning, same for both |
| **`forge project`** (a Forge project file with a karaoke source) | **built**: `test_project_command.cpp` (`project command: the karaoke example is the line its README gives`, `...parts that cannot make a karaoke stream say which`) | n/a today: the project names parts, and the stream is AC-3 | **E6** with the Forge GUI |
| **`forge probe`** (table and JSON) | **built**: `test_cli_probe.cpp` `probe names bsmod 7 by acmod: voice over at 1/0, karaoke wider` | **built** (measured: `bsmod 7 (karaoke)` and `3/2` on an E-AC-3 stream) but **no test names E-AC-3** | **E3**: one test case |
| **Analysis** (`levels`, `loudness`, `qc`) | n/a: they read the coded channels; a karaoke stream's vocals are the surrounds | n/a: the same | none |
| **`forge metadata`** (set `bsmod`) | **built** (`bsmod=7` rewrites in place) | **built** where `infomdate` exists; `insert` adds it (as the tool says) | none |
| **`forge transcode`** between AC-3 and E-AC-3 | **gap**: `bsmod` is not carried (karaoke becomes complete main, measured both directions); `bsmod=7` on the command line restores it | **gap**: the same | **E3** (carry `bsmod`; one test, both directions) |
| **Containers**: MP4 `dac3`/`dec3`, MPEG-TS PMT descriptors, Matroska | **built**: `test_mpegts.cpp` `the narrow layouts and the voiceover/karaoke split`; measured: `forge mp4`, `forge ts atsc` and `forge mkv` keep `bsmod 7` | **built**: `test_mp4.cpp` `dec3 box's asvc bit follows the karaoke/voice-over acmod split`; measured the same three | none |
| **`forge play`, `monitor`, `spdif`, `spatial`** | **in progress**: `karaoke` options on all four (a different decoder path from `decode`): Karaoke F, branch `feature/forge-karaoke-on-monitor-and-play`, pushed within the hour | **gap**; `play`/`spdif` pass the coded bytes (a receiver reproduces or not, P7), so only `monitor` and `spatial` decode | AC-3: Karaoke F; E-AC-3: **E3** after F's push |
| **C API** | **gap**: no karaoke field in `libs/capi` | **gap** | Karaoke F (their cell 3) |
| **Python, Rust bindings** | **gap**: none carry it | **gap** | Karaoke F (their cell 4) |
| **JavaScript binding** | **built**: `bindings/js/tests/push-decoder.test.js` (`the constructor forwards the karaoke reproduction and the vocals...`, `push says whether the unit is karaoke type...`) | **partial**: `is_karaoke_unit()` in the WASM glue reads an AC-3 header only | **E5** |

## The applications

| Capability | AC-3 today | E-AC-3 today | Build, owner |
|---|---|---|---|
| **Hearth: the Karaoke page** (reproduction, vocals, levels, notices) | **built**: `test_stream_decoder.cpp` (`stream decoder: a karaoke stream's reproduction reaches the speakers it names`), `test_decoder_settings.cpp`, `test_hearth_controller.cpp` (`decoder settings: the karaoke choice round-trips through the map`) | **gap**: `stream_decoder.cpp` `report_unit` sets `karaoke = false` for E-AC-3 and the bed layout path is `kAc3` only; the page disables its controls ("E-AC-3 and AC-4 ... never flagged", P6) | **E4**; Karaoke G is on Hearth's other leftovers, so sequence after G's push |
| Hearth: the **Media page** names a karaoke programme | **built**: `media_info_to_map: a karaoke programme names its coding mode and its parts`, `media info: bsmod 7 is voice over at 1/0 and karaoke above it` | **built** for a programme's own `bsmod` and `acmod` (the programme list is codec-blind): `media_programmes_to_list` | none |
| Hearth: **lyrics, timing, CD+G, duets** | **built** (phase 2) | n/a: the lyrics follow the position, whatever codec plays | Karaoke C (CD+G), lyrics verification 8996e5, E, D |
| **Network sinks: the host's rule** (`choose_sink_form`) | **built**: `test_sink_form.cpp` (`sink form: a karaoke stream goes as it is to a sink that takes the karaoke keys`, `...a sink that does not take the keys is sent a karaoke stream as PCM`, `...a group of a board that takes karaoke and one that does not plays one reproduction`) | **gap, and a trap**: `ItemFacts::karaoke` is the AC-3 fact; a sink that lists `karaoke` today decodes AC-3's flag and has no E-AC-3 path, so an E-AC-3 karaoke stream sent coded plays as coded on it while a PCM member plays the reproduction: the mixed-group failure phase 2's findings named | **E4a**, decision D9 (a capability key for the codec) |
| **Network sinks: the wire** (`karaoke`, `karaoke_vocals`, `karaoke_*_db`) | **built**: `libs/sendspin/tests/test_iclforge_player.cpp`; `test_engine_network_group.cpp` (`engine: a karaoke stream is one reproduction on a board that decodes it and one sent PCM`, `...a part level changed while a karaoke item plays reaches the board that decodes it`) | the keys are codec-neutral, **unchanged**; what is missing is a way for a sink to say which codec it reproduces them for | **E4a** |
| The **test sink** (`apps/hearth/testsink`) | **built**: `burst_output.cpp` reads `is_karaoke` from an AC-3 header | **gap**: the same header read | **E4a** |
| **ESP32 sinks, S3**: the board reproduces and levels | **built and run on a board**: `test_network_live.cpp` `network sinks live: a karaoke stream is reproduced by the board that decodes it` (S3a: as coded, aware, capable v1 and none within 0.25 dB of the host; the levels within 0.05 dB) | **gap**: `firmware/esp-idf/iclforge/src/burst_player.cpp` takes the flag from an AC-3 header (`kind == kAc3`); the E-AC-3 decoder it shares sets none | **E4b**, on a board; footprint check first (the S3 main-task stack margin) |
| **ESP32 sinks, C6 and P4** | **partial**: the same firmware path, **not run with karaoke** on either board; the P4's panel is Karaoke support on P4X | **gap**, with the S3 row | **E4b** for the codec; P4X for the panel (they confirm they touch none of `burst_player.cpp`, `libs/sendspin` or the karaoke settings) |
| **Forge GUI: the Karaoke flow** | **built**: `test_karaoke_authoring.cpp` (sixteen `karaoke authoring: ...` cases), `test_karaoke_outputs.cpp` (the part-to-speaker table against the decoder for every mode and choice), `tst_karaoke_flow.qml`, `tst_projects.qml` | **gap**: `KaraokeFlow.qml` sets `EncoderController.codecIndex = 0` and says "E-AC-3 and AC-4 have no karaoke mode" | **E6**, decision D7 |
| The Forge GUI's **stream player** | **built**: a reproduction choice (K6; the Hear step of `tst_karaoke_flow.qml` plays the built stream under each) | **gap**: with the decoder (E2) it takes the same choice | **E4c** |
| **Browser decode demo** (WebAssembly) | **built**: `apps/demos/wasm/tests/decode.spec.js` (`a karaoke AC-3 stream is said to be one, and which vocals are heard is a choice`, `the page offers the vocals of a karaoke stream and of no other...`) | **gap**: `is_karaoke_unit()` reads an AC-3 header; the library does not reproduce E-AC-3 yet | **E5**, after E2 |
| **Crucible** | n/a: it shows lyrics against an application; it does not decode | n/a | Crucible media session and CD+G (a533fc) |
| **User guide** | **built**: `docs/karaoke/index.md`, `docs/hearth/index.md#karaoke`, `docs/library/decoding.md` | "E-AC-3 and AC-4 have no karaoke mode in their specifications; this is AC-3 only" | **E8**; the correction list is [in the findings](karaoke-eac3-findings.md#what-the-repository-says-that-this-corrects) |

## What this leaves, by owner

| Owner | Cells |
|---|---|
| **Karaoke F** [873020] | play, monitor, spdif, spatial options; `karaoke-melody=`/`-v1=`/`-v2=`; C API; Python and Rust bindings; the browser demo's lyrics |
| **Karaoke G**, **C**, **D**, **E**, **H**; lyrics verification [8996e5]; Crucible [a533fc]; **P4X** [dab81f]; the microphone programme | what their names say; none of it is codec-specific |
| **This programme** | E2 (decoder), E3 (reports, `transcode`, `monitor`/`spatial` after F), E4 (Hearth, host rule, wire, firmware), E5 (authoring flags, WASM glue), E6 (Forge GUI), E7 (the oracle), E8 (docs), and the AC-3 fixes the measurements found (E3's `transcode`; D8's LFE) |
| **Nobody yet** | the Android Hearth page (phase 2 D4: no player to put it in), the mono reproduction's own test against Dolby's decoder |
