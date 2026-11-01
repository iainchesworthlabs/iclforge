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
| A: the matrices change | 1 | 0: keep the last set (and its deltas) and go to A2 | Not seen with A = 0 in an owner file. VERIFIED (hand-built only): `hb_substream3_object_extension` (the matrices and the deltas stand) |
| B: headers follow | 1 (only if A) | 0: reuse the last set's headers and read coefficients only (the coefficients replace the deltas) | PARSE-ONLY on real streams (the units after the first update, 4,072 in `soll_e12`, parse because of it); VERIFIED (hand-built only): `hb_substream3_object_extension` |
| count less one | 4 (only if B) | 1 to 16 on the wire; the decoder refuses more than 15 | VERIFIED |
| per matrix, output channel | 4 | row the matrix writes (0 to 15, may exceed `max_matrix_chan`) | VERIFIED |
| `frac_bits` | 4 | 0 to 15 | VERIFIED |
| `f_shift` + 1 | 3 | `f_shift` is -1 to 6 | VERIFIED |
| `field_2b` | 2 | not 0: the matrix has an LSB bypass of `field_2b` bits a sample (1, 2 and 3 are seen) | the bit count: VERIFIED on `ctl_c` (1), and on `soll_e12` unit 11319 (2) and `soll_sil6` unit 19277 (1 and 2), where the block data then ends with the last-block flag 1 and zero padding exactly at the end of the segment; the hand-built `hb_substream3_object_bypass_wide` holds it; what the bits add to the output is the decoder's reading (they are added to the matrix output, whose channel has a quantiser step of that many bits) |
| `field_m4` | 4 | the matrix's noise shift (generator 1) | VERIFIED |
| presence mask | N | bit c set: the matrix has a coefficient on matrix channel c; channel 0 is sent first | VERIFIED |
| (after all headers) coefficients, per matrix, per set mask bit in ascending channel | `frac_bits` + 2, signed | the raw value | VERIFIED |
| A2: an extension follows | 1 | | PARSE-ONLY on real streams; VERIFIED (hand-built only) |
| B2: this block has deltas (only if A2) | 1 | | PARSE-ONLY on real streams; VERIFIED (hand-built only) |
| C2: widths follow (only if B2) | 1 | 0: the widths of the last set stand | PARSE-ONLY on real streams; VERIFIED (hand-built only) |
| per matrix, delta width w (only if C2) | 4 | | PARSE-ONLY on real streams; VERIFIED (hand-built only) |
| per matrix, reserved (only if C2) | 2 | carried as read, not used | OPEN (the hand-built stream sends 0 to 3 and the decode ignores it) |
| deltas: per matrix with w not 0, per set mask bit | w + 1, signed | refine the coefficient (section 3) | layout PARSE-ONLY on real streams; the layout and the scale 2^-(w + 1) VERIFIED (hand-built only), the scale also supported by the owner's runs (section 5) |

A set of headers whose `frac_bits` less `f_shift` exceeds 15 is refused (the finest coefficient is 2^-15). When new headers are sent, each
matrix keeps the extension width and reserved field of the matrix at the same place in the last set (VERIFIED, hand-built only). The deltas
of a set stand until the set is replaced; sending coefficients again (A = 1) clears them, so a block that sends coefficients and no
deltas has none. A restart clears the set with its widths. A present coefficient of base 0 stays in the matrix when its delta is not 0
(a refinement from nothing); a coefficient with base and delta 0 is dropped as before. Held by the hand-built stream
`hb_substream3_object_extension` (below), which fails when the zero base is dropped. The library's encoder writes A = 1, B = 1, no
extension, and A = 0 with no extension for an empty set.

**The hand-built stream.** `hb_substream3_object_extension` (`tools/references/truehd_writer.py`, `case_substream3_object_extension`) has
four matrices (outputs 8 to 11; `frac_bits` 12, 13 with `f_shift` 1, 0 and 15; noise shifts 2, 0, 5 and 3) and seven matrix sections in
16 access units: new headers with C2 (widths 3, 5, 2, 0; the reserved bits 1, 2, 3, 0) and deltas, with present coefficients of base 0
carrying a delta; new headers with different masks and C2 clear (the widths stand, the deltas follow the new masks); A = 0 with
A2 = 0 (the matrices and deltas stand); A = 1 and B = 0 with new coefficients, widths standing and new deltas; a restart with A2 set and B2 clear (no
deltas, widths gone); A = 0 with C2 (widths 0, 4, 6, 2) and deltas on the set that stands; and A = 0 again (those deltas stand). The coded
channels are derived by running the matrices backwards in integers. The Python reference decodes it with its own implementation of the
rule (`truehd_syntax.py`, `read_object_matrices` and `decode_block`), and C++ and Python both give the signal sample for sample. Each
of these changes fails the stream's decode (ERRATA 69): the deltas left out, the scale 2^-w, the widths not inherited, the deltas
cleared under A = 0, and the zero base dropped.

**Where the layout evidence comes from.** The section reads to the bit, with the extension bit 0, in nine restarts of DEE's silent
encodes and in the steady restart of the disc (ERRATA 69). In `ctl_c` unit 256 (a restart with A2 set) the 1,184 bits that follow the
coefficients read as the extension and end where the next field, twelve output-shift nibbles of 4, begins. That is the whole evidence for
the extension layout.

## 3. One matrix in integer form

Rows are 32-bit integers. The decoder scales every coefficient to 2^-15, so a sum is 64-bit and is shifted down by 15.

1. **Coefficient.** With `scale = 15 - frac_bits + f_shift`, the coefficient at 2^-15 is `raw << scale`. The real value is
   `raw / 2^(frac_bits - f_shift)`. The finer extension part, if the matrix has width w not 0, is `delta << scale`, read in units of
   2^-(w + 1) of the coefficient's last place (the base's last place is 2^-(frac_bits - f_shift)).
2. **Sum.** `sum = Σ row[c] × coeff[c]` over the matrix's coefficients, in 64 bits. A coefficient whose base is 0 is kept when its delta is not 0.
3. **Noise.** If `field_m4` (the shift s) is not 0, add `buffer[index] × 2^(s + 8)`. The buffer, its index and its stride are in section 4.
4. **Fine terms.** If w is not 0, `sum = (sum << (w + 1)) + Σ row[c] × (delta[c] << scale)` and the final shift becomes 15 + w + 1.
5. **Shift and mask.** `value = (sum >> shift) & keep`, an arithmetic shift (a negative sum rounds toward minus infinity). `keep` clears
   the low `quant_step[out]` bits of the output row.
6. **LSB bypass.** If the matrix has bypass, `value += bits`, where `bits` are the `field_2b` bits the block data sent for this matrix and this sample, read as a number, first bit most significant.
7. The result replaces `row[out]`. Matrices run in the order of the list, each seeing the previous ones' results.

Worked example. The third matrix of the `z_short.mlp` chain is described in ERRATA 69 as 2.3167 times row 0 plus row 11, written to
row 11. A coefficient of 2.3167 does not fit in `frac_bits` 13 without `f_shift`, so it is sent with `frac_bits` 13 and `f_shift` 1.
The raw values below are the ones the hand-built stream `hb_substream3_fine_matrices` uses for the same coefficient (9489 and a unity
coefficient of 4096); the sample values, the noise shift of 2, the buffer value and the delta are chosen to show each step.

| Step | Value |
|---|---|
| header | out 11, `frac_bits` 13, `f_shift` 1, `field_m4` 2, mask rows 0 and 11 (15 bit coefficients) |
| raw coefficients | row 0: 9489, row 11: 4096 (= 2.31665 and 1.0) |
| scale | 15 - 13 + 1 = 3, so coefficients at 2^-15 are 9489 × 8 = 75,912 and 4096 × 8 = 32,768 |
| inputs | row 0 = 1000, row 11 = -500 |
| sum | 1000 × 75,912 + (-500) × 32,768 = 59,528,000 |
| noise | third of three matrices (k = 2 of N = 3): first index 3 - 2 = 1, stride 3, so samples 0, 1, 2 read entries 1, 4, 7. With the seed `0x2A5F31` of the hand-built stream, entry 1 is 9. Draw = 9 × 2^(2 + 8) = 9,216, sum = 59,537,216 |
| shift by 15 | 59,537,216 >> 15 = 1816 (the exact value is 1816.93) |
| extension, w = 3, delta +5 on row 0 and 0 on row 11 | delta at 2^-15 = 5 × 8 = 40; sum = (59,537,216 << 4) + 1000 × 40 = 952,635,456; shifted by 19 gives 1817 (1817.008) |
| quantiser step 2 on row 11 | `keep` = ~3, 1817 & ~3 = 1816 |
| bypass bit 1 for this sample | 1816 + 1 = 1817 |

The sample row 0 and row 11 values in `z_short.mlp` are small (within about -2 to 2): the rows carry the encoder's dither, and the
three matrices cancel it by adding it back, which is why the noise shift has to be right for the check to pass (a shift of
`field_m4` plus or minus 1 passes no interval). The extension scale (step 4) is held by the hand-built stream
`hb_substream3_object_extension` and by `test_kernels.cpp` (VERIFIED, hand-built only: no outside decoder confirms it, and the owner's
streams give only the counts in section 5).

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

All of these were run with `iclforge-truehd-ss3-lab` on the owner's files. "No lift" means no count of passing substream 3 intervals
rose, and the counts of substreams 0 to 2 did not change. The state of the decoder is that none of these is in force; the switch that
tests each is in `DecoderLab` (`libs/truehd/include/iclforge/truehd/decoder/decoder.hpp`).

| Hypothesis | Result |
|---|---|
| A matrix sum exactly between two outputs rounds up, rounds to even, or every sum rounds to nearest (`object_tie_mode`) | Rejected; relayed from the owner's runs, ERRATA 69 does not record the counts |
| Substream 3 reads the bed rows after substreams 0 to 2 applied their matrices (`lower_rows_after_matrices`) | Substreams 0 to 2 keep their counts, substream 3 falls |
| No quantiser mask on the object matrices (`no_quantiser_mask`) | No lift |
| Quantiser mask on coded rows only, or on bed rows only (`mask_rows` 1, 2) | No lift |
| Matrices applied last to first (`reverse_matrices`) | No lift |
| The other sign of `f_shift`, coefficient = raw / 2^(`frac_bits` + `f_shift`) (`inverse_f_shift`) | No lift |
| Every one of the 15 combinations of the four switches above | No lift |
| Noise index counted from the start of the block, matrices numbered from the first, or both (`object_noise_index` 1 to 3) | No lift |
| Filter histories cleared at an object restart (`object_restart_clears`) | No difference at all |
| Noise shift = `field_m4` plus or minus 1 | Passes no interval (`field_m4` itself is right) |
| `field_2b` in the coefficient divisor | `ctl_c` passes 1 of 93 intervals, against 43 without |
| Extension delta scale 2^-w instead of 2^-(w + 1) | Fewer pass: `soll_e12` 10 against 11, `soll_sil6` 15 against 18. The hand-built `hb_substream3_object_extension` fails under it |
| The check folds the coded channels alone, the channels below them, or the coded samples before the matrices, each by the matrix-channel shift and by the place shift (`fold_scope` 1 to 6) | At chance, 0 to 3 of 244 |
| The check folds the matrix channels from N upward, N = 1 to 8 (`fold_scope` 11 to 18) | At chance |
| The check folds some subset of the 24 per-channel folds (after and before the matrices) | None above 3 of 12 intervals on `EX_00002b.thd`, 2 of 12 on `soll_sil6.mlp` |
| The check covers a single pre-matrix row (seen in `z_short.mlp` before the noise was found) | Superseded: the rows were silent apart from dither, and the noise rule makes the standard fold pass |
| A block-position effect, or a section effect | Out: one block per unit in every owner file, and `EX_00002a/b` carry one matrix section in every interval |
| The standard matrix section (4-bit count, a presence bit for every channel) for substream 3 | Rejected by ERRATA 69; entries 65 to 68 were this layout read wrongly |
| Four early grammars for the extension (a value for every present channel or every channel, one width or one per matrix) | All failed at the first update (unit 4,072 in `soll_e12`, 4,175 in `EX_00002a`) |
| The extension as one bit per matrix channel | Changes nothing |
| The extension's delta read at 2^-(w + 1 + k), k = -6 to 2, with either sign; a fixed 2^-n for n = -1 to 8; the delta counted from 2^-frac or from one absolute place; flat, ramped over the block, or adding up over blocks | No lift in `ctl_c`, `e5` (one line 93 against 92), `e7` (at most 25 against 23) or `EX_00002a`; on matrix channel 0 of `ctl_c` interval 2 several give 0 to 1 misses against 9 (see section 6) |
| Refilling the noise buffer for every block (`object_noise_index` bit 2) | Worse: the hand-built extension stream fails, every file at or below its base |
| A sweep of 96 grammars of the extension | Inconclusive: it held the layout now used, but ran before the bypass bit was known, so its blocks did not parse (no longer in the lab) |

## 6. Open items

**Rare differences of plus or minus 1 in files with several active objects.** Substream 3 passes every interval where the objects are
silent or one object carries a tone (`ctl_a`, `ctl_b`, and `z_short.mlp`, where rows 2 to 10 are silent). With a constant on one object (`ctl_d`) 27 of 93 intervals pass, the
failures differing in a few low bits (XOR 03 to 07) with an output shift of 4 on every row. The latest counts in ERRATA 69 are
`EX_00002b.thd` 28 of 244 intervals, `EX_00002a.thd` 17 of 206, `soll_e12.mlp` 11 of 296 checks and `soll_sil6.mlp` 18 of 357 (the last two
with the extension deltas applied). The check is one byte for an interval and fails on a single wrong sample, so these counts do not say
how many samples are wrong; the owner reports the errors as rare and of size 1, and the lab has no per-sample count. The cause is not
found; section 5 lists what it is not. The controlled encodes of DEE are what settles it, and DEE's licence ends on
2026-11-06.

**Parse breaks.** In `soll_e12` the block header fails at unit 11,319 ("block header"); in `soll_sil6` at unit 19,277. The cause is not
known. The lab prints the failing block header's bit position, the flags and the extension widths it was read against (the line
starting "block header at bit").

**The extension, newest reading (not in force; sections 5 and 6 below it are older).** In the synthetic control `ctl_c`, the section sent at
output sample 12,280 (the only block of 40 samples between it and the next section, at 12,320) has the extension. For all of its 8 matrices, every
coefficient that has a delta satisfies raw + delta / 8 = the raw coefficient of the next section, to rounding (for example 15,723 + 773 / 8 = 15,819.6
against 15,820; 13,744 + 297 / 8 = 13,781.1 against 13,781; -20,159 - 444 / 8 = -20,214.5 against -20,214), whatever the width (5 to 10) or `f_shift`. So the delta is
the change of the coefficient over the block, in raw last places divided by 8, not a width-dependent refinement. The same section's sine on matrix channel 0 (the
object's tone, 1 kHz) is exact to the sine's rounding with a flat delta / 16 (2^-4) or with a ramp across the block of delta / 8 (2^-3, either (i + 1) / block or i / block),
and misses 9 samples by 4 to 8 with the rule in force. Counts of misses at threshold 4 in interval 2: rule in force 9; delta / 2^(w + 1) weighted 4 times 1; flat 2^-4 0; ramp 2^-3 0.
These settings (`object_extension_fixed`, `_unit`, `_cumulative`, `_bias`) do not change any count of passing intervals in `ctl_c`, `e5`, `e7` or `EX_00002a`, so the lock does not
yet confirm any of them, and the rule in force is unchanged. The sections at 10,240 and 10,760 of the same file carry the extension too, with 13 and 38 blocks until the next section; the
coefficients in the next section differ from raw + delta / 8 there (by about 2 to 38 times delta / 8, depending on the matrix), so the delta is not simply a
step that repeats in each block; what it is over several blocks is not known. Tools: `sections`, `sine`, `sinesweep`, `sinefit` of the lab.

**The extension, the older reading.** What the deltas do, and their scale: the decoder applies `delta / 2^(w + 1)` of the base's last place, which lifts
`soll_e12` from 9 to 11 passing checks and `soll_sil6` from 15 to 18. The deltas of `ctl_c` and `EX_00002a` are near 0 and the scale is
not exercised there. The hand-built stream `hb_substream3_object_extension` holds the decoder's rule (section 2), but it holds the
decoder's reading and no outside decoder's. The 2 reserved bits that follow each width are not understood. A present coefficient of 0
with a delta is kept (a refinement from nothing); nothing in the owner's streams shows whether a real encoder sends one.

**`field_2b` as the bit count of the bypass.** A matrix whose `field_2b` is n has an LSB bypass of n bits a sample: the output row's coded value
is multiplied by 2^n by the matrix (an own coefficient of 2.0 for n = 1 in `ctl_c`, where every other matrix has 1.0), its quantiser step is n, and the
n bits restore what the step cleared. The evidence is the length of the block data in two failing DEE blocks (`soll_e12` unit 11319 with one matrix of n = 2;
`soll_sil6` unit 19277 with matrices of n = 1 and n = 2): with n bits read before the first channel's residual in each sample, all 40 samples parse and
the last-block flag is 1 followed by zero padding to the end of the segment. Held by `hb_substream3_object_bypass_wide` (2 bits).

**The missing first matrix in `ctl_c` at unit 256.** The first matrix of that restart (output 8, 9 coefficients) is missing from the
matrix list the decoder reads. Why is not known. Unit 256 is also the unit that shows the extension layout.

**Without evidence.** ERRATA records no owner's stream that shows a section with A = 0 (the hand-built extension stream holds the decoder's reading), a different output shift on each row of substream 3, the
quantiser mask on matrix outputs, or several blocks in one unit of substream 3.

**Not changed in this repository.** The encoder writes substream 3 with `min_chan` 0 and no matrix (the discs code rows 8 and up and
rebuild the rest with matrices, ERRATA 63, question 63).

## 7. The lab tool

`iclforge-truehd-ss3-lab` is built with the test suite (`libs/truehd/tests/tools/ss3_lab.cpp`; the target is a dependency of
`iclforge-tests`):

    iclforge-truehd-ss3-lab STREAM

STREAM is an elementary TrueHD stream (`.thd` or `.mlp`). The lab decodes the whole stream 43 times with `Presentation::kAll` and lenient
checking, each time under one setting of `DecoderLab`, and prints one line a run. All off is the decoder (`standard`). Four of the 15
combination runs (one switch each) repeat the single-switch runs. It prints counts and error texts only; no sample leaves the decoder.

| Runs | Settings |
|---|---|
| 1 | `standard` |
| 4 | one arithmetic switch: lower rows after matrices, no quantiser mask, matrices last to first, inverse `f_shift` |
| 15 | every combination of those four |
| 2 | quantiser mask on coded rows only, on bed rows only |
| 3 | tie rounding: up, to even, to nearest |
| 1 | object restart clears the filter histories |
| 3 | noise index from the block, matrices from the first, both |
| 6 | fold scope 1 to 6 |
| 8 | fold the matrix channels from 1, 2, ..., 8 upward |

Each run prints a line like

    standard   units U decoded D errors E  ss3 ok A failed B  ss0-2 ok C failed F  first: unit N: TEXT

giving the units read and decoded, the errors, the lossless checks of substream 3 and of substreams 0 to 2, and the first error. If the
object block header failed, the next line gives its bit position, the presence flags and the previous matrices' extension widths. The next
line, `stored/folded/xor`, lists the first 12 checks of substream 3: the byte the restart header stored, the byte the decoder folded and
their XOR. A setting that explains a point of the format makes the `ss3 failed` count fall to 0 and the `xor` bytes all 00.

The first run, `standard`, prints more:

| Block | Content |
|---|---|
| ranges | for the first intervals, each channel's coded and output minimum, maximum and number of changes |
| first object matrices | each matrix's output, `frac_bits`, `f_shift`, `field_2b`, `field_m4`, mask and raw coefficients |
| interval log | one character a check (`o` passed, `F` failed) and, below, the number of matrix sections in the interval |
| quantiser steps and output shifts | by matrix channel at the first check |
| per interval | check result, matrix count, largest noise shift, and the bit sizes of the largest coded, bed and output samples (80 intervals) |
| subsets | the five subsets of per-channel folds that XOR to the most stored bytes |

How the owner's sessions use it. The files are on the owner's machine and stay there. A local session builds `iclforge-truehd-ss3-lab`,
runs it on one stream at a time, and pastes back the printed lines, which hold parameters and counts and no audio. The pass counts
and the stored/folded bytes go into ERRATA 69. The test applied so far is that a change of the decoder counts only if it lifts a count of
substream 3 without lowering the counts of substreams 0 to 2. For one stream the plain decoder gives the lossless-check line with
`forge decode DISC.thd out.wav truehd-presentation=all truehd-check=lenient`. `tools/checks/truehd_substream3_search.py` is from the
earlier search for the layout: it tries readings of the standard matrix section, which ERRATA 69 superseded.

## Where the code is

| What | Where |
|---|---|
| matrix section, block header | `libs/truehd/src/core/syntax.cpp` (`read_object_matrices`, `write_object_matrices`, `read_block_header`) |
| restart, noise draw, bypass, fine terms, output stage, fold | `libs/truehd/src/decoder/decoder.cpp`, `kernels.hpp` |
| lab switches | `DecoderLab` in `decoder.hpp`; the tool in `libs/truehd/tests/tools/ss3_lab.cpp` |
| Python reference and writer | `tools/references/truehd_syntax.py`, `truehd_writer.py` |
| hand-built streams | `testdata/truehd/handbuilt/hb_substream3_fine_matrices.thd`, `hb_substream3_object_noise.thd`, `hb_substream3_object_bypass.thd`, `hb_substream3_object_extension.thd`, `hb_substream3_limits.thd`, `hb_atmos_objects.thd` |
| tests | `libs/truehd/tests/core/test_syntax.cpp`, `tests/decoder/test_kernels.cpp`, `test_substream3.cpp`, `test_handbuilt.cpp` (the extension stream) |
