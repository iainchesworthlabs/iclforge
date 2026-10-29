# TrueHD and MLP: `iclforge::truehd`

`iclforge/truehd/decoder/decoder.hpp`, with the bitstream layer under it in `iclforge/truehd/core/` and
`iclforge/truehd/io/`, all in the library `iclforge::truehd`. A reader, a lossless decoder and an encoder for Dolby
TrueHD (`0xF8726FBA`, Blu-ray and HD DVD) and MLP (`0xF8726FBB`, DVD-Audio) elementary streams, written
from the owner's *Dolby TrueHD (MLP) Reference Specification*, v3.5 ("TH spec" in the code). The library links
`iclforge::base` and nothing else; the Atmos substream and the object metadata are phase T2b
([the plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/truehd.md)).

The specification contradicts itself and the streams in places and is silent in others. Every such place is
an entry of
[`libs/truehd/ERRATA.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md),
which lists first the questions only the specification's owner can answer. Where a stream forced a reading the
specification does not give, the entry says so.

## What it decodes

The layers a stream has, in the order the reader walks them:

- the **access unit**: a four-byte header (a check nibble that makes the nibbles of the header and the
  substream directory XOR to `0xF`, a length in 16-bit words, `input_timing`), a **major sync** when the unit has
  one (28 bytes, CRC-16), the **substream directory**, the segments, and extra data after them;
- each **segment**: blocks, each with an optional **restart header** (CRC-8) and **block header**, then the
  residuals, Huffman-coded with one of the three codebooks or raw, and a parity byte and a CRC-8 after the last
  block;
- the **reconstruction**: the FIR and IIR prediction filters, the quantiser step, the primitive matrices
  (with LSB bypass), `ch_assign` and the output shift, and the lossless check of each restart interval, which
  the next restart header carries.

Substreams 0 to 2 (2, 6 and 8 channels) and the generic 16-channel substream 3 decode the same way, each
reading the channels the ones before it produced. Sample rates are 44.1 to 192 kHz in 16 and 24 bits (a decode
is always 24 bits wide, in an `int32`).

## Decoding

```cpp
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

iclforge::truehd::ElementaryReader reader(stream);
iclforge::truehd::Decoder decoder;               // DecoderConfig: presentation, strictness
while (const auto unit = reader.next()) {
    auto result = decoder.decode(*unit);         // std::expected<std::optional<DecodedAccessUnit>, Error>
    if (result && *result) {
        // (*result)->pcm: samples x channels, interleaved; ->layout names the speakers when the major sync does
    }
}
```

`decode()` gives no samples and no error for a unit that comes before the first major sync or before a
substream's first restart header (a stream joined partway). `Presentation` picks the substreams read: `kBest`
(0 to 2, the default), `k2ch`, `k6ch`, `k8ch` or `kAll` (substream 3 too). `Strictness::kStrict` makes a failed check
(check nibble, CRC, parity, lossless check) an error that drops the unit and leaves the substream waiting for its next
restart header; `kLenient` counts it (`DecoderCounters`) and decodes on. The last unit of a stream that does not end
on a unit boundary carries an end-of-stream marker; the decoder removes the padding samples it counts
(`padding_samples`). Decoding does not allocate once a stream is running (the buffers are sized when the decoder is
built), apart from a change of major sync and an extra data field longer than 256 bytes. The extra data of a unit is
not decoded: `extra_data` hands back its bytes. `read_extra_data` and `read_evolution`
(`iclforge/truehd/io/evolution.hpp`) read them into plain structures, as a reading that waits for the owner: the
check nibble, the framing and the parity are reported (not enforced), and the evolution payload is read as the
bit-level container of TH spec v3.6 section 15.4 (version, key id, payloads with their configuration and bytes, and the
protection field, which is returned and not verified). `EvolutionFrame::oamd()` hands out the bytes of payload id 11, the
object audio metadata, for a program that links `iclforge::objects` to parse with `iclforge::objects::oba::parse_payload`;
this library links `iclforge::base` and nothing else. `forge probe` prints what it finds. The reading is provisional
(`libs/truehd/ERRATA.md`, questions 20 to 22): the owner's reading is that this container is the outer one and the Thrift content
signature is nested in one of its payloads, so every payload is carried as bytes with its id and size and none but the object audio
metadata is looked into here.

The syntax is also written: `write_restart_header`, `write_block_header`, `write_major_sync` and
`write_access_unit_prefix` are the readers' inverses, which the encoder is built on.

## Encoding

`iclforge/truehd/encoder/encoder.hpp`: `Encoder`, `EncoderConfig`, `EncodedAccessUnit`, `EncoderStats`. The encoder
streams: `create()` checks the configuration (`refusal_reason()` gives the same sentence without building one),
`encode()` takes interleaved PCM and returns the access units that are complete, and `flush()` codes what is left,
pads the last unit with zero samples and ends the stream with the marker the decoder removes.

```cpp
#include "iclforge/truehd/encoder/encoder.hpp"

iclforge::truehd::EncoderConfig config;
config.sample_rate = 48000;                       // 44.1, 48, 88.2, 96, 176.4 or 192 kHz
config.channels = 6;                              // 1 to 8
config.bits = 24;                                 // 16, 20 or 24: the width of the PCM given to encode()
auto encoder = iclforge::truehd::Encoder::create(config);   // std::expected<Encoder, EncodeError>
if (!encoder) { /* encoder.error().detail names the rule */ }
for (const auto& chunk : pcm_chunks) {            // interleaved int32
    auto units = encoder->encode(chunk);          // std::expected<std::vector<EncodedAccessUnit>, EncodeError>
    // append each unit's bytes to the .thd file
}
auto tail = encoder->flush();
```

The encoder holds one restart interval of input before it emits it (128 access units by default, 107 ms at 48 kHz).
In each interval it finds the low bits every sample leaves clear and sends them as the output shift and the
quantiser step (a 16 bit source in a 24 bit container is shift 7, step 1), decorrelates neighbouring pairs of
channels with a primitive matrix where that makes the stream smaller, predicts each channel with a FIR filter of up
to order 8 found by windowed autocorrelation and the Levinson-Durbin recursion (order 4 at 176.4 and 192 kHz, the
specification's limit), and codes each block's residual with whichever of the three codebooks or raw bits takes the
fewest bits, with the offset and raw width to match. Channels are spread over substreams the way FFmpeg's encoder
spreads them: two in substream 0, the next four in substream 1 and the last two in substream 2 (one substream at
176.4 and 192 kHz).

`DownmixMode::kStandard` writes the 2 and 6 channel presentations as downmixes instead: substream 0 holds a stereo
mix, the matrices of the later substreams take the folded-in channels out again, and a 7.1 stream folds its rear
surrounds into substream 1's 5.1 the same way. The gains are `EncoderConfig::downmix_gains`; a fold that would leave a
presentation outside 24 bits has its gains lowered for that interval and `EncoderStats::downmix_reduced_intervals`
counts it. The full presentation always comes back exactly; the gains and the limiter are the encoder's choice, since
the specification gives none (`libs/truehd/ERRATA.md`, entry 38), and no decoder checks them.

The stream is held to the peak data rate it declares (18 Mbps by default, Blu-ray's) and to the 75 ms latency of the
specification's FIFO model: a stream that cannot be sent in time is `EncodeErrorKind::kRateExceeded`, or with
`allow_rate_violations` is written and counted. `verify` decodes every unit with this library's decoder as it is
produced and compares it with the input. `extra_data` frames a payload into a unit's extra data and never looks
inside it. The restart interval is 1 or at least 8 access units; a longer interval makes a smaller stream and a shorter
one a quicker random access.

FFmpeg's strict decode of what it writes is the check, sample for sample
(`tools/checks/verify_truehd_encode_interop.py`, with `--forge` through `forge truehd-encode`); the streams are 0.95 times the size of
FFmpeg's encoder's over the layouts it writes. It writes no noise, no IIR filter and no LSB bypass (nothing outside this library reads
them), no MLP, and no block data CRC; the Atmos substream and the evolution payload it only frames, from bytes it is given.
[`libs/truehd/ERRATA.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md) (entries 30 to 38, questions 12 to 15)
records each place the specification and the streams left it a choice. `forge truehd-encode` is the command;
`iclforge-truehd-encode-tool` (WAV in, `.thd` out) is the test tool the interoperability checks use.

## Forge

```
forge probe stream.thd [json=1]
forge decode stream.thd out.wav [truehd-presentation=best|2ch|6ch|8ch|all] [truehd-check=strict|lenient]
forge truehd-encode in.wav out.thd|out.mkv|out.mp4|out.ts [truehd-downmix=...] [truehd-restart=N] [verify] ...
forge levels|loudness|mkv|mp4|ts|spdif|unspdif|demux|remux  stream.thd ...
```

The second group reads a TrueHD or MLP stream like the AC-3 family; the
[command reference](../forge/cli/commands.md#truehd-and-mlp) has each option, and which commands refuse a TrueHD input.

`decode` writes a 24-bit PCM WAV with the channels in the order the stream carries them, and a channel mask when
that order is the WAV's own (5.1 is). A failed check, or a byte the reader had to skip to find the next unit,
stops a strict decode with exit status 2; `truehd-check=lenient` counts them and goes on. `probe` describes the
stream and checks every unit's header, directory, parity and CRC, and exits 1 if any fails.

## What it is checked against

| Check | What it covers | Oracle |
|---|---|---|
| FFmpeg's strict decode (`-err_detect crccheck+bitstream+buffer+explode`) of the streams its `truehd` and `mlp` encoders write | 25 fixtures in `testdata/truehd/`: 1 to 6 channels, 44.1 to 192 kHz, 16, 20 and 24 bits, TrueHD and MLP; 19 more while the decoder was written | FFmpeg: sample for sample |
| FFmpeg's strict decode of the streams this library's **encoder** writes (`tools/checks/verify_truehd_encode_interop.py`, with `--forge` it runs `forge truehd-encode`; the codec matrix's encode legs and the gold-reference gate also run it) | 43 cases: 1 to 8 channels, 44.1 to 192 kHz, 16, 20 and 24 bits, real music, speech and 5.1 programme material and synthetic signals chosen to be hard (full-scale squares, impulses, silence, DC, noise, level steps), the downmix presentations, restart intervals of 1, 8 and 128 units, lengths that are not whole units | FFmpeg: sample for sample, 43 of 43 |
| `tools/ci/fuzz_truehd_encoder_space.py`, random legal configurations against adversarial PCM, and the libFuzzer harness `fuzz_truehd_encode` | the encoder's whole configuration space, FFmpeg's strict decode of each stream | FFmpeg: sample for sample |
| `tools/references/truehd_syntax.py`, an independent transcription | the same 25, and the hand-built streams | none beyond the two readings agreeing |
| Hand-built streams, `tools/references/truehd_writer.py` (`tools/checks/verify_truehd_interop.py --handbuilt`) | an IIR filter, LSB bypass, noise generator 0, several matrices with a permuted `ch_assign`, an output shift, the block data check, the end-of-stream marker, extra data, the directory's DRC word, four substreams with a 16-channel one, and one at the limits TH spec v3.6 gives substream 3 (15 matrices, `huff_lsbs` 31) | FFmpeg: the whole signal each was built from, for 11 of the 15; the manifest names what FFmpeg does with the other 4. **None** for the extra data's layout, the DRC word's values, the block data CRC and the 16-channel substream |
| DEE's streams of known sources | scored on the owner's machine by `tools/checks/score_truehd_decode.py`, which skips when the set is absent | the sources |

FFmpeg's encoders do not write an IIR filter, LSB bypass, 7.1, a non-zero noise coefficient, the Atmos substream or
extra data. The hand-built streams carry the first, second and fourth and extra data, and FFmpeg's decoder reads them to the
same samples; 7.1 is checked through this library's encoder. What FFmpeg does not read has no external oracle: the extra data's
layout, the DRC word's values, the block data CRC, the 16-channel substream (FFmpeg refuses its restart sync `0x31EC`) and
the end-of-stream marker in every unit (FFmpeg returns two of the twenty units of the hand-built stream).
[Validation](../verification.md#truehd-and-mlp) says what that leaves.

## What it does not do

- Several rules the specification states are not checked: saturation in Huffman decoding, `coeff_q` below 8, the range of
  filter coefficients and states, `output_shift` above `max_shift`, the extra data's length against the unit, the restart-gap
  rules. The encoder does not model the 30,000-byte FIFO of §18.1 and writes a restart interval of 1 unit throughout when
  asked, which §7.5 forbids. `libs/truehd/ERRATA.md` entry 41 lists them.
- Noise generator 1: LSB bypass, or a matrix with a non-zero noise shift, under restart sync `0x31EB` is refused with
  `ErrorKind::kUnsupported`, because the 256-entry table the noise comes from is not in the specification yet (§17.4).
  A `0x31EB` restart with neither decodes: FFmpeg's streams of more than 6 channels use it, and its matrices carry no
  noise coefficients. Generator 0, the default (`0x31EA`), is decoded from the
  specification's LFSR: two noise channels follow the matrix channels and enter through ordinary matrix
  coefficients. No stream FFmpeg writes uses a non-zero noise coefficient; a hand-built stream does, and FFmpeg's strict decode
  of it equals this library's with `shr7` read as 16 bits (ERRATA question 5, provisional).
- Heavy DRC in the restart header (TH spec 12.2): where its bits sit cannot be told from the streams.
- The block data CRC is not checked, only the length it follows.
- The evolution frame is read (provisionally) and the object audio metadata in it is handed out as bytes; nothing
  decodes from either, and the signature and protection bytes are not verified.
- The encoder does not write an IIR filter, LSB bypass, a negative output shift, heavy DRC, noise or MLP
  (`ExperimentalTools::iir` and `lsb_bypass` are refused with a reason), nor the Atmos substream. Against FFmpeg's
  encoder the streams are smaller on most material and larger at 192 kHz (the specification's order 4 against
  FFmpeg's 7), on full-scale square waves, with a restart interval of 1 and with the downmix presentations.
- The object renderer and the Atmos substream are phase T2b. Python, JavaScript and Rust bindings over the C API
  (`iclforge_truehd_*` in [the C API](c-api.md#truehd-and-mlp)) are not written.
- Content signing: nothing signs or checks a TrueHD stream, and an evolution payload is carried, not read. The owner's
  specification describes it as the container [AC-4's content signature](signing.md#ac-4-content-signing) uses; see
  [the seam for other codecs](signing.md#the-seam-for-other-codecs).
- Protected files: a protected (Common Encryption) `mlpa` track is refused by the MP4 reader's code for protected audio, which
  names the scheme; no fixture or test covers an `mlpa` one.
- Nothing outside the library uses it but `forge` (and the C API's callers): the Forge GUI, Hearth and Crucible do not link it,
  `forge cut`, `cat`, `metadata`, `normalize`, `transcode`, `qc`, `fmp4`, `monitor` and `play` refuse it by name (the last five
  read the AC-3 or E-AC-3 track after a TrueHD one in a file that has both), and no device output sends a MAT burst.
  [Development status](development-status.md#truehd-mlp) has each as a row.
