# TrueHD and DD+ parity: the Hearth player and its network output

!!! note "Status as of 2026-10-29: an audit; nothing is built"
    Round 1 of the TrueHD and DD+ parity audit, for the desktop player, its Sendspin output and the
    sinks it sends to. It reads the tree at `523cf5dc` (branch `integ`) and says what DD+ does
    in Hearth, what TrueHD would do, and what is missing. The ESP32 firmware is another audit's.
    Round 2 is the owner's to decide after reading this page.

!!! note "Status as of 2026-10-31: H4 and the streaming demuxer are built; H5 is not"
    H0 to H3 are built (2026-10-30). The streaming demuxer for Matroska and MP4, the per-output choice of a remux's track
    (engine only), and H4 (a group sent PCM and Opus at one rate, with the audio converted for a sink that does not list the
    item's) are built on `feature/truehd-hearth-network` and checked by engine and loopback tests on Windows with Qt 6.12:
    no board, receiver or Shield was used. Questions 13 to 17 below are new. H5 and the ESP32 phases have not been started.

"DD+" here is E-AC-3, with AC-3 and the JOC object layer riding with it. "TH spec" is the owner's
*Dolby TrueHD (MLP) Reference Specification*, which is not in the repository
([truehd.md](truehd.md) says what it is).

## Basis

**Audited:** commit `523cf5dc` (the merge of the TrueHD review fixes on `integ`), in the worktree
branch `parity-hearth`.

**What was run.** The `forge` built from that commit (`forge probe`, `truehd-encode`, `eac3-encode`,
`encode`, `decode`, `spdif`), and throwaway scripts kept outside the tree: one that reads the
12-bit length that opens each TrueHD access unit (TH spec section 3.1, the framing
`libs/containers/include/iclforge/containers/iec61937/mat.hpp` also uses) to get the size of every
access unit, whose totals agree with `forge probe`'s access-unit counts and with the size of each
file; and one that makes PCM test material. FFmpeg was used only to
mux two Matroska files and to take 6 channels of a 7.1 file.

**What was only read.** All of Hearth. The build the audit was given has `ICLFORGE_BUILD_HEARTH`
off, so no `hearth`, `hearth-testsink`, `hearth-testserver` or engine test was run, nothing was
played to a device or a sink, no receiver was involved and nothing was sent over a network. A cell
whose evidence says "read only" is a reading of the code and not an observation.

**How figures are marked.** **[M]** measured here, **[C]** computed from the input named, **[D]**
taken from the document named. A figure with no mark is a count of something visible in the text
beside it.

**Test material** (all in the scratch directory, none committed):

| Name | What it is |
|---|---|
| fixtures | The eight FFmpeg-made streams in `testdata/truehd/`, 0.25 s each, a restart every 16 access units |
| music | 10 s (and 30 s for timing) of `testdata/audio/programme_music_stereo.flac`, 48 kHz, with 24-bit TPDF dither added; the 7.1 version has the centre, LFE and three delayed pairs made from the stereo, so its channels are correlated the way a mix's are not |
| noise | Full-scale uniform noise at 7.1/48 kHz, 5.1/96 kHz, 2.0/192 kHz and 7.1/96 kHz, 24-bit: the least compressible input there is, so it bounds what a lossless coder can need |

All were coded with `forge truehd-encode` (restart interval 128 access units, peak rate 18,000
kbit/s, its defaults).

**Not established by this audit.** Nothing of Hearth ran, so every Hearth row is a reading. No
Wi-Fi throughput, no Opus encoder cost, no TrueHD decode cost on a phone, a box or a board, and no
receiver's behaviour with a MAT stream (or with a missing frame of one) is measured or stated in
the repository. How often a Dolby-made stream restarts is not known, as the DEE streams are not
committed. The sections on desktop passthrough and on the Android sink are the thinnest for that
reason: they report what the code and the documents say and little that was observed.

## How DD+ moves through Hearth today

1. **Files.** `apps/hearth/ui/src/item_loader.cpp` accepts `.ac3 .ec3 .ac4 .mp4 .mkv .ts`,
   reads the whole file into memory, and tells a container by its first bytes
   (`apps/shared/media/src/container_input.cpp`, `sniff_container`). A container is demuxed to
   the elementary stream of its first selectable audio track, with the MP4 edit list becoming
   skip and play counts.
2. **Session.** `Session::open` (`apps/hearth/engine/src/session.cpp`) splits the stream into
   access units with `ac3::io::scan`, or AC-4 sync frames, and keeps for each its bytes and the
   sample at which it starts. Duration, bit rate, channels, objects and the karaoke flag are
   filled from that.
3. **Decode.** `StreamDecoder` decodes a unit, places it on the output layout through
   `libs/render`'s `LayoutRenderer` in 256-sample blocks, and reports what the unit said about
   itself (`UnitReport`). `Session::seek` restarts one unit early for AC-3 and E-AC-3, and at an
   I-frame at least 6,144 samples earlier for AC-4.
4. **Output decision.** `choose_output` (`output_decision.cpp`) takes the stream as a bitstream
   if an endpoint accepts it, else E-AC-3 transcoded to AC-3 5.1 at 448 kbit/s
   (`ac3_transcoder.cpp`), else decoded PCM, else a network group the user chose. It is a pure
   function of `audio::BitstreamFormat` (`kAc3 kEac3 kAc4 kAc4Hbr4 kAc4Hbr16`) and per-endpoint
   `accepts_ac3`, `accepts_eac3`, `accepts_pcm`.
5. **Local passthrough.** `libs/audio` backends send IEC 61937 bursts in exclusive mode on a
   2-channel 16-bit carrier: Windows WASAPI, ALSA, PipeWire, Core Audio and Android's
   `ENCODING_IEC61937`. The decode still runs beside the stream, for the meters.
6. **Network group.** `sink_form.cpp` picks a form for each member: the coded stream (burst chunks,
   ID 192, over `_iclforge_player@v1`), PCM at the layout the member lists, or Opus. One decode
   serves all of them; each extra PCM width is another `StreamDecoder` (`set_variants`). The group
   plays frame *n* at a fixed time on the server's clock; a burst carries its Pc, Pd and the
   elementary-stream bytes, without the IEC 61937 padding.
7. **Sinks.** `libs/sink` hands a burst to a pass-through output (Android's AudioTrack) or to a
   decoding output (the test sink, `hearth-player`). The ESP32 boards decode on the board.
8. **Everything around it:** gapless joins (`Transport`), meters and BS.1770 loudness
   (`PlayMeters`), the Media page (`media_info`, `iclforge.hearth.media/1`), the Decoder page's
   settings, diagnostics, and the test tools `hearth-render`, `hearth-testserver` and
   `hearth-testsink`.

## What TrueHD does today

Nothing in Hearth reads it, and one thing is worse than nothing.

- `.thd` and `.mlp` are refused by extension: "not a file hearth reads: it opens .ac3, .ec3, .ac4,
  .mp4, .mkv and .ts files". Read only.
- `truehd` is in no row of `hearth`, `sink`, `app-media` or `hearth-android` in
  `tools/checks/projects.json`, so none of them may include it. Read only.
- **A regression.** Commit `ef92bcba` added `A_TRUEHD`, `mlpa` and stream type 0x83 to the
  containers' automatic track selection (`is_selectable_codec` in
  `libs/containers/src/matroska/reader.cpp`; the MP4 and MPEG-TS readers handle the same tracks).
  Hearth takes the
  first selectable track and cannot read a TrueHD one, so an MKV, MP4 or TS file whose first audio
  track is TrueHD, with an AC-3 or E-AC-3 track after it (the usual Blu-ray remux), played its
  AC-3 track before that commit and fails now. [M] on the demuxer: `forge probe` on a two-track
  Matroska file reports track 1 `A_TRUEHD` when TrueHD is first and track 1 `A_AC3` when AC-3
  is first. The Hearth half is read only: `Session::open` falls to `ac3::io::scan` and returns
  "is not an AC-3, E-AC-3 or AC-4 stream this player can read".
- `forge play` and `forge monitor`, which use the same `libs/audio` passthrough as Hearth, do not
  read TrueHD either ([commands.md](../docs/forge/cli/commands.md), "TrueHD and MLP").

## The matrix

Size is the ROADMAP's scale: S an afternoon, M a day or two, L a focused week, XL several PRs.
"Q" is a numbered question below. Paths are relative to the repository root.

| # | Capability | DD+ status | TrueHD status | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| 1 | Queue accepts the file type (Add files, Add folder, drop, saved queue) | Yes: `.ac3 .ec3 .ac4 .mp4 .mkv .ts` (`apps/hearth/ui/src/item_loader.cpp`, `kMediaExtensions`) | `.thd` and `.mlp` refused by extension | missing | S | nothing | read only |
| 2 | Container read and track choice (`apps/shared/media/src/container_input.cpp` over `libs/containers`) | `dac3`/`dec3`, `A_AC3`/`A_EAC3`, MPEG-TS; the first selectable track | The demuxers read `mlpa`, `A_TRUEHD`, 0x83 and pick a TrueHD track first when it is first; Hearth cannot open it, so files that played now fail | partial, a regression | S | nothing to restore the old behaviour; Q8 for the policy | `forge probe` on two-track MKVs: first-track TrueHD is chosen; Hearth half read only |
| 3 | Bare-stream sniffing and error text | `sniff_container` checks an AC-3/E-AC-3 header before the TS grid; `ac3::io::scan` splits | `sniff_container` knows a TrueHD major sync and calls it a bare stream; `Session::open` has no branch for it | partial | S | nothing | read only |
| 4 | Library and layering | `hearth` may use `ac3`, `ac4`, `audio`, `containers`, `sendspin`, `sink`, `app-media` | `truehd` is in no row of `hearth`, `sink`, `app-media` or `hearth-android` | missing | S | Q1 (whether `sink` links it) | `tools/checks/projects.json`, read only |
| 5 | Open an item: access units, durations, bit rate, rate, channels (`Session::open`) | `ac3::io::scan`; per-unit sample counts; programmes; objects | `truehd::probe_stream` and `ElementaryReader` exist and are not called. A unit is 40, 80 or 160 samples, so `units_` and `starts_` (at least 24 bytes a unit) hold 38.4 times as many entries as E-AC-3's: 8.64 million for two hours, 207 MB, against 225,000 and 5.4 MB [C]. The loader also reads the whole file into one vector, then demuxes into a second | missing | M | nothing | computed from `session.hpp`; `item_loader.cpp` read |
| 6 | Decode and place on the layout (`StreamDecoder`) | AC-3, E-AC-3 (+ objects) and AC-4 branches; 256-sample blocks | None. The library decoder is ready: lossless 24-bit in int32, presentations `2ch/6ch/8ch/best/all`, one unit per call (`libs/truehd/include/iclforge/truehd/decoder/decoder.hpp`). Missing: the branch, the TrueHD-channel-to-slot map (as `ac4_bed()` is for AC-4), and blocks of 40/80/160 samples into a render path organised on 256 | missing | L | nothing (defaults as `forge decode`: `best`) | decoder timing [M]; the rest read only |
| 7 | Fold to stereo or mono, LFE in the fold, mix levels (`DecoderSettings`) | A/52 section 7.8 fold with the stream's own levels, Lt/Rt option, per-fold override | The stream has its own 2ch and 6ch presentations (substreams 0 and 1) and mix levels in the major sync (`ChannelMeaning`); the library does no fold | partial | M | Q3 | read only |
| 8 | Dialogue normalisation and dynamic range (line, RF, late-night on sinks: [hearth-late-night.md](hearth-late-night.md)) | Yes | The decoder reads dialnorm, the DRC start-up gain and the per-unit DRC word and applies none; heavy DRC is not decoded (`libs/truehd/ERRATA.md` entry 10) | missing | M | Q10 | read only |
| 9 | Output decision (`choose_output`) | AC-3, E-AC-3, E-AC-3 as AC-3, local PCM, group, none, each with a reason | `audio::BitstreamFormat` has no MAT value; `EndpointFacts` has `accepts_ac3`, `accepts_eac3`, `accepts_pcm` only | missing | M | row 10 | read only |
| 10 | Local passthrough (`libs/audio/src/backend/{windows,alsa,pipewire,macos,android}/passthrough.cpp`) | AC-3 and E-AC-3 on all five; AC-4 on ALSA and Android; every backend opens a 2-channel 16-bit carrier | The MAT packer exists (`libs/containers/src/iec61937/mat.cpp`) and `forge spdif` writes it as an 8-channel 16-bit WAV at 192,000 Hz (176,400 for the 44.1 family), marked provisional (`libs/containers/ERRATA.md` question 7). No backend opens more than two channels; Windows defines the Dolby Digital and Dolby Digital Plus subtypes only; AC-4 HBR16 has the same eight-channel hole | missing | XL | Q5, and a receiver | each backend's header read; `forge spdif` [M] |
| 11 | Which outputs take it (probe, EDID/ELD, descriptor) | `supports_ac3_passthrough`, `supports_eac3_passthrough`; ELD reader | The ELD reader maps "MAT/TrueHD" to "other"; the PipeWire code can name TRUEHD in SPA's list but `iec958_codec_for` returns nothing for it | missing | M | row 10 | `libs/audio/src/backend/alsa/eld_proc.hpp`, `pipewire/pipewire_support.hpp`, read only |
| 12 | Transcode for a receiver that does not take the format (`Ac3Transcoder`) | E-AC-3 to AC-3 5.1 at a fixed 448 kbit/s, dialnorm and compr carried across, 256-sample encoder delay | None. `libs/ac3` has the AC-3 `FrameEncoder` and a frame-at-a-time E-AC-3 `AccessUnitEncoder` (`eac3_frame.hpp`); Hearth uses the first only | missing | M | Q4 | encode timing [M]; the rest read only |
| 13 | Gapless joins (`Transport::same_stream`, burst packer carried across items) | Same rate, mode and format join; the E-AC-3 packer keeps a part-filled burst across the join | None. A MAT frame holds 24 access units, so a bitstream join needs the packer to carry a part frame (`MatPacker::flush` pads a last frame, `libs/containers/ERRATA.md` M5) | missing | M | rows 6 and 10 | read only |
| 14 | Seek (`Session::seek`, `first_decoded`) | One unit early for AC-3 and E-AC-3; an I-frame at least 6,144 samples early for AC-4 | The decoder gives nothing until a restart header, so a seek goes back to the last restart at or before the target. The interval is 13.3 ms in FFmpeg's streams, 106.7 ms in this library's encoder by default, up to 3.4 s as the encoder allows | missing | M | nothing | restart intervals [M] |
| 15 | Queue facts: rate, channels, duration, bit rate, frame rate (`ItemFacts`) | All from `scan` | None. The bit rate varies. `frame_rate` is "access units a second, one burst chunk each" and is compared with `max_frame_rate`, which is 1 to 1,000; a TrueHD access unit is 1,200 a second at 48 kHz [C] | missing | S | Q2 | read only |
| 16 | Network group: a form for each member (`sink_form.cpp`) | Coded, PCM at the member's layout, or Opus, with the reason on the group editor | `StreamNeeds::stream` cannot name TrueHD. PCM goes only at the stream's own rate ("this app does not resample", `sink_form.cpp`), and every ESP32 board lists 48 kHz, so a 88.2 to 192 kHz item reaches those members as nothing. Opus is 48 kHz stereo | missing | M | row 6; Q7 | read only |
| 17 | Sendspin burst chunk and support object (`libs/sendspin`, [hearth-sendspin-extension.md](hearth-sendspin-extension.md)) | ID 192 with Pc 1, 21, 24; `data_types` ac3, eac3, ac4; the sink drops a payload that lacks 0x0B77 | No data type 22; `parse_burst_chunk` refuses it. Pd is 16 bits and MAT's 61,424 fits; 24 raw access units are at most 17 + 61,440 bytes, inside E12's 65,518, so no fragments | missing | M | Q1, Q2 | read only; sizes [C] |
| 18 | Group timeline and pacing (`Group::push_burst`, `server_host.cpp`) | 1,536 samples a burst; read ahead 1.5 s; no member above three quarters of its `buffer_capacity` | Not codec-specific. `Burst::frames` would be 960 at 48 kHz, not 1,536. At 18 Mbit/s the bytes in flight for the Android sink are 5.4 MB against 6.29 MB allowed [C] | partial | S | Q2 | constants read; arithmetic [C] |
| 19 | Sink: pass-through output (`libs/sink` `PassthroughOutput`; `apps/hearth/android` `PassthroughBridge`) | AC-3 and E-AC-3: burst rebuilt with `burst_from_parts`, null bursts keep the receiver's lock, 6 bursts ahead and 8 to prime, `PassthroughOptions` from the Shield's measurements [D: [hearth-android-findings.md](hearth-android-findings.md), F12]; 3,790 of 3,792 AC-3 bursts and all 3,792 E-AC-3 played on a Shield [D: [android.md](../docs/hearth/android.md)] | `pc_of` knows Pc 1 and 21; `LinkFormat` assumes a stereo 16-bit link frame; whether a null burst keeps a receiver on a TrueHD stream is not known | missing | L | row 10; a Shield and a receiver | read only |
| 20 | Sink: decoding output (`libs/sink` `DecodingOutput`, `hearth-player`, `hearth-testsink`) | AC-3, E-AC-3 (+ objects), AC-4 | None; `sink` may not use `truehd` | missing | L | Q1 | read only |
| 21 | Android sink encodings (`apps/hearth/android`) | `ENCODING_IEC61937`, `CHANNEL_OUT_STEREO`, 16-bit, carrier 48 kHz (AC-3) or 192 kHz (E-AC-3), probed with `isDirectPlaybackSupported`; PCM, FLAC and Opus are accepted and dropped; 8 MiB `buffer_capacity`, 800 ms lead | No `ENCODING_DOLBY_TRUEHD`, no 8-channel mask. The Shield's HDMI policy lists TrueHD and IEC 61937 at 7.1 masks; only stereo IEC 61937 at 48 and 192 kHz was tried [D: [hearth-android-findings.md](hearth-android-findings.md), spike S4] | missing | L | rows 10 and 19; Q12 | `PassthroughBridge.kt`, `native_sink.cpp` read only |
| 22 | ESP32 members | Decode AC-3 and E-AC-3 on the board | Not a target here. From the sender a board takes PCM (stereo, 48 kHz, 16 or 24 bit) or Opus | not applicable to this page; the firmware audit has the board | n/a | n/a | [D: [esp32-sink-compatibility-matrix.md](esp32-sink-compatibility-matrix.md)] |
| 23 | Meters, loudness, frame metadata (`PlayMeters`, `UnitReport`) | Per-slot levels, BS.1770 loudness, This-frame card from `UnitReport` | The meters read rendered PCM and need nothing. `UnitReport` has AC-3 and AC-4 fields and none for TrueHD (its `ac4` optional is the pattern) | missing | M | row 6 | read only |
| 24 | Media page and JSON export (`media_info`, `iclforge.hearth.media/1`) | Codec token `ac3`, `eac3`, `ac3+eac3`, `ac4`; the probe object | No TrueHD member. `forge probe` prints a TrueHD description but no average or peak bit rate | missing | M | row 6 | `forge probe ... json=1` [M] |
| 25 | Decoder page and settings | An AC-3/E-AC-3 tab, an AC-4 tab, Karaoke | None. The TrueHD controls would be the presentation and strict or lenient checking | missing | M | Q3 | read only |
| 26 | Diagnostics (ring, report, server) | Records outputs opened, items played, the first error of an item and a count of the rest | Works unchanged once an item opens. The decoder's counters (`units_waiting`, `restarts`, `lossless_check_failures`) are not surfaced | partial | S | row 6 | read only |
| 27 | Window text and documents | E-AC-3 is named in 24 engine sources, 8 window sources and 16 QML files; `docs/hearth/*.md` | TrueHD is named nowhere | missing | S | rows 9 and 10 | `grep -ril` for `eac3` and `e-ac-3` |
| 28 | Karaoke (Annex C) | Yes | Not applicable: Annex C is AC-3's | n/a | n/a | n/a | read only |
| 29 | Objects and Atmos | JOC reconstruction, Objects card | The library does not decode the Atmos substream (on hold, [truehd.md](truehd.md)). A TrueHD Atmos stream would pass through untouched in MAT once row 10 exists; a local decode plays the 7.1 bed | not applicable until the library has it | n/a | the owner's spec | read only |
| 30 | Test tools and fixtures | `hearth-render`, `hearth-testserver`, `hearth-testsink`, fake sinks, 47 engine test files | None. `testdata/truehd/` has eight FFmpeg-made streams with the SHA-256 of FFmpeg's strict decode and 13 hand-built ones | missing | M | row 6 | `ls apps/hearth/engine/tests`, the manifest |
| 31 | CI and platform documents | `[hearth]` tests on the windows, linux and macos lanes; `docs/hearth/*` | None | missing | S | rows 9 and 10 | `tools/checks/projects.json` |

## Bandwidth and processing

This is the part the owner flagged. Everything TrueHD adds to Hearth's cost follows from three
facts: the stream is several times to tens of times the size of DD+, its size varies from
access unit to access unit, and a decoder that loses a unit cannot resume until the next restart.

### The rates

The access-unit period is the same at every rate in the 48 kHz family: 40, 80 or 160 samples at
48, 96 or 192 kHz is 0.8333 ms, so 1,200 access units a second (1,102.5 at 44.1 kHz) [C:
`libs/truehd/include/iclforge/truehd/core/format.hpp`, `nominal_samples_per_access_unit`]. Rates
below are bits over the duration of what is counted. "Peak AU" is the largest single access unit
as if it were sent alone over its own 0.83 ms; it sizes a buffer, not a link. "Peak 20 ms" is the
largest 24 consecutive units, which is one MAT frame.

TrueHD, in kbit/s:

| Stream | Average | Peak AU | Peak 20 ms | Basis |
|---|---|---|---|---|
| mono 48 kHz 24-bit | 739 | 1,459 | 762 | fixture [M] |
| stereo 48 kHz 16-bit | 629 | 1,670 | 661 | fixture [M] |
| stereo 48 kHz 24-bit | 1,414 | 2,438 | 1,457 | fixture [M] |
| stereo 96 kHz 24-bit | 1,967 | 3,245 | 2,062 | fixture [M] |
| stereo 192 kHz 24-bit | 2,737 | 4,589 | 2,978 | fixture [M] |
| 5.1 48 kHz 24-bit | 4,227 | 6,528 | 4,340 | fixture [M] |
| music stereo 48 kHz 24-bit, 10 s | 1,336 | 2,496 | 1,515 | generated [M] |
| music 7.1 48 kHz 24-bit, 10 s | 4,561 | 8,448 | 5,303 | generated [M] |
| noise 7.1 48 kHz 24-bit | 9,435 | 10,330 | 9,469 | generated [M] |
| noise 5.1 96 kHz 24-bit | 13,983 | 14,650 | 14,008 | generated [M] |
| noise 2.0 192 kHz 24-bit | 9,316 | 9,773 | 9,332 | generated [M] |
| noise 7.1 96 kHz 24-bit | 18,651 | 19,565 | 18,684 | generated [M]; `forge truehd-encode` refuses it with "the stream cannot be sent within the peak data rate and 75 ms (TH spec section 18)" and writes it only with `truehd-rate-check=lenient` |

The music files code to 58.0% (stereo) and 49.5% (7.1) of their 24-bit PCM [M: the encoder's
report]; noise codes to 101.1% to 102.4% of PCM, so for a lossless stream the PCM rate is the
ceiling to within about 2%: 9.2 Mbit/s for 7.1 at 48 kHz, 13.8 for 5.1 at 96 kHz, 18.4 for 7.1 at
96 kHz [C]. A 7.1 programme at 48 kHz did not pass 10.4 Mbit/s in any access unit even as noise
[M]. The 18 Mbit/s case needs 96 kHz at eight channels, and the encoder will not write it unless
told to.

DD+, in kbit/s:

| Stream | Rate | Basis |
|---|---|---|
| stereo, 10 s | 256, constant | 320,512 bytes over 10.016 s [C] |
| 7.1, 10 s | 1,536, constant (6,144 bytes a unit) | `forge probe` "1536.0 kbit/s measured" [M] |
| largest syncframe of one substream at 48 kHz | 1,024 | 2,048 words (the 11-bit `frmsiz`) in 32 ms; `forge eac3-encode` refuses 1,664 kbit/s at 7.1 with that limit [M, C] |
| the IEC 61937 burst | 6,144 | `kEac3BurstBytes` 24,576 bytes in 32 ms [C]; this is the ceiling the owner's "6 Mbit/s" is |
| what Hearth has moved on real hardware | 256 and 448 | E-AC-3 5.1 and AC-3 5.1 to a Shield [D: [android.md](../docs/hearth/android.md)] |
| the ESP32 stream set | 96 to 768 | [D: [esp32-stream-set.md](esp32-stream-set.md)] |
| 5.1.4 with JOC | about 768 | [D: [topology.md](topology.md)] |

So against DD+ at the same layout: music 7.1 TrueHD is 3.0 times the 1,536 kbit/s above and
10.2 times AC-3 5.1 at 448; music stereo is 5.2 times 256 [C]. Against the DD+ carrier ceiling of
6,144 the noise streams are 1.5 to 3.0 times (9,316 to 18,651).

**PCM for the same layouts** [C]: stereo 48 kHz 16-bit 1,536 kbit/s, 24-bit 2,304; 5.1 24-bit
6,912; 7.1 24-bit 9,216; 7.1 at 96 kHz 24-bit 18,432. Opus, stereo, at the default 64 kbit/s a
channel [D: `libs/sendspin/include/iclforge/sendspin/codec.hpp`], is 128 kbit/s. The Opus encoder
takes two channels at most.

**What a MAT burst costs on a link.** The burst is 61,440 bytes whatever the stream holds, 24
access units to a burst, one burst per 20 ms (21.77 ms in the 44.1 family) [C]. `forge spdif`
wrote the stereo stream (1.67 MB) and the 7.1 stream (5.70 MB) to carriers of exactly 30,720,044
bytes each for 10 s [M], which is 24.576 Mbit/s: the 8 channels of 16 bits at 192 kHz
(`forge spdif` says "eight channels of 16-bit PCM at 192000 Hz (worked out from the burst size;
no receiver has been tried)"). The E-AC-3 carrier for the 7.1 file is 7,692,332 bytes for 10 s
[M], 6.144 Mbit/s, one quarter. So a padded MAT burst must not be what goes over Sendspin:
it would put 24.6 Mbit/s on the network for a 1.3 Mbit/s stream. The existing design for DD+
already sends the elementary stream bytes without the padding (`hearth-sendspin-extension.md`,
"Burst chunks"), and TrueHD should do the same (Q2).

### What Hearth moves today, and the framing

- **Chunk.** A burst chunk is a 17-byte header (`kBurstChunkHeaderBytes`) and the payload; at most
  65,518 bytes go in one encrypted message before fragments are needed (E12 and E13 of
  `hearth-sendspin-extension.md`). One E-AC-3 chunk is one 32 ms burst, 31.25 a second, 1,792
  bytes for AC-3 5.1 at 448 kbit/s [C].
- **TrueHD chunk shapes** [C]. One chunk per access unit would be 1,200 messages a second, and
  over `max_frame_rate`'s 1,000 limit. One chunk per MAT group of 24 units is 50 a second, 11.4 KB
  for the 7.1 music stream on average, 13.3 KB in its busiest 20 ms, 46.6 KB at 18.65 Mbit/s, and
  never more than 61,424 bytes, so 17 + 61,424 stays inside 65,518 with 4,077 bytes spare. The
  17-byte headers at 50 chunks a second are 6.8 kbit/s.
- **Group clock and read-ahead** (`libs/sendspin/src/server_host.cpp`). A file is a buffered
  source: a chunk is sent when its play time is within the lead plus `kReadAhead` (1.5 s), the
  lead being the largest member's `max(min_buffer, required_lead) + output_delay` plus
  `kNetworkLead` (100 ms). No member is sent more than three quarters of its `buffer_capacity`,
  and a burst that would pass the capacity waits. A PCM member whose buffer holds under a second
  is sent 10 ms units (`kShortUnit`).
- **What that holds for a coded TrueHD member** [C]. The Android sink asks 800 ms of lead and
  lists 8 MiB, so a server keeps about 0.9 + 1.5 = 2.4 s in flight: 134 KB for AC-3 5.1 at 448
  kbit/s; 1.84 MB at the E-AC-3 ceiling of 6.144 Mbit/s; 1.37 MB for the 7.1 music stream; 5.4 MB
  at 18 Mbit/s, against the 6.29 MB (three quarters of 8 MiB) the server will send. The 8 MiB
  holds 3.7 s of 18 Mbit/s. So the existing throttle copes with the highest rate the encoder
  writes, with 14% to spare. A sink that lists less gets less read-ahead, and below about 2.7 MB
  (the lead's worth at 18 Mbit/s, 2.03 MB, over three quarters) its first second could not be
  sent in time [C]. `libs/sink`'s default is 32 MiB.

### Wi-Fi sinks and a group whose members take different forms

The repository has no throughput measurement of a Wi-Fi link. The only link figures are the
ESP32-C6's: on a busy SSID a message's payload after its header took 30 to 60 ms in a bad minute
and 200 to 320 ms about twice a second; idle pings were 2 ms least, 8 to 18 ms mean, 35 to 90 ms
most; and the board's 48 KB ring holds 192 ms of 16-bit stereo [D:
[esp32-sink-compatibility-matrix.md](esp32-sink-compatibility-matrix.md)]. Two stalls a second of
200 to 320 ms is a link stalled for 40% to 64% of the time, so a stream needs the link to run
1.7 to 2.8 times its rate while it is not stalled [C]. Whether a given Wi-Fi link runs 5 or 18
Mbit/s of TCP, with that margin, is not in the repository.

A group sends each member its own unicast stream, so what leaves the server is the sum of the
members' forms. The TrueHD premium is only in the coded members; a PCM or Opus member costs the
same whatever the source codec. For a group of an Android box into a receiver (coded), two ESP32-S3
boards (stereo 16-bit PCM) and an ESP32-C6 (Opus), in kbit/s [C]:

| Source | Android (coded) | 2 x S3 (PCM) | C6 (Opus) | Server sends |
|---|---|---|---|---|
| DD+ 7.1 | 1,536 | 3,072 | 128 | 4,736 |
| TrueHD 7.1 music, average | 4,561 | 3,072 | 128 | 7,761 |
| TrueHD 7.1 music, busiest 20 ms | 5,303 | 3,072 | 128 | 8,503 |
| TrueHD at 18,651 | 18,651 | 3,072 | 128 | 21,851 |
| TrueHD 7.1 sent to the Android box as PCM 24-bit instead | 9,216 | 3,072 | 128 | 12,416 |

A coded TrueHD stream is half the rate of the PCM it replaces for music and 1.0 to 1.02 times it
for noise, so for a group with one capable member it is cheaper than PCM on average and no cheaper
at the peak. A 96 kHz or 192 kHz item reaches the three boards as nothing today (row 16): they list
48 kHz, and Hearth does not resample. `libs/dsp`'s resampler is offline and whole-buffer
(`resample`, `resample_planar`), and the live one in `libs/audio` is a linear-interpolation drift
corrector, so there is no streaming sample-rate converter in a form Hearth can call.

### Jitter, latency and what a lost unit costs

- **The lead.** Nothing in the lead arithmetic depends on the codec. The pass-through sink's
  start is a count of bursts: the Shield's head did not move until 8 were queued, 12,288 frames or
  256 ms of E-AC-3 [D: [hearth-android-findings.md](hearth-android-findings.md), F12]. Eight MAT
  bursts are 7,680 frames or 160 ms and 491,520 bytes of carrier [C]. Whether the threshold is a
  count, a duration or a number of bytes is not known, and neither is the margin of 6 bursts
  ahead (192 ms of E-AC-3, 120 ms of MAT).
- **A late chunk.** The extension's rule is that a late chunk is dropped and the decoder is reset
  and told to conceal (`hearth-sendspin-extension.md`, "Timing"). For DD+ that costs the chunk.
  For TrueHD the library says that after a unit that does not read, every substream "gives no
  samples and no error" until its next restart header (`decoder.hpp`, `Strictness` and
  `lost_data`). So a dropped chunk costs the chunk plus up to a restart interval of silence:
  13.3 ms in FFmpeg's streams, 106.7 ms in this encoder's, 3.4 s at its longest [M for the
  intervals]. A dropped DD+ chunk is 32 ms of audio; a dropped TrueHD group of 24 units is 20 ms
  and then the wait, so the cost of the same late chunk is 1.7 to 6 times as much at the
  intervals above, and on a link with stalls of 200 to 320 ms the lead has to cover the stall
  and the wait.
- **The receiver.** A pass-through member has the same dependence on its own decoder, and what a
  receiver does with a missing MAT frame is not in the repository.

### The sender's processor

The sender decodes once for the meters whatever the output, and once more for each distinct PCM
width in the group (`StreamDecoder::set_variants`). Measured with the `forge` binary built from
this commit (x86-64, SSE2 kernels, `-O3`) on a machine that was shared at the time (load average
4.7 on 4 cores); each figure is user CPU for 30 s of 48 kHz audio including process start, and
decode went to `/dev/null` [M]:

| Work | User CPU | Share of one core |
|---|---|---|
| TrueHD decode, 7.1 music, 24-bit (3 runs) | 0.40 to 0.48 s | 1.3% to 1.6% |
| TrueHD decode, 7.1 noise (3 runs) | 0.27 to 0.37 s | 0.9% to 1.2% |
| TrueHD decode, stereo music | 0.11 s | 0.4% |
| E-AC-3 decode, 7.1 at 1,536 kbit/s | 0.19 to 0.20 s | 0.6% |
| E-AC-3 decode, stereo at 256 | 0.047 s | 0.16% |
| AC-3 encode, 5.1 at 640 | 0.44 to 0.45 s | 1.5% |
| E-AC-3 encode, 7.1 at 1,536 | 1.6 to 1.8 s | 5.4% to 6.0% |
| E-AC-3 encode, stereo at 256 | 0.39 to 0.44 s | 1.3% to 1.5% |

A TrueHD decode costs about 2 to 2.5 times a DD+ decode of the same layout, and both are a small
share of one desktop core. What the repository can do today, in terms of those costs:

- **TrueHD to PCM for a member:** decode (row 6) and render; 1.3% to 1.6% of a core plus the
  renderer, which was not timed.
- **To Opus:** decode, fold to stereo, then the Sendspin Opus encoder. Its cost was not measured;
  only the board's decode of Opus was [D: `esp32-sink-compatibility-matrix.md`]. Two channels at
  most.
- **To AC-3 5.1 (a receiver that takes nothing newer):** about the decode plus 1.5% of a core,
  through the AC-3 `FrameEncoder` that `Ac3Transcoder` already drives, 3% in all [C]. Lossy, no
  height channels, and 7.1 folds to 5.1.
- **To E-AC-3:** 7.1 at 1,536 kbit/s is the decode plus 5.4% to 6.0%, about 7% [C]. The library
  has a frame-at-a-time encoder for it (`AccessUnitEncoder`); Hearth has no path that calls it.
- **On a phone or box:** not measured. The Shield decodes E-AC-3 5.1 at 80 times real time and
  AC-3 5.1 at 94 [D: hearth-android-findings.md, F14]; `libs/truehd` has not been built for
  Android, and the ratio above is x86-64's.

### Seek and start-up

The decoder joins a stream only at a restart, and the encoder chooses how often:

| `truehd-restart=` (units) | Interval | Size of the 7.1 music stream | Against 128 |
|---|---|---|---|
| 1 | 0.83 ms | 7,789 kbit/s | +71% |
| 8 | 6.7 ms | 4,938 | +8.3% |
| 16 (FFmpeg's streams) | 13.3 ms | 4,727 | +3.6% |
| 64 | 53.3 ms | 4,580 | +0.4% |
| 128 (default) | 106.7 ms | 4,561 | 0 |
| 1,024 | 853 ms | 4,550 | -0.2% |
| 4,096 | 3,413 ms | 4,551 | -0.2% |

[M, 10 s of the 7.1 music stream, `forge truehd-encode`.] In this library's encoder every
restart interval opens with a major sync (94 of each in 12,000 units). How often a Dolby-made
stream restarts is not in the repository: the DEE streams stay on the owner's machine
([truehd.md](truehd.md)).

What this means for Hearth:

- **Local decode.** A seek goes back to the restart at or before the target, decodes forward and
  drops up to one interval; at 75 times real time (the 7.1 music stream, 30 s in 0.40 s) 3.4 s of
  interval is 45 ms of work and 107 ms is 1.4 ms [C]. There is no pre-roll to settle, since a
  restart interval is coded whole. Seeks land to the sample, as they do now.
- **Pass-through or a coded member.** Only whole units go out, and the stream must start at a
  restart, so playback starts up to one interval before the target (the same shape as
  `Session::play_whole_units` for E-AC-3, whose unit is 32 ms). The MAT packer must start its
  first frame at that unit.
- **A new item** starts at a major sync and has no wait.
- **Memory.** Row 5: whole-file loading is a limit for a film remux (the audio alone is
  about 4.5 GB for two hours at 5 Mbit/s [C]), and it is a limit DD+ also has but rarely meets.

### Desktop passthrough: what the repository says

MAT is IEC 61937 data type 22. What the repository states about opening an output for it:

| Place | What it says |
|---|---|
| `libs/containers/ERRATA.md`, M1 to M7 and question 7 | The burst is 61,440 bytes, Pc `0x0016`, Pd `0xEFF0`, 24 access units, all read from FFmpeg's `spdif` muxer, which is the one oracle; the 8-channel 192/176.4 kHz carrier is arithmetic and an assumption, "owner to confirm"; nothing has played the WAV into a receiver |
| `libs/audio/include/iclforge/audio/passthrough.hpp` | AC-4 HBR16 "needs the eight-channel high-bit-rate link, which no backend here opens" |
| Windows (`windows/passthrough.cpp`) | Exclusive mode; `kCarrierChannels = 2`; subtypes defined for Dolby Digital and Dolby Digital Plus only. The Dolby MLP subtype and what an exclusive open of it takes are not in the repository |
| ALSA (`alsa/passthrough.cpp`) | A 16-bit stereo `iec958` or `hdmi` device with the non-audio status bit; optical is "not specified past 96 kHz", so 192 kHz is HDMI's. The ELD reader classes MAT/TrueHD as "other". An 8-channel open is not in the repository |
| PipeWire (`pipewire/`) | Offers a codec only once the session manager has filled `iec958.codecs` from the EDID; SPA's list names TRUEHD; a Raspberry Pi and an Atmos receiver came up with `[PCM, DTS, AC3, EAC3, TrueHD, DTS-HD]` unprompted [D: [linux.md](../docs/platforms/linux.md)]; the repository's mapping has no TrueHD |
| Core Audio (`macos/passthrough.cpp`) | Retunes the device's physical format to `'cac3'` or `'ec-3'`; the repository names no TrueHD format, and "nothing on macOS has played anything" [D: [macos.md](../docs/platforms/macos.md)] |
| Android | `ENCODING_IEC61937` with `CHANNEL_OUT_STEREO`, the carrier rate being the wire's. The backend's own comment says TrueHD and DTS-HD MA are exempt from the stereo rule, as in Kodi's sink |

So the sample rate and channel count an OS device must be opened with for MAT are, as far as the
repository goes, 8 channels of 16 bits at 192,000 Hz for the 48 kHz family and 176,400 Hz for the
44.1 family, and that is provisional. It has been read from nothing but the burst size.
What a device takes in exclusive mode, and whether it can be opened at 8 channels and 192 kHz, is
untested on every platform. The only receiver runs the project has made are AC-3, E-AC-3 and
signed Atmos on an Onkyo TX-RZ740 from Windows and from a Raspberry Pi [D:
[index.md](../docs/hearth/index.md)]; whether that receiver takes TrueHD is not in the repository.

Local PCM is not bit-perfect: it is shared-mode float32 through the OS mixer
(`libs/audio/include/iclforge/audio/monitor.hpp`: "the audio engine is free to resample, mix and
volume-scale"), after Hearth's own trim, delay and crossover. A 24-bit sample is exact in float32,
so the decoder's output survives to the renderer; the mixer then decides. The only bit-exact path
for TrueHD would be MAT passthrough, or an exclusive integer PCM output that Hearth does not have.

## Things TrueHD has that DD+ never needed

1. **MAT carriage.** Data type 22, a 61,440-byte burst of 24 units on an 8-channel link at 192 or
   176.4 kHz: 24.576 Mbit/s on the wire for any content [M]. Every passthrough backend and the
   Android bridge assume a stereo carrier.
2. **A high and variable rate with a rule.** Up to 18 Mbit/s peak and a 75 ms transmission bound
   (TH spec section 18). Pacing by frames, as the group does, is by bytes in effect; the buffer
   caps of rows 18 and 21 are what protect a sink.
3. **Restart-interval granularity** for seek, join and loss (rows 14 and the late-chunk text).
4. **Lossless verification.** The decoder checks every restart interval against the next restart
   header and either drops the unit (strict) or counts and goes on (lenient). A player has to say
   which, and show the counter.
5. **Sample formats.** 24-bit integer samples at 44.1 to 192 kHz, in a pipeline that is float32
   and shared-mode: exact in float32, but not through the OS mixer, and not to a 48 kHz board
   without a streaming resampler.
6. **Units smaller than the renderer's block** (40, 80, 160 against 256) and 38 times as many.
7. **Several presentations in one stream.** 2ch, 6ch and 8ch (and the 16-channel one) are
   substreams; a decode reads as many as it needs, and a receiver chooses its own. Each has its own
   dialnorm and mix level in the major sync.
8. **Two stream types.** TrueHD (`0xF8726FBA`) and MLP (`0xF8726FBB`, DVD-Audio). MAT is TrueHD's;
   the repository has no carriage for MLP over HDMI.
9. **Files that are films.** The usual TrueHD file is a remux of tens of gigabytes.
10. **A bit-exact expectation.** A listener who has a lossless codec expects a bit-exact path or to
    be told there is not one.

## A proposed definition of first class, for this area

For the owner to approve or change. TrueHD (and MLP, decode only) is first class in Hearth when:

1. Every route a DD+ file takes into the queue takes a TrueHD file: Add files, Add folder, drop,
   a saved queue, `.thd`, `.mlp`, and the track of an MP4, Matroska or MPEG-TS file; and a file
   with more than one audio track plays a track the output can play, never one it cannot.
2. It plays to a local output as PCM through the same speaker layout, routing, trim, delay and
   crossover as DD+, at its own rate, and with the default settings and a layout equal to the
   stream's the samples that reach the sink equal `forge decode`'s (the fixtures' SHA-256 in
   `testdata/truehd/manifest.json`).
3. The output decision names TrueHD, and offers passthrough where an output takes MAT, else PCM
   where it takes multichannel PCM, else a stated fallback or a stated refusal, each with a reason
   in the header and the Signal path card.
4. A seek lands within one restart interval and plays exact samples from the target; a join between
   TrueHD items of the same format has no gap; a stream joined or damaged mid-way recovers at the
   next restart and says so in diagnostics.
5. The Media page, the JSON export, the meters, the This-frame card and the diagnostics report
   what a TrueHD stream says and what its decoder counted.
6. In a group every member is sent something it can play, or is told why not, with the form and
   the reason on the group editor, including a member that cannot take the rate; and the throughput
   a coded TrueHD member needs is stated and held by the throttle at the stream's peak rate.
7. Hearth does not claim what it has not tried: pass-through to a receiver, a Wi-Fi sink and
   an Android box are marked as run or not run on the pages that name them, as the DD+ pages do.
8. It is covered by engine tests with the fake sinks, a loopback group test, a `hearth-render`
   comparison with the manifest, a CI lane that runs them, and the pages that name AC-3 and E-AC-3
   say TrueHD where it applies.

## The plan

Phases are in dependency order. The first three need no decision from the owner; the others wait
for the questions named. Sizes add up the rows they carry. Nothing here changes the clean-room
rule or touches the firmware.

| Phase | Rows | Size | Waits for | Proof it is done |
|---|---|---|---|---|
| **H0. Contain the regression** | 2, 3 | S | nothing | A two-track MKV, MP4 and TS fixture, TrueHD first and AC-3 or E-AC-3 second, queues and plays the second track; a file whose only track is TrueHD is marked not playable with "TrueHD is not played by Hearth yet". An engine test per container |
| **H1. Decode and play locally** | 1, 4, 5, 6, 15 | L | nothing: `best` and the renderer's fold, as `forge decode` takes them | The 8 fixtures and the 13 hand-built streams through `hearth-render` equal `forge decode`'s output, and the 8 fixtures equal the manifest's SHA-256 at a matching layout; a fake-device queue test plays one to its end; `projects.json` rows for `hearth`, `app-media` and `sink` updated and `check_layering.py` passes |
| **H2. Seek, joins and the queue** | 13 (decode path), 14 | M | nothing | Seek tests at every restart boundary and one sample either side, comparing with an unbroken decode; a gapless queue of two TrueHD items has the expected sample count at the join; a damaged unit recovers at the next restart |
| **H3. What the window says** | 7, 23, 24, 25, 26, 27, 30 | M | the control list only (Q3) | `UnitReport` and the Media JSON carry a TrueHD member, additive within `iclforge.hearth.media/1`; the Decoder tab, Signal path text and diagnostics counters show it; QML tests; the Hearth pages and `docs/hearth/index.md` say what plays and what does not |
| **H4. PCM and Opus members** | 16, 18 | M | Q7 for rates above 48 kHz; none for 44.1 and 48 | A loopback group of two PCM sinks and one Opus sink, one item, equal sample counts; 96 kHz to a 48 kHz sink either plays through the resampler or is held back with the reason, as Q7 decides |
| **H5. Coded TrueHD over Sendspin** | 17, 20, 19 (host part) | L | Q1, Q2 | The extension page and the library gain the data type and the group rate; `hearth-testsink` decodes it; the test sink's output equals `forge decode`; a throttle test at 18 Mbit/s; aiosendspin sees an unknown role object and ignores it |
| **H6. MAT passthrough, local** | 9, 10, 11, 13 (bitstream part) | XL | Q5, a receiver run, then per platform | Per platform in this order: ALSA and PipeWire (a Raspberry Pi and a receiver have been used), WASAPI, then Core Audio last; each opens the carrier the owner confirms and plays a MAT stream that a receiver names as TrueHD; until then the output decision marks each as not run |
| **H7. Android** | 19, 21 | L | H5 or H6, Q12, a Shield | The bridge opens an 8-channel mask on `ENCODING_IEC61937`; the receiver names TrueHD; the 6-burst and 8-burst figures are measured again for 20 ms bursts |
| **H8. A fallback for receivers that cannot** | 12 | M | Q4 | TrueHD to AC-3 5.1 through `Ac3Transcoder`'s encoder with dialnorm from the major sync, bounded by the same checks as E-AC-3's |

Risks. H0 touches code `forge` shares: `forge` reads a TrueHD track from a container through the
same demuxers, so the fix belongs in Hearth's loader and not in the containers. H1 is the largest piece of code: the block size mismatch (40 against 256; the
renderer is organised on 256-sample blocks, and AC-4 hands it a short last block, but whether every
stage after the renderer takes any length was not read) and the memory model (rows 5 and the
index); an index of restart points and sample-exact lengths, not one entry per unit, is the plan.
H4 is held by a decision that also moves what ESP32 boards can play. H6 is the one that can fail
for reasons outside the repository: a device may not open at 8 channels and 192 kHz, a receiver
may need more than the burst FFmpeg writes, and the AC-4 HBR16 hole shows the same limit.

## Open questions for the owner

Each has a recommendation, and none is settled here.

1. **Does a coded TrueHD stream cross the network at all?** Boards cannot decode it (`esp-idf` and
   `hearth-sink` list no `truehd`), so boards always get PCM or Opus; the question is the Android
   box and the Windows `hearth-player`, which could take it coded. *Recommend:* no in the first
   round; decode at the server for every network member (H4), and decide H5 after H6 shows MAT
   works on a receiver. It saves a new wire type and a decoder in `libs/sink` until then.
2. **If it does, what is a chunk?** Raw access units in groups of 24 (20 ms; at most 17 + 61,424
   bytes, no fragments) with Pc 22 and Pd in bytes, the sink packing MAT itself; or the padded MAT
   burst (24.6 Mbit/s whatever the stream holds). And what `max_frame_rate` and `frame_rate` mean
   (the group rate, 50 a second, not 1,200). *Recommend:* raw units in groups of 24.
3. **Which presentation for 2.0 and 5.1 layouts?** TrueHD carries a 2ch and a 6ch mix made by the
   producer; the renderer can instead fold the 8ch bed as it does for DD+. Reading fewer
   substreams is cheaper. *Recommend:* the stream's own presentation when the layout equals one
   of its (2.0, 5.1), else `best` and the renderer's fold, as a Decoder-tab choice that defaults to
   the stream's.
4. **A receiver that takes no TrueHD.** Multichannel LPCM over HDMI where it is listed; AC-3 5.1 at
   448 kbit/s (lossy, no height) for an optical receiver; or a refusal. *Recommend:* LPCM first,
   then AC-3, never silent, with the loss named in the header as the E-AC-3 transcode does today.
5. **The MAT carrier.** Confirm 8 channels of 16 bits at 192 kHz (176.4 for the 44.1 family)
   (`libs/containers/ERRATA.md` question 7), and say which receiver to try first and whether it
   takes TrueHD. *Recommend:* the receiver already used for the AC-3, E-AC-3 and Atmos runs, from
   the Raspberry Pi, then Windows.
6. **Bit-perfect PCM.** Should Hearth gain an exclusive-mode integer PCM output for lossless
   audio? *Recommend:* not in round 2; say plainly on the pages that the PCM path is float32
   shared-mode, and that MAT is the exact path.
7. **Rates above 48 kHz to boards, and to anything that lists one rate.** Add a streaming
   sample-rate converter Hearth can call (the AC-4 one in `libs/dsp` is internal), or send nothing
   with a clear reason. *Recommend:* add one, because 96 kHz masters are common in TrueHD and the
   alternative is silence on a board.
8. **Which track in a file with several.** The first one Hearth can play; TrueHD when the output
   takes MAT and DD+ otherwise; or a picker. *Recommend:* the first track the chosen output can
   play, and a picker later. H0 restores the old behaviour meanwhile.
9. **Large files.** Hearth reads the whole file. A streaming or memory-mapped loader helps DD+ too.
   *Recommend:* before TrueHD is announced for films, with a stated limit until then.
10. **Dialogue normalisation and DRC.** The library applies neither and the layout of heavy DRC
    is unresolved (`libs/truehd/ERRATA.md` entry 10). *Recommend:* show the values, apply none,
    and say so on the Decoder tab, until the owner answers that entry.
11. **MLP.** Accept `.mlp` for decode (the library reads it) with no passthrough, since the
    repository has no HDMI carriage for it. *Recommend:* yes.
12. **Android.** May a session on a Shield open an 8-channel `ENCODING_IEC61937` track and list a
    `truehd` data type? *Recommend:* yes, after H5 or H6.

13. **Transport Streams over 1 GiB.** The Matroska and MP4 readers say where a frame lies in the file, so a film's
    track is read in windows; the MPEG-TS reader rebuilds a PES packet from many transport packets and says
    nothing of where they lay, so a transport stream is read whole and one over 1 GiB is refused by name. *Recommend:*
    leave it until a remux in `.ts` or `.m2ts` is wanted, since Blu-ray remuxes are Matroska; then give
    `mpegts::Reader` a callback with each payload's packet runs, which `ExtentMap` can already hold.
14. **Where the track preference is set.** `DecoderSettings::track` is automatic (the AC-3, E-AC-3 or AC-4 track for an output
    whose decision for it is a bitstream, the TrueHD everywhere else), best, or compressed; the window has no control for it.
    The speaker setup is one setup, not one per output (`settings_model.hpp`), so a stored value per output would be
    the first of its kind. *Recommend:* a three-way choice on the Decoder tab beside the presentation, stored with the other
    decoder settings, and automatic as the default until H6 sends TrueHD to a receiver as it is; then automatic is
    retired for a receiver that takes MAT.
15. **PCM at another rate beside the stream as it is.** A group has one timeline in the stream's frames when any member is
    sent the coded stream, so PCM members that need another rate are held back there. The case is rare (AC-3 and E-AC-3 are
    32, 44.1 and 48 kHz; TrueHD's high rates have no coded form on the network until H5) and becomes common with H5 for a
    96 kHz item with one capable receiver and some boards. *Recommend:* decide it with H5: either the group's PCM follows the
    coded timeline by frame count at the converted rate, or the boards are sent nothing for that item.
16. **The cost of the map of a track.** The map keeps 24 bytes for each run of the track in the file, and a run is a frame
    when other tracks lie between the frames. A remux with one TrueHD access unit to a block is some 8.6 million blocks in two
    hours, 200 MB; one with a block of 24 units is 8 MB. This session has no remux of a disc to count. *Recommend:* the
    owner opens one in Hearth and reports the memory; if it is large, keep one entry for each 64 KiB of the track and parse
    from the nearest cluster, which is a change inside `ExtentMap`.
17. **The order of H5 and H6.** Branch `feature/truehd-hearth-endpoints` adds `BitstreamFormat::kMat` and edits
    `data_type_of()` and the output decision, which H5's data type touches. *Recommend:* merge it first and start H5 on top.
    H5 and the ESP32 phases 0 to 4 also need board time (P4, S3, C6), which was not asked for because no phase that
    needed one was reached.

The questions that block the most work are 1 and 2 (H5, H7), 5 (all of H6 and so H7), 7 (part of
H4) and 8 (the last part of H0).
