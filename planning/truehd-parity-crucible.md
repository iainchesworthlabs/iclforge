# TrueHD and DD+ parity in Crucible

!!! note "Status as of 2026-10-29: an audit of `integ` at `523cf5dc`; nothing here is built"
    Round 1 of the TrueHD and DD+ parity work: one document per area, audit and plan only. This page covers
    Crucible (`apps/crucible`, `docs/crucible`, the planning pages that name it, and its tests). It reads
    [truehd.md](truehd.md), [../libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md) and
    [../docs/library/capabilities.md](../docs/library/capabilities.md) as the state of TrueHD, and does not repeat them.
    Atmos in TrueHD is on hold until the owner confirms the evolution payload layout; every row that needs it is
    marked blocked and is not in the plan.

!!! note "Update of 2026-10-31 (work package 4): phases 0, 2 and 3 are built; phases 1 and 4 need the owner"
    Built on `feature/truehd-crucible`: the seams (`BurstFormat`, `accepts_truehd`), the pin-only mode `kTrueHd51`
    with the equality oracle, the window, runner, diagnostics, translations and pages. The answers taken from the
    recommendations below, since the owner has not replied: question 1 yes, pin-only; 2 a 5.1 bed; 3 pin-only; 4
    plain rounding; 5 48 kHz; 8 the restart interval stays at the encoder's 128 until a receiver confirms 8; 9
    the codec-path switch keeps its E-AC-3 meaning; 10 `DownmixMode::kNone` and the E-AC-3 encoder's bed. Question 6
    was answered by the owner on 2026-10-31 (eight 16-bit channels at 192 kHz, 176.4 kHz for the 44.1 family; Microsoft's page gives the same link).
    Phase 1 was built by the Hearth session in parallel and merged first (`BitstreamFormat::kMat`, `supports_mat_passthrough`, ALSA, PipeWire and WASAPI;
    Core Audio refuses it); this branch uses that. Not built: the hardware reading and the throughput (phase 4). `crucible-probe` and the hidden case `[truehd_cost]` are
    the tools for it. What this round asks is in
    [Questions from the build](#questions-from-the-build), at the end.

## Basis

The tree is `integ` at `523cf5dc` (worktree branch `parity-crucible`).

What was run:

- `iclforge-crucible-tests`, the binary of the existing `integ` build at the same commit (not rebuilt):
  `All tests passed (192303 assertions in 200 test cases)`, 9 s. This is the suite that needs no window and no audio
  device; it runs the policy, the output stage, the engine and the tap pool over fake devices. The Qt Quick suites
  (`crucible_qmltests`, label `crucible-ui`) were not built or run: that build has `ICLFORGE_BUILD_CRUCIBLE=OFF` and no Qt.
- `iclforge-containers-tests "[truehd]"`: `All tests passed (6226 assertions in 26 test cases)`, which includes the MAT cases.
- `tools/checks/check_layering.py` (`39 projects, 2604 edges between them ... 0 failures`), with `--edges` for Crucible's
  edges; `tools/checks/test_project_graph.py` (13 tests, OK); `tools/checks/project_graph.py affected` and `lanes`.
- `forge truehd-encode`, `probe`, `spdif` and `decode` on a 30 s, 48 kHz, 24-bit synthetic WAV (noise through a one-pole
  filter plus tones) in 6 and 8 channels, to see what a TrueHD leg would cost and what it would hand a burst sink. The
  numbers are from a shared 4-core machine at `nice -n 10`, and the host is not the Raspberry Pi or the Windows PC
  Crucible has run on. The script is `mkwav.py` in this agent's scratch directory, not in the tree.
- `.github/ci/legs.jsonc`, parsed, to list the legs that build Crucible.

Everything else is read only: the engine, the output stage and policy, the window's QML, the platform halves of
`libs/audio`, the driver, the CI workflows and the documentation. There is no audio device here (`forge outputs` answers
that this build has no passthrough backend), no Windows, no macOS and no receiver, so no row below claims that a TrueHD
stream reached a device. Where a number or a behaviour could not be found in the repository, the cell says so.

## Where DD+ enters and leaves Crucible

**It does not enter as a coded stream.** Crucible captures PCM and makes its own E-AC-3.

- *Taps.* One process-loopback tap per application, float PCM at 48 kHz, 2, 6 or 8 channels
  (`apps/crucible/engine/src/tap_pool.hpp`, `EngineConfig::tap_channels`; the Windows spike S1 got float32 at 48 kHz and an
  8-channel request granted, `apps/crucible/spikes/README.md`). The silent device that applications play into is PCM only: the
  Windows driver advertises one format, 8 channels, 48 kHz, 16-bit (`apps/crucible/windows/driver/Source/Main/circuit.cpp`,
  `Pcm48000c8`); the Linux node is a PipeWire adapter; macOS has no silent device.
- *No other way in.* No file input, no network input, no recording output (`grep` finds no file writer in
  `apps/crucible/engine/src` or the runner). The host plugin of [host-plugin.md](host-plugin.md) is a study with no code
  (`grep` finds no CLAP or VST3 code under `apps` or `libs`), and its scope list says it does not "talk to Crucible".
  The driver is the only driver path.

**What is done with the audio.** Per frame (`apps/crucible/engine/src/engine.cpp`, the loop after `sync_taps()`):

1. Each tap is folded to one object (`fold_to_mono`) or two (`fold_to_pair`) for a placed application, or summed into five
   bed slots (L R C Ls Rs) for an unplaced one (`apps/crucible/engine/src/bed_mixer.cpp`). The LFE of a surround tap is
   dropped. There are 15 object slots: 10 positioned, 5 pinned to the bed (`slots.hpp`).
2. `iclforge::ac3::oba::AtmosEncoder` encodes the 15 slots as E-AC-3 with a JOC object layer: six blocks (1536 samples,
   32 ms) or one block (256 samples, 5.3 ms) per frame, 448 kb/s or 1536 kb/s in low latency, or a fixed rate from Settings.
   `AtmosConfig::dialnorm` stays at its default, 31. Without a signing key `emit_object_metadata` is false and the encoder
   pans the objects into its 5.1 bed.
3. With a key, `SigningHook::sign` (`signing_hook.cpp`, `sign_atmos_frame`, the EMDF scheme) signs the access unit.
4. `OutputStage::submit` takes the unit and a `RawFrame` (the objects, their placements and the encoder's 5.1 bed in
   AC-3 coded order).

**Where it leaves** (`apps/crucible/engine/src/output_stage.cpp`, `output_policy.cpp`):

| Mode | What leaves | Path |
|---|---|---|
| Atmos, DD+ 5.1 | E-AC-3 bursts | `Eac3BurstPacker` then `BurstSink` (`iclforge::audio::PassthroughSink`, E-AC-3): WASAPI exclusive, PipeWire `iec958` |
| DD 5.1 | AC-3 bursts | the AC-3 `FrameEncoder` over `raw.bed` (dialnorm 31, 448 kb/s), gathered to six-block frames, `wrap_frame`, `BurstSink` |
| PCM surround, stereo | float PCM | `Eac3Decoder` of the unit (stereo through the Lo/Ro fold), or the bed itself under the bypass switch, to `MonitorSink` |
| Headphones | objects | `Eac3Decoder`, `describe_objects`, the LFE delayed by the JOC reconstruction delay, to `SpatialObjectSink` (Windows Spatial Sound only) |

The policy (`choose_output`) takes a mode and an endpoint from what the probe found (`accepts_eac3`, `accepts_ac3`, shared
PCM channels, spatial sound) in the order Atmos, DD+ 5.1, DD 5.1, PCM surround, headphones, stereo, and never opens a
bitstream mode on the default endpoint, because Windows refuses an exclusive open there and invalidates the applications'
streams. On Linux only the PipeWire backend is usable, since ALSA has no per-application capture
(`apps/crucible/CMakeLists.txt`). A mode switch stops the old sink and starts the new one; the encoder upstream does not
restart.

**Level and loudness.** Crucible does no dialnorm or loudness handling. There is one fixed dialnorm (31, no level shift) on
both encoders and a per-application dBFS meter in the window (`dbfs()` in `engine.cpp`). The "Metadata and loudness QC:
Settings only" cell of the generated capability table is the only mention.

**Verification of the output.** The tests run the stage over fake sinks and count what arrives. On hardware, a receiver's
front panel read "5.1 DD+" for a pre-encoded fixture and "Atmos/DD+" at 7.1 for Crucible's own signed stream, on a
Raspberry Pi 4B over PipeWire on 2026-09-05 ([promotion record](../docs/crucible/design/promotion.md)). Nothing in the engine
checks a decode of its own output against its input; the decoded modes decode the unit so that the codec is in the path
(`output_stage.hpp`, header comment).

## What the same path needs for TrueHD

What exists today, all outside Crucible:

- `iclforge::truehd::Encoder` (`libs/truehd/include/iclforge/truehd/encoder/encoder.hpp`): 1 to 8 channels of 16, 20 or
  24-bit integer PCM at 44.1 to 192 kHz; `encode()` takes any whole number of frames and returns the access units that are
  complete; it holds one restart interval of input (128 access units, 107 ms at 48 kHz, by default; 1 or at least 8 is
  allowed) before it emits anything.
- `iclforge::truehd::Decoder`: bit-exact, one access unit per call, no samples before a restart header.
- `MatPacker`, `wrap_mat_stream`, `unwrap_mat_stream` (`libs/containers/include/iclforge/containers/iec61937/mat.hpp`): data
  type 22, 24 access units to a 61,440-byte burst, every layout read off FFmpeg's `spdif` muxer
  ([ERRATA](../libs/containers/ERRATA.md), M1 to M7).
- Nothing in `libs/audio`: `BitstreamFormat` is `kAc3, kEac3, kAc4, kAc4Hbr4, kAc4Hbr16`
  (`libs/audio/include/iclforge/audio/passthrough.hpp`), and every backend's carrier is a 2-channel, 16-bit stream
  (`kCarrierChannels = 2` in the ALSA and Windows backends, `kCarrierFrameBytes = 4` in PipeWire's).

The shape of the TrueHD leg is the DD 5.1 leg's, not the E-AC-3 legs': it does not take the encoded unit. It takes
`raw.bed` (the encoder's 5.1 bed, floats, AC-3 coded order), puts it in TrueHD channel order (the order PCM sinks already
use, `kCodedForWave = {0, 2, 1, 5, 3, 4}` in `output_stage.cpp`), rounds to 24-bit integers, gathers to whatever the
encoder takes, passes each returned access unit to `MatPacker::push`, and hands each burst to the burst sink. That leg is
a bed-only mode, the analogue of "DD+ 5.1 without a key". The E-AC-3 encoder keeps running upstream because it is the
panner that makes the bed.

Code that changes: `OutputMode` and `choose_output`; `OutputStage::apply` and `submit`; `BurstSink::start`, whose third
argument is `bool eac3`; `DeviceFacts` and `EndpointFacts`; `library_devices.cpp`; the engine's choice of what to build
when TrueHD is pinned; the window, the tray menu, the runner and the diagnostics; six translation catalogues; the
fakes and the tests. Build edge: `tools/checks/projects.json` gives `crucible` `may_use` of `ac3, ac4, audio, base,
containers, lyrics, objects` and three app libraries, with no `truehd`; `crucible_engine` links `iclforge::ac3
iclforge::audio iclforge::tracy`. The MAT packer is in `containers`, which Crucible already uses
(`output_stage.cpp` includes `iec61937.hpp`), so only the encoder needs the new edge.

## The matrix

Gap: none, partial, missing, or n/a (not applicable, with the reason). Size is the ROADMAP's scale: S an afternoon, M a
day or two, L a focused week, XL several PRs. "Q" numbers are the [open questions](#open-questions-for-the-owner). Paths are
under `apps/crucible/` unless they start with `libs/`, `tools/`, `docs/` or `.github/`.

### Making the stream

| ID | Capability | DD+ status | TrueHD status | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| C1 | Live encode of the room's mix as a bed | Built. `AtmosEncoder` in `engine/src/engine.cpp` (`build_encoder`); AC-3 `FrameEncoder` leg in `engine/src/output_stage.cpp` | The encoder exists (`libs/truehd/include/iclforge/truehd/encoder/encoder.hpp`). No Crucible code links or calls it; no `OutputMode` for it; the bed would come from `encoder->bed()`, which is 5.1 (Crucible's bed has five slots and no LFE source) | missing | M | C10, C20; Q1, Q2, Q4, Q10 | `forge truehd-encode` of 30 s of 6-channel 48 kHz 24-bit: 3.2 s wall, 2.5 s user (about 9x real time); 8-channel: 4.5 s. `forge decode` of the result: PCM SHA-256 equals the input's (`72674e774a374f63...`). Crucible side: read only; `grep -ril truehd apps/crucible` finds nothing outside translations |
| C2 | Frame cadence and the latency setting | Built. 6-block frames (1536 samples, 32 ms) or 1-block (256, 5.3 ms) at 1536 kb/s; the AC-3 leg gathers one-block beds into six-block frames (`ac3_pending`; test `output stage edges: low-latency one-block beds reach DD 5.1 as whole AC-3 frames`). `docs/crucible/settings.md` block 03 | An access unit is 40 samples at 48 kHz. A MAT burst is 24 of them: 960 samples, 20 ms. Engine frames are 38.4 and 6.4 access units, so a gather is needed. The encoder adds its restart interval (6.7 ms at 8 units, 107 ms at 128). The floor is therefore about 27 ms with an interval of 8, against 5.3 ms for E-AC-3 low latency: the setting cannot apply to this mode, as it cannot to AC-3 | partial (different floor) | S | Q8 | Arithmetic from `mat.hpp` and `encoder.hpp`. `forge probe` of the 5.1 stream: `36000 access units (40 sample(s) each), 282 major sync(s)`. `truehd-restart=8`: 22,061,052 bytes against 21,403,780 at 128 (+3.1%) on this signal. No end-to-end measurement of either path exists (`docs/crucible/troubleshooting.md`, "Sound is behind the picture") |
| C3 | Bitrate setting | Built. Settings block 04: automatic (448 or 1536 kb/s) or seven fixed rates; runner `--bitrate` | Lossless and variable: no setting applies. Peak data rate is a limit (18 Mbit/s default), not a choice; at 48 kHz and 8 channels the raw PCM rate is 9.216 Mbit/s, below it. The window must show the control as not applicable while the TrueHD mode is in use | n/a (no bitrate to set); window text needed | S | C17 | Read only; arithmetic |
| C4 | Sample rate and word length | 48 kHz only: `AtmosConfig` is `k48000`; 48000 is written into `tap_pool.hpp`, `engine.cpp` and `OutputStageConfig` | The library writes 44.1 to 192 kHz and 16, 20 or 24 bits. Crucible's taps are float, so a float to 24-bit step is new code. Higher rates would need new tap rates and a silent device that offers them (the driver has one format) | partial | S (48 kHz, 24-bit); higher rates not sized | Q4, Q5 | Read only. Could not tell whether the Windows driver's 16-bit endpoint format limits what the tap delivers: the spike measured float32 at the tap, not the precision behind it |
| C5 | Level and dialogue normalisation | One fixed dialnorm, 31, on both encoders (`AtmosConfig::dialnorm` default; `.dialnorm = 31` in `output_stage.cpp`). No loudness handling | The encoder writes `EncoderConfig::channel_meaning` (dialogue norm per presentation, mix levels, DRC start-up gains, control flags), all zero and disabled by default. The repository does not say what a receiver does with zero, or which value means "no level shift" | partial | S | Q7 | `grep dialnorm` in `apps/crucible` finds only `output_stage.cpp`. Read only |
| C6 | Objects intact to a receiver (Atmos) | Built. Mode `kAtmos`, `emit_object_metadata`, JOC. Receiver read "Atmos/DD+" at 7.1, Pi and PipeWire, 2026-09-05 | The sixteen-channel substream is decoded as opaque bytes; the encoder writes no Atmos substream; object metadata is not written | missing | XL (library and Crucible) | **Blocked**: the owner has not confirmed the evolution payload layout ([truehd.md](truehd.md), decision 1). Not planned | Read only |
| C7 | Object signing: key from a path or the environment, status line, "objects on" | Built. `engine/src/signing_hook.cpp`, `sign_atmos_frame` (EMDF); `test_engine_commands.cpp` ("a signing key turns objects on and clearing it...") | The content signature of the evolution container has no confirmed scope; the library has no signer. [object-signing.md](../docs/concepts/object-signing.md#sibling-truehd-evolution) says the TrueHD check is a separate scheme, not the EMDF path | missing | L (after C6) | **Blocked** with C6; Q11 | Read only |
| C8 | Decoded modes (PCM surround, stereo, headphones) play a decode of the stream; the bypass switch | Built. `Eac3Decoder` in `output_stage.cpp`; tests `output stage: a multichannel PCM endpoint gets decoded 5.1`, `...a stereo endpoint gets a Lo/Ro decode, and the bypass a bed fold` | A TrueHD decode equals the encoder's input, so the PCM modes already play what a decode would. The leg needs no decoded mode. The decoder is useful as a check (see [lossless verification](#things-truehd-has-that-dd-never-needed)) | n/a (lossless: a decode is the identity, so the codec-path switch has nothing to show) | S (words in Settings block 05) | Q9 | Read only |

### Delivering the stream

| ID | Capability | DD+ status | TrueHD status | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| C9 | IEC 61937 framing | Built. `Eac3BurstPacker`, `wrap_frame` (`libs/containers/include/iclforge/containers/iec61937/iec61937.hpp`), called from `output_stage.cpp` | `MatPacker` exists and is tested against FFmpeg's muxer. Crucible does not call it | partial | S | C1 | `iclforge-containers-tests "[truehd]"`: 26 cases pass, including `MAT frames equal the bursts of FFmpeg's spdif muxer` and `a damaged MAT carrier is refused, not misread`. `forge spdif` of the 5.1 stream: `wrapped 36000 TrueHD access units into 1500 MAT burst(s)`, `eight channels of 16-bit PCM at 192000 Hz (worked out from the burst size; no receiver has been tried)` |
| C10 | Crucible's `BurstSink` seam | Built. `BurstSink::start(device, rate, bool eac3)` in `engine/src/audio_devices.hpp`; the fake in `engine/tests/fake_devices.hpp` records `eac3` | A boolean cannot name a third format. Becomes a format enum; `library_devices.cpp` and the fakes follow | missing | S | nothing | Read only |
| C11 | Passthrough delivery, Windows (WASAPI exclusive) | Built. `make_eac3_format` in `libs/audio/src/backend/windows/passthrough.cpp`: subtype `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS`, link at 4x the content rate, 2 channels, 16-bit. ROADMAP lists WASAPI exclusive confirmed on an Onkyo TX-RZ740 | No TrueHD subtype in the file; the carrier is 2-channel in every function. The MAT carrier (eight channels at 192 kHz, 176.4 kHz for the 44.1 family) is the library's own arithmetic, marked provisional | missing | L | Q6; a receiver | Read only. The Windows SDK's TrueHD subtype and format fields were not checked against `ksmedia.h` or Microsoft's page, which the DD+ code cites |
| C12 | Passthrough delivery, Linux (PipeWire) | Built. `iec958_codec_for` maps AC-3 and E-AC-3 (`libs/audio/src/backend/pipewire/pipewire_support.hpp`); the sink's `iec958.codecs` gates the probe | The header's comment names `TRUEHD` among SPA's codecs (1.6), and `codec_listed` is tested with a `TrueHD` token. The mapping, the 8-channel carrier and the 40-burst queue sized for 20 ms bursts (800 ms) are not written | missing | M | Q6; a receiver | Test `an iec958.codecs list is read name by name` in `libs/audio/tests/backend/pipewire/test_pipewire_support.cpp` reads `[ PCM EAC3 TrueHD MPEG2-AAC ]`. Backend code read only |
| C13 | Passthrough delivery, macOS (Core Audio) | Written, never run. Probes `kAudioFormat60958AC3` and `kAudioFormatEnhancedAC3` (`libs/audio/src/backend/macos/passthrough.cpp`); the file says Apple documents no HAL mechanism for E-AC-3 | No MAT or TrueHD format is probed. Whether Core Audio can carry MAT is not in the repository. Crucible on macOS has never made a sound | missing (could not tell whether possible) | not sized | hardware (a Mac); Q6 | Read only |
| C14 | Endpoint probe and capability facts | Built. `RenderDeviceInfo::supports_eac3_passthrough` (`passthrough.hpp`), `DeviceFacts`/`EndpointFacts::accepts_eac3`, `enumerate()`; the window's endpoint table (`eac3`, `ac3`); the diagnostics line `eac3=... ac3=...`. CEA-861 SADs: `SinkAudioCapabilities` has `ac3`, `eac3` only; `eld_proc.hpp` collapses MAT/TrueHD into "other" | No `truehd` field in any of them | missing | M (S in Crucible; the rest is C11 to C13) | C11, C12 for a real answer; plumbing needs nothing | Read only. The Pi's receiver published `iec958.codecs = [PCM, DTS, AC3, EAC3, TrueHD, DTS-HD]` ([promotion record](../docs/crucible/design/promotion.md), Phase 4) |
| C15 | Output policy, pin, default-endpoint rule | Built. `OutputMode::kAtmos`, `kDdPlus51`, `kDd51`; `choose_output`; pinned mode; chosen endpoint; exclusive modes never on the default endpoint | No mode. Order in the automatic choice is undecided | missing | S | Q1, Q3 | `iclforge-crucible-tests`: 200 cases pass; 19 `output_policy` cases (`an Atmos receiver with a key gets Atmos`, `the same receiver without a key gets DD+ 5.1 and is told why`, `a receiver on the default endpoint is never opened exclusively`, ...) |
| C16 | Mode switch and joining a stream | Built. `OutputStage::apply` tears down and starts a sink; the encoder is not restarted (test `output stage: a probe that changes the answer switches sinks mid-stream`) | A receiver, or any new sink, can use TrueHD only from a major sync and each substream's restart header; the decoder returns nothing before one (`DecoderCounters::units_waiting`). Switching into the mode must start a fresh `Encoder` (or `flush()` and continue) so that a major sync leaves at once, or wait up to a restart interval | missing | S | C1 | Read only; behaviour from `decoder.hpp` and `encoder.hpp` comments |

### The window, the runner, the diagnostics

| ID | Capability | DD+ status | TrueHD status | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| C17 | Mode names, the Pin list, the tray's signal-path menu, the endpoint table, the Signal path page | Built. `ui/src/crucible_controller.cpp` (`mode_key`, `short_mode_name`), `ui/assets/qml/OutputPage.qml` (pin list; `chosenNote`'s exclusive-mode test), `ui/assets/qml/Main.qml` (tray), `docs/crucible/signal-path.md` (mode table) | Absent | missing | M | C15; Q3 | QML suites `tst_output.qml`, `tst_signalpath.qml`, `tst_shellworkflow.qml` exist; not run here |
| C18 | Translations | Six catalogues (`ui/assets/translations/crucible_{ar,de,es,fr,he,yi}.ts`), machine-made, carrying the mode names; the Windows MSVC leg fails on a stale catalogue (`_ci-windows.yml`, "Check the Crucible translations are up to date") | No TrueHD strings | missing | S (a few strings; the language review CR1 is already open) | C17 | `iclforge-crucible-tests` includes `test_translations.cpp`, passing |
| C19 | `crucible-run` and the diagnostics file | Built. `--pin` words `atmos ddplus dd pcm headphones stereo` (`runner/src/main.cpp`); `engine/src/diagnostics.cpp` prints `eac3=` and `ac3=` per endpoint; `test_diagnostics.cpp` | Absent | missing | S | C14, C15 | Read only |

### Build, tests, lanes, documentation, hardware

| ID | Capability | DD+ status | TrueHD status | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| C20 | Dependency edge and project table | `ac3` is in `may_use`, linked by `crucible_engine`; tests link `ac3 audio containers ac4` (`engine/tests/CMakeLists.txt`) | No `truehd` in `may_use`, no `iclforge::truehd` link | missing | S (one line in `projects.json`, two in CMake; the generated `docs/crucible/index.md` "Uses" line follows) | lands with C1 | `check_layering.py --edges`: `crucible -> ac3 (include): 16`, `-> audio (include): 19`, `-> containers (include): 2`, no `truehd` line. `project_graph.py affected libs/truehd/src/decoder/decoder.cpp`: `capi,examples,forge,rust,truehd` (no `crucible`); for `libs/audio/...`: `audio,crucible,demo-android,forge,hearth,hearth-android` |
| C21 | Tests that need no device | Built over fakes: `test_output_policy.cpp`, `test_output_stage.cpp`, `test_output_stage_edges.cpp`, `test_engine.cpp`, `test_engine_commands.cpp`, `test_diagnostics.cpp`, `fake_devices.hpp` | None for TrueHD | missing | M | C10, C1 | 200 cases pass (see Basis). The windows-llvm coverage floor over `apps/crucible` is 67.0% lines, 53.5% branches (`tools/checks/coverage_crucible.ps1`), so new code needs tests |
| C22 | CI lanes | Crucible is built and tested on six legs: Linux LLVM x86_64 and arm64, Windows MSVC and clang-cl, macOS arm64 and x86_64 (`.github/ci/legs.jsonc`, parsed). The GCC, sanitizer and Windows arm64 legs do not build the application, but `iclforge-crucible-tests` is not gated on it (`engine/tests/CMakeLists.txt`), so those legs build the device-free suite. TrueHD's FFmpeg checks run in the core lane (`_ci-core.yml`) | A `truehd` edge makes `libs/truehd` changes light Crucible's three platform lanes. No FFmpeg check covers bursts that Crucible's leg makes | partial | S | C20 | Read only |
| C23 | Documentation | `docs/crucible/index.md` ("Codecs", item 5 of "What it does"), `signal-path.md` (modes, Pin list, block 05), `settings.md` (03, 04, 05), `troubleshooting.md`, `install.md` and `design/promotion.md` under `docs/crucible/`; generated table `docs-snippets/generated/application-capabilities.md` from `docs/assets/data/support-catalogue.json` (`application_capabilities`); `ROADMAP.md`; `CHANGELOG.md` | The only TrueHD words are the receiver's codec list in `promotion.md` and `troubleshooting.md`. The catalogue has no TrueHD row for any application | missing | M | C15 names | `grep -rIil truehd apps/crucible docs/crucible` lists `promotion.md` and `troubleshooting.md` |
| C24 | Throughput on the machines Crucible runs on | Measured. Spike S4 (`spikes/s4_throughput/s4_throughput.cpp`): encode and pack per frame against the 32 ms budget | Not measured on a target. About 9x real time for 5.1 on the audit host (see C1); the Pi 4B and the Windows desktop are unknown | missing | S | C1 | The audit-host figure above only |
| C25 | A receiver's own reading | "5.1 DD+" and "Atmos/DD+" read off a receiver, Pi and PipeWire, 2026-09-05; Windows exclusive on an Onkyo TX-RZ740 per the ROADMAP (the promotion record still lists "Windows bitstream to a real receiver" as not yet, for Crucible itself) | None. [ERRATA](../libs/containers/ERRATA.md) says no receiver was tried for MAT. `passthrough_probe`, which streamed the DD+ fixture, was not a CMake target and had not run since the library was split; as of 2026-11-01 it is the `crucible-probe` target (`apps/crucible/runner/src/bitstream_probe.cpp`) and streams a TrueHD file as MAT too | missing | M (needs the owner's receiver time) | C11, C12, C1 | Read only |

Counts: 25 rows. By gap: 18 missing, 5 partial, 2 n/a, none 0. By size: 14 S, 7 M, 2 L, 1 XL, 1 not sized (C13, whose feasibility is unknown). C6 and C7 are blocked on the owner's evolution-payload answer and are outside the plan.

## Things TrueHD has that DD+ never needed

**MAT carriage over a wider link.** A TrueHD stream reaches a receiver as MAT (IEC 61937 data type 22): a 61,440-byte burst
holding 24 access units, 20 ms of audio at the 48 kHz family's rates and 21.8 ms at the 44.1 family's. The library's reading of the carrier is eight 16-bit channels
at 192 kHz (176.4 kHz for the 44.1 family), 24.6 Mbit/s against E-AC-3's 6.1 Mbit/s; it is provisional and is the owner's to
confirm ([ERRATA](../libs/containers/ERRATA.md), item 7). Every `PassthroughSink` backend assumes a two-channel carrier. This is
the same missing piece as AC-4 HBR16, which `passthrough.hpp` says no backend opens. The work sits in `libs/audio`
and serves forge and Hearth as well; it is listed here because Crucible cannot send TrueHD without it (C11 to C13).

**Lossless verification.** The decode of a TrueHD stream is the encoder's input, sample for sample. For E-AC-3 Crucible
decodes its own unit so that the codec is in the path; for TrueHD the same decode is a check: unwrap the bursts, decode, and
compare with the 24-bit integers that went in. The check is exact, so it belongs in the tests of the leg as an equality, and
it can run in the field at the cost of one more decode (about a ninth of the encode's time here: 0.35 s to decode the 30 s
stream against 3.2 s to encode it) or through `EncoderConfig::verify`, which costs "about as much again as the encode".
The boundary of the claim needs stating in the documents: lossless covers the path from the 24-bit integers on. Crucible's
mix is float, and the tap's precision on Windows behind a 16-bit endpoint format is not known (C4).

**Rate that is high and variable.** The 5.1 test stream averaged 5.7 Mbit/s (21.4 MB in 30 s) with a declared peak of
18 Mbit/s as the default limit. Nothing in the settings or the policy has a variable rate today. The encoder emits its
output a restart interval at a time, so the sink receives about five MAT bursts together every 107 ms at the default interval
and none in between. The sink queue holds 40 bursts in all three backends, so the burst fits, but a sink that starts empty has
nothing to play for the first interval: the start needs a prebuffer, and `kSubmitPatience` (200 ms) in `output_stage.cpp`
must be checked against it.

**Restart-interval granularity.** A listener joins a TrueHD stream at a restart header, not at a frame. The interval is the
encoder's choice: 128 access units by default (107 ms), or 8 (6.7 ms) at about 3% more bytes on the test signal. It decides
the latency floor (C2), how long a hot switch is silent (C16), and how quickly a receiver that lost lock can recover.

**Presentations.** A TrueHD stream can carry a stereo and a 5.1 presentation beside the 7.1 one. With `DownmixMode::kNone`
(the form FFmpeg's encoder writes, and the one an external decoder reads) the 2-channel presentation is the first two
channels as they are, with no downmix; with `kStandard` the encoder writes real downmixes that, per
[truehd.md](truehd.md), nothing outside the library reads. How a receiver chooses among the presentations is not in the repository. DD+ leaves
downmixing to the decoder and its metadata, so Crucible never made this choice (Q10).

**Word length and sample rate.** 16, 20 or 24 bits and up to 192 kHz are TrueHD's range, and 48 kHz and float are Crucible's
(C4).

**Atmos is blocked.** TrueHD Atmos (C6, C7) cannot be built until the evolution payload is confirmed, and the DD+ pair
(E-AC-3 JOC, EMDF signing) has no TrueHD counterpart to copy. The first TrueHD mode is therefore the equivalent of DD+ 5.1
without a key: the room's placements panned into a bed, no objects.

## A proposed definition of "first class" for Crucible

For the owner to approve or change.

1. Crucible can send TrueHD to a receiver that accepts it, as a pinned mode, on Windows and Linux, through the same policy
   as DD+: never opened exclusively on the default endpoint, hot-switched with the rest, one endpoint at a time.
2. The endpoint probe reports whether each endpoint takes TrueHD, by the rule the E-AC-3 probe follows (a read of the
   sink's own capabilities where the platform gives one, a format query where it does not, never an open of a live
   endpoint).
3. The mode has a name in the window, the tray menu, the endpoint table, `crucible-run`, the diagnostics file and the six
   catalogues, and the settings that do not apply to it (bitrate, low latency) say so.
4. The leg is tested with no device in all six CI legs, and the test is an equality: the bursts the leg hands the sink
   unwrap and decode to the 24-bit integers it was fed. The same bursts pass FFmpeg's strict decode in the core lane.
5. A receiver's front panel has read "TrueHD" from Crucible's own engine on one platform, and the reading is recorded in the
   promotion record as the DD+ readings were.
6. The throughput of the leg is measured on the Raspberry Pi 4B and the Windows machine Crucible has run on, and the
   latency floor is written down.
7. `truehd` is in Crucible's row of `projects.json`, and `check_layering.py` passes.
8. Every page that names E-AC-3 or AC-3 as something Crucible sends names TrueHD too, or says why not; the generated
   capability table has the TrueHD rows with a cell for Crucible; ROADMAP and CHANGELOG say it.
9. Where TrueHD cannot equal DD+ it says so in the user's documents and the row stays marked blocked with its question:
   objects intact (C6) and object signing (C7).

## Plan

Phases are in dependency order. Sizes are the sums of their rows.

**Phase 0: seams and plumbing, no new behaviour (C10, the Crucible side of C14). Size S to M. No decision needed.**
`BurstSink::start` takes a format enum; `DeviceFacts` and `EndpointFacts` gain `accepts_truehd` (always false from the real
probe for now); `fake_devices.hpp` records the format and can script `accepts_truehd`; the diagnostics line prints it.
*Risks:* a wide edit across tests that name `eac3`. *Proves done:* the 200 existing cases pass unchanged in meaning, on the
six legs; no new mode exists.

**Phase 1: the audio backends carry MAT (C11, C12, then C13). Size L to XL. Waits for Q6.**
`BitstreamFormat::kMat` (built by the Hearth session), an eight-channel carrier in `PassthroughSink`, `supports_mat_passthrough`, and per backend:
PipeWire first, since it is Crucible's Linux path and the Pi's receiver already published TrueHD in its codec list; WASAPI
second; Core Audio only if the owner wants macOS pursued. ALSA and Android serve forge and Hearth and are not Crucible's.
This phase belongs to whichever document owns `libs/audio`; it is listed so that the dependency is visible. *Risks:* the
carrier is a reading of the burst size and an eight-channel assumption; whether the Pi's HDMI output takes an
eight-channel high-bit-rate link is not in the repository; the queue sized for 20 ms bursts (C12) and the position
accounting that counts content frames per burst (1536 for both current formats). *Proves done:* backend unit tests with the
repository's fakes where they exist; then Phase 4.

**Phase 2: the TrueHD bed leg over fakes (C1, C2, C4, C5, C9, C15, C16, C20, C21, C22). Size M to L. Waits for Q1, Q2, Q4, Q7, Q8 and Q10; can run beside Phase 1.**
`OutputMode::kTrueHd51` (pin-only unless Q3 says otherwise), the leg in `OutputStage`, the `truehd` edge in
`projects.json` and the two CMake files, the policy cases and the stage cases. The stage cases have the oracle: feed a
known bed, `unwrap_mat_stream` the bursts, decode with `iclforge::truehd::Decoder`, and require equality with the rounded
24-bit input; a second case switches into the mode mid-stream and requires a major sync in the first burst. In the core
lane, a script writes the leg's bursts to a file and runs the existing FFmpeg strict decode on them
(`tools/checks/verify_truehd_containers.py` is the pattern). *Risks:* the gather and restart-interval latency; the engine's
`catchups` logic assumes a sink queue in samples (`kMaxSinkQueueFrames`); coverage floor. *Proves done:* the new cases on
all six legs, the coverage floor held, `check_layering.py` clean, and the FFmpeg leg green on Linux LLVM.

**Phase 3: window, runner, diagnostics, translations, documents (C17, C18, C19, C23, C3, C8). Size M. Waits for the mode's name (Q1, Q3).**
Pin list, tray menu, endpoint flag, mode note, "not applicable" text for bitrate and low latency, `crucible-run`, six
catalogues through `crucible_lupdate`, the pages in C23, the catalogue rows, ROADMAP and CHANGELOG, the screenshots
(`tools/pictures`). *Proves done:* the QML suites (141 cases at the last count, `docs/crucible/design/promotion.md`) pass
with the new entries; `generate_support_matrices.py --check`, `check_doc_paths.py` and the translation checks are clean.

**Phase 4: on hardware (C24, C25). Size M. Waits for Phases 1 to 3 and the owner's receiver time.**
The Pi 4B over PipeWire first (the receiver there advertised TrueHD on 2026-09-05), then the Windows machine. Record the
front-panel reading, the worst frame time and the underruns; measure the encode's share of a frame; revive or replace
`passthrough_probe` with a MAT fixture path (done: `crucible-probe`, 2026-11-01). Record the result in the promotion record, with what it does
not show (it names the format, not the correctness of the audio). *Risks:* the receiver may not lock on a stream whose
restart interval or downmix presentations differ from FFmpeg's; the tap-precision question (C4) needs a short spike with a
24-bit ramp through the driver.

**Phase 5: blocked, not planned (C6, C7).** TrueHD Atmos in Crucible waits for the owner's confirmation of the evolution
payload and for the library's T2b. When it unblocks, Crucible gets a mode, a second signing scheme beside the EMDF one (the
`SigningHook` holds one key today), a policy order, and a hardware reading. No size is given.

Phase 0 and the tap-precision spike need no decision. Phase 1 waits for Q6. Phase 2 waits for the owner's answers to
Q1, Q2, Q4, Q7, Q8 and Q10, and may start on the recommendations if the owner agrees to them. Phase 3 follows the name.
Phase 4 needs the owner's hardware.

## Open questions for the owner

1. **Does Crucible get a bed-only TrueHD mode now, while Atmos in TrueHD is on hold?** It gives a lossless 5.1 to a receiver,
   shows "TrueHD" on its display, exercises MAT end to end and puts the carriage in place for Atmos. It cannot carry
   placements as objects. *Recommendation:* yes, pin-only at first (see Q3).
2. **How wide is the bed?** Crucible's bed is five slots (L R C Ls Rs) and the LFE of a tap is dropped; the leg would take
   the E-AC-3 encoder's 5.1. A 7.1 bed needs seven bed slots, an LFE source and a panner for placed applications, none of
   which DD+ ever needed. *Recommendation:* 5.1 first; 7.1 is a separate decision.
3. **Where does the mode sit in the automatic choice?** Today: Atmos, DD+ 5.1, DD 5.1, PCM surround, headphones, stereo. A
   receiver that takes both TrueHD and E-AC-3 without a key would move from DD+ 5.1 to TrueHD if TrueHD ranked above it.
   *Recommendation:* pin-only until a receiver has read it, then rank it between Atmos and DD+ 5.1, so that a loaded key
   still means objects.
4. **Float to 24 bits.** Rounding or dither at the step into the encoder. It sets where "lossless" starts. *Recommendation:*
   plain rounding, and say so in the documents.
5. **Sample rate.** Stay at 48 kHz (the taps, the silent device, the encoder, the Atmos limit) or open the engine to 96 and
   192 kHz? The driver offers one format today. *Recommendation:* 48 kHz; treat higher rates as a separate piece of work.
6. **The MAT carrier.** Is eight channels at 192 kHz (176.4 kHz for the 44.1 family) the carrier a receiver expects over
   HDMI? It is arithmetic from the burst size, marked provisional in [ERRATA](../libs/containers/ERRATA.md), item 7. All
   of Phase 1 rests on it. *Recommendation:* take the Pi receiver's first reading as the check, and ask the owner to confirm
   or correct before the Windows format is written.
7. **Dialogue normalisation and mix levels in the major sync.** DD+ writes dialnorm 31. What should the TrueHD leg write in
   `channel_meaning`, and what do zero and the `*_control_enabled` flags mean to a receiver? *Recommendation:* write the
   encoder's defaults and read a receiver's behaviour; ask the owner what TH spec §14 intends before claiming "no level
   shift".
8. **Latency.** Accept that this mode has no low-latency variant (a floor near 27 ms with a restart interval of 8,
   against 5.3 ms for E-AC-3), as AC-3 has none; and use an interval of 8 rather than 128 for the live path, at about 3%
   more bytes. *Recommendation:* yes to both, after a receiver confirms that it locks on an interval of 8.
9. **The codec-path switch.** Should "Bypass the codec on headphones and PCM" mean anything for TrueHD? A lossless decode is
   the identity. *Recommendation:* no; the switch keeps its E-AC-3 meaning and the documents say the TrueHD mode has no
   decoded variant.
10. **Presentations and the bed's source.** `DownmixMode::kNone` or `kStandard` for the stream's 2-channel and 5.1
    presentations; and whether the leg keeps taking its bed from the E-AC-3 encoder (no new panning code; the E-AC-3 encode that
    the TrueHD path does not need costs about 0.8 ms of a 32 ms frame with four objects, per the encoder's own comment) or gets a panner of its own. *Recommendation:* `kNone` for the first
    receiver reading, since the oracle reads it; reuse the E-AC-3 bed and move the panner out only if the cost shows.
11. **Deferred until the evolution payload is confirmed:** one signing setting for both codecs or two (the ROADMAP says the
    TrueHD check must not go through `iclforge::ac3::signing`), and whether Crucible's "objects need a key" rule holds for
    TrueHD. Recorded here so that it is asked then and not settled in code. *Recommendation:* decide when C6 unblocks.

## Questions from the build

Raised on 2026-10-31 by the session that built phases 0, 2 and 3. Each has a recommendation and each is built the way the
recommendation says, provisionally.

12. **Should the TrueHD entry be offered in the Pin list and the tray while no backend can carry it?** The real probe
    answers false for TrueHD on macOS (and on a PipeWire sink that does not list it, or a Windows endpoint that refuses the format), so a pin on it falls back there,
    with the reason line saying why. The headphones entry is
    dropped where no renderer exists, on the argument that a list should offer what can be chosen. *Recommendation:*
    keep it, because the hardware phase needs the pin and the reason line is honest; revisit when phase 1 lands and the probe can
    answer, and hide it on a platform whose backend will never carry MAT.
13. **What should the mode do when the TrueHD encoder or the MAT packer stops?** It stops the output, adds the reason and
    leaves the mode at "none" until the next probe, as the spatial sink does. It could instead fall back to DD+ 5.1 at once.
    *Recommendation:* as built; a lossless stream that silently becomes a lossy one is the thing to avoid. The only way the
    encoder fails with a whole bed is a peak rate set too low (the setting is
    `OutputStageConfig::truehd_peak_data_rate_bps`, 18 Mbit/s, which the bed's raw 6.9 Mbit/s cannot approach).
14. **The LFE channel is silent in the live path.** The mode sends the encoder's bed, whose LFE is whatever the objects send
    there, which in Crucible is nothing: a tap's LFE is dropped, as for DD+ 5.1 (`bed_mixer.cpp`), and `encode_frame` has an
    overload that takes a source's own LFE that the engine does not call. *Recommendation:* feed it, for both modes, in one
    change of its own; a lossless 5.1 stream with an empty LFE is the first thing a listener with a subwoofer will notice.
15. **How is the first reading to be taken?** Phase 4 needs, on the Pi 4B over PipeWire, a build with phase 1 (question 6,
    unanswered), the restart interval at 128 and then at 8 (question 8), and the front-panel word. *Recommendation:* the owner
    answers question 6 first; the carrier is then one backend and one reading, and every other question here waits for that
    reading.
