# iclforge::truehd

The Dolby TrueHD and MLP bitstream, a lossless decoder and an encoder, written from the owner's *Dolby TrueHD (MLP) Reference
Specification* v3.5 ("TH spec") and checked against streams FFmpeg's encoders write and against FFmpeg's decoder.
[planning/truehd.md](../../planning/truehd.md) is the plan; [ERRATA.md](ERRATA.md) lists, first, the questions only
the owner can answer, and then every place the specification and the streams disagree.

It links `iclforge::base` and nothing else. The option is `ICLFORGE_BUILD_TRUEHD` (on by default).

## Layout

| Directory | What |
|---|---|
| `include/iclforge/truehd/core/` | `format.hpp` (constants, sample rates, channel vocabulary), `error.hpp` (`ErrorKind`, `Error`), `crc.hpp` (the four check values), `syntax.hpp` (restart header, block header, filters, matrices, Huffman codes: a reader and a writer for each) |
| `include/iclforge/truehd/io/` | `major_sync.hpp`, `access_unit.hpp` (parse a unit, write its header and directory), `elementary.hpp` (`probe_stream`, `ElementaryReader`), `evolution.hpp` (the extra data's evolution frame and its payloads, a provisional reading) |
| `include/iclforge/truehd/decoder/` | `decoder.hpp`: `Decoder`, `DecoderConfig`, `DecodedAccessUnit`, `DecoderCounters` |
| `include/iclforge/truehd/encoder/` | `encoder.hpp`: `Encoder`, `EncoderConfig`, `EncodedAccessUnit`, `EncoderStats`, `DownmixMode` |
| `src/` | the same split; `core/huffman.hpp` holds the codebooks |
| `tests/`, `fuzz/` | Catch2 tests, `iclforge-truehd-encode-tool`, and the libFuzzer harnesses `fuzz_truehd_parse`, `fuzz_truehd_decode` and `fuzz_truehd_encode` |

## Decoding

```cpp
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

iclforge::truehd::ElementaryReader reader(stream_bytes);
iclforge::truehd::Decoder decoder;
while (const auto unit = reader.next()) {
    auto result = decoder.decode(*unit);          // std::expected<std::optional<DecodedAccessUnit>, Error>
    if (result && *result) {
        // (*result)->pcm: samples x channels, interleaved, 24-bit values in int32
    }
}
```

`decode()` returns no samples, and no error, for a unit that comes before the first major sync or before a substream's
first restart header. A failed check (the check nibble, a CRC, a parity byte, the lossless check) is an error with
`Strictness::kStrict` and a counter with `kLenient`. The decoder reads substreams 0 to 2 (up to 8 channels) and the
generic 16-channel substream 3 the same way. FFmpeg's encoders write neither 7.1 nor the sixteen-channel substream: 7.1 is
checked through this library's encoder, whose streams FFmpeg's decoder reads, and the sixteen-channel substream has no
external oracle, because FFmpeg refuses its restart sync (see the plan).

## Encoding

```cpp
#include "iclforge/truehd/encoder/encoder.hpp"

iclforge::truehd::EncoderConfig config;      // 48 kHz, 2 channels, 24 bits by default
config.channels = 6;
auto encoder = iclforge::truehd::Encoder::create(config);   // std::expected<Encoder, EncodeError>
auto units = encoder->encode(interleaved_pcm);                // the access units that are complete
auto tail = encoder->flush();                                  // pads and ends the stream
```

The encoder holds one restart interval of input before it emits it. `verify` has the decoder read every unit back; `Encoder::refusal_reason()`
names the rule a configuration breaks. See [docs/library/truehd.md](../../docs/library/truehd.md#encoding).

## Checks

`iclforge-truehd-tests` builds its own streams with the library's writers, and decodes the FFmpeg fixtures in
`testdata/truehd/` (`tools/generators/gen_truehd_ffmpeg.py` makes them) against the SHA-256 of FFmpeg's strict decode.
The hand-built streams in `testdata/truehd/handbuilt/` (`tools/references/truehd_writer.py`) cover what FFmpeg's encoders
do not write: an IIR filter, LSB bypass, several matrices with a permuted `ch_assign`, noise generator 0, the block data check,
the end-of-stream marker, extra data, the directory's DRC word and the 16-channel substream. They are held to the signal each
was made from, by this decoder and by `tools/references/truehd_syntax.py`, and FFmpeg's strict decode of eleven of the fifteen
returns that signal too (`tools/checks/verify_truehd_interop.py --handbuilt`; its manifest says what FFmpeg does with the
other four). The extra data's layout, the DRC word's values, the block data CRC and the 16-channel substream have no external
oracle.

`tools/checks/verify_truehd_encode_interop.py --encoder <iclforge-truehd-encode-tool>` encodes 43 cases and requires FFmpeg's strict decode of each
to equal the source; `tools/ci/fuzz_truehd_encoder_space.py` does the same for random legal configurations against adversarial PCM
(`--replay <seed>` reruns a case).
