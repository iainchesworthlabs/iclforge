# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

See [README.md](README.md) for the project overview and
[Releasing](docs/releasing.md) for the release process.

## [Unreleased]

Changes pending release are one file each in [`changelog.d/`](changelog.d/README.md); a release moves them into the section below.

## [0.11.0-beta.1] - 2026-11-09

Eleventh tagged release. TrueHD and MLP become a full programme: a lossless decoder and encoder, a TrueHD Atmos object layer that is written and read here (provisional until an outside decoder reads it), the main `forge` commands, the Forge window, and MAT playback to a receiver from Hearth and Crucible. AC-4 arrives complete: a decoder and encoder for channel-based, immersive and object streams, in `forge`, Hearth, the language bindings and the ESP32-P4 and S3, with content signing under the operator's own key. Annex C karaoke streams are reproduced by the decoder, shown with word-timed lyrics and scored against a singer's pitch in Hearth. Hearth grows into a Windows, macOS and Linux player with a Sendspin network and an Android sink app, and the ESP32 boards become Sendspin players that update their own firmware over the network, with the P4X panel drawing lyrics and the room. The project is now ICL Forge, with new names for every program and library.

Before you upgrade: the programs are `forge`, `forge-gui`, `hearth` and `crucible`, with no alias for the old names ([Renamed](docs/renamed.md)), and every already-flashed ESP32 board needs one USB flash before network updates work again. E-AC-3 streams without `mixmdate` now fold at -3 dB, and RF mode no longer decodes 11 dB below a Dolby decoder. macOS 14.4 is the minimum. Several paths are written but not yet run on the hardware they target, and their entries say so: notably TrueHD to a receiver on Windows, macOS and Android, AC-4 to a receiver, and the ESP32-P4X panel.

### Added

**TrueHD, MLP and Dolby Atmos in TrueHD**

- **`forge truehd-encode` writes lossless TrueHD from one or several WAVs, placing each channel by name.** `src=`, `map=` and `offset=` work as for the other encoders (`map=0.0:L,0.1:R,1.0:C,2.0:LFE,2.1:Ls,2.2:Rs`); a map that is not one valid channel assignment is refused with the nearest valid sets. Tuning options are `truehd-restart=`, `truehd-major-sync=`, `truehd-peak-rate=`, `truehd-bitrate=` and `truehd-stereo-mode=`, plus the provisional channel-meaning fields (`truehd-dialnorm-6ch=`, `truehd-mix-level-8ch=`, `truehd-drc-start-up-gain=`). Output is a bare `.thd` or a Matroska, MP4 or Blu-ray transport stream file.
- **Streams are held to a bitrate, and `forge probe detail=rate` reports how well.** The arrival model of TH spec 3.4 and 18 (`RateModel`) runs at the declared peak and at a chosen `truehd-bitrate=<kbit/s>`, so one loud burst no longer ends a long stream. `truehd-rate-mode=shape` lets Atmos object channels give up low bits in intervals that do not fit; off by default and provisional.
- **`forge truehd-atmos` writes a TrueHD Atmos stream (provisional, unsigned).** A WAV's channels (first the LFE) or an ADM master become the object substream (12, 14 or 16 channels); the 7.1 bed, from a WAV (`truehd-bed=`) or rendered from the objects, is in substreams 0 to 2. FFmpeg's strict decode returns the bed. 96 000 and 192 000 Hz are accepted; the Dolby Reference Player plays the bed at both but has not yet read the object metadata there. `truehd-evo-frames=all|every:N|first|none` chooses which metadata frames get an evolution frame, since a licensed decoder rejects tags that are not its own. `EncoderConfig::object_paths` moves objects block to block and `object_downmix` takes a general 8 by N downmix of 9 to 16 objects. The bed render uses the law measured on DEE controls (`iclforge/truehd/encoder/bed_pan.hpp`), within 0.02 % for a lone fixed object; interior places, height and size are not measured.
- **`forge decode` reads the object layer of a TrueHD Atmos stream (provisional).** With `truehd-presentation=all`, `objects_dir` writes each object as a mono WAV, `adm_out` an ADM master, and `truehd-render=<layout>` renders onto a speaker layout; `forge probe` names the object substream. `truehd-reading=` (`seed-shr7-32`, `zero-histories`, `shift-by-position`, `fold-by-position`) decodes with the other reading of a point the specification leaves open, for scoring against Dolby's DEE set.
- **TrueHD evolution frames can be signed with your own key.** `forge truehd-atmos sign-objects signing-key=PATH` and `forge metadata <in> <out> sign-objects signing-key=<path>` write a keyed HMAC-SHA-256 tag over the access unit; `forge decode ... verify-objects` or `gate-objects` check it (repeat `signing-key=` for a keyring), and `gate-objects` strips the object layer from units that fail so the stream plays as its bed. The library is `iclforge::truehd::signing`. The tag is one byte: it detects tampering but does not authenticate. It is ICL Forge's own scheme, not Dolby-authentic; a licensed Dolby decoder will not accept it. See [Object signing](docs/concepts/object-signing.md).
- **`forge probe`, `forge qc` and `forge normalize` understand TrueHD and MLP.** `probe` reports data rate against the declared peak, presentations and restart spacing; `detail=checks` adds the lossless check and `detail=matrix` lists every matrix section and `output_shift`. `qc` also fails a misplaced terminator word or a `zero_samples` of 4096 or more, as a licensed player does. `normalize` writes the 2 channel dialogue norm as the measurement plus 6, as DEE does.
- **More `forge` commands take TrueHD and MLP.** `cut` and `cat` move whole access units; `silence`, `sine` and `record ... codec=truehd` write `.thd`; `monitor` plays a stream; `transcode` writes AC-3 or E-AC-3 (AC-4 is refused); `decode channels=2` reads the stereo presentation.
- **A Forge project of codec `truehd` carries the whole encode** (options, several sources with `offsetSeconds`, `assignments`); `forge project p.forgeproj out print` prints the equivalent `forge truehd-encode` line. A project with several WAVs no longer drops all but the first.
- **The Forge window opens, plays, checks and encodes TrueHD and MLP.** Open stream takes `.thd`, `.mlp` and a TrueHD track inside Matroska, MP4 or a Blu-ray transport stream, plays a window at a time (a film that would decode to about 11 GB plays in a few megabytes), seeks and exports a 24-bit WAV; QC gives a card per presentation. **Encode TrueHD...** takes several sources with a channel map and offsets, picks the container from the Save as name (`.thd`, `.mkv`, `.mp4`, `.m2ts`) and shows the twin command line.

**AC-4**

- **A complete AC-4 decoder** (`libs/ac4`, `iclforge::ac4::Decoder`, from TS 103 190-1 and -2). It decodes channel-based streams from mono to 7.X in SIMPLE, A-SPX and A-CPL; the immersive element (5.1.4, 7.0.4, 7.1.4); 9.0.4 and 9.1.4; 22.2; object audio (A-JOC, direct-coded objects and their metadata); the speech spectral frontend; the efficient high frame rate mode; and 96 and 192 kHz SIMPLE substreams. Every `frame_rate_index` is converted to 48 kHz, and DEE's 2.0 streams decode at unity gain within 0.03 dB. 96/192 kHz, 22.2, 9.X.4, efficient high frame rate and the speech frontend have no real streams to test against and are checked on constructed streams only. Hearth and the ESP32 player refuse 22.2 and 9.X.4.
- **Presentations, mixing and a player API.** `DecoderConfig::presentation` selects by `presentation_id`, position, language, associated-audio service or `b_pre_virtualized`; main plus associated audio and music-and-effects plus dialogue mix within 0.01 dB of their formulas. `OutputConfig` sets output level, DRC mode, dialogue enhancement and downmix (5.X, stereo, mono; Lo/Ro, Lt/Rt or Pro Logic II), and `ConcealmentPolicy` repeats and fades, or mutes, an undecodable frame. For players, `set_output()` and `set_presentation()` apply from the next frame, `decode_by_block()` returns 256-sample blocks, and `SyncFrameSplitter` splits sync frames out of a piecemeal stream. The library installs as `iclforge::ac4` (pkg-config `iclforge-ac4`); see [AC-4](docs/library/ac4.md) and `examples/decode_ac4.cpp`.
- **An AC-4 encoder** (`libs/ac4`, `iclforge::ac4::Encoder`) for 48 and 44.1 kHz PCM from 8 kbps: SIMPLE and ASPX in mono, stereo, 5.0 and 5.1, A-CPL (ASPX_ACPL_2 and 3) for 5.1, every frame rate, average and variable rates, loudness, DRC and downmix metadata, dialogue enhancement, and several substreams and presentations. At 192 kbps its SNR is 5.6 dB above DEE's on music and 14.9 dB on speech; `CodecMode::kAuto` steps down so 5.1 still encodes from 20 kbps. `Encoder::refusal_reason()` names the rule a refused configuration breaks, `build_dac4()` writes the full `dac4` box for every presentation (matching DEE's muxer byte for byte on the reference streams), and `cmaf_refusal()` names the rule a stream breaks for a CMAF track.
- **`forge` speaks AC-4 throughout.** `decode` reads raw or MP4 AC-4 and takes `output-level=`, `drcmode=`, `dialogue-enhancement=`, `downmix=`, `conceal=`, `presentation=`, `presentation-id=`, `language=`, `associated=`, `decoding=full|core`, `speakers=` (5.1 to 7.1.4; objects render to 7.1.4 by default) and `syntax-trace=<file>`. `ac4-encode` writes raw sync frames or MP4 with `codec-mode=aspx|aspx-acpl-1|aspx-acpl-2|aspx-acpl-3`, `frame-rate=`, `rate-mode=`, `iframe-interval=`, `dialnorm=`, `loudness=`, `drc=`, `substream2=` to `substream32=` and `presentation1=` to `presentation64=` with per-item keys, and `substreamN-enhances=` and `dialogue-hybrid=` for hybrid dialogue enhancement. `transcode` converts AC-4 to and from AC-3 or E-AC-3; `record` and `live` encode with `codec=ac4`; `monitor`, `play`, `qc`, `levels`, `loudness`, `spdif` and `probe` handle AC-4, and `probe` reports each presentation in `stream.ac4` of `iclforge.probe/1`.
- **AC-4 over IEC 61937-14.** `iclforge::containers::iec61937::Ac4BurstPacker` packs a sync frame into any of the four burst types (AC-4, HBR4, HBR16, LD); `BurstReader`, `unwrap_stream` and `PassthroughDetector` read them. `PassthroughSink` takes `BitstreamFormat::kAc4`, `kAc4Hbr4` and `kAc4Hbr16`: ALSA and Android send the first two, WASAPI, PipeWire and Core Audio refuse with `PassthroughError::kUnsupportedFormat`, and every backend refuses HBR16. Hearth's `_iclforge_player@v1` role carries `"ac4"` burst chunks. No receiver found accepts AC-4, so none has been tried.
- **The AC-4 encoder codes objects** (`experimental.objects`). Each object's PCM and Annex F properties are given over time (`encode()` takes `ObjectMetadataUpdate`s) and coded as an A-JOC substream or as direct-coded object substreams; updates land within 32 samples of their input and decoded objects return at 40 to 75 dB SNR. `forge ac4-encode objects=<scene file>` takes a scene.
- **Experimental AC-4 encoder layouts and tools.** No outside reader has read any of these (the readings are the specification text's alone, `libs/ac4/ERRATA.md`), and none is in the C API. Immersive 5.0.4 and 5.1.4 are written as DEE writes them (SCPL from 640 kbps, ASPX_SCPL from 480, ASPX_ACPL_2 below); `forge ac4-encode` takes the layout from the WAV's channel count with `codec-mode=scpl|aspx-scpl|aspx-ajcc`, `height-downmix=` and `height-gain=`. Opt-in through `EncoderConfig::experimental` or `experimental=`: 7.0.4/7.1.4 with the back pair (`back_pair`), ASPX_ACPL_1 (`acpl`) and A-JCC (`ajcc`); 9.0.4 and 9.1.4 (`nine_x_4`, `nine-x-4`; decode with `md-compat=7`); the 22.2 element as 24 channels (`twenty_two_two`, `twenty-two-two`; A-CPL, companding, dialogue enhancement, DRC gains and downmix values are refused by name); spectral noise fill (`noise_fill`, `noise-fill`); the efficient high frame rate mode at constant frame rates (`frame_rate_fraction` 2 or 4, `hfr-2`, `hfr-4`); and 3.0 (`three-zero`).
- **The AC-4 decoder builds in `float` and fixed-point tiers and is smaller and faster.** `ICLFORGE_DECODE_SCALAR=float` and `=fixed` (on `Fixed32`) build beside `double`; `float` agrees with `double` to 109 dB or better below A-SPX's crossover (fixed 105.7 dB), and each tier gives one PCM on every platform tested (MSVC, GCC, Clang, Cortex-M3 under QEMU, RV32IMC, ESP32 boards). A 64-sample QMF slot takes 0.77 us in analysis and 0.83 us in synthesis at `double` (from 3.15 and 3.3), and per-substream state is 10.9 KB (from 299 KB). `Decoder::decode(frame, next)` and `DecoderConfig::executor` use a second core without changing the PCM, and the frame-rate converter's compile-time tables bring the first 1001/960 frame on the P4 to 0.31 s from 5.9 s.
- **AC-4 plays on the ESP32-P4, ESP32-S3 and, in the fixed tier, the C3 and C6**, behind `CONFIG_ICLFORGE_AC4` (off by default, `sdkconfig.ac4`); `hearth_sink` plays an AC-4 stream from its HTTP source. On a P4 at 360 MHz with Wi-Fi up, a 5.1 frame takes 0.42 to 0.73 of its duration (real time) and 5.1.4 takes 0.94 to 1.21. A C6 cannot fit AC-4 beside Wi-Fi, so a C6 sink takes AC-4 programmes from Hearth as PCM. The S3's decode runs under QEMU in CI; no S3 board has run it, so there is no S3 hardware time. Figures: [ESP32-P4](docs/platforms/bare-metal/esp32-p4.md#ac-4), [ESP32-S3](docs/platforms/bare-metal/esp32-s3.md#ac-4), [ESP32-C6](docs/platforms/bare-metal/esp32-c6.md#ac-4).

**AC-3 and E-AC-3**

- **E-AC-3 folds a stream with no `mixmdate` at -3 dB, as Dolby's decoder does.** The default Lo/Ro levels were AC-3's -4.5 and -6 dB; a stereo or mono fold of such a stream is now 1.5 dB louder in the centre and 3 dB louder in the surrounds. Streams with `mixmdate`, and all AC-3, are unchanged.
- **`encode` and `eac3-encode` warn about `bsmod=7` on mono and on 1+1,** where the specification gives it another meaning or prohibits it; the stream is still written.
- **E-AC-3 associated services mix into the main programme.** `iclforge::ac3::AssociatedServiceMixer`, `forge decode associated=1|visually-impaired|<0..7>` (with `associated-gain=<dB>`) and `forge monitor associated=` mix a description or commentary carried as a second independent substream using Annex E section E3.10's controls. `play` and `spatial` ignore `associated=`; Hearth does not mix.
- **`forge metadata ... insert` adds a field an E-AC-3 stream lacks.** `compr`, `compr2`, `bsmod` and `dsurmod` go into every independent substream (`insert_stream_metadata()`), re-stamping `frmsiz` and the CRC. AC-3 cannot take it.
- **`forge transcode` keeps `bsmod`.** A karaoke, voice-over or associated-service stream transcoded between AC-3 and E-AC-3 used to come out `bsmod` 0.
- **A fixed-point decode tier** (`-DICLFORGE_DECODE_SCALAR=fixed`), a Q7.24 integer path for parts with no FPU (an ESP32-C3, a Cortex-M3); it measures 121 dB+ on the gold streams and 111 dB+ on third-party fixtures.
- **Faster E-AC-3 7.1.4 and Atmos JOC decode on small parts.** JOC reconstruction runs in single precision where the FPU is single precision (140 dB against the double bank), and `DecoderConfig::executor` hands independent work to a second core with bit-identical output. On the ESP32-S3 JOC with four objects runs at 0.92 of real time (12 to 28 before); every stream measured on the ESP32-P4 is faster than real time ([ESP32-S3](docs/platforms/bare-metal/esp32-s3.md#ac-3-and-e-ac-3-in-the-hearth-player), [ESP32-P4](docs/platforms/bare-metal/esp32-p4.md#ac-3-and-e-ac-3-in-the-hearth-player)). `decode_frame_by_block` delivers 256-sample PCM blocks to a `BlockSink`, and `delta=off` skips the section 7.2.2.6 search.
- **`eac3-encode` authors all eight section E2.3.1.2 programmes.** `programme2=` through `programme8=` each add an independent substream with its own `programmeN-layout=`, `-bitrate=` and the primary's full metadata (`bsmod=`, `dsurmod=`, `dmixmod=`, `pgmscl=`, `mixdef=`, `dialnorm=<1..31>|auto` and the rest). A `programmeN=` without them is refused.
- **`numblkscod=N` (0-3) on the `atmos*` encode commands** carries the object layer over short syncframes of 1 to 3 blocks.
- **`downmix=auto` on `decode` and `monitor`** picks Lt/Rt or Lo/Ro from the stream's first `dmixmod` (A/52 section D3.1.1) and prints the choice (`iclforge::ac3::automatic_stereo_target()`). `probe` now reports `dmixmod`, including `metadata.dmixmod` in `iclforge.probe/1`.
- **Fold levels follow the stream, and the caller can set them.** The AC-3 decoder folds an Annex D stream with its own `xbsi1` levels (A/52 section D3.1.2): Lt/Rt uses `ltrtcmixlev`/`ltrtsurmixlev`, and Lo/Ro and mono use `lorocmixlev`/`lorosurmixlev`, where all three used `cmixlev`/`surmixlev`. Callers folding for themselves use `iclforge::ac3::mix_levels()`; `bsid`-8 streams, and `bsid`-6 streams without `xbsi1`, fold as before. `DecoderConfig::drc_boost_scale` gives a `dynrng` word above unity its own scale, and `OutputConfig::mix_override` replaces the stream's Lo/Ro, Lt/Rt and LFE levels one field at a time; both default to the previous behaviour.

**Immersive audio and object formats (Atmos objects and JOC, IAB, IAMF, ADM/BW64)**

- **IAMF v2.0.0 reads and writes, carries Opus, AAC-LC and FLAC, and rebuilds scalable audio.** `iclforge::containers::iamf` covers channel-based, scene-based and object-based Audio Elements, Mix Presentations, Parameter Blocks and trimming, as a raw OBU stream (`write_sequence()`/`read_sequence()`) or ISO-BMFF file and fragments (`write_isobmff()`, `read_isobmff()`, `FragmentedWriter`). `mux_objects()` writes animated objects; `mux_coded()` writes caller-encoded packets (no codec is linked) that FFmpeg 8.0.1 decodes to the same samples. `decode_pcm()` reconstructs up to six scalable layers and `expanded_layout_info()` lists the 20 expanded layouts.
- **The Atmos object renderer applies channel lock, zones, extent and extended-precision positions.** `LayoutRenderer` (Hearth, the ESP32 player, `forge decode`'s AC-4 objects) follows TS 103 420 §5.2. Divergence and the screen and depth factors are still not applied.
- **IAB (SMPTE ST 2098-2) gains lossless audio, a writer and IMF Track Files.** `iclforge::iab::decode_dlc()` decodes `AudioDataDLC` bit exact, so `forge atmos-iab` accepts DLC-coded masters; `write_iabitstream()`, `encode_dlc()` and `write_mxf_iab()` (ST 2067-201) write them, checked with Netflix Photon.
- **The IAB bridge carries spread, zones and every defined channel.** The full Atmos cinema bed, Center Height and Top Surround no longer fail with `kUnsupportedIabChannel`; `forge atmos-iab` warns about inexact zones.
- **ADM/BW64 carries Matrix, HOA and Binaural, both ways.** `parse_bw64` reads Matrix blocks and pack references (libadm skips them) and HOA block details; `write_bw64` writes polar positions, HOA/Binaural/Matrix channels and packs, nested `audioObject`s and `audioPackFormat`s, and an Objects block's `diffuse`, `importance` and `channelLock`. `AdmWriteOptions` picks 16/24/32-bit integer or 32/64-bit float (default 24-bit integer), and 96 kHz masters no longer have their sample rate narrowed to 16 bits. The bridge to Atmos still refuses Matrix, HOA and Binaural packs.
- **Rendering constraints round-trip.** `AudioBlockFormat` carries `zoneExclusion`, `objectDivergence`, `screenRef` and `headLocked`; zone exclusion maps both ways to `ZoneConstraint` (TS 103 420 Annex B.2.6), and the OAMD writer transmits divergence and screen reference. `BridgeResult::unmapped` lists what the bridge dropped, and `forge atmos-adm` warns. The OAMD decoder reads divergence from the printed Tables 41 and 42 instead of guessed tables.
- **`forge decode ... adm_out` writes an ADM master for E-AC-3 and AC-4 objects.** For E-AC-3 that is not dynamic-object-only it writes a channel-based-immersive bed programme (bed channels as `DirectSpeakers` at their Table 12 positions, objects with OAMD timelines); ISF objects, a second bed instance, a Table 13 assignment and an LFE2 are still refused with a warning. For AC-4, `objects_dir` writes each object to `object_NN.wav` and `adm_out` (needs `-DICLFORGE_BUILD_ADM=ON`) builds the ADM BWF from the decoded Annex F properties; the `zone_mask` reading is not independently verified against the specification text. `probe`'s JSON gains an `oamd_common_data` object.
- **Object metadata encoding covers what the decoder reads.** `AtmosEncoder` marks an object `b_object_not_active` for a frame with no energy in any band, and `oba::build_payload_updates()` writes several metadata updates inside one E-AC-3 frame for sub-frame object motion.
- **`forge atmos-adm`, `atmos-iab` and `atmos-encode` take `codec=ac4`.** Every bed and object channel of an ADM or IAB source (or a WAV's channels, or `src=`, `map=` and `offset=`) becomes an AC-4 A-JOC object, or with `coding=direct` a direct-coded one, moved by an optional scene file; `.mp4`, `.m4a` and `.mov` names write an MP4 file, `dialnorm=` sets dialnorm and `crc=off` drops the CRC; `coding=` or `crc=` without `codec=ac4` are refused. A round trip through AC-4 keeps object positions to 0.06 and gains to 2 dB.

**Containers, streams and carriage (MP4, Matroska, MPEG-TS, fragmented MP4 and DASH/HLS, IEC 61937 and MAT, lyrics carried in files)**

- **The MP4 and Matroska readers list the tracks they do not read.** `Demuxed::skipped` in `iclforge::containers::mp4` and `::matroska` holds each other track's id, kind, codec and language; `forge probe` prints an `other tracks` line and writes `container.other_tracks`, so a person can see where a karaoke video's words are carried.
- **An AC-3 core with E-AC-3 dependents is carried in MP4, fMP4 and MPEG-TS** (`forge mp4`, `fmp4`, `ts`); Matroska still refuses it. `dec3`'s `chan_loc` is now written from the dependents' `chanmap` (it was always 0).
- **A multi-programme stream keeps every programme in MP4, fMP4 and MPEG-TS** (they used to write the first). `programme=<0..7>` on `mkv`, `mp4`, `fmp4` and `ts` writes one alone. FFmpeg 8.0.1 cannot decode a multi-programme stream.
- **`forge demux` reads an MP4 whose `moov` follows its `mdat`** (`demux_seekable()`); from a pipe it is still refused, with the reason.
- **`forge fmp4`'s DASH manifests are validated against ISO/IEC 23009-1's schema** by `tools/checks/verify_dash_schema.py`.
- **MPEG-TS associated-service signalling reads back as well as writes.** `iclforge::containers::mpegts::demux` and `Reader` decode the PMT's AC-3/E-AC-3 descriptor into `ReadStream::service`. `forge ts` accepts `asvc=0,2` beside a raw mask, and `mainid=`/`asvc=` are checked against the stream's `bsmod`, so a mismatch is a usage error rather than a silently wrong descriptor.
- **AC-4 in containers.** `forge mp4`, `ts` and `demux` carry AC-4 byte-identically (`ac-4` sample entry, `dac4` box, DVB descriptors), including 29.97, 59.94 and 119.88 fps. `fmp4` fragments AC-4 as a CMAF track (fragments start at I-frames, `ca4m`/`ca4s` brands). `mkv` refuses AC-4, for which Matroska registers no codec ID.
- **MP4 edit lists are read and written.** `ReadTrack::edits` reports a track's `elst` entries and `MuxOptions::edit` writes one edit; an inconsistent edit list is left out and the file still reads. Hearth's player applies it; `forge` and the GUI do not yet.
- **`iclforge::containers::iec61937::burst_from_parts`, `burst_period_bytes` and `null_burst`.** A sink that bitstreams a Sendspin burst chunk holds only `Pc`, `Pd` and the payload; `burst_from_parts` rebuilds the full IEC 61937 burst byte for byte (AC-3, E-AC-3 with JOC, AC-4). `null_burst` gives a null-data burst, which a receiver's Dolby lock survived for 6 s where PCM silence lost it in half a second.

**Karaoke (lyrics, scoring, microphone, karaoke streams)**

- **Karaoke streams are reproduced (Annex C).** AC-3 and E-AC-3 karaoke streams decode to the aware reproduction (L, C, R and the LFE) or the capable one with `karaoke_vocals` (none, V1, V2, both) and part gains, set through `OutputConfig::karaoke`; `output_mode()` gives the layout. E-AC-3 does this by analogy with Annex C, as Dolby's reference decoder does, for one independent substream with no object layer and `bsmod` 7 above 1/0; with a dependent substream or objects it plays as coded. AC-4 has no karaoke mode. `eac3-encode` with `bsmod=7` writes `mixmdate` (`cmixlev=`, `surmixlev=`, -4.5 and -6 dB by default). Hearth, the Forge GUI's Karaoke flow and the WebAssembly `PushDecoder` use it.
- **CLI commands read the karaoke choice.** `forge decode` and `forge monitor` take `karaoke` and `karaoke=none|v1|v2|v1+v2`, and re-level the capable decoder with `karaoke-melody=<dB>`, `karaoke-v1=<dB>` and `karaoke-v2=<dB>` (0 dB is the table, -24 or `off` mutes, up to +12). `play`, `spdif` and `spatial` accept the choice and warn it is not applied.
- **Hearth has a Karaoke page** (`Ctrl+7`) that chooses the reproduction and vocals, shows which part each speaker carries, and has three level sliders with **Reset levels**. Choices are kept across starts.
- **A karaoke stream is one reproduction across a network group.** `_iclforge_player@v1` carries `karaoke`, `karaoke_vocals` and `karaoke_melody_db`/`karaoke_vocal1_db`/`karaoke_vocal2_db`; Hearth sends the stream undecoded to a sink that lists its codec in `karaoke_codecs` (default AC-3 alone) and PCM to one that does not, and tells members what is playing through `metadata@v1`. The ESP32-S3 lists AC-3 and E-AC-3 (within 0.25 dB of the host's decode); the C6 (two channels) and a v1.x P4 (2.0 only) get PCM. New `lyrics` and `lyrics_position` commands keep a sink's lyrics in step.
- **A sink can ask the server for the singer's karaoke choice.** A `client/command` `_iclforge_player` request, limited to the keys the support object's `requests` lists (reproduction, vocals, three levels), is applied as the Karaoke page's own change; an out-of-range request is refused whole. Hearth for Android's controls sheet has a Vocals row (Both, V1, V2, None) that uses it, but the TV plays every vocal whatever the server decides, and the row says so. The Android remote and the panel's touch control are not wired.
- **The lyric reader is a library.** `iclforge::lyrics` (`libs/lyrics`) reads LRC (UTF-8 or UTF-16), SubRip, Advanced SubStation Alpha and MIDI karaoke `.kar`, with duet singers from `V1:`/`V2:` markers and `[v1:name]`/`[v2:name]` tags; `read_midi_melody()` picks a `.kar` melody track. UTF-32 and 8-bit code pages are refused by name. In ASS, two actors or styles are the duet singers and Aegisub templater scripts read their `karaoke` lines, not the `fx` effects (not yet checked on a real exported script).
- **Forge writes lyrics beside the stream it encodes.** `lyrics=<file>` on `forge encode`, `eac3-encode` and `ac4-encode` takes `.lrc`, `.srt`, `.ass` or `.kar` and writes `<stream>.lrc` (UTF-8, duet markers, word times). `lyrics-encoding=<page>` reads a Windows, ISO, DOS, KOI8, Shift-JIS, EUC, GBK, GB18030 or Big5 code page (never guessed); `forge lyrics-to-lrc <in> <out.lrc>` converts with no stream. The Forge GUI's Lyrics row does the same with a **Code page** list. Library: `place_lyrics_beside()`, `convert_lyrics_file()`.
- **Hearth shows, times and edits lyrics.** The Lyrics card follows the `.lrc` beside the stream word by word (delay -1,000 to +1,000 ms); `F11` opens a full-screen singer view with **Queue** (`Q`), and `V1:`, `V2:` or `V1+V2:` lines show the singer by name, side and colour. **Time the lyrics...** stamps lines and words with `Space`, and **Save** writes an enhanced `.lrc` (keeping `.lrc.bak`) with duet markers from a **Singer** with two **Name** fields.
- **CD+G karaoke disc pictures are shown.** A `name.cdg` with no lyrics file is drawn in Hearth, Crucible and the Forge GUI's Open stream dialog; if a song has both, **Show** (Words or Picture) picks.
- **`iclforge::pitch` (`libs/pitch`) scores a voice against a tune,** and Hearth's engine uses it: a McLeod pitch tracker, reference melodies from audio or MIDI notes, a scorer, and `BleedMonitor`, which flags frames where the microphone is mostly the speakers (provisional threshold; Hearth does not call it yet). `Performance` places each frame at the heard position (less the microphone's declared delay plus an adjustable offset) and scores how sharp or flat and how early or late. `reference_from_karaoke()` takes the tune from the stream being played (melody M, V1, V2, or the part the singer has muted) and refuses a stream with no clear pitch, with the reason.
- **Hearth's Karaoke page has a Microphone card with a pitch score.** It is off until turned on and nothing leaves the machine; the score shows only with "I am wearing headphones" (open speakers make it read as nearly perfect). It shows cents sharp or flat and a running score (tolerance 50, 100 or 200 cents; "Score against my key" for a consistently off-key singer). A pitch ribbon (also in the `F11` view) draws the tune and the voice, each sung line gets a mark (dots and a percentage, or **not sung**), and a **Last performance** summary is never ranked, named or sent. **Record a take** keeps a mono 24-bit FLAC with a `.take.json` under "Hearth takes" in documents (backing not mixed in); "Your takes" lists the newest five with **Delete**.
- **`hearth --smoke-shot x.png --page karaoke --open <song> --sing <voice.wav> --seek <ms>` captures a sung performance** with no singer or microphone (`VoiceFileInput`); `--finish-singing` captures the **Last performance** card. A non-WAV file or a stream with no melody exits with code 3. [Timing a song with a forced aligner](docs/karaoke/forced-alignment.md) uses WhisperX (run outside the repository); `examples/karaoke/auld-lang-syne/` and `examples/karaoke/leave-her-johnny/` (a duet) are generated, nothing recorded.

**Hearth (desktop and Android players, Sendspin network output)**

- **`hearth-player` is a Sendspin player that plays through the computer's speakers (Windows).** It lists `_iclforge_player@v1` (AC-3 and E-AC-3, decoded here) and `player@v1` (PCM, FLAC, Opus), advertises as `hearth-pc-<computer name>`, and plays each burst at its server time. Options: `--list-devices`, `--device ID`, `--device null`. See `docs/hearth/player.md`.
- **Hearth opens MP4, Matroska and MPEG-TS files and takes files from the command line.** It demuxes the first AC-3, E-AC-3 or AC-4 track and honours an MP4 edit list's priming and padding. `hearth song.ac3` or `--open <file>` (with `--seek <ms>`) queues and plays a file; a second launch hands its files and `--page` to the running Hearth over a local socket ([Hearth's command line](docs/hearth/index.md#command-line)). Hearth also plays to the output chosen last time.
- **A player engine** (`apps/hearth/engine`): play queue and transport with gapless joins and MP4 edit lists applied; decoder settings change on the playing item with no loss; level meters and programme loudness follow the device clock; pause and seek work until an item's tail has been heard. `Engine::unit_report()` gives each unit's channels, service, dialnorm, `compr`, `dynrng`, fold levels, concealment and object metadata at play time.
- **Hearth plays AC-3 and E-AC-3 as bitstreams, or decoded here, as the output decision says.** An item is sent its own access units over IEC 61937 while the decode still feeds the meters; only whole units can be sent, so an edit list's priming or padding inside a unit is heard. The output reopens, and says why, when a join changes stream, endpoint or burst shape. For a receiver that takes only AC-3, E-AC-3 is transcoded (to 3/2 plus LFE at 448 kbit/s) with a protective `compr` word so RF mode stays protected; `dynrng` is not carried.
- **Hearth plays AC-4.** `Session::open()` takes an AC-4 elementary stream with exact durations at every frame rate (29.97 fps included) and seeks equal to an unbroken decode; a stream with no decodable presentation is refused with the decoder's reason. `DecoderSettings::ac4` holds presentation, language, audio description, dialogue level and enhancement, dialogue normalisation, DRC mode and fold, shown on the Decoder page's AC-4 tab and the Media page. AC-4 objects render through the layout renderer (`immersive_layout`, `core_decoding`). A network group is sent AC-4 as IEC 61937-14 bursts for members that list `"ac4"`.
- **Hearth plays TrueHD to a receiver as MAT, on desktop and Android.** Access units are packed 24 to a burst on the eight-channel link, by default through ALSA and PipeWire and elsewhere when Bitstream is chosen in the Output picker. A Raspberry Pi 4B played TrueHD to an Onkyo TX-RZ740 on both (the receiver took 1.5 to 4.8 s to lock, so an item starts silent); WASAPI, Core Audio and Android are not run (Android only against a model of the device, not a Shield) and the macOS backend was written without a Mac. A remux with TrueHD first and AC-3 or E-AC-3 after it plays as the receiver can take it (`DecoderSettings::track`). A Matroska or MP4 TrueHD track is read a window at a time, not copied whole into memory; a Transport Stream is still read whole, and one over 1 GiB is refused.
- **`_iclforge_player@v1`, Hearth's own Sendspin role, carries coded AC-3/E-AC-3 with Atmos objects, and TrueHD, for the sink to decode and render.** TrueHD is the data type `"truehd"` (1 to 24 whole access units a chunk) with a `max_data_rate` in the support object; a stream outside a sink's limits is sent as PCM, naming the limit on the Network page. Not done: starting at the restart before a seek.
- **A full Sendspin implementation in `libs/sendspin`** (`-DICLFORGE_BUILD_HEARTH=ON`), usable as player, server or both: Noise encryption, pairing (PSK, dynamic and static code), mDNS discovery, clock synchronisation, `player@v1` with PCM, FLAC and Opus, the other roles, and `ServerHost`/`Group` for playing one programme in sync to several clients. It speaks the specification's form and aiosendspin 9.1.1's (the version Music Assistant uses); Music Assistant itself has not been tested.
- **The Media page reads each queue item's own file**: codec, sample rate, measured bitrate, duration and container for AC-3, E-AC-3 and AC-4; dialogue and mix levels; programmes; an **Objects · OAMD** table; and **Container** and AC-4 **Immersive** cards. Copy/Export JSON use `iclforge.hearth.media/1`.
- **Network page: discovery, pairing and groups.** `hearth` browses `_sendspin._tcp` and lists every player found, live. An unpaired sink is paired with its own button and the six-digit code it shows; a sink held by another server offers "Pair with this computer" or "Take it back". The server's Noise identity is kept (`identity/server`), so pairings survive a restart, with "Forget this pairing". "+ New group..." makes a group with member and group volume and mute, and "Play here" or "Play to this group" sends playback to it. Four Hearth sinks played one E-AC-3 programme as a group, 312 of 312 bursts each, every frame within 0.6 ms of due.
- **A group is planned by what each member takes**: a member that decodes the stream keeps it coded, one that cannot is sent PCM decoded here, and one that can take neither is held back with its reason in the log. A sink whose buffer holds under a second is sent PCM in units of at most 10 ms. PCM is converted to the nearest rate a sink lists in the family (96 to 48 kHz) with `iclforge::dsp::StreamResampler` (loopback test only). An "Output" row reads "passed through as coded" or "decoded by the sink" from the extension role's optional `output` state key (`"passthrough"` or `"pcm"`), and "not reported" for older sinks.
- **Diagnostics leave a running app four ways**, all the same scrubbed report (Settings > Diagnostics): "Save diagnostics", "Copy diagnostics", "View live..." (every 500 ms), and on Windows `OutputDebugString`. `ICLFORGE_HEARTH_DIAGNOSTICS_PORT` turns on a loopback `GET /diagnostics`. Folders are withheld (`C:\Music\a.ec3` reads `<withheld>\a.ec3`) and settings under `pairing/` and `queue/` are left out.
- **Settings and saved queue.** Playback (gapless, resume where it was, "An item fails: Stop") and Network settings (name shown to sinks, whether to look for Sendspin players) persist, and the queue with item and position is restored without playing.
- **Hearth's speaker layout can be changed live** (`Player::set_layout()`, `Engine::set_layout()`): the output reopens at the new width and resumes the same item. The Speakers page's layout picker, "As text" field, Heights control and per-speaker Size toggle use it, and its setup (trim, delay, crossover, routing, layout) survives a restart as one setup, not one per output device. `Routing`, `TrimDelay`, `IdentifyTone` and `LayoutRenderer::set_crossover_hz()` (40 to 250 Hz) are header-only and allocation-free, so boards can use them.
- **Hearth for Android is a Sendspin sink app** (`apps/hearth/android`): a foreground service runs one `iclforge::sink`, advertises `_sendspin._tcp` through `NsdManager`, and shows its name, address, pairing code and accepted formats, with first-run guidance. The coded AC-3 or E-AC-3 stream goes to the receiver as it is (`iclforge::sink::PassthroughOutput`); the lyric footer names the stream and route ("Dolby Digital Plus 5.1 + objects · Decoded here"). On a real Shield and Onkyo receiver it reported `Dolby Audio - DD` 5.1 for AC-3 and `Dolby Audio - DD+` 5.1 for E-AC-3, with no underrun in eleven runs; two or three bursts dropped late at about 9.2 s in three of ten AC-3 runs (cause not found). On a Shield, Dolby JOC streams locked an Onkyo as Atmos.
- **Hearth for Android draws the lyrics.** `Sink::lyric_frame()` follows the lyric document on the sink's own clock and hands the screen one value (`lyric_frame_json()` for non-C++ layers): past, current (with word fill) and next lines, duet singers, count-in, plain text, and a title, artist and album header from `metadata@v1` when an item has no lyrics. Right-to-left, Arabic shaping and Japanese are covered; the D-pad nudges the delay by 50 ms and the controls sheet sets delay and text size S, M, L. The idle screen shows the pairing code large (a QR when the server asks) and a "Passing through" or "Decoded here" chip, and dims after two quiet minutes for OLED burn-in. Design tokens come from `docs/hearth/design/sink-screen-tokens.json`, held to the desktop and P4X palettes by a static check.
- **Hearth's whole window is translated** into German, Spanish, French, Arabic, Hebrew and Yiddish. The translations are machine-made and unread by a speaker, and Settings says so. A test fails on anything unfinished, empty or dead ([Languages](docs/hearth/index.md#languages)).
- **Packaging, About and Windows Firewall.** `hearth` is packaged as `iclforge-hearth` for Windows (NSIS, ZIP), macOS (DMG) and Linux (DEB, RPM, TGZ), and takes over as the `.ac3`/`.ec3` handler from `forge-gui`. An About dialog and a Licences view show the generated third-party `NOTICES.txt`. On Windows, `hearth` and `hearth-testsink` add an inbound firewall rule (private/domain profiles, never public) before their first socket binds, elevating once through a UAC prompt.

**ESP32 sinks and firmware (hearth_sink, the boards, the P4X panel, updates over the network)**

- **The `p4-rev3` sink uses the kit's RJ45 port as well as Wi-Fi (PoE).** Everything that answers on Wi-Fi answers wired, including updates. Wired wins while it holds an address, falling back to Wi-Fi within half a second. `GET` and `PUT /network-preference` take `auto`, `ethernet` or `wifi`. A PoE splitter itself is not yet tried. See [An ESP32-P4 sink](docs/hearth/sink-esp32-p4.md#wired-and-wi-fi).
- **The ESP32-P4 sink draws lyrics on a panel (unverified on a board).** `hearth_sink` takes an LVGL 9 screen showing lines, word fill, duets and an idle screen with the board's name, address and pairing code; touch sets lyric delay and text size. It targets the 7-inch panel of the ESP32-P4X-Function-EV-Board, or a PSRAM frame buffer that `GET :8081/panel.bmp` reads back. [An ESP32-P4 sink](docs/hearth/sink-esp32-p4.md)
- **The P4X panel shows the room while a song has no lyrics, and settles when nothing plays.** A plan of the layout's speakers, each glowing with its own level, plus a title card (from `metadata@v1`) and a clock view; set with `PUT /now-playing` (`speakers`, `title`, `clock`, `off`), `/dim-after` or the Adjust sheet. After five minutes untouched the backlight drops to 4% and a tap wakes it. *When nothing is playing* and `PUT /idle-view` choose stay lit, dim (default), clock or dark. The footer names a coded stream ("Dolby TrueHD 7.1 · 5200 kb/s", "Dolby AC-4 5.1"), not "Coded audio".
- **A developer view and a System preferences group on the P4X.** Three taps on the board's name open network, configuration, stream and performance information (never the Wi-Fi passphrase or pairing token). Preferences gain the wired-or-Wi-Fi choice, name, About, paired servers, Restart, Reset preferences and Factory reset. See [the developer view](docs/hearth/sink-esp32-p4.md#the-developer-view) and [on the screen](docs/hearth/sink-esp32-p4.md#on-the-screen).
- **Every ESP board with a network keeps time with a time server.** On by default: the named server, else DHCP option 42, else `pool.ntp.org`. `GET` and `PUT /ntp-server`, `/timezone` (a POSIX string or one of 420 IANA names such as `Australia/Melbourne`; `GET /time-zones`) and `/time-sync`; `GET /status` has a `time` object. A zone is shown as a place, never as its POSIX string.
- **Display and night preferences.** `PUT /brightness` (10 to 100 percent), `/clock-format` (`24`, `12`, `24 seconds`, `12 seconds`), `/night-schedule` (`22:00-07:00 on`) and `/night-idle`, all on the Preferences screen. **Late night** (`PUT /dynamic-range`, `standard` or `late-night`) applies the stream's own compression at the level dialogue was heard at, not RF mode's 11 dB higher.
- **`PUT /name` takes effect without a restart,** and a name must be 1 to 32 bytes of valid UTF-8 (`409` otherwise). `POST /reset-preferences` restores a person's choices; `POST /factory-reset` with the body `erase everything` erases pairings and settings. See [Name, restart and reset](docs/hearth/sink-esp32-s3.md#name-restart-and-reset).
- **A sink knows what it is, joins a network it was told about, and is found by name.** Settings live in NVS, set by **Improv Wi-Fi** over USB serial or by `PUT /name`, `/wiring`, `/network` and `/slot-width`. The board advertises **`_sendspin._tcp`** over mDNS (port 8928, `path=/sendspin`).
- **A sink plays as a Sendspin player**: pairing by token or six-digit code, stereo PCM on `player@v1`, AC-3/E-AC-3 with objects on `_iclforge_player@v1`, and karaoke reproduction that reports the line it is on. Two ESP32-S3 boards played one JOC programme as a group for ten minutes with no underrun and play times within 549 µs ([An ESP32-S3 sink](docs/hearth/sink-esp32-s3.md)). On the C6, 5.1 does not fit memory beside the stream ring, so `BurstPlayerConfig::max_coded_channels` (2 there) refuses a wider syncframe instead of aborting. `GET /pairing` lists paired servers and `POST /pairing` with `forget` drops one. A dual-programme stream no longer plays both programmes, and a 44.1/32 kHz stream, which played at the wrong speed, is refused.
- **Sixteen channels out of an ESP32-S3, and slot width as a setting.** 7.1.4 leaves through the `i2s` sink (sixteen 16-bit or eight 32-bit slots a frame); `GET`/`PUT /slot-width` change width between plays.
- **ESP32-C3 and ESP32-C6 targets** (`firmware/baremetal/platform/esp32c3/`, `esp32c6/`) decode in the fixed-point tier. The C3 runs under `qemu-riscv32` in CI (12 of 14 fixtures PCM-identical; speed unmeasured). On the C6, 5.1 and below run in real time with Wi-Fi up and E-AC-3 7.1 does not; CI builds but cannot run it.
- **The board's web page and status** (`/`): state, layout, volume, the play's output layout and a Firmware section. `GET /status` gains `network`, `sink_slots`, `stream.layout`, `render`, `coded` and `silent`; the output layout accepts `:small` and `:height`/`:top`/`:upfiring`. The page is embedded gzipped (47 KB less per image; `curl` needs `--compressed`), which puts the 4 MB ESP32-C6 image under its OTA slot. A tap on Brightness - or + no longer hangs the screen.
- **Two P4 images.** `hearth-sink-esp32p4-rev3` (chip revision v3.1 to v3.99, 400 MHz) is built over `sdkconfig.p4`; the two images refuse each other's chips and `ota.py` picks one by the revision reported. CI builds both; the v3.x chip has not run on a board. The P4 image runs from PSRAM and reads flash in QIO mode (`firmware/hearth-sink/sdkconfig.p4`), which takes 7 to 8 ms off a 5.1 AC-4 frame; a board flashed before keeps DIO until reflashed over USB, since QIO is the second stage bootloader's. The I2S queue is 64 ms on the P4, so slow-decoding streams no longer run dry.
- **Firmware updates over the network, with rollback.** Boards have two app slots; `GET`/`PUT /firmware`, `PUT /firmware/rollback` and `POST /restart` stop playback, check the image against the board, then verify its SHA-256, `Content-Digest` and a read-back. The new image boots on trial, is accepted after 30 s healthy, and rolls back after 5 minutes or an early reset. A panic's core dump survives restart (`GET /firmware/coredump`; C6 and P4 only). `tools/hearth/ota.py` (and `idf.py ota`) pushes to one board or all and fetches a release (`push --release <tag|latest>`); `hearth` gains a **Firmware** tab. Releases carry `hearth-sink-esp32s3`, `-esp32c6` (4 MB), `-esp32c6-16mb` and `-esp32p4-rev1` images. See [Sink firmware](docs/hearth/sink-firmware.md) and the [browser installer](docs/hearth/sink-installer.md) (Chrome, Edge or Firefox, then Improv).
- **The Hearth sink decodes TrueHD (`CONFIG_ICLFORGE_TRUEHD`).** Boards list `truehd` with their own channel limit (`CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_TRUEHD*`) and play an unreadable unit as silence. The ESP32-P4 played 2.0, 5.1 and a 7.1 stream of about 5 Mbit/s over Wi-Fi with no late, dropped or invalid chunk. The ESP32-S3 played 2.0 for ten minutes without a lost chunk but is tight (listed only with `CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_TRUEHD=1`) and cannot decode 5.1 in real time (1.03 to 1.13). The ESP32-C6 cannot take it (53 KB of RAM free against a 158 KB decoder). `firmware/truehd-probe` measures the decoder on a board: all eight fixtures match FFmpeg's PCM hash, at a real-time factor for 2.0 at 48 kHz of 0.34 (C6), 0.13 (P4) and 0.56 (S3).

**Crucible**

- **`crucible --shot` runs over a scripted machine on a settings store of its own,** so it no longer touches the person's settings. `--app`, `--setting group/key=value`, `--lyrics "Name=<file>[@<time>]"` and `--size <width>x<height>` shape a capture, which exits with code 3 if it cannot show what was asked. See [the pictures' README](tools/pictures/README.md#crucible).
- **Crucible shows the lyrics of an application.** The Room page's **06 LYRICS** block imports an `.lrc`, `.srt`, `.ass`, `.kar` or `.cdg` against the selected application and follows its media session where it publishes one, or by **Start** and nudges. The macOS reader is a private framework that macOS 15.4 and later reportedly withholds, and no Mac has run it (unverified).
- **Crucible can send the lyric position over OSC.** **Lyric events out** in Settings (off until turned on) sends `/crucible/lyrics/state`, `/line`, `/word` and `/position` as OSC 1.0 over UDP (port 9000 to start with).
- **Crucible is built and tested on Windows and Linux, with its macOS code built and tested in CI.** It places each captured application in a room and streams E-AC-3 JOC over HDMI, or AC-3, PCM, Spatial Sound or stereo as the endpoint requires. The test-signed Windows driver must be built from source.
- **Per-process loopback capture and endpoint notifications on Windows** (`Capture::start_process_loopback`, Windows 10 build 20348+; `DeviceWatcher`).
- **Keyboard and screen-reader support** ([Keyboard and screen readers](docs/crucible/accessibility.md)), with **Settings → Appearance → Text size** (100-175% or System); needs Qt 6.8+, and no screen reader has been run against it by hand yet.
- **Smaller additions.** Per-application icons on Linux, a first-run dialog that offers to move the default output (restored on quit), Settings → Save diagnostics… (never containing the signing key), and a Licences view in About.
- **A pinned TrueHD 5.1 mode (`OutputMode::kTrueHd51`).** It encodes the 5.1 bed to TrueHD in MAT bursts of 24 access units. It is chosen only by pinning (`crucible-run --pin truehd`, the Signal path page's Pin list, the tray), carries no objects, and adds a TRUEHD column to the endpoint table; where no endpoint opens the eight-channel link (ALSA, PipeWire and WASAPI do, Core Audio does not) a pin falls back with the reason. `crucible-probe` lists each endpoint with `truehd=` and streams a TrueHD file as MAT to one. Not measured against any receiver.

**Forge CLI and GUI**

- **The Forge GUI, Hearth and Crucible open where they were left,** restoring window size, position and maximised state; a position on no connected screen is ignored.
- **A Motion setting (System, Reduced or Full) in all three programs.** System follows the operating system's request to cut back animation; Reduced stops animation that is not the signal itself. The macOS and Linux reads have not been run on those systems. See [Motion](docs/crucible/accessibility.md#motion).
- **The Forge GUI has a Karaoke flow.** A fourth Controls entry builds a karaoke AC-3 from a stereo backing, an optional melody guide, one or two vocals and an optional LFE part: Parts, Levels, Hear and Save. Hear plays the stream as a karaoke player decodes it.
- **The Forge GUI opens and saves projects, and `forge project` runs them.** A `.forgeproj` JSON file holds sources, channel plan, assignments, object motion, encode settings (AC-4 included) and the Karaoke flow's choices. Use **Project... > Open project... / Save project...**, drop a file on the window, or `forge-gui file.forgeproj`; `forge project file.forgeproj [out]` runs it and `print` shows the commands. Adds nlohmann/json as a vcpkg dependency. [Projects](docs/forge/gui/projects.md)
- **Fixed: the window's LFE mix and preferred downmix** were written to the command line (and a project) when E-AC-3's mixing metadata or AC-3's Annex D was off, giving a different stream than the window encoded.
- **`forge identify`** walks pink noise across an output's speakers one rendered channel at a time (30-80 Hz for LFE), printing which channel and output each burst went to; it takes a layout and a routing patch. `forge outputs` now prints each device's speaker mask and rates.
- **The Forge GUI encodes and reads AC-4.** AC-4 is the codec picker's third choice; its tab sets the frame and codec modes, dialnorm (or measures it), loudness, DRC profile, stereo downmix and dialogue enhancement for one source from mono to 5.1, and echoes the `forge ac4-encode` line that writes the same bytes. QC, Open stream and Inspect objects recognise AC-4. Substreams, presentations, dialogue stems, per-mode DRC, the LFE mix and layouts past 5.1 stay with `forge ac4-encode`; a live GUI session under AC-4 is refused, while `forge live` takes `codec=ac4`.
- **The Forge GUI's encoder page authors AC-4 objects.** The Objects tab writes A-JOC or direct-coded objects (up to 64, at most one LFE), echoes one `forge atmos-encode ... codec=ac4` command, and writes the scene as `<name>-paths.json`; "Export objects..." in the stream player now exports AC-4 objects. A live session and Guided's Movement step stay with E-AC-3, and the page reads no ADM BWF or IAB.

**Library, bindings and browser demos**

- **`iclforge::sink` (`libs/sink`) is a Sendspin sink a program runs with its own audio output,** moved out of the test sink so the Android app needs no third copy; `DecodingBurstOutput` decodes AC-3, E-AC-3 with objects and AC-4 into a `PcmSink`.
- **`Capture` stamps each packet with its capture time (Windows only so far)** via `Capture::stamps()` and `CaptureTimeline`.
- **The Android passthrough backend names no Java package,** so a second Android app can link it (`iclforge::audio::android::set_java_vm()`, `register_passthrough_bridge()`).
- **Audio output library.** `iclforge::audio::RenderDeviceInfo` carries a speaker mask and rate list. `MonitorSink` and `PassthroughSink` report frames played from the device's clock and can flush and pause (a receiver may need a moment to re-lock after a resume). `PcmOutput` places channels by a routing patch, `RenderDeviceWatch` keeps a device list current, and PipeWire reads which codecs a sink takes (`read_sink_capabilities`).
- **macOS passthrough fills buffers shorter than a burst.** The usual 512-frame buffer used to go out as silence. Not yet tried on a Mac.
- **The C API reproduces karaoke streams and reads lyrics.** `iclforge_decoder_create_with_karaoke()` and `iclforge_lyrics_parse()` (status `ICLFORGE_ERROR_LYRICS_UNREADABLE`). [C API](docs/library/c-api.md#karaoke-streams-annex-c)
- **The Python and Rust bindings do the same.** Python: `DecoderConfig(karaoke=...)` and `iclforge.lyrics`. Rust: `Decoder::with_karaoke(KaraokeConfig)` and `iclforge::lyrics::Lyrics`. [Python](docs/library/python-api.md#karaoke-streams-and-lyrics), [Rust](docs/library/rust-api.md#karaoke-and-lyrics)
- **The browser decode demo reproduces karaoke streams and shows the words.** `PushDecoder` and `FoldOptions` take `karaoke` and `karaokeVocals`; a **Words** panel follows a `.lrc`, `.srt`, `.ass` or `.kar` file. An AC-3 stream's fold levels now come from its own `cmixlev`/`surmixlev`, not the E-AC-3 defaults. It also has a Microphone card and pitch score (each line marked with dots and a percentage, or **not sung**) that is off until pressed, scores only when the person says they wear headphones, and sends nothing; recording a take is not in the browser.
- **The C API gains `iclforge_ac4_*` decoder and encoder functions** behind `ICLFORGE_HAS_AC4`, with status ranges `ICLFORGE_ERROR_AC4_DECODE_*` (60-64) and `ICLFORGE_ERROR_AC4_ENCODE_*` (80-81): the object encoder (`iclforge_ac4_encoder_encode_objects()`), `iclforge_ac4_encoder_refusal_reason()` and decoded objects' update ramps (`iclforge_ac4_decoded_frame_object_update()`). The object substream is experimental: `experimental.objects` must be set beside the objects.
- **Rust, Python and WebAssembly gain AC-4.** Rust wraps it as `iclforge::ac4` (each `ICLFORGE_ERROR_AC4_*` status has its own `Error` variant; `EncoderConfig` is now `Clone`, no longer `Copy`). Python gains an `ac4` submodule with typed errors (`Ac4Error`, a `ValueError`, with `Ac4DecodeError` and `Ac4EncodeError`). WebAssembly gains a third Embind module, `iclforge_wasm_ac4`, and a typed wrapper exported from the npm package as `./ac4`; no demo page uses it yet. Android's CMake wrapper no longer forces `ICLFORGE_BUILD_AC4` off, though the Shield app does not link AC-4 yet.
- **Python and Rust bindings are fuller.** Python gains `iclforge.containers` (Matroska/MP4/MPEG-TS mux/demux), `iclforge.meta` (BS.1770 loudness, QC) and `iclforge.signing` (EMDF object signing); `Eac3Decoder` is a context manager; wheels build for manylinux aarch64 and Intel macOS. Rust covers the wide-layout encoder/decoder, the Atmos/JOC object encoder with OAMD/JOC decode accessors, stream framing/scan helpers and the BS.1770 meter; `build-rust` runs on all three desktop OSes.
- **The browser encode demo covers wide E-AC-3 layouts (7.1/5.1.4/7.1.4), content-measured `dialnorm` and live microphone capture** (`getUserMedia` to `AudioWorklet`), plus an Atmos object-authoring page (`apps/demos/wasm/atmos/`) that pans audio objects on a room canvas.
- **`iclforge.truehd`, the TrueHD and MLP decoder in the Python bindings.** `probe`, `access_units`, `scan`, `decode` (a bare stream to an `int32` array), a `Decoder` taking one access unit at a time, and `TruehdError`. The encoder, the Atmos layer and metadata edits are not bound.

**Signing and security (content signing, Atmos signing, protected-stream handling)**

- **AC-4 content signing: one ECDSA P-256 signature over a whole stream, made with the operator's own key and certificate chain.** The `pace-ccjf1` scheme of the owner-supplied CS spec v2.3 is appended after the last sync frame of a raw `.ac4` file, so a signed file decodes as the unsigned one. It is opt-in and bring-your-own-key; no key, certificate or fingerprint of any issuer ships. A valid verdict needs a chain leading to a named trust anchor or pin, else `chain unanchored`. Commands: `forge ac4-sign`, `ac4-verify`; `sign-content` on `ac4-encode`, `atmos-encode`, `atmos-adm` and `atmos-iab`; `verify-content` and `check-content` on `decode`; options `content-key=`, `content-chain=`, `trust-anchor=`, `pin=`, `verify-time=`, `unanchored-ok`. `trust-key=<public key file>` (PEM or DER) anchors a chain whose signing certificate carries that P-256 key; every other check still runs, so a trusted key never rescues a changed stream. The report ends in a one-word `result:` line (`valid`, `untrusted`, `no-signature`, `unreadable`, `chain-rejected`, `signature-invalid`, `content-altered`, `metadata-altered`, `refused-by-policy`); `iclforge_ac4_sign_stream` and `iclforge_ac4_verify_stream` do the same in memory from the C API. The signature chains to your certificates, not Dolby's, and a licensed Dolby decoder will not take it as Dolby-authentic. Not implemented: MP4 carriage, revocation, digests other than SHA-256. See [Signing](docs/library/signing.md#ac-4-content-signing) and [Object signing](docs/concepts/object-signing.md).
- **Atmos object signing takes a keyring, and `gate-objects` is the AVR-like policy.** `verify-objects` accepts repeated `signing-key=` (valid if any key reproduces the tag); `gate-objects` on `decode`, `monitor` and `spatial` plays objects for frames that verify and the bit-identical 5.1 bed for the rest, and fails closed on an object layer it cannot verify or remove. Library: `iclforge::ac3::signing::verify_atmos_stream_any`, `gate_atmos_stream`. On an Onkyo a signed stream locked as `Dolby Atmos/DD+`; unsigned, it fell back to `Dolby Audio - DD+`.

**Build, CI, verification, packaging and documentation**

- **An Android preset builds the native libraries Hearth for Android links.** `config-android-arm64` and `build-android-arm64` build `libs/lyrics` and `libs/sendspin` for `arm64-v8a` under NDK r26 (tests pass on a Shield); a `build-android-native` CI job runs it.
- **Hearth is built and tested on every build-and-test leg** (the vcpkg `hearth` feature), so Windows and macOS compile `libs/sendspin` and `apps/hearth` in CI for the first time. The first macOS build found and fixed a `poll()` count narrowing in Sendspin's mDNS discovery.
- **New gates and checks.** A `Memory gate` (`_compare.yml`) compares `iclforge-membench` at the PR head and merge base in the merge queue and fails a doubling of heap churn unless `memory-regression-approved` is set. CI asserts that Linux and macOS packages carry the `forge` man page and completions (`check_cli_docs_package.py`) and fails an sdkconfig that burns eFuses or a misnamed firmware package. `tools/checks/check_esp_component_targets.py` fails when the ESP-IDF component links an `iclforge::` target its own options never define, which the pull-request gate (no ESP-IDF image) could not see.
- **Coverage and fuzzing.** Windows line and branch coverage over `apps/crucible` (`tools/checks/coverage_crucible.ps1`); fuzz harnesses `fuzz_iab_parse` and `fuzz_ac4_parse` for the two parsers that had none; `fuzz_iamf_parse` has 17 seeds and runs in `fuzz-regress`, `fuzz-short` and `fuzz-nightly` (`tools/fuzz/generate-seeds.sh`).
- **Bit-exactness across architectures.** The bitstream-hash gate pins `aarch64-neon`, byte-identical to `x86_64-sse2`. The ~6.02 dB cross-platform split in the gold-reference comparison is a last-bit decode-side difference, not a codec error; floor headroom drops from 6.02 dB to 1.0 dB, so the gates catch a 1 dB regression. `tools/checks/verify_karaoke_oracle.py` compares `forge decode karaoke` with the Dolby Reference Player's decoder; local only (the player is licensed), and it exits 0 without it.
- **AC-4 joins performance and quality reporting.** AC-4 encode and decode are workloads of `iclforge-perf`, `iclforge-bench` and `iclforge-membench`; decode quality goes to `ac4-quality-main.jsonl` and encoder programme legs to `ac4-encode-quality-main.jsonl`. The decode profile can carry the AC-4 decoder (`ICLFORGE_MINIMAL_AC4`; `tools/checks/run_baremetal_probe.sh --ac4`), and the `float` build compiles clean with `-Werror` on GCC 16 and Clang 22.
- **Golden masters from Dolby's encoders** for AC-3, E-AC-3, E-AC-3 JOC and TrueHD. DEE's licence ended on 2026-11-06 and is not renewed, so `tools/generators/gen_dee_gold.py` made, and keeps on a local disk, 1,111 legs. `forge`'s decoder refuses the 23 streams that use transient pre-noise processing.

### Changed

**TrueHD, MLP and Dolby Atmos in TrueHD**

- **The Atmos object substream uses the object-audio layout of TH spec v3.20 §10.2.1a.** The decoder reads matrix coefficients to `frac_bits` 15 and writes any of the 16 rows; the owner's disc and DEE encodes read to the bit. Provisional against outside decoders.
- **`iclforge::truehd::Decoder::reset()` allocates nothing and is `noexcept`.** It used to need the decoder's 158 KB twice and ran out of memory on an ESP32-C6.

**AC-4**

- **The AC-4 decoder claims `md_compat` level 7 by default, not 3.** Streams of more than eleven tracks (9.0.4, 9.1.4, 22.2) no longer need `md-compat=7` and Hearth no longer refuses them on open. `DecoderConfig::level` (and the C API's, Python's, Rust's and JavaScript's `level`, and `forge decode md-compat=`) set it lower.
- **The decoder can leave object audio undecoded** (`DecoderConfig::skip_objects`) for players that place no objects. Channels are bit-identical to full decoding and a frame takes about half the time (54 to 29 ms with four objects, Debug host build); a presentation of objects alone is refused with a reason.
- **The inverse transform has built-in tables for every frame length**, so 24, 29.97 and 30 fps and high-frame-rate streams no longer stall their first frame on parts without double precision (0.49 s to 47 ms on an ESP32-S3).

**AC-3 and E-AC-3**

- **E-AC-3 decodes in real time on the ESP32-S3.** At 240 MHz a 5.1 frame went from 78.8 ms to 6.2 and an Atmos objects frame from 82.7 to 21.2, every level unchanged. See [the ESP32-S3 page](docs/platforms/bare-metal/esp32-s3.md#timing).
- **The fixed-point tier decodes 5.1 in real time on the ESP32-C6 with Wi-Fi up**, with bit-identical PCM (AC-3 5.1 from 44.3 to 26.2 ms a frame); see `docs/platforms/bare-metal/esp32-c6.md`.
- **The encoders are faster on small parts.** On the ESP32-S3 AC-3 2/0 encode went from 75.0 to 12.1 ms and E-AC-3 5.1 from 348.9 to 55.5 ms (real time); E-AC-3 golden hashes were re-pinned for a changed rate-control search.
- **The gold-reference quality gate has one SNR floor per channel**, not per fixture, gaining 19-71 dB of real gate on front channels and LFE. See [Validation](docs/verification.md).

**Immersive audio and object formats (Atmos objects and JOC, IAB, IAMF, ADM/BW64)**

- **An object scene file is read with nlohmann/json** (3.12.0 or newer, now a dependency of `iclforge::objects`). Errors still come back as a `SceneError`; syntax follows RFC 8259 more strictly (a repeated key and non-UTF-8 bytes are refused).

**Hearth (desktop and Android players, Sendspin network output)**

- **A sink that lists Opus can be sent Opus in place of PCM** (the Decoder tab's "Audio decoded here" choice), per sink and kept with its pairing record.
- **An AC-4 item declares its objects to a group** (`ItemFacts::objects`), so a sink's `max_objects` is honoured: a sink that places fewer gets PCM decoded in Hearth, or a stated refusal where PCM is impossible.
- **Hearth for Android is `com.iainchesworthlabs.hearth`** (Shield demo `com.iainchesworthlabs.shield`; the pre-release `com.ac3forge.shield` stays installed beside it). Hearth's Settings page has an **About** card.

**ESP32 sinks and firmware (hearth_sink, the boards, the P4X panel, updates over the network)**

- **A board can decode AC-4 from a Sendspin group** (`CONFIG_ICLFORGE_AC4`). An ESP32-P4 lists `ac4` to 5.1 (0.51-0.73 of real time, levels within 0.01 dB of the host decoder). An S3 with `ESP32S3_DATA_CACHE_64KB` lists `ac4` at 2 channels up to 25 frames a second; faster streams are sent PCM. Release images have the decoder off; a C6 is sent PCM.
- **Late AC-4 chunks are decoded and discarded rather than dropped when that loses less audio**: audio lost at a play's start on an S3 fell from 0.90-0.98 s to 0.00-0.09 s at 23.44 fps.
- **Boards skip object reconstruction they would throw away**, playing the bed (`BurstPlayerConfig::max_objects`); on a P4 a 5.1 bed with 16 A-JOC objects went from 2.51 to 0.59 of real time.
- **The Sendspin player decodes Opus** (`CONFIG_ICLFORGE_SENDSPIN_OPUS`). On an ESP32-C6 30 s of tone played with no underrun where the same board's PCM dropped out 14 and 17 times in 15 s; Opus is lossy and a twelfth of PCM's bytes.
- **A sink's web page is redesigned** in Hearth's look, offering only settings the board reports (`GET /wiring` answers `404` where there is no wiring). Its built-in WiFi network is empty by default instead of `my-network`, so a fresh board no longer wastes up to 30 s before Improv listens.
- **The streaming-player example is now `hearth_sink`** (`firmware/hearth-sink/`); scripts using the old path or `ac3forge_stream_player.bin` need updating.
- **The P4X footer says a coded stream is decoded on the board**, with bitrate and server (`PCM from <server>` for PCM), under the speakers, the title card and the Meters view; a TrueHD 7.1 stream is named 7.1, not 5.1. `PUT /layout` survives a restart, so Music Assistant boards can show 5.1 or 7.1.4.

**Crucible**

- **The Desktop Atmos Demo is now Crucible**, a desktop application and not Windows-only; settings migrate on first launch.
- **Crucible runs on Linux on PipeWire**, verified on a Raspberry Pi 4B against an Atmos receiver over HDMI (live E-AC-3 with a signed JOC layer). `.tar.gz` and `.deb` ship for x86_64 and aarch64. Passthrough offers AC-3/E-AC-3 only when the sink's `iec958.codecs` (its EDID) lists them.
- **`process_loopback` is reported unavailable on macOS,** where the Core Audio process tap froze the process. The Windows silent device is renamed "Speakers (Crucible Silent Output)" (driver `IclForgeNullSink`, now an ACX driver on KMDF).
- **The TrueHD mode holds the first 150 ms back before it starts the device,** so the stream no longer has 70 to 90 ms gaps that a receiver reads as a lost lock (`OutputStageConfig::truehd_prefill_ms`; about 190 ms more start-up delay).

**Forge CLI and GUI**

- **Settings and About match across Forge, Crucible and Hearth.** Forge's Preferences is now Settings, and **Licences…** opens the installed `NOTICES.txt`.
- **A filled accent control's label reaches 4.5:1 contrast in every palette and mode** (`Theme.accentFill`, `Theme.accentText`); buttons and chosen segments are a deeper red in Signal light. The app icon is now Signal red in place of the demos' blue.

**Library, bindings and browser demos**

- **Programs, libraries, packages and addresses have new names.** The programs are `forge`, `forge-gui`, `hearth`, `crucible` (were `ac3cli`, `ac3gui`, `ac3hearth`, `ac3crucible`), with no alias. The libraries are `iclforge::<library>` (headers `iclforge/<library>/`); `iclforge::ac3` holds AC-3, E-AC-3 and Atmos, `iclforge::ac4` AC-4. The C API is `iclforge_c/iclforge.h` (prefix `iclforge_`), options are `ICLFORGE_*`, the Python module is `iclforge`, the Rust crates `iclforge` and `iclforge-sys`, the npm package `iclforge-wasm-decoder`. The Sendspin extension role and the sink firmware's project name carry the new name too, so every already-flashed ESP32 board needs one flash over USB before updates over the network work again. `forge-gui`, Hearth and Crucible copy the settings stored under the old names on their first start. See [Renamed](docs/renamed.md).
- **Decimal text becomes a `double` through `iclforge::base::parse_double` (fast_float), not `strtod`.** It is exact and locale-independent (a German `LC_NUMERIC` no longer reads `"1.5"` as 1); hex floats such as `"0x1p3"` are no longer numbers.
- **Source-breaking header moves before the v1.0 freeze, ABI unchanged.** `coupling.hpp` and `eac3_tools.hpp` moved from `ac3/encoder/` to `ac3/core/` (under `iclforge/`), and the ESP32 player's layout and renderer became `iclforge::render`.

**Signing and security (content signing, Atmos signing, protected-stream handling)**

- **A chain with a certificate that expired before the signature's date is rejected by default** and accepted only with `verify-time=ignore`, an instant before the expiry, or an anchor or pin below it; `trust-key=` does not rescue it. Documentation only.

**Build, CI, verification, packaging and documentation**

- **One vcpkg route.** `cmake/VcpkgRoot.cmake` picks the checkout (`CMAKE_TOOLCHAIN_FILE`, `ICLFORGE_VCPKG_ROOT`, a shared store `ICLFORGE_VCPKG_STORE`/`ICL_VCPKG_STORE`, then `VCPKG_ROOT`); the presets no longer set the toolchain file. See [Building](docs/building.md#which-vcpkg-a-configure-uses).
- **Platform floors.** Qt 6.12.0 (long-term support) builds the GUI apps, and macOS 14.4 is the minimum (was 13.3). Android builds use NDK r28c, up from r26b whose libc++ miscompiled `std::expected` payloads; an Android Clang older than 19 is a configure error (`-DICLFORGE_ALLOW_OLD_NDK=ON` overrides).
- **Pull requests are gated on a Linux build and `main` is verified after merge** (`pr-gate.yml`, `ci.yml`, `main-health.yml`; see [CI](docs/ci-agentic.md)). The merge queue adds Windows MSVC and, for source changes, performance and memory comparisons that fail a workload taking twice as long unless `perf-regression-approved` or `memory-regression-approved` is set. A failing `main` opens one `main-red` issue; nothing is reverted automatically. A nightly run covers sanitizers, the ABI gate, CodeQL and SonarCloud.
- **Packaging.** The vcpkg port and Conan recipe install the AC-4 libraries, IAB reader and IAMF writer only on request (`vcpkg install iclforge[ac4,iab,iamf]`).
- **Changelog entries are one file each, in `changelog.d/`.** A pull request adds `changelog.d/<slug>.<type>.md` instead of editing `CHANGELOG.md`; `PR Gate` fails one that edits the file or misnames a fragment, and the release assembles them with `tools/release/changelog_tool.py assemble` ([Releasing](docs/releasing.md), step 5).
- **New guides:** [Using the desktop player](docs/hearth/desktop-player.md), [Hearth for Android](docs/hearth/android.md), [ESP32 build profiles](docs/platforms/bare-metal/build-profiles.md), [ESP32 memory layout](docs/platforms/bare-metal/memory-layout.md), [Testing and benchmarking the ESP32 boards](docs/platforms/bare-metal/board-bench.md), [The room](docs/crucible/room.md), [Settings](docs/crucible/settings.md).

### Fixed

**TrueHD, MLP and Dolby Atmos in TrueHD**

- **The TrueHD encoder sends each channel's own output shift and quantiser step.** One shared step spent bits on the low 8 bits of 16-bit channels in a 24-bit programme; the DEE fixtures' 5.1 music is 56,672 bytes, down from 101,314 (DEE: 58,688).
- **Restart sync is 0x31EB in substream 1 of every multi-substream stream**, as in retail streams (3 to 6 channels wrote 0x31EA). At 176.4 and 192 kHz, 3 to 6 channels are now two substreams; FFmpeg decodes 192 kHz 5.1 exactly (provisional).
- **TrueHD can be measured against FFmpeg and DEE** with `tools/checks/compare_truehd_encoders.py` and `compare_truehd_decoders.py`; see [TrueHD against FFmpeg and DEE](docs/truehd-comparison.md).
- **The Dolby decoder and Reference Player now accept more of our streams.** `forge truehd-atmos` writes the Atmos flag `0x1000` and the fixed-configuration evolution frame (unsigned); the restart header writes the true `max_bits`; a three-substream stream writes `substream_info` 0x7C; and a large access unit is stamped early enough to arrive in time.
- **`forge qc`, `forge probe` and the Forge window's QC no longer fail Dolby's own streams on the data rate.** Each unit is now held to its own timing words. The probe JSON `rate_model` gains `worst_overrun_ms` and loses `worst_latency_ms`.
- **Streams that clear bit 0 of `param_presence_flags` no longer misalign** from the next block on (TH spec §8.1).
- **The Atmos object substream's lossless check and matrices are corrected** (channel-number shift, matrix ramps); held on 42 DEE controlled encodes, provisional against outside decoders.
- **The Forge window plays a TrueHD track in Matroska, MP4 and Blu-ray transport streams** instead of refusing it as "not a valid AC-3 stream".
- **`forge monitor` plays a 7.1 stream's side and rear surrounds on the right speakers;** the pairs were swapped.
- **TrueHD reading fixes.** A bare TrueHD stream from a Blu-ray transport stream reads all its units, and `forge demux` to `.thd` or `.mlp` takes the TrueHD track.

**AC-4**

- **The decoder applies an alternative presentation's target loudness correction and the real-time loudness correction** (Part 2 clauses 4.8.5.4 and 4.8.5.5); the latter is `LoudnessInfo::real_time_correction_db`.
- **The decoder dropped the stream's downmix gains when the listener changed the downmix or LFE choice**, so Hearth's Lo/Ro, Lt/Rt and LFE controls were wrong for up to a second. They now persist.
- **The encoder left the band above A-SPX's crossover empty where the low band held nothing to copy** (sweeps, steady high tones; 32 to 61 dB under the source). It now lands within 6 dB, and ViSQOL beats DEE's on every sweep leg at 2.0, 5.1 and 5.1.4.
- **The parser misread everything after a `presentation_config` of 1, 4 (dialogue enhancement) or 6 (EMDF-only)**, in `iclforge::ac4` and `forge probe`.
- **The AC-4 and IAB parsers read out of bounds, looped forever and could be made to allocate gigabytes on malformed input**, unseen because their fuzz harnesses were not instrumented. With ASan, UBSan and coverage on, `fuzz_ac4_parse` and `fuzz_iab_parse` found them (one frame asked for 1.8 GB, now 33 MB); both now run clean for 300 seconds.
- **Smaller fixes in AC-4.** The encoder refuses an object whose screen factor of 0 it would have written as 1/8, a float decode honours `DecoderConfig::fast_imdct = false`, and the experimental ASPX_ACPL_1 in immersive layouts no longer switches difference groups apart from their sum groups.

**AC-3 and E-AC-3**

- **A WAV's speakers were never read, so 2/1 and 3/1 files were encoded as the wrong mode.** `WavData::channel_mask` and `WavStreamReader::channel_mask()` now carry a `WAVE_FORMAT_EXTENSIBLE` `dwChannelMask`, and `encode`, `eac3-encode`, `programmeN=` and the Forge GUI follow it: FL FR BC is 2/1, FL FR FC BC is 3/1, FL FR BL BR or FL FR SL SR is 2/2, each with or without LFE. Narrower folds use that mode's section 7.8 coefficients and `transcode` keeps a single-mode stream in its mode (helpers in `iclforge::ac3::plan`). A WAV with no stated speakers is read by width as before.
- **`forge decode` states its speakers.** AC-3 or E-AC-3 decodes of three or more channels are written as `WAVE_FORMAT_EXTENSIBLE` with a channel mask; mono, stereo and unnamed layouts keep the 44-byte header.
- **E-AC-3 `strmtyp` 2 ("previously coded in AC-3", section E2.3.1.1) is a whole stream type.** The decoder, EMDF walker, scanner and metadata editor would have misread a real type 2 stream; they now treat it as independent. `FrameConfig::ac3_frmsizecod` writes one and `forge probe` reports the code. The conversion to and from AC-3 is not built.
- **Legacy-core streams (section E2.3.1.2) decoded wrongly, and held-back last units were lost.** `monitor`, `transcode`, `metadata`, `cut`, `cat` and `spatial` read an AC-3 core with Annex E dependents as plain AC-3; they now treat it as E-AC-3. The core's output stage no longer runs twice (bed 14 dB down at dialnorm 24) and folds with its own downmix levels. `monitor` and `spatial` never played the final access unit held back by the transient pre-noise tool, and `decode` and `transcode` could misplace it; all now share `iclforge::apps::held_back_unit`.
- **Multi-programme E-AC-3 in the CLI and GUI.** A stream carrying two programmes crashed `forge monitor`, and `spatial`, `play` and `forge spdif` spliced or doubled them so a receiver heard the main at half speed. They now play one programme: `programme=<0..7>`, or the first the stream carries; a missing programme is refused by name. `forge transcode` takes `programme=` too, and the GUI stream player plays the first programme and says so instead of hanging busy.
- **E-AC-3 streams using transient pre-noise processing now decode.** The decoder refused (`DecodeError::kUnsupported`) any correction reaching past the frame that signalled it; corrections now wait for their frame and no longer land one block early (self-scored stereo at 192 kbit/s rises from 24.0 to 26.2 dB).
- **RF mode decoded 11 dB below a Dolby decoder, and heavy compression did nothing for dependent substreams.** `OperatingMode::kRf` now applies each `compr` word with 11 dB on top, like the Dolby Reference Player (DEE's streams: -19.90 LUFS, was -30.90); see `docs/library/decoding.md`. `AccessUnitEncoder` measures the last dependent's word from the whole program and writes words for an RF-mode decode that adds the 11 dB itself, as DEE does, so they no longer play 11 dB hot.
- **Smaller fixes in AC-3 and E-AC-3.** `forge probe` had bsmod 7's two names backwards, and `probe json=1` wrote invalid JSON for AC-4 bitstream version 0 or 1. The GUI offered E-AC-3 bitrates a 16 kHz source could not frame and wrote silence into a short final frame where `forge encode` holds the last sample. `forge play`, `monitor`, `identify` and `live` spun for ever when an output device went away; they now stop and say which endpoint went (exit `5` for `play`, `monitor`, `identify`), and `live` drops the leg and carries on. `spatial`, `qc objects=` and `decode ... adm_out=` played Atmos objects against an LFE 576 samples too early. The encoders chose different delta bit allocation segments on Windows and Linux; syncframes of one to three blocks with transient pre-noise no longer write past the decoder's buffers; refused decodes are no longer all described as "valid AC-3 this decoder does not implement (bsid > 8)" (`DecodeError::kNoReferenceTransform` is new); dual mono's Ch2 is levelled by `dialnorm2`.

**Immersive audio and object formats (Atmos objects and JOC, IAB, IAMF, ADM/BW64)**

- **ADM BWF masters now carry `bitDepth`.** `iclforge::adm::write_bw64()` wrote each `audioTrackUID` without it, and Dolby Encoding Engine 6.5.4 refused the master ("Mismatched track bit depth between ADM and WAV"). Each now carries `bitDepth` equal to the `<fmt >` chunk's bits per sample; `parse_bw64()` reads it with or without the attribute.
- **`write_bw64` dropped an `audioObject`'s duration**, so an object that ran for part of the programme was written as running for all of it.
- **The renderer played a JOC programme's LFE ahead of its objects.** Objects arrive `oba::joc::reconstruction_delay()` samples (576, or 12 ms, in the QMF domain) after their bed, so `LayoutRenderer::render()` now delays the LFE by that amount while objects are placed (`set_joc_domain()`).
- **The BW64/ADM reader had a heap overread, 1.7 GB allocations and a hang** (found by an instrumented fuzz harness); all are fixed. The project moved to the maintained `libbw64` fork `github.com/pwnified/libbw64`, which reads float WAV natively, so 64-bit float `<fmt >` now works. `write_bw64()` also returns `AdmWriteError::kInvalidDocument` on a polar block instead of throwing.

**Containers, streams and carriage (MP4, Matroska, MPEG-TS, fragmented MP4 and DASH/HLS, IEC 61937 and MAT, lyrics carried in files)**

- **The `dec3` `asvc` bit misclassified karaoke as an associated service.** `iclforge::ac3::io::build_codec_config_box` read bsmod 7 as voice-over whatever the acmod; it now shares the MPEG-TS descriptor writer's rule, `iclforge::ac3::meta::is_associated_service`.
- **Shift-JIS lyrics with a lead byte and no trail byte are refused on macOS 15 as elsewhere.** `lyrics-encoding=shift-jis` and the Forge GUI's Lyrics page choice silently dropped such a pair there; each character is now checked alone before the text is converted.

**Karaoke (lyrics, scoring, microphone, karaoke streams)**

- **The Forge GUI Karaoke flow's length warning is accurate.** It said a shorter part "is padded with silence"; the window and `forge encode` hold its last sample, and the warning now says so. The status line no longer asks for assignments the flow has already made.
- **The P4 panel's lyric footer counted lines that only mark a time** ("Line 2 of 5" for a four-line song); `text_line_count()` and `text_line_number()` in `iclforge::lyrics` count only lines with text.
- **A board's lyric document cost about ten times its text of internal heap.** A board with PSRAM now holds it there (`PsramScope`): on the P4 a 16,000-byte document costs 49 KB at the peak, not 157 KB. A board takes a document only while enough memory is free and says why when it holds none (default limits: none without PSRAM, 2,048 bytes on an S3, 16,384 on a P4). A document is no longer sent three times at the start of an item.
- **The Karaoke page in Hearth fits its window.** At 1280 px the Microphone card ran past its edge; rows now shrink to their card (also `AppCheckBox` and `SectionHeader`) and it holds from 100% to 175% text at the 960 px minimum width. A muted microphone now gets the "No sound from the microphone" hint.

**Hearth (desktop and Android players, Sendspin network output)**

- **Right-to-left text and the largest text size.** In Arabic, Hebrew and Yiddish a number field's unit no longer sits over its number (`AppTextField`, so Crucible and the Forge GUI follow), minus signs no longer land after their digits, lyric-line words run in the line's own direction, and the Keyboard shortcuts, About and First run dialogs scroll above their buttons at 175% text.
- **A member rejoining a playing group could stop the engine for good.** The group and the Network page's poll took two locks in opposite orders; the host's trail now has its own mutex.
- **Smaller fixes in Hearth.** The empty-queue hint wraps instead of being clipped; the first-run dialog says the output name is "at the top of the window" (right in mirrored languages too); the transport bar clears a stale play error when the next play, next or previous starts.
- **Sendspin pairing is compliant.** A player now sends `client/pair-init` and then `client/pair-finalize`, as pairing.md requires (aiosendspin 10.2.1 logged the lone finalize as non-compliant); a board takes the change when reflashed.
- **A Sendspin session no longer reserves eight bytes of heap per byte of a message.** Sessions read messages into `json::TokenStorage`; malformed, over-nested or over-cap text is refused before any block is taken (`PlayerConfig::max_json_tokens`, 4,096 by default, 512 for the Hearth sink). Documents above `kRetainedMessageBytes` (4 KB) release their receive buffers; an 8,000-byte lyrics document used to keep 12.7 KB of an ESP32-S3's internal heap.
- **Hearth's window now passes AC-3 and E-AC-3 to a receiver.** It was designed to and never did: every item was decoded. A matching item now goes out as IEC 61937 bursts, access units unchanged (E-AC-3 transcoded for an AC-3-only receiver); AC-4 items, a stream's later programmes and karaoke streams with a reproduction chosen are decoded. Unverified: the window has not yet been run into a receiver.
- **The engine plays a 9.X.4 presentation instead of refusing it**, rendering to 7.X.4 where no layout was chosen (22.2 and the ESP32 player still refuse).
- **Sendspin lost the start of some streams.** A `player@v1` stream that ended before the clock's first exchange lost every chunk it had carried, and a board moved onto `player@v1` as an item started could miss up to 29,184 frames; `PlayerSession` now delivers what it holds, and the group waits up to a second for a member still moving.
- **Pairing a network sink never said it still needed a group, so the first Play fell back to the local output silently.** The first-run dialog, the Play page and a Network-page banner now say so. Two decoder-setting changes within one poll no longer undo each other.

**ESP32 sinks and firmware (hearth_sink, the boards, the P4X panel, updates over the network)**

- **The ESP32-C6 on a 4 MB module has room for its player again.** `firmware/hearth-sink/partitions_c6.csv` now has two 1.875 MiB application slots and no `audio` or `storage` partition; the Sendspin image had grown 38,784 bytes past the old slot. Move a board with one `write-flash @flash_args` over USB, which keeps its NVS.
- **An ESP32-P4 restart or update could leave its link to the C6 dead until a USB flash** (`sd_host_wait_for_event returned 0x107`), because `esp_restart()` resets the CPU alone. A restart now ends in the RTC watchdog's system reset (`firmware/hearth-sink/main/system_reset.cpp`): 24 of 24 updates clean. A board from before this needs one more update or a USB flash.
- **Network joining is robust.** The station keeps retrying after an access point restarts or a power cut; on S3 boards one was back 1.2 s after the access point returned. Improv reports *ready* whenever the board is off a network so the client's Wi-Fi form appears, and `wifi_settings` joins at once. A mistyped passphrase followed by the right one no longer aborts the board, and a board that joins after boot starts mDNS and Sendspin without a restart.
- **Improv answers are reliable over serial.** S3 USB console answers no longer wait for the next printed line, and CR/LF rewriting no longer corrupts packets (a 13-character SSID was answered `invalid_packet`).
- **Smaller fixes in hearth_sink.** An ESP32-P4 sink takes its name (`hearth-<last three bytes of its MAC>`) from the chip's base MAC where no station MAC exists; the page can no longer reach the Sendspin player before it has started, and `PUT /layout`, `/name`, `/wiring` and `/slot-width` sent during the start are no longer lost; the first play after a Wi-Fi reconnect no longer starts late with underruns, because `iclforge::sendspin::ClockSync` confirms convergence with a second burst; a stream playing when the network drops ends cleanly and the board rejoins; `settings_forget()` no longer keeps the Wi-Fi network until restart.
- **Smaller fixes in the ESP32 decoder and player.** A twelve-channel play on the ESP32-S3 no longer aborts for want of internal RAM; `iclforge::Player` sizes its block storage from the layout and uses PSRAM when available; the streaming example's `tdm` sink refuses a frame over the ESP32-S3's 128-bit limit; `PUT /layout` no longer overflows the control surface's stack (`Control::start` takes a stack size); the ESP-IDF component chooses `fixed` decode without an FPU (`SOC_CPU_HAS_FPU`) instead of software `float`, up to 3.1 times slower on an ESP32-C6.
- **The ESP32-P4 heap held 32 KB of low-power SRAM at 174 cycles a load, and AC-4 decoder buffers landed in it.** `sdkconfig.p4` turns `CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP` off: the 29.97 fps frame-rate converter fell from 93.8 to 12.0 ms a frame (a play at 3.51x real time is now 1.00x). See [ESP32-P4](docs/platforms/bare-metal/esp32-p4.md#the-low-power-sram).
- **On the C6, an E-AC-3 stream over Sendspin was left 0.9 to 4.5 KB of heap and failed after an AC-3 stream in the same boot.** Tables of unused tools are now on-demand or not linked, and an E-AC-3 decoder no longer holds spectra for seven streams per block. The C6 still refuses an HTTP play with `why: memory` and plays over Sendspin.

**Crucible**

- **The Room's right rail scrolls.** It was cut off by the window's lower edge (Lyrics hidden at 1480 by 820 and at the 960 by 620 minimum); it now scrolls, and the lyric strip's buttons wrap instead of widening it.
- **A re-probe requested mid-probe was dropped (now queued).** Crucible also tapped applications before it had anywhere to play them (on macOS this mutes the source); taps now wait for an endpoint and `Engine::start()` says why it failed. A restarted engine no longer carries a stale endpoint answer that made `start()` refuse.
- **On Linux Crucible listed every PulseAudio application as one entry and tapped none** (PipeWire reports `pipewire-pulse`'s pid); streams now bind by `application.process.id`. It also crashed on launch with a system tray; the tray menu is now flat.
- **The 5.1 bed carries the LFE of the applications playing into it,** so a subwoofer is no longer silent in DD+ 5.1, Atmos and TrueHD (a positioned application's LFE is still dropped). On Linux the silent output now says it is eight channels wide, so a 5.1 or 7.1 application reaches the bed by channel; a 60 Hz LFE tone that arrived at -120 dBFS arrives at full level.

**Forge CLI and GUI**

- **The GUI's players play out what they queued.** An object audition, the stream player and the motion preview were cut off before their end (closing the output discarded its 1.4 to 1.8 s queue), so short objects were never heard. Each now waits via `iclforge::apps::wait_until_played_out`; stop and pause still end it at once.
- **The GUI could crash on exit while still decoding, measuring, inspecting or encoding.** Each controller now runs workers on its own pool (`apps/forge/gui/src/background_jobs.hpp`) that its destructor stops and waits for.
- **`quiet` crashed `decode` on multi-programme streams and `transcode`, `metadata`, `normalize`, `cut` and `cat` on every stream.**
- **`bap-census=` was accepted by eight commands that cannot produce one;** it is now scoped to `decode` (others give `error: unknown option`).
- **`transcode dialnorm=auto` and `dialnorm2=auto` did not measure anything;** they now run the BS.1770 pass and print `(measured)`. `transcode` prints the encoder's reason instead of crashing, and `forge spatial` ends with a reason on an unplugged endpoint instead of hanging.

**Library, bindings and browser demos**

- **The browser demo's microphone is closed reliably.** `MicrophoneSession.start()` releases everything it had made when any step throws, and `stop()` ends an opening in progress, so loading another stream while the browser is still asking no longer leaves the microphone live behind a hidden card or shows "Cannot pass deleted object as a pointer of type ReferenceContour"; a second `start()` no longer opens a second microphone.
- **Two browser demo tests were fixed** (the "follows a lyrics file" test failed against `auld-lang-syne.ac3` from the start; the microphone test was flaky). The page and module are unchanged.
- **A lost output device left the sink saying it was still playing.** On every backend `running()` now goes false, `position()` reports nothing, `submit()` refuses, and `start()` reopens without `stop()`. Hearth names the lost output, and the GUI, Crucible and the Shield demo no longer spin forever.
- **`MonitorSink::start()` returns `MonitorError::kFormatRejected` when a device refuses the format,** not `kComFailure`. On Windows it also refused every rate but its endpoint's own; it now resamples with `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`.
- **`PassthroughSink` crashed on a real exclusive-mode bitstream endpoint;** the WASAPI lifecycle now runs on one thread. The AudioWorklet browser test, which never ran in CI, now does.

**Signing and security (content signing, Atmos signing, protected-stream handling)**

- **A signed Atmos frame could carry unauthenticated object data and still verify.** A second payload in another block's skip field, or bytes trailing the container, were outside the tag. `verify_atmos_frame` now reports such a frame, and a container declaring no primary protection field, as a mismatch; `sign_atmos_frame` no longer reports having signed one. The docs' claim that the tag does not cover the audio was wrong and is corrected.
- **`iclforge::base::crypto::decode_signing_key` silently signed with the wrong bytes** for a key file holding a comma-separated `0xHH` hex array, which a real AV receiver then refused. The format is now recognised, and ambiguous hex/array-shaped content is refused.

**Build, CI, verification, packaging and documentation**

- **Android: `std::expected` no longer loses a payload with tail padding.** NDK r26's libc++ overwrote the has-value flag (a Shield got "no value, error 0" from `read_frame_metadata()` and the stream-metadata edit calls); the workaround `cmake/LibcxxExpectedLayout.cmake` was removed once the pin moved to NDK r28c, and `iclforge-base-tests` gains `[expected]` cases. Two other r26 gaps are worked around; the audit is in `docs/platforms/android.md`.
- **{fmt} everywhere.** `take_recorder.cpp` and `atmos.cpp` use `fmt::format` with byte-identical output, and `tools/checks/check_no_std_format.py` fails `<format>`, `<print>` and any `std::format`/`std::print` in first-party C++.
- **ESP32 CI legs are stricter.** A panic or reset after `result=pass` passed every QEMU leg; each leg now fails on panic output, a second boot banner or any failed allocation, with a free-heap floor held. The minimum-footprint ESP32-S3 build's main-task stack moves from 32,768 to 40,960 bytes (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`), and the E-AC-3 fold fixtures were re-measured for the -3 dB fold that had failed the arm-none-eabi, ESP32-C3 and ESP32-S3 footprint probes.
- **Green Dependabot pull requests never merged.** The workflow now sets auto-merge with a person's token (secret `MERGE_QUEUE_TOKEN`) and fails when it is unset or expired.
- **Packages were incomplete.** Linux and macOS packages shipped without the `forge` man page and the four shell completions; a Linux Crucible package was stamped with the previous version and checksums were missing.
- **Installed libraries did not link.** `iclforge_c/iclforge.h` could not be included (`version.h` was not installed); static archives left {fmt} unresolved and now compile a private copy; `pkg-config --static --libs iclforge_c` now carries the C++ runtime and libm. A C project linking a static C API target must still enable CXX (`docs/library/c-api.md`). On Linux `libiclforge_c.so` now exports only its 216 C functions.
- **Smaller build fixes.** A tests-only configure no longer fails on `iclforge::app_text_encoding`; macOS cross-builds pick the target's SIMD kernels; {fmt} older than 11.1.0 is skipped; Linux wheels build under GCC 14; the Forge GUI user guide's Karaoke note matches the length warning.

## [0.10.0-beta.1] - 2026-09-01

Tenth tagged release. The E-AC-3 encoder catches up with the decision quality AC-3 got in 0.7.0,
both decoders gain a consumer output stage, all three containers become readable as well as
writable, and the verification estate extends to E-AC-3. The immersive surface widens well past
Atmos-in-DD+: an IAMF writer, an IAB/MXF reader bridged onto the Atmos encoder, and an AC-4
bitstream inspector. The browser gains in-page encoding and QC beside the existing decode demo,
plus a reusable streaming decoder package. The Shield Atmos demo grows into a real application,
and the library is now reachable from Rust as well as C and Python. The repository also moved to
trunk-based development, and a concrete API-freeze plan for v1.0 now exists.

### Added

**Encoding**

- **Per-channel exponent strategies and short syncframes for E-AC-3.** The encoder wrote one
  exponent set per frame for every channel; it now plans them per channel or per block, and can
  emit 1/2/3-block syncframes with `convsync`. Spectral distance improves about 0.6 dB on
  transient material. See [Encoding E-AC-3](docs/library/encoding-eac3.md).
- **Real bit-allocation parameters for E-AC-3.** `bamode=1` transmits the frame's own parameters
  instead of inheriting Table E1.4's, and `dbpbcod=3` — measured better at every rate and layout
  tried — replaces the pinned default.
- **Content-decided `dithflag`** on both encoders, per channel per block, free in bits because
  the flag is transmitted either way. `dither=off` pins it at 0 for callers needing bit-for-bit
  agreement between two decodes.
- **Delta bit allocation under coupling, and per-channel coupling membership.** Delta is no
  longer skipped whenever coupling is active; `chincpl` is decided per channel rather than
  frame-wide; 2/0 gets a measured phase-restoring `phsflg`; enhanced-coupling angle interpolation
  is encoded and decoded.
- **Content-adaptive tool selection.** E-AC-3's `auto` chooses coupling, spectral extension and
  AHT from the frame's own spectrum rather than the bit rate alone: +0.11 MOS and +0.36 dB on
  real programme material.
- **Average-rate (ABR) E-AC-3 encoding.** `vbr=avg:kbps[,win:frames]` holds a long-run average
  through a sliding bit reservoir. [The rate-control curve](docs/concepts/ac3-eac3.md#e-ac-3-rate-control-what-vbr-and-abr-are-worth)
  shows where CBR, VBR and ABR each win.
- **A per-frame bit-allocation search** (`EncoderConfig::search`, `eac3::FrameConfig::search`),
  judged by a decoded-domain distortion measure and psychoacoustic model in `ac3::quality`.
  `search=distortion` is a measured win on AC-3 from 448 kbit/s up; `search=perceptual` is not
  yet competitive. Both are off by default. See [Quality measures](docs/library/quality.md).
- **`fgaincod` is settable on both codecs** (`fgaincod=` on the command line). On E-AC-3 this
  means writing Table E1.4's per-block `fgaincode` element, which the encoder had never emitted.
  The default is unchanged and writes no element at all.

**Decoding and playback**

- **A decoder output stage.** `ac3::OutputStage` applies dialnorm, the §7.8 Lo/Ro, Lt/Rt and mono
  downmixes using the stream's own levels, LFE mixing, and the line and RF operating modes.
  Reachable as `decode`/`monitor`'s `channels=`, `downmix=`, `drcmode=` and `mix-lfe`. Off by
  default, so existing callers are unaffected. Lo/Ro agrees with FFmpeg's own fold to 119–121 dB.
  See [Decoding](docs/library/decoding.md).
- **Error concealment**, opt-in: a bad frame is repeated-and-faded or muted in the overlap-add
  domain, so the delay state stays coherent. `conceal=repeat|mute`.
- **The full metadata surface**, writable and reportable on both codecs instead of being constants
  on the way out and skipped on the way in: AC-3's Annex D alternate syntax (`bsid` 6), the
  informational BSI fields, and E-AC-3's `mixmdate` and `infomdat` groups — `bsmod`, `dsurmod`,
  separate Lt/Rt and Lo/Ro levels, programme scale factors, mixing and pan information. See
  [Metadata](docs/library/metadata.md).
- **More than one programme per stream.** Access units are grouped by programme; `decode`, `qc`
  and `levels` take `programme=<0..7>`, and `eac3-encode` can author a second with `programme2=`.
- **Third-party Atmos streams decode.** OAMD, JOC and EMDF read the real breadth of the syntax —
  multiple update blocks, object size/zone/snap, sparse JOC matrices, alternate object data,
  several bed instances — rather than only the shapes this encoder produces. A committed Dolby
  Encoding Engine fixture exercises it.
- **QMF-domain JOC.** Object reconstruction runs in the 64-band complex filterbank the format
  calls for. Mean per-object SNR 22.8 → 28.6 dB. `joc-domain=qmf|mdct` selects it on both sides.
- **A consumer-facing diagnostic sink.** `DecoderConfig::diagnostics` reports recoverable,
  informational decode events — a CRC failure (fired the moment the check runs, so it still
  reaches a caller even when `conceal=` turns the same frame into a successful, concealed
  result) and an EMDF payload id neither decoder interprets. A plain function pointer, no
  allocation, off by default, usable from the minimum-footprint decoder profile.

**Containers and streams**

- **Readers for Matroska, MP4 and MPEG-TS**, plus `ac3cli demux`. Each reads real third-party
  shapes this project never writes, and `decode`, `qc`, `levels`, `play` and `monitor` — and the
  GUI's QC and Inspect pickers — now take a container directly, sniffed by content rather than by
  extension. See [Muxing and sinks](docs/library/muxing-and-sinks.md).
- **`ac3cli probe`**: what a stream declares — layout, substream map, tools in use, metadata
  ranges, CRC validity — without decoding audio. `json=1` emits a versioned schema.
  [Command reference](docs/forge/cli/commands.md).
- **`ac3cli probe` reads AC-4 too**, auto-detected. A new standalone `ac4::` library parses the
  sync frame, table of contents, presentation and substream-group framing (ETSI TS 103 190-1/-2)
  — channel-coded, A-JOC-coded, direct-coded-object and OAMD substream groups alike, including
  7.0.4 through 22.2 channel-based immersive layouts — bitstream inspection, not decoding: audio
  content is reported by byte range, never decoded, and `oamd_common_data()` is refused cleanly
  rather than misparsed. Backed by an independent Python transcription, real Dolby Encoding
  Engine fixtures for the channel-coded path, and synthetic hand-built vectors for A-JOC/object/
  OAMD, no real fixture being reachable for that path. See [Validation](docs/verification.md#ac-4).
- **IEC 61937 de-framing.** A burst parser and `ac3cli unspdif`, plus capture-side recognition, so
  a loopback of a bitstreaming player records the elementary stream rather than PCM.
- **A streaming fMP4/CMAF fragmenter** with a rolling HLS playlist and dynamic MPD, the DASH
  object-audio signalling, the `ceao` brand, and the MPEG-TS ATSC profile beside DVB.
- **Object-layer strip without re-encoding**, so a JOC stream yields a bit-identical-bed 5.1
  companion rendition (`strip-objects`, and `fmp4 … fallback-51` writing both).
- **Stream tools that leave the audio alone**: `transcode` (DD+→DD, carrying metadata across),
  in-place metadata rewrite with CRCs re-stamped, and access-unit-aligned `cut`/`cat`.

**Immersive formats**

- **A JOC → ADM BWF writer.** `decode … adm_out` writes a Dolby Atmos Master ADM Profile BW64
  from a decoded stream's own bed LFE and reconstructed objects, positioned by their real OAMD
  timeline. Scoped to dynamic-object-only programmes. Needs `-DAC3FORGE_BUILD_ADM=ON`.
- **`iamf`, a writer for AOM's IAMF (Immersive Audio Model and Formats) v1.1.0.** E-AC-3 can never
  be an IAMF codec, so this decodes a 7.1.4 stream and re-wraps it as a channel-based IAMF Audio
  Element carrying `ipcm` substreams, in IAMF's own ISO-BMFF encapsulation — a direct route to the
  IAMF/Eclipsa Audio ecosystem alongside the indirect one the ADM writer above already opens
  (AOM's `iamf-tools` encoder accepts ADM-BWF input). Object elements and a reader are not
  started. See [IAMF writing](docs/library/iamf.md).
- **`ac3iab`, a reader for SMPTE ST 2098-2's Immersive Audio Bitstream** — the frame framing and
  every element in the format's element tree, with positions, spreads and gains resolved. Its
  lossless coder is read by identity only. Validated against the DTS reference validator's own
  sample corpus. Reads real MXF IAB Track Files too (`ac3iab::parse_mxf_iab`), not just a bare
  elementary `.iab` file — the wrapping is governed by a separate standard, SMPTE ST 2067-201,
  which clip-wraps the whole bitstream as a single KLV.
- **`atmos-iab`: a real Dolby Atmos cinema/IMF master straight to DD+ JOC E-AC-3.** Every Bed
  channel/Object an IAB file (or MXF Track File) names becomes an `AtmosEncoder` object, driven by
  the file's own authored per-frame panning — `ac3::admbridge::build_iab`, the IAB counterpart to
  the existing `atmos-adm`/ADM bridge. Needs `-DAC3FORGE_BUILD_ADM=ON`.
- **One object-scene timeline type** (`ac3::oba::ObjectScene`) shared by `atmos-path`, the GUI and
  the examples, replacing four ad-hoc formats.
- **Object extent, channel lock and zone constraints on encode**, mapped from the ADM bridge.

**Command line and GUI**

- **A documented exit-code scheme**, `help <command>`, `quiet`/`verbose`, and a man page and shell
  completions generated from the same command table and installed by the build.
- **`record` and `live` reach parity with the GUI session**: any layout up to 7.1.4, either codec,
  `container=raw|mkv|ts|spdif|fmp4` written incrementally, a capture-silence watchdog, an object
  slot budget for `mode=atmos`, and a parallel 5.1 leg for an AC-3-only endpoint.
- **Live object positioning over OSC**, replacing the synthetic orbit `live mode=atmos` and the
  GUI's live room used to fake motion with. `ac3cli live ... mode=atmos positions=osc:<port>`
  and a "Drive objects from OSC" toggle on the GUI's Live session card both drive object
  placement from a show-control rig or a DAW in real time (`/object/<n>/xyz|gain|lfe|release`,
  0-based), room markers greying out while a live update owns them. Loopback-only by default;
  `positions=osc:any:<port>` opts into every interface. MIDI and a desktop game controller are
  follow-ons under the same `positions=<scheme>:...` grammar, not implemented yet.
- **`play` follows the sink**: it reads what a chosen receiver actually accepts (EDID short audio
  descriptors on ALSA; a live probe elsewhere) and adapts instead of refusing — a source format the
  sink can't bitstream is transcoded to AC-3 or decoded to PCM automatically, so the "no 5.1 PCM
  over optical" case now takes one command instead of two. `follow=off` restores the old refusal.
- **A GUI stream player** — the twin of `ac3cli monitor` — with transport, live meters, the
  soundfield view, and WAV/object export from the same decode pass. A finished run offers **QC
  this run** and **Inspect objects** directly. See [Open stream](docs/forge/gui/open-stream.md).
- **Desktop integration**: drag-and-drop, `ac3gui <file>`, and `.ac3`/`.ec3` file associations on
  Windows, macOS and Linux, so the app appears in application menus instead of being launch-only.
- **A self-contained Linux AppImage for `ac3gui`**, bundling its own Qt 6 instead of depending on
  the host distro's own `qt6-base-dev`/`qml6-module-*` split, alongside the existing `.deb`/`.rpm`.
  See [Linux](docs/platforms/linux.md#appimage).
- **Loudness of the rendered layout and of objects.** Metering follows BS.1770-5's extended
  algorithm for advanced sound systems, weighting channels by position, and can re-render an
  object programme onto a named layout by its own positions before metering. `qc` gained
  `layout=rendered|bed` and `objects=<layout>`, plus two new delivery presets.
- **GUI localisation.** A Preferences **Language** picker switches the app live between English
  and five real languages (Français, Deutsch, Español, العربية, עברית, יידיש — the same set the
  sibling CountdownSolver project ships), with right-to-left mirroring and bundled Noto Sans
  Arabic/Hebrew faces for the three languages that need them. Coverage is partial today (window
  chrome, tab names, the Guided wizard, all of Preferences) and tracked, not hidden — see
  [Localisation](docs/forge/gui/localisation.md). A pseudo-locale QA fixture proves the extraction/
  compile/load pipeline end to end independent of real-language completeness, and CI now fails if
  a `qsTr()` change isn't reflected in the committed translation catalogue.
- **GUI accessibility.** Every custom control and every control in the main window now reports a
  real `Accessible` name, role and description to screen readers, built from the same live state
  the visuals already read rather than a static copy of a label — channel meters, QC gates, the
  Guided wizard's cards, the object-placement room and timeline views, run-strip chips, all of it.
- **`ac3cli spatial`, a Windows Spatial Sound object sink.** Every JOC-reconstructed object goes
  out as a dynamic object at its real OAMD position, and the bed's LFE as a static one, through
  `ISpatialAudioObjectRenderStream`. This is the one path that lets Dolby's own renderer engage
  with this project's reconstructed objects at all — a licensed decoder otherwise refuses to
  object-decode a stream without a signing key this project doesn't ship. Refuses cleanly, naming
  which Settings toggle to flip, when the chosen endpoint has no spatial sound format enabled;
  `ac3cli outputs` reports each device's spatial capability alongside its passthrough columns.

**Browser (WASM)**

- **An in-browser encode demo**, alongside the existing decode one: drop a `.wav` file and get back
  a real AC-3/E-AC-3 elementary stream, encoded entirely client-side by the same codec compiled to
  WebAssembly, plus a real BS.1770 loudness/true-peak QC verdict against the same five delivery
  presets `ac3cli qc` checks — computed on the same PCM, in the page. A round-trip preview decodes
  the produced stream through the existing decode module to prove it's real. Headless-browser CI
  coverage (Playwright) now spans both demos, not just decode.
- **A reusable browser decoder package**, `ac3forge-wasm-decoder` (source in
  [`js/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/js)), turning the WASM decode
  demo's underlying build into a reusable browser decoder — a real answer to Chrome's continued
  inability to decode EC-3 natively
  ([video.js http-streaming#1297](https://github.com/videojs/http-streaming/issues/1297)). It is
  **not on the npm registry yet**: the publish job is deliberately held to a manual dispatch until
  the one-time npmjs trusted-publisher setup in [docs/releasing.md](docs/releasing.md) is done, so
  consume it from source for now.
- **A push-frame decode API** over the caller-buffer `decode_access_unit_into` form, so decoding a
  live/streaming source allocates nothing on the hot path.
- **A realtime AudioWorklet playback pipeline**: decoding runs in a Worker, off the main thread;
  only a lock-free `SharedArrayBuffer` ring-buffer drain runs on the audio-rendering thread.
  Multichannel output or the library's own §7.8 downmix (never a hand-rolled fold) is selectable
  per stream.
- **An hls.js/MSE bridge** for playing EC-3 audio where the browser cannot decode it natively —
  patches `MediaSource`'s codec-support/`SourceBuffer` surface (a passive event listener alone
  doesn't work: hls.js drops an audio track outright the moment the real `addSourceBuffer` throws
  for an unsupported codec) and extracts access units from the fMP4 segments hls.js's own remuxer
  produces.
- The docs site's WASM demo now consumes the bundled JavaScript bindings and their
  decode/playback logic.

**Shield Atmos Demo (Android)**

- **New: the wire trace.** A second thread parses back the exact access units going out over HDMI
  and draws what a decoder finds in them — the lead object's intended height against the height read
  back off the wire, which is a visible staircase because height is sent in sixteen steps. It
  deliberately computes no reconstruction-quality figure: both ends share the same non-normative QMF
  prototype, so such a number would be unfalsifiable by construction. What it does prove is that the
  object container survives on the wire, and that OBJECTS OFF genuinely removes it.
- **New: five demo scenes and a guided tour.** The app had exactly one thing to show — three
  objects on fixed orbits — from launch until you walked away. It now has Orbit, Flyover, Overhead,
  Elevator and Front/back, each with its own line of what to listen for, blended rather than jumped
  between; and once left idle it walks them itself rather than just inviting the next person.
- **New: record a path and loop it.** Fly the object by hand, press again, and it flies your own
  gesture forever — still pushable, still springing back to itself.
- **New: controller rumble** on the two crossings the ear is least sure of: passing overhead, and
  passing through the listening position.
- **New: a settings panel and a phone remote.** Every control was previously an undocumented
  keypress. The panel is D-pad navigable; the phone remote serves one page so anyone in the room can
  drive the object from their own phone. The remote is **off by default** and has no authentication
  — it starts only when switched on, and stops when the demo leaves the screen.
- **New: OBJECTS OFF** strips the object layer out of the live stream on a keypress, so a licensed
  decoder can be watched dropping from Atmos to DD+ and back with the object layer's byte cost on
  screen. Plus a real BS.1770 loudness readout, a programme meter with PPM ballistics replacing a
  fixed display gain, and a soundfield-energy arrow computed from the encoded bed.

**Library, C API, Python and Rust**

- **A pimpl sweep across the exported surface**, so a private-state change is no longer an ABI
  break for anyone linking the shared libraries. [Library overview](docs/library/index.md) records
  the one deliberate exception and how it is meant to grow.
- **An E-AC-3 encoder in the C API and in Python**, covering plain E-AC-3 and the wide
  dependent-substream layouts with the Annex E tools. See [C API](docs/library/c-api.md) and
  [Python API](docs/library/python-api.md) for what is deliberately not mirrored.
- **Stream scan, caller-buffer decode, and loudness/level/QC metering, all now in the C API.**
  `ac3forge_scan` reports what a stream actually contains — layout, every programme, the
  DVB/ATSC service fields a muxer's descriptors want — without decoding any audio.
  `ac3forge_decoder_decode_frame_into`/`ac3forge_eac3_decoder_decode_access_unit_into` decode
  into caller-owned buffers instead of allocating per call, for the realtime embedder this C
  surface exists for, and preserve the §3.7 transient pre-noise hold-back exactly (a held-back
  frame leaves the caller's spans untouched). `ac3forge_loudness_meter_t`/
  `ac3forge_level_meter_t`/`ac3forge_qc_preset`/`ac3forge_evaluate_qc_gate` mirror the library's
  BS.1770-5 loudness meter, level meter and named delivery-QC gates. See
  [C API](docs/library/c-api.md).
- **A first Rust binding over the C API**: `ac3forge-sys` (raw, `bindgen`-generated against the C
  header at build time) plus a safe `ac3forge` wrapper covering AC-3 and E-AC-3 encode/decode. The
  C API had never crossed a real FFI boundary before — building this found and fixed two real
  header defects (a missing `ac3forge_object_placement_init()`, undocumented pointer lifetimes on
  four decoded-audio accessors). See [Rust bindings](docs/library/rust-api.md).
- **A latency budget** exposed through every binding, and **a minimum-footprint decoder profile**
  (`AC3FORGE_MINIMAL_DECODER`) proven on a cross-compiled bare-metal target.
- **Zero-copy numpy encode/decode in Python**, plus caller-buffer decoding. Every `encode_frame`/
  `encode_access_unit` call accepts a 2-D `(n_channels, n_samples)` array as well as a sequence of
  1-D arrays, and reads directly out of whichever is passed when it is already contiguous
  `float32`; decoded `.channels`/`.object_audio` are read-only views onto the decoded object's own
  memory instead of a fresh copy on every access; `FrameDecoder.decode_frame_into`/
  `Eac3Decoder.decode_access_unit_into` write PCM into caller-supplied buffers for a realtime
  embedder or tight batch loop that wants to reuse them. See [Python API](docs/library/python-api.md)'s
  "Zero-copy numpy and buffer reuse".
- **pkg-config files** for every installed component (`ac3forge`, `ac3signing`, `matroska`,
  `mp4`, `mpegts`, `iamf`, `ac3iab`, `ac3adm`, `admbridge`, `ac3forge_c`), for a non-CMake
  consumer.
  **`ac3adm`/`ac3::admbridge` (the ADM/BW64 reader and its Atmos bridge) are now installable via
  `find_package(ac3forge)`**, shared-only, without re-exporting the third-party libbw64/libadm
  they embed. **A `capi` feature** for the vcpkg port and Conan recipe reaches `ac3::forge_c`
  through either package manager for the first time. See
  [Using ac3::forge](docs/library/index.md).
- **Stream scanning in Python.** `ac3.scan()`/`ac3.read_frame_header()` read an elementary
  stream's shape — channel layout, every programme, every access unit's byte range — without
  decoding any audio, plus timing helpers (`ac3.access_unit_timing`, `stream_duration_seconds`,
  and neighbours) for a muxer computing where to cut. See [Python API](docs/library/python-api.md)'s
  "Scanning a stream".
- **Research trace export, reachable from Python.** The encoder/decoder mirror trace — added for
  the in-repo self-check — now fills in from an ordinary decode too, and
  `ac3::verify::append_trace_csv`/`append_trace_json_lines` (`ac3.verify.trace_to_csv`/
  `trace_to_json_lines` in Python) turn it into one tidy row per (frame, substream, block, stream,
  kind, index, value): per-frame bap, exponent, the §7.2.2.5 masking curve and the composite SNR
  offset, ready for `pandas.read_csv`/`read_json` and `.to_parquet()` from there.
- **`FrameError` gained `describe()`**, matching every other error type. Python's `Ac3EncodeError`
  now carries a real message instead of just the failing enumerator's name.
- **A concrete API-freeze plan for v1.0** ([docs/library/api-stability.md](docs/library/api-stability.md),
  roadmap `AP1`): a Public/Internal/Diagnostic/Experimental tier for every header under `ac3/`, a
  SemVer/deprecation policy, a C config struct growth policy, and release criteria. The C API
  gained a compile-time version alongside its existing runtime-only `ac3forge_version()`:
  `AC3FORGE_C_VERSION_MAJOR`/`MINOR`/`PATCH`/`AC3FORGE_C_VERSION` in `ac3forge_c/ac3forge.h`.
  `SOVERSION` and an ABI-tagging inline namespace are deliberately deferred to the `v1.0.0` cut
  itself — see the page's own reasoning.

**Verification**

- **E-AC-3 gains the coverage AC-3 already had**: an encoder input-space fuzzer, a mirror
  self-check diffing the encoder's model against a real decode per block, and metadata-parser
  fuzzers for the EMDF, OAMD, JOC, signing and ADM paths with a CRC-repairing mutator.
- **Real programme material in the fixture corpus** — two 30 s CC0 speech and music fixtures
  beside the synthetic ones, versioned and hash-enforced — and **a perceptual column that carries
  real numbers in CI** rather than nulls.
- **Third-party decode interop gates** against committed Dolby Encoding Engine and FFmpeg streams,
  plus a nightly run over pinned FATE samples.
- **Published conformance vectors** ([usage](docs/conformance-vectors.md)) and **a threat model
  for untrusted input** ([threat model](docs/threat-model.md)), both shipped with every release.
- **New CI legs**: ThreadSanitizer over the audio layer, script linting, PR-time performance
  comparison, an advisory ABI diff against the last release, CodeQL over the Android app's Kotlin,
  container-command tests, a headless browser test of the WASM demo, and instrumented tests for
  the Android bridge's device-free paths.
- **A Windows ARM64 CI leg** on GitHub's hosted `windows-11-arm` runner, building and testing
  `ac3cli` on real ARM64 hardware and packaging a `win-arm64` release archive — CLI-only for now
  (no resolvable prebuilt Qt6 ARM64 kit yet) and experimental until proven green over real runs.
- **An object-reconstruction quality trend**, and listening-test apparatus (no session has been
  run yet).
- **The block-switch decision's cross-toolchain determinism is proven, not assumed.** The
  transient detector that decides `blksw` — which reshapes MDCT type, coupling/AHT eligibility
  and rematrix bands every block, on both encoders — is verified bit-identical across five
  independent compiler/architecture builds, and its decision is now pinned by tests at all six
  A/52 sample rates instead of just one.

**Release engineering**

- **The packaging manifests bump themselves after a release.** A new post-release job downloads
  the release's own source tarball and platform assets, computes the digests the vcpkg port, the
  Homebrew formula and cask, the winget manifest and the Conan recipe each need, cross-checks the
  ones that are real built packages against the release's own published `SHA512SUMS`, opens a PR
  bumping all four together, and pushes the Homebrew formula/cask straight to the live tap. This
  is what had gone stale two releases in a row before it existed. Testable without cutting a
  release: it is also directly runnable by hand in dry-run mode against any already-shipped tag.
- **GitHub Release notes are drawn from CHANGELOG.md**, not drafted from the commit list — the
  matching dated section becomes the release body directly, since that curation already happens
  in CHANGELOG.md as part of normal development.
- **`check_packaging_versions.sh` gained a latest-tag advisory**: a warning, not a failure, when
  a manifest does not yet match the most recent release.

**Tooling and packaging**

- **macOS release packages are now universal (arm64 + x86_64) binaries.** A new CI leg builds a
  real (not cross-compiled) x86_64 half on GitHub's native-Intel `macos-15-intel` runner, and a
  merge job `lipo`s it together with the existing Apple Silicon build into one `.dmg`. The
  Homebrew Cask no longer restricts itself to `arch: :arm64`.

### Changed

- **JOC defaults to the QMF domain** on both sides. Reconstructed object audio now lags the bed by
  576 samples rather than 256.
- **The fast inverse transform reaches enhanced coupling and JOC, and the FFT core is radix-4.**
  A 30-second 15-object decode drops from 6.5 s to under 3 s. Encoder output is byte-identical.
- **SIMD kernels are selected by CMake per architecture** rather than by `#ifdef`, with
  bit-identical output and no runtime dispatch.
- **Runtime AVX2 dispatch — and the three non-SIMD findings that outweighed it.** A second,
  AVX2-flagged kernel tier is now chosen per process by CPUID (`AC3FORGE_SIMD_TIER=auto|sse2|avx2`
  forces either way for testing), carrying 256-bit windowing and twiddle stages plus batched
  four-transform IMDCT/MDCT kernels. Output is unchanged: real encodes and decodes are
  byte-for-byte identical under `sse2` and `avx2`.
  Profiling by *source line* rather than by symbol then found three costs larger than every
  transform in the codec put together, all of them redundant work rather than missing
  vectorisation, and all with unchanged output:
  `FrameParameters::at()` re-walked an O(objects) offset list on **every** coefficient access,
  making a frame O(objects²) — a 12-object Atmos decode is now **1.82×** faster under
  `joc-domain=mdct` and **2.90×** under the default `joc-domain=qmf`;
  `aht_bin_gaq_bits` fully quantised six mantissas per candidate gain to read one integer width
  off each, where that width follows from a single predicate — **1.70×** on `eac3_51_auto`
  whole-frame encode;
  and §6.6.5's QMF mixing coefficient re-evaluated its shape/timeslot branches once per
  (subband, channel) instead of once per (object, timeslot) — **−8.6%** instructions on a
  12-object QMF-domain decode.
  FMA3 was measured (~1%, and it perturbs results) and declined, so `-ffp-contract=off` stays
  pinned. See [docs/building.md](docs/building.md)'s "Runtime AVX2 dispatch" and
  [docs/performance-trend.md](docs/performance-trend.md)'s "Profile by source line, not by symbol".
- **Floating-point contraction is pinned off project-wide**, and the timing benches run real
  programme material instead of a single tone.
- **The coverage gate covers `apps/cli` and `python/`**, not just `src/`, and the fuzz jobs are no
  longer `continue-on-error`.
- **Enhanced coupling and transient pre-noise are measured and documented but not automatic.**
  Enhanced coupling sounds better on real material at every point tried but is kept out of `auto`
  because FFmpeg misreads its syntax; transient pre-noise measures worse than leaving the audio
  alone at every rate, because block switching gets there first.
- **The repository moved to trunk-based development.** `develop` is retired and `main` is the
  single long-lived branch; topic branches are `feature/*` and `bugfix/*` only. This removes the
  promotion and sync-back pull requests entirely. Branch protection moved across with the same
  parameters. See [CONTRIBUTING.md](CONTRIBUTING.md) and
  [branch protection](.github/branch-protection.md). The trend pages still show two tracks so
  historical data stays visible; reworking them for a single track is separate follow-up work.
- **ROADMAP.md was rebuilt** for the post-0.9.0 state.
- **A pre-freeze naming sweep, source- and ABI-breaking.** JOC's namespace now matches its header
  path: `ac3::joc` is `ac3::oba::joc`. The S/PDIF burst packer's directory now matches its
  namespace, which was already correct: `ac3/sinks/iec61937.hpp` is `ac3/iec61937/iec61937.hpp` —
  `ac3::audio`'s `PassthroughSink`/`MonitorSink` are the library's actual `Sink` types, and this
  header was never one. `ac3::FrameEncoder`/`ac3::eac3::FrameEncoder` keep their shared name across
  namespaces on purpose; [Library overview](docs/library/index.md) now writes down the
  codec-vs-codec-blind namespace split that rule follows.
- Internal: `std::format`/`std::print` replaced with {fmt} throughout, since the NDK's libc++ has
  no usable `<format>`; the WASM demo plays the library's own downmix rather than a hand-rolled
  one.
- Internal: the macOS backend's loopback-capture gap is documented against Apple's real Core Audio
  process/system tap API (`AudioHardwareCreateProcessTap`/`CATapDescription`, macOS 14.2 — the
  in-tree comment previously cited 14.4) and now carries a pure, CI-verified OS-version capability
  check (`ac3::coreaudio::system_audio_tap_api_available()`) a future implementation should refuse
  on. Capture there is still input-only; the tap itself needs real Mac hardware to build and
  verify. See [macOS](docs/platforms/macos.md#per-application-capture-the-core-audio-process-tap).

### Fixed

**Codec correctness**

- **Five E-AC-3 decoder defects in syntax only a third-party encoder produces**, found by pointing
  the decoder at real Dolby Encoding Engine and FFmpeg streams: AHT flags gated wrongly, the
  coupling channel's own gain and offset fields not read at all, band-structure tables not carried
  across blocks, `first*` state tracked wrong, and a missing coupling-state reset.
- **Coupling and delta bit allocation**: the decoder never read `cpldeltbae`; AC-3 coupling
  desynchronised once membership went per channel; `deltbaie=0`'s "retain" meaning was not honoured
  once exponent sets could change mid-frame; `snroffststr 0x2` read the wrong fields; a coupled
  block skipped `cplfgaincod`/`cplfsnroffst` entirely.
- **A framing bug on real disc and broadcast content**: an AC-3 frame's `crc1` bytes were read as
  `strmtyp`/`substreamid`, merging unrelated frames into one access unit — 176 of 480 groups on one
  sample.
- **`dialnorm=auto` and `ac3cli loudness` mis-assigned channel weights** on any layout wider than
  stereo, feeding WAV-order channels to a coded-order meter, so LFE could receive the surround
  boost meant for a surround channel.
- **A coordinate's binade shift was computed through `std::log2`**, whose last-bit behaviour is not
  required to agree across compilers, at exactly the input class where the true result is an
  integer. Replaced with `std::ilogb`, which reads the exponent directly. Byte-identical on real
  material.
- **The QC dialog reported the wrong preset's verdict.** Its preset list was written when there
  were three presets and never updated when two more were inserted into the middle of the shared
  list, so the option labelled "Netflix" applied a different preset's gate under Netflix's name,
  and two presets were unreachable. The control now derives from the same list the selection
  indexes into.
- **`latency_samples()` ignored the syncframe length**, reporting the six-block figure for a short
  syncframe and so overstating a one-block frame by about 27 ms — to exactly the caller sizing
  buffers for low-latency use.

**Robustness**

- **`mp4::Reader` could index far past its input** on a fragmented box using the 64-bit largesize
  escape to declare a size near `UINT64_MAX`, wrapping the parse position behind the streaming
  reader's window.
- **`ac3::io::read_wav` could read past the end of its buffer** on a file whose header sits near
  EOF, plus seven more out-of-bounds and precondition bugs in bit allocation, ADM parsing and
  signing verification. Each has a reproducer under `fuzz/regressions/`.
- **`eac3-encode`/`eac3-sine` crashed instead of erroring** on a bitrate beyond what `frmsiz` can
  signal, and when `auto` chose AHT under a short syncframe.
- **Python's encoders segfaulted instead of raising** when given the wrong *number* of channel or
  object arrays; only the per-array length was checked, and the underlying guard is an `assert()`
  compiled out in release wheels.

**Tooling and packaging**

- **The committed WASM demo fallbacks had gone stale, and the two directories had drifted apart.**
  Every module committed under `docs/assets/` predated the bindings it serves: the decode demo's
  `ac3forge_decode.wasm` was 365 KB and the encode demo's own copy of that same module 372 KB —
  already inconsistent with each other — against the 615 KB a current build produces, and the
  encode module was 389 KB against 640 KB. So a local `mkdocs serve` (and the PR-time docs build)
  embedded much older modules than the checked-in pages expect. The live site was never affected:
  the docs deploy job rebuilds both demos fresh on every publish. All four copies are now taken
  from a single fresh build on the pinned Emscripten (6.0.6), so the two directories agree, and
  verified by running `apps/wasm/tests`' Playwright suite against the committed copies themselves
  rather than the build tree: the decode demo decodes the bundled Atmos-in-DD+ fixture with real
  moving object positions, and the encode demo encodes a known stereo tone, matches its QC verdict
  and round-trip decodes it.
- **Containers hardcoded 1536 samples per frame**, breaking timelines on short E-AC-3 syncframes;
  `atmos bed51` still advertised an object layer it deliberately did not encode.
- **The minimum-footprint image ceiling was stale**, measured before the QMF work landed on the
  branch it was taken from. Re-measured and re-based.
- **Several CI checks false-failed on outcomes they exist to report** rather than on real defects:
  the trend runner aborted on a leg whose infeasible tool variants are the point of the leg; two
  encoder-space fuzzers treated a documented loudness-gate refusal as a hard failure because it
  arrives on a different exit code; a matrix-coverage check compared two spellings of a
  parameterised token that could never match; and the E-AC-3 mirror self-check carried an
  assumption that went stale when a parallel branch gave the coupling channel a delta field.
- **A Python latency test asserted a figure the C++ side never agreed with**, failing the wheel
  workflow on two platforms. The C++ value was correct; the test had drifted.
- **Packaging manifests and the Homebrew tap** were two releases behind, and **several pages
  described shipped work as still pending** — both corrected.
- **`python/pyproject.toml`'s licence identifier drifted to `GPL-3.0-only`** while `vcpkg.json`,
  the Conan recipe, the Homebrew formula and the README's own grant language ("or (at your
  option) any later version") all agreed on `GPL-3.0-or-later` — corrected to match. The ABI
  gate's exported-symbol allowlist and shared-library-diff steps discovered libraries from a
  hardcoded list rather than the actual build output, which had silently left `libac3iab.so`
  uncovered by both since it landed; both now discover dynamically, and a statically-embedded
  third-party dependency (`libadm`, pulled in by the new `ac3adm` export above) was found leaking
  ~16,800 of its own template-instantiation symbols into `libac3adm.so`'s dynamic symbol table
  through this change, fixed with a linker `--exclude-libs` flag rather than shipped. Both the
  licence check and a vcpkg-feature/Conan-option/pkg-config completeness check are now part of
  `tools/checks/check_packaging_versions.sh`.
- **The Windows installer stopped silently degrading to a ZIP-only package.** `cpack`'s NSIS
  generator dropped itself whenever `makensis` was missing with no diagnostic anywhere, so the
  release shipped without an installer for several releases before anyone noticed. CI now
  installs `makensis` and fails the build if a real `.exe` doesn't come out of `cpack`; a local
  build without NSIS installed still falls back to ZIP-only, but now says so. The
  packaging-consistency check also now catches a winget manifest whose `InstallerType` doesn't
  match its own installer URL or nested-installer fields — the same class of drift a manual
  copy-forward release bump can introduce.
- **The ABI gate stopped failing on every pull request.** `abi-gate` compared HEAD against the
  last release tag, so it reported the whole release cycle's accumulated drift — 806 commits'
  worth by 2026-08-28 — on every PR, including ones that touched no source at all. It now
  compares against the PR's own merge base; the last release tag is still the comparison point
  on a push or a tag, where that view is the useful one. `abidiff` also runs under
  `tools/ci/abi-suppressions.ini`, which drops the libstdc++ template instantiations that are
  not part of any ABI this project controls — about 900 of the roughly 1050 entries the gate was
  emitting. Advisory pre-1.0 now means green: the job reports through a single `ABI_ENFORCE`
  switch rather than `continue-on-error: true`, which never made the check green in the first
  place, since GitHub still reports a continue-on-error job's own check run as `failure`. The
  exported-symbol allowlist, six symbols behind `main`, is back in sync.

**Browser (WASM)**

- **The encode demo's round-trip preview 404'd on the published docs site.** The page loaded its
  decode module as `../ac3forge_decode.js` even though the build copies that module in next to the
  page precisely so the directory is servable from anywhere; the parent-relative path only worked
  when the demo directory was the server root, and broke under the docs site's subdirectory embed.
  The Playwright harness now serves both demos from a subdirectory for every run, so the layout
  that failed is the layout that gets tested.

**Shield Atmos Demo (Android)**

- **The encode loop kept streaming to the receiver after the demo left the screen.** It stopped only
  in `onDestroy`, so pressing HOME left a cached process pushing E-AC-3 bursts into the AVR with no
  UI and nothing to stop it. Now stopped in `onStop`, without tearing down the stream for the app's
  own About screen. Both on-screen render loops likewise ran behind other windows.
- **"Waiting for receiver" cleared on a capability probe rather than on audio flowing**, so a failed
  sink open left a fully-drawn dashboard over permanent silence. Readiness now means the encode loop
  is confirmed running, with a distinct "starting" state in between, and the waiting screen reports
  what the HDMI route actually advertises — including whether it claims the Atmos (JOC) profile.
- **A native library load failure crashed instead of showing its own failure screen**, because a
  throwing static initializer marks the class erroneous and the later `NoClassDefFoundError` is not
  what the call sites caught.
- **A partial `AudioTrack` write duplicated bytes into the IEC 61937 stream**, since a short write
  was retried by resubmitting the whole burst. Now resumed from.
- **Precise placement was impossible**: a flat per-axis deadzone with no rescaling meant the
  smallest deflection anyone could hold was about a third of full travel. Now radial and rescaled,
  seeded from the device's own declared flat range. Right-stick height is resolved by probing which
  axis the device declares rather than assuming.
- **The status line now reports whole-frame occupancy**, not just `encode_frame()`. The previously
  quoted figure excluded synthesis, the limiter, both meters, signing, stripping, the packer and the
  JNI submit — most of the frame.
- **The real-time encode thread no longer attaches to and detaches from the JVM once per frame**, and
  both worker threads now have explicit priorities instead of inheriting whatever started them.
- **`isDirectPlaybackSupported` was called unguarded on a minSdk-26 app**, so on any API 26–28
  device — a 2015/2017 Shield on Android 9, for instance — the app's most load-bearing platform
  query threw `NoSuchMethodError` rather than degrading. Guarded.

### Security

- **Build-time key material can no longer reach a published Shield APK.** The EMDF object-signing
  key asset is now deleted after the debug smoke build and before any release step, and the staged
  release APK is asserted to contain no `signing.key` entry before it can be uploaded. The check
  reads the APK's actual entry list rather than trusting step ordering, so a reordering or a Gradle
  asset-merge change fails the release instead of shipping the asset. Worth knowing because it
  changes the shipped artifact: a published release APK therefore carries no object-signing key,
  so it emits the 5.1 bed rather than a signed object stream and a receiver's Atmos indicator will
  not light where a previously published build lit it. Locally built debug APKs, which still have
  the key, are unaffected.

## [0.9.0-beta.1] - 2026-08-22

Ninth tagged release. The headline is the memory-usage optimization programme landing in full:
per-frame codec allocation churn down 54–88%, every CLI command and GUI recording streaming
instead of buffering, and a new memory trend that gates regressions the same way the timing
series always has — alongside a default-on fast inverse transform (4.5–4.7× faster decodes), a
whole-library per-component coverage gate, `ac3::signing` joining the installed/exported library
surface, and continued `apps/cli` command-group extraction.

### Added

- **Performance and reference transform modes.** The decoder's inverse transform joins the
  forward MDCT in having a fast path: §7.9.4 step 3 — the one O(N²) part of the normative
  inverse — now runs through the same radix-2 FFT core the fast forward fold uses, and after
  its evidence was reviewed (worst transform-level relative error 7.8e-14 against the direct
  form; 214.9 dB SNR agreement for AC-3 and 284.7 dB for E-AC-3 over 180 seconds of real 5.1
  material) it became the default: **decodes run 4.5–4.7× faster** (a 180-second decode drops
  from ~3.5 s to ~0.8 s), and the direct form's 320 KiB of tabulated matrices are no longer
  built at all on the default path. The pair is exposed as one intent-level switch:
  `mode=reference` runs every transform in a command on the spec's own direct evaluations —
  the forms the fast paths are validated against, for fixture regeneration or sample-for-sample
  comparison against an external decoder — and `mode=performance` (the default state) names the
  fast paths; `fast-mdct=off` / `fast-imdct=off` still adjust one half at a time. Encoded
  output never depends on the decode-side switch. See
  [Validation → Performance and reference modes](docs/verification.md#performance-and-reference-modes).
- **Span-output decode forms.** `FrameDecoder::decode_frame_into` and
  `Eac3Decoder::decode_access_unit_into` decode into caller-owned planar storage rather than
  allocating a fresh vector per call, with the same results as the value forms, pinned by
  lockstep equivalence tests. The E-AC-3 form keeps
  §3.7's transient-pre-noise hold-back semantics exactly: a held-back frame leaves the caller's
  spans untouched and is copied out at release.
- **Streaming I/O for unbounded sessions.** `ac3::io::WavStreamReader` (block-at-a-time WAV
  reading with the same parsing and sample conversion as the whole-file reader),
  `ac3::io::WavPcm16StreamWriter` (the incremental sibling of the one-shot PCM16 writer, for
  IEC 61937 carriers whose length isn't known up front), and `mpegts::Writer` (incremental
  transport-stream muxing whose output is byte-identical to `mpegts::mux()` — that equality is
  its contract and its test). Matroska already had its incremental `Writer`; MP4 deliberately
  does not get one — `moov`/`stco` need every frame's final offset, and `fragment()` (fMP4) is
  that format's streaming shape.
- **A memory trend beside the timing trends.** `ac3membench` counts heap allocations and
  allocator traffic per frame, live-byte drift and peak RSS across the encoder configurations
  *and* the decode paths the timing benches never covered; every `develop`/`main` push appends
  to the same `quality-history` series the CPU numbers use, rendered on
  [docs/performance-trend.md](docs/performance-trend.md) with the same trailing-baseline gates
  (either churn metric regressing flags the row) plus an absolute leak check that applies
  regardless of the trailing baseline.
- **`ac3::signing` is now an installed, exported library component** (repo-structure review D6),
  restructured into the same OBJECT+STATIC+SHARED shape `ac3::forge` itself uses
  (`ac3::signing_static`/`ac3::signing_shared`, `AC3SIGNING_EXPORT`-annotated) instead of a
  single internal-only `STATIC` target with no `install()` at all. `signing_static`/
  `signing_shared` each publicly link their own matching `forge_static`/`forge_shared`,
  preserving today's `PUBLIC ac3::forge` propagation; a real standalone
  `find_package(ac3forge CONFIG REQUIRED)` consumer linking `ac3::signing_static` now builds and
  runs across the installed-package boundary.

### Fixed

- **The encoder input-space fuzz no longer reports FFmpeg container-probe misses as encoder
  failures.** Case seed 1124127684685913171 (stereo at 512 kbit/s, 48 kHz) produced a fully
  valid stream — every syncframe on its exact 2048-byte boundary, both CRC words of every frame
  good, a clean strict decode under `-f ac3` — that FFmpeg 8.0's auto-detection nonetheless
  handed to its MPEG-PS demuxer: with frames that large, ffmpeg's AC-3 prober cannot clear its
  own accept threshold inside the 8 KiB probe window (it wants seven consecutive syncframes),
  while three start-code-shaped byte patterns inside ordinary quantized mantissas were enough
  for the MPEG-PS prober to win that window outright, and no amount of appended audio can win it
  back. `tools/ci/fuzz_encoder_space.py` now arbitrates any FFmpeg refusal by rerunning with
  `-f ac3` forced and every error check kept — a clean forced decode classifies the case as
  "misprobed" (counted and reported, never failing), a refused one still fails with the real
  decode error. The seed is recorded in the script's new `--regressions` replay list, which CI
  gates on before each unseeded search, and `fuzz/seeds/` gained a 512 kbit/s stereo stream so
  decoder-side fuzzing mutates from the big-frame corner too.
- **Installed packages now actually export `ac3::forge_c_static`/`ac3::forge_c_shared`**, matching
  what [docs/library/c-api.md](docs/library/c-api.md) and the in-tree `ALIAS` targets always
  documented. The raw CMake targets were previously `capi_static`/`capi_shared` under the `ac3::`
  namespace with no matching alias, so an installed package actually provided `ac3::capi_static` —
  a name nothing in the documented consumer surface used, and a `find_package(ac3forge)` consumer
  following the docs could not link the C API at all. Fixing the name surfaced a second, more
  serious bug: the C API's object library always privately links the static codec regardless of
  `BUILD_SHARED_LIBS` (a deliberate self-contained-ABI design), and
  `AC3FORGE_INSTALL_BOTH_LINKAGES=OFF` combined with a shared-only build used to leave that static
  target out of every export set, failing the configure step outright. That combination now
  configures, builds and installs cleanly.
- **The Conan and Winget packaging manifests are back on the real latest release** — both were
  still pinned to `0.8.0-beta.1` after `0.8.0-beta.2` shipped. `tools/checks/check_packaging_versions.sh`
  now runs in CI and fails the build if any packaging manifest's version drifts from the others
  again.
- **The hosted WASM decode demo (`docs/assets/wasm-decode-demo/`) matches the real one again** — it
  had silently fallen out of sync with `apps/wasm/`'s own copy (missing favicon links and the GPL
  footer). `docs.yml`'s docs build now byte-compares the two and fails if they drift apart again.
- **The Debian/Ubuntu package's homepage field is no longer empty** — `PROJECT_HOMEPAGE_URL` is now
  set on the root `project()` call, so `dpkg -s ac3forge` reports the real project URL instead of
  nothing.
- Fixed a stale anchor in `docs/platforms/raspberry-pi.md` pointing at a `linux.md` heading whose
  text no longer matches.
- Fixed `docs/library/index.md` and `docs/releasing.md`'s vcpkg port sections, which still blamed
  `ac3::forge_c`'s absence from the port on the installed-export-set bug fixed above — the port
  has always passed `-DAC3FORGE_BUILD_CAPI=OFF` regardless of that bug and continues to now that
  it's gone, as a deliberate scope decision pending a `capi` feature. Verified with a real
  `vcpkg install ac3forge --overlay-ports=packaging/vcpkg-port` that the port still installs no
  `ac3::forge_c` artifacts today.
- **A stack-overflow-risk PREfast finding (alert #77) is fixed**: `examples/atmos_objects.cpp`
  now heap-allocates its `Eac3Decoder` instead of stack-declaring it, the same fix already
  applied to `atmos_fallback.cpp` and `station_broadcast.cpp` for the identical scratch-state
  growth. Two duplicate false-positive `optional`-access findings (alerts #70/#71, in
  `apps/gui/qc_controller.cpp`'s and `apps/cli/main.cpp`'s `measure_qc`/`measure_eac3`) are
  documented and suppressed — a `have_first`/non-empty-stream guard already proves the meter
  optional is engaged before use, matching a pattern already fixed once elsewhere in `main.cpp`.
- **`misc-include-cleaner` findings that leaked back into `apps/cli/main.cpp` and
  `commands/analysis.cpp`** after the CLI command-group extraction (both predate that move and
  were never revisited for their own include lists) are fixed, keeping the `static-analysis` CI
  leg green.

### Changed

- **The CI coverage gate now measures the whole library, per component.** The `coverage` leg
  previously instrumented and gated the codec core (now `src/forge`) alone; it now instruments
  every library component —
  `ac3::forge`, `ac3::audio`, `ac3::signing`, the Matroska/MP4/MPEG-TS writers, the C API, and
  the opt-in ADM module plus its bridge — and gates statement (line) and branch coverage per
  component via the new `tools/checks/coverage_report.sh`, so a regression in a small module can no
  longer hide inside a blended number. `src/forge`'s own floor rose from 80%/70% line/branch to
  88%/78% to track the suite's growth, and the first whole-library measurement put honest floors
  under two thin spots — `src/audio`'s device I/O paths and the C API's E-AC-3 surface — rather
  than leaving them unmeasured. See the script's floor table for every component's numbers.
- **The C API's E-AC-3 surface is now tested, and its coverage floor raised to match.**
  `tests/test_capi.cpp` gained the E-AC-3 half it was missing: substream and access-unit round
  trips across the Annex E tool combinations, dependent-substream and dual mono metadata,
  transient pre-noise hold-back and flush, the decode/encode error mappings, and the NULL-handle
  defaults across the whole opaque-handle surface — all on real multi-frame audio. `src/capi`'s
  measurement moved from 48.4%/27.1% line/branch to 87.8%/79.2%, and its floor in
  `tools/checks/coverage_report.sh` from 42/22 to 82/72 per the table's own calibration rule.
- **Memory use no longer scales with how long a session runs.** The memory-usage optimization
  programme changed how every front end moves audio: the CLI's encode commands stream their
  input and their output (a 3-minute 5.1 encode peaked at 437.8 MiB before the programme and
  9.3 MiB after; decode 217 → 28.5 MiB; `spdif` — whose IEC 61937 payload runs at the 4×
  carrier rate — 225.7 → 18.0 MiB; an hour of `eac3-silence` 205 → 8.7 MiB), and every
  output-producing command holds keep-partial and error semantics exactly as before, verified
  byte-for-byte against pre-change binaries in every case. GUI recordings now stream to disk as
  they encode for the containers whose format permits it (elementary, Matroska, MPEG-TS, the
  IEC 61937 carrier), so a crash partway through a recording no longer loses the audio already
  captured. The WASM demo gained real memory ceilings and reports an out-of-memory error instead
  of the tab being killed.
- **The codec's own per-frame allocation churn is down 85–88 % on encode and 54–61 % on
  decode.** Working buffers that were freshly allocated every 32 ms frame — the exponent
  strategy plan, the coupling work set, the E-AC-3 encoder's whole per-(stream, block) MDCT
  spectrum set, the decoder's AHT and enhanced-coupling stores among them — are now owned,
  reused storage with an every-field reset discipline, bit-exact by construction and verified
  bit-exact in practice (AC-3 encode: 225,028 → 26,778 bytes and 286 → 86 allocations per
  frame on the measured runner). The E-AC-3 decoder's per-substream state moved from
  `std::map`s onto flat 32-slot arrays — the identity key space is exactly [0, 32) — for O(1)
  lookup and zero setup allocations. Every step is recorded on the new memory trend, which now
  gates regressions the same way the timing series always has.
- **`apps/` now holds every platform-facing target, and internal naming matches it.**
  `platform/{cli,gui,wasm,android}` moved to `apps/{cli,gui,wasm,android}`; `src/lib` (the codec
  core) is now `src/forge`; `src/adm_bridge` is now `src/admbridge`; and `ac3::audio`'s former
  three-way split (`ac3::platform`/`ac3::capture`/`ac3::sinks`) retired in favour of one
  consolidated `ac3::audio` namespace and header tree. None of this is installed/public surface
  except where called out separately below, so it only affects building from source, not an
  existing library consumer.
- **`apps/cli/main.cpp`'s single ~6,100-line file is being broken into one file per command group
  under `apps/cli/commands/`.** The shared parsing/I/O/metering support layer, the `src=`/`map=`
  multi-source subsystem, and the container-wrapping, audio-hardware, synthetic-signal-generator,
  Atmos, and real-material-encode command groups have moved out so far, each verified with a full
  rebuild and the whole test suite; `main.cpp` itself is down to 1,763 lines, with the decode and
  level/loudness/spdif command groups still to move. The command dispatch table
  (`kCommands`) — the thing that keeps an argv index from ever being silently wrong — is untouched
  throughout.
- **Build- and test-tree hygiene**: `scripts/` and `tools/` merged into one
  `tools/{checks,generators,references,ci}/` convention; the six top-level `requirements-*.{in,txt}`
  files moved into `requirements/`; `tests/` regrouped from ~53 flat files into subdirectories
  mirroring `src/forge/include/ac3/<namespace>/`'s own granularity, folding in a stalled
  platform/CRT axis split along the way; `CMakePresets.json`'s test and package presets
  deduplicated behind hidden base presets; the `examples/` target's separate output directory (and
  the DLL-copy machinery it required on Windows) removed by building examples alongside the shared
  libraries like every other target already does.
- **The installed CMake export set is now named `forgeTargets`, not `ac3forgeTargets`**, matching
  the bare-component-name convention every other export set here already uses (`matroskaTargets`,
  `mp4Targets`, `mpegtsTargets`, `capiTargets`) — it was the one export set named after the whole
  package instead of its own component. Anything referencing the old `ac3forgeTargets.cmake`
  filename directly (rather than going through `find_package(ac3forge)`, which needs no change)
  will need updating.
- **[CONTRIBUTING.md](CONTRIBUTING.md) now documents the repository's actual layout rule** — an
  `ac3/<name>/` header prefix means the component depends on `ac3::forge`, a bare `<name>/` prefix
  means it's deliberately codec-blind, and the C API is the one deliberate exception (depends on
  the codec, but isolated as a C surface) — plus the `apps/` vs `src/` split and the per-backend
  directory pattern. The docs site's nav also got a pass: the four data-trend pages now sit
  contiguously, `docs/project/history.md` moved to `docs/history.md` alongside its own nav
  siblings, and `apps/gui/icons/` gained a README marking it as generated output.
- **`static-analysis` now enforces correct header inclusion.** clang-tidy's
  `misc-include-cleaner` check joins the curated set the `static-analysis` CI leg gates: every
  symbol used in `src/forge`, `src/matroska`, and `apps/cli` must have its owning header
  `#include`d directly, not merely reachable through another header's transitive includes —
  closing the gap where a file built only because of what a sibling header happened to pull in,
  and would break the moment that sibling's own includes changed. The first run found 548
  pre-existing findings (538 missing includes, almost all standard-library facades — `<span>`,
  `<vector>`, `<expected>`, `<cstdint>`, and similar — plus a couple of `ac3::` types; 10 unused
  includes); all were fixed mechanically with `clang-tidy -fix` as part of this change and
  verified against a full rebuild plus a clean `ctest` run (615/615) before the check joined the
  enforced baseline. See `.clang-tidy`'s own header comment for the full rationale.

### Known gaps

- The macOS `ac3gui.app` is still not Apple-notarized or code-signed — unchanged from
  0.8.0-beta.2; this release signs artifacts with GPG and attests provenance via Sigstore/OIDC,
  neither of which satisfies Gatekeeper. Expect a "developer cannot be verified" prompt on first
  launch.
- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has been confirmed against real bitstreaming hardware on
  ALSA only, via a Raspberry Pi 4B to a real Atmos-capable AVR over HDMI (see
  [docs/platforms/raspberry-pi.md](docs/platforms/raspberry-pi.md); this record corrected
  post-release once that validation's own docs were reconciled). WASAPI exclusive mode, PipeWire
  and CoreAudio remain unconfirmed against real bitstreaming hardware on any platform.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.8.0-beta.2] - 2026-08-19

Eighth tagged release. `ac3gui` builds and packages on macOS for the first time — every
platform's release archive now carries a real GUI, not just Windows/Linux's — plus Python
bindings on PyPI and a C API over the encode/decode core.

### Added

- **Python bindings (`ac3forge` on PyPI)**, roadmap F2: a pybind11 module bound directly onto
  `ac3::FrameEncoder`, `ac3::FrameDecoder`, `ac3::Eac3Decoder` and `ac3::oba::AtmosEncoder` —
  numpy-friendly PCM, Python exceptions in place of `std::expected`. `.github/workflows/wheels.yml`
  builds wheels for Windows, macOS and Linux via `cibuildwheel`; publishing to PyPI itself is
  wired up but stays off until a maintainer provisions PyPI trusted publishing — see
  [docs/releasing.md](docs/releasing.md#publishing-to-pypi). See
  [docs/library/python-api.md](docs/library/python-api.md).
- **A C API over the encode/decode core** (roadmap F1), for consumers that can't link C++23
  directly.
- **`ac3gui` now builds, tests and packages on macOS.** The `macos-llvm` CI leg was CLI-only
  since it was promoted out of experimental; it now installs Homebrew's `qt` formula and builds
  the GUI the same opt-in way the four Linux legs do, `ac3gui_qmltests` and a headless
  `ac3gui --smoke` included, and this release's `ac3forge-0.8.0-Darwin.dmg` carries `ac3gui.app`
  for the first time. Getting there needed two real fixes for hangs under Qt's offscreen platform
  plugin, not just turning the option on — see
  [docs/platforms/macos.md](docs/platforms/macos.md#gui-on-macos).
- **A Homebrew Cask for `ac3gui`** is staged at `packaging/homebrew/Casks/ac3gui.rb`, alongside
  the existing CLI-only Formula — a Cask, not a Formula, being the right shape for a prebuilt
  `.app`. Not yet published to the `homebrew-ac3forge` tap; see
  [docs/releasing.md](docs/releasing.md#homebrew-formula-and-cask).

### Known gaps

- The macOS `ac3gui.app` is not Apple-notarized or code-signed — this release signs artifacts
  with GPG and attests provenance via Sigstore/OIDC, neither of which satisfies Gatekeeper.
  Expect a "developer cannot be verified" prompt on first launch.
- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA, PipeWire or CoreAudio.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.8.0-beta.1] - 2026-08-17

Seventh tagged release. The repository moved from `iainchesworth/ac3forge` to
`iainchesworthlabs/ac3forge`; this release cuts over to the new location and closes out
everything left stale by that move. No AC-3/E-AC-3/Atmos codec or CLI/GUI behavior changed.

### Added

- **CI can build on a self-hosted runner when one is actually online and idle**, per OS, falling
  back to GitHub-hosted otherwise — never as an all-or-nothing switch, and never for fork PRs,
  which always stay on GitHub-hosted regardless of runner availability. See
  [docs/ci-self-hosted-runners.md](docs/ci-self-hosted-runners.md) for the live-check and
  override design.

### Fixed

- **The published docs site was about to go stale at its own URL.** GitHub's repo-transfer
  redirect covers `github.com/<owner>/<repo>` paths (blob/tree/actions/releases), but the default
  GitHub Pages URL is owner-scoped with no such redirect — `iainchesworth.github.io/ac3forge`
  would 404 once this repo's `gh-pages` branch (now under `iainchesworthlabs`) next deployed.
  Docs now publish to and link from `iainchesworthlabs.github.io/ac3forge`.
- **Dependabot auto-merge silently stopped working after the transfer.**
  `dependabot-auto-merge.yml`'s repository guard hardcoded the pre-transfer
  `iainchesworth/ac3forge` slug; since `github.repository` now reports
  `iainchesworthlabs/ac3forge`, the job's `if` condition never matched, so no Dependabot PR
  auto-merged since the move.
- Roughly 40 hardcoded `iainchesworth/ac3forge` repo-path links across docs,
  README/ROADMAP/CONTRIBUTING/SECURITY, `mkdocs.yml`, and the vcpkg portfile updated to
  `iainchesworthlabs/ac3forge`. PR/issue references that predate the transfer
  (`docs/wasm-demo.md`'s `#168`/`#169` links) were deliberately left as-is — GitHub's redirect
  still serves them, and rewriting would misrepresent when they were filed.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA or PipeWire.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.7.0-beta.1] - 2026-08-17

Sixth tagged release. The main change is an AC-3 quality push: three independent fixes to the
encoder's bit allocation — weighing delta segments against their own cost at every layout, raising
`dbpbcod` past the spec's own recommendation, and giving the LFE its own fine SNR offset instead
of a shared one — move the 5.1 landscape leg at 448 kbit/s from 2.98 dB behind FFmpeg 8.0.1 to
0.72 dB ahead of it, with perceptual quality unchanged. Finding and fixing those relied on new
verification infrastructure landing alongside them: a fuzz harness over the encoder's own input
space (as opposed to only the decoder's), an opt-in encoder/decoder mirror self-check, and a codec
matrix now driven by real programme material rather than synthetic tones — which is what caught a
stale coupling-channel delta cursor and a frame-ending mid-delta bug that had escaped every
existing gate. Also landing this release: a native PipeWire audio backend for Linux, a shared app
icon and About dialogs across every GUI surface, and an E-AC-3 `auto` tool set that picks
coupling/spectral extension/AHT from the per-channel bitrate instead of taking on/off flags as
given.

### Added

- **An opt-in AC-3 encoder/decoder mirror self-check (`ac3::verify`)**, which decodes every frame
  the encoder just emitted with this project's own decoder and diffs the decoder's model against
  the encoder's own — per-block bit offset, decoded exponents, bit allocation and delta correction.
  Motivated by a bug where `deltbaie == 0` was written to mean "no delta this block" instead of
  §5.4.3.47's "keep the previous block's" — the decoder kept a stale correction, mantissa fields
  were then sized differently on each side, and the failure surfaced two blocks later as an
  exponent walking outside 0..24, misdirecting the investigation into the wrong file entirely. The
  self-check catches that class of bug structurally, at the block where the two models first part
  company, rather than at whatever `§7.10.2` guard the misaligned bits happen to trip first. Off by
  default (`EncoderConfig::trace`/`DecoderConfig::trace` are null pointers, costing one branch per
  block and no allocation); `ac3::verify::MirrorEncoder` drives the encode-decode-compare loop for
  a caller that wants it. AC-3 (`FrameEncoder`/`FrameDecoder`) only for now — E-AC-3 computes its
  delta bit allocation once per frame rather than carrying it block to block, so it is not exposed
  to this specific bug class, and Annex E's dependent-substream/transient-pre-noise holdback
  machinery would need its own instrumentation design rather than reusing this one as-is.
- **A property/fuzz harness over the AC-3 encoder's own input space**
  (`tools/fuzz_encoder_space.py`). Every fuzzing target this project had mutates an
  already-encoded bitstream, which asks whether the *decoder* survives corrupt input; the codec
  matrix walks a hand-enumerated list of command lines against one bootstrap tone. Neither has
  any notion of option *combinations*, and neither varies the input material. This one draws
  random legal encoder configurations crossed with adversarial PCM whose character can change
  part-way through a frame — which is what drives exponent-run splits, block switching and the
  delta bit allocation — then holds every resulting stream against both this project's decoder
  and FFmpeg's strict decode. Motivated by the `deltbaie` defect below, which produced streams
  both decoders reject and escaped every existing gate; reverting that fix, the harness finds
  rejected streams within seconds. Runs bounded on every pull request (in the FFmpeg-oracle
  job) and deeper nightly, mirroring how `fuzz.yml` already splits short from nightly.
- **A new `auto` E-AC-3 tool set, which picks coupling/spectral extension/AHT from the
  per-channel bitrate** instead of taking the on/off flags as given. Every Annex E tool trades
  waveform fidelity for a band it can describe more cheaply than it can code, so each is a win
  below some rate and a loss above it — `auto` applies the measured crossovers (56 kbit/s per
  channel for spectral extension; `12 + 14n` for coupling, whose saving scales with how many
  channels share the band). It still honours an explicit `cpl:N`/`spx:N`/`aht:N` band-edge pin,
  so geometry stays steerable without taking over the decision.
- **A native PipeWire audio backend for Linux** (`src/audio/src/platform/pipewire/`,
  `AC3FORGE_WITH_PIPEWIRE`), selected via pkg-config when ALSA's headers are not present.
  Live capture and monitor playback are genuine `pw_stream` PCM; IEC 61937 bitstream passthrough
  negotiates PipeWire's own compressed-format API for real
  (`SPA_MEDIA_SUBTYPE_iec958`/`spa_format_audio_iec958_build()`/`PW_STREAM_FLAG_EXCLUSIVE`), but
  depends on the target node's `iec958Codecs` having been enabled by the session manager, which
  is outside this library's control — see `src/platform/pipewire/passthrough.cpp` and
  `docs/building.md`'s "Why ALSA still comes first" for the full account, including why ALSA
  keeps precedence over PipeWire when both are present.
- **A shared app icon and About dialogs across every GUI surface.** One procedurally-generated
  mark (`assets/icon/generate_icons.py`, Pillow-based, plus a matching hand-authored SVG) now
  backs `ac3gui`'s window/taskbar icon and packaged `.exe`/`.app` icon, Shield's launcher icon and
  Android-TV Leanback banner, and the WASM demo's favicon. `ac3gui` gained an About dialog and
  Shield an About screen (reached via the TV remote's Info button), both showing real build
  version/git provenance through the existing `ac3::version_details()`, alongside a GPLv3 notice
  and font attribution.

### Changed

- **The AC-3 encoder now gives the LFE its own fine SNR offset instead of copying the one every
  other channel gets.** The bitstream carries a separate `lfefsnroffst`, but this encoder wrote
  the shared value into it, which left the LFE a price-taker in a search it cannot influence: the
  offset search picks the one value at which the frame's *total* mantissa cost fits, and that
  total is set by channels of about 250 bins each. The LFE's 7 bins are rounding error in that
  sum, so its precision was decided entirely by channels 36 times its size — and it lost
  precision at the same rate as them despite costing a fraction as much to serve. Raising only
  its own field by 4 fine steps moves about 12 bits per frame at 448 kbit/s and leaves the
  frame's total mantissa cost unchanged. Measured on two materials (the 5.1 fixture and the
  synthesized full-band decorrelated 5.1) at 192/256/320/384/448/640 kbit/s: LFE SNR up at every
  point, by as much as 5.7 dB, overall SNR never lower, ViSQOL MOS flat.
- **The AC-3 encoder now weighs delta bit allocation against what it costs at every layout, not
  only when coupling is active.** A delta segment is 12 bits of side information taken from the
  same budget that would otherwise buy a higher composite SNR offset, so the encoder already
  re-ran its offset search with delta cleared and kept whichever pass came out higher — but only
  when a coupling channel existed, because that is where a failing test first exposed it. Nothing
  in that reasoning is about coupling, and the layouts that never couple were the ones paying
  most: 5.1 at 448 kbit/s was emitting about ten segments per block, 724 bits per frame, 5% of
  the whole frame. On the 5.1 reference this is worth 0.7 dB.
- **The AC-3 encoder raises `dbpbcod` from the §8.2.12 recommendation of 2 to 3.** `dbpbcod` sets
  the knee below which §7.2.2.5 lifts a band's excitation, so raising it steers bits away from
  bands holding almost no energy and towards the ones that do. Measured on three materials
  (the 5.1 and stereo fixtures and the synthesized full-band decorrelated 5.1) at 192/256/320/
  384/448/640 kbit/s, it improves SNR in every case — by 5.9 dB at 192 kbit/s on the 5.1
  reference, where there are fewest bits to misplace — with ViSQOL MOS flat or better throughout.
  The other four parameters are unchanged: `floorcod` turns out never to bind, and `fgaincod`,
  though worth more still at high rates, regresses at 192 kbit/s.
- Together with the LFE exponent fix below, these move the AC-3 5.1 landscape leg at 448 kbit/s
  from 36.02 dB to 39.71 dB — from 2.98 dB behind FFmpeg 8.0.1 to 0.72 dB ahead of it — with MOS
  unchanged at 3.67. The three are independent and were each measured separately: the delta cost
  check and `dbpbcod` account for 39.13 dB between them, and the LFE fix adds the remaining
  0.58 dB on top.
- **Coupling is now dropped, rather than moved down in frequency, when spectral extension leaves
  it no room.** §E3.3.1 derives the coupling end frequency from `spxbegf`; when that landed below
  the requested `cplbegf` the encoder used to slide `cplbegf` down to meet it, which silently
  coupled from 8.0 kHz where the rate model had asked for 10.2 kHz and made every coefficient
  above 8.0 kHz parametric. On the stereo reference at 192 kbit/s this was worth 6.8 dB of SNR
  (21.6 → 28.5 dB with all tools forced on).
- **The landscape comparison now reports `auto` rather than a forced `all`.** The headline number
  is meant to be what a real user of this encoder gets, the same standard applied to FFmpeg's and
  DEE's own automatic choices; `all` was a configuration this encoder would never itself choose.
  Against FFmpeg 8.0.1 the E-AC-3 stereo leg moves from −11.19 dB to −0.83 dB, and the 5.1 leg is
  unchanged at +0.49 dB.
- **The landscape page shows SNR, LSD and MOS side by side, each with its own vs-FFmpeg/vs-DEE
  delta.** These tools trade waveform fidelity for banded envelope fidelity deliberately, so a
  single-metric headline reported a working tool as a straight loss.
- **The quality landscape page (`docs/landscape.md`) now shows a spectrogram alongside its
  SNR/LSD/MOS numbers** — one stacked original/ac3forge/FFmpeg/DEE image per tracked leg,
  refreshed each release promotion, so there's a visual reference next to the trend numbers, not
  only figures.
- **The CI quality gate now includes an AC-3 5.1 leg.** It was stereo-only, which left the LFE and
  the full channel count with no absolute gate — two separate faults have now shipped through that
  hole. The floor is deliberately loose: the gate decodes with FFmpeg under `-xerror`, so a
  malformed frame fails it as a hard decode error, which is the failure mode both faults had.
- **A new `tools/check_ac3_allocation.py`** reports per-channel and per-band SNR against FFmpeg at
  a matched bitrate, to say *which* part of an allocation gap is worth chasing rather than only
  that one exists. It is what found the LFE fault below.
- **The AC-3 codec matrix (`scripts/run-codec-matrix.sh`) now sweeps real programme material,
  not only synthetic tones.** A stationary sine keeps near-identical exponents in every block, so
  a defect that only appears at a mid-frame exponent-run boundary — exactly the shape of the
  `deltbaie` bug below — was structurally unreachable at any bitrate or layout. The golden
  stereo/5.1 fixtures now run the full encode sweep too, decoded by both this project's decoder
  and FFmpeg's strict decode.

### Fixed

- **AC-3 encoder: a delta bit allocation that ended part-way through a frame produced an
  undecodable stream.** `deltbaie = 0` means "keep the previous block's delta bit allocation",
  not "no delta" (A/52 §5.4.3.47), so a channel whose exponent run stopped wanting a correction
  mid-frame was never told to drop it. The decoder kept applying the stale correction, its bit
  allocation diverged from the encoder's, and every field after that point was read at the wrong
  bit offset — a stream both this project's decoder and FFmpeg reject. Real material hit this at
  several bitrates, 64 and 96 kbit/s stereo among them. E-AC-3 was unaffected.
- **AC-3 encoder: the LFE sent one exponent set per frame however much its level moved.** A
  frame's exponents are the per-bin minimum across the blocks they cover, so a single set for six
  blocks is a set chosen by the loudest of them and every quieter block was then quantized
  against a scale meant for something louder. §5.4.3.15 makes `lfeexpstr` a single bit, and the
  encoder was reading that bit as though it could only ever say "reuse". On the 5.1 reference the
  LFE moves 10–16 dB inside one frame, which cost 12 dB of LFE channel SNR — 56% of the whole
  encode's noise power, on a channel carrying a third of its signal. Worth +0.3 to +3.8 dB
  overall across 192–640 kbit/s (+1.6 at 448), for 18 bits per refresh against a 14336-bit frame.
  Stereo is unaffected, having no LFE.

- **AC-3 coupling channel: delta bit allocation could push corrections past band 50, or land
  them somewhere the decoder never reads.** `choose_delta_segments()` and
  `compute_bit_allocation()` (`src/lib/src/core/bitalloc.cpp`) both started their §7.2.2.6 delta
  band cursor at band 0 regardless of which band a channel's own allocation starts at — harmless
  for fbw/LFE (start band 0), but the coupling channel starts higher, so a literal band-0 cursor
  either overshoot band 50 or wrote corrections into mask bands the coupling channel's own
  allocation never reads. Both FFmpeg and Dolby's own reference decoder require the cursor to
  start at the channel's own start band instead; this project's decoder shared the encoder's
  reading, so the round trip never noticed. Found by the encoder input-space fuzz harness above.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA or PipeWire — the new PipeWire path additionally depends on the
  target node's `iec958Codecs` having been enabled by the session manager, which is outside this
  library's control.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.6.0-beta.1] - 2026-08-17

Fifth tagged release. The main change is Atmos object *decode*: earlier releases could only
encode object audio, and decoding an Atmos file just played its 5.1 bed. The E-AC-3 decoder now
reads OAMD object positions and reconstructs JOC object audio, surfaced through the CLI's
`decode`/`monitor` commands, a new GUI object inspector, and a browser-based WASM demo that
renders real decoded object motion and solos individual object audio. A companion
`verify-objects` mode checks a stream's own EMDF authenticity tag (not Dolby's proprietary
decoder gate — see Known gaps).

Also landing this release: a standalone BW64/RF64 + Audio Definition Model (ADM) reader that
drives a real professional ADM BWF master straight through to a DD+ JOC E-AC-3 stream; two new
container writers — MP4/ISOBMFF (with fragmented MP4/CMAF segmenting plus HLS/DASH signaling)
and MPEG-2 Transport Stream — alongside the existing Matroska writer; full ITU-R BS.1770/EBU
R128 loudness metering and a bitstream-aware delivery-QC command; Raspberry Pi (arm64 Linux) and
a real macOS CoreAudio backend; and the library is now installable through vcpkg.

### Atmos object decode

- **The E-AC-3 decoder reads OAMD object metadata and reconstructs JOC object audio**, closing
  the gap where only the encoder side supported objects. `ac3cli decode`/`monitor` surface the
  decoded object layer directly (including per-object WAV export via `objects_dir`).
- **A new GUI "Inspect objects…" dialog** plays back a decoded Atmos stream's object positions
  and lets you solo individual objects' audio.
- **A browser-based WASM demo** renders real decoded object motion and solo-plays real isolated
  object audio, entirely in-browser.
- **`ac3::signing` gained stream verification** (`verify_atmos_frame`/`verify_atmos_stream`, CLI
  `verify-objects`): checks a stream's own embedded EMDF authenticity tag. This is opt-in and
  separate from Dolby's own decoder gate — see Known gaps.
- **The E-AC-3 decoder now applies dynamic range control** (`drc=`/`heavy`), matching the legacy
  AC-3 decoder; previously accepted and silently ignored.

### ADM ingest

- **A standalone BW64/RF64 + Audio Definition Model parser** (`ac3adm::ac3adm`) reads a
  professional ADM BWF master's object graph into memory, and a bridging layer maps it onto the
  Atmos object encoder's input shape.
- **`ac3cli atmos-adm`** drives both together end to end: a real ADM BWF master straight to a
  DD+ JOC E-AC-3 stream. This module is opt-in (`-DAC3FORGE_BUILD_ADM=ON`, off by default) since
  it needs several Boost header libraries; see [docs/library/index.md](docs/library/index.md).

### Delivery containers

- **A standalone MP4/ISOBMFF container writer**, with a spec-correct `dec3`/`dac3` box, plus
  fragmented MP4/CMAF segmenting and HLS/DASH manifest signaling.
- **A standalone MPEG-2 Transport Stream container writer.**
- **Live capture sessions can mux straight to Matroska.** The GUI's Format tab and the CLI both
  gained the new container options.

### Loudness & delivery QC

- **Full ITU-R BS.1770-4/EBU R128 metering**: momentary and short-term loudness, loudness
  range, and true peak.
- **`dialnorm=auto` finished for multi-source assignments and dual-mono streams**, in both the
  CLI and GUI (dual-mono measures each channel independently).
- **A new CLI `qc` command and GUI QC dialog** audit an already-encoded stream's bitstream-level
  loudness against its embedded metadata and delivery gates.
- **A perceptual-quality (ViSQOL) column** sits alongside SNR in the quality-comparison tooling.

### Platform & packaging

- **Raspberry Pi (arm64 Linux)** is now a supported platform, Pi 4/5 tier (Pi 3 out of scope on
  real-time budget grounds).
- **A real macOS CoreAudio backend** for live capture/monitor playback.
- **The library is installable via vcpkg** (staged in-tree pending submission to the curated
  registry — see [docs/releasing.md](docs/releasing.md#vcpkg-port)): `ac3::forge` plus
  `matroska`/`mp4`/`mpegts` as opt-in container-writer features.

### Fixes

- **AC-3 decode's reported dynamic-range floor was wrong whenever the true minimum sample was
  exactly 0.0 dB** — an accumulator seeded at 0.0 instead of the first real sample silently
  widened the reported range.
- **A flushed E-AC-3 dependent substream (e.g. a height-only pair at end of stream) could crash
  the CLI decoder** instead of writing correct audio, when its channel layout didn't match the
  program's main substream.
- **`fast-mdct=off` is now honored consistently** across all `eac3-encode`/`eac3-encode-multi`
  commands.
- **Piping CLI output to `-` no longer risks corrupting stdout** when `dialnorm=auto` or a
  multi-source summary is printed.
- **The GUI's auto-monitor preference now actually takes effect** on the input rail's Add
  button.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware: DD+ JOC gates
  that on an authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this
  project ships no key for. `verify-objects` checks a stream against its *own* signature, not
  Dolby's gate. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.5.0-beta.1] - 2026-08-15

Fourth tagged release. The main change is a fast-transform performance initiative: an opt-in
FFT-based MDCT was introduced, taken default-on, and then progressively hardware-optimized down
through every transform kernel the encoder touches — the long transform, both block-switched
short transforms, and the opt-in enhanced-coupling tool's DFT — alongside an algorithmic
warm-start for the bit-allocation rate-control search. Measured on the same 5950X release build
throughout, default 5.1 encoding drops from 0.4.0-beta.1's ~3.0 ms/frame to ~0.47 ms/frame (about
6.4×) and 8-object Atmos from ~4.8 ms/frame to ~0.43 ms/frame (about 11×), with SNR held at
+0.000 dB against an independent FFmpeg oracle at every step along the way. Alongside the
performance work: two GUI fixes (object-drag losing its mouse grab mid-gesture, and ambiguous
plan/elevation axis labeling), a quality-trend dashboard fix, and Linux packaging now ships real
`libFOO`/`libFOO-dev`-style system packages instead of one `.deb`/`.rpm` silently bundling the
CLI together with the entire library SDK.

### Performance: fast transforms, default-on and hardware-optimized

- **A new FFT-based fast forward MDCT**, landed opt-in behind `fast_mdct` (off by default): the
  §7.9.4 N/4-FFT structure replaces the direct §8.2.3.2 O(N²) evaluation for the long transform,
  ~25× faster at the kernel level (76.8 µs → 3.1 µs/call) with the direct form kept in-tree as
  the permanent reference/validation oracle. Verified bit-identical-class agreement (peak-relative
  ~3e-15) against the direct form on goldens, random data and real audio, plus **+0.000 dB**
  through an independent FFmpeg oracle at 192–448 kbps.
- **The inverse transform and enhanced coupling's windowing step got the equivalent fix**: `std::cos`/
  `std::sin` calls inside `imdct512_windowed`, `imdct256_pair_windowed` and `ecpl_channel_spectrum`'s
  windowing loop, previously recomputed fresh every call, are now one-time tables. Bit-exact by
  construction (the naive periodic-index shortcut is provably *not* bit-exact for the IMDCT's
  un-reduced angles — documented as a trap so it isn't re-attempted). A real 5.1 E-AC-3 decode
  drops from ~640 ms to ~145 ms (~4.4×).
- **The fast MDCT is now the default everywhere**, with `band_energy` (Atmos's JOC reconstruction
  solve) wired through the same flag — the gap that had capped Atmos's win at ~2.0×. Whole-frame:
  plain 5.1 3.0 → 0.67 ms/frame (~4.5×), 8-object Atmos 4.8 → 0.64 ms/frame (**~7.6×**, up from
  ~2.0× before `band_energy` rode the flag). `fast-mdct=off` (AC-3 commands) / `tools=nofastmdct`
  (E-AC-3) force the direct form back; the old opt-in spellings still parse as no-ops so existing
  run history keeps working.
- **The fast MDCT kernel itself closed to its standalone-prototype speed** (3.09 µs → 903 ns/call,
  a further 3.4×) by moving every angle-dependent value in the §7.9.4 fold — pre/post twiddles and
  the FFT's own butterfly twiddles/bit-reversal — into one-time tables, and switching the FFT to
  split real/imaginary arrays so the auto-vectorizer can see the butterfly's independent
  multiply-add chains.
- **Both block-switched short transforms get their own fast folds**, closing the last kernels still
  running direct-form O(N²) sums under the default `fast_mdct`. Each derives to the same scaled
  DCT-IV core the long transform already uses (877 ns/call vs. 35.8 µs direct — ~41×), removing the
  worst-case real-time hazard on transient-heavy material: a fully block-switched 5.1 frame's
  transform stage drops from ~1.3 ms-class to ~32 µs-class.
- **The opt-in enhanced-coupling tool's `dft512` gets the same FFT treatment** as the long MDCT
  (both now share one `fft_radix2.hpp` core): `ecpl_channel_spectrum`, still the single most
  expensive kernel measured, drops from 277 µs to 47 µs/call (~5.9×). Not run by any default
  encode, but a real-time hazard whenever `ecpl` is enabled.
- **The bit-allocation rate-control search now warm-starts from the previous frame's converged
  offset** instead of a fixed bracket, exploiting that consecutive frames of real material converge
  to the same or a neighbouring value. A stationary frame's ~11 full bit-allocation evaluations
  drop to 2–3; whole-frame time falls a further 18% (5.1) / 11% (Atmos) on top of the kernel work
  above. Brute-force verified against the plain binary search over 4,355 monotone-predicate cases
  with zero mismatches; outputs are byte-identical on every monotone path, and the one path where
  they can legitimately differ (AHT's locally non-monotone cost function) was already
  probe-order-dependent before this change — decoded PCM agrees at 102–115 dB SNR per channel.
- **New performance observability**: Tracy zones across every previously
  unzoned encoder stage, a standalone `ac3kernelbench` micro-benchmark harness timing kernels in
  isolation against real audio, and a per-kernel trend history (non-gating, `::warning::`-only)
  alongside the existing whole-frame performance trend — see
  [docs/performance-trend.md](docs/performance-trend.md).

### Packaging

- **Linux `.deb`/`.rpm` now ship a real `libFOO`/`libFOO-dev` split** instead of one package
  silently bundling `ac3cli` together with the entire library SDK (headers, static archives, the
  CMake package config — confirmed against real `dpkg-deb -c` output, not assumed). `libac3forge0`
  carries just the versioned shared library a linked binary loads at runtime; `libac3forge-dev`/
  `ac3forge-devel` carries everything a builder needs, version-pinned to its exact matching
  `libac3forge0`. Installable with a plain `apt install`/`dnf install` rather than a manual archive
  download — see [docs/releasing.md](docs/releasing.md#what-gets-published). ZIP/TGZ downloads are
  unaffected: `library`+`libruntime` still merge into one `ac3forge-dev-*` archive, exactly as
  before.

### GUI fixes

- **Object-drag no longer loses the mouse grab mid-gesture.** The Objects tab's plan/elevation/
  live-session `MouseArea`s sit inside a `Flickable`-based `ScrollView`, which could steal the grab
  from a child `MouseArea` once movement looked flick-like — most reproducible on the elevation
  view's vertical drag, the same axis `Flickable` watches for scrolling. `preventStealing: true`
  on all five affected `MouseArea`s holds the grab for the whole gesture.
- **The plan and elevation views in the Objects tab are now labeled as what they are** — "(top-down)"
  / "(side-on)" headers, a one-line caption naming which screen axis maps to which room axis, and a
  corrected elevation hint ("drag: depth + height" rather than "drag for height", since the plan
  view's marker moves too during an elevation drag — correct behaviour, previously unexplained).

### Developer tooling

- **The quality-trend dashboard's table no longer conflates unrelated checks.** The chart already
  scoped rows by codec and `isPrimaryCheck`; the table below it rendered the raw, unfiltered record
  list, which let a steady ~25 dB interop fixture read as a crash relative to an unrelated ~68 dB
  series. The table now follows the same Codec scoping as the chart, with a `Check` column and a
  tooltipped `†` marker on non-primary checks.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.

## [0.4.0-beta.1] - 2026-08-14

Third tagged release. The GUI is rebuilt to the canon design handoff — a numbered-rail workflow,
a single assignment table driving all channel routing, an audible timeline with per-source
offsets and motion editing, live capture (including two-device parallel capture with software
clock-drift correction), per-source gain/LFE/resample controls, dual-mono independent DRC,
S/PDIF-wrapped WAV output, four selectable colour palettes with a native system-accent theme, and
a full round of dark-mode fixes. Alongside the GUI work: enhanced coupling's encoder now fits
real angle/chaos coordinates instead of sending them as zero, the decoder accepts Annex E's
default coupling band structure, the EMDF object signer is a committed clean-room library, eight
new library examples ship, an external-encoder (FFmpeg/DEE) comparison joins the quality
dashboards, and Android release builds sign with a real keystore.

### E-AC-3 encoding and decoding

- **Enhanced coupling's encoder now fits real angle and chaos coordinates**, closing the last
  known gap from 0.3.0-beta.1's enhanced coupling work — it no longer sends angle/chaos as zero.
  Amplitude and angle are solved as an exact 2-variable linear least squares per band (§3.5.5.4's
  reconstruction is linear in the complex gain a band's amplitude/angle pair expresses); chaos is
  chosen by searching its 8 legal codes directly against the decoder's own deterministic
  de-correlation sequence and keeping whichever reconstructs closest to the source, rather than
  estimated from a statistical proxy. Quality on ordinary material is unchanged (a correlated
  signal's best fit lands near angle zero anyway); the case the amplitude-only fit could not
  represent at all — two channels' different content forced into the same narrow coupling band —
  improves measurably, from a ~3 dB floor to ~6 dB, without threatening the coding tool's own
  structural limit on how much a single coordinate per band can ever separate.
- **E-AC-3 stereo (2/0) rematrixing** — the bitstream syntax and decoder undo path have existed
  since 0.2.0-beta.1; only the encoder's own §7.5.3 minimum-power decision was missing, and it
  turned out to need no new logic at all, just the same rule AC-3's own encoder already makes,
  over the same Table 7.25 bands (Annex E only changes how many of the four are active, not their
  boundaries or the rule itself).
- **The decoder now accepts Annex E's legal default coupling band structure (`cplbndstrce=0`)**
  instead of rejecting it with `DecodeError::kUnsupported`. This project's own encoder always
  transmits an explicit band structure, so the default path had only ever been exercised against
  the encoder's own output — decoding FFmpeg's E-AC-3, which legally chooses the default, failed
  immediately. The root cause was a stale assumption that Table E2.12's array needed
  relative-to-`cplbegf` indexing; cross-checked against FFmpeg's own `decode_band_structure()`,
  the table is indexed absolutely from `cplbegf == 0`. A permanent regression fixture (a real
  FFmpeg 8.0.1 encode with nonzero `cplbegf`) now covers this in the gold-reference gate.

### Atmos object signing

- **The EMDF object signer is now a committed, clean-room library (`ac3::signing`)** rather than a
  gitignored overlay. The HMAC-SHA-256 construction and the layout of what gets signed are in-tree
  and dependency-free; the **key** is the only secret and is supplied by the operator at runtime,
  never embedded and never written to disk. `ac3cli atmos` gains `sign-objects` with
  `signing-key=<path>` (or the `AC3FORGE_SIGNING_KEY_FILE` / `AC3FORGE_SIGNING_KEY` env vars);
  signing engages only when both a request and a key are present. The Shield app reads its key from
  a bundled `signing.key` asset written from the `ATMOS_SIGNING_KEY` CI secret at build
  time. See [docs/concepts/object-signing.md](docs/concepts/object-signing.md).

### GUI: canon workbench redesign

- **The desktop GUI is rebuilt to the canon design handoff**, replacing the earlier workbench that
  had drifted from it — the numbered rail (01 Input / 02 Levels / 03 Soundfield), plan strip,
  two-tier bitrate picker, routing strip and command bar now match the handoff, landed via a
  6-agent conformance sweep against the mockup (~70 fixes across CLI parity, run history, timeline
  editing, live-tab truth and guided copy).
- **A single assignment table now drives channel routing everywhere**, replacing the free-text
  token field that only appeared once a second source was loaded. Each source channel gets one
  destination dropdown (bed position / a new object / programme / nothing); sending a channel to
  an object turns object mode on, fixes the 5.1 bed, and raises the rate to ≥384 kbps atomically.
  In object mode, a channel assigned to a bed position becomes a static object pinned at that
  speaker's azimuth; unassigned channels drop with a named warning, and encoding enforces the
  sixteen-object cap over dynamic + pinned together.
- **Meter and soundfield redraws no longer tear down and rebuild ~30x/second.** The 30 Hz level
  stream previously rebuilt fresh JS arrays (and every delegate) on every tick; meter/soundfield
  models are now layout-keyed and read by index, and encode-progress/object-drag updates are
  coalesced onto the ~30–60 Hz publish cadence instead of flooding the GUI event queue per frame.
- **A real first-run screen, Preferences dialog and honest run history** round out the shell: first
  run synthesizes a bundled 5.1 test signal into a real WAV; Preferences persists via `QSettings`;
  and run history, failure-banner actions, and the live tab now reflect actual encoder/session
  state rather than mockup placeholders.

### GUI: timeline & time model

- **Timeline length is now derived, not fixed** — `max(offset + duration)` over every loaded
  source, rather than a hardcoded 8 s.
- **Each source gets an independent start offset**, settable from a rail numeric field or by
  dragging its clip band, applied as leading silence in both the channel and object encode loops
  and the meter preview — and reproducible on the command line via a new `offset=` CLI token.
- **Keyframes stay programme-absolute when a clip is dragged**; Shift-drag explicitly carries a
  source's object keyframes along by the same delta (clamped at 0), so a plain drag no longer
  silently drags authored motion with it.
- **Zoom (wheel/button, up to 40x) and snap** — ruler-tick and drag-snap tiers at 1 s / 0.1 s / a
  32 ms floor — move together as the view scales.
- **The Preview button is now audible**: it renders every object through the Atmos encoder and
  plays the 5.1 bed back live through the monitor sink, paced in real time with the playhead
  following the audio clock.
- **Object identity is now keyed by (source, channel)** instead of position in the dynamic-object
  list, so reassigning a channel or removing a non-primary source no longer silently migrates or
  destroys motion belonging to a different or surviving channel.
- **`atmos-encode` gains an optional keyframes-file argument**, matching `atmos-path`'s grammar;
  the GUI's "Export paths…" writes that exact format, closing the last gap in object-mode CLI
  reproducibility.

### GUI: live session and two-device capture

- **A live take now streams to disk incrementally** instead of buffering the whole session in RAM:
  an elementary-stream take *is* the growing output file, muxed to Matroska once at a clean stop,
  so a crash still leaves the elementary take behind. An optional raw-WAV safety copy streams the
  untouched captured PCM alongside it.
- **A silence watchdog fails a session ~3 s after a capture device goes quiet**, instead of the
  transport reading "Running" forever against a vanished device, with a "Choose another device"
  recovery action on the resulting failure banner.
- **Live Atmos sessions pre-allocate a fixed object-slot budget** rather than baking the capture
  device's channel count straight into the JOC stream, so objects can be added or reassigned to a
  different capture channel mid-session.
- **Changing the receiver — or toggling passthrough — mid-session now hot-swaps the passthrough
  sink** on the worker thread between frames, without restarting capture or encode.
- **A live session can now pace a second capture device off the first's clock in software.** The
  master device's delivery paces the frame loop as before; the second device is conformed to the
  master's clock via a streaming linear-interpolation fractional resampler and a proportional
  drift-correction servo, since there's no shared hardware clock between two independent capture
  endpoints. Available from the GUI and from `ac3cli`'s new `live capture2=<index>` token, with
  the slave device's measured drift correction visible in the chain's capture cell. A plain
  channel-mode session's bed still comes from the master device alone — there is no principled
  default position to auto-pan a second, independent device's audio into.

### GUI: source gain, metering, and format/output controls

- **Per-assignment gain/trim** on the channel routing table, applied inside the same routing
  matrix that drives encode, meter preview, and fed-channel flags.
- **Source-side metering pips**: a whole-programme, pre-routing peak/RMS reading per loaded file
  source.
- **Resample-on-load**: adding a source at a different sample rate than the primary no longer
  refuses outright — it resamples to the primary's rate via an offline windowed-sinc polyphase
  resampler and labels the row accordingly; the refusal survives only when the primary's own rate
  has no legal AC-3 target at all.
- **LFE low-pass filtering**: a full-bandwidth channel explicitly routed onto LFE through the
  assignment table now runs through a 120 Hz 4th-order Butterworth low-pass in preview and
  channel-encode. Automatic single-source routing (a file's own dedicated LFE channel) stays
  bit-exact.
- **CLIP latches per channel** in the meters — once lit, stays lit until clicked or a new
  transport starts.
- **`objm` fold-to-mono**: the range grammar (`0.1-2:objm`) can now fold a contiguous run of one
  source's channels into a single dynamic object.
- **Dual-mono programmes get independent DRC.** A/52 §7.7.1/§7.7.2.2 give 1+1's two programmes
  independent DRC curves and heavy-compression ceilings, but the encoder was building the second
  programme's controller from the first's own config. CLI gains `drc2=`/`heavy2`/`ceiling2=`/
  `dialogue2=`; GUI gains a Programme 2 DRC combo and a "Heavy compression — programme 2" card.
- **A third container option: S/PDIF-wrapped WAV**, reusing the existing IEC 61937 burst-wrapping
  machinery. Works for both codecs — E-AC-3's carrier runs at 4x rate.
- **An advisory bit-rate floor for wide layouts**: a muted hint under Bit rate when the CBR rate
  works out to fewer than ~77 kbps per full-bandwidth coded channel. A hint, not a gate.
- **Guided now applies measured loudness and film-standard DRC automatically** while it's driving
  and Loudness/Metadata is untouched this session; dual mono gets the DRC-only half of the
  contract on both programmes, since loudness measurement is refused there.

### GUI: guided-mode workflow polish

- **Finished run chips now carry their own Play action**, sending that run's own output to a
  receiver — not whatever the most recent encode happened to produce.
- **Run history now survives a restart.** The last 30 completed runs persist to Settings as JSON;
  clicking a run chip opens a details popover with status, rate, duration, size, frames, failure
  text, and the `ac3cli` command line snapshotted when that run started.
- **Guided's amp destination now auto-picks a bitstream-capable output device** — the first device
  that can carry the prospective encode plan — with a "Choose a different device" override and a
  stated reason when nothing qualifies.
- **Guided's Movement step, once object mode is on, offers two cards**: *Everything moves* (every
  loaded channel becomes an object) and *Keep the bed, add movers* (only claims still-unassigned
  channels).
- **Good/Better/Best now maps to VBR quality, not a fixed bitrate**, when a VBR default or an
  already-selected Variable rate mode applies — Guided's Quality step rate cards set a VBR quality
  target (40/75/90) instead of a CBR number.
- **Preferences defaults apply on Save to untouched fields only**, generalising the existing
  loudness-touched contract to container/rate mode/bit rate/VBR quality.
- **The guided wizard's Back/Next footer no longer disappears off-screen.** It previously shared
  the tab `StackLayout`, whose implicit height is the max over every page — inheriting the Format
  tab's height let the footer stretch a full screen below the visible content. The wizard now owns
  its own surface outside the tab stack: the step bar and footer stay pinned, only the step content
  scrolls between them.
- **The always-on `ac3cli` command bar is now a popover.** Encode runs the encoder in-process, so
  the full command line is reference material, not the primary act: a compact chip opens a popover
  with the complete line, wrapped, with Copy.
- Fixed the runs lane's empty-state text riding the top edge instead of centring in the strip.

### CLI

- **Fixed: a bare `heavy2` token was silently misparsed** as `encode`/`eac3-encode`'s optional
  `in2.wav` positional instead of enabling Ch2 heavy compression — `run_main`'s bare-token
  classifier was missing it alongside `couple`/`heavy`/`mixmeta`/`sign-objects`/`keep-partial`.
- **`keep-partial` token**: a bare trailing-options token that keeps whatever frames
  `encode`/`eac3-encode`/`atmos-encode` already produced before a failure, at
  `<name>.partial.<ext>` — mirrors the GUI's own keep-partial-output preference.

### GUI: theming

- **Four selectable colour palettes, including a native system-accent theme.** *Signal* (the
  design system's red, default), *Ink* (cool greys, cobalt accent), and *Console* (warm greys,
  studio amber) join *System* — a new `SystemTheme` singleton that reads the platform's native
  accent colour and re-announces on OS colour-scheme changes, so changing the OS accent colour
  restyles the running app live. All four are selectable in Preferences → Appearance.
- **Dark mode is now hand-tuned per palette instead of a mechanical inversion of the light ramp.**
  The previous approach turned near-white accent tints into murky red-blacks and left the
  fully-saturated accent glaring against near-black; each palette now defines both modes by hand.

### GUI: dark-mode audit fixes

- **A round of dark-mode fixes found by auditing every tab across all four palettes.** Smoke-mode
  screenshot captures are now hermetic — session restore previously ran at window creation, so a
  screenshot inherited whatever session the last run saved, and closing the smoke binary could
  clobber the user's real saved session with smoke state. The Coding tools tab now explains itself
  instead of rendering a bare void when object mode or plain AC-3 hides its contents. The runs
  lane's hard-capped height had exposed a horizontal scrollbar overlaying the chips and eating
  their clicks — the scrollbar is now off, wheel/drag still pan. The Encode button's `.ac3`/`.ec3`
  suffix no longer goes stale after the codec moves the plan between containers.

### Quality & verification tooling

- **Added an external-encoder landscape comparison against FFmpeg and Dolby DEE**, giving the
  encoder a real point of reference beyond its own gold-reference gate. A new stereo fixture
  exercises coupling, enhanced coupling, spx, AHT, transient pre-noise, and rematrixing together; a
  local-only baseline tool encodes fixed legs through FFmpeg, DEE, and `ac3cli`, while CI itself
  runs a compute-only trend mode scoring against those legs using only this project's own decoder —
  no FFmpeg or DEE invocation at CI time. Results render in two new docs pages,
  `docs/tool-comparison-trend.md` (per-commit, per-variant detail) and `docs/landscape.md`
  (release-over-release headline table). This work directly surfaced the `cplbndstrce=0` decoder
  gap fixed above, and found that the installed DEE build silently drops the Ls channel on discrete
  5.1 input — the affected rows are honestly marked `"status": "unverified"` rather than reporting
  a fabricated score.
- **The gold-reference gate now checks a real Annex E tool-enabled stream (`tools=cpl`)**, not just
  the `tools=none` baseline, at the existing 55 dB SNR floor. `spx`/`aht`/`all` are deliberately
  left off this specific check: those tools are approximate/generative reconstruction where two
  independent spec-correct decoders legitimately diverge much further, so a 55 dB floor would
  false-fail on normal divergence rather than catch a real regression.
- **The quality trend chart and tool-comparison trend chart both gained a per-series breakdown
  view** ("Worst of legs, by branch" / "By platform leg", and "By branch" / "By variant"), so one
  CI leg — or one Annex E tool-set — quietly drifting relative to its siblings is visible as a
  trend line instead of only by scanning table rows.

### Android (Shield)

- **Android release builds now sign with a real release keystore instead of the debug key**, once
  a maintainer has provisioned the `ANDROID_KEYSTORE_*` secrets per
  [docs/releasing.md](docs/releasing.md). Local dev, ordinary CI, and any release run with no
  keystore provisioned all still degrade to the debug keystore exactly as before.

### Bug fixes

- **Windows audio backends no longer list a blank row in the device picker.** A real WASAPI
  endpoint that never fills in its friendly-name property was enumerated with an empty display
  string, and both the capture and passthrough front ends put that straight into a combo box as an
  unlabeled entry. The fix resolves a display name through a fallback chain (friendly name → device
  description → an endpoint-id-carrying stand-in), and an endpoint whose id can't be read is now
  skipped entirely rather than listed.

### Library examples & documentation

- **Eight new `examples/` programs**, each a build target and `ctest` entry like every other
  example: `wav_roundtrip` (real WAV file I/O, not just in-memory PCM), `custom_layout` (a
  channel selection no named `LayoutId` covers, via `Plan::custom_locations`),
  `multi_source_assignment` (combining separate sources via `ac3::plan::Assignment`),
  `scripted_object_motion` (authored `KeyframePath`/`OrbitPath` driving `AtmosEncoder`),
  `object_signing` (`ac3::signing::sign_atmos_stream`, previously undemonstrated),
  `level_metering` (`ac3::analysis::LevelMeter`/`energy_vector`), `decode_robustness`
  (recovering from one damaged frame in an otherwise-good stream via `ac3::split_frames`), and
  `atmos_fallback` (`AtmosConfig::emit_object_metadata`'s objects-or-nothing design decision,
  side by side). Three new library reference pages —
  [Channel plans & routing](docs/library/channel-plans-and-routing.md),
  [File I/O](docs/library/file-io.md) and [Object signing](docs/library/signing.md) — and new
  sections on the existing [Spatial & Atmos objects](docs/library/spatial-and-atmos.md),
  [Decoding](docs/library/decoding.md) and [Muxing & sinks](docs/library/muxing-and-sinks.md)
  pages are written from them.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.

## [0.3.0-beta.1] - 2026-08-11

Second tagged release. Adds the two remaining Annex E coding tools (enhanced coupling,
transient pre-noise processing), a native Android app on NVIDIA Shield TV, packaged
`find_package(ac3forge)` libraries for third-party consumers, explicit multi-source channel
assignment, and a GUI tier split for first-time users through experts.

### E-AC-3 encoding and decoding

- **Enhanced coupling (§E3.5)** and **transient pre-noise processing (§3.7)**, the two Annex E
  tools the decoder previously recognised but refused (`DecodeError::kUnsupported`) — now
  implemented end to end, encoder and decoder, each behind its own tool token (`cpl+ecpl`,
  `tpn`). Enhanced coupling round-trips at the same ~20dB near-transparent bar as standard
  coupling for realistic content; transient pre-noise processing follows the spec's own
  time-scaling synthesis pseudocode, reusing the existing block-switch transient detector rather
  than a second one.
- Fixed two real conformance bugs found implementing the above: a missing §3.3.2 `nrematbd`
  formula for `ecplinu` (both encoder and decoder), and a systematic 2:1 gain error in enhanced
  coupling's FFT-based reconstruction pathway.
- `Eac3Decoder::decode_substream` now returns an optional decoded substream plus a new
  `flush()`, since transient pre-noise processing can hold a frame back until the next one
  confirms whether a correction reaches into it. Streams that never use the tool see no
  behavioural change.

### Dolby Atmos objects and multi-source encoding

- **Explicit multi-source channel assignment** alongside automatic routing — `ac3cli`'s encode
  commands take `src=`/`map=` to assign specific input files/channels to specific output
  channels and objects, instead of relying purely on automatic layout inference.
- Object mode now addresses objects by source, not a stale positional index, so multi-source
  sessions keep object identity stable as sources are added or reordered.

### GUI

- **Guided/Advanced/Expert tier split**: a real step-by-step wizard for first-time users, with
  Advanced and Expert tiers exposing the same controls power users had before.
- Multi-source input and an explicit per-channel assignment surface in the GUI, mirroring the
  CLI's `src=`/`map=`.
- **Dual mono (1+1) as a bed**, not a distinct layout — it now feeds the same object/motion
  pipeline as any other bed.
- **Variable bit rate** as a selectable GUI rate mode (a quality target with optional min/max
  kbps bounds), alongside CBR.
- Live sessions no longer clobber a file's authored objects when a live capture starts, and warn
  before silently dropping VBR settings that don't apply live.
- A Qt Quick Test harness drives the real `EncoderController` end-to-end, not a mock, for GUI
  regression coverage.

### Android (Shield) — new platform

- **ac3forge on NVIDIA Shield TV**: a native Android app (`platform/android/`) pairing
  `ac3::forge`/`ac3::audio` via JNI with a live Atmos demo — authored object trajectories,
  deflection, and ambient object motion, encoded and rendered on-device.
- HDMI receiver resilience hardening for the Shield demo, so a receiver renegotiating format
  mid-playback doesn't drop the session.
- Ships as a debug-signed `.apk` this release — see Known gaps.

### Library and packaging

- **`find_package(ac3forge)` support**: `ac3::forge` and `matroska::matroska` now build as
  proper static and shared CMake targets with `install()`/export support, so a third-party
  project can consume them without vendoring the source tree. `ac3::audio` (live capture/
  monitor/passthrough) stays CLI/GUI-internal, not part of what's installed.
- `ac3::forge` split into a platform-independent codec core plus `ac3::audio`, clearing the way
  for the library package above and for platforms — like Android — that only want the codec.

### Quality and packaging infrastructure

- Quality-trend dashboard redesign (readability, tightened gate thresholds) and a fix for CI
  concurrency dropping quality data mid-run.
- A round of security hardening prompted by OpenSSF Scorecard: hash-pinned CI tool installs,
  commit-SHA-pinned GitHub Actions (replacing tag-pinned ones), a `SECURITY.md`
  vulnerability-reporting policy, patched CVEs in docs dependencies, branch-protection scoring
  wired up, and build provenance republished as `.intoto.jsonl` for Scorecard to read.
- Several MSVC `/analyze` and clang-tidy findings fixed for real: heap-allocating large
  encoder/decoder objects out of worker-thread stacks, reusing MDCT scratch buffers instead of
  stack-declaring them per call, and a couple of static-analysis false-positive suppressions.
- macOS packaging now stays a single `.dmg` bundling both the runtime and library components,
  matching the archive packages' intent — CPack's DragNDrop generator defaulted to splitting
  per component the first time this leg actually ran on real macOS CI, caught by this release's
  own packaging dry run.

### Known gaps

- The Shield `.apk` ships debug-signed via Android's default debug keystore — no release
  keystore is provisioned in this repo yet, so it's a sideload-only build, not one suited for
  store distribution.
- Enhanced coupling's encoder always sends angle/chaos as zero (an amplitude-only fit) — quality
  degrades if two channels' content shares one narrow coupling band. Closed in
  [0.4.0-beta.1](#040-beta1---2026-08-14).
- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.

## [0.2.0-beta.1] - 2026-08-10

First tagged release. ac3forge is a clean-room AC-3 and E-AC-3 encoder and decoder in C++23,
implemented from the published standards — no FFmpeg or other codec library is linked, only
used during development as an independent oracle to check output against.

### AC-3 and E-AC-3 encoding

- Every AC-3 coding mode (1+1 dual mono, 1/0 through 3/2, each with or without LFE) at 48,
  44.1 and 32 kHz, CBR only, across all 19 nominal Table 5.18 bit rates. Exact 44.1 kHz timing
  via Bresenham alternation between the two legal frame lengths.
- E-AC-3, all of the above plus 7.1, 5.1.2, 5.1.4 and 7.1.4 through dependent substreams, and
  either CBR or VBR (a quality target with optional min/max kbps bounds) per substream.
- Per-block, per-channel block switching (§8.2.2 transient detector, long 512-point vs. switched
  256-point transform pairs), automatic delta bit allocation (§7.2.2.6), and 2/0 rematrixing
  (§7.5.3) on AC-3.
- Channel coupling (§7.4 / §E3.3), spectral extension (§E3.6) and the adaptive hybrid transform
  with gain-adaptive quantization (§E3.4) on E-AC-3, each opt-in per stream.
- `fscod2`, Annex E's half sample rates (24, 22.05, 16 kHz).

### Dolby Atmos objects (Joint Object Coding)

- Mono sources placed and moved in 3D space, panned into a 5.1 bed with OAMD + JOC metadata
  carried in an EMDF container (ETSI TS 103 420) — playable as plain 5.1 by any decoder, and
  reconstructible as discrete objects by one that understands the container.
- Authored keyframe paths and closed-form orbits for object motion, both file-driven
  (`ac3cli atmos-path`) and live per-frame (`ac3cli live --atmos`).
- Syntax checked field-for-field against Dolby's own Reference Player and Dolby Media Encoder.

### Decoding

- A single in-repo decoder core shared with the encoder, reading both AC-3 and E-AC-3 —
  dependent substreams, `chanmap`, and the §E3.8.2 render — at float32-precision parity with
  FFmpeg on every layout FFmpeg itself can read.
- All three Annex E coding tools (coupling, spectral extension, AHT) decode individually or all
  stacked together, at every channel layout including 7.1.4 — the one combination FFmpeg cannot
  check at all, since its parser refuses a second dependent substream.
- Block switching and dual mono decode on both formats; decoded switch decisions are reported
  back (`DecodedFrame::blksw`), the same tier of diagnostic as `dynrng`.

### Metadata

- `dynrng` (five DRC profiles: film-standard, film-light, music-standard, music-light, speech),
  `compr` heavy compression, measured `dialnorm` (ITU-R BS.1770-4 gated loudness), and downmix
  levels (`cmixlev`/`surmixlev`, the E-AC-3 `mixmdate` group) — verified against FFmpeg applying
  the metadata, not just against the encoded bits.

### Live audio, capture and passthrough

- WASAPI (Windows) and ALSA (Linux) backends for live input/loopback capture, shared-mode
  monitor playback, and exclusive-mode S/PDIF (IEC 61937) bitstream passthrough — AC-3 and
  E-AC-3/Atmos alike.
- A lock-free SPSC ring carries samples from capture into the encoder; `ac3cli live` wires
  capture → encode → monitor/passthrough continuously.
- `MonitorSink` playback confirmed against real Windows hardware, including a live
  microphone-capture-to-monitor session; ALSA verified headless (WSL2 has no sound devices) plus
  under AddressSanitizer/UndefinedBehaviorSanitizer with leak detection.

### Tools and formats

- `ac3::io::scan`: derives stream format, access-unit boundaries and channel count directly from
  the bitstream.
- `matroska::matroska`: a standalone MKV muxer, independent of the codec library.
- `ac3::sinks::iec61937`: S/PDIF burst packing, byte-exact against FFmpeg's `spdif` muxer for
  AC-3 and independently verified against Microsoft's own IEC 61937 documentation for E-AC-3.
- `ac3::analysis`: peak/RMS/loudness metering with console ballistics and the Gerzon energy
  vector, shared by both front ends.
- `ac3cli`, a 21-command command-line front end, and `ac3gui`, a Qt Quick GUI with file and
  live-capture encoding, an object placement/motion view, and channel-level metering.

### Quality and packaging infrastructure

- CI across Windows (MSVC, clang-cl), Linux (GCC, Clang) and macOS (Homebrew LLVM) — CLI and GUI
  on Windows/Linux, CLI on macOS — plus a dedicated AddressSanitizer+UndefinedBehaviorSanitizer
  leg, clang-tidy static analysis, a coverage gate, a per-platform gold-reference quality gate,
  and an independent FFmpeg-validation leg.
- libFuzzer harnesses over every untrusted-input entry point (stream scanning, both decoders,
  WAV reading), run on every push and nightly with deeper mutation.
- Signed, attested release packages (Windows `.zip`/`.exe`, Linux `.tar.gz`/`.deb`/`.rpm`, macOS
  `.tar.gz`/`.dmg`) with SHA-512 checksums, keyless Sigstore/OIDC build provenance, and an SPDX
  SBOM — see [docs/releasing.md](docs/releasing.md).

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.
