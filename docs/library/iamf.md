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
| `iamf.hpp` | The short route: `mux()` for a 7.1.4 programme, `mux_objects()` for object elements, `build_sequence()` and `build_object_sequence()` for the `Sequence` either would write, and `decode_pcm()` to read an element's audio back. |
| `model.hpp` | The element graph as plain data: `Sequence` (Descriptors and Temporal Units), `CodecConfig`, `AudioElement`, `MixPresentation`, parameter definitions and `ParameterBlock`, `AudioFrame` with its trimming, `Metadata`. Q7.8 gains and coded positions are kept as the bitstream carries them. |
| `sequence.hpp` | OBU bytes: `write_descriptors()`, `write_temporal_unit()`, `write_sequence()` and their readers `read_descriptors()`, `read_temporal_unit()`, `read_sequence()`. `write_sequence()`/`read_sequence()` are the standalone raw OBU stream. |
| `container.hpp` | ISO-BMFF: `write_isobmff()`, `read_isobmff()` and `FragmentedWriter`. |

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
named at each call site in `src/containers/src/iamf/`), per this project's clean-room rule — `libiamf`, FFmpeg and
AOM's Open Audio Renderer are oracles used to validate the output, never sources this code was
transcribed from.

- **IA Sequence Header OBU**: Simple Profile for the 7.1.4 programme; Base-Advanced for object programmes
  (the profile whose first Mix Presentation references only object-based elements, at most 18 channels).
  `model.hpp` lets a caller set any profile.
- **Codec Config OBU**: `ipcm` at 16, 24 or 32 bits and the sample rates the LPCM decoder config allows.
  Other codecs (`Opus`, `mp4a`, `fLaC`) are carried as raw `decoder_config` bytes by the model, so a
  Sequence read from a file that uses them reads back and writes again, but this module encodes none of
  them and `decode_pcm()` reads only `ipcm`.
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
- **ISO-BMFF** (`write_isobmff()`): `iamf`-branded `ftyp`, one `trak` whose `stsd` carries an `iamf`
  `IASampleEntry` wrapping an `iacb` box of the Descriptors, one IA Sample per Temporal Unit, an `stss`
  box when a Temporal Unit is not a key frame, and 64-bit chunk offsets when the file needs them.
- **Fragments** (`FragmentedWriter`): an initialization segment (`ftyp` and a `moov` with an empty sample
  table and an `mvex`), then one `moof`/`mdat` per call, each fragment usable as soon as it is returned.
  Non-key Temporal Units carry `sample_is_non_sync_sample` in their sample flags.
- **Raw OBU stream** (`write_sequence()`): the Descriptors then each Temporal Unit, the standalone
  representation, with Temporal Delimiters when the units ask for them.

## What gets read

`read_isobmff()` reads a file from this writer, from `FragmentedWriter`, or from another muxer that
follows the encapsulation: sample tables or `moof`/`trun` fragments (with `default-base-is-moof`, explicit
base offsets and per-sample or default sizes, durations and flags), `co64`, `stss`, `edts`/`elst` and the
`mdhd` timescale. `read_sequence()` reads a raw OBU stream, splitting Temporal Units at Temporal
Delimiters or, without them, where a substream repeats. Both skip redundant copies of the Descriptors,
Reserved OBUs and bytes past the syntax an OBU defines, stop at a second IA Sequence, and fail with
`Error::kTruncated`, `kBadLeb128` or `kBadObu` rather than read past their input; the readers'
allocations are bounded by the bytes that remain.

`decode_pcm()` turns an `ipcm` Audio Element into planar float channels with the trimming applied:
channel-based elements of one layer, object-based elements, and scene-based elements in mono mode.

## What it does not cover

- **Encoding Opus, AAC-LC or FLAC.** They are carried and parsed as bytes, not produced.
- **Scalable channel audio reconstruction.** A multi-layer Audio Element reads and writes, but
  `decode_pcm()` does not apply the demixing and recon gain that rebuild its higher layers.
- **Rendering.** Mix Presentations and animated parameters are data here; applying them (the Open Audio
  Renderer's job) is outside the module.
- **Encryption** (Common Encryption) and the codecs parameter string.
- **ISO-BMFF with more than one IA track**, and 64-bit `mdat` sizes: the first IA track is read, and one
  `mdat` must stay under 4 GiB.

## Checked against

The channel-based output (batch, trimmed, fragmented and raw) was checked with FFmpeg 7.0.2's IAMF
demuxer, which reads the Audio Element, the Mix Presentation and every substream, remuxes the file, and
decodes the first substream to the input samples within 24-bit rounding, honouring the trim (46,400 of
48,000 samples after a 1,500 and 100 sample trim). FFmpeg 7.0.2 reads IAMF v1.0.0; it has no
object-based elements, so the v2.0 additions (object elements, position and momentary loudness
parameters, the Mix Presentation's rendering config extension, `is_not_key_frame`) have no external
oracle here. They are covered by the tests in `tests/containers/iamf/`, which assemble OBU bytes by hand from the
syntax (the position fields' bit packing, trimming headers, delimiters) and round-trip a Sequence that
uses every structure.

---

See also: [ADM / BW64 reading and writing](adm.md) — the indirect route to the same ecosystem;
[Muxing & sinks](muxing-and-sinks.md) — `iclforge::containers::mp4`/`iclforge::containers::matroska`, the container modules
this one's shape is modeled on; [Decoding](decoding.md) — `iclforge::ac3::Eac3Decoder`, this module's own
source of PCM.
