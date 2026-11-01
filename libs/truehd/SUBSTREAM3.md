# Substream 3: the object substream

This is a developer's reference for the fourth substream of a TrueHD Atmos stream, the one whose restart header carries sync `0x31EC`.
It states what is on the wire, what the decoder does with each field, and how much evidence stands behind each statement.
[ERRATA.md](ERRATA.md) (entries 60 to 69) holds the history of how these facts were found; some of its paragraphs were corrected
by later ones, and this file follows the latest state of the code. Where this file and a paragraph of ERRATA differ, the code is
the reference and the difference is a defect in one of the two documents.

The sources are the owner's streams and the library's own tests. The closed specifications are not quoted and no section numbers
are given. Streams that the owner's machine holds (`z_short.mlp`, `EX_00002a.thd`, `EX_00002b.thd`, `soll_e12.mlp`, `soll_sil6.mlp`,
and the controlled encodes `ctl_a` to `ctl_d`) are not in the repository; the numbers quoted for them come from the owner's runs of
the lab tool (section 7).

## Evidence tags

| Tag | Meaning |
|---|---|
| VERIFIED | A stream of the owner's decodes losslessly because of it, or a hand-built test holds it and real streams agree. |
| VERIFIED (hand-built only) | A hand-built stream of this repository holds the decoder's reading and the Python reference decodes it the same way, sample for sample. No owner's stream shows the rule and no outside decoder reads it, so it is provisional. |
| PARSE-ONLY | The field is read to the right length (the fields after it then read as expected), but no stream shows what it does. |
| OPEN | The meaning is unknown or the decoder's reading has no stream behind it. |

## 1. What substream 3 is

A TrueHD Atmos stream has four substreams. Substreams 0 to 2 are the ordinary ones: they carry the rows of a 7.1 bed in the usual
way (2 channels, then 6, then 8 as the presentations a decoder may stop at). Substream 3 carries the object channels. The major
sync of such a stream has four substreams, `substream_info` `0xFC`, `extended_substream_info` 3 (the low pair of its four bits) and the
16-channel extension (ERRATA 47, 63). The segment of substream 3 may end in terminatorB, `0x1234`, or in zero padding.

All substreams share one buffer of rows for the access unit: one row for each sample time, holding the matrix channels 0 to 15
and two noise columns. Each substream codes a range of rows, `min_chan` to `max_chan`, and its matrices combine rows into rows.

| Substream | `min_chan` to `max_chan` | `max_matrix_chan` | Restart sync |
|---|---|---|---|
| 0, 1, 2 | the bed rows (0 to 7 between them) | up to 7 | `0x31EA` or `0x31EB` |
| 3 on the disc and in DEE's encodes | 8 to N-1 | N-1 | `0x31EC` |

N is the number of elements: 12, 14 or 16 in the encodes seen. The coded rows of substream 3 are the rows above the bed. Its
matrices read the 7.1 bed's rows 0 to 7 (as substreams 0 to 2 decoded them) together with the coded rows, and write the rows
0 to N-1 that become the object channels. In DEE's own encodes the bed is a dynamic downmix of the objects, so the matrices are the
inverse of that downmix and differ from restart to restart. The library's encoder writes a different form of substream 3: `min_chan`
0, no matrix, independent channels (ERRATA 63). The decoder reads both.

**Which rows the matrices read.** A substream's matrices change that substream's output rows and nothing a later substream reads
(ERRATA 34). Substream 3 therefore starts from the rows substreams 0 to 2 decoded before their own matrices. The decoder keeps the coded
rows in one buffer and works the matrices on a copy. The opposite reading (substream 3 reading the bed after substream 2's matrices)
was tried and made substream 3 worse (section 5). VERIFIED.

**`ch_assign`.** The restart header lists N six-bit values. `ch_assign[m]` is the output channel that matrix channel m goes to. Output
channel p takes the matrix channel m with `ch_assign[m] == p`. The values must form a permutation of 0 to N-1; the decoder refuses a
repeated value. `output_shift` is indexed by matrix channel m, not by output position p (ERRATA 43, 45). VERIFIED on the disc, on DEE's
encodes and in hand-built streams, in substreams 0 to 2; the `ch_assign` of substream 3 on the disc is a permutation (ERRATA 65).

**What the lossless check folds.** Each restart header carries one byte, the check of the interval that ends at that header. For every
sample time and every output channel p the decoder takes the output row m = the matrix channel p takes, applies the output shift of m
(left shift for a positive value, arithmetic right shift for a negative one), keeps the low 24 bits, shifts them left by **m modulo 8**
and XORs the result into a 32-bit fold. The check byte is the XOR of the four bytes of the fold. Each substream folds its own rows
(`max_matrix_chan` + 1 of them); substream 3 folds all N, the bed rows included. VERIFIED for substreams 0 to 2 (732, 54, 927 and 1,077
intervals over the owner's files, all passing). VERIFIED for substream 3 on the owner's synthetic encodes: `z_short`, `ctl_a`, `ctl_b`,
`e1` and the other single-object streams, and with the modulo 8 also the dense-chain controls (`e2`, `e8`, `e12`, `d3`, `ctl_d`) at 93
of 93. The modulo is what a shift by the plain channel number gets wrong from channel 8 up: 24 + m is then above 32 bits and the top
bits of a sample (the sign bits of a negative one) fall out of the word, so that the byte differs by a fixed pattern whose parity is that of
the number of negative samples in the interval (07 or 00 in `e8`). A shift by m modulo 8 gives the byte a rotation of the word by m gives
(ERRATA 69). A file of real music still passes only part of its intervals (section 6).

**Restart interval, block, access unit.** On the disc the restarts of substream 3 are 128 units apart (ERRATA 67). Every owner file has one
block per access unit, so the position of a block in its unit has not been exercised against real streams.

## 2. The block and its header, in stream order

### Block framing

| Field | Bits | Decoder | Evidence |
|---|---|---|---|
| header present | 1 | 0: a continuation block with no header | VERIFIED |
| restart present | 1 (only if the first is 1) | 1: the restart header follows, then the block header | VERIFIED |
| restart header | 121 + 6N | section 4 | VERIFIED |
| block header | variable | below | VERIFIED |
| block data length | 16 (only if `data_check_present`) | counts from the start of this field, checked in strict mode | VERIFIED (via the substreams 0 to 2) |
| block data | variable | below | VERIFIED |
| block data CRC | 8 (only if `data_check_present`) | skipped, not checked | PARSE-ONLY |
| last block | 1 | 1 ends the segment | VERIFIED |

The block data of one sample time is: first `field_2b` bits for each matrix that has LSB bypass (`field_2b` not 0), in matrix order, then for each coded channel
(`min_chan` to `max_chan`) its Huffman code, if `huff_type` is not 0, and then `huff_lsbs` less the quantiser step raw bits.

### Block header

Every group below is sent only if its bit in `param_presence_flags` is set (`0xFF` after a restart).

| Field | Bits | What the decoder does | Evidence |
|---|---|---|---|
| new presence flags present | 1 | 1: 8 bits of flags follow and replace the current flags | PARSE-ONLY: the streams seen send `0xFF` throughout, so the assignment of the flag bits to groups is a reading, not a finding |
| block size present (flag `0x80`) | 1 | | VERIFIED |
| block size | 9 | samples in the block, 8 to 160 | VERIFIED (continuation blocks of 40) |
| matrices present (flag `0x40`) | 1 | 1: the matrix section follows | VERIFIED |
| output shift present (flag `0x20`) | 1 | | VERIFIED |
| output shift | 4 signed, N times | one per matrix channel | VERIFIED when equal for all rows (4 in `ctl_c` and `ctl_d`, 1 in `EX_00002b`); a different value per row has not been seen in substream 3 |
| quantiser step present (flag `0x10`) | 1 | | VERIFIED |
| quantiser step | 4, `max_chan` + 1 times | one per row from 0 to `max_chan` (every row in substream 3); a coded row shifts its residual up by it | VERIFIED for the residuals. Its mask on the matrix outputs: OPEN, see below |
| for each coded channel (flag `0x01`): channel present | 1 | | VERIFIED |
| FIR present (flag `0x08`) | 1 | filter follows | VERIFIED |
| IIR present (flag `0x04`) | 1 | filter follows | VERIFIED |
| huff offset present (flag `0x02`) | 1 | | VERIFIED |
| huff offset | 15 signed | added to every residual of the channel | VERIFIED |
| `huff_type` | 2 | 0: raw bits only; 1 to 3: a codebook | VERIFIED |
| `huff_lsbs` | 5 | 31 at most under `0x31EC` (24 under the other syncs) | VERIFIED |

A filter is: order 4, shift 4, coefficient width 5, coefficient shift 3, `order` coefficients of the stated width (stored shifted left
by the coefficient shift), a state flag 1, and if set the state width 4, state shift 4 and `order` state values. The state flag is
sent for a FIR filter too; a FIR filter with a state is refused. The FIR order is at most 8 and the IIR order at most 4, with the two
at most 8 together and equal shifts. VERIFIED (these sections read to closure in every unit of every owner file that parses, and decode
losslessly in substreams 0 to 2 and in the coded rows of substream 3).

**The quantiser mask on matrix outputs.** The decoder clears the low `quant_step` bits of a matrix's output (for an output row up to
`max_chan`, which is every row in substream 3), as the standard matrix does. No stream discriminates it: switching it off, or applying it
to the coded rows (8 and up) only or to the bed rows only, changes no count of passing intervals (section 5). Hand-built `hb_substream3_object_bypass` holds the decoder's reading. OPEN.

### The matrix section (when the matrices-present bit is 1)

N is `max_matrix_chan` + 1. The matrices of substream 3 are numbered in the order their headers are sent.

| Field | Bits | What the decoder does | Evidence |
|---|---|---|---|
| A: coefficients follow | 1 | 0: the set stands as it is, with its running coefficients (not rebuilt from the last raw values) and its deltas, and go to A2 | VERIFIED on the owner's corpus (309 sections with A = 0 in `soll_e12`) |
| B: headers follow | 1 (only if A) | 0: reuse the last set's headers and read coefficients only | VERIFIED |
| count less one | 4 (only if B) | 1 to 16 on the wire; the decoder refuses more than 15 | VERIFIED |
| per matrix, output channel | 4 | row the matrix writes (0 to 15, may exceed `max_matrix_chan`) | VERIFIED |
| `frac_bits` | 4 | 0 to 15 | VERIFIED |
| `f_shift` + 1 | 3 | `f_shift` is -1 to 6 | VERIFIED |
| `field_2b` | 2 | not 0: the matrix has an LSB bypass of `field_2b` bits a sample (1, 2 and 3 are seen) | the bit count: VERIFIED on `ctl_c` (1), and on `soll_e12` unit 11319 (2) and `soll_sil6` unit 19277 (1 and 2), where the block data then ends with the last-block flag 1 and zero padding exactly at the end of the segment; the hand-built `hb_substream3_object_bypass_wide` holds it; what the bits add to the output is the decoder's reading (they are added to the matrix output, whose channel has a quantiser step of that many bits) |
| `field_m4` | 4 | the matrix's noise shift (generator 1) | VERIFIED |
| presence mask | N | bit c set: the matrix has a coefficient on matrix channel c; channel 0 is sent first | VERIFIED |
| (after all headers) coefficients, per matrix, per set mask bit in ascending channel | `frac_bits` + 2, signed | the raw value | VERIFIED |
| A2: the extension is on | 1 | 0: the extension is off and every delta is cleared | VERIFIED on the owner's corpus (305 of 309 A = 0 sections of `soll_e12` have it on) |
| B2: new deltas (only if A2) | 1 | 1: every delta is replaced (0 for a clear mask bit or width 0); 0: the deltas stand, also across new headers at the same matrix index | VERIFIED (14 sections with A2 = 1, B2 = 0 in `soll_e12`; clearing them there breaks the check) |
| C2: widths follow (only if B2) | 1 | 0: the widths and scales of the matrix at the same place stand | VERIFIED |
| per matrix, delta width w (only if C2) | 4 | 0 to 15 are seen | VERIFIED |
| per matrix, scale `ext_field2` (only if C2) | 2 | the delta is worth 2^-`ext_field2` raw places a unit; 0 to 3 are seen; S = `ext_field2` - `f_shift` + `frac_bits` (section 3) | VERIFIED (leaving it out of S fails 246 of 309 checks in `soll_e12`) |
| deltas: per matrix with w not 0, per set mask bit | w + 1, signed | the slope of the coefficient (section 3) | VERIFIED |

A set of headers whose `frac_bits` less `f_shift` exceeds 15 is refused. When new headers are sent, each matrix keeps the extension
width and scale of the matrix at the same place in the last set. The running coefficients and the deltas live in the matrix at that
place, so an A = 0 section keeps the last set whole. A restart clears the set, its widths and its deltas. The library's
encoder writes A = 1, B = 1, no extension, and A = 0 with no extension for an empty set.

**The hand-built stream.** `hb_substream3_object_extension` (`tools/references/truehd_writer.py`, `case_substream3_object_extension`) has
four matrices (outputs 8 to 11; `frac_bits` 12, 13 with `f_shift` 1, 0 and 15; noise shifts 2, 0, 5 and 3) and twelve matrix sections in
16 access units of 40 samples: new headers with C2 (widths 3, 5, 2, 0; scales 1, 0, 3, 2), A = 0 with new deltas, new headers with the deltas
standing, A = 0 with the slopes running over five access units, the extension off and on again, a restart that clears deltas and widths, and new widths
and scales with deltas that stand. The coded channels are derived by running the matrices backwards in integers. The Python reference decodes it
with its own implementation of the rule (`truehd_syntax.py`), and C++ and Python both give the signal sample for sample. Each of
these changes fails the stream's decode: the ramp's step, its reset at the start of the unit, the move of the coefficients at the end of the unit,
the clearing of the deltas when the extension is off, the scale, and the truncation of the fine sum.

**Where the layout evidence comes from.** The section reads to the bit, with the extension bit 0, in nine restarts of DEE's silent
encodes and in the steady restart of the disc (ERRATA 69). In `ctl_c` unit 256 (a restart with A2 set) the 1,184 bits that follow the
coefficients read as the extension and end where the next field, twelve output-shift nibbles of 4, begins. That is the whole evidence for
the extension layout.

## 3. One matrix in integer form

Rows are 32-bit integers. The coefficients of the object matrices run at 2^-18 and move between sections; the other matrices (the
bed's) use 2^-15 and no state. The object matrices keep, for each matrix, the running coefficients `c[ch]` (int32), the deltas `d[ch]`
and a `ramp` (int32). They are the evidence of ERRATA 69.

1. **Coefficients.** A = 1: `c[ch] = raw << (18 - frac_bits + f_shift)`, 0 where the mask bit is clear. A = 0: `c` keeps its running value.
   The real value is `raw / 2^(frac_bits - f_shift)`.
2. **Deltas.** A2 = 0 clears `d`; A2 = 1 with B2 = 1 replaces every `d[ch]` (0 for a clear mask bit or width 0); A2 = 1 with B2 = 0 leaves it.
   `S = ext_field2 - f_shift + frac_bits`.
3. **Each sample.** `sum = Σ row[ch] × c[ch]`. With the extension on: `fine = Σ row[ch] × d[ch]` in 64 bits, `f = (int32)(S >= 0 ? fine >> S : fine << -S)`,
   `sum += f × ramp`, then `ramp += step`. The ramp is 0 at the first block of every access unit; `step = (0x666 >> r) × 4`, where the unit has 40 << r samples,
   so 6552 at 48 kHz (the ramp then ends at 262,080, a little under 2^18). 6553 and 6554 fail most checks.
4. **Noise.** If `field_m4` (the shift s) is not 0, add `buffer[index] × 2^(s + 11)` (the same noise as 2^(s + 8) at 2^-15). The buffer, its index and its stride are in section 4.
5. **Shift and mask.** `value = (int32)(sum >> 18) & keep`, an arithmetic shift. `keep` clears the low `quant_step[out]` bits of the output row.
6. **LSB bypass.** If the matrix has bypass, `value += bits`, where `bits` are the `field_2b` bits the block data sent for this matrix and this sample, read as a number, first bit most significant.
   The streams do not tell whether the mask is applied before the bits are added: they pass the same either way, and this decoder masks first.
7. **End of the access unit** (the block that reaches its last sample), with the extension on: `c[ch] += (int32)d[ch] << (18 - S)`. So a delta is a change of `d / 2^ext_field2` raw
   places in every unit, for as long as it stands.
8. The result replaces `row[out]`. Matrices run in the order of the list, each seeing the previous ones' results.

Worked example. The third matrix of the `z_short.mlp` chain is described in ERRATA 69 as 2.3167 times row 0 plus row 11, written to
row 11. A coefficient of 2.3167 does not fit in `frac_bits` 13 without `f_shift`, so it is sent with `frac_bits` 13 and `f_shift` 1.
The raw values below are the ones the hand-built stream `hb_substream3_fine_matrices` uses for the same coefficient (9489 and a unity
coefficient of 4096); the sample values, the noise shift of 2, the buffer value and the delta are chosen to show each step.

| Step | Value |
|---|---|
| header | out 11, `frac_bits` 13, `f_shift` 1, `field_m4` 2, mask rows 0 and 11 (15 bit coefficients) |
| raw coefficients | row 0: 9489, row 11: 4096 (= 2.31665 and 1.0) |
| scale | 18 - 13 + 1 = 6, so coefficients at 2^-18 are 9489 × 64 = 607,296 and 4096 × 64 = 262,144 |
| inputs | row 0 = 1000, row 11 = -500 |
| sum | 1000 × 607,296 + (-500) × 262,144 = 476,224,000 |
| noise | third of three matrices (k = 2 of N = 3): first index 3 - 2 = 1, stride 3, so samples 0, 1, 2 read entries 1, 4, 7. With the seed `0x2A5F31` of the hand-built stream, entry 1 is 9. Draw = 9 × 2^(2 + 11) = 73,728, sum = 476,297,728 |
| shift by 18 | 476,297,728 >> 18 = 1816 (the exact value is 1816.93) |
| quantiser step 2 on row 11 | `keep` = ~3, 1816 & ~3 = 1816 |
| bypass bit 1 for this sample | 1816 + 1 = 1817 |

A slope on its own: a row of 8000, a delta of 40 with S = 3. At the sample where the ramp is 65,520 (ten samples into the unit) `fine` = 320,000, `f` = 40,000 and the
term is 40,000 × 65,520 >> 18 = 9,997. The whole unit's share, when the ramp reaches 2^18, would be 40,000: a delta of 40 over 2^3 is 5 raw places, times the row's 8000.

The sample row 0 and row 11 values in `z_short.mlp` are small (within about -2 to 2): the rows carry the encoder's dither, and the
three matrices cancel it by adding it back, which is why the noise shift has to be right for the check to pass (a shift of
`field_m4` plus or minus 1 passes no interval). The extension (steps 2, 3 and 7) is held by the hand-built stream
`hb_substream3_object_extension` and by `test_kernels.cpp`, and by the owner's corpus (ERRATA 69).

After the matrices, each output channel p takes row m (from `ch_assign`), applies `output_shift[m]`, is folded into the check and
written as a 24-bit sample (sign-extended).

## 4. Restart, noise and the interval

### Restart header

| Field | Bits | Notes |
|---|---|---|
| sync | 14 | `0x31EC` selects the object matrix section and generator 1 noise |
| `output_timing` | 16 | |
| `min_chan`, `max_chan`, `max_matrix_chan` | 4 each | `min_chan` <= `max_chan` <= `max_matrix_chan`; `max_matrix_chan` up to 15 in substream 3 (7 in substreams 0 to 2 of a TrueHD stream) |
| `noise_shift` | 4 | belongs to generator 0 and is not used under `0x31EC` |
| `noisegen_seed` | 23 | loaded as the noise seed |
| `max_shift`, `max_lsbs`, `max_bits`, `max_bits_repeat` | 4, 5, 5, 5 | not used by the decoder |
| `data_check_present` | 1 | block data length and CRC present |
| `lossless_check` | 8 | the check of the interval that ends here |
| (reserved) | 16 | carried as read |
| `ch_assign` | 6 × N | a permutation |
| CRC | 8 | checked; a bad CRC is an error, or a counted warning in lenient mode |

### Procedure

1. Read the header, check its CRC and ranges, and check that `ch_assign` is a permutation.
2. If the substream was running with an open interval, compare the fold of the interval with `lossless_check`, count the result per
   substream, and in strict mode stop on a mismatch.
3. Reset the substream, as in the table below, load the seed, and set the presence flags to `0xFF`.
4. Read the block header and decode the first block. The first fold is empty; the interval opens with the first block.

| Reset by a restart | Kept across a restart |
|---|---|
| block size (back to 8), matrices (including the last set and its deltas and widths, so a later B = 0 has nothing to reuse), output shifts (0), quantiser steps (0), every channel's FIR, IIR, huff offset, `huff_type` (0) and `huff_lsbs` (24), presence flags, the fold, the noise seed | FIR output history and IIR residual history of every channel, if the substream was running (ERRATA 13) |

Clearing the histories at a restart of substream 3 made no difference to any owner file (section 5).

Parameters stay in force from block to block until replaced: the matrices, output shifts, quantiser steps, filters and huff settings.

### Generator 1 noise

Once for each access unit, at its first block, the substream fills a buffer from the seed: for each entry, `index = (seed >> 15) & 0xFF`,
`buffer[i] = table[index]` (a 256-entry table of int8 values, `libs/truehd/src/core/noise.hpp`), and
`seed = (seed << 8) ^ index ^ (index << 5)` in 32 bits. The buffer holds 64 entries at 44.1 and 48 kHz, 128 at 88.2 and 96, 256 at 176.4
and 192. The fill happens whether or not any matrix draws noise, so the seed advances by that many steps every unit.

Matrix k of N (from 0, in header order) starts each access unit at buffer index N - k and steps by `2 × (N - k) + 1` a sample, wrapping at
the buffer size, across the blocks of the unit; the index starts again at the next unit. A matrix with shift 0 draws nothing and still
counts in N. VERIFIED: `z_short.mlp` passes 18 of 18 intervals with it and `hb_substream3_object_noise` fails without it.

## 5. Hypotheses tested and rejected

Status: **substream 3 is solved** (ERRATA 69). With the rule of section 3, 42 streams (13.5 million access units, eight Blu-ray streams, the two long
Sollevante encodes and all 22 controls) pass 106,308 of 106,308 substream 3 checks, and the object channels of `soll_e12`, `soll_e14` and `soll_e16`
match Dolby Reference Player's raw output in all 60,490,752 samples. (Measured on a private copy of the decoder; the run on this repository's code is pending.) Everything that was
tried before is
listed here with what it showed, as a record; none of it is in the decoder, and the switches that tested it were removed from `DecoderLab`.
Counts of the earlier runs were made with a fold that shifted by the channel instead of the channel modulo 8 (ERRATA 69) and are not comparable.

| Hypothesis | Result |
|---|---|
| The extension as a constant refinement of the coefficient (2^-(w + 1), 2^-w, any fixed 2^-n, a ramp across the block, summed over blocks) | Wrong form: the deltas are slopes per access unit, section 3. On matrix channel 0 of `ctl_c` the nine misses in interval 2 fell to 0 under a flat 2^-4 or a ramp of 2^-3 because the section lasted one unit |
| The standard matrix section for substream 3 | Rejected by ERRATA 69 |
| Matrices applied last to first, other sign of `f_shift`, no quantiser mask, mask on coded or bed rows only, lower rows read after substreams 0 to 2 | No lift |
| Tie rounding of a sum exactly between two outputs | No lift |
| Noise number capped, stride masked, index from the block or counted from the first matrix, buffer refilled for every block | Worse or no lift: the noise rule of ERRATA 69 stands |
| Filter histories cleared at an object restart | No difference |
| `field_2b` in the coefficient divisor | Rejected |
| The check folds other samples than the output of every matrix channel, or a subset of the per-channel folds | At chance |

## 6. Open items

**Sample rates other than 48 kHz.** The ramp's step and the length of the access unit (40 << r samples) are measured at 48 kHz only; every stream in the
corpus is 48 kHz. The step for 96 and 192 kHz is the rule's reading, untested.

**The quantiser mask with the LSB bypass.** The streams pass the same whether the mask is applied before the bypass bits are added or not; this decoder
masks first. Likewise the truncation of the scaled fine sum to 32 bits is not distinguished by the streams.

**Cases without a stream.** A restart header with the extension active in its first block, a delta for a mask bit that a later set clears (the delta
stands in the decoder but the sum covers present channels only), and A2 = 1 with B2 = 0 before any delta was sent.

**Not changed in this repository.** The encoder writes substream 3 with `min_chan` 0 and no matrix (the discs code rows 8 and up and
rebuild the rest with matrices, ERRATA 63, question 63) unless the opt-in `kBedMatrices` layout is chosen (ERRATA 70); it does not write the extension.

## 7. The lab tool

`iclforge-truehd-ss3-lab` is built with the test suite (`libs/truehd/tests/tools/ss3_lab.cpp`):

    iclforge-truehd-ss3-lab STREAM
    iclforge-truehd-ss3-lab STREAM sections FROM_SAMPLE TO_SAMPLE
    iclforge-truehd-ss3-lab STREAM sine CHANNEL AMPLITUDE HZ [THRESHOLD [FROM_SAMPLE]]
    iclforge-truehd-ss3-lab STREAM sinesweep CHANNEL AMPLITUDE HZ [THRESHOLD [FROM_SAMPLE]]
    iclforge-truehd-ss3-lab STREAM sinefit HZ HZ2 THRESHOLD INTERVAL

STREAM is an elementary TrueHD stream (`.thd` or `.mlp`). Without a mode the lab decodes the stream with `Presentation::kAll` and lenient checking and
prints the counts of the lossless checks, the pass/fail pattern per interval, ranges, the first matrices and per-interval values, parameters and counts
only. `sections` lists every matrix section sent between two output samples (the first 600,000) with the extension widths, scales and deltas, and the
blocks around them; the sine modes compare a channel's output with a sine of the stated frequency and report the misses per 5,120-sample interval. A last
argument `from=N` makes the value tables and sine modes record 51,200 samples from output sample N instead of the first. No audio leaves the decoder.

## Where the code is

| What | Where |
|---|---|
| matrix section, block header | `libs/truehd/src/core/syntax.cpp` (`read_object_matrices`, `write_object_matrices`, `read_block_header`) |
| restart, noise draw, bypass, object matrices and slopes, output stage, fold | `libs/truehd/src/decoder/decoder.cpp`, `kernels.hpp` |
| the lab | `DecoderLab` in `decoder.hpp`; the tool in `libs/truehd/tests/tools/ss3_lab.cpp` |
| Python reference and writer | `tools/references/truehd_syntax.py`, `truehd_writer.py` |
| hand-built streams | `testdata/truehd/handbuilt/hb_substream3_fine_matrices.thd`, `hb_substream3_object_noise.thd`, `hb_substream3_object_bypass.thd`, `hb_substream3_object_extension.thd`, `hb_substream3_limits.thd`, `hb_atmos_objects.thd` |
| tests | `libs/truehd/tests/core/test_syntax.cpp`, `tests/decoder/test_kernels.cpp`, `test_substream3.cpp`, `test_handbuilt.cpp` (the extension stream) |
