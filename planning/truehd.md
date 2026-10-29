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
| P1 | 1.2, 3.3, 8.4, F | Samples per access unit given as 40 and as 320; 3.3 also says "[16, 256)"; "8 blocks per access unit" at 40 samples gives 5-sample blocks, which contradicts the block size range 8 to 160 in multiples of 8 | An access unit is 1/1200 s: 40 samples at 44.1 and 48 kHz, 80 at 88.2 and 96 kHz, 160 at 176.4 and 192 kHz. "8 blocks" and 320 are wrong | FFmpeg streams: `output_timing` advances 640 per 16 access units at 48 kHz and 1280 at 96 kHz; `input_timing` steps 40 at 48 kHz |
| P2 | 4.4 | `rate_multiplier` is not defined; 15 bits of 200 bps cannot reach the 18 Mbps the text cites | `peak_data_rate` is in units of fs/16 bits per second: rate = raw x fs / 16, so 18 Mbps at 48 kHz is 6000. The bit before it is `is_vbr` | FFmpeg writes 3199 at 48 kHz and 1599 at 96 kHz for the same 9.6 Mbps ceiling |
| P3 | 9.1, 9.2 | Four filter "types" against two filters, A (FIR) and B (IIR) | Two filters per channel, A and B; the four "types" are combinations of their orders | 9.2's syntax |
| P4 | Appendix C | Huffman tables | All three codebooks are prefix-free; each leaves exactly two 9-bit words unused (`000000000`, `010000000`), both with a zero ninth bit, which agrees with 11.4's escape rule | Kraft sum 255/256, checked `consteval` |
| P5 | 2.3, 7.2, A | Restart sync words for substreams 1 to 3: 2.3 gives 0x31EB for 1 and 0x31EC for 2 and 3; 7.2 lets 0x31EA serve substream 1 and 0x31EB substream 2 | 0x31EA serves substreams 0 and 1 (FFmpeg writes it in both); substream 2 takes 0x31EB: FFmpeg refuses 0x31EA there ("8 channels ... not implemented" for a 7-channel substream, which it counts as `max_matrix_chan + 2`) and rejects 0x31EC ("restart header sync incorrect"); 0x31EC is kept for the 16-channel substream | FFmpeg decodes our 7.1 streams exactly with 0x31EB in substream 2 |
| P6 | 7.1 | Restart header: 14-bit sync, then a `noise_type` bit, then `output_timing`; then 19 reserved bits, and later `max_shift`, `max_lsbs`, `max_bits`, `max_bits_repeat` | No separate `noise_type` bit: `output_timing` follows the 14-bit sync directly. The "19 reserved bits" are `max_shift` 4, `max_lsbs` 5, `max_bits` 5, `max_bits_repeat` 5, and the second copy of those fields, the hires bit and `heavy_drc_present` do not exist. Order: sync 14, output_timing 16, min_chan 4, max_chan 4, max_matrix_chan 4, noise_shift 4, noisegen_seed 23, max_shift 4, max_lsbs 5, max_bits 5, max_bits_repeat 5, data_check_present 1, lossless_check 8, reserved 16, ch_assign 6 x (max_matrix_chan + 1), CRC 8 | Restart-header CRC verifies over 107 FFmpeg headers at this extent only |
| P7 | 3.5, A | Termination word 0xD234E000 (32 bits) against a "substream 3 terminator" 0x1234 | The word ends the last access unit of a stream, after the last block and its padding and before the check bytes. Its top 20 bits are 0xD234E and its low 12 the number of padding samples at the end of the access unit, which the decoder drops. The "0x1234" terminator is not seen | FFmpeg writes 0xD234E017 after a source 17 samples into its last 40-sample access unit, and its decoder then gives exactly the source's length; our decoder does the same (`stereo-48k-24-odd`) |
| P8 | 6.2, 8.1 | `restart_header_exists` appears both at the start of a substream segment and inside each block header; "8 blocks" per segment against a `last_block_in_segment` bit | A segment is blocks only: each block starts with `block_header_exists`, then `restart_header_exists` inside it; 6.2's segment-level bit is the first block's. Block count is open | FFmpeg segments start `11` then the 0x31EA sync |
| P9 | 4.1 | `flags`: bit 14 is inside the non-reserved mask 0xD800 yet SHALL be clear | open | |
| P10 | 11.1, 11.3 | Raw-only samples (`huff_type` 0) read `huff_lsbs` bits in 11.1 and use `lsbs_bits` in 11.3 | open | |
| P11 | 8.2 | `param_presence_flags` is read but no field says which parameter each bit gates | Bits 7 to 1 gate, in order, the `new_*` bits of block size, matrix, output shift, quantiser step, filter A, filter B and Huffman offset; a clear bit means the `new_*` bit is absent. Bit 0 has no known role. The flags are 0xFF after a restart. Our encoder never sends `new_guards`, so it depends on none of this; the decoder follows it | FFmpeg streams parse with flags 0xFF throughout; the reading of a clear bit is unverified |
| P12 | 4.1, B | CRC-16 0x2D and CRC-8 0x1D: initial value, reflection and covered bytes not given | Every MLP CRC is the unaugmented polynomial remainder, MSB first: bits are shifted into the register's low end and the register is reduced when its top bit leaves. Major sync: 0x2D, initial 0, over all 26 bytes before the CRC field (equivalently, the ordinary MSB-first CRC of the first 24 bytes XOR bytes 24 and 25). Restart header: 0x1D, initial 0, from the sync word to the end of `ch_assign` | Major sync: 5 FFmpeg streams, and a stream with a bit set in byte 24 that FFmpeg's strict decoder accepts only under this rule. Restart header: 107 headers |
| P13 | 6.2, B | Parity: which bytes are XORed, and Appendix B's "CRC-8 (parity) 0x63" | `substream_end_ptr` counts 16-bit words from the end of the substream directory. Parity byte = XOR of every segment byte before it, XOR 0xA9. The substream CRC uses polynomial 0x63, not 0x1D, unaugmented remainder with initial value 0xA2 over the segment bytes before the parity byte | 1,320 FFmpeg segments, every one |
| P14 | 7.3 | Lossless check: which samples (before or after matrixing, which channels) and where the value for an interval is carried | The value in a restart header checks the interval that ends there (the first restart header's is 0). It is computed over the presentation's output samples after output shift and channel assignment: for every sample, each output channel c contributes its low 24 bits rotated left by c (mod 24) into a 24-bit XOR accumulator, and the check is the XOR of the accumulator's three bytes. For substream 1 the presentation is the 6-channel one, so its check covers substream 0's channels too. 7.3's plain byte XOR is wrong | Every interval of three FFmpeg streams: 37, 22 + 22 and 22 intervals |
| P15 | 9.4, 9.5 | IIR state update `(residual - predicted) << state_shift` and the order of A and B in inverse prediction | One prediction: the sum of filter A's coefficients times past samples and filter B's times its history, shifted down by the shared `coeff_q`, then rounded down to the quantiser step. Filter B's history holds each sample less the prediction before that rounding. `state_shift` scales only the states a header sends | FFmpeg's decoder gives our IIR streams' sources exactly: with quantiser step 0 (24-bit legs) and step 1 (16-bit legs, which settled the unrounded reading against three others) |
| P16 | 16.1 | Extra data length field: "12 bits" with "4 bits part of AU header parity" | open | |
| P18 | brief | FFmpeg 6.1.1's `truehd` encoder takes at most 5.1 (mono to 5.1(side)); asked for 7.1 it silently writes 5.0 | FFmpeg golden masters are 2 and 6 channel only; 8 channel interoperability rests on our encoder and FFmpeg's decoder | `ffmpeg -h encoder=truehd`; a 7.1 WAV came back as 5.0 |
| P19 | 9.2 | `new_states` is shown for filter B only, yet 9.2 also says filter A "SHALL NOT have new_states set" | The bit is read after the coefficients of both filters; it must be 0 for filter A | FFmpeg silent stream: the bit after an order-1 FIR's coefficient is 0, and only that reading lines up the next channel's parameters |
| P20 | 8.2 | Block 1 of an FFmpeg access unit has `block_size` 8 and block 2 has 32 | Access units may hold blocks of different sizes summing to the access unit's samples | FFmpeg silent stream |
| P21 | 10.2, 11.1 | `matrix_parameters()` reads a coefficient for channels 0 to `max_chan`; nothing reads the LSB that `lsb_bypass` says is bypassed | The coefficient loop runs over `max_chan + 3` channels (the two extra are the noise channels of 17); each sample row starts with one bit per primitive matrix whose `lsb_bypass` is set, before the channels' samples | Only this reading parses 300 access units of FFmpeg's 96 kHz stream, whose matrix has `lsb_bypass` set |
| P22 | 7.1, 7.2, 10.2 | A `noise_type` bit after the 14-bit sync does not exist (P6), yet 10.2 reads `matrix_noise_shift` when `noise_type` is set | `noise_type` is the sync word's last bit: 0x31EA (and 0x31EC) are noise type 0, 0x31EB noise type 1; see P37 for what each changes | FFmpeg's reported errors on three layouts of the same substream (P37) |
| P23 | 11.2, 11.3, C | The codebooks' symbol ranges are given as -7 to +10, -7 to +8 and -7 to +7, and 11.3 adds `huff_offset` to the MSBs with no further bias | The table's symbol is the MSB value. Reconstruction for `huff_type` t in 1 to 3 with `lsbs_bits` n: `(msbs << n) + raw_lsbs - (1 << (n + 2 - t)) + huff_offset`, then `<< quant_step_size`, then prediction; for `huff_type` 0, `raw_lsbs - (1 << (n - 1)) + huff_offset` as 11.3 says. The bias centres each codebook: 2, 1 and 0.5 MSB steps | The scratch model decodes FFmpeg's 2-channel 48 kHz 16-bit, 2-channel 96 kHz 24-bit, 6-channel 48 kHz 16-bit and silent streams to their sources exactly |
| P24 | 4.3, A | Signature 0xB753 for TrueHD and 0xB752 for MLP | FFmpeg writes 0xB752 in a 0xBA stream and its decoder accepts both. Our encoder writes 0xB752; our decoder accepts either in a 0xBA stream and reports which | FFmpeg streams; a crafted stream with 0xB753 |
| P25 | 5.1, 13.2 | 13.2 labels assignment 0x0B as 5.1 and 0x0F as 7.1, against 13.1's bit table (bit 0 L/R, 1 C, 2 LFE, 3 Ls/Rs, 6 Lrs/Rrs) | 13.1's bits are right: 0x0B is 5.0, 0x0F is 5.1, 7.1 is 0x4F. FFmpeg writes `format_info` 0x0050A001 for stereo (2-bit modifiers 1, 1, 1; six- and eight-channel assignments 1), 0x0007800F for 5.1 and 0x0005800B for 5.0; `substream_info` 0x14 with one substream and 0x3C with two | FFmpeg streams, and its decoder's channel layouts for them |
| P26 | 6.2 | Segment end | After the last block (its `last_block_in_segment` bit set) the segment is zero-padded to a 16-bit boundary, then the parity and CRC bytes when `crc_present` | Every FFmpeg segment |
| P27 | 17 | No noise generator algorithm is given for `noisegen_seed` | The decoder refuses a matrix that takes a noise channel with a typed error; the encoder never uses them | No source |
| P28 | 4.1, 14.2 | Where `extra_channel_meaning()` lies, and the unit of `extra_channel_meaning_length` | It follows the 64-bit channel meaning inside the major sync, before the CRC, and the CRC covers it. Its length is `extra_channel_meaning_length + 1` 16-bit words, the 4-bit length field included; the bits after its fields are padding | FFmpeg's strict decoder accepts a crafted stream exactly when the block is placed and sized this way (L = 0, 1, 2, 3, 7, 15) and rejects every other placement tried |
| P29 | 2.2, 6, 10 | How a substream's presentation uses the channels of the substreams below it | Each substream decodes its channels `min_chan` to `max_chan`; its matrices then run over channels 0 to `max_chan`, the lower ones taken from the lower substreams before their own matrices, and its output shift and `ch_assign` give the whole presentation. Its lossless check covers that whole presentation (P14) | FFmpeg decodes exactly our 5.1 and 7.1 streams whose upper substreams undo downmix folds of the lower ones |
| P30 | 8.1 | `block_data_crc`: coverage and initial value not given | Read and not checked; our encoder never sets `data_check_present` | FFmpeg never sets it |
| P31 | (FFmpeg) | FFmpeg's demuxer does not recognise its own 44.1 kHz TrueHD output (`Invalid data found when processing input`) | Black-box finding only: forcing `-f truehd` decodes it exactly. The interop gate always passes `-f truehd` | `stereo-44k-16` |
| P32 | 9.3, E | "Filter output SHALL not overflow" read as a ±2^23 bound on the prediction | A prediction may pass ±2^23 near full scale; FFmpeg's streams do, on a full-scale square wave. The decoder bounds the reconstructed sample to 24 bits and the prediction to the filters' 32-bit state | `stereo-48k-24`, `51-48k-24` |
| P33 | 3.4, 18 | The FIFO model behind `input_timing` is not given | Access unit n is decoded from sample n x T and must have arrived whole by then; its bits arrive at the peak rate from its input timing, which is at most (n - 1) x T (FFmpeg's timing), no earlier than the previous unit's end or a quarter unit after its start, and no more than 75 ms before its decode time. Each restart interval is placed latest first, so a large unit borrows the slack of earlier ones | Our reading; FFmpeg's decoder does not check timing. Matches FFmpeg's timing wherever no unit exceeds its slot |
| P34 | 6.4 | `substream_info` bits | FFmpeg writes 0x14 (one substream) and 0x3C (two); we write those, 0x7C for three and 0xFC for four | FFmpeg's decoder accepts all four |
| P35 | 7, 20.3 | Whether a restart header clears the decoder's filter state | FFmpeg's decoder keeps a channel's filter coefficients and history across a restart header that does not resend them; FFmpeg's encoder codes the first 8 samples after a restart unpredicted. Our encoder does both of FFmpeg's things (an 8-sample unpredicted block, every parameter resent) so that its streams decode alike whatever a decoder does; our decoder clears the state, as 20.3's random access needs | Our first encoder streams decoded exactly in FFmpeg up to the second restart and not after |
| P36 | 8.2, 9.3 | `output_shift` is a 4-bit signed field, so at most 7 | A source's trailing zeros past 7 go to the quantiser step: a 16-bit source in 24-bit words is shift 7 with step 1, as FFmpeg codes it | FFmpeg's 16-bit streams; ours decode exactly in FFmpeg |
| P37 | 10.2, 17 | The two noise mechanisms | Noise type 0: a primitive matrix takes coefficients for `max_chan + 3` inputs, the last two noise channels, and no noise shift. Noise type 1: it takes coefficients for `max_chan + 1` inputs and is followed by a 4-bit `matrix_noise_shift`. With no generator given (P27), our decoder refuses a non-zero noise coefficient or noise shift, and our encoder writes neither | Of eight layouts tried offline on two streams, only this one gives the output channels FFmpeg's decoder reported misreading (10 and 13); written this way the streams decode exactly |
| P38 | 13.1, (FFmpeg) | FFmpeg's channel order | FFmpeg outputs a 7.1 presentation in its native order, the rear pair (13.1 bit 6, Lrs and Rrs) before the side pair (bit 3, Ls and Rs); stereo and 5.1 come out in 13.1's order. 6.1 (13.1's 0x08F, Cs in bit 7) comes out with Cs before the side pair. The interop gate compares in FFmpeg's order | Our streams decode in FFmpeg equal to the source under the orders 0 1 2 3 6 7 4 5 (7.1) and 0 1 2 3 6 4 5 (6.1) |
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
3. **Downmix by default.** For 5.1 and 7.1 the encoder makes the 2-channel (and for 7.1 the
   6-channel) presentation a fold-down of the full mix (C and the surrounds at -3 dB, the
   limiter scaling the fold down when it would clip), undone losslessly by the upper substreams'
   matrices. FFmpeg's encoder instead carries the first two channels as the 2-channel
   presentation. The fold costs bits, signal-dependently: on the 5.1 golden master's source
   it costs about half again (136 KB against 91 KB unfolded; FFmpeg writes 111 KB).
   `truehd-downmix=0` (EncoderConfig::downmix = false) gives FFmpeg's layout.
4. **FFmpeg as arbiter.** Where the specification contradicts itself, the reading under which
   FFmpeg's decoder accepts our streams and our decoder reproduces FFmpeg's streams is taken, and
   each is listed in the register and in `libs/mlp/ERRATA.md`.

## Status

| Phase | What | State |
|---|---|---|
| 0 | this design | drafted 2026-10-29 |
| 1 | bitstream layer | core done 2026-10-29: structures, one syntax description (`src/core/syntax.hpp`), access unit framing, CRCs, Huffman; `iclforge-mlp-tests` passes. Extra data container and public `io/elementary.hpp` next. A scratch Python model that decodes all four FFmpeg test streams exactly is at `/tmp/claude-0/-home-claude/4316ae20-e191-53b7-a454-66eafc52d7e3/scratchpad/exp/model.py` (outside the repository, may be lost) |
| 2 | decoder | core done 2026-10-29: `libs/mlp/src/decoder/decoder.cpp` decodes all eleven FFmpeg golden masters exactly (`iclforge-mlp-tests "[gold]"`), 2-channel presentation of 5.1, typed errors. Not yet exercised: IIR filters, matrices in upper substreams, 8 and 16 channels (need our encoder) |
| 3 | encoder | core done 2026-10-29: plan (presentations, downmix with limiter, decorrelation), filter pool (FIR by Levinson-Durbin, FIR+IIR by two-stage least squares), exact Huffman costing, FIFO schedule, self-verification. Streams of 1 to 8 channels at 44.1 to 192 kHz, 16 to 24 bit decode exactly in our decoder and in FFmpeg's. Next: block-size choice, rate-control test |
| 4 | Atmos | done 2026-10-29: `iclforge/mlp/encoder/atmos.hpp` (AtmosEncoder, decode_atmos, render_gains), forge `truehd-atmos-encode`/`truehd-atmos-decode`; bed, objects and OAMD round-trip exactly; FFmpeg decodes the stream as its 8-channel presentation, equal to ours |
| 5 | library surface, CLI, docs | forge `truehd-encode`, and TrueHD in `decode`, `probe` (table and json=1), `transcode` landed 2026-10-29; public headers core/, io/, decoder/, encoder/. Docs, CHANGELOG, ROADMAP not yet |
| 6 | tests and gates | FFmpeg golden masters committed; `tools/checks/verify_truehd_interop.py` passes all four legs locally (204 checks, full matrix). DEE harness and exporter, fuzz harness, CI wiring next |
