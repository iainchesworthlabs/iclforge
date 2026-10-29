# iclforge::truehd: where the specification and the streams disagree

"TH spec" is the owner's *Dolby TrueHD (MLP) Reference Specification*, v3.5 (planning/truehd.md says what it is and
how it is used; section numbers are v3.3's, except §16.3.3 (v3.4) and §17 (v3.5)). Every entry below is a place where the specification is silent, contradicts itself or disagrees
with the streams FFmpeg 6.1.1's `truehd` and `mlp` encoders write (and its decoder reads). The streams decide:
FFmpeg's decoder decodes them, and the codec is lossless, so a reading that decodes all of them sample for sample
is the reading in use. Where a reading is forced by the streams and the specification is silent, it is marked
**provisional, owner to confirm**. Where no stream has the feature the library implements the specification as
written and says so (**no oracle**).

The scripts quoted as evidence (`explore27.py`, `dumpms.py`, `endmark.py`, `dump_struct.py`, `tail.py` and the like) are the
exploration scripts of the T1 work and are not in the tree; the checks that stay are
`tools/references/truehd_syntax.py` (the independent Python transcription) and
`tools/checks/verify_truehd_interop.py` (FFmpeg's strict decode against this library's, on the FFmpeg-made fixtures and,
with `--handbuilt`, on the hand-built streams, whose manifest says what FFmpeg does with each).

## Open questions for the owner

1. **The evolution payload (TH spec §16.3, §16.6).** Since v3.4 the payload is an outer Thrift container that carries
   the typed payload entries (the object audio metadata, §16.3.2) and a nested `ContentSignature` (`pace-ccjf1`).
   §16.3.3 then lists as **not yet confirmed**: the outer container's field IDs ("are the payload entries a list
   field alongside the ContentSignature, or a field within it using higher field IDs?"), the digest scope ("it
   likely covers the serialized payload entries only ... The exact scope needs confirmation"), and whether each
   frame carries its own full ContentSignature or the certificate chain is sent once. The specification's owner
   does not know these either, so the answer has to come from a stream or from the encoder. What is on the wire? Until this is answered
   the library carries the extra data as bytes (`DecodedAccessUnit::extra_data`) and phase T2b writes nothing.

   ETSI TS 103 420 (V1.2.1, public) is the document the owner names for the object data. Its scope is E-AC-3 only: it
   specifies the object audio metadata (OAMD) syntax (§5, EMDF payload id 11) and the JOC tool (§6, payload id 14), and the
   text does not mention TrueHD or MLP. The owner's v3.5 §15 and §16.3.2 list OAMD as typed payload entry 0x0B (11) with
   field names that overlap §5's (`multiple_bed_instances_present`, the intermediate spatial format, the object info block
   count), which fits the entry being the same OAMD payload that `libs/ac3` already reads and writes for E-AC-3. The owner
   said on 2026-10-29 that the object data follows TS 103 420. That settles the syntax of the entry, if it was meant that
   way; it does not settle what this question asks (the outer container's field IDs, the digest scope, one signature per
   frame or per stream), and TS 103 420 does not either. Nothing is written until the owner confirms the reading.
2. **The 16-channel extension of the channel meaning (§14.2).** `extra_channel_meaning_length` has no unit and the
   section gives no size for a major sync that carries it. The library finds the size by the CRC (it tries 30, 32,
   ... bytes) and reads the fields as written, with "extended_substream" taken to mean `extended_substream_info != 0`.
   No stream has it (**no oracle**). Is the length in bits, bytes or words, and is the CRC placed after padding to
   a 16-bit word?
3. **Terminators (§3.5, §16.5).** FFmpeg's encoder writes none between segments; its last access unit carries an
   end-of-stream marker that the specification does not describe (entry 14). Do DEE's streams carry `terminatorA`
   at the end of every segment of substreams 0 to 2, and `terminatorB` (0x1234) at the end of substream 3, as the
   specification says? Is the 12-bit count after `terminatorA` part of the terminator? The decoder accepts a
   stream with or without them.
4. **Which presentation `forge decode` gives for a stream with the sixteen-channel substream.** The library's
   default (`Presentation::kBest`) is the highest of substreams 0 to 2, the 7.1 bed; `kAll` reads substream 3.
5. **Noise generator 1 (§17.3, §17.4).** Generator 0 (`noise_type` 0, sync 0x31EA) is decoded from §17.2 (entry 23). For
   generator 1 (sync 0x31EB) the specification gives the LFSR and `matrix_noise_shift` but not the 256-entry `int8`
   noise table: §17.4 says the values are "pending extraction from the DEE binary". The library refuses 0x31EB with
   `ErrorKind::kUnsupported` and waits for the table from the owner; it does not take the values from anywhere else.
   Also asked, for generator 0: is `seed_shr7` in §17.2 a 32-bit or a 16-bit value? The recurrence gives different
   sequences (bits 16 and up of the new state). FFmpeg's decoder reads it as 16 bits: on the hand-built
   `hb_noise_generator_0` its strict decode equals the library's over the whole stream (3200 values, the first
   restart interval's 1280 among them) with `shr7 = (seed >> 7) & 0xFFFF`, while the 32-bit reading diverged at the third noise sample. The library
   reads 16 bits. **Provisional, owner to confirm.** Evidence: `python3 tools/checks/verify_truehd_interop.py
   --handbuilt` (`hb_noise_generator_0`).
6. **The lossless check's channel shift (§7.3).** Entry 12: the fold shifts a sample by its channel. The FFmpeg-made
   streams cannot say whether that is the output position or the matrix channel `ch_assign` names, because their
   `ch_assign` is the identity. FFmpeg's decoder shifts by `ch_assign[output channel]`: its strict decode of a stream
   built with a permuted `ch_assign` and the position shift says `Lossless check failed - expected 23, calculated ec`
   (the first `hb_matrices.thd`, its major sync patched to a channel assignment FFmpeg accepts), and 0xEC is the fold
   shifted by the matrix channel. The committed `hb_matrices` carries 0xEC and FFmpeg decodes it to the stream's signal
   (`python3 tools/checks/verify_truehd_interop.py --handbuilt`). The library follows FFmpeg, in decoder and encoder through
   one helper (`lossless_fold_step`). Does the specification mean the matrix channel? **Provisional, owner to confirm.**
7. **Negative `output_shift` (§8.2).** The field is signed; the specification allows no negative value and the
   streams have none. The library shifts right (arithmetically) for a negative value (**no oracle**).
8. **IIR state (§9.2, §9.5).** The streams never send `new_states`. The library loads `state[i]` as the history at
   lag i + 1 without scaling by `state_shift`; §9.5's pseudo-code scales the state a filter *produces* by
   `state_shift`, which no stream shows (**no oracle**).
9. **The block data CRC (§8.1, App. B).** Polynomial 0x63; the initial value and the bytes covered are not given and
   no stream FFmpeg writes sets `data_check_present`. The library checks `block_data_bits` (the length, entry 21) and does
   not check the CRC; FFmpeg's strict decode of `hb_data_check`, whose CRC byte is zero, does not object either.

10. **Heavy DRC (§7.1, §12.2).** The restart header is listed with `heavy_drc_present` and, when set, a 9-bit gain and a
    3-bit time. The streams' header has no room for them where the specification puts them (entry 7: its fields after
    `lossless_check` replace reserved bits the streams have), so where they sit cannot be told, and a header that
    sets the bit may be longer than any stream here. The library reads the 16 bits as `tail` and writes nothing for
    heavy DRC; the directory's standard DRC word (§12.1) is read and carried (entry 18). Please give the layout, or
    a DEE stream that sets `heavy_drc_present`.
11. **Where a bypassed LSB comes from (§10.2, §17.1, §17.5).** v3.5 says that with `lsb_bypass` set "the matrix output
    LSB comes from noise" ("rather than the coefficient computation"). The twelve FFmpeg streams whose matrix sets the flag
    (every 24 bit stereo stream; `python3 tools/checks/check_truehd_coverage.py --list`) carry one bit a sample for it and
    decode exactly as FFmpeg's decoder does, so the reading below has that oracle; what is not known is the specification's
    word "noise". The block data
    syntax (§8.1) has no field for it. The library reads one bit a sample, for each matrix with the flag, from the
    sample data (before the channels' residuals) and adds it to the matrix output; that reading is not in the
    specification (**provisional, owner to confirm**). Which noise channel, which bit, and does the stream carry it?
12. **The scale of `peak_data_rate` (§4.4, ERRATA 30).** The specification says raw x 200 x a rate multiplier; FFmpeg's
    streams hold raw values that make raw x rate / 16 equal 9.6 Mbps at every rate. The encoder writes raw =
    peak_data_rate_bps x 16 / rate (18 Mbps by default, Blu-ray's) and keeps the stream within it. Which scale does a
    player's FIFO model use?
13. **The lower presentations of a 6 or 8 channel stream (§2.2, §10.5, ERRATA 31).** FFmpeg's encoder writes no downmix:
    substream 0 holds L and R as they are. The encoder does the same by default (`DownmixMode::kNone`) and can write
    the 2, 6 and 8 channel presentations as downmixes that the later substreams' matrices undo
    (`DownmixMode::kStandard`). Which does a Blu-ray authoring flow expect, and with which gains and limiter?
14. **FIR order at 176.4 and 192 kHz (§9.6, ERRATA 35).** The specification limits substream 0 to order 4 there; FFmpeg's
    encoder writes order 7. The encoder follows the specification and writes streams about 5 % larger than FFmpeg's.
15. **`substream_info` and `extended_substream_info` for three substreams (§4.5, §6.4, ERRATA 17).** The encoder writes 0x3C
    and 0 for a stream of three substreams because that is what FFmpeg's streams have for two. FFmpeg's decoder
    accepts them; nothing shows what a Blu-ray player expects.
16. **A WAV's channel mask as TrueHD channels (§13.1; `forge truehd-encode`).** The specification names the channels (L, R,
    C, LFE, Ls, Rs, Lvh, Rvh, Lc, Rc, Lrs, Rrs, Cs, Ts, ...) and gives no positions, and a WAV names speakers by position.
    The command reads the mask this way: FL FR FC LFE are L R C LFE, SIDE_LEFT and SIDE_RIGHT are Ls Rs, BACK_LEFT and
    BACK_RIGHT are Lrs Rrs when the file also has side speakers (the usual 7.1 mask) and Ls Rs when it has none (the usual 5.1
    mask is front, LFE and back), FRONT_LEFT_OF_CENTER and its right are Lc Rc, BACK_CENTER is Cs, TOP_CENTER is Ts, and
    TOP_FRONT_LEFT, TOP_FRONT_CENTER and TOP_FRONT_RIGHT are Lvh, Cvh and Rvh. The top back speakers have no TrueHD channel
    and are refused. A file with no mask is taken in the order `decode` writes (the default layout for its count). Is that
    the reading a disc authoring flow expects, in particular Ls Rs for a 5.1 whose WAV says back, and Lvh Rvh for the front
    heights? **Provisional, owner to confirm.**
17. **The speakers of the loudness measurement (§13.1; `forge loudness`).** The BS.1770-5 Annex 3 weights are keyed on
    speakers, so each TrueHD channel is given one: L, R, C, LFE, Ls, Rs, Lrs, Rrs, Cs, Lw, Rw and LFE2 the speakers of the same
    name, Lvh and Rvh the top front pair, Cvh the top front centre, Ts the top centre, Lc and Rc the screen pair, Lsd and Rsd
    the side surrounds. The last six are the nearest speaker and nothing in the specification supports them (**provisional,
    owner to confirm**). A stream whose major sync does not name the channels for its count (the assignment has fewer
    speakers than the channels) is refused by `loudness` and measured by `levels`, which needs no weights.

18. **Filter histories across a restart (§7.1, §9.4, §9.5; entry 13).** §7.1 says a restart "resets all codec state". FFmpeg's
    decoder keeps the FIR and IIR filters' histories across a restart, and the library follows it. The evidence is a
    stream that filters from the first sample after a restart: the hand-built `hb_filters`, `hb_extra_data`,
    `hb_drc_word` and `hb_lsb_bypass` (and `hb_marker_last`, `hb_noise_type1`, `hb_data_check`, which share the coding)
    decode to the same samples in FFmpeg's strict decode and in the library when the histories run on; a library that
    zeroed them differed from the first sample of the second interval, by the contribution of the previous interval's
    last samples. Streams from FFmpeg's encoder are not affected, because their first block after a restart is not
    filtered and fills the history before the first filtered sample. Does the specification mean the history to
    survive, or does a restart zero it and FFmpeg differ from the reference decoder? **Provisional, owner to confirm.**
    Evidence: `python3 tools/checks/verify_truehd_interop.py --handbuilt` (the FFmpeg leg for the hand-built set).

19. **The length of the extra data (§16.1, §16.2, §16.8).** `extra_data_length` is "in 16-bit words" (§16.1, §16.2). §16.8's
    capacity rule, `(extra_data_length + 1) x 16 >= evo_frame_byte_length x 8 + 40`, reads as the words of the whole extra data
    less one (a frame of 3 words carries 2), and the encoder and the hand-built writer write that; the writer first wrote the
    whole count. FFmpeg ignores the field: its strict decode of `hb_extra_data` returns the same samples with the length changed
    by -1 and +5 and with a wrong check nibble (`python3 tools/checks/probe_truehd_extra_length.py`), so no oracle decides it.
    Is it the words less one? **Provisional, owner to confirm.**

20. **The lossless check across a join (§7.3; entry 12).** Two streams of one format joined end to end
    (`testdata/truehd/truehd_cat_st48_s24.thd`, `planning/truehd-fixtures.md`): the second stream's first restart header
    carries a lossless check that checks nothing (entry 12). The library reads it as the check of the first stream's last
    interval, finds it wrong and, strict, stops at the join; lenient, it counts the failure and decodes on, to the samples
    FFmpeg's strict decode returns (FFmpeg logs "Lossless check failed - expected 00, calculated e7" as a warning and goes
    on: `-xerror` does not make it fatal). Should a decoder skip the check at a major sync that starts a new stream, and how
    is that told? Recommendation: keep the strict stop, since one file holding two streams is not a disc's stream, and
    document `truehd-check=lenient` for it. Evidence: `python3 tools/checks/verify_truehd_interop.py --forge forge`.

21. **An option that a TrueHD run does not apply (`forge`; planning/truehd-parity-forge.md, question 7).** `forge` has one
    option vocabulary for every command, and a run on TrueHD or MLP applies few of the options in it. The rest were accepted
    and dropped with exit 0: on `decode` `channels=2`, `drc=0.5`, `programme=3`, `verify-content` and the objects directory,
    on `truehd-encode` `src=`, `dialnorm=` and `map=`, on `ts`, `mkv`, `mp4` and `spdif` `mainid=`, `asvc=` and
    `programme=`. A verification flag that is dropped passes without verifying anything, and a second source that is dropped
    is a stream that is not what was asked for. E-AC-3 refuses `verify-content` and `syntax-trace=` by name instead. The rule
    now applied, **provisionally**: a TrueHD or MLP run refuses by name, with a usage error (exit 1), every option it does
    not apply, and says which it does (`decode`, `levels` and `loudness` apply `truehd-presentation=` and `truehd-check=`;
    `truehd-encode` its `truehd-*` options and `verify`; `probe` `json=`, and `detail=` has no TrueHD form yet; `mkv`,
    `mp4`, `ts`, `spdif` and `remux` none; `quiet` and `verbose` always). `decode` also refuses an objects directory and an ADM
    path, which would never be written. The only options warned about are the choices between ways of computing a transform
    (`fast-mdct`, `fast-imdct`, `mode=`), which cannot change the output or any verdict of a codec that has no transform, and
    the two `truehd-` options on a stream that is not TrueHD, as before. Not covered: `demux`, `unspdif` and the positional
    arguments a command does not read, which `forge` ignores for every codec. Refuse, warn, or refuse only some? Recommendation:
    refuse, and warn only where the option cannot change the output or any verdict, so that a verification flag or a second
    source is never dropped. **Provisional, owner to confirm.**

## Entries

Each is numbered; code and documentation cite them as "ERRATA n".

**1. The signature.** TH spec §4.3 (v3.3): 0xB752 for both stream types; v3.0 said 0xB753 for TrueHD. The streams
have 0xB752 (`dumpms.py`: `f8 72 6f ba ... b7 52` for TrueHD, `f8 72 6f bb ... b7 52` for MLP). The library requires
0xB752 (`ErrorKind::kBadSignature`).

**2. Samples per access unit.** §3.3: 40 at every rate. The streams: 40 at 44.1 and 48 kHz, 80 at 88.2 and 96,
160 at 176.4 and 192 (`nominal_samples_per_access_unit`; the blocks of a segment add up to the unit and the decoder
does not depend on the nominal figure). Evidence: `endmark2.py programme_music_stereo.flac 96000 7` encodes 1,607
samples into 21 units (`samples_in_AUs=1680`, one block of 80 in each); at 192 kHz 3,211 samples take 21 units of 160.

**3. The check nibble (§3.2).** The nibbles of the four header bytes and of the substream directory XOR to 0xF; the
major sync is not covered. `nib.py` over five streams (48 and 96 kHz, mono, 5.1, MLP): `{15: 2400}` for every
stream, 2,400 access units each, whether or not the unit carries a major sync.

**4. The major sync CRC (§4.1, App. B).** CRC-16, polynomial 0x002D, initial value 0, over the bytes from the sync
word up to the last two before the CRC; those two are XORed into the register as a big-endian word, not fed to it.
`msync_crc.py set/m51_48.thd mlp/st48.mlp`: the stored CRC equals `crc(bytes[0:24]) ^ be16(bytes[24:26])` in both
(0xE4D1 and 0x2F92); the little-endian reading of the last two bytes gives 0xE4D1 and 0x3489, so it matches the first
by chance and not the second.

**5. The restart header CRC (§7.1).** CRC-8, polynomial 0x1D, initial value 0, over the bits from the first bit of
the sync to the CRC field, the last eight of which are XORed into the register instead of fed to it. Found by
running the candidate polynomials and initial values over the restart headers of the 5.1 stream (300 of them); this is
the only combination of the search that matches all of them.

**6. The substream parity and CRC (§6.2, App. B).** Parity: the XOR of the payload bytes XORed with 0xA9 (as
written). CRC: polynomial 0x63, initial value **0x3C**, over the payload without its last byte, that byte XORed in
(v3.1's note); the parity byte is not covered. Same search as entry 5, over the segments of the 5.1 stream; the
library's strict decode then passes the parity and CRC of every segment of every FFmpeg stream tried (19 exploration streams and the 12 fixtures).

**7. The restart header's tail.** §7.1 lists 19 reserved bits after `noisegen_seed` and `max_shift`, `max_lsbs`,
`max_bits`, `max_bits_repeat` after the lossless check. The header is 113 bits to `ch_assign` in the streams
(confirmed by the CRC), so the four fields take the place of the 19 reserved bits, and the 16 bits after
`lossless_check` are zero in every stream. The `hires_output_timing` and `heavy_drc_present` bits (and the DRC word
`heavy_drc_present` would add) are not in this header: the library reads the 16 bits as `tail` and carries them.
Whether `heavy_drc_present` makes the header longer cannot be seen in streams that never set it (**no oracle**).

**8. The matrix coefficients (§10.2).** The specification sends a coefficient for each channel up to `max_chan`.
The streams send one for each of the `max_matrix_chan + 1` matrix channels and then two more, the noise channels
(the walk of entry 9 stays aligned with the block data only with `max_matrix_chan + 1 + 2` presence bits). Each coefficient is `frac_bits + 2` bits, signed. The value is scaled by
`2^(14 - frac_bits)`; the matrix output is `(sum >> 14)` masked to the output channel's quantiser step, plus the
bypassed LSB when `lsb_bypass` is set (Q11). The noise channel coefficients are zero in every stream; entry 23 is what a
non-zero one does.

**9. The FIR filter's state flag (§9.2).** The specification sends `new_states` for the IIR filter only. The streams
send the bit for the FIR filter as well, always clear (a FIR filter that sets it is refused). Evidence: with the bit
read for the IIR filter alone the two-channel chunk of a FIR filter is 36 bits where the data needs 51; with it read
for both, `check_struct.py set/m51_48.thd 400` prints `AUs 400 segments 800 bad 0` and `set/st192.thd 400` prints
`AUs 400 segments 400 bad 0` (each segment parses to within 16 bits of its end).

**10. The per-channel parameters (§8.3).** `huff_type` (2 bits) and `huff_lsbs` (5 bits) are sent for every channel
whose `params_for_this_chan` bit is set, not under their own presence bits. Same evidence as entry 9.

**11. Sample reconstruction (§11.3).** The residual is `(symbol << lsb_bits) + raw`, then a centring term, then
`huff_offset`, then `<< quant_step`. The centring term is `-(1 << (lsb_bits + 2 - huff_type))` for a Huffman channel
(when the exponent is not negative) and `-(1 << (lsb_bits - 1))` for `huff_type` 0, with
`lsb_bits = huff_lsbs - quant_step`; §11.3 has only the second. The Huffman symbol is signed (-7 upward, App. C). Every
FFmpeg stream tried (19 exploration streams and the 12 fixtures, TrueHD and MLP) decodes exactly with these.

**12. The lossless check (§7.3).** The specification XORs the three low bytes of each sample. The streams: for every
sample of every output channel of the substream, `acc ^= (sample & 0xFFFFFF) << channel` (32-bit), and the check is
`acc ^ acc>>8 ^ acc>>16 ^ acc>>24` in its low byte, where "channel" is the matrix channel the output channel takes
(`ch_assign[output channel]`; the output position until the FFmpeg evidence of open question 6 corrected it). It covers the output of the substream after the output shift, from
the restart header before to the one after (the header carries the check of the interval that *ends* at it; the first
header's value checks nothing). `explore27.py set/m51_48.thd 1000 1`: stored 129, 8, 13, 125, 180, 146 and the
channel-shifted fold gives 129, 8, 13, 125, 180, 146; the plain byte fold gives 17, 41, 214, 201, 40, 103. The library's
decode checks every interval of every FFmpeg stream tried (19 exploration streams and the 12 fixtures) (`m51_48.thd`: 298 intervals, 149 for each of its two
substreams, none failing). Those streams have an identity `ch_assign`. The shift by `ch_assign` (Q6) is provisional: it
is FFmpeg's, found with a stream that permutes it (`python3 tools/checks/verify_truehd_interop.py --handbuilt`, `hb_matrices`).

**13. The FIR and IIR histories (§9.4, §9.5).** The FIR filter predicts from the channel's previous *outputs*. The
"IIR" filter predicts from its previous *residuals* (`output - prediction`), not from previous outputs. The
prediction is `sum >> coeff_q` with one shift for both filters (§9.3 says they are equal); the output is
`(prediction + residual) & ~((1 << quant_step) - 1)`. Histories are kept across blocks and, as FFmpeg's decoder does it, across a restart; they start at zero at the
first one (open question 18, **provisional**; the specification says a restart resets all state).

**14. The end-of-stream marker (§3.5, §16.5).** The last access unit of a stream whose length is not a multiple of
the unit size ends every segment with 32 bits: `0xD234E` (20 bits, `terminatorA`) and a 12-bit count of the samples of
the unit that are padding, after zero bits that fill the segment to the marker. FFmpeg's decoder returns the unit
without them. Evidence, `endmark.py` (FFmpeg encodes n samples; the last unit's bits after its last block):

    n=4001 ffmpeg=4001 last: 00000 11010010001101001110 000000100111   (0xD234E, count 39 = 4040 - 4001)
    n=4025 ffmpeg=4025 last: 00 11010010001101001110 000000001111      (count 15)
    n=4039 ffmpeg=4039 last: 00000 11010010001101001110 000000000001   (count 1)
    n=803 (5.1) both substreams: ...11010010001101001110 000000100101  (count 37 = 840 - 803)
    n=1607 (96 kHz): ...000001001001 (73 = 1680 - 1607); n=3211 (192 kHz): ...000010010101 (149 = 3360 - 3211)

The second value in §16.5, 0x348D3, is `0xD234E` read two bits late (when the zero padding before it ends in "00"):
it is not a second pattern. The decoder takes the last 32 bits of a segment that has at least 32 bits after its last
block as the marker, and removes the counted samples from the unit (`DecodedAccessUnit::padding_samples`). The MLP
encoder writes no marker. **Provisional, owner to confirm** (Q3): the meaning of the 12 bits, from these streams.

**15. Substreams share the sample rows (§2.3, §20.2).** Substream 1's matrices take the channels substream 0 produced
(after its matrices) as inputs, and its restart header's `min_chan` is where substream 0's `max_chan` ends. A decode
of substream 1 therefore decodes substream 0 first. The samples of the units must agree in count. The 5.1 streams
have a stereo downmix in substream 0 and the six channels in substream 1; `Presentation::kBest` gives the six.

**16. The channel meaning (§14.1).** The widths in the specification add up to 64 bits (6+1+1+1+1+7+6+6+5+6+5+5+6+6+1+1);
all of them are zero in the streams FFmpeg writes (**no oracle**). The 8 bytes after `substream_info` of an MLP major
sync are not described: FFmpeg writes `56 3f 00 00 80 80 00 00` (5.1) and `56 03 00 00 80 80 00 1b` (stereo) and
the library carries them as bytes. FFmpeg's MLP stream also has `flags` 0x4000 (§4.1 says bit 14 shall be clear) and
`format_info` 0x2F0F0001 for stereo (group 1 word length code 2, 24 bits; group 2 code 0xF, absent).

**17. The substream info byte (§6.4).** The bit table does not fit the streams: 0x3C (TrueHD 5.1, two substreams),
0x14 (TrueHD stereo and mono), 0x04 and 0x05 (MLP 5.1 and stereo); the library carries the byte and uses
`num_substreams` alone.

**18. The directory entry (§6.1).** `restart_nonexistent` clear means the segment's first block carries a restart
header, and every segment of a unit with a major sync has one. The first-unit flags are 0x2, the others 0x6
(`tail.py`: `{(0,2): 150, (0,6): 2250}` per 2,400 units). An entry with `extra_substream_word` is followed by a
16-bit word (DRC gain 9 bits signed, time 3 bits, 4 bits padding), counted by the check nibble. FFmpeg's strict decode of
`hb_drc_word` returns the samples and no gain, so the word's values have no oracle.

**19. Block parameters carried over.** After a restart header the presence flags are 0xFF, the block size is 8 until a
header sends one (the specification does not give the initial value), matrices and shifts are empty and zero, and
every channel is `huff_type` 0 with `huff_lsbs` 24. The bit that gives each presence flag its group is the order of
§8.2 from bit 7 down; every stream sends 0xFF, so the assignment is **provisional, owner to confirm**.

**20. The Huffman codebooks (App. C).** The three codebooks decode every residual of every stream. Their codes use
255/256 of the code space (the codes are prefix-free: checked at compile time), so an unused 9-bit prefix is an error
(`ErrorKind::kBadBlockData`).

**21. Block data length (§8.1).** With `data_check_present`, `block_data_bits` counts the block's data from the start of
its own 16-bit field to the end of the sample data, where the block data CRC byte follows: a block whose samples take
n bits carries n + 16. The first `hb_data_check` counted from the end of the field, and FFmpeg's strict decode of it said
`block data length mismatch`; counting from the start of the field it decodes without a message
(`python3 tools/checks/verify_truehd_interop.py --handbuilt`). **Provisional, owner to confirm.** No stream FFmpeg writes sets
the flag. FFmpeg accepts a zero CRC byte (question 9), so the CRC has no oracle.

**22. Extra data (§16.1) and the terminators written by `tools/references/truehd_writer.py`.** Read and written as
the specification lays them out: a header word (`header_check_nibble`, `extra_data_length`), a word with
`evo_reserved` and `evo_frame_byte_length`, the payload, zero padding, and the parity byte; the length is taken as the
words of the whole extra data less one (question 19), and the decoder does not depend on it. The payload
is carried as opaque bytes. `terminatorB` (0x1234) is read right after the last block's flag of substream 3. No stream
FFmpeg writes has either. FFmpeg's strict decode of `hb_extra_data` returns the samples and no payload, so the extra data's
layout has no oracle; `terminatorB` is in `hb_sixteen_channels`, whose restart sync 0x31EC FFmpeg refuses
(`testdata/truehd/handbuilt/`).

**23. Noise generator 0 (§17.2, v3.5).** The two noise channels sit after the matrix channels in the coefficient order
(coefficient indices `max_matrix_chan + 1` and `+ 2`, which entry 8 found in the streams). Each sample, in order:
`shr7 = seed >> 7`; channel 1 is `int8(seed >> 15) << noise_shift`; channel 2 is `int8(shr7) << noise_shift`; then
`seed = (seed << 16) ^ shr7 ^ (shr7 << 5)` in 32 bits, where `shr7` is the 16 bits `(seed >> 7) & 0xFFFF` (Q5: FFmpeg's
decoder reads it so; the 32-bit `seed >> 7` of the text gives another sequence). The seed is loaded from `noisegen_seed` at each restart and
runs on across blocks and units; it takes one step for every sample, whether or not a matrix draws on the noise.
`matrix_noise_shift` is not sent for sync 0x31EA (the reader sends it for 0x31EB, §10.2). No stream FFmpeg writes has a
non-zero noise coefficient, so the generator is checked on the hand-built `hb_noise_generator_0`, whose every sample
FFmpeg's strict decode reproduces (1600 samples, three restart intervals; Q5).

## Encoder (phase T2a)

Entries 30 and up come from writing a stream and reading it back in FFmpeg's strict decode
(`tools/checks/verify_truehd_encode_interop.py`). That is a stronger check of a reading than decoding FFmpeg's own
streams: the encoder writes things FFmpeg's encoder never does, and FFmpeg's decoder says whether they are right.

**30. `peak_data_rate` (§4.4).** FFmpeg's streams hold 3199 at 48 kHz, 3482 at 44.1, 1599 at 96 and 799 at 192 (the bytes
after the signature: `8c 7f`, `8d 9a`, `86 3f`, `83 1f`; the top bit is `is_vbr`). raw x rate / 16 is 9.597, 9.597, 9.594
and 9.588 Mbps; raw x 200 is 640, 696, 320 and 160 kbps for streams of 1 to 4 Mbps. The encoder reads the field as
raw x rate / 16 and writes `peak_data_rate_bps x 16 / rate`. Nothing decodes differently. **Provisional, owner to
confirm** (Q10).

**31. How the channels are spread over substreams.** FFmpeg's TrueHD 5.1 streams have two substreams: substream 0 codes
rows 0 and 1 (L, R), substream 1 rows 2 to 5, `ch_assign` is the identity and there are no matrices
(`dump_struct.py truehd_m51_s24.thd`); a 2 channel decode therefore gives L and R unchanged. The encoder spreads
channels the same way, and puts rows 6 and 7 of an 8 channel stream in a third substream. At 176.4 and 192 kHz a stream
has one substream (§9.6). FFmpeg writes no 7.1 stream, so the three substream form has FFmpeg's decoder as its only
check: it decodes it (entries 32 to 34).

**32. A substream above 6 channels (§7.2, §10.2).** A stream whose third substream (rows 6 and 7) has restart sync 0x31EA
is refused by FFmpeg's decoder: `9 channels (more than the maximum supported by the decoder) is not implemented`, then
`No restart header present in substream 2` for every unit. The same stream with that substream's sync 0x31EB
(`noise_type` 1) decodes. A matrix in a `noise_type` substream sends one coefficient flag for each matrix channel and a
4 bit noise shift after them; the two flags for the noise channels that entry 8 found in the streams without noise are
absent. With them FFmpeg reports `Invalid channel 15 specified as output from matrix` and `substream 2 length
mismatch`; without them the 29 intervals of a 3 s 7.1 stream decode bit-exactly. The encoder writes 0x31EB for a
substream whose last row is above 5, with a zero seed, zero shifts and no matrix drawing on a noise channel. The decoder
accepts a `noise_type` restart and refuses only the matrices that need generator 1's table (below; the table is still
not available, Q5). **Provisional, owner to confirm.**

As a decoder, then: a matrix under noise_type 1 whose `matrix_noise_shift` (TH spec §17.3) is non-zero injects
noise from generator 1's table, which is not available. The decoder refuses it (`ErrorKind::kUnsupported`), as it
refuses LSB bypass under that sync, and decodes the restart when every shift is zero. `hb_noise_type1` (decodes) and
`hb_noise_type1_shift` (refused) hold both outcomes, in the decoder and in `tools/references/truehd_syntax.py`.
FFmpeg's strict decode of `hb_noise_type1` equals the library's; it also decodes `hb_noise_type1_shift`, with a noise table
of its own that this library does not have (`verify_truehd_interop.py --handbuilt`, outcome `decodes`).

**33. FFmpeg's channel order and the six channel field.** For 7 and 8 channel streams FFmpeg's decode returns the channels
in the order of its own layout bits: a stream in the order L R C LFE Ls Rs Lrs Rrs comes back as L R C LFE Lrs Rrs Ls Rs
(columns 0 1 2 3 6 7 4 5), and L R C Ls Rs Lrs Rrs as columns 0 1 2 5 6 3 4. A 7 channel stream whose substream 1 held
6 rows while `sixch_decoder_channel_assignment` named 5 channels (0x0B) was refused with `channel count mismatch`; with
substream 1 holding the 5 rows the field names (Lrs and Rrs in substream 2) it decodes. The rule used: substream 1
holds as many rows as the assignment's low four bits name. **Provisional, owner to confirm.** FFmpeg also refuses a major sync
whose six channel assignment is zero (the first hand-built streams had one), so the hand-built writer now writes the
assignment as the encoder does: in the eight channel field and, as far as six channels fit, in the six channel field.

**34. A substream's matrices (§2.3, §20.2; corrects entry 15).** Entry 15 had substream 1 take the rows substream 0
produced after its matrices. FFmpeg's decoder gives it the rows before them: a 3 channel stream with a matrix in
substream 0 alone decodes with the matrix's output channel equal to its pre-matrix value (`L - R` where `L` was
expected) in every restart interval that has one, and the same stream with the matrix also in substream 1 decodes
bit-exactly. So a substream's matrices shape that substream's own output and nothing a later substream reads. The
decoder now works on a copy of the rows for the matrices and the output. The encoder puts a pair's matrix in every
substream that decodes both rows. **Provisional, owner to confirm.**

**35. FIR order at 176.4 and 192 kHz (§9.6).** `dump_struct.py truehd_st192_s24.thd` shows FFmpeg's 192 kHz stereo stream
with order 7 filters, where §9.6 allows 4 in substream 0. The encoder obeys the specification: its 192 kHz stereo stream
is 5.5 % larger than FFmpeg's (`verify_truehd_encode_interop.py`, `music-st192-s24`). Q12.

**36. The restart header's `max_*` fields (§7.1, entry 7).** FFmpeg writes `max_shift` 0 and `max_lsbs` 18 and 19 (24 bit
sources, first interval) and 10 with `max_shift` 7 (a 16 bit source), `max_bits` and `max_bits_repeat` 21 for stereo and
mono and 24 for 5.1, always equal to each other. `max_shift` is the output shift and `max_lsbs` the largest `huff_lsbs`
the interval sends; the other two vary in a way no simple rule explains. The encoder writes the output shift, the
largest `huff_lsbs` it uses in the interval, and 24 for both others. FFmpeg's decoder does not use them.
**Provisional.**

**37. `input_timing` and the rate model (§3.4, §18).** FFmpeg's units carry `(n - 1) x samples_per_unit` mod 65536, and its
restart headers `output_timing` = the first sample of the unit mod 65536. The encoder writes the same unless the unit
could not arrive at the declared peak rate in time: the unit may start to arrive when the previous one has finished and
no sooner than one unit duration after the previous start (§3.4), and the stream fails (or counts a `rate_violation`) if
a unit would finish arriving later than 75 ms after its slot (§18).

**38. Downmix presentations (§2.2, §10.5).** The specification describes substreams 0 and 1 as the 2 and 6 channel
presentations of an 8 channel stream, and §10.5 says the matrices of a later substream take the folded-in channels out
again; it gives no gains and no limiter. `DownmixMode::kStandard` writes one reading: substream 0 holds
`L + gC*C + gLFE*LFE + gS*Ls` and the same for R (defaults 1/sqrt(2), 0 and 1/sqrt(2)); in an 8 channel stream
substream 1 holds a 5.1 whose surrounds are `Ls + gR*Lrs` (gR 1/sqrt(2)) and substream 2 undoes that. Each later
substream's matrices carry every earlier step, because a substream reads the rows its predecessor decoded before that
predecessor's matrices (entry 34): the stereo steps first (coefficient -gain on each source, 1.0 on the row itself,
`frac_bits` 14, so the sent coefficient is 16 bits), then the rear steps. When a fold would leave a downmix row outside
24 bits in a restart interval, the gains of that fold are multiplied by 0.75, 0.5 and 0.25 in turn and finally dropped,
and the interval is counted in `EncoderStats::downmix_reduced_intervals`. FFmpeg's decoder returns the last
substream only, so its strict decode of these streams checks that the full presentation comes back exactly and nothing
about the downmix; the library's tests read the 2 and 6 channel presentations back and compare them with the formula
(**provisional, owner to confirm the gains and the limiter**; open question 13).

**39. Channel assignment codes (§13.1, §13.2).** §13.2 lists 0x0B as 5.1 and 0x0F as 7.1. §13.1's bit table, which the
streams follow, makes 0x0F L R, C, LFE, Ls Rs (bits 0 to 3: 5.1), 7.1 0x4F (with Lrs Rrs, bit 6) and 0x0B L R C Ls Rs
(5.0). FFmpeg's 5.1 streams carry 0xF, and its strict decode of this library's 7.1 streams (0x4F) is exact. The library
follows §13.1. 0x13 ("additional heights") and 0x17 ("full Atmos configuration") name no layout the bit table can say and are
not used. **Provisional, owner to confirm.**

**40. Blocks per access unit (§3.3, §8.1, §8.4).** The specification says an access unit holds 40 samples and 8 blocks (§3.3,
§8.1), and §8.4 a block of 40 samples and "8 blocks per access unit = 320 samples": the three cannot all hold. FFmpeg's streams
have one block of the whole unit in an access unit without a restart header and two (8 samples, then the rest) in one with it
(entry 19). The encoder writes the same, and the decoder reads any sequence of blocks of 8 to 160 samples that adds up to at
most 255 in a unit.

**41. What the library does not check or model.** Rules the specification states that the decoder does not enforce: §11.4's detection of positive and negative saturation, its
29-bit limit for a Huffman sample and its ninth bit; §9.2 and §9.3's coeff_q of at least 8, `coeff_bits + coeff_shift` of at
most 16, the ban on a coefficient of -32768, the range of `state_bits` and of the state, and the range of a filter's output;
`output_shift` above `max_shift`; §16.8's `extra_data_length` within the unit, its capacity rule and `evo_reserved` zero; §8.4's
two blocks per restart interval and at most two blocks per segment with two substreams; §7.5's restart-gap rules, and §3.3's
minimum of 16 samples per unit. The decoder does refuse more than 4 substreams, a FIR and IIR order above 8 together,
FIR and IIR filters with different coeff_q, and more than 255 samples in a unit. The encoder keeps §9.2 and §9.3's filter limits
(`libs/truehd/tests/encoder/test_coding.cpp`) and writes extra data that fits §16.8; the hand-built writer keeps coeff_q at 8 or more. Two
rules the encoder does not keep: it writes a restart interval of 1 unit throughout when asked (`restart_interval_units` 1),
which §7.5's non-splice rule forbids (a gap under 8 must be followed by one of 8 or more), and FFmpeg's strict decode reads
such a stream exactly (the quick matrix of `verify_truehd_encode_interop.py` has the case); and neither the encoder nor the
decoder models §18.1's 30,000-byte FIFO, the encoder keeping the peak rate unit by unit (entry 37), a weaker rule than the
buffer.
