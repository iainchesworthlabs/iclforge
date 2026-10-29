# TrueHD and MLP: `iclforge::truehd`

`iclforge/truehd/decoder/decoder.hpp`, with the bitstream layer under it in `iclforge/truehd/core/` and
`iclforge/truehd/io/`, all in the library `iclforge::truehd`. A reader and a lossless decoder for Dolby
TrueHD (`0xF8726FBA`, Blu-ray and HD DVD) and MLP (`0xF8726FBB`, DVD-Audio) elementary streams, written
from the owner's *Dolby TrueHD (MLP) Reference Specification*, v3.5 ("TH spec" in the code). The library links
`iclforge::base` and nothing else. The lossless encoder (`iclforge/truehd/encoder/encoder.hpp`, [Encoding](#encoding)) is
written from the same specification; the Atmos substream and the object metadata are phase T2b
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
not interpreted: `extra_data` hands back its bytes.

The syntax is also written: `write_restart_header`, `write_block_header`, `write_major_sync` and
`write_access_unit_prefix` are the readers' inverses, which the encoder is built on.

## Encoding

```cpp
#include "iclforge/truehd/encoder/encoder.hpp"

iclforge::truehd::EncoderConfig config;          // 48 kHz, 2 channels, 24 bit
config.sample_rate = 48000;
config.channels = 6;                             // the layout of TH spec 13.1 for the count: 5.1
auto encoder = iclforge::truehd::Encoder::create(config);   // std::expected<Encoder, EncodeError>
auto units = encoder->encode(interleaved_pcm);   // std::vector<EncodedAccessUnit>; mostly empty
auto rest = encoder->flush();                    // the last units, padded to a whole unit
```

It writes 1 to 8 channels of 16, 20 or 24 bit PCM at 44.1 to 192 kHz as TrueHD. A restart interval (128 access units by
default) is coded whole: the low bits every sample leaves clear become the output shift, a primitive matrix decorrelates a
channel pair where that is smaller, each channel is predicted with a FIR filter (windowed autocorrelation and the
Levinson-Durbin recursion, quantised to the widths of TH spec 9.3), and the residual of each block is coded with the shortest
of the three codebooks or raw. Channels are spread over substreams as FFmpeg's encoder spreads them (2, then 4, then 2), or,
with `DownmixMode::kStandard`, as the 2, 6 and 8 channel presentations whose lower members are downmixes. The stream is kept
within a declared peak rate and TH spec 18's 75 ms, and `EncoderConfig::verify` decodes every unit as it is written.
FFmpeg's strict decode of what it writes is the input, sample for sample (`tools/checks/verify_truehd_encode_interop.py`); it
is about 6% smaller than FFmpeg's encoder's output over the cases FFmpeg writes. It writes no noise, no IIR filter and no LSB
bypass (nothing outside this library reads them), no MLP, and no block data CRC; the Atmos substream and the evolution payload
it only frames, from bytes it is given. [`libs/truehd/ERRATA.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md)
(entries 30 to 38, questions 12 to 15) records each place the specification and the streams left it a choice.

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
| FFmpeg's strict decode (`-err_detect crccheck+bitstream+buffer+explode`) of the streams its `truehd` and `mlp` encoders write | 12 fixtures in `testdata/truehd/`: 1, 2 and 6 channels, 44.1, 48, 96 and 192 kHz, 16 and 24 bits, TrueHD and MLP; 19 more while the decoder was written | FFmpeg: sample for sample |
| FFmpeg's strict decode of the streams this library's encoder writes (`forge truehd-encode`) | 32 cases of rates, 1 to 8 channels, 16, 20 and 24 bits and signals (`tools/checks/verify_truehd_encode_interop.py`), the codec matrix's encode legs, and the gold-reference gate | FFmpeg: sample for sample |
| `tools/references/truehd_syntax.py`, an independent transcription | the same 12, and the hand-built streams | none beyond the two readings agreeing |
| Hand-built streams, `tools/references/truehd_writer.py` | an IIR filter, LSB bypass, noise generator 0, several matrices with a permuted `ch_assign`, an output shift, the block data check, the end-of-stream marker, extra data, the directory's DRC word, four substreams with a 16-channel one | **none**: held to the signal each was built from |
| DEE's streams of known sources | scored on the owner's machine by `tools/checks/score_truehd_decode.py`, which skips when the set is absent | the sources |

FFmpeg's encoders do not write an IIR filter, LSB bypass, 7.1, a non-zero noise coefficient, the Atmos substream or
extra data, so those have no external oracle. [Validation](../verification.md#truehd-and-mlp) says what that leaves.

## What it does not do

- Noise generator 1: restart sync `0x31EB` decodes (a 7.1 stream's third substream is written with it, ERRATA entry 32)
  unless a matrix bypasses an LSB, which is refused with `ErrorKind::kUnsupported`, because the bypassed bits come from
  generator 1 and its 256-entry table is not in the specification yet (§17.4). Generator 0, the default (`0x31EA`), is decoded from the
  specification's LFSR: two noise channels follow the matrix channels and enter through ordinary matrix
  coefficients. No stream FFmpeg writes uses a non-zero noise coefficient, so that has no external oracle.
- Heavy DRC in the restart header (TH spec 12.2): where its bits sit cannot be told from the streams.
- The block data CRC is not checked, only the length it follows.
- The evolution frame and the object audio metadata in the extra data are carried as bytes, not read.
- The object renderer and the Atmos substream are phase T2b. Python, JavaScript and Rust bindings over the C API
  (`iclforge_truehd_*` in [the C API](c-api.md#truehd-and-mlp)) are not written.
