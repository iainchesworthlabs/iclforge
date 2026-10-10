# IAMF reading and writing: `iclforge::containers::iamf`

`iclforge/containers/iamf/`, `iclforge::containers::iamf`, a part of `iclforge::containers`. A standalone reader and writer for AOM's IAMF (Immersive Audio
Model and Formats) v2.0.0 — the bitstream format behind Eclipsa Audio. Like `iclforge::containers::matroska`,
`iclforge::containers::mp4` and `iclforge::iab`, it links nothing from `iclforge::ac3`: it knows nothing about
AC-3, E-AC-3 or the JOC/Atmos object layer, and takes already-rendered PCM in.

**Why a writer exists at all.** IAMF's codec list is Opus, AAC-LC, FLAC and LPCM — E-AC-3 can
never be carried inside it. The channel-based path is therefore a decode → rewrap bridge: a caller
decoding a stream that is already coded as a 7.1.4 channel layout (`iclforge::ac3::plan::LayoutId::k714` — an
independent substream plus two E-AC-3 dependents) gets the 12 discrete channels straight off
`iclforge::ac3::Eac3Decoder::decode_access_unit`; this module needs them permuted into its own channel
order and handed over as PCM. The object-based path (v2.0) takes mono PCM per object and positions.

**Two routes to the same ecosystem.** [ADM / BW64 writing](adm.md)'s `write_bw64()` already opens
an *indirect* one: AOM's own `iamf-tools` encoder accepts ADM-BWF input, so a decoded programme
written as an ADM master already reaches IAMF via a second, external encoder — but
only for the ADM writer's own scope (dynamic-object-only programmes, cartesian positions). `iclforge::containers::iamf`
writes the IAMF bitstream directly, with nothing else in the chain.

Default-on (`ICLFORGE_BUILD_IAMF`), installed/exported the same way as the container writers —
unlike `iclforge::adm`, it has no third-party dependency to opt in around. The vcpkg port and
the Conan recipe install it where asked for, off by default: `vcpkg install iclforge[iamf]`, or
`-o "iclforge/*:iamf=True"` (see [Using the libraries](index.md)).

## Layers

| Header | What it holds |
|---|---|
| `iamf.hpp` | The short route: `mux()` for a 7.1.4 programme, `mux_objects()` for object elements, `mux_coded()` for packets a caller's Opus, AAC-LC or FLAC encoder made (with `opus_decoder_config()`, `aac_lc_decoder_config()` and `flac_decoder_config()`), `build_sequence()`, `build_object_sequence()` and `build_coded_sequence()` for the `Sequence` each would write, `decode_pcm()` to read an element's audio back (with `DecodeOptions` to choose a layer of a scalable one), and `reconstruct_channels()` to rebuild a channel element from substreams a caller's own codec decoded. |
| `model.hpp` | The element graph as plain data: `Sequence` (Descriptors and Temporal Units), `CodecConfig`, `AudioElement`, `MixPresentation`, parameter definitions and `ParameterBlock`, `AudioFrame` with its trimming, `Metadata`. Q7.8 gains and coded positions are kept as the bitstream carries them. |
| `sequence.hpp` | OBU bytes: `write_descriptors()`, `write_temporal_unit()`, `write_sequence()` and their readers `read_descriptors()`, `read_temporal_unit()`, `read_sequence()`. `write_sequence()`/`read_sequence()` are the standalone raw OBU stream. `layout_info()` and `expanded_layout_info()` give the substream order of every loudspeaker layout and expanded layout. |
| `container.hpp` | ISO-BMFF: `write_isobmff()`, `read_isobmff()` (the first IA track), `read_isobmff_tracks()` (every IA track of a file) and `FragmentedWriter`. |

```cpp
iclforge::containers::iamf::AudioTrack track{.samples_per_frame = iclforge::ac3::kSamplesPerFrame};
std::vector<iclforge::containers::iamf::Frame> frames;
// ... frames.push_back(...) for each temporal unit ...

const auto file = iclforge::containers::iamf::mux(track, frames);
if (!file) {
    fmt::printf("iclforge::containers::iamf::mux failed: %.*s\n", static_cast<int>(iclforge::containers::iamf::describe(file.error()).size()),
                iclforge::containers::iamf::describe(file.error()).data());
    return 1;
}
```

Full program: [`examples/mux_iamf.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/mux_iamf.cpp) — the whole
round trip: encode a synthetic 7.1.4 E-AC-3 access unit, decode it with `iclforge::ac3::Eac3Decoder`,
permute the result, and mux it.
[`examples/iamf_objects.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/iamf_objects.cpp) writes
an object-based program as a file, a raw OBU stream and fragments, and reads each back.

## The permutation a caller does (7.1.4)

`iclforge::containers::iamf::Frame::channels` is planar, ordered exactly as this module's Audio Element OBU declares
(IAMF §3.6.2, `loudspeaker_layout` = 7, "7.1.4ch"): **L, C, R, Lss, Rss, Lrs, Rrs, Ltf, Rtf, Ltb,
Rtb, LFE**. A decoded `iclforge::ac3::DecodedAccessUnit::channels` is ordered by Table E2.5 *bit* order
instead (`DecodedAccessUnit::layout`), which is neither this order nor WAV's — so
`examples/mux_iamf.cpp` builds the permutation itself, one `layout.index_of(Location::kX)` call
per IAMF channel:

```cpp
constexpr std::array<Location, 12> kIamf714Order{
    Location::kLeft,   Location::kCentre, Location::kRight,
    Location::kLeftSurround, Location::kRightSurround,
    Location::kLrs,    Location::kRrs,
    Location::kVhl,    Location::kVhr,   // Table E2.5's front-height pair = IAMF's Ltf/Rtf
    Location::kLts,    Location::kRts,   // Table E2.5's rear-height pair  = IAMF's Ltb/Rtb
    Location::kLfe,
};
```

This permutation is not part of `iclforge::containers::iamf` itself — the module stays codec-blind, the same
reason `iclforge::containers::mp4::AudioTrack::codec_config`'s ETSI TS 102 366 payload is built by the *caller*
(`iclforge::ac3::io::build_codec_config_box`), not by `iclforge::containers::mp4` — so it lives in the example, not a bridge
library. There is no IAMF counterpart of `iclforge::adm`'s bridge; the mapping is small,
one-directional, and this is what it looks like.

## What gets written

Every OBU and box field is transcribed from the published IAMF v2.0.0 specification (the structure is
named at each call site in `libs/containers/src/iamf/`), per this project's clean-room rule — `libiamf`, FFmpeg and
AOM's Open Audio Renderer are oracles used to validate the output, never sources this code was
transcribed from.

- **IA Sequence Header OBU**: Simple Profile for the 7.1.4 programme; Base-Advanced for object programmes
  (the profile whose first Mix Presentation references only object-based elements, at most 18 channels).
  `model.hpp` lets a caller set any profile.
- **Codec Config OBU**: `ipcm` at 16, 24 or 32 bits and the sample rates the LPCM decoder config allows.
  `Opus`, `mp4a` and `fLaC` are carried: the model keeps their `decoder_config` bytes, `mux_coded()`
  builds them with their `audio_roll_distance` (see "Carrying Opus, AAC-LC and FLAC"), and a Sequence read
  from a file that uses them reads back and writes again. This module encodes and decodes none of them,
  so `decode_pcm()` reads only `ipcm`.
- **Audio Element OBU**: channel-based (one or more layers), scene-based (mono and projection
  Ambisonics config), and object-based — one Audio Substream carrying one object, coded mono, or two,
  coded as a stereo pair. `mux()` writes one layer of 7.1.4ch: seven substreams, five coupled pairs, the
  centre and the LFE. `layout_info()` gives the substream order of every loudspeaker layout the
  specification lists for a single layer.
- **Mix Presentation OBU**: sub-mixes, rendering config (headphones rendering mode, binaural filter
  profile, element gain offset), element and output mix gains, layouts with `LoudnessInfo()` (true peak,
  anchored loudness, momentary loudness histograms, loudness range), tags and the optional fields. For
  an object-based element the rendering config holds the position parameter definition.
  `AudioTrack::stereo_loudness`/`layout_714_loudness` supply the numbers `mux()` writes; this module
  measures nothing itself.
- **Parameter Block OBUs**: every parameter type — mix gain, demixing, recon gain, the six position types
  (polar, Cartesian 8 and 16 bit, and their dual forms) and momentary loudness — with all five
  animation types, definitions that carry their timing (`param_definition_mode` 0) or leave it to the
  block (1), and sub blocks of constant or listed duration. A block of an unknown `parameter_id` or
  animation type is dropped on read, as the specification says parsers do. A parameter that holds its
  default needs no block, so a static object writes none.
- **Audio Frame OBUs**: one per substream per Temporal Unit, using the compact `OBU_IA_Audio_Frame_ID0..17`
  types for the first 18 substreams and an explicit id beyond them, with the trimming fields
  (`num_samples_to_trim_at_start` and `_at_end`).
- **Temporal Delimiter OBUs**, with `is_not_key_frame`, in a raw OBU stream; an ISO-BMFF IA Sample never
  holds one.
- **Metadata OBUs**: ITU-T T.35 and IAMF tags, in the Descriptors and in a Temporal Unit.
- **Trimming.** `AudioTrack::trim_start_samples` and `trim_end_samples` become Audio Frame trimming (a
  start trim longer than a frame fully trims the leading frames, as the specification requires), shorter
  IA Sample durations and an `edts`/`elst` box. A program whose length is not a whole number of frames is
  padded and the padding trimmed from the last frame.
- **ISO-BMFF** (`write_isobmff()`): `iamf`-branded `ftyp`, one `trak` (6.2.1 stores an IA Sequence as one
  track) whose `stsd` carries an `iamf` `IASampleEntry` wrapping an `iacb` box of the Descriptors, one IA
  Sample per Temporal Unit, an `stss` box when a Temporal Unit is not a key frame, the `roll` sample group
  that 6.2.2 requires of Opus and AAC-LC, and 64-bit chunk offsets and a 64-bit `mdat` size when the file
  needs them (`IsobmffOptions::large_mdat` asks for the long header on a small file).
- **Fragments** (`FragmentedWriter`): an initialization segment (`ftyp` and a `moov` with an empty sample
  table and an `mvex`), then one `moof`/`mdat` per call, each fragment usable as soon as it is returned.
  Non-key Temporal Units carry `sample_is_non_sync_sample` in their sample flags.
- **Raw OBU stream** (`write_sequence()`): the Descriptors then each Temporal Unit, the standalone
  representation, with Temporal Delimiters when the units ask for them.

## What gets read

`read_isobmff()` reads a file from this writer, from `FragmentedWriter`, or from another muxer that
follows the encapsulation: sample tables or `moof`/`trun` fragments (with `default-base-is-moof`, explicit
base offsets and per-sample or default sizes, durations and flags), `co64`, `stss`, 32- and 64-bit `mdat`
sizes, `edts`/`elst` and the `mdhd` timescale. It reads the first IA track; tracks that are not IA tracks
and the fragments of other tracks (matched by `track_ID`) are skipped. `read_isobmff_tracks()` returns every
IA track of a file as its own Sequence, for files that hold several. An IA track protected with Common
Encryption (an `enca` entry whose original format is `iamf`) is recognised and refused as `kUnsupported`,
not mistaken for a file with no IAMF in it. `read_sequence()` reads a raw OBU stream, splitting Temporal Units at Temporal
Delimiters or, without them, where a substream repeats. Both skip redundant copies of the Descriptors,
Reserved OBUs and bytes past the syntax an OBU defines, stop at a second IA Sequence, and fail with
`Error::kTruncated`, `kBadLeb128` or `kBadObu` rather than read past their input; the readers'
allocations are bounded by the bytes that remain.

`decode_pcm()` turns an `ipcm` Audio Element into planar float channels with the trimming applied:
channel-based elements (a single layer in any layout `layout_info()` or `expanded_layout_info()` lists,
or a scalable element of up to six layers, see below), object-based elements, and scene-based elements
in mono mode.

## Scalable channel audio

An Audio Element with more than one layer codes a base layout and, in each further Channel Group, only
what the next layout adds; the decoder rebuilds the rest. `decode_pcm()` and `reconstruct_channels()`
apply the three steps of IAMF section 7.2 to the layer asked for (`DecodeOptions::layer`; the last, full
layout when unset):

1. **Gain** (7.2.1): each Channel Group's `output_gain` is applied, as 10^(output_gain / (20 x 256)), to the
   mixed channels its `output_gain_flags` name.
2. **De-mixer** (7.2.2): S1to2, S2to3, S3to5 and S5to7 for the surround channels, TF2toT2 and T2to4 for
   the height channels, with alpha, beta, gamma and delta from the frame's `dmixp_mode` and
   w(k) from the running `wIdx(k)`. A frame without a demixing Parameter Block uses the definition's
   `default_dmixp_mode` and `default_w`. Which channels a Channel Group holds follows 3.6.2.2 and their
   substream order 3.6.2.3, so a layer list that breaks the generation rule of 3.6.2.1, or whose substream
   counts do not match its groups, is `kBadDescriptor`.
3. **Recon Gain** (7.2.3): the `recon_gain` of the layer's channels flagged in `recon_gain_flags`, smoothed
   with the moving average (N = 7) and the Hann overlap windows of the specification: 60 samples for
   `Opus` and 64 for `mp4a`, the values it recommends, and 12 for `ipcm` and `fLaC`, which are lossless,
   have no recommendation and normally carry no recon gain (12 is what AOM's libiamf measures as).
   Channels no Parameter Block flags are left exactly as the de-mixer made them.
   `DecodeOptions::apply_recon_gain = false` returns the plain de-mixer output.

Against AOM's `libiamf` (its `iamfdec`, built without codecs, so `ipcm`), this module's output for ten
layer chains, every `dmixp_mode`, output gain and recon gain matches to the 24-bit quantization at every
sample except one: libiamf also runs the overlap window over de-mixed channels when the stream has no
recon gain, which at unity gain still dips the first 12 samples of each frame by up to 6.8%. This module
leaves a lossless reconstruction alone there.

`decode_pcm()` reads the substreams itself, so it covers `ipcm` only. For Opus, AAC-LC and FLAC the caller
decodes each Audio Substream with its own codec and hands the planar PCM (every frame, untrimmed) to
`reconstruct_channels()`, which does the rest and returns the same `DecodedElement`.

## Carrying Opus, AAC-LC and FLAC

IAMF's lossy and lossless codecs are carried, not produced: this module links no codec (the same
boundary `iclforge::containers::mp4` keeps), so the caller's encoder makes the packets and the module
writes everything else.

```cpp
iclforge::containers::iamf::CodedTrack track;
track.codec = iclforge::containers::iamf::CodedCodec::kOpus;
track.loudspeaker_layout = 1;      // Stereo: one coupled Audio Substream
track.trim_start_samples = 312;    // the encoder's pre-skip, also written into the Codec Config
std::vector<iclforge::containers::iamf::CodedFrame> frames;
// ... frames.push_back({{opus_packet_for_substream_0, ...}}) for each 20 ms ...
const auto file = iclforge::containers::iamf::mux_coded(track, frames);
```

- **The Codec Config** is built from 3.13: Opus's ID Header without its magic signature and big-endian
  (`opus_decoder_config()`), AAC-LC's DecoderConfigDescriptor with an AudioSpecificConfig of two channels
  and 1024 line frames (`aac_lc_decoder_config()`), FLAC's STREAMINFO as the only metadata block with the
  block size fixed (`flac_decoder_config()`). `audio_roll_distance` is -ceil(3840 / frame length) for Opus,
  -1 for AAC-LC and 0 for FLAC, as 3.5 sets it, and the ISO-BMFF output carries the matching `roll`
  sample group.
- **One packet per Audio Substream** per Temporal Unit, in the order `layout_info()` lists the layout's
  substreams (coupled pairs first). Packets are checked cheaply and not decoded: an Opus packet must hold
  one frame of `samples_per_frame` samples (the TOC byte says), a FLAC frame must name the block size,
  sample rate, depth and independent channel coding the STREAMINFO does, an AAC packet must not be empty.
  A packet that breaks one is `kBadCodedPacket`.
- **Single layer.** `build_coded_sequence()` writes one channel-based layer of `loudspeaker_layout` 0 to 8.
  Scalable, Ambisonics and object elements with these codecs are assembled on the `Sequence` model
  directly, as the readers return them.
- **Reading back.** `read_isobmff()` returns the packets in `AudioFrame::data`. The caller decodes them and
  passes the PCM of each substream to `reconstruct_channels()`; `examples/iamf_coded.cpp` does the whole
  round trip with FLAC frames it packs itself. `codecs_string()` gives the RFC 6381 string of 6.4
  (`iamf.000.000.Opus`, `iamf.000.000.mp4a.40.2`, ...).

## What it does not cover

- **Encoding or decoding Opus, AAC-LC or FLAC.** Their packets are carried and the module links no codec;
  `reconstruct_channels()` takes the PCM of the caller's own decoder. A known and accepted gap: encoding
  is a codec's job, the boundary the container modules keep.
- **Rendering.** Mix Presentations and animated parameters are data here; applying them (the Open Audio
  Renderer's job, which section 7.4 leaves to the OAR specification) is outside the module.
- **Encryption** (Common Encryption, 6.3). A protected track is recognised and refused. Writing and
  reading it needs AES, and this module, default-on, has no third-party dependency; 6.3 asks for whole-sample
  encryption, so a packager can protect the file this module writes. A known and accepted gap.
- **Writing more than one IA track.** 6.2.1 stores an IA Sequence as one track, so the writers write one;
  `read_isobmff_tracks()` reads a file that has several.

## Checked against

The channel-based output (batch, trimmed, fragmented and raw) was checked with FFmpeg 7.0.2's IAMF
demuxer, which reads the Audio Element, the Mix Presentation and every substream, remuxes the file, and
decodes the first substream to the input samples within 24-bit rounding, honouring the trim (46,400 of
48,000 samples after a 1,500 and 100 sample trim). FFmpeg 7.0.2 reads IAMF v1.0.0; it has no
object-based elements, so the v2.0 additions (object elements, position and momentary loudness
parameters, the Mix Presentation's rendering config extension, `is_not_key_frame`) have no external
oracle here. They are covered by the tests in `libs/containers/tests/iamf/`, which assemble OBU bytes by hand from the
syntax (the position fields' bit packing, trimming headers, delimiters) and round-trip a Sequence that
uses every structure.

Opus, AAC-LC and FLAC carriage was checked with FFmpeg 8.0.1: stereo and 5.1 programmes whose packets came
from `libopus`, FFmpeg's AAC encoder and a verbatim-subframe FLAC packer, muxed by `mux_coded()`, decode
through FFmpeg's IAMF demuxer to exactly the samples the encoders' own files decode to (FLAC bit for bit
against the source), with the Opus pre-skip and the AAC priming trimmed by the Audio Frame trimming.
FFmpeg's `-map 0:<n>` of one dependent substream decodes a single frame (an FFmpeg CLI behaviour, not the
file's); mapping the stream group, `-map 0:g:0`, decodes them all.

The scalable reconstruction was checked against AOM's reference decoder, `libiamf` (commit b276f43, built
with `-DENABLE_BUILD_CODECS=OFF`, which leaves `ipcm`), by decoding the same raw OBU streams with
`iamfdec -s<system> -disable_limiter` at the playback layout of each layer and comparing with this
module's `decode_pcm()`. For ten chains of layers, one `dmixp_mode` per frame (all seven values), output
gain on two groups and recon gain on the four channels the last layer de-mixes, every channel agrees to
the 24-bit quantization at every sample outside the first 12 of each frame. In those 12, libiamf
multiplies de-mixed channels by the overlap window even when the stream carries no recon gain (unity
gain, so a dip of up to 6.8%); this module applies the window only to channels a Parameter Block flags,
with the same 12 samples, so a lossless stream is reconstructed exactly. No other IAMF decoder was
available to compare with (FFmpeg demuxes the substreams and does not de-mix).

---

See also: [ADM / BW64 reading and writing](adm.md) — the indirect route to the same ecosystem;
[Muxing & sinks](muxing-and-sinks.md) — `iclforge::containers::mp4`/`iclforge::containers::matroska`, the container modules
this one's shape is modeled on; [Decoding](decoding.md) — `iclforge::ac3::Eac3Decoder`, this module's own
source of PCM.
