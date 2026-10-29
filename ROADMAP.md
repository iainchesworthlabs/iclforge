# Roadmap

Candidate and in-flight work only. This is not a commitment.

For what already ships, see [CHANGELOG.md](https://github.com/iainchesworthlabs/iclforge/blob/main/CHANGELOG.md) and each product's index page. For the
library's capability record, see
[`docs/library/capabilities.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/capabilities.md).
Detailed design lives in [`planning/`](https://github.com/iainchesworthlabs/iclforge/tree/main/planning)
and product `design/` records — linked below, not copied here.

Last reviewed: 2026-10-13; the TrueHD, AC-4 content signing and Common Encryption rows, 2026-10-29. Reconciliation source:
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

### Library — TrueHD module (was IM5, Partial)

`libs/truehd` (`iclforge::truehd`, `ICLFORGE_BUILD_TRUEHD`, on by default) is built and checked against FFmpeg. It is written from the
owner's TrueHD (MLP) Reference Specification, which is not in the tree, and every place that specification and the streams disagree is an entry
or an open question in [`libs/truehd/ERRATA.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md). The plan is
[`planning/truehd.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd.md); what ships is in [CHANGELOG.md](https://github.com/iainchesworthlabs/iclforge/blob/main/CHANGELOG.md) and
[TrueHD and MLP in the library](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/truehd.md). This table keeps what is unfinished. The branch `feature/truehd-atmos-support`
is superseded: no TrueHD decoder reads its block layout and it carries non-redistributable PDFs, so nothing is taken from it.

| Layer | State |
|---|---|
| Decoder and encoder (`libs/truehd`) | **Built** (phases T1 and T2a of the plan). The decoder reads TrueHD and MLP: substreams 0 to 2 and a generic 16-channel substream 3, 44.1 to 192 kHz, 16 and 24 bits, bit-exact with FFmpeg's strict decode on its 25 committed fixtures; an IIR filter, LSB bypass, noise generator 0, several matrices, the block data check and extra data are checked against 15 hand-built streams, of which FFmpeg's decoder reads 11. The encoder writes TrueHD, 1 to 8 channels of 16, 20 or 24 bits, and FFmpeg's strict decode returns every sample in 43 cases and a fuzzer. **Not done:** noise generator 1 with a non-zero noise shift or an LSB bypass (the specification has no table; refused), heavy DRC (the streams do not show where its bits sit), and in the encoder an IIR filter, LSB bypass, a negative output shift, heavy DRC, noise and MLP; nothing outside this library reads the encoder's 2 and 6 channel downmix presentations |
| Containers and carriage (`libs/containers`) | **Built** (phase T2c1). MP4 (`mlpa`, `dmlp`), Matroska (`A_TRUEHD`), MPEG-TS (`stream_type` 0x83, Blu-ray profile) and MAT (IEC 61937 data type 22), in both directions, bit-exact with FFmpeg's muxers. **Not done:** every layout has one oracle, FFmpeg, and FFmpeg's `spdif` demuxer reads no MAT stream, so reading MAT has none; nothing has been played into a receiver or a disc player; an M2TS writer and the TrueHD-plus-AC-3-core arrangement of a disc ([`libs/containers/ERRATA.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/containers/ERRATA.md)) |
| Applications (`forge`) | **Built** (phase T2c2). `decode`, `probe`, `levels`, `loudness`, `mkv`, `mp4`, `ts`, `spdif`, `unspdif`, `demux`, `remux` and `truehd-encode`; the codec matrix, the gold-reference gate and the FFmpeg Validate job hold them to FFmpeg's strict decode. **Not done:** `transcode`, `qc`, `fmp4`, `monitor` and `play` refuse a TrueHD input; the Forge GUI, Hearth and Crucible do not link the library; the project file does not name `truehd-encode`; `truehd-encode` refuses an `.mlp` name |
| C API and bindings | The C API (`iclforge_truehd_*`, decode and encode) is **Built**. **Not done:** the Python, JavaScript (WebAssembly) and Rust bindings (was UX10); each is a follow-up on the C API alone |
| Atmos substream and object audio metadata (phase T2b) | **Blocked** on the owner's answers about the evolution payload ([`libs/truehd/ERRATA.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md), open questions 1 and 2, and 22 to 39 for the v3.6 syntax). The library reads the extra data's evolution frame into plain structures, provisionally, and `forge probe` reports it (`detail=format` is the check to run on a disc's stream); nothing decodes the objects and no Atmos stream is written ([the plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd-atmos.md)). |
| Content signing for TrueHD | **Not built.** The owner's specification describes the evolution payload as the container AC-4's content signature uses, but lists its field IDs and the digest scope as unconfirmed (§16.3.3). `iclforge::base::crypto` is codec-neutral and would carry it; see the AC-4 content signing row below and [Signing](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/signing.md#the-seam-for-other-codecs) |
| Scoring against Dolby Encoding Engine | `tools/checks/score_truehd_decode.py` has been run on streams this tree made and not on DEE's. DEE's licence ends on 2026-11-06 and is not renewed; the set of known-source TrueHD streams (2, 6 and 8 channels, 48 and 96 kHz, 16 and 24 bits, several presentations) is made by `tools/generators/gen_dee_gold.py` and stays on the owner's machine |

Parity with E-AC-3 in every product and tool is the next section.

### TrueHD and E-AC-3 as equals (Proposed)

The owner's direction is that TrueHD and E-AC-3 (DD+) are equal first-class citizens: where one has a row, the other has a row with its
honest status. Round 1 of the audit compares each area with E-AC-3 and writes a planning document per area; this section links each when it
lands and takes its sizes (the scale above) from it. No implementation below is started.

| Area | Today | Wanted | Plan |
|---|---|---|---|
| Forge CLI | The commands in the row above; the help and the usage text name TrueHD and MLP, and `cut`, `cat`, `metadata`, `normalize`, `transcode`, `qc`, `fmp4`, `monitor` and `play` refuse a TrueHD stream by name | The refused commands, MLP encoding, the project file | [Not done in T2c](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd.md#not-done-in-t2c); parity audit |
| Forge GUI | Nothing: no page opens, plays, inspects, checks or encodes TrueHD | An Open stream, QC, player and encode route for TrueHD | Parity audit |
| Crucible | Not linked; Crucible uses the AC-3, E-AC-3 and Atmos codec only | A decision on what a lossless stream means for a capture product | Parity audit |
| Hearth desktop player | Not linked; `.thd` and `.mlp` are not among the kinds it lists | Playback to local outputs, MAT to a receiver, the Media and Decoder pages | Parity audit |
| Hearth for Android | Not linked; passes AC-3 and E-AC-3 to the receiver | Pass-through of the coded stream where the device takes it | Parity audit |
| ESP32 sinks | Not linked; no firmware compiles the library | A feasibility figure first (rates to 192 kHz and up to 8 channels against the parts' memory) | Parity audit |
| Library bindings | The C API only | Python, JavaScript (WebAssembly) and Rust | [Not done in T2c](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd.md#not-done-in-t2c); parity audit |
| Performance and platform kernels | TrueHD is in the real-time guards, the speed, memory and kernel benchmarks and the trend pages, with host instruction ceilings and an AVX2 kernel for the encoder's bit count; it is not in the Cortex-M3 and ESP32 probes, the footprint report or the per-platform pages | Instruction counts and image sizes on the small parts, platform pages with measured numbers, and a minimum-footprint profile | [The performance and platforms audit](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd-parity-performance.md) |
| CI | The Linux gate, codec matrix and nightly FFmpeg Validate run the TrueHD checks; every platform leg builds and tests the library | The platform and fuzz legs held to E-AC-3's standard | [Verified configuration](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/building.md#verified-configuration); parity audit |
| Fixtures and the owner's disc soundtracks | 25 FFmpeg-made fixtures, one stream joined from two of them and 15 hand-built streams are committed; DEE's set stays local | Offline checks that read a directory named by an environment variable and skip when it is unset, as the Online Delivery Kit checks do; nothing from a disc is committed | [Oracles](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd.md#oracles); parity audit |
| Fuzzing | Harnesses for the parser, decoder, encoder and MAT unwrap, in `tools/fuzz/run.sh`'s default targets with seeds, a dictionary and a mutator that repairs a stream's checks; a differential harness against FFmpeg's strict decode (its CI steps cannot fail the job until it has a clean record against the FFmpeg the runners install); an encoder-space search; ten-minute runs of each found three disagreements with FFmpeg, one fixed | Campaigns longer than ten minutes, and the differential harness promoted | Parity audit |
| Static analysis | The nightly scanners cover `libs/` generally; what they report for `libs/truehd` has not been looked at | The alerts for `libs/truehd` and `libs/containers`' TrueHD code triaged | Parity audit |
| Documentation | This sweep: [`planning/documentation-sweep.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/documentation-sweep.md) | Round 2 folds the audits' matrices into the status pages | [The sweep](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/documentation-sweep.md) |

**Content signing (TrueHD).** Two accounts of the evolution frame's check exist and neither has been checked against a stream. An earlier
note in this project, taken from the superseded branch, described a truncated HMAC-SHA-256 over the access unit and the evolution frame. The
owner's specification (v3.4 and later) describes the payload as the same `ContentSignature` container, in Thrift and under the same
`pace-ccjf1` scheme, as [AC-4's content signing](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/signing.md#ac-4-content-signing). The specification is the later source
and the one the code follows, so any TrueHD verification would sit on `iclforge::base::crypto`, not on `iclforge::ac3::signing`'s EMDF path.
See [Object signing](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/concepts/object-signing.md#sibling-truehd-evolution).

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
| **Object authenticity modes** | `iclforge::ac3::signing` HMAC tag; `sign-objects`; `verify-objects` with one or several `signing-key=` (a keyring: a frame is good under any key; hard fail); `gate-objects`, the licensed soft-gate (tag mismatch / unsigned → bed-only, decode continues); default decode reconstructs objects **unchecked** (FOSS-style) | Nothing planned. Known and accepted: this project ships no key, so a Dolby-licensed decoder accepting its objects is a personal-use path only — [Object signing](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/concepts/object-signing.md#decode-modes). This is E-AC-3's EMDF tag; AC-4's is the next row, and TrueHD's is not built (see the TrueHD module above) |
| **AC-4 content signing** | `iclforge::base::crypto` (codec-neutral: ECDSA P-256, DER/X.509 chain, Thrift, the `pace-ccjf1` signature) and `iclforge::ac4` `io/content_signature.hpp`: a signature after the last sync frame of a raw `.ac4`; `forge ac4-sign` and `ac4-verify`, and the options `sign-content` (`ac4-encode`, `atmos-encode codec=ac4`) and `verify-content` and `check-content` (`decode`); the operator's own key and chain, opt-in, strict and report modes; checked against OpenSSL and the owner's sample, which is kept outside the repository and read from `ICLFORGE_CONTENT_SIGNATURE_SAMPLE` (the tests that need it skip when it is unset) | Fifteen open questions for the owner at the top of `libs/ac4/ERRATA.md` (the validity-time default, carriage in an MP4, signer metadata, ...). Carriage in an MP4, a second entry, revocation and other digests are not implemented. The two fuzz harnesses have not yet run under libFuzzer. Known and accepted: no issuer's key or certificate ships, so a Dolby-issued credential is the operator's to supply — [Signing](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/signing.md#ac-4-content-signing). **TrueHD has no counterpart yet** (see the TrueHD module above) |
| **Dolby reference content** (the AC-4 kit's streams, Spott's files, the Online Delivery Kit tools) | `tools/checks/check_ac4_kit_streams.py` holds the decoder to the kit's 12 AC-4 streams (every frame decodes, the sample count the frame count gives, the channels the presentation names, loudness against the declared LKFS); `check_dolby_mp4_boxes.py`, `check_dolby_demux_roundtrip.py` and `check_dolby_cenc.py` compare forge's MP4, DASH, HLS and MPEG-TS and the protection boxes of 108 protected files with Dolby's `mp4drop` and `streamdemuxer`; of the nine AC-4 files from Dolby's public test application eight decode in full, since two decoder fixes of 2026-10-29 (an A-JOC substream with no dialogue configuration, and `md_compat` 4). All of it is optional and local, skips without the tools or the files, and commits nothing of Dolby's | OTT00234 stops at frame 1 (its tools metadata size); HLS packed audio with ID3v2 tags is not read; `forge decode` exits 0 when a stream loses most of its frames; streamdemuxer answers "Need a key for decryption" whatever key it is given, so no decryption leg runs; Dolby's hosted StreamValidator refused the automated browser (HTTP 403); `check_ac4_kit_streams.py` does not read the Spott files. Dolby-made TrueHD streams exist only as DEE's, on the owner's machine. [Validation](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/verification.md#dolbys-online-delivery-kit-tools), [AC-4 plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#open-after-dolbys-kit-streams-2026-10-29) |
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
| Common Encryption (ISO/IEC 23001-7) for E-AC-3, AC-3, AC-4 and TrueHD in MP4 | Refusal built; nothing decrypts and nothing writes protected tracks. A protected `ec-3`, `ac-3` or `ac-4` track is refused with its scheme, original format and default KID, also when the file holds a clear sample entry beside the protected one (the whole track is refused, so no ciphertext is read as audio). A protected `mlpa` (TrueHD) track takes the same path in the reader's code, and no fixture or test covers it (the audit's G24 predates TrueHD in the containers). Reading with an operator's keys is a plan (about two and a half focused weeks), awaiting the owner's decisions. Writing protected files and the manifests are gaps G13 to G19 of the audit. A Matroska audio track with `ContentEncryption` and an MPEG-TS audio PID with scrambled packets are refused too. AC-4 protected tracks are also listed under Out of scope below | [`planning/cenc-conformance.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/cenc-conformance.md), [`planning/cenc-read-side.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/cenc-read-side.md) |
| ESP32 sink tiers (C6 / C61 / S3 / P4) on one ES9080 PCB | Modular MCU: C6 2.0 / one DAC (shipped); C61 5.1 desired (no board); S3 ≤7.1.4 no ecpl / both DACs @ 16-bit (shipped); P4 ≤9.1.6 full tools desired / both DACs @ 32-bit on one I2S. P4 reopened only as best tier; its probe and hosted-Wi-Fi sink are built, its TDM output and the PCB are not | [`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-sink-tiers.md) |
| vcpkg git registry | Consumers install via `vcpkg install iclforge` | DR3 |
| winget and ConanCenter | Manifests staged; CLA and submission pending | DR4 |
| AC-4 in the performance and quality reporting | **Partial** — stereo and 5.1 encode/decode in `iclforge-perf`, `iclforge-bench` and `iclforge-membench`; AC-4 kernels in `iclforge-kernelbench`; decode quality in `ac4-quality-main.jsonl` with a chart on [Quality trend](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/quality-trend.md#ac-4-decode-quality). Still open: encode quality trend and tool-comparison series. The merge queue compares speed and memory for a change to `src/`, AC-4's rows included (#1132). **M** | [Performance and quality](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/performance-quality.md) |
| Names and layout (phase N1 of `planning/ac4.md`) | The tree has the new names. The programs are `forge`, `forge-gui`, `hearth` and `crucible` (N1A, #1164), the libraries have the targets `iclforge::<library>` and the headers `iclforge/<library>/` (N1B S2 to S4, #1160 to #1162) and, since the monorepo move (#1199), are 16 projects under `libs/` beside `apps/`, `bindings/` and `firmware/`, the pages, addresses and package metadata follow (S5), and the AC-3 codec's C++ names are in `iclforge::ac3`, a peer of `iclforge::ac4` (S6). The Windows driver is `IclForgeNullSink` and its endpoint "Speakers (Crucible Silent Output)" (N1D). [Renamed](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/renamed.md) lists every old name beside its new one. The repository and the Homebrew tap are renamed. **Not done:** the SonarCloud project key (still `iainchesworthlabs_ac3forge`) and the first release under the new names, which publishes the PyPI project and the renamed formula and cask; `iclforge` is a new PyPI project and the old one stays as it is, and the Homebrew formula and cask are renamed (decisions 40 and 41); U1 revises wording that is not a name. **L** | [`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#n1-the-names), [`planning/layout.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/layout.md), [`planning/monorepo.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/monorepo.md) |

---

## Blocked

| Item | Blocker | Was |
|---|---|---|
| TrueHD checked against a Dolby decoder, a disc player or a receiver | The library is checked against FFmpeg's encoder and decoder only. DEE's streams can be scored only on the owner's machine while its licence runs (it ends on 2026-11-06), and nothing has been played into a receiver or a player. Black-box analysis of Dolby streams is out of scope below | IM6 |
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
- **AC-4 beyond the plan's scope** — transcoding between AC-4 and AC-3 or E-AC-3 without decoding (`forge transcode` goes through PCM); rendering objects inside the decoder, headphone virtualisation and head tracking; presentations spread over several elementary streams; protected (encrypted) tracks (an MP4 with one is refused, naming its scheme, original format and default KID; nothing decrypts it); MPEG-TS under ATSC A/342-2's profile (DVB's is carried); the speech spectral frontend, 96 and 192 kHz, A-SPX time-interleaved waveform coding and bitstream versions 0 and 1 in the encoder (accepted gaps, each with its reason in the [development status](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/library/development-status.md) page); writing immersive stereo (`presentation_version` 2); the encoder on any ESP32; and AC-4 in Crucible, which captures applications into Atmos objects for receivers when no receiver accepts AC-4. The decoder's speech frontend, 9.X.4 modes and 96 and 192 kHz decoding wait for a stream that uses them. See [Deliberately not in scope](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#deliberately-not-in-scope).
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
| D1 | TrueHD branch | in progress → IM5 |
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
| IM | IM1–IM4, IM7 | IM5 in progress; IM6 blocked; IM3/IAB tails |
| VX | VX1–VX23 except VX9/VX12 tails | VX9/VX12 partial; VX5 proposed |
| PF | PF1–PF8 performance | — |
| AP | AP2–AP7, AP9, AP11–AP12 | AP1 in progress; AP8/AP10 proposed |
| UX | UX1–UX6, UX8–UX9, UX11 | UX12/UX7 partial; UX10 proposed |
| CR | — | CR1 partial |
| DR | DR1–DR2, DR5, DR7–DR8 | DR9 partial; DR3/DR4 proposed; DR6 blocked |

For per-item detail on shipped work, see [CHANGELOG.md](https://github.com/iainchesworthlabs/iclforge/blob/main/CHANGELOG.md).

</details>

---

Rebuilt 2026-09-17 from [`planning/roadmap-inventory.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/roadmap-inventory.md)
and a repository survey. Previous version (~3,100 lines, theme sections with full shipped
records) is in git history at `a7d3bd56` and earlier.
