# TrueHD Reference Specification: issues for the author

As of 2026-10-31, for the owner to hand over. One line each: the section, what contradicts what (or what is missing), and the reading `libs/truehd` uses.
The section numbers and the wording of the specification are the owner's text and are not in the repository: each line is taken from the `libs/truehd/ERRATA.md`
entry named in the last column, which quotes v3.9 where it can. **Before handing the page over, check each line against the current text of the specification**:
a line may describe a passage that a later version has corrected, and the column "Evidence" says what the library rests on.

Questions 22 to 39 of ERRATA.md (the Atmos section, §15 and §16.3 to §16.6) are numbered there with the public standard's text and a recommendation each, and
belong to the Atmos package; they are not repeated except for the three that are errors of the text alone (the last three rows).

## Contradictions inside the text, or between the text and itself

| Section | The problem | The reading used | Evidence | ERRATA |
|---|---|---|---|---|
| §2.3, §20.1, Appendix A, against §7.2 | §7.2 (v3.9) says 0x31EC marks the 16-channel Atmos substream only and a non-Atmos substream carries 0x31EA or 0x31EB whatever its index. §2.3 and §20.1 still say all substreams share one 13-bit sync (0x18F5, `noise_type` the 14th bit, which cannot tell 0x31EC apart); Appendix A lists 0x31EB for substreams 0 and 1 and 0x31EC for 2 and 3 | §7.2 | two discs: 0x31EA in substream 0, 0x31EB in 1 and 2, 0x31EC in 3 | Q47, entry 48 |
| §7.4 | Reads "maps output positions to matrix channels"; v3.5 and v3.6 read "maps matrix channels to output channels". The bits of the stream, read in order, give the output position of each matrix channel (the inverse of the array §7.3's pseudo-code indexes with `ch_assign[out_ch]`). The two readings differ for any permutation that is not its own inverse | the field gives the output position of each matrix channel; its inverse is the array the pseudo-code uses | FFmpeg's decode of a three-cycle; the lossless checks of two discs (`ch_assign` (0,5,1,4,2,3) and (0,6,1,4,2,3,7,5)) | Q45, entries 42 and 45 |
| §3.3, §8.1, §8.4 | An access unit holds 40 samples (§3.3) and 8 blocks (§8.1), and a block 40 samples with "8 blocks per access unit = 320 samples" (§8.4): the three cannot all hold | blocks of 8 to 160 samples adding up to at most 255 in a unit; FFmpeg's streams have one block, or two (8 and the rest) with a restart header | FFmpeg's fixtures | entry 40 |
| §6.4 | The bit table of `substream_info` does not fit the streams (0x3C for 5.1 in two substreams, 0x14 for stereo, 0xFC for four) | the byte is carried; `num_substreams` is used | FFmpeg's fixtures; one disc (0xFC) | entry 17 (Q15) |
| §7.1 | Lists 19 reserved bits after `noisegen_seed` and `max_shift`, `max_lsbs`, `max_bits`, `max_bits_repeat` after the lossless check; the streams' header is 113 bits to `ch_assign`, so the four fields replace the reserved bits and the 16 bits after the lossless check are zero. `hires_output_timing` and `heavy_drc_present` have no place in the header | the 16 bits are read as `tail` and carried | the CRC-8 of every restart header of every stream | entry 7, Q10 |
| §8.2 and the DRC start-up gain (§7.1, §12.2) | `heavy_drc_present`, a 9-bit gain and a 3-bit time are listed where the streams have no room. On the discs the 16 bits after the lossless check are 0x0000 or 0x8000 (the bit set with a zero gain and time) | nothing is read as heavy DRC | two discs | Q10 |

## Missing from the text

| Section | What is missing | The reading used | Evidence | ERRATA |
|---|---|---|---|---|
| §17.3 | The size of the noise buffer (`access_unit_size_pow2`), the start index (`index_orig`) and the stride are used and not defined | 64 << the rate factor entries, one buffer for every access unit of the substream, matrix `k` of `N` starting at `N - k` with stride `2 (N - k) + 1` | hand-built streams in FFmpeg's decode; the gold set's legs with a noise shift are the check | Q43, entry 44 |
| §10.2, §10.5 | The scale of a matrix coefficient (`frac_bits + 2` bits signed, scaled by `2^(14 - frac_bits)`) and the range DEE accepts (it rejects `\|c\| >= 16`, `>= 32` for Atmos) | as given; the encoder writes 0 to 1.99 | FFmpeg's decode of the encoder's streams | entries 8 and 49, Q48 |
| §4.5 | `extended_substream_info` has values 0, 1 and 2; an Atmos disc has 3 (the low pair of the four bits after the substream count holds it, which §4.1 does say) | any non-zero value means the 16-channel fields are present; a bit field is the recommended reading | two discs | Q46, entry 47 |
| §14.2 | Where the 4-bit `extra_channel_meaning_length` sits against the first word of the body: inside it (the extension is `length + 1` words in all) or in front of it | whichever size the major sync's CRC confirms is taken, and reported | two discs: the major sync is 32 bytes (the 26-byte base, a 4-byte extension of length 1, then the CRC) | Q44, Q2 |
| §16.8 | The capacity rule `(extra_data_length + 1) x 16 >= evo_frame_byte_length x 8 + 40` reads as the words of the whole extra data less one; §16.1 and §16.2 say only "in 16-bit words". v3.9 §16.2 settles the count (the words after the header word). The frames on the owner's two discs fit it with a byte to spare (a 64-byte frame, length 31, payload 58 bytes: 504 against 512 bits) | the length counts the words after the header word; a header word of zero is the padding-only form | 11,376 regions, six streams of four discs | Q19, entry 22 |
| §19.1 and the 96 kHz legs | v3.9 makes the apodising filters a configuration option, and the text does not say what an encoder does to a 96 kHz input by default. All 25 DEE streams at 96 kHz decode with every lossless check ok, to what FFmpeg decodes, and not to their sources (about -47 dB on music and tones, -10 dB on noise and a sweep); a 96 kHz source of eight isolated samples decodes to a dense pattern of +/- the source's own amplitudes, so it is neither a delay nor a filter. If the specification says what an encoder does at 96 kHz (or the "apodising" option's default), the author can say | the decoder returns what the encoder coded; the legs are UNEXPLAINED | the 25 legs and the `thd-last-*` diagnostics | Q50 |
| §12.5, §14.1 | The unit of the 2 channel dialogue norm (6 bits, 0 to 63: is it -n dBFS, and what do 32 to 63 mean), whether 0 is a level or "not indicated", the unit and table of the mix level and the source format, what the `*_control_enabled` bits gate, and the unit of the DRC start-up gains | raw numbers in the stated widths; the dialogue norm shown as -n dBFS | DEE legs of a known dialogue norm N and six disc streams: the 6 and 8 channel fields hold N (-n dBFS, 5 bits), the 2 channel field holds N + 6 (so it uses its 6 bits and 32 to 63 occur); the mix level is 35 and 29 and the source format 0 whatever the programme; DEE sets the 2 channel control bit only, and writes the 6 and 8 channel dialogue norms with their bit clear | Q51 to Q55 |
| App. B, §8.1 | The block data CRC: polynomial 0x63 is given; the initial value and the bytes covered are not | the length is checked, the CRC is not | no stream sets `data_check_present` (FFmpeg's, DEE's, the discs') | Q9 |
| §3.5 and §16.5 | The width of `zero_samples` (the 12-bit count after terminatorA) and where in a segment DEE puts the terminator relative to the padding. v3.9 says DEE writes terminators in a pass that can be switched off; the Blu-ray encoder on the owner's discs writes none in the four streams probed (about 291,000 access units) | 12 bits, at the end of the segment | FFmpeg's end marker; two discs (none) | Q3, entries 14 and 46 |
| §9.2 | The FIR filter sends the `new_states` flag as well as the IIR filter; the text sends it for the IIR filter alone | read for both | the data of FFmpeg's streams, to within 16 bits of every segment's end | entry 9 |
| §8.3 | `huff_type` (2 bits) and `huff_lsbs` (5 bits) are sent for every channel whose `params_for_this_chan` bit is set, not under their own presence bits | as the streams do | same | entry 10 |
| §11.3 | The centring term for a Huffman channel is `-(1 << (lsb_bits + 2 - huff_type))`; the text has only the term for `huff_type` 0 | as the streams do | every FFmpeg fixture | entry 11 |
| §8.2 | The initial block size after a restart header (8 in the library) and which bit of the presence flags belongs to which group | 8; bit 7 down, in the order of §8.2 | every stream sends 0xFF, so this has no oracle | entry 19 |
| §6.2, §7.1, §4.1, Appendix B | The parameters of the three CRCs were found by search against streams: the substream CRC (polynomial 0x63, initial value 0x3C, the last byte XORed in, the parity byte not covered, parity XORed with 0xA9), the restart header's CRC-8 (0x1D, its last eight bits XORed in) and the major sync's CRC-16 (0x002D, its last two bytes XORed in) | as found | every FFmpeg stream's every segment and header; the discs' 291,012 of 291,012 parity and CRC segments | entries 4 to 6 |

## Atmos section: errors of the text alone

| Section | The problem | Evidence | ERRATA |
|---|---|---|---|
| Appendix E and §15.6 | Appendix E uses the v3.5 field names and says ISF SHALL be NULL, which §15.6 contradicts | the text | Q36 |
| §16.1 | The evolution frame is conditioned on a flag (`evolution_frame_present`) that no field defines | the text | Q38 |
| §15.11.3 | The speaker positions differ from the public standard's informative Annex B for 14 of 17 labels | ETSI TS 103 420 Annex B | Q39 |
| §15.4 and §16.3 | The same bytes are described twice, as an EMDF-style bit container (§15.4) and as a Thrift container (§16.3), and the text does not relate them; the owner's answer of 2026-10-29 is that they nest | the owner | Q22 |

## Found in the discs, for the author to confirm

- **The Blu-ray encoder's extra data** is a 58-byte frame in a 64-byte region once per 1,536 samples, not a variable container, on both discs (1,895 and 5,691
  frames). §16.3 describes a variable one (`planning/truehd-disc-findings.md`, question 1). Whether the payload is the container of §15.4 is not read.
- **A 12-channel substream with `frac_bits` up to 15** (the Atmos 7.1 disc's last substream): the decoder refuses above 14. Whether the text's range for the field
  allows 15 is to be checked; open (`planning/truehd-disc-findings.md`, "Not done").
- **The 2 channel dialogue norm is 6 above the multichannel one** in every stream that has both, DEE's and the six discs' (ERRATA 51). The specification gives the 2 channel field 6 bits and the others 5
  and no relation between them; the author can say whether the +6 is a downmix offset the field is defined to hold.
- **The control bits do not gate the dialogue norms.** DEE writes a dialogue norm in the 6 and 8 channel fields with the bit clear, and sets the 2 channel bit in every stream; on the discs the 2 channel
  bit is set on three of six streams. §14.1 describes the bits without saying what they gate (ERRATA 54).
