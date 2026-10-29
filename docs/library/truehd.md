# TrueHD (MLP): `iclforge::mlp`

`iclforge/mlp/decoder/decoder.hpp`, `iclforge/mlp/encoder/encoder.hpp` and
`iclforge/mlp/encoder/atmos.hpp`, with the framing both work through in `iclforge/mlp/io/` and the
stream's structures in `iclforge/mlp/core/`, all in the library `iclforge::mlp`. A decoder and a
lossless encoder of Dolby TrueHD, Meridian Lossless Packing (MLP) as Blu-ray carries it, with
Atmos: a 7.1 bed and up to eight objects in a 16-channel substream, the objects' metadata in
evolution payloads.

The module is written from a clean-room specification of the format (`truehd-spec.md`, version
3.0, kept outside the repository). That text contradicts itself in many places; where it does,
the reading taken is the one under which FFmpeg's TrueHD decoder, used as a black box, accepts
this library's streams and this library's decoder reproduces FFmpeg's streams. Each such reading
is listed, with the experiment that settled it, in the problem register of
[planning/truehd.md](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd.md#spec-problem-register),
and `libs/mlp/ERRATA.md` gives the corrected syntax by section.

It decodes every presentation a TrueHD stream carries (2, 6, 8 and 16 channels) at 44.1, 48,
88.2, 96, 176.4 and 192 kHz, checking every CRC, parity and lossless check the stream holds, and
starts from any major sync. It encodes 1 to 8 channels of 16 to 24-bit PCM at those rates, and an
Atmos bed with objects at 48 kHz, losslessly: with its self-check on, the default, it decodes
every access unit it writes and fails rather than emit one that does not give back its input. It
refuses, by name, what the specification does not define well enough to do losslessly:
[What it does not do](#what-it-does-not-do) lists those.

## Decoding a stream

A TrueHD elementary stream (`.thd`, `.mlp`) is access units back to back, each 1/1200 of a second
(40 samples at 44.1 and 48 kHz, 80 at 88.2 and 96 kHz, 160 at 176.4 and 192 kHz).
`iclforge::mlp::scan()` finds them and checks their framing; `iclforge::mlp::Decoder` turns each into the
PCM of one presentation:

```cpp
#include "iclforge/mlp/decoder/decoder.hpp"
#include "iclforge/mlp/io/elementary.hpp"

const std::vector<std::byte> stream = read_file("film.thd");
const iclforge::mlp::ScanResult framing = iclforge::mlp::scan(stream);

iclforge::mlp::DecoderConfig config;
config.presentation_channels = 6;  // 2, 6, 8 or 16; 0 for the richest the stream has
iclforge::mlp::Decoder decoder(config);

for (const iclforge::mlp::AccessUnitInfo& unit : framing.units) {
    const auto decoded =
        decoder.decode(std::span(stream).subspan(unit.offset, unit.size));
    if (!decoded) {
        std::cerr << iclforge::mlp::describe(decoded.error()) << '\n';
        break;
    }
    // decoded->pcm: decoded->channels x decoded->samples, interleaved, at 24-bit scale.
    write_pcm(decoded->pcm, decoded->channels, decoded->sample_rate);
}
```

Samples are integers at 24-bit scale, as a 24-bit PCM file holds them: a 16-bit source's samples
come back shifted left by 8, and forge's `decode` writes a 16-bit WAV when every sample's low
byte is zero. `iclforge::mlp::decode_stream()` decodes a whole buffer in one call.

The decoder needs a major sync to start. Before the first one, and after an error until the next
one, `decode()` returns `Error::kNoMajorSync`; `reset()` forgets every substream's state, as a seek
does, and decoding resumes at the next major sync. A stream cut from the middle of another is
found from its next random access point.

### Presentations

A TrueHD stream holds up to four substreams, and each presentation is the substreams up to one:
substream 0 alone gives the 2-channel presentation, 0 and 1 the 6-channel one, 0 to 2 the
8-channel one and 0 to 3 the 16-channel (Atmos) one. The decoder decodes only the substreams the
asked-for presentation needs, and gives the richest presentation at or below the one asked for
when the stream has fewer. A 2-channel presentation may be a fold-down of the full mix, made by
the encoder and undone by the upper substreams' matrices, or simply the first two channels; the
stream decides, and the decoder plays what it says.

### What the decoder reports

Each `DecodedAccessUnit` carries, besides the PCM:

- `major_sync` and `restart`: whether the access unit carried a major sync, and whether the
  presentation's substreams restarted in it (a random access point);
- `padding`: the samples at the end of a stream's last access unit that its termination word
  marks as padding, already dropped from `pcm`, so a source that is not a whole number of access
  units long comes back at its own length;
- `drc`: the dynamic range control metadata of section 12 (the start-up gains of the channel
  meaning and the latest gain update), exposed and not applied;
- `evolution`: the payloads of the access unit's extra data, in order: an Atmos stream's object
  audio metadata, a content signature;
- `extra_data_error`: set when the access unit had extra data this library could not read. The
  audio decodes regardless.

`Decoder::major_sync()` gives the stream's last major sync: its format information, channel
assignments, peak data rate, substream count and channel meaning.

### Checks

Every check the stream carries is made, each failure a typed `iclforge::mlp::Error`: the access unit
header's check nibble, the major sync's CRC-16, each restart header's CRC-8, each substream
segment's parity and CRC, the extra data's parity, every range the syntax allows (filter orders
and coefficient precision, quantiser steps, matrix channels, block sizes, sample and prediction
ranges), and the lossless check of each restart interval, which compares a hash of the decoded
output with the one the encoder wrote. `DecoderConfig::verify_checks` and
`DecoderConfig::verify_lossless_check`, both on by default, turn the CRC and parity checks and the
lossless check off for a caller that wants the samples of a stream it knows to be damaged.

## Encoding a stream

`iclforge::mlp::Encoder` takes interleaved integer PCM at 24-bit scale and returns access units:

```cpp
#include "iclforge/mlp/encoder/encoder.hpp"

iclforge::mlp::EncoderConfig config;
config.sample_rate = 48000;
config.channels = 6;  // L R C LFE Ls Rs
auto encoder = iclforge::mlp::Encoder::create(config);
if (!encoder) {
    std::cerr << iclforge::mlp::describe(encoder.error()) << '\n';
    return 1;
}
std::vector<std::byte> out;
for (std::span<const std::int32_t> block : blocks_of(source)) {  // whole frames, any count
    const auto units = encoder->encode(block);
    if (!units) {
        std::cerr << iclforge::mlp::describe(units.error()) << '\n';
        return 1;
    }
    out.insert(out.end(), units->begin(), units->end());
}
const auto tail = encoder->flush();  // pads the last access unit and marks the padding
out.insert(out.end(), tail->begin(), tail->end());
```

A restart interval is coded once all of it has arrived, so output lags input by up to
`restart_interval` access units; `iclforge::mlp::encode_stream()` does a whole buffer in one call.

Full program: [`examples/truehd_round_trip.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/truehd_round_trip.cpp),
which encodes 5.1 in pieces and decodes both presentations.

Channels are in the order of section 13.1's channel assignment: 2 channels L R; 6 channels L R C
LFE Ls Rs; 8 channels L R C LFE Ls Rs Lrs Rrs. Two channels make one substream, six make two (the
2-channel and 6-channel presentations), eight make three (2, 6 and 8). `channel_assignment`
chooses another of 13.1's layouts for the top presentation (6.1, say); 0 picks the usual one for
the channel count.

### What the encoder chooses

TrueHD is lossless, so every choice the encoder makes is about size. Per restart interval it
chooses, from the signal:

- the output shift that carries the source's trailing zeros, so that a 16-bit source in 24-bit
  words codes 16-bit samples;
- the presentations' downmix matrices (below);
- inter-channel decorrelation: a primitive matrix per channel whose least-squares prediction from
  the channels before it saves bits, within each substream's matrix budget;
- per channel and access unit, the prediction filter: FIR filters of orders 1 to 8 by
  Levinson-Durbin over the access unit and over the whole interval, FIR and IIR pairs by two-stage
  least squares, and the filter already in place, each costed exactly in the decoder's own
  arithmetic;
- per channel and access unit, the Huffman codebook, the number of raw low bits and the offset,
  by exact bit count;
- per access unit and substream, one block or two, by exact cost.

It then schedules each access unit's input timing against `peak_data_rate` through the decoder's
input buffer, so that every unit arrives whole before it is decoded, and fails with
`Error::kRateExceeded` when the lossless stream cannot be delivered at that rate. 18 Mbit/s,
Blu-ray's ceiling for TrueHD, is the default. `EncoderStats` reports the peak and mean rates, the
filters chosen and how often the downmix limiter acted.

On the eleven FFmpeg golden masters' sources (0.2-second synthetic test signals), the encoder's
streams are 13 to 34 per cent smaller than FFmpeg's for stereo at every rate and depth, and 3 to
23 per cent smaller for 5.1 with `downmix` off (FFmpeg's layout); with the fold-down on, the 5.1
streams cost 46 to 53 per cent more than without it on these signals.

### The lower presentations

With `downmix` on (the default) the 2-channel presentation of a 5.1 or 7.1 stream is a fold-down
of all its channels (the centre and the surrounds at -3 dB, the LFE left out), and the 6-channel
presentation of a 7.1 stream folds the rear pair into the side pair, each scaled down by a limiter
over the interval when the fold would clip. The upper substreams' matrices undo the folds
exactly, so the full presentation is still lossless. With `downmix` off each lower presentation is
the first channels of the one above, as FFmpeg's encoder writes it, which costs fewer bits and
gives a stereo player only the front pair. [Decision 3 in the design](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd.md#decisions-needing-owner-review)
records the default for the owner's review.

### Settings

| `EncoderConfig` | Default | forge option | What it does |
|---|---|---|---|
| `restart_interval` | 16 | `truehd-restart=8..128` | Access units between restart headers, each with a major sync: the random access spacing |
| `peak_data_rate` | 18,000,000 | `truehd-peak=<kbit/s>` | The delivery ceiling the schedule must meet |
| `max_fir_order`, `max_iir_order` | 8, 4 | `truehd-fir=0..8`, `truehd-iir=0..4` | The filter orders searched; together at most 8 |
| `decorrelate` | on | `truehd-decorrelate=0\|1` | Inter-channel decorrelation matrices |
| `downmix` | on | `truehd-downmix=0\|1` | Fold-down lower presentations, or the first channels |
| `verify` | on | `truehd-verify=0\|1` | Decode every access unit written and fail on any difference |
| `channel_meaning` | 0 dB gains | | Section 14.1's channel meaning in every major sync |
| `signature_hook` | none | | A content signature payload per access unit ([below](#content-signatures)) |

## Atmos

`iclforge::mlp::AtmosEncoder` codes a 7.1 bed and 1 to 8 dynamic objects, one a channel, at 48 kHz
into four substreams. Channels 0 to 7 of the 16-channel substream are the bed in 13.1's 7.1 order,
channels 8 onwards the objects. The 8-channel presentation, which a decoder without Atmos plays,
is the bed with every object rendered into it; the 6-channel and 2-channel presentations are its
fold-downs; and the 16-channel substream's matrices undo the render, so the 16-channel
presentation gives back the bed and the objects exactly as they were coded.

```cpp
#include "iclforge/mlp/encoder/atmos.hpp"

iclforge::mlp::AtmosConfig config;
config.objects = 2;
auto encoder = iclforge::mlp::AtmosEncoder::create(config);

std::vector<iclforge::objects::oba::DynamicObject> state(2);
// x 0 (left wall) to 1 (right), y 0 (front wall) to 1 (back), z -1 (floor) to 1 (ceiling)
state[0].position = {0.0, 0.0, 0.0};  // front left
state[1].position = {1.0, 1.0, 1.0};  // back right, overhead
const auto units = encoder->encode(bed_8ch, objects_2ch, state);
// ...further calls with an empty `state` keep the objects where they are;
// a new `state` moves them from the access unit its first frame falls in.
const auto tail = encoder->flush();
```

The object metadata goes in an evolution payload of type `EvolutionPayloadType::kOamd`, as
`iclforge::objects`' serialisation of ETSI TS 103 420 object audio metadata (section 15 of the
specification gives no bit syntax of its own and points there): one bed instance, the dynamic
objects, one update a payload, sent in the access unit the state starts at and again at every
restart, so that a decoder joining there has it. `iclforge::mlp::render_gains()` is the render the
8-channel presentation uses: a distance-weighted, power-normalised pan over the seven full-range
bed speakers, height folded to the floor, scaled by the object's gain.

`iclforge::mlp::decode_atmos()` decodes the 16-channel presentation and splits it into the bed, the
objects and each metadata frame with the sample it takes effect at. FFmpeg's decoder, which
decodes at most 8 channels, plays an Atmos stream from this library as its 8-channel presentation,
sample for sample the same as this decoder's.

## Probing

`iclforge::mlp::probe()` summarises a stream from its framing and major syncs, without decoding audio:
the first major sync, the access unit and major sync counts, the duration, the mean rate and the
largest access unit, the presentations present, and whether any access unit carries evolution
payloads. `iclforge::mlp::parse_major_sync()` reads one major sync, and `iclforge/mlp/core/` holds the
structures themselves (`AccessUnitHeader`, `RestartHeader`, `BlockHeader` and the rest) for a tool
that wants them.

## Content signatures

The specification (section 22) says TrueHD content is signed with the `pace-ccjf1` ECDSA content
signature, carried in the extra data; this project's earlier notes said a truncated HMAC-SHA-256
"Evolution frame protection" ([Object signing](../concepts/object-signing.md#sibling-truehd-evolution)).
The two disagree, and neither scheme is implemented. What the library has is the carriage: an
evolution payload of type `EvolutionPayloadType::kContentSignature` is written into an access
unit's extra data by the encoder's `SignatureHook`, which is called once per access unit with the
bytes to sign, and read back by the decoder into `DecodedAccessUnit::evolution`. Which scheme the
hook will call is for the owner to decide (decision 1 of the design).

## forge

| Command | What it does |
|---|---|
| `forge truehd-encode in.wav out.thd` | 16 or 24-bit PCM, 1 to 8 channels, to TrueHD; `out.mkv` writes Matroska (`A_TRUEHD`). The options of [Settings](#settings) apply |
| `forge truehd-atmos-encode bed.wav objects.wav out.thd` | A 7.1 bed and 1 to 8 objects to TrueHD Atmos. The objects circle the room on fixed paths, to exercise the metadata; the library takes any |
| `forge truehd-atmos-decode in.thd bed.wav objects.wav metadata.json` | The bed, the objects and every metadata frame (as JSON) of a TrueHD Atmos stream |
| `forge decode in.thd out.wav` | Any TrueHD stream, bare or in Matroska; `truehd-presentation=2\|6\|8\|16` picks the presentation, `truehd-bits=16\|24` the WAV depth |
| `forge probe in.thd` | The stream's summary, as a table or with `json=1` as JSON |
| `forge transcode in.thd out.ec3` | TrueHD on either side: through integer PCM when both sides are TrueHD, through a WAV the other codec reads or writes otherwise |

A WAV file orders a 7.1 source's rear pair before its side pair, as its channel mask does; forge
reorders between that and 13.1's order on the way in and out, so a WAV encoded and decoded again
comes back sample for sample.

## Containers

forge's Matroska writer and reader carry TrueHD as `A_TRUEHD`, and `forge decode` and `probe`
select a TrueHD track by itself. FFmpeg reads the Matroska files forge writes. FFmpeg's demuxer
does not recognise a bare 44.1 kHz TrueHD stream on its own, its own encoder's included; give it
`-f truehd`.

## What it does not do

- **Noise.** Section 17 gives no noise generator, so a stream whose matrices take a noise channel
  or a noise shift is refused with `Error::kNoiseChannel` rather than decoded wrongly; the encoder
  never uses noise.
- **DVD-Audio MLP** (major sync 0xF8726FBB), whose format information is laid out for two
  sample-rate groups, is refused with `Error::kUnsupportedStreamType`.
- **IEC 61937 and MPEG-TS.** Passthrough over HDMI or S/PDIF (IEC 61937 data type 22, MAT framing)
  and MPEG-TS stream type 0x83 are not written or read: the specification gives no MAT frame
  syntax.
- **Signing.** No content signature scheme ([above](#content-signatures)).
- **Dolby's extra data.** The extra data layout is this library's design (section 16.1 gives
  none that can be implemented), so a Dolby encoder's evolution payloads read as
  `extra_data_error`, with the audio decoded regardless.
- **Block data CRCs.** `block_data_crc` is read and not checked, as its coverage is not given; the
  encoder never writes one.

## Errors

Nothing throws for a stream or a configuration. `iclforge::mlp::Error` is the error of every
`std::expected` the module returns, and `iclforge::mlp::describe()` names the check that failed in a
sentence. The decoder's errors name the structure and section (`kRestartHeaderCrc`,
`kLosslessCheck`, `kFilterOrder`); the encoder's are `kInvalidConfiguration` (a setting outside
what is supported), `kRateExceeded` and `kVerificationFailed`.

## Validation

- **Unit and property tests** (`iclforge-mlp-tests`): CRCs and Huffman tables against the
  specification, every structure written and read back, round trips at every rate, depth and
  channel count with random signals, the FIFO schedule, Atmos bed, object and metadata round
  trips.
- **Golden masters from FFmpeg** (`testdata/truehd/ffmpeg`, made by
  `tools/generators/gen_truehd_ffmpeg_gold.py`): eleven streams written by FFmpeg's experimental
  TrueHD encoder from known sources, stereo and 5.1 at 44.1 to 192 kHz, 16 and 24-bit; the decoder
  must give each source sample for sample.
- **The interoperability gate** (`tools/checks/verify_truehd_interop.py`, in CI): forge's streams
  of 1 to 8 channels at every rate and depth must decode in FFmpeg, under strict error detection,
  to their source; the lower presentations must equal FFmpeg's `-downmix` of the same stream; the
  same in Matroska; FFmpeg's golden masters must decode in forge; and FFmpeg must play an Atmos
  stream's 8-channel presentation equal to forge's.
- **Dolby Encoding Engine masters** (`tools/checks/verify_truehd_dee_gold.py`): DEE's TrueHD
  streams of known sources, made on the owner's machine, decoded by forge and by FFmpeg and
  compared with their sources. The set is kept outside the repository, so the script runs where
  `ICLFORGE_DEE_GOLD_DIR` points at it, and over a committed subset
  (`tools/generators/export_dee_truehd_gold.py`) with `--committed`.
- **Fuzzing** (`libs/mlp/fuzz`): `fuzz_mlp_decode` feeds damaged streams to the decoder,
  `fuzz_mlp_encode` arbitrary PCM and settings to the encoder with its self-check on.

No Dolby decoder has been tried on this library's streams.

## Linking

**In-tree:**

```cmake
target_link_libraries(your_target PRIVATE iclforge::mlp)
```

**Installed package** (`find_package(iclforge)`, see [Using the libraries](index.md)):

```cmake
find_package(iclforge REQUIRED)
target_link_libraries(your_target PRIVATE iclforge::mlp_static)   # or iclforge::mlp_shared
```

`iclforge::mlp` depends on `iclforge::base` and, for Atmos's metadata, `iclforge::objects`. Through
pkg-config it is `iclforge-mlp`. `ICLFORGE_BUILD_MLP`, on by default, builds it; the vcpkg port
and the Conan recipe install it where asked for: `vcpkg install iclforge[truehd]`, or
`-o "iclforge/*:truehd=True"`. The module is Experimental ([API stability](api-stability.md)).
