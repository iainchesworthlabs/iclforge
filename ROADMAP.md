# Roadmap

Candidate and in-flight work only. This is not a commitment.

For what already ships, see [CHANGELOG.md](https://github.com/iainchesworthlabs/iclforge/blob/main/CHANGELOG.md) and each product's index page. For the
library's capability record, see
[`docs/library/capabilities.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/capabilities.md).
Detailed design lives in [`planning/`](https://github.com/iainchesworthlabs/iclforge/tree/main/planning)
and product `design/` records — linked below, not copied here.

Last reviewed: 2026-10-13. Reconciliation source:
[`planning/roadmap-inventory.md`](https://github.com/iainchesworthlabs/iclforge/tree/main/planning/roadmap-inventory.md).

## How to read this

| Status | Meaning |
|---|---|
| **In progress** | Active development on `main`, or on a named branch named below |
| **Partial** | Some of the scope is shipped; the row splits what is done from what is not |
| **Proposed** | Strong candidate or agreed direction; not started, or study only |
| **Blocked** | Wanted but waiting on an external dependency |
| **Out of scope** | Deliberately not doing (with reason) |

Sizes, where they still matter, are rough: **S** (an afternoon), **M** (a day or two), **L** (a
focused week), **XL** (several PRs).

---

## In progress

### Hearth

Hearth has no legacy roadmap IDs. Status is reconciled from
[`planning/hearth-reference-player.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/hearth-reference-player.md),
[`planning/hearth-android.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/hearth-android.md)
and [`docs/hearth/index.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/hearth/index.md).

| Feature | Done | Not done | Detail |
|---|---|---|---|
| Desktop player | Queue, transport, gapless, passthrough, meters, settings, diagnostics (saved, copied, a live view in Settings, a native debug channel on Windows and an opt-in loopback HTTP endpoint) (`apps/hearth/engine/`; `[hearth]` tests); the Qt window (Play, Media, Speakers, Decoder, Network, Settings and Karaoke pages); raw streams and the first AC-3, E-AC-3 or AC-4 track of an MP4, Matroska or MPEG-TS file; AC-4 playback of channel-based, immersive and object streams (A-JOC and direct-coded); a file named on the command line or opened from the desktop plays, and a second launch hands its files to the window already running; a package component for Windows, macOS and Linux (the macOS release `.dmg` does not carry it); a user guide with a picture of each page; the window passes AC-3 and E-AC-3 to a receiver chosen in its output picker as IEC 61937 bursts, and decodes for that output what the receiver does not take; the output chosen there is kept across starts, and at a start where the receiver is not listed the window decodes to the default output instead | A run of the window's passthrough into a receiver (CI plays it into a stand-in link that reads the bursts back, and `forge play` has run the same path into a receiver); playing to the kept network group before the network has found it (the item is refused until the group is ready); pictures of a paired sink and of a group on the Network page; the other tracks of a container; removing an item from the queue; listing AC-4 objects on the Play page; a Pro Logic II control on the Decoder page; a decoder-delay trim at the start of an AC-4 item (a gap of about 27 ms between two in a queue); the words of every page but Karaoke in the six other languages (the catalogues hold every string, and about 190 of 1,079 are translated) | [Hearth index](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/hearth/index.md), [user guide](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/hearth/desktop-player.md) |
| Karaoke (Annex C) | Hearth: a Karaoke page with the three reproductions, the listener's choice of vocals and a level for the melody and each vocal, word-timed lyrics from `.lrc`, SubRip, Advanced SubStation Alpha and MIDI karaoke files, a CD+G disc's picture, duets (names, sides and who the guide is), a window that times the words and the singers of a file, a full-screen singer view and a lyric delay, and a microphone that is off until it is turned on, with a pitch score against the melody guide or the vocal taken away, a pitch ribbon, a mark under each line sung, a summary, and takes of the voice kept as FLAC files; the Forge GUI: a Karaoke flow that builds a stream from a backing, a guide melody, one or two vocals and an LFE part and plays it back under each reproduction; `forge encode`, `probe` and `decode` (the aware and capable decoders), `lyrics=` on the encoders, which writes the lyrics beside the stream as UTF-8 `.lrc` (from a file in a code page with `lyrics-encoding=`), and `lyrics-to-lrc`; the same reproduction for an E-AC-3 stream flagged karaoke, by analogy and as Dolby's decoder does, in the decoder, Hearth, `forge decode` and `monitor`, the ESP32-S3 sink and the ESP32-P4 at 2.0 on a v1.x chip, which reproduce it themselves (the C6 decodes two channels, so Hearth decodes for it), and the browser decode demo, with `forge eac3-encode` writing the mixing group it is levelled by; a browser decode demo, with a microphone card, a pitch score and a mark on each line sung; Hearth for Android's lyric screen, which follows the lyrics a server sends on the sink's own clock; the ESP32 sinks, which decode a karaoke stream themselves and follow its lyrics on their own clock, and the P4 sink, which draws them (built and drawn on a board with no panel, an image for a v3.x chip with a 7-inch panel); Crucible's lyric strip, which imports the text formats and a CD+G disc's picture, follows an application's media session where it publishes one (Windows and Linux) and can say its line and word over OSC; the library API | Karaoke in AC-4 (its specification has no karaoke mode; [the options](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/karaoke-ac4-options.md) are written down and none is built); the P4 lyric screen run on a real panel and a v3.x chip (the kit has not arrived); a karaoke stream decoded on Android (Hearth for Android passes the stream to the receiver; phase 4 of [its plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/hearth-android-phases-4-5.md)); a media-session reader for macOS that has run on a Mac (the code is there and is unverified, and developers report the framework withheld from macOS 15.4); lyrics carried inside an MP4 or Matroska file; a microphone on a board (M8 to M10 of the microphone plan, which wait for its decision D3); saving a performance as a karaoke stream in one step (the guide gives the steps by hand) | [User guide](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/karaoke/index.md), [plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/karaoke.md), [microphone plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/karaoke-microphone.md) |
| Network output | Sendspin discovery, pairing, groups and playing to a group from the app; a group whose members take different forms, each sent the coded stream, PCM at its own layout or Opus as it lists, with the group editor showing what each member is sent and why; a sink reporting how it plays the stream, a server telling its members what is playing, and a sink asking the server for the karaoke choice; updating a sink's firmware from the app; aiosendspin CI exit | Music Assistant tested against a real instance | [Sendspin extension plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/hearth-sendspin-extension.md), [what each sink is sent](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-sink-compatibility-matrix.md) |
| Hearth for Android | A Sendspin sink for Android TV (`apps/hearth/android`; phases 0 to 3 of its plan and the protocol additions D6 to D8): advertised, paired with from Hearth, an AC-3 or E-AC-3 stream passed to the receiver as the IEC 61937 bursts it came from, the lyric screen, the controls sheet, the starting, idle and attention screens, and a pairing code drawn as a QR; one run on a Shield and a receiver recorded; CI builds the debug APK and runs its instrumented tests; the decode a sink plays is a library (phase 4a) | Decoding on the device, for karaoke and for a route that refuses a format (phase 4); playing as an ordinary `player@v1` player (phase 5: a PCM, FLAC or Opus stream is accepted and dropped today); a page for it on the documentation site; a release package; AC-4, which no receiver here has decoded over this route | [App README](https://github.com/iainchesworthlabs/iclforge/blob/main/apps/hearth/android/README.md), [plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/hearth-android.md), [phases 4 and 5](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/hearth-android-phases-4-5.md) |
| ESP32 sinks | `hearth_sink` on the ESP32-S3, the ESP32-C6 (stereo) and the ESP32-P4 (revision 1.x; an image for a v3.x chip with a panel runs on a P4X kit, with its screen, touch, a 16-slot TDM play and a karaoke play with lyrics working, and AC-4 5.1 measured on it; the clocks on the TDM pins are not yet measured, and tearing was only looked at): Improv, groups, updates over the network, QEMU CI (S3); one locked build profile per board, with a board suite and recorded baselines; a karaoke stream decoded on the board with its lyrics followed on the board's own clock; AC-4 decoded in a Sendspin group by a build with the decoder (a P4 to 5.1, an S3 at 2.0 up to 25 frames a second; a C6 is sent PCM); firmware images for each board, published from the next release | TDM DAC hardware exits (ES9080 pair); TDM on the P4 (this board's chip revision cannot open it), and with it the wide P4 sink; AC-4 in the published images, which have the decoder off | [ESP32-S3 sink guide](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/hearth/sink-esp32-s3.md), [Sink firmware](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/hearth/sink-firmware.md), [build profiles](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-platform-profiles.md) |

**Sink module tiers (shared PCB → pair of ES9080s):** **OK** C6 (2.0, shipped, one DAC) ·
**good** C61 (5.1 desired, proposed, no board) · **better** S3 (7.1.4 without enhanced coupling,
both DACs @ 16-bit) · **best** P4 (9.1.6 + full tools desired, both DACs @ 32-bit on one I2S).
Study:
[`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-sink-tiers.md).

**Not built:** the C61 tier (no board, no probe, no sink shape); TDM on the P4, which this board's revision v1.3
chip cannot open above two channels; the ES9080 pair and its PCB.

**Done:** ESP32-P4 probe and board timing table, no network (tier study P1) — real time on every
fixture at this board's 360 MHz — and `hearth_sink` on the P4 over the onboard C6 (P3, 2026-09-23).
[`docs/platforms/bare-metal/esp32-p4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/platforms/bare-metal/esp32-p4.md).

### Library — AC-4 decode and encode (Partial)

The decoder, the encoder, the IEC 61937 carriage, the applications, the decoder on the ESP32
parts and AC-4 in a Sendspin group on the ESP32 sinks are built, and the tree has the new
names. What ships is in
[CHANGELOG.md](https://github.com/iainchesworthlabs/iclforge/blob/main/CHANGELOG.md) and
[AC-4 in the library](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/ac4.md);
this table keeps what is unfinished.

| Layer | State |
|---|---|
| Inspector, decoder and encoder (`libs/ac4`) | **Shipped** (phases D1 to D10 and E1 to E10 of `planning/ac4.md`). Decode to PCM: mono, stereo, 3.0, 5.X and 7.X in every codec mode Part 1 gives them (SIMPLE, ASPX and the A-CPL modes), the immersive element of 7.0.4 and 7.1.4 in full and core decoding, the 22.2 element in full decoding as coded (24 channels, checked on constructed streams alone), A-JOC and direct-coded objects with their metadata, streams of several presentations, every frame rate (the efficient high frame rate mode included), the speech spectral frontend, the output level, DRC, dialogue enhancement, the downmix and concealment. Encode: mono, stereo, 5.0 and 5.1 in the SIMPLE, ASPX and A-CPL modes, 5.0.4 and 5.1.4 in the immersive element, several substreams and the presentations of Part 2 Table 53, every frame rate, constant, average and variable rates, I-frames and the metadata, and, behind `experimental.objects`, A-JOC and direct-coded objects. The three libraries are installed and exported ([AC-4 in the library](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/ac4.md), [encoding a stream](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/ac4.md#encoding-a-stream), [encoding objects](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/ac4.md#encoding-objects)) |
| What they refuse, and what is experimental | The decoder refuses, by name, core decoding of 22.2 and its rendering to any layout but as coded (Part 2 gives it neither), and, at 96 or 192 kHz, everything but the SIMPLE codec mode (A-SPX and A-CPL, the speech spectral frontend, the immersive and 22.2 elements, objects, the mixing of a presentation's substreams, dialogue enhancement and DRC's compression curve; the output level and the downmix apply). The speech spectral frontend, the efficient high frame rate mode, the 9.X.4 modes and the SIMPLE mode at 96 and 192 kHz are decoded from the text alone, no stream from outside the project using any of them. The encoder writes as experimental options what DEE's streams do not contain and no reader outside the project has read from it: 7.0 and 7.1 in the 7.X element, 3.0, the 5.X element's other coding configurations, ASPX_ACPL_1 and A-CPL in stereo, balance coding, VARVAR framing and frequency interleaving in A-SPX, 7.0.4 and 7.1.4 with the back pair, A-JCC, objects, spectral noise fill, the efficient high frame rate mode (at a constant rate), 9.X.4 and 22.2 |
| IEC 61937 carriage (`iclforge::containers::iec61937`, `PassthroughSink`, Hearth's extension role) | **Built** (phase D11) — the four IEC 61937-14 burst types at every frame rate, read back unchanged; passthrough on ALSA and Android, the platforms whose APIs can send AC-4; the extension role's AC-4 data type, decoded by the test sink and by an ESP32-P4 or ESP32-S3 sink built with the decoder; no receiver found accepts AC-4 |
| Applications (`forge`, Hearth, Forge GUI, bindings) | **Shipped** (phases I1 to I5b). `forge` reads and writes AC-4 in `decode`, `ac4-encode`, `transcode`, `record`, `live`, `monitor`, `play`, `qc`, `levels`, `loudness`, `spdif`, `probe`, `mp4`, `ts`, `fmp4`, `atmos-encode`, `atmos-adm` and `atmos-iab`; `mkv` refuses it, since Matroska registers no codec ID. Hearth's desktop player plays channel-based, immersive and object AC-4, decoded for every output and sent as bursts to network sinks that list it, with the Decoder page's AC-4 controls and the Media page's AC-4 information. The Forge GUI encodes AC-4 from a file (mono to 5.1, or objects, to a raw stream or an MP4 file), and its QC, player, Open stream and object pages read it. The C API and the Python, Rust and WebAssembly bindings decode and encode it, objects included. Android's CMake wrapper builds the AC-4 library; the Shield app does not link it. **Not done:** an AC-4 live session in the GUI; Pro Logic II from Hearth's Decoder page; a WebAssembly demo page for the AC-4 module; the encoder's substreams, presentations, loudness, DRC, downmix and dialogue groups in the bindings; a gap of about 27 ms between two AC-4 items in a Hearth queue, since the decoder's delay is not trimmed at an item's start |
| ESP32 (`float` on the P4 and the S3; fixed point on the C6 and the C3) | **Built** (plan phases D14a to D14i, and I6). The ESP-IDF component builds the decoder behind `CONFIG_ICLFORGE_AC4`, in `float` or in fixed point by target, as AC-3 and E-AC-3 are, and the PCM of the pinned fixtures is the host's on each part. An ESP32-P4 at 360 MHz with Wi-Fi up keeps up at 2.0 in every mode and frame rate and at 5.1 in all four codec modes (0.42 to 0.63 of a frame), and takes 5.1.4 at 0.94 to 1.15 ([ESP32-P4](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/platforms/bare-metal/esp32-p4.md#ac-4)). An ESP32-S3 keeps up at 2.0 (0.40 in SIMPLE mode, 0.50 with A-SPX) and through the frame-rate converter, and takes 5.1 in SIMPLE mode at 0.99 ([ESP32-S3](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/platforms/bare-metal/esp32-s3.md#ac-4)). The fixed-point tier gives one hash on the host, the Cortex-M3 leg and RV32IMC; a 2.0 decode needs 286,365 bytes, which a C6 does not have beside Wi-Fi, so a C6 refuses the play and its sink takes AC-4 programmes from Hearth as PCM ([ESP32-C6](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/platforms/bare-metal/esp32-c6.md#ac-4)). `hearth_sink` decodes AC-4 sent in a Sendspin group (I6): a P4 lists `ac4` at 6 channels, an S3 with the 64 KB data cache at 2 channels and 25 frames a second, neither with objects, and a server decodes what is past those and sends PCM. **Not done:** the decoder in the published firmware images; 5.1.4 in real time on the P4 with A-SPX; 5.1 with A-SPX or A-CPL on the S3; a presentation's objects decoded on a board |

Plan: [`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md).
Its first phase (G0) made the DEE reference streams the others need, and G1 made the golden masters
for the phases after it; the local DEE licence ends on 2026-11-06 and will not be renewed, so a
stream DEE has not been asked for cannot be made after that date.

### Library — API freeze → v1.0.0 (was AP1, L)

**Done:** tiering and SemVer policy in
[`docs/library/api-stability.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/api-stability.md);
C version macros; release criteria written.

**Not done (deferred to the v1.0.0 cut):** flip `SOVERSION` to the major component; introduce
`inline namespace v1`; make the ABI gate **required** (today advisory — see `_ci-core.yml`
`ABI_ENFORCE`).

### Library — TrueHD (MLP) codec (was IM5, L)

**Done (branch `feature/truehd-codec`, not yet on `main`):** `iclforge::mlp` in `libs/mlp`, behind
`ICLFORGE_BUILD_MLP`, written fresh from the clean-room specification `truehd-spec.md` with FFmpeg
as a black-box arbiter (the earlier internal codec on `feature/truehd-atmos-support` was not used):
the bitstream layer, a decoder of every presentation (2, 6, 8 and 16 channels, 44.1 to 192 kHz), a
lossless encoder (presentation downmixes, inter-channel decorrelation, FIR and IIR filter search,
exact Huffman costing, block-size choice, a FIFO schedule against `peak_data_rate`, self-check by
decoding) and Atmos (a 7.1 bed and up to eight objects in the 16-channel substream, OAMD in
evolution payloads). forge has `truehd-encode`, `truehd-atmos-encode`, `truehd-atmos-decode`, and
TrueHD in `decode`, `probe` and `transcode`, and writes and reads Matroska `A_TRUEHD`. Every
specification problem found, and the reading taken, is in
[planning/truehd.md](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd.md)
and `libs/mlp/ERRATA.md`. **Not done:** IEC 61937 data type 22 (MAT) and MPEG-TS stream type 0x83
carriage (the specification gives no MAT frame syntax); the noise generator (none is specified, so
streams that use it are refused); Forge GUI front ends (was UX10).

**Authenticity:** the specification (section 22) says TrueHD uses the `pace-ccjf1` ECDSA content
signature, carried as an evolution payload; earlier notes here and in
[Object signing](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/concepts/object-signing.md#sibling-truehd-evolution)
said a truncated HMAC-SHA-256 "Evolution frame protection". Neither is implemented: the codec carries
a signature payload (`EvolutionPayloadType::kContentSignature`, `SignatureHook`) for whichever the
owner decides (planning/truehd.md, decision 1).

### Crucible — cross-platform product (was UX12, Partial)

**Done:** rename to Crucible; Windows and Linux verified on real hardware (Pi PipeWire pass,
receiver display read); Linux packages; macOS platform half **compiles and runs headless suites
in CI**.

**Not done:** launch Crucible interactively on a Mac with audio; macOS package; native-speaker
translation review (CR1); attestation-signed Windows driver (see DR6).

Detail: [`docs/crucible/design/promotion.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/crucible/design/promotion.md).
Current product status:
[`docs/crucible/index.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/crucible/index.md).

### Shared audio — macOS process tap (was UX7, Partial)

Code is in `libs/audio/src/backend/macos/process_tap.mm` and compiles on macOS CI legs. The path
is **refused by default** because `AudioDeviceCreateIOProcID` hung the HAL client in Crucible's
first real run; opt-in via `ICLFORGE_MACOS_PROCESS_TAP`. No Mac has captured audio through a tap
yet.

### Shared — hardware verification (was DR9, Partial)

| Backend | State |
|---|---|
| Linux / ALSA passthrough | **Confirmed** on Raspberry Pi → AVR |
| Linux / PipeWire | **Confirmed** on Pi; Crucible live path verified |
| Windows / WASAPI exclusive | **Confirmed** on Onkyo TX-RZ740 |
| macOS / CoreAudio tap and desktop apps | **Not verified** on real Mac hardware |
| Pi 5; second Android TV | **Outstanding** |

---

## Partial tails on shipped work

These shipped but have an open follow-on. They do not belong in "In progress" as whole features.

| Topic | Shipped | Follow-on |
|---|---|---|
| **IAMF** (was IM3) | `libs/containers/src/iamf`: v2.0 reader and writer — channel-based and object-based `ipcm` elements, scalable channel reconstruction, Opus / AAC-LC / FLAC packets carried, Parameter Blocks, trimming, raw OBU streams, ISO-BMFF and fragments | Nothing planned. Known and accepted: encoding and decoding Opus, AAC-LC and FLAC (the module links no codec; packets are carried), Common Encryption (needs AES, and the module has no third-party dependency), rendering to a playback layout and mixing (section 7.4 leaves the algorithms to the Open Audio Renderer) |
| **IAB** (was IM1) | Reader, `AudioDataDLC` decode, writer and DLC encoder, spread and zone control into the Atmos bridge, ST 2067-201 MXF Track File write | A Track File checked by an IMF packager or validator |
| **Object authenticity modes** | `iclforge::ac3::signing` HMAC tag; `sign-objects`; `verify-objects` with one or several `signing-key=` (a keyring: a frame is good under any key; hard fail); `gate-objects`, the licensed soft-gate (tag mismatch / unsigned → bed-only, decode continues); default decode reconstructs objects **unchecked** (FOSS-style) | Nothing planned. Known and accepted: this project ships no key, so a Dolby-licensed decoder accepting its objects is a personal-use path only — [Object signing](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/concepts/object-signing.md#decode-modes). TrueHD's content signature (IM5) is carried as an evolution payload, scheme undecided, not EMDF |
| **Multi-programme E-AC-3** | `programme2=`..`programme8=` authoring via CLI (all eight §E2.3.1.2 substreams, full per-programme `mixmdate`/`bsmod` metadata); receiver-side mixing of an associated service into the main (`AssociatedServiceMixer`, `forge decode associated=`: §E3.10's scales, per-channel trims and pan) | Playing it live in Hearth: `forge monitor associated=` mixes an associated service into the main as it plays, and Hearth does not yet — see [capabilities](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/capabilities.md) |
| **Encoder reproducibility** (was VX12) | Audit done; `ilogb` fix landed; sixteen real-programme streams in the bitstream-hash gate, identical on MSVC, clang-cl, GCC 16 and Clang 22 on the x86-64 and generic kernels, Linux arm64 and macOS (libc++), after the delta-segment selection stopped depending on `std::nth_element`'s tie choice | The float32 encoder's pins, which no longer match a fresh GCC build; fixed-point transient port optional |
| **Listening test** (was VX9) | Apparatus in `tools/listening/` | **No session run yet** — see [`tools/listening/responses/README.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/listening/responses/README.md) |
| **Crucible translations** (was CR1) | Six catalogues complete (421 messages each) | Native-speaker reading of mechanical translations |

---

## Proposed

| Item | Notes | Was |
|---|---|---|
| Generated API reference and versioned docs | Doxygen into mkdocs; `latest` / `dev` branches | AP8 |
| GStreamer element or FFmpeg external encoder for >5.1 / JOC encode | Out of tree, over the C API; AP5 (C API) is done | AP10 |
| Dolby Reference Player wider CI crosscheck | Extend beyond `none/cpl/spx/aht/all`; self-hosted Windows job | VX5 |
| Perceptual encoder criterion calibration | EQ13 follow-on; `kPerceptual` | EQ14 |
| QC delivery report file | `forge qc` writes stdout today | [`planning/qc-report.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/qc-report.md) |
| DAW / NLE host plugin | Feasibility study only | [`planning/host-plugin.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/host-plugin.md) |
| ESP32 sink tiers (C6 / C61 / S3 / P4) on one ES9080 PCB | Modular MCU: C6 2.0 / one DAC (shipped); C61 5.1 desired (no board); S3 ≤7.1.4 no ecpl / both DACs @ 16-bit (shipped); P4 ≤9.1.6 full tools desired / both DACs @ 32-bit on one I2S. P4 reopened only as best tier; its probe and hosted-Wi-Fi sink are built, its TDM output and the PCB are not | [`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-sink-tiers.md) |
| vcpkg git registry | Consumers install via `vcpkg install iclforge` | DR3 |
| winget and ConanCenter | Manifests staged; CLA and submission pending | DR4 |
| AC-4 in the performance and quality reporting | **Partial** — stereo and 5.1 encode/decode in `iclforge-perf`, `iclforge-bench` and `iclforge-membench`; AC-4 kernels in `iclforge-kernelbench`; decode quality in `ac4-quality-main.jsonl` with a chart on [Quality trend](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/quality-trend.md#ac-4-decode-quality). Still open: encode quality trend and tool-comparison series. The merge queue compares speed and memory for a change to `src/`, AC-4's rows included (#1132). **M** | [Performance and quality](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/performance-quality.md) |
| Names and layout (phase N1 of `planning/ac4.md`) | The tree has the new names. The programs are `forge`, `forge-gui`, `hearth` and `crucible` (N1A, #1164), the libraries have the targets `iclforge::<library>` and the headers `iclforge/<library>/` (N1B S2 to S4, #1160 to #1162) and, since the monorepo move (#1199), are 16 projects under `libs/` beside `apps/`, `bindings/` and `firmware/`, the pages, addresses and package metadata follow (S5), and the AC-3 codec's C++ names are in `iclforge::ac3`, a peer of `iclforge::ac4` (S6). The Windows driver is `IclForgeNullSink` and its endpoint "Speakers (Crucible Silent Output)" (N1D). [Renamed](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/renamed.md) lists every old name beside its new one. The repository and the Homebrew tap are renamed. **Not done:** the SonarCloud project key (still `iainchesworthlabs_ac3forge`) and the first release under the new names, which publishes the PyPI project and the renamed formula and cask; `iclforge` is a new PyPI project and the old one stays as it is, and the Homebrew formula and cask are renamed (decisions 40 and 41); U1 revises wording that is not a name. **L** | [`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#n1-the-names), [`planning/layout.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/layout.md), [`planning/monorepo.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/monorepo.md) |

---

## Blocked

| Item | Blocker | Was |
|---|---|---|
| TrueHD interoperability with Dolby's decoders | FFmpeg's decoder reads every stream `iclforge::mlp` writes, exactly (`tools/checks/verify_truehd_interop.py`); a Dolby decoder has not been tried, and the DEE streams that would test the decoder side (`tools/checks/verify_truehd_dee_gold.py`) are on the owner's machine (IM6) | IM6 |
| macOS notarisation and Windows Authenticode | Certificates and accounts, not code (Known gap since 0.8.0-beta.2) | DR6 |

---

## Out of scope

- **Forging Dolby's authenticity tag** — see [`docs/concepts/object-signing.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/concepts/object-signing.md). The signer ships; the key is the operator's. Multi-key verify and the licensed soft-gate use **operator-provisioned** keys only — they do not recover or invent decoder secrets.
- **AC-3 VBR** — structurally impossible; frame size indexes a fixed table.
- **Room correction and equalisation** — covered by [Cavern](https://github.com/VoidXH/Cavern). Hearth renders to the speakers and manages them (per-output trim and delay, a bass-management crossover) and does no measurement, equalisation or filtering beyond the crossover. A headphone/binaural preview for the WASM demo stays off unless that boundary is redrawn on purpose.
- **A DAMF reader** — no public specification; IM1 / ADM BWF is the replacement.
- **TrueHD interoperability by black-box analysis of Dolby streams** — not until the clean-room rule explicitly allows it.
- **An external oracle for `fscod2` audio** — FFmpeg and Dolby Reference Player refuse it.
- **Perfect separation of co-directional objects** — property of parametric object coding.
- **Enabling PipeWire `iec958Codecs` on the user's behalf** — session-manager policy; documented instead.
- **HOA, Matrix and Binaural ADM pack types in the Atmos bridge** — refused with `kUnsupportedType`: `AtmosEncoder` takes positioned mono objects, and none of the three is one. The parser reads all three, a Matrix block's coefficients and its pack's references included (from the axml text, since libadm skips Matrix blocks), and the writer writes them; only the bridge refuses.
- **AC-4 in Matroska** — Matroska registers no codec ID for AC-4, so `forge mkv` refuses it; MP4, CMAF and MPEG-TS carry it.
- **AC-4 beyond the plan's scope** — transcoding between AC-4 and AC-3 or E-AC-3 without decoding (`forge transcode` goes through PCM); rendering objects inside the decoder, headphone virtualisation and head tracking; presentations spread over several elementary streams; protected (encrypted) tracks; MPEG-TS under ATSC A/342-2's profile (DVB's is carried); the speech spectral frontend, 96 and 192 kHz, A-SPX time-interleaved waveform coding and bitstream versions 0 and 1 in the encoder (accepted gaps, each with its reason in the [development status](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/development-status.md) page); writing immersive stereo (`presentation_version` 2); the encoder on any ESP32; and AC-4 in Crucible, which captures applications into Atmos objects for receivers when no receiver accepts AC-4. The decoder's speech frontend, 9.X.4 modes and 96 and 192 kHz decoding wait for a stream that uses them. See [Deliberately not in scope](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#deliberately-not-in-scope).
- **APT/DNF repositories and Docker images** — not planned; see [`docs/releasing.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/releasing.md).
- **ESP32-P4 as a replacement for the S3 Wi-Fi Sendspin sink** — closed 2026-09-08 (no on-die radio; no float PIE win; S3 probe already real-time). **Complementary P4 “best” module** (Ethernet / hosted C6, dual ES9080 @ 32-bit) is partly built: the probe and the hosted-Wi-Fi sink exist, TDM and the ES9080 pair do not — see [`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-sink-tiers.md) and [`esp32-c3.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/platforms/bare-metal/esp32-c3.md#why-not-the-esp32-p4).
- **Per-channel and per-block SNR offsets** — tried and declined (EQ2); reference encoders agree with shipped behaviour.
- **Multi-service (multi-PID) MPEG-TS authoring** — one PMT with a main-service PID plus associated-service PIDs, built in one invocation. `iclforge::containers::mpegts::mux` stays a single-elementary-stream muxer (its own header comment calls a general multiplexer out of scope); `mainid`/`asvc` describe links an operator authors across separately-muxed files, not a multiplex this tool builds for them.

---

## Legacy ID index

Old IDs (`EQ1`, `UX12`, `A1`–`G4`, planning chips) are **retired for new work**. They remain here
so old PRs and comments still resolve. Do not allocate new IDs.

<details markdown="1">
<summary>2026-08-15 roadmap → v0.9.0 carry-forward (single-letter IDs)</summary>

| ID | Item | |
|---|---|---|
| A1–A5 | Container mux / CLI streaming | merged → IO* |
| B1 | ADM BWF → JOC | merged |
| B2 | DAMF reader | out of scope → IM1 |
| B3 | IAMF | merged phase 1 → IM3 |
| C1–C4 | Metering / QC / dialnorm | merged → IO*, DC* |
| D1 | TrueHD branch | superseded: rewritten on `feature/truehd-codec` → IM5 |
| D2–D4 | Decoder / AC-4 inspect | merged → EQ4, IM4 |
| E1–E4 | Audio backends / CI | merged → DR*, audio |
| F1–F6 | C API / bindings / packages / API freeze | merged or in progress → AP*, DR*, UX* |
| G1–G4 | Verification / fuzz | merged → VX* |

Full ledger text preserved in git history of this file before 2026-09-17.

</details>

<details markdown="1">
<summary>Theme IDs (EQ, DC, IO, IM, VX, PF, AP, UX, CR, DR) — disposition summary</summary>

| Theme | Shipped (removed from this file) | Still active (see sections above) |
|---|---|---|
| EQ | EQ1–EQ13 encoder quality work | EQ14 proposed; EQ2 out of scope |
| DC | DC1–DC10 decoder and stream tools | Multi-programme mix metadata tail |
| IO | IO1–IO12 containers, QC, loudness | QC report file proposed |
| IM | IM1–IM4, IM7 | IM5 done on its branch; IM6 blocked on Dolby decoders; IM3/IAB tails |
| VX | VX1–VX23 except VX9/VX12 tails | VX9/VX12 partial; VX5 proposed |
| PF | PF1–PF8 performance | — |
| AP | AP2–AP7, AP9, AP11–AP12 | AP1 in progress; AP8/AP10 proposed |
| UX | UX1–UX6, UX8–UX9, UX11 | UX12/UX7 partial; UX10 partial (CLI done, GUI not) |
| CR | — | CR1 partial |
| DR | DR1–DR2, DR5, DR7–DR8 | DR9 partial; DR3/DR4 proposed; DR6 blocked |

For per-item detail on shipped work, see [CHANGELOG.md](https://github.com/iainchesworthlabs/iclforge/blob/main/CHANGELOG.md).

</details>

---

Rebuilt 2026-09-17 from [`planning/roadmap-inventory.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/roadmap-inventory.md)
and a repository survey. Previous version (~3,100 lines, theme sections with full shipped
records) is in git history at `a7d3bd56` and earlier.
