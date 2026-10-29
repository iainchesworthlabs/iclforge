# TrueHD (MLP) specification errata

What `libs/mlp` implements where `truehd-spec.md` (version 3.0, 2026-10-29, kept outside the
repository) is wrong, silent or self-contradictory, by specification section. Each item names the
entry of the problem register in [`planning/truehd.md`](../../planning/truehd.md#spec-problem-register),
which holds the evidence (the FFmpeg streams and black-box decodes that settled it). This page is
the corrected syntax as the code reads and writes it; the register is why.

## Access unit (3)

- An access unit is 1/1200 s: 40 samples at 44.1 and 48 kHz, 80 at 88.2 and 96 kHz, 160 at 176.4
  and 192 kHz. Not "40 at all sample rates", not "8 blocks", not 320 (P1).
- The check nibble makes the XOR of every nibble of the 4-byte header and the substream directory
  (extra words included) 0xF; the major sync is not covered.
- The last access unit of a stream may end each substream segment with a 32-bit termination word
  whose top 20 bits are 0xD234E and whose low 12 bits count the padding samples at the end of the
  access unit (P7).

## Major sync (4, 5, 14)

- CRC-16, polynomial 0x002D, initial value 0, is the unaugmented remainder over every byte from
  the sync word to the CRC field, the channel meaning and its extension included (P12).
- `peak_data_rate` is in units of fs / 16 bits per second (P2).
- The signature is 0xB752 in FFmpeg's TrueHD streams; either value is accepted (P24).
- `extra_channel_meaning()` follows the 64-bit channel meaning inside the major sync, before the
  CRC, and takes (`extra_channel_meaning_length` + 1) 16-bit words (P28).
- The channel assignment codes of 13.2 are mislabelled; 13.1's bits are right: stereo 0x001, 5.1
  0x00F, 7.1 0x04F (P25).

## Substream directory and segments (6)

- `substream_end_ptr` counts 16-bit words from the end of the directory (P13).
- A segment is blocks only; after the block whose `last_block_in_segment` is set, zero padding to a
  16-bit boundary, then the termination word if any, then the parity and CRC bytes (P8, P26).
- Parity: the XOR of every segment byte before it, XOR 0xA9. CRC: polynomial 0x63 (not 0x1D),
  initial value 0xA2, unaugmented, over the same bytes (P13).

## Restart header (7)

The layout, from the 14-bit sync word on (P6):

```
restart_sync_word 14, output_timing 16, min_chan 4, max_chan 4, max_matrix_chan 4,
noise_shift 4, noisegen_seed 23, max_shift 4, max_lsbs 5, max_bits 5, max_bits_repeat 5,
data_check_present 1, lossless_check 8, reserved 16, ch_assign 6 x (max_matrix_chan + 1),
restart_header_crc 8
```

- There is no separate `noise_type` bit, no second copy of the max fields, no `hires_output_timing`
  and no `heavy_drc_present` (P6).
- The CRC-8 (0x1D, initial 0, unaugmented) runs from the sync word to the end of `ch_assign` (P12).
- 0x31EA serves substreams 0 and 1 (at most 6 channels), 0x31EB substream 2, 0x31EC the
  16-channel substream (P5).
- The sync word's last bit is the noise type (P22, P37).
- `lossless_check` checks the restart interval that ends at the header: a 24-bit accumulator into
  which each output sample's low 24 bits go rotated left by its channel's index, folded to a byte,
  over the substream's whole presentation (P14).
- A decoder may keep a channel's filter state across a restart; FFmpeg's does. Streams that code
  the first 8 samples after a restart unpredicted decode alike either way (P35).

## Block header and parameters (8, 9, 10)

- Each block begins `block_header_exists`, then within it `restart_header_exists` (P8).
- The parameter presence flags gate, bit 7 to bit 1, the `new_*` bits of block size, matrix, output
  shift, quantiser step, filter A, filter B and Huffman offset (P11).
- `output_shift` is 4 bits signed: at most 7; a 16-bit source in 24-bit words takes shift 7 and
  quantiser step 1 (P36).
- Both filters carry the `new_states` bit after their coefficients; filter A's must be 0 (P19).
- A primitive matrix's coefficient loop covers `max_chan` + 3 inputs (two noise channels) with noise
  type 0, and `max_chan` + 1 inputs followed by a 4-bit `matrix_noise_shift` with noise type 1
  (P21, P37).
- Each sample row starts with one bit for each primitive matrix whose `lsb_bypass` is set (P21).
- Blocks of one access unit may differ in size (P20).

## Prediction and matrices (9, 10)

- One prediction: filter A's coefficients times past samples plus filter B's times its history,
  shifted down by the shared `coeff_q`, rounded down to the quantiser step. Filter B's history
  holds each sample less the prediction before that rounding (P15).
- A prediction may pass the 24-bit range near full scale; the reconstructed sample may not (P32).
- A matrix output is the weighted sum of its inputs in units of 2^-14, shifted down 14, rounded
  down to its channel's quantiser step, plus the bypassed LSB (P21).
- A substream's matrices run over channels 0 to `max_chan`, the lower ones taken from the lower
  substreams before their own matrices (P29).

## Huffman coding (11)

- The table symbol is the MSB value; for `huff_type` t and `lsbs_bits` n the residual is
  `(msb << n) + raw - (1 << (n + 2 - t)) + huff_offset`, then shifted up by the quantiser step
  (P23).
- Each codebook is prefix-free and leaves the 9-bit words 000000000 and 010000000 unused (P4).

## Noise (17)

No noise generator is given. The decoder refuses a noise-channel coefficient or a noise shift
other than 0 (`kNoiseChannel`); the encoder uses neither (P27, P37).

## Extra data (16)

The layout is this library's (P16): a check nibble and a 12-bit length in 16-bit words, evolution
payloads (type 8, size 16, bytes), padding, then a parity byte (XOR of the bytes before it, XOR
0xA9) and a zero byte. FFmpeg's decoder ignores the bytes after the last segment.
