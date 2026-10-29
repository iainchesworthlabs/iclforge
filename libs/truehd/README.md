# iclforge::truehd

The Dolby TrueHD and MLP bitstream and a lossless decoder, written from the owner's *Dolby TrueHD (MLP) Reference
Specification* v3.5 ("TH spec") and checked against streams FFmpeg's encoders write and against FFmpeg's decoder.
[planning/truehd.md](../../planning/truehd.md) is the plan; [ERRATA.md](ERRATA.md) lists, first, the questions only
the owner can answer, and then every place the specification and the streams disagree.

It links `iclforge::base` and nothing else. The option is `ICLFORGE_BUILD_TRUEHD` (on by default).

## Layout

| Directory | What |
|---|---|
| `include/iclforge/truehd/core/` | `format.hpp` (constants, sample rates, channel vocabulary), `error.hpp` (`ErrorKind`, `Error`), `crc.hpp` (the four check values), `syntax.hpp` (restart header, block header, filters, matrices, Huffman codes: a reader and a writer for each) |
| `include/iclforge/truehd/io/` | `major_sync.hpp`, `access_unit.hpp` (parse a unit, write its header and directory), `elementary.hpp` (`probe_stream`, `ElementaryReader`) |
| `include/iclforge/truehd/decoder/` | `decoder.hpp`: `Decoder`, `DecoderConfig`, `DecodedAccessUnit`, `DecoderCounters` |
| `src/` | the same split; `core/huffman.hpp` holds the codebooks |
| `tests/`, `fuzz/` | Catch2 tests, and the libFuzzer harnesses `fuzz_truehd_parse` and `fuzz_truehd_decode` |

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
generic 16-channel substream 3 the same way; the FFmpeg encoders write neither 7.1 nor the sixteen-channel substream,
so those have no external oracle (see the plan).

## Checks

`iclforge-truehd-tests` builds its own streams with the library's writers, and decodes the FFmpeg fixtures in
`testdata/truehd/` (`tools/generators/gen_truehd_ffmpeg.py` makes them) against the SHA-256 of FFmpeg's strict decode.
The hand-built streams in `testdata/truehd/handbuilt/` (`tools/references/truehd_writer.py`) cover what FFmpeg's encoders
do not write: an IIR filter, LSB bypass, several matrices, the block data check, the end-of-stream marker, extra data,
the directory's DRC word and the 16-channel substream. They have no external oracle: they are held to the signal each
was made from, by this decoder and by `tools/references/truehd_syntax.py`.
