# TrueHD and MLP: `iclforge::truehd`

`iclforge/truehd/decoder/decoder.hpp`, with the bitstream layer under it in `iclforge/truehd/core/` and
`iclforge/truehd/io/`, all in the library `iclforge::truehd`. A reader, a lossless decoder and an encoder for Dolby
TrueHD (`0xF8726FBA`, Blu-ray and HD DVD) and MLP (`0xF8726FBB`, DVD-Audio) elementary streams, written
from the owner's *Dolby TrueHD (MLP) Reference Specification*, v3.9 ("TH spec" in the code). The library links
`iclforge::base` and nothing else. The Atmos substream is decoded and written (below, provisionally: nothing outside this
repository reads it), and the object audio metadata is handled by `forge`, which links `iclforge::objects`
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

A stream may have bytes in front of its first access unit (the owner's timecode fixtures have a 16 byte packet there). `find_stream_start`
and the reader look for the first offset, at most `kMaxLeadingBytes` (64 KiB) in, at which a unit reads with a valid major sync CRC
and the unit after it reads too (or is cut short by the end of the data with a consistent header). `ElementaryReader::leading_bytes()`
says how many were skipped, `forge truehd-decode` and `forge probe` warn with the count (a warning, not an error), and what the
bytes hold is not looked into.

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
check nibble (the four nibbles of the header word XOR to 0xF, §16.2; a zero header word is padding), the framing and the parity are reported (not enforced), and the evolution payload is read as the
bit-level container of TH spec section 15.4 (version, key id, payloads with their configuration and bytes, and the
protection field, which is returned and not verified). `EvolutionFrame::oamd()` hands out the bytes of payload id 11, the
object audio metadata, for a program that links `iclforge::objects` to parse with `iclforge::objects::oba::parse_payload`;
this library links `iclforge::base` and nothing else. `forge probe` prints what it finds. The reading is provisional
(`libs/truehd/ERRATA.md`, questions 22 to 24): the owner's reading is that this container is the outer one and the Thrift content
signature is nested in one of its payloads, so every payload is carried as bytes with its id and size and none but the object audio
metadata is looked into here.

The syntax is also written: `write_restart_header`, `write_block_header`, `write_major_sync` (with the 16-channel
extension of TH spec §14.2, `MajorSync::extra`) and `write_access_unit_prefix` are the readers' inverses, which the encoder is
built on.

### Atmos: substream 3

`Presentation::kAll` decodes substream 3 (restart sync `0x31EC`, up to 16 channels), whose channels are the objects of the
stream's object audio metadata, channel i being object i (`libs/truehd/ERRATA.md` entry 61). The matrices' coefficients are
scaled to 2^-15 so that `frac_bits` 15 reads (entry 60). What the owner's discs show of substream 3's block header is open
(question 60): `tools/checks/truehd_substream3_search.py` is the check the owner runs on a disc excerpt. `forge decode
... truehd-presentation=all` writes the channels (`objects_dir`, `adm_out` and `truehd-render=` read them as objects:
[Forge](../forge/cli/commands.md#truehd-and-mlp)); the objects are parsed and placed by `forge` and `iclforge::render`, not by this library.

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

`EncoderConfig::channel_meaning` writes the channel meaning of TH spec §14.1 into every major sync: for the 2, 6 and 8 channel
presentations a dialogue norm, a mix level, a source format (not the 2 channel one) and a control-enabled bit, and the two DRC
start-up gains. A field outside its width, or `extra_present` (the sixteen-channel extension is not written), is
`EncodeErrorKind::kInvalidConfig` with the field's name in the detail (`channel_meaning_problem()` gives the same sentence);
left alone every field is zero, as in FFmpeg's streams, so a configuration that does not set them writes the stream it always wrote.
**Provisional, no oracle until the DEE set:** the units, the presentation that applies when and what a control bit gates are open
(`libs/truehd/ERRATA.md`, questions 51 to 55).

`iclforge/truehd/io/metadata_edit.hpp` changes the same fields in a finished stream: `edit_channel_meaning(stream, edit)` takes a
`ChannelMeaningEdit` (each member optional, range-checked by `apply_edit()`), finds the major syncs as `ElementaryReader` finds the access
units, sets the named fields in every one and re-stamps its CRC-16. No other check covers the major sync, so no other byte changes and a
decoder gives the same samples; bytes that are not an access unit, units without a major sync and the 16-channel extension are left as they
are. An MLP stream (its bytes at that place are not described), a stream with no major sync and a value outside its field's width are errors,
and then nothing is written. `forge metadata` and `forge normalize` are the commands over it.

The stream is held to the peak data rate it declares (18 Mbps by default, Blu-ray's) and to the 75 ms latency of the
specification's FIFO model: a stream that cannot be sent in time is `EncodeErrorKind::kRateExceeded`, or with
`allow_rate_violations` is written and counted. `verify` decodes every unit with this library's decoder as it is
produced and compares it with the input. `extra_data` frames a payload into a unit's extra data and never looks
inside it. The restart interval is 1 or at least 8 access units; a longer interval makes a smaller stream and a shorter
one a quicker random access.

FFmpeg's strict decode of what it writes is the check, sample for sample
(`tools/checks/verify_truehd_encode_interop.py`, with `--forge` through `forge truehd-encode`); the streams are 0.95 times the size of
FFmpeg's encoder's over the layouts it writes. It writes no noise, no IIR filter and no LSB bypass (nothing outside this library reads
them), no MLP, and no block data CRC; the evolution payload it only frames, from bytes it is given. With
`EncoderConfig::object_channels` it also writes an Atmos stream: the 7.1 bed in substreams 0 to 2 and 1 to 16 object channels in
substream 3 (restart sync `0x31EC`, no matrix, FIR prediction), with the 16-channel extension in the major sync. The bed's
substreams are those of the 7.1 stream of the bed alone, byte for byte, so FFmpeg's strict decode (which leaves substream 3 alone)
returns the bed. `forge truehd-atmos` is the command that supplies the metadata. Provisional (entry 63): the stream codes the bed
and the objects separately, where the owner's discs most likely predict one from the other, and no decoder outside this repository
reads substream 3.
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
| Hand-built streams, `tools/references/truehd_writer.py` (`tools/checks/verify_truehd_interop.py --handbuilt`) | an IIR filter and its `new_states`, LSB bypass (under either restart sync), noise generators 0 and 1 (at 48, 96 and 192 kHz), a negative output shift and an output shift on a moved channel, a 16 bit MLP stream, terminators of both kinds, several matrices with a permuted `ch_assign` (also one that changes at each restart), an output shift, the block data check, the end-of-stream marker, extra data, the directory's DRC word, four substreams with a 16-channel one, and one at the limits TH spec v3.6 gives substream 3 (15 matrices, `huff_lsbs` 31) | FFmpeg: the whole signal each was built from, for 19 of the 33, and the bed of four more (it leaves their substream 3 alone); the manifest names what FFmpeg does with the other 10 (it refuses the 16-channel and limit streams and a stream with a terminator, applies no negative output shift, and clears the low bits of a 16 bit MLP stream). **None** for the extra data's layout, the DRC word's values, the block data CRC and the 16-channel substream |
| DEE's streams of known sources | scored on the owner's machine by `tools/checks/score_truehd_decode.py`, which skips when the set is absent; 13 of them are in `testdata/truehd-dee` (11 pass, the two at 96 kHz differ from their sources in the encoder, none fail) | the sources |
| The owner's Blu-ray (an Atmos 7.1 stream of four substreams), analysed on the owner's machine | `planning/truehd-disc-findings.md`: every lossless-check interval of the 2, 6 and 8 channel presentations, the 8 channel output against FFmpeg's | the disc's own lossless checks, and FFmpeg |

FFmpeg's encoders do not write an IIR filter, LSB bypass, 7.1, a non-zero noise coefficient or shift, the Atmos substream or
extra data. The hand-built streams carry all of them but the Atmos substream, and FFmpeg's decoder reads them to the
same samples, except a negative output shift (which it does not apply) and a 16 bit MLP stream with bits below bit 8 (which it clears); 7.1 is checked through this library's encoder. What FFmpeg does not read has no external oracle: the extra data's
layout, the DRC word's values, the block data CRC, the 16-channel substream (FFmpeg refuses its restart sync `0x31EC`) and
the end-of-stream marker in every unit (FFmpeg returns two of the twenty units of the hand-built stream).
[Validation](../verification.md#truehd-and-mlp) says what that leaves.

## What it does not do

- Several rules the specification states are not checked: saturation in Huffman decoding, `coeff_q` below 8, the range of
  filter coefficients and states, `output_shift` above `max_shift`, the extra data's length against the unit, the restart-gap
  rules. The encoder does not model the 30,000-byte FIFO of §18.1 and writes a restart interval of 1 unit throughout when
  asked, which §7.5 forbids. `libs/truehd/ERRATA.md` entry 41 lists them.
- Noise: both generators are decoded. Generator 1 (restart sync `0x31EB`) fills a buffer from the 256-entry table of TH spec §17.4
  for every access unit and a matrix with a non-zero `matrix_noise_shift` adds a value of it to its sum (the buffer size, the start
  index and the stride are measured against FFmpeg and wait for the owner: ERRATA entry 44, question 43). LSB bypass is one explicit bit a
  sample from the block data under either sync and has nothing to do with the generators (§10.2.1). Generator 0, the default
  (`0x31EA`), is decoded from the specification's LFSR: two noise channels follow the matrix channels and enter through ordinary matrix
  coefficients; `shr7` is 16 bits (§17.2). No stream FFmpeg writes uses a non-zero noise coefficient or shift; hand-built streams do, and
  FFmpeg's strict decode of them equals this library's.
- Heavy DRC in the restart header (TH spec 12.2): where its bits sit cannot be told from the streams.
- The block data CRC is not checked, only the length it follows.
- The evolution frame is read (provisionally) and the object audio metadata in it is handed out as bytes, which `forge` parses
  to read and to write the objects; the signature and protection bytes are not verified, and a written frame has zero
  protection bytes.
- The encoder does not write an IIR filter, LSB bypass, a negative output shift, heavy DRC, noise or MLP
  (`ExperimentalTools::iir` and `lsb_bypass` are refused with a reason). Against FFmpeg's
  encoder the streams are smaller on most material and larger at 192 kHz (the specification's order 4 against
  FFmpeg's 7), on full-scale square waves, with a restart interval of 1 and with the downmix presentations.
- Substream 3 of the owner's discs does not decode yet (the block header's fields, question 60), and the object layer is
  provisional (entries 61 to 64). Python, JavaScript and Rust bindings over the C API
  (`iclforge_truehd_*` in [the C API](c-api.md#truehd-and-mlp)) are not written.
- Content signing: nothing signs or checks a TrueHD stream, and an evolution payload is carried, not read. The owner's
  specification describes it as the container [AC-4's content signature](signing.md#ac-4-content-signing) uses; see
  [the seam for other codecs](signing.md#the-seam-for-other-codecs).
- Protected files: a protected (Common Encryption) `mlpa` track is refused by the MP4 reader's code for protected audio, which
  names the scheme; no fixture or test covers an `mlpa` one.
- Nothing outside the library uses it but `forge` (and the C API's callers): the Forge GUI, Hearth and Crucible do not link it,
  `forge cut`, `cat`, `transcode`, `fmp4`, `monitor` and `play` refuse it by name (the last four
  read the AC-3 or E-AC-3 track after a TrueHD one in a file that has both), and no device output sends a MAT burst.
  [Development status](development-status.md#truehd-mlp) has each as a row.
