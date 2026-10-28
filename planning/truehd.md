# TrueHD (MLP): a decoder, an encoder and Atmos

!!! note "Status as of 2026-10-29: phase 0 (this design) drafted; nothing built yet"
    Written on 2026-10-29 as the plan for `iclforge::mlp`, a TrueHD (MLP) encoder and decoder with
    Atmos, built fresh from the clean-room specification `truehd-spec.md` (version 3.0, 2026-10-29,
    supplied by the owner and kept outside the repository). The [Status](#status) section is kept
    current as each phase lands.

## Scope

TrueHD is Dolby's name for Meridian Lossless Packing (MLP) as used on Blu-ray and HD DVD. The
library gains a codec module `iclforge::mlp`, CMake option `ICLFORGE_BUILD_MLP`, in `libs/mlp`, laid
out like `libs/ac4`:

- a bitstream layer: access unit, major sync, substream directory, restart header, block header
  and decoding parameters, the check nibble, CRCs and parity, termination and the extra data
  (evolution) container; one description of each structure drives both the parser and the writer;
- a decoder: Huffman and LSB sample decode, FIR and IIR inverse prediction, primitive matrices,
  output shifts, noise, substream selection for the 2, 6, 8 and 16 channel presentations, random
  access, the conformance checks of the specification's Appendix E as typed errors, and the DRC
  metadata exposed to the caller;
- an encoder: matrix generation (with the downmix matrices that give the 2 and 6 channel
  presentations), filter search, quantiser and codebook choice, block size, restart interval policy,
  rate control against `peak_data_rate`, and an optional self-check by decoding; always lossless;
- Atmos: the 16-channel substream, `extra_channel_meaning`, and object audio metadata (OAMD) carried
  in evolution frames, built from the repository's object model;
- the library surface, Forge's `encode`, `decode`, `probe` and `transcode` with `codec=truehd`,
  documentation, unit and property tests, a fuzz harness, golden masters from FFmpeg, a harness
  for Dolby Encoding Engine (DEE) masters, and an interoperability gate in CI.

Out of scope for this round: either content signing scheme (see
[decision 1](#decisions-needing-owner-review)), passthrough integration beyond IEC 61937 data type 22
and MAT framing, and GUI front ends.

## Sources and the clean-room position

The only codec reference is the specification. It names its lineage (the MLP patents and the 2004
JAES paper) but gives no bit syntax for OAMD (section 15 points at ETSI TS 103 420), so the
repository's OAMD serialiser in `libs/objects` is the implementation of record for the Atmos side.

The specification has many internal contradictions (see the [register](#spec-problem-register)).
The arbiter is FFmpeg 6.1.1, used as a black box only:

- FFmpeg's experimental `truehd` encoder (`-strict -2`) makes test streams from known sources.
  Our parser must decode them sample for sample. Where the specification allows several readings,
  the reading under which our decoder reproduces the source is taken. These are FFmpeg's streams,
  not a Dolby encoder's.
- FFmpeg's `truehd` decoder decodes our encoder's output under strict error detection
  (`-err_detect crccheck+bitstream+buffer+explode -xerror`). Where a field (a CRC's initial value,
  a check nibble's coverage) cannot be settled from FFmpeg's streams alone, our encoder writes each
  candidate and the decoder's verdict settles it.
- FFmpeg's source is not read. DEE output is not analysed; DEE streams are test inputs only.

The earlier internal codec on `feature/truehd-atmos-support` (ROADMAP item IM5) is not fetched,
read or reused ([decision 2](#decisions-needing-owner-review)).

## Module layout

```
libs/mlp/
  CMakeLists.txt, README.md, ERRATA.md
  include/iclforge/mlp/
    core/stream_info.hpp        major sync, format info, channel meaning, presentations
    decoder/decoder.hpp         Decoder, DecoderConfig, DecodedAccessUnit, errors
    encoder/encoder.hpp         Encoder, EncoderConfig, EncoderStats
    io/elementary.hpp           access-unit framing of a .thd / .mlp byte stream, probe
    io/extra_data.hpp           evolution payload carriage (the signing hook)
  src/
    core/   syntax.hpp/.cpp     the shared field descriptions (read and write from one table)
            crc.hpp/.cpp        CRC-8 0x1D, CRC-16 0x2D, parity, check nibble
            huffman.hpp/.cpp    the Appendix C codebooks, consteval-checked
            filters.hpp/.cpp    FIR / IIR prediction, shared by encoder and decoder
            matrix.hpp/.cpp     primitive matrices, shared
            lossless_check.*
    decoder/ decoder.cpp, substream.cpp, block.cpp, presentation.cpp
    encoder/ encoder.cpp, analysis (matrix, lpc, filter pool), coder (huffman choice),
             rate_control.cpp (FIFO model), writer.cpp
    io/     elementary.cpp, extra_data.cpp, probe.cpp
    atmos/  oamd_carriage.cpp   OAMD to and from evolution payloads via libs/objects
  tests/    one binary, ctest label mlp
  fuzz/     fuzz_mlp_parse.cpp, fuzz_mlp_decode.cpp, seeds/
```

The module links `iclforge::base` (bit reader and writer, CRC helpers, speakers) and
`iclforge::objects` (the OAMD payload). It links nothing of `libs/ac3` or `libs/ac4`.

## How the specification maps to code

| Specification | Code |
|---|---|
| 3 access unit, 3.2 check nibble | `core/syntax` `AccessUnitHeader`; `core/crc` `check_nibble` |
| 4, 5, 14 major sync, format info, channel meaning | `core/stream_info.hpp` `MajorSync` |
| 6 substream directory | `core/syntax` `SubstreamDirectoryEntry` |
| 7 restart header | `core/syntax` `RestartHeader` |
| 8 block header, channel parameters | `core/syntax` `BlockHeader`, `ChannelParams` |
| 9 filters | `core/filters` |
| 10 primitive matrices | `core/matrix` |
| 11, Appendix C Huffman | `core/huffman` |
| 12 DRC | `decoder/decoder.hpp` `DrcMetadata` |
| 15, 16 OAMD, evolution | `io/extra_data`, `atmos/oamd_carriage` |
| 18 FIFO | `encoder/rate_control` |
| 21 transport | `libs/containers` (IEC 61937 type 22, MAT; Matroska `A_TRUEHD`; MPEG-TS 0x83) |
| Appendix E | `decoder` `ErrorCode` values, one per check point |

## Test and oracle plan

1. Unit tests per layer: each structure round-trips through the shared description; CRCs against
   hand-computed vectors; the Huffman tables are prefix-free and their escape rule holds
   (`consteval`).
2. Property tests: random channel count, rate, depth, block size, silence, full scale, clipping
   edges and noise; encode then decode, and the PCM must be identical.
3. FFmpeg golden masters in `testdata/truehd/` made by
   `tools/generators/gen_truehd_ffmpeg_gold.py` (never in CI): 2, 6 and 8 channels, 48 and 96 kHz,
   16 and 24 bit, with a SHA-256 manifest of each stream and its source. Our decoder must reproduce
   each source.
4. DEE masters: a harness reads `ICLFORGE_DEE_GOLD_DIR` (the local set of
   `tools/generators/gen_dee_gold.py`), decodes every TrueHD leg with our decoder and FFmpeg and
   requires both to equal the source where the manifest says audio was not altered; it skips with a
   message when the variable is unset. A small exporter copies a hash-pinned subset into
   `testdata/` for the owner to commit.
5. `tools/checks/verify_truehd_interop.py`: forge encode then FFmpeg decode equals the source;
   forge decode of the committed FFmpeg streams equals the source; forge round trip; Atmos round
   trip of bed, objects and OAMD, and FFmpeg decodes the Atmos stream without error. Wired into
   `_build.yml` and `interop.yml`.

## Spec problem register

Each entry: what the specification says, the reading taken, and the evidence. Entries marked
"open" are settled as the oracle work proceeds.

| # | Where | Problem | Reading | Evidence |
|---|---|---|---|---|
| P1 | 1.2, 3.3, 8.4, F | Samples per access unit given as 40 and as 320; 3.3 also says "[16, 256)"; "8 blocks per access unit" at 40 samples gives 5-sample blocks, which contradicts the block size range 8 to 160 in multiples of 8 | open | |
| P2 | 4.4 | `rate_multiplier` is not defined; 15 bits of 200 bps cannot reach the 18 Mbps the text cites | open | |
| P3 | 9.1, 9.2 | Four filter "types" against two filters, A (FIR) and B (IIR) | Two filters per channel, A and B; the four "types" are combinations of their orders | 9.2's syntax |
| P4 | Appendix C | Huffman tables | All three codebooks are prefix-free; each leaves exactly two 9-bit words unused (`000000000`, `010000000`), both with a zero ninth bit, which agrees with 11.4's escape rule | Kraft sum 255/256, checked `consteval` |
| P5 | 2.3, 7.2, A | Restart sync words for substreams 1 to 3: 2.3 gives 0x31EB for 1 and 0x31EC for 2 and 3; 7.2 lets 0x31EA serve substream 1 and 0x31EB substream 2 | open | |
| P6 | 7.1 | Restart sync word width (14 bits) | open | |
| P7 | 3.5, A | Termination word 0xD234E000 (32 bits) against a "substream 3 terminator" 0x1234 | open | |
| P8 | 6.2, 8.1 | `restart_header_exists` appears both at the start of a substream segment and inside each block header; "8 blocks" per segment against a `last_block_in_segment` bit | open | |
| P9 | 4.1 | `flags`: bit 14 is inside the non-reserved mask 0xD800 yet SHALL be clear | open | |
| P10 | 11.1, 11.3 | Raw-only samples (`huff_type` 0) read `huff_lsbs` bits in 11.1 and use `lsbs_bits` in 11.3 | open | |
| P11 | 8.2 | `param_presence_flags` is read but no field says which parameter each bit gates | open | |
| P12 | 4.1, B | CRC-16 0x2D and CRC-8 0x1D: initial value, reflection and covered bytes not given | open | |
| P13 | 6.2, B | Parity: which bytes are XORed, and Appendix B's "CRC-8 (parity) 0x63" | open | |
| P14 | 7.3 | Lossless check: which samples (before or after matrixing, which channels) and where the value for an interval is carried | open | |
| P15 | 9.4, 9.5 | IIR state update `(residual - predicted) << state_shift` and the order of A and B in inverse prediction | open | |
| P16 | 16.1 | Extra data length field: "12 bits" with "4 bits part of AU header parity" | open | |
| P17 | 22, ROADMAP | Signing: pace-ccjf1 ECDSA in the specification, truncated HMAC-SHA-256 in ROADMAP.md and `docs/concepts/object-signing.md` | carriage only, no scheme | [decision 1](#decisions-needing-owner-review) |

## Decisions needing owner review

1. **Signing.** Section 22 says TrueHD uses the `pace-ccjf1` ECDSA-P256 content signature of the
   AC-4 signing specification, carried as extra data or an evolution payload. ROADMAP.md (IM5) and
   `docs/concepts/object-signing.md` say TrueHD uses a truncated HMAC-SHA-256 "Evolution frame
   protection". The two disagree, and neither is implemented in this round. What is built is the
   carriage: an evolution payload can be written into and read back from an access unit's extra
   data through a named interface, and the hook is documented so the codec-agnostic signing core
   being built in `libs/base` can use it. Recommendation: follow the specification (pace-ccjf1),
   since it is the newer and more specific source, and correct ROADMAP.md and the concepts page
   once the owner agrees.
2. **The old branch.** `feature/truehd-atmos-support` is not fetched, read or reused; the module is
   written fresh from the specification under the present clean-room ruling.
3. **FFmpeg as arbiter.** Where the specification contradicts itself, the reading under which
   FFmpeg's decoder accepts our streams and our decoder reproduces FFmpeg's streams is taken, and
   each is listed in the register and in `libs/mlp/ERRATA.md`.

## Status

| Phase | What | State |
|---|---|---|
| 0 | this design | drafted 2026-10-29 |
| 1 | bitstream layer | not started |
| 2 | decoder | not started |
| 3 | encoder | not started |
| 4 | Atmos | not started |
| 5 | library surface, CLI, docs | not started |
| 6 | tests and gates | not started |
