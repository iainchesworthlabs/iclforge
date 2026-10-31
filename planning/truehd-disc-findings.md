# TrueHD questions, read against the owner's Blu-ray (2026-10-29)

Decision D 3 (2026-10-29): the owner's own Blu-ray discs, and only they, may be analysed to settle format
questions. This is the first such analysis. It is not a DEE analysis: no DEE stream was used for any
finding below.

**Source.** *Blade Runner 2049* (BD, an AnyDVD HD image mounted as F:). The main feature,
`BDMV/STREAM/00002.m2ts`, carries one TrueHD + Atmos 7.1 stream (stream 0:5, 48 kHz, 24 bit); the other 23
clips hold only AC-3 and LPCM, so nothing else on the disc is TrueHD. A 60 s excerpt (from 900 s, 72,753
access units) and the whole stream (4.5 GB when the extraction finished) were copied with `ffmpeg -c copy` to
`D:/ac3bld/bd-br2049/` and stay there: nothing from the disc is committed, and the figures below are
structure, not audio. The scripts that produced them are in `D:/ac3bld/bd-br2049/scripts/` (they import
`tools/references/truehd_syntax.py` for the bit reader and the CRC helpers; the experiments on the decode
use a patched copy of it, not the tree's file).

Everything below is from the 60 s excerpt unless it says otherwise. forge reads the stream (probe: 4
substreams of 2, 6, 8 and 12 channels, an object substream, 291,012 of 291,012 parity and CRC segments
pass) and decodes its 2-channel presentation (568 of 568 lossless-check intervals ok).

## Answers

| Q | What the disc shows | Status |
|---|---|---|
| 19 (extra data length) | 1,895 access units (one per 1536 samples: gaps of 38 and 39 units) end with 64 bytes after the last substream's segment, outside every directory pointer. Their first 16 bits are a check nibble and `extra_data_length` = 31. 64 bytes is 32 words, so the field is the word count less one, as the encoder writes it. The frame is: 16-bit header, a 16-bit payload length (58), the payload, one byte of padding, and one parity byte. 40 + 58 x 8 = 504 bits fits (31 + 1) x 16 = 512, the §16.8 capacity rule with a byte to spare. The last byte equals 0xA9 XOR every byte from the third to the 63rd (all 1,895). | **Settled for the length.** Real stream, not an inference from FFmpeg. |
| 19 (check nibble) | The header's nibble is the complement of the library's `extra_data_check_nibble(length)`: the four nibbles of the header XOR to 0xF, as the access unit's check nibble does. The library's nibble fails on all 1,895. | **Finding.** `extra_data_check_nibble` and the encoder's `frame_extra_data` have it inverted. Not changed. |
| 1 (what is on the wire) | The payload is 58 bytes per 1536 samples. Its first 11 bytes are identical in all 1,895 frames, 339 of its 512 bits never change, the rest varies frame to frame; 1,657 of the 1,895 frames are different. It is a fixed-size frame and a parity byte, not a variable Thrift container as v3.4 describes. I have not decoded the payload. | **Open.** The framing is known; the contents are not. |
| 2 (16-channel extension) | The major sync is 32 bytes, not 28: the 26-byte base, a 4-byte extension, then the CRC (found by the CRC, the library does this). The extension is `extra_channel_meaning_length` = 1 followed by 28 bits that read, as §14.2 lays the fields out, as: 16ch dialogue norm 31, mix level 35, channel count 11, dynamic-object-only 1, LFE present 1. 11 + the LFE is the probe's 12-channel substream. So the length is in 16-bit words, (length + 1) x 16 = 32 bits including the length nibble itself. | **Settled for one length value** (every major sync has 1). The unit and the CRC position fit; larger values are not seen. |
| 2 (which bits say it is present) | The byte after the 4-bit substream count is 0x43 here (0x20 in the repo's FFmpeg two-substream fixtures). The two low bits are set and the two above are not. The library reads the two above as `extended_substream_info` (0 here, so it does not read the 16ch fields) and the two below as reserved. With the extension present, the presence bit is more likely the lower pair. | **Finding.** The library's `has_extended_substream` is false for this stream. Not changed. |
| 15 (`substream_info`) | The 4-substream value is 0xFC. The repo's 1- and 2-substream fixtures have 0x14 and 0x3C, so each substream sets the next bit from bit 4 up, and the two lowest bits of 0x3C and 0xFC are 1. | **Partly.** 4 substreams only; three are not on this disc. |
| 3 (terminators) | No `terminatorA` at the end of any segment of substreams 0 to 2, and no `terminatorB` (0x1234) at the end of substream 3, in 72,753 access units: scanning every bit offset in the last 12 bytes of each segment finds only the one to four chance matches the pattern's size predicts. | **Settled** for Dolby's Blu-ray encoder: it writes none. |
| 4 (which presentation) | Substreams 2, 6, 8 and 12 channels. The restart sync of substream 3 is 0x31EC (so an Atmos reader is needed); substreams 1 and 2 use 0x31EB; substream 0 uses 0x31EA. A plain player reads substreams 0 to 2. | Consistent with the library's `kBest`. |
| 5 (generator 1 and its table) | Substreams 1 and 2 are 0x31EB (noise type 1) in every restart, with a restart-header noise shift of 0, and their matrices carry no noise coefficients and a 4-bit per-matrix shift that is 0 in the first 520 units (any other layout loses the next restart sync within 128 units). So this stream does not need the 256-entry table for those intervals. | **Partly.** The table is still needed for any stream whose matrices shift the noise (the 8ch streams from DEE did). |
| 6 (lossless check shift) | **Settled for the matrix channel.** The substreams here carry permuted `ch_assign` ((0,5,1,4,2,3) and (0,6,1,4,2,3,7,5)). The reference decode fails the check at every interval of substreams 1 and 2 until two things are changed together: `output_shift` is indexed by the matrix channel an output takes (not by the output's position), and each substream's matrices read the previous substream's channels *before* its matrices (ERRATA 34). Then substreams 0, 1 and 2 pass every interval, and all eight channels of the 8-channel bed equal FFmpeg's PCM sample for sample (2.9 million samples, FFmpeg's channel order of ERRATA 33). With the output shift by position, or the fold by position, substreams 1 and 2 fail every interval. | **Settled** (both the fold and the output shift go by the matrix channel). Fix: ERRATA 43. |
| 9 (block data CRC) | `data_check_present` is 0 in all four substreams' restart headers. | **No oracle** on this stream either. |
| 10 (heavy DRC) | The major sync's heavy DRC start-up gain is 0 at all 569 major syncs. The 16 bits after the lossless check in each restart header are 0x0000 (314 restarts) or 0x8000 (255 restarts), identically in all four substreams. The standard DRC start-up gain (7 bits) changes at every major sync (-51 to -44 over the 60 s). The 2,351 directory entries with the extra-word bit set carry the standard DRC word. | **Partly.** The bit at 0x8000 is set with a zero gain and time. |
| 12 (`peak_data_rate`) | Raw 2417, constant. raw x rate / 16 = 7.25 Mbps, which is the FFmpeg-fixture scale, not raw x 200 x a multiplier. The highest data rate over a 16-unit window is 7.19 Mbps (7.60 over 8 units, 6.65 over 128, 14.8 for a single unit). | **Consistent with raw x rate / 16** over a ~16-unit window. |
| 18 (filter history across a restart) | Substreams 0 to 2 use FIR orders up to 8 and IIR orders 1 and 3 with the state flag set. The reference decode of the whole excerpt with the history kept and with it zeroed at each restart gives the same samples (all 568 intervals ok in each substream, all 8 channels equal to FFmpeg's). | **Not decided** by this stream: it does not use the history across a restart. |

Also seen, not asked: the directory word's reserved bit (bit 12) is set on substream 0's entry in about 47% of
access units; the major sync repeats every 128 access units
with the DRC start-up gain counting up; the stream's average rate is 4.77 Mbps.

## What it means for forge

- forge could not decode the 6- or 8-channel presentation of this stream, and the cause was not the noise
  table: the decoder indexed `output_shift` by output position where the stream indexes it by matrix channel
  (`libs/truehd/src/decoder/kernels.hpp`, `assign_output`). With that one index changed
  (`bugfix/truehd-output-shift-by-matrix-channel`, ERRATA 43), forge decodes the 2-, 6- and 8-channel
  presentations of the whole 60 s excerpt with every lossless-check interval ok (568, 1,136 and 1,704), and the
  8-channel output equals FFmpeg's, sample for sample.
- The same fault is likely to be why the 6- and 8-channel DEE streams of the gold set fail where they do not
  stop at generator 1's table: not tested here.
- The two other findings that point at code are not in that branch: the extra-data check nibble is inverted
  (Q 19), and the major sync's extended-substream bits are read from the wrong pair (Q 2).

## Not done

- The whole 4.5 GB stream has been copied but not read (only the 60 s excerpt was). It would settle whether any
  Q 2 length other than 1, any 3-substream form or any other peak-rate figure occurs, and where the payload
  of the extra data changes.
- The payload of the extra-data frame (58 bytes per 1536 samples) is not decoded.
- The 12-channel Atmos substream (restart sync 0x31EC): its matrices carry `frac_bits` from 0 to 15 (forge
  refuses above 14) and the block parse then breaks at a filter order, so it has more to it than the 8-channel
  bed; open.
- Q 18 is settled in neither direction: the reference decode of the whole excerpt with the filters' histories
  kept across each restart and with them zeroed gives the same samples for substreams 0, 1 and 2 (all 568
  intervals ok in each, all eight channels equal to FFmpeg's), so this encoder does not rely on the history
  after a restart.

## Second disc: *Dune: Part Two* (UHD BD, 2026-10-30)

A second owner's disc (G:, a decrypted image). Its main feature, `BDMV/STREAM/00075.m2ts` (9,949 s), carries three
TrueHD + Atmos 7.1 streams (streams 2, 5 and 8 of the clip), each 48 kHz. A 60 s excerpt of each (from 1800 s, about
72,830 access units) is in `D:/ac3bld/bd-dune/` and stays there. Results, with forge at `2f9a3a907` (the output-shift fix):

- **The fix holds on a second encode.** All three streams decode at 2, 6 and 8 channels with every lossless-check
  interval ok (569, 1,138 and 1,707 for streams 2 and 5; 568, 1,136 and 1,704 for stream 8), and the 8-channel output equals FFmpeg's
  strict decode sample for sample on all eight channels (2.91 million samples each, FFmpeg's channel order of ERRATA 33).
- **Q 19 holds exactly.** Every one of the 5,691 extra-data frames in the three excerpts is 64 bytes, has `extra_data_length`
  31, a payload length of 58, a trailing parity byte that is right, a header nibble that is the complement of the library's, and one
  frame per 1536 samples (gaps of 38 and 39 units).
- **Q 2 holds.** The major sync is 32 bytes with the same extension, `1fc6bc00`, in all three; no terminators in any substream
  (only chance matches); four substreams of 2, 6, 8 and 12 channels with restart syncs 0x31EA, 0x31EB, 0x31EB and 0x31EC again.
- **`ch_assign` changes mid-stream.** Stream 2's second and third substreams change their `ch_assign` at a restart (from
  (4,5,1,0,2,3) to (4,5,2,1,0,3), for example), so a permutation is not fixed for a stream; the fix handles it.
- `peak_data_rate` raw: 1849, 1792 and 1817 (against 2417 on the first disc); `twoch_control_enabled` is set here and not there.

## The rest of the owner's discs (2026-10-31)

`forge probe detail=format` (forge built from `1e4ee5c3e`) on the 60 s excerpts the other sessions had already cut: the three *Dune: Part Two* streams above, *The Lord of the
Rings: The Two Towers* (`D:/ac3bld/bd-lotr/lotr.thd`) and a Hans Zimmer concert disc (`D:/ac3bld/bd-zimmer/zimmer.thd`), beside *Blade Runner 2049* again. Nothing from the
discs is committed; the probe texts are in `D:/ac3bld/truehd-validation-probes/`. All six streams are Atmos with four substreams; **no non-Atmos TrueHD disc, no 5.1 Atmos disc
and no Matroska or MP4 rip has been probed yet**, so ERRATA 15 (`substream_info` for one and three substreams) and the question whether a container carries an Atmos flag are not
answered.

| | *Blade Runner 2049* | *Dune* s2 | *Dune* s5 | *Dune* s8 | *Two Towers* | Zimmer |
|---|---|---|---|---|---|---|
| Substreams (channels) | 2, 6, 8, 12 | 2, 6, 8, 12 | 2, 6, 8, 12 | 2, 6, 8, 12 | 2, 6, 8, 12 | 2, 6, 8, 14 |
| `peak_data_rate` raw (Mbit/s) | 2417 (7.25) | 1849 (5.55) | 1792 (5.38) | 1817 (5.45) | 1673 (5.02) | 3147 (9.44) |
| 2 channel control bit | off | on | on | on | off | on |
| Dialogue norm 2ch / 6ch / 8ch, mix level 2ch / 6ch / 8ch, source format | 37 / 31 / 31, 29 / 35 / 35, 0 | the same | the same | the same | the same | the same |
| Standard DRC start-up gain (first major sync) | -40 | -64 | -6 | -6 | -26 | -25 |
| Extra-data regions (all with nibble, framing, parity, `evo_reserved` and length right) | 1,895 | 1,897 | 1,897 | 1,897 | 1,897 | 1,893 |
| Payload 11 (bytes) | 50 | 50 | 50 | 50 | 50 | 57 to 58 |
| Segments passing parity and CRC | 291,012 of 291,012 | 291,368 | 291,340 | 291,308 | 291,448 | 290,812 |
| Lossless-check intervals ok / failed (lenient, every substream) | 1,704 / 0 | 1,707 / 0 | 1,707 / 0 | 1,704 / 0 | 1,725 / 0 | 1,701 / 0 |
| `data_check_present` set | 0 | 0 | 0 | 0 | 0 | 0 |
| Terminators after the last block | none in 72,753 units | none | none | none | none | none |
| The 16 bits that end the restart header's fixed part | 0x0000 x314, 0x8000 x255 | 0x0000 x331, 0x8000 x239 | x332, x238 | x330, x239 | x334, x242 | x331, x237 |
| Matrices with LSB bypass | 0 | 26 | 0 | 0 | 5 | 0 |

What it settles, with the library's readings unchanged:

- **ERRATA 19 (the extra data's length and check nibble):** 11,376 regions on six streams of four discs, every one passes the check nibble as the library has it now, the framing, the parity
  byte, `evo_reserved` zero and the capacity rule. The Zimmer disc's payloads are 7 or 8 bytes longer than the others' (57 to 58 bytes against 50) and still fit. This is the evidence the owner accepted
  (2026-10-31) in place of a DEE score, which cannot be had without an Atmos master that scores a leg.
- **ERRATA 3 (terminators):** none on any substream of any of the six streams. **ERRATA 9:** `data_check_present` is 0 everywhere, so the block data CRC still has no oracle.
- **ERRATA 10:** the 16 bits are 0x0000 or 0x8000 in every restart header of every stream and the heavy DRC gain is 0 at every major sync; the bit at 0x8000 is set in 41 to 45 % of the restart headers
  and is the same in all four substreams of a header. Where the heavy-DRC gate sits is still not told.
- **ERRATA 2 and 44:** `extra_channel_meaning_length` is 1 in every major sync of every stream (the 4-byte extension, a 32-byte major sync), the 16-channel count field is 11 (13 on the Zimmer disc, whose
  last substream has 14 channels); a larger length has not been seen. **ERRATA 46:** `extended_substream_info` is 3 in all six, `substream_info` 0xFC.
- **ERRATA 51 to 55:** the channel meaning is the same on all six streams: dialogue norm 37 (2 channel) and 31 (6 and 8 channel), mix level 29 and 35, source format 0, the 2 channel control bit set on the
  *Dune* and Zimmer discs and clear on the other two. DEE writes the same values for a dialogue norm of 31 (`planning/truehd-validation.md`, ERRATA 51).
- **ERRATA 12 (`peak_data_rate`):** raw values 1673 to 3147 at 48 kHz, 5.0 to 9.4 Mbit/s with `(raw x rate + 8) >> 4`.
- **Decoding:** the lenient decode of every substream stops on the 12 or 14 channel substream's matrices (`parameter out of range`, ERRATA "Not done" above, `frac_bits` above 14); the lossless checks of the substreams
  it does decode (0 to 2) pass in every interval of every stream.

## A seventh stream: the disc labelled CIVIL_WAR_UHD (2026-10-31)

The owner's fifth disc, a UHD Blu-ray image mounted read-only at `G:` (label `CIVIL_WAR_UHD`, UDF), said to be a 5.1 Atmos disc. Main feature `BDMV/STREAM/00000.m2ts` (72.7 GB) holds one
TrueHD stream (`ffprobe`: stream 2, `Dolby TrueHD + Dolby Atmos`, 48000 Hz, 8 channels), beside three AC-3 streams. A 60 s excerpt from 900 s was cut with `-c copy` (23.3 MB,
`D:/ac3bld/bd-civilwar/excerpt-2.thd`) and probed with `detail=format` (`probe-2.txt`, `probe-2.json` beside it). Nothing from the disc is committed.

**It does not answer ERRATA 15.** The stream has the same four substreams as the 7.1 discs (2, 6, 8 and 12 channels, `substream_info` 0xFC, `extended_substream_info` 3), so `substream_info` for one and
three substreams is still unseen, and the disc is "5.1" only in its bed. What it adds:

| | CIVIL_WAR_UHD |
|---|---|
| Substreams (channels) | 2, 6, 8, 12 |
| `peak_data_rate` raw (Mbit/s) | 2411 (7.23) |
| 2 channel control bit | on |
| Dialogue norm 2ch / 6ch / 8ch, mix level 2ch / 6ch / 8ch, source format | 37 / 31 / 31, 29 / 35 / 35, 0 (the same as the other six streams) |
| Start-up gains, heavy / standard | 0 / -4 |
| Extra-data regions (nibble, framing, parity, `evo_reserved`, length all right) | 1,893, 58-byte frames, payload 11 of 50 bytes, 38 to 39 units apart |
| Segments passing parity and CRC | 290,908 of 290,908 |
| Lossless-check intervals ok / failed (lenient, every substream) | 1,704 / 0 |
| `data_check_present` set | 0 |
| Terminators after the last block | none in 72,727 units |
| The 16 bits that end the restart header's fixed part | 0x0000 x316, 0x8000 x253 |
| Matrices with LSB bypass | 0 |
| Major syncs with a different channel meaning | 13 (the first is shown) |

So ERRATA 3, 9, 10, 19, 44, 46 and 51 to 55 hold on a seventh stream of a fifth disc, with nothing new: the extra data are 1,893 more regions (13,269 on seven streams in all) and the
channel meaning is the same constants again. The lenient decode stops on the 12 channel substream's matrices, as on the other discs (`parameter out of range` once per major sync, `frac_bits` above 14;
ERRATA "Not done" above). The 60 s excerpt was the only part read.
