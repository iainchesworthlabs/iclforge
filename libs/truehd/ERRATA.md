# iclforge::truehd: where the specification and the streams disagree

"TH spec" is the owner's *Dolby TrueHD (MLP) Reference Specification*, v3.5, except §15, which is v3.6's (planning/truehd.md says what it is and
how it is used; section numbers are v3.3's, except §16.3.3 (v3.4), §17 (v3.5) and §15 (v3.6)). Every entry below is a place where the specification is silent, contradicts itself or disagrees
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

   v3.6 (2026-10-29) adds a second description of the same bytes: §15.4 gives the evolution frame as a bit-level container (the EMDF
   container of ETSI TS 102 366 with renamed fields), and §16.3 still says Thrift. v3.6 does not relate the two. The comparison is in
   "Atmos, spec v3.6" below; the container question is now question 22, and this question's digest scope and signing granularity stay open.
2. **The 16-channel extension of the channel meaning (§14.2).** TH spec v3.9 §14.2 answers the unit (TH-2): `extra_channel_meaning_length`
   is 4 bits and the body is (value + 1) 16-bit words, 2 to 32 bytes, read from DEE's channel meaning reader. The library still finds
   the size by the CRC, now trying the two sizes the length gives first (the nibble inside the first word of the body, or the
   body after the nibble, which takes one more word) and then the old search, and reports `body_words()`, whether the confirmed size
   is one of those two (`size_from_length`) and which (`nibble_in_body`). Not said, and no stream has one (**no oracle**): where the
   4-bit length sits against the first word of the body, whether the CRC follows on a 16-bit boundary as the library assumes, and what
   "extended_substream" is in the field list (the library takes `extended_substream_info != 0`). `forge probe DISC detail=structure`
   prints the length and the size the CRC confirmed (section H); `test_access_unit.cpp` builds both placements. **Length confirmed by
   TH spec v3.9 §14.2; the placement is question 44.**
3. **Terminators (§3.5, §16.5).** TH spec v3.9 §3.5 and §16.5.4 answer it (TH-3): DEE writes terminatorA (`0xD234E` or `0x348D3`)
   after the segments of substreams 0 to 2 and terminatorB (`0x1234`) after substream 3 in a pass that can be switched off, so a stream
   has them or has none and a decoder accepts either; the 12-bit count after terminatorA is a real field, `zero_samples`, gated by
   `zero_samples_indicated` in DEE (its width not isolated: the library keeps 12 bits, which FFmpeg's end-of-stream marker shows,
   ERRATA 14); the end-of-stream marker `0xD234D234` is another thing. The decoder takes either pattern of terminatorA with its count
   as the last 32 bits of a segment, and terminatorB at once after the last block or as the last 16 bits of the segment of substream 3
   (entry 46). Open: where in the segment DEE puts terminatorA relative to the padding (the library takes the end of the segment and
   FFmpeg-made streams agree; a terminator right after the last block is counted and refused in strict mode), and the width of
   `zero_samples`. A disc's stream settles it: `forge probe DISC detail=structure` counts what follows the last block of every
   substream's segments (section H).
4. **Which presentation `forge decode` gives for a stream with the sixteen-channel substream.** The library's
   default (`Presentation::kBest`) is the highest of substreams 0 to 2, the 7.1 bed; `kAll` reads substream 3.
5. **Noise generator 1 (§17.3, §17.4).** Answered by TH spec v3.7 §17.4 (2026-10-29), which supplies the 256-entry table. The decoder
   now decodes restart sync 0x31EB, matrix noise shifts included, and LSB bypass under it (entry 44 says how, and which details v3.7
   leaves open: question 43). Generator 0 (`noise_type` 0, sync 0x31EA) is decoded from §17.2 (entry 23). Also asked, for generator 0:
   is `seed_shr7` in §17.2 a 32-bit or a 16-bit value? The recurrence gives different sequences (bits 16 and up of the new state).
   FFmpeg's decoder reads it as 16 bits: on the hand-built `hb_noise_generator_0` its strict decode equals the library's over the whole
   stream (3200 values, the first restart interval's 1280 among them) with `shr7 = (seed >> 7) & 0xFFFF`, while the 32-bit reading
   diverged at the third noise sample. The library reads 16 bits. TH spec v3.7 §17.2 says the same (`seed_shr7` is 16 bits, TH-5):
   confirmed by TH spec v3.7 §17.2; awaiting the DEE score. Evidence: `python3 tools/checks/verify_truehd_interop.py
   --handbuilt` (`hb_noise_generator_0`).
6. **The lossless check's channel shift (§7.3).** Entry 12: the fold shifts a sample by its channel. The FFmpeg-made
   streams cannot say whether that is the output position or the matrix channel `ch_assign` names, because their
   `ch_assign` is the identity. FFmpeg's decoder shifts by the matrix channel an output channel takes (entry 42 says which way `ch_assign` reads): its strict decode of a stream
   built with a permuted `ch_assign` and the position shift says `Lossless check failed - expected 23, calculated ec`
   (the first `hb_matrices.thd`, its major sync patched to a channel assignment FFmpeg accepts), and 0xEC is the fold
   shifted by the matrix channel. The committed `hb_matrices` carries 0xEC and FFmpeg decodes it to the stream's signal
   (`python3 tools/checks/verify_truehd_interop.py --handbuilt`). The library follows FFmpeg, in decoder and encoder through
   one helper (`lossless_fold_step`). TH spec v3.9 §7.3 answers it: the shift is by the matrix channel, `mat_ch = ch_assign[out_ch]`,
   the sample masked to 24 bits after `output_shift`, the fold since the previous restart header, the first header after a start or
   seek checking nothing (TH-6). **Confirmed by TH spec v3.9 §7.3; awaiting the DEE score.** A Blu-ray stream with a permuted `ch_assign` fits the matrix channel for both this fold and `output_shift`
   (entry 43); entry 45 has the rest.
7. **Negative `output_shift` (§8.2).** The field is signed; the streams FFmpeg writes have none. TH spec v3.9 §8.2.1 answers it
   (TH-7): the field is a signed 4-bit value in [-8, +7] and a negative one is an arithmetic right shift, which DEE writes to gain
   headroom (FFmpeg's decoder does not apply it: it forces the shift to zero). The library shifts right (arithmetically) for a negative
   value; `hb_output_shift_negative` holds it (FFmpeg's decode of that stream differs from the signal, and the manifest says so).
   **Confirmed by TH spec v3.9 §8.2.1.**
8. **IIR state (§9.2, §9.5).** The streams FFmpeg writes never send `new_states`. TH spec v3.9 §9.2 answers it (TH-8): each value is
   loaded as `sbits(state_bits) << state_shift` and becomes the IIR history at lag i + 1. The library loads it so (`read_filter`
   scales when it reads; the history takes the scaled value), which an earlier version of this entry described wrongly as unscaled.
   `hb_iir_state` holds it with a non-zero `state_shift` and FFmpeg's strict decode of it equals the signal. **Confirmed by TH spec
   v3.9 §9.2.**
9. **The block data CRC (§8.1, App. B).** Polynomial 0x63; the initial value and the bytes covered are not given and
   no stream FFmpeg writes sets `data_check_present`. The library checks `block_data_bits` (the length, entry 21) and does
   not check the CRC; FFmpeg's strict decode of `hb_data_check`, whose CRC byte is zero, does not object either.

10. **Heavy DRC (§7.1, §12.2).** The restart header is listed with `heavy_drc_present` and, when set, a 9-bit gain and a
    3-bit time. The streams' header has no room for them where the specification puts them (entry 7: its fields after
    `lossless_check` replace reserved bits the streams have), so where they sit cannot be told, and a header that
    sets the bit may be longer than any stream here. The library reads the 16 bits as `tail` and writes nothing for
    heavy DRC; the directory's standard DRC word (§12.1) is read and carried (entry 18). Please give the layout, or
    a DEE stream that sets `heavy_drc_present`. A disc's stream shows whether the 16 bits that end the fixed part are ever non-zero:
    `forge probe DISC detail=structure` (section H).
11. **Where a bypassed LSB comes from (§10.2, §17.1, §17.5).** Answered by TH spec v3.7 §10.2.1 and §17.5 (2026-10-29): with
    `lsb_bypass` set the block data carries one explicit LSB per sample for that matrix, read in sample order, and it has nothing to do
    with the noise generators or `noise_type`. That is the reading the library had (one bit a sample, for each matrix with the flag, from
    the sample data before the channels' residuals, added to the matrix output after the quantiser mask), now confirmed by TH spec
    v3.7 §10.2.1 and also decoded under restart sync 0x31EB, which it used to refuse (`hb_noise_type1_lsb_bypass`). The twelve FFmpeg
    streams whose matrix sets the flag (every 24 bit stereo stream; `python3 tools/checks/check_truehd_coverage.py --list`) decode
    exactly as FFmpeg's decoder does.
12. **The scale of `peak_data_rate` (§4.4, ERRATA 30).** The specification said raw x 200 x a rate multiplier; FFmpeg's
    streams hold raw values that make raw x rate / 16 equal 9.6 Mbps at every rate. TH spec v3.9 §4.4 answers it (TH-12): the unit is
    bits per second, `(raw x group 1 sample rate + 8) >> 4`, with no x 200 factor (raw 6000 at 48 kHz is Blu-ray's 18 Mbit/s); the
    field feeds a player's buffer model and changes no decoding. The encoder writes raw = peak_data_rate_bps x 16 / rate rounded down
    (18 Mbps by default, Blu-ray's), the largest value whose rate does not exceed the declared one, and keeps the stream within it;
    `MajorSync::peak_data_rate_bps()` and `forge probe` (text and the JSON member `peak_data_rate_bps`) give the rate.
    **Confirmed by TH spec v3.9 §4.4.**
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
    TH spec v3.9 §16.2 answers it (TH-19): the field counts the 16-bit words after the header word, so the region is `length + 1`
    words, and the four nibbles of the header word (the check nibble and the three of the length) XOR to 0xF; the library's check
    nibble used to be the XOR of the length's three nibbles without the 0xF, which only agreed with itself. It is now
    `nibble_2 ^ nibble_1 ^ nibble_0 ^ 0xF`, in the library, the encoder, the fuzz mutator, the Python references and the 135 vectors of
    `testdata/truehd/evolution`. A header word of zero (nibble and length both zero) is the padding-only form: exempt from the rule
    and reported as `ExtraData::padding_only`. **Confirmed by TH spec v3.9 §16.2; awaiting the DEE score.**

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
    `truehd-encode` its `truehd-*` options and `verify`; `probe` `json=` and `detail=evolution`, `structure` and `format` (`detail=frames` and `blocks` have no TrueHD form yet); `mkv`,
    `mp4`, `ts`, `spdif` and `remux` none; `quiet` and `verbose` always). `decode` also refuses an objects directory and an ADM
    path, which would never be written. The only options warned about are the choices between ways of computing a transform
    (`fast-mdct`, `fast-imdct`, `mode=`), which cannot change the output or any verdict of a codec that has no transform, and
    the two `truehd-` options on a stream that is not TrueHD, as before. Not covered: `demux`, `unspdif` and the positional
    arguments a command does not read, which `forge` ignores for every codec. Refuse, warn, or refuse only some? Recommendation:
    refuse, and warn only where the option cannot change the output or any verdict, so that a verification flag or a second
    source is never dropped. **Provisional, owner to confirm.**

22. to 39. The questions v3.6's Atmos section raises (the container, the coding of variable-length numbers, the program assignment, the
    element framing, timing, channel accounting and the rest) are numbered in "Atmos, spec v3.6" below, with what the public standard says
    and a recommendation for each. Section H there says which fields of `forge probe DISC detail=format` answer questions 2, 3, 10 and 11
    above and the Atmos ones, for the owner to run on a disc's stream.

40. **The word length of an MLP stream (§1.4, §5.2; FFmpeg's decoder).** An MLP major sync declares the word length of
    its output (group 1: code 0 is 16 bits, 1 is 20, 2 is 24); a TrueHD stream has no such field. FFmpeg's decoder writes
    a 16-bit MLP stream as 16-bit samples (`ffprobe`: `s16`, 16 bits per raw sample) and every other stream as 24. The
    library returns the 24-bit value a stream decodes to. A stream that is what it declares never shows the difference,
    since the samples of a 16-bit stream have eight zero bits below them, but a stream whose coded samples have a bit set
    below its declared width does: the differential fuzzer's first finding was `mlp_st44_s16.mlp` with two bytes of audio
    changed (the checks put right), whose right channel decoded to 80256 in the library and 80128 in FFmpeg. TH spec v3.9 §7.4
    answers it (TH-21): the declared word width is informational, not a mask; a decoder SHALL NOT clear the bits below bit 8 of a
    stream that declares 16-bit words, since the codec is lossless and the only mask in the codec is the quantiser's. That is what
    the library does; `hb_mlp_word16` holds it (FFmpeg's decode of that stream differs, as the manifest says) and
    `libs/truehd/fuzz/differential_oracle.hpp` still clears the bits on the library's side before it compares with FFmpeg, which it
    could stop doing for a different reason: FFmpeg is then the one that differs. Evidence:
    `libs/truehd/fuzz/regressions/fuzz_differential_truehd_decode/` (replay it with `ICLFORGE_DIFF_MEASURE_ONLY=1` to see the
    outcome). **Confirmed by TH spec v3.9 §7.4.**

41. **A `ch_assign` that is not a permutation (§7.4, entry 42).** §7.4 said only that each value is at most `max_matrix_chan`. A
    stream whose values repeat leaves an output channel with no matrix channel. The decoder refuses it (`kBadParameter`). FFmpeg's
    decoder does not: an output channel nobody was assigned takes matrix channel 0, and of two matrix channels with one output the
    later wins (the differential fuzzer's second finding, entry 42). TH spec v3.9 §7.4 answers it (TH-22): `ch_assign` SHALL be a
    permutation of 0..`max_matrix_chan`, DEE validates exactly that, and a decoder SHOULD refuse a repeat rather than follow a lenient
    reading. **Confirmed by TH spec v3.9 §7.4**; the decoder refuses, as it did. Evidence:
    `libs/truehd/fuzz/regressions/fuzz_differential_truehd_decode/ch-assign-repeats-an-output-channel`, which FFmpeg decodes and the
    library refuses. Which way the permutation reads is a different matter: question 45.

42. **A changed major sync and the filter histories (§7.1, §7.5; entry 13, question 18).** The library carries the filter
    histories across a restart (question 18). It used to forget all codec state when a major sync changed the stream's `kind`,
    `format_info` or substream count, and so also on a change of the channel assignment words alone, which FFmpeg's decoder does not
    do: the differential fuzzer's third finding, `hb_marker_last.thd` with the channel assignment word of its second major sync
    changed, decoded from that unit on to other samples than in FFmpeg's strict decode and in `tools/references/truehd_syntax.py`.
    TH spec v3.9 §9.6 (TH-18, TH-23) answers it: a restart does not clear the histories, and a decoder SHOULD reset them only on a
    change of stream kind, sample rate, or substream or matrix-channel count, never on a bare `ch_assign` or channel assignment
    relabelling, since the histories belong to the matrix channels. The decoder now resets for a change of kind, sample rate (both
    group rates of an MLP stream) or substream count and not for the rest of `format_info`; `test_handbuilt.cpp` rewrites the
    channel assignment word of every later major sync of `hb_filters` and holds the decode to the unchanged one. A restart header
    that changes `max_matrix_chan` inside a stream keeps the histories (FFmpeg does too; v3.9 permits a reset there, and no stream
    does it). A restart header that changes `ch_assign` keeps them too, which the owner's second disc needs (its restart headers change
    `ch_assign` between `(4, 5, 1, 0, 2, 3)` and `(4, 5, 2, 1, 0, 3)` inside the stream): `hb_ch_assign_changes` cycles those two
    assignments at every restart of a six-channel stream whose channels all have a FIR filter, FFmpeg 6.1.1's strict decode returns its
    signal, and `test_handbuilt.cpp` holds the decode to it (the test fails when the decoder drops the histories at such a restart).
    **Confirmed by TH spec v3.9 §9.6; awaiting the DEE score.** The differential harness
    (`libs/truehd/fuzz/differential_oracle.hpp`) still skips a stream whose major syncs differ; it could stop. Evidence:
    `libs/truehd/fuzz/regressions/fuzz_differential_truehd_decode/major-sync-format-info-changes-mid-stream`.

43. **The details of noise generator 1 that §17.3 leaves out (entry 44).** `access_unit_size_pow2`, the starting index (`index_orig`) and
    the unit the buffer is filled for are not defined in v3.7. The library takes 64 << the rate factor entries, one buffer for every access
    unit of the substream (a unit with no noise draw too), and for matrix `k` of `N` the start index `N - k` with stride `2 (N - k) + 1`,
    which is what FFmpeg 6.1.1 does on hand-built streams. Does DEE do the same? **Recommendation:** keep it; every DEE leg with a
    non-zero matrix noise shift is the check. **Provisional, owner to confirm.**

44. **Where the length nibble of the extension of the channel meaning sits (§14.2, question 2).** v3.9 says the body is
    `extra_channel_meaning_length + 1` words and gives the field as 4 bits at the start of the extension. Is the nibble inside the first
    word of the body (the extension is `length + 1` words in all) or in front of it (the extension is `length + 1` words after the
    nibble, padded to a word)? The library confirms whichever size the CRC confirms and says which. **Recommendation:** nothing until
    a stream with the extension shows the CRC position; `forge probe DISC detail=structure` prints `extra_channel_meaning_length` and
    the confirmed size.
45. **Which way `ch_assign` reads (§7.3, §7.4, entry 42).** v3.9 §7.4 reads "The 6-bit `ch_assign` field maps output positions to matrix
    channels" and §7.3's pseudo-code takes `mat_ch = ch_assign[out_ch]`; v3.5 to v3.6 had "maps matrix channels to output channels". The
    stream's `ch_assign[m]` as read from the bits is, in FFmpeg's decoder and in this library (entry 42, `hb_output_shift_permuted`
    and the three-cycle stream of `test_channel_assignment.cpp`), the output position of matrix channel `m`; the array that maps an output
    position to its matrix channel is its inverse, which FFmpeg holds in memory and the pseudo-code is written from. The two readings
    decode a permutation that is not its own inverse differently, and the new wording, taken for the bits of the stream, is the other one.
    The owner's Blu-ray decides the stream: its second and third substreams carry `ch_assign` (0, 5, 1, 4, 2, 3) and (0, 6, 1, 4, 2, 3,
    7, 5), neither its own inverse, and with the library's reading every lossless-check interval of the 2, 6 and 8 channel presentations
    passes and the 8 channel output equals FFmpeg's (planning/truehd-disc-findings.md, question 6; entry 43). So the library's reading is
    what Dolby's encoder writes, and what is left is the wording. **Recommendation:** word §7.4 as "the field, read in order, gives the
    output position of each matrix channel; the inverse maps output positions to matrix channels", and keep the pseudo-code's
    `ch_assign[out_ch]` as that inverse.

46. **What `extended_substream_info` 3 means (§4.5).** The table has 0 (none), 1 (8-channel extension) and 2 (16-channel extension, Atmos); the
    owner's Blu-ray, an Atmos 7.1 stream of four substreams of 2, 6, 8 and 12 channels, has 3 in a major sync that carries the 16-channel
    fields. The library takes any non-zero value as "the 16-channel fields are present" and ignores the rest. Is 3 "both extensions", the
    value of an Atmos stream, or a bit field (bit 0 the 8-channel extension, bit 1 the 16-channel)? **Recommendation:** treat it as a bit
    field, which fits 0, 1, 2 and 3, and keep the library's reading until the owner says.

47. **The restart syncs of §2.3, §20.1 and Appendix A (§7.2).** v3.9 §7.2 says 0x31EC marks the 16-channel Atmos substream only and a
    non-Atmos substream carries 0x31EA or 0x31EB whatever its index. §2.3 and §20.1 still say all substreams share one 13-bit restart sync
    (0x18F5, with `noise_type` as the 14th bit, which cannot tell 0x31EC apart), and Appendix A lists 0x31EB for "substreams 0/1" and
    0x31EC for "substreams 2/3". **Recommendation:** follow §7.2 (the library does) and bring the three places in line.
48. **Downmix gains beyond 2 (§10.5, entry 49).** DEE takes any coefficient of magnitude below 16 (32 for Atmos), with the output shift
    for headroom; the encoder takes 0 to 1.99. Should it take more, and negative ones? **Recommendation:** not until a mix needs it.

49. **What the packet ahead of the first access unit is (entry 50).** DEE's timecode streams start with 16 bytes before the first access
    unit. The library skips them (a bounded search for a major sync that reads and is followed by an access unit that reads) and
    says how many; it does not read them. Is the packet something a decoder should carry (a timecode to report), and is the limit of 64 KiB
    right? **Recommendation:** skip and warn, as now; carry it only when something needs the timecode.
50. **The 96 kHz legs of the DEE gold set.** `thd-fx-2ch-music-96k24` and `thd-fx-8ch-tones-96k24` decode in forge with every
    lossless-check interval ok, to the samples FFmpeg 6.1.1's strict decode gives, and not to their sources (about -47 dB on music and
    tones, -10 dB on noise and a sweep for the whole set). The decoder is therefore giving what the encoder coded. The FFmpeg-made
    fixtures at 88.2, 96, 176.4 and 192 kHz (`truehd_st88_s24`, `truehd_st96_s24`, `mlp_st96_s24`, `truehd_st176_s24`, `truehd_st192_s24`)
    decode to FFmpeg's strict decode bit for bit with every check ok, the encoder's 44.1 to 192 kHz streams decode to their inputs here and
    in FFmpeg (`verify_truehd_encode_interop.py`), and `hb_noise_generator_1_96k` and `_192k` hold the hand-built path at those rates. So
    the codec is lossless at every rate here, and what differs is upstream of the stream. v3.9 §19.1 makes the apodising filters a
    configuration option, so it is not offered as the reason. **Open; the scorer reports the legs as UNEXPLAINED.** **Recommendation:** ask what DEE
    does to a 96 kHz input before coding (a resampler or a filter that its options name) rather than look in the decoder.
51. **Dialogue normalisation (TH spec §12.5, §14.1; `forge truehd-encode`, `metadata`, `normalize`, `probe`).** The channel meaning has one
    dialogue-norm field for each presentation: 6 bits for the 2 channel presentation (§12.5 gives the range 0 to 63 and no unit) and 5
    bits for the 6 and 8 channel ones ("0 to 31 -> 0 to -31 dBFS"). Open: (a) is the 2 channel field -n dBFS as well, and what do 32 to 63
    mean; (b) is 0 a level (0 dBFS) or "not indicated", as A/52 §5.4.2.8 reserves 0 in E-AC-3; (c) which presentation's value applies when,
    that of the presentation a decoder reads (substream 0, 1 or 2, which is how the library names them: `Presentation::k2ch`, `k6ch`,
    `k8ch`) or that of the largest presentation the stream has, for a decoder that folds a larger one down itself; (d) the 6 and 8 channel
    fields exist in a stream with fewer substreams, and the library takes the presentation names for substream indexes (a stream with one
    substream has a 2 channel presentation whatever its channel count). **Built, provisional, no oracle until the DEE set:** the library
    writes and reads the raw numbers, widths and ranges as §12.5 and §14.1 give them, rejecting a value outside its range by field name;
    `probe` shows each as n and as -n dBFS; `normalize` writes round(-LKFS) clamped to 1..31 for every presentation the stream has (the
    mapping of ATSC A/85 §8 that E-AC-3's `normalize` uses, and the range the 5 bit fields hold, so no value relies on 32 to 63 and none is
    0). **Recommendation:** as built; the DEE set is two encodes of one programme, one with dialnorm 0 and one with another value for each
    presentation, and a read of the field, which settles (a) and (b); (c) and (d) need a statement from the owner or a decode of a stream
    with a known dialogue norm by a decoder that applies it.
52. **Mix level (§12.5, §14.1).** 6 bits for each presentation, no unit, no table, no statement of the range in use. Whether it is a level in
    dB (an offset such as the 80 + n dB SPL of A/52's `mixlevel` in the audio production information, which is an analogy and not evidence),
    a code, or something else is not known. **Built, provisional:** written, read and shown as the raw number, never converted; `normalize`
    leaves it alone, as E-AC-3's `normalize` leaves its mixing metadata. **Recommendation:** raw until the DEE set shows what DEE writes for
    a known mixing level.
53. **Source format (§12.5, §14.1).** 5 bits for the 6 channel presentation and 6 bits for the 8 channel one, none for the 2 channel one; no
    table of values and no reason for the extra bit. Whether it names the layout the stream was authored in (and so duplicates the channel
    assignment of §13), the programme's original format, or something else is not known. **Built, provisional:** raw, as the mix level.
    **Recommendation:** raw; the DEE set is a 5.1, a 7.1 and a stereo programme encoded as such and their fields read.
54. **What the `*_control_enabled` bits gate (§14.1).** `twoch_control_enabled`, `sixch_control_enabled` and `eightch_control_enabled` are one bit each,
    undescribed. If a bit says that a decoder applies the presentation's dialogue norm and mix level only when it is set, a dialogue norm
    written with the bit clear is ignored; if it only says that the fields are present, a decoder reads them either way. **Built,
    provisional:** the bits are written as the line names them (off by default, as in FFmpeg's streams) and are never set on their
    own; `normalize` writes only the dialogue norm and says, for each presentation whose bit is off, that a decoder may ignore the field;
    `metadata` sets a bit with `truehd-control-6ch=on` and the like. The owner's two discs show that the bit is not constant in streams
    that exist: `twoch_control_enabled` is set in the Atmos streams of *Dune: Part Two* and clear in *Blade Runner 2049*
    (planning/truehd-disc-findings.md), so a decoder meets both and the library's default of clear is one of the two. **Recommendation:** leave `normalize` as it is until the DEE set
    shows whether a stream with the bit clear carries a dialogue norm at all; if DEE sets the bit whenever it writes one, `normalize`
    should set it too.
55. **The DRC start-up gains (§12.3, §14.1).** `heavy_drc_start_up_gain` is signed, 6 bits, and `drc_start_up_gain` signed, 7 bits; no unit, and
    "start-up" is not explained: the gain a decoder applies before the first DRC word of the directory (§12.1) or the restart header (§12.2)
    arrives, or the gain at the start of each restart interval, or the reference level. Whether the most negative value means "off" is not
    given. **Built, provisional:** raw, in the stated ranges (-32 to 31 and -64 to 63); nothing applies them (the decoder does not apply
    the directory's DRC word either, entry 18). **Recommendation:** raw until the DEE set; then decide with the owner whether `decode`
    should apply them, which is a separate decision.
56. **What `forge qc` of a TrueHD stream fails (TH spec §7.3, §12.5, §18).** Built, as the parity audit recommended (question 2 of
    `planning/truehd-parity-forge.md`): an integrity gate and the existing loudness presets, exit 6 when either fails. The integrity gate fails on
    a byte the reader does not take for an access unit, a failed check (nibble, major sync CRC, restart header CRC, parity, segment CRC),
    a restart interval whose lossless check fails, and a unit that cannot be sent within the declared peak rate and 75 ms by the encoder's own
    model (the declared rate is `MajorSync::peak_data_rate_bps()`, the unit TH spec v3.9 §4.4 gives and question 12 records; the model is the
    encoder's, so the gate is as provisional as the model is). Left, because each needs a decision: (a) whether a dialogue
    norm that differs from the measurement by more than some tolerance fails a stream: E-AC-3's `qc` reports the difference and does not
    gate it, and for TrueHD the field is all zero in every stream FFmpeg writes with its control bit clear, so a gate would fail them all;
    (b) the rate check's tolerance (question 12 settled the scale: the field is in bits per second), and whether `input_timing` (§3.4) should be held to its three
    constraints (FFmpeg's streams were not checked against them); (c) a minimum count of lossless checks, since a stream shorter than
    one restart interval has none to make; (d) whether a join of two streams (question 20) should be a failure, as it is now, or a note.
    **Recommendation:** none of (a) to (d) until the DEE set shows what a stream that sets the control bit carries; then gate (a) only for a
    presentation whose control bit is set.

60. **How the object substream's block header reads (§8.2, §9.2, §10.2, §15.3; entry 60).** Reported for the owner's discs: the 12 channel
    substream (restart sync 0x31EC) has matrices with `frac_bits` from 0 to 15, and the block parse then fails at a filter order. A spread of
    `frac_bits` over its whole range is also what a header read at the wrong offset looks like, so the first reading to settle is not the
    arithmetic (entry 60) but where the fields sit: the number of coefficient presence bits of a matrix (`max_matrix_chan + 1` and the two
    noise channels, `max_chan + 1`, ...), the matrix noise shift, the FIR state flag, the count of output shifts and quantiser steps, bits
    between the restart header and the first block header. `tools/checks/truehd_substream3_search.py` tries every combination on the
    owner's excerpt and prints parameters and counts only. **Recommendation:** the owner runs it on one disc excerpt per disc; the reading
    that decodes the first restart intervals with every lossless check passing is built into the decoder and the Python reference,
    and this question closes. Status: the first disc excerpt shows (c) of entry 65 (the parse is off after the matrices); the script's first
    run stopped on an empty segment, which it now skips, and it tries the output shifts and quantiser steps of the channels `min_chan` to
    `max_chan` only and 16 coefficient presence bits as well.
    Entry 68 is the latest state: the layout reads the first matrices and not the rest.

61. **Which channel is which object (TH spec §15.6, §15.16, §15.17; entry 61).** Entry 61 reads channel i of substream 3 as object i of the
    payload (beds, then ISF objects, then dynamic objects, TS 103 420 §5.6.4.8; a dynamic-object-only programme with an LFE has the LFE first).
    §15.17 lists 12, 14 and 16 as "Max Channel Count" and §14.2 has `sixteench_channel_count` (5 bits); whether a programme with fewer objects
    is padded with silent channels up to 12, 14 or 16 (so `object_count` is smaller than the channel count) is not said, and the decode
    refuses to name channels when the two differ. How the 7.1 bed of substreams 0 to 2 relates to the channels of substream 3 (the same
    mix, a downmix of it, or independent) is not said either. **Recommendation:** the owner runs `forge decode DISC.thd out.wav objects/ truehd-presentation=all`
    on one disc excerpt and pastes the "object audio metadata" line and the warning, if any, and, where an object sits at a bed position, the
    rendered feed (`truehd-render=`, when built) against the 8 channel decode.
62. **The cadence of the object audio metadata (TH spec §15.5.3; entry 61).** §15.5.3 says one OAMD evolution frame per access unit, with
    timing re-based to the 40 sample unit. The owner's two discs carry one extra-data frame of 64 bytes for every 1536 samples (gaps of 38 and
    39 units, planning/truehd-disc-findings.md, question 19), which is E-AC-3's frame length, and the standard's block offset factor runs in
    32 sample steps to 2016. The reader takes whatever cadence it finds: a payload applies from the first sample of its access unit plus the
    payload's `smploffst`, its update blocks from there plus their offsets, and an object keeps its last position until the next block.
    **Recommendation:** read the 1536 sample cadence as it is on the discs, and the writer (WP1 step 4) write frames at that cadence; the owner
    confirms with `forge probe DISC detail=format` (`evolution.cadence`).

63. **What an Atmos stream this encoder writes is (TH spec §4.5, §7.2, §14.2, §15; entry 63).** The encoder codes the 7.1 bed in substreams 0 to
    2 as for any 7.1 stream and the object channels in a fourth substream of their own, with restart sync 0x31EC, no matrix and a prediction filter
    per channel. The owner's discs most likely express the bed and the objects in terms of each other (the matrices of substream 3 read the rows
    of substreams 0 to 2, entry 34), which codes the channels the two share once; this does not, so its streams are larger than Dolby's for the
    same programme. Whether the larger stream fits the 18 Mbps of a disc is the material's: the 12 channel real-audio test stream in
    `test_encoder_atmos.cpp` peaks at 15.2 Mbps over a unit. Also open: the value of `extended_substream_info` (the discs have 3, §4.5 lists 0 to 2;
    question 46), whether `sixteench_channel_count` counts the LFE (the discs read as 11 for 12 channels with an LFE; the encoder writes the channels
    less an LFE of a dynamic-object-only programme), what `sixteench_dialogue_norm` and `sixteench_mix_level` mean (questions 51 and 52), and the
    12, 14 and 16 channel presentations (question 61). **Recommendation:** write what is written, label the streams as not accepted by every licensed
    decoder, and let the owner's first play of one settle the rest.

64. **What `forge truehd-atmos` writes (entries 61 to 64).** One OAMD payload per 1536 samples (question 62), the first of the update blocks at the
    frame's first sample, the objects' positions at the frame's end and their ramp the frame's length, as `atmos-encode` places an E-AC-3 frame; the
    LFE is channel 0 (a dynamic-object-only programme with an LFE); the evolution frame is the EMDF reading with protection codes 32 and 8 bits of
    zeros, as `iclforge::objects` writes E-AC-3's container. Whether a licensed decoder accepts frames at that cadence, with zero protection bytes
    and no signature (question 37), is not known. **Recommendation:** none until the owner has played one; then the cadence follows what the discs
    show and the protection follows the owner's decision on signing.

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
(the channel `ch_assign` assigns to the output channel, entry 42; the output position until the FFmpeg evidence of open question 6 corrected it). It covers the output of the substream after the output shift, from
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
all of them are zero in the streams FFmpeg writes (**no oracle**). The library now writes them (`EncoderConfig::channel_meaning`,
`forge truehd-encode`), rewrites them in a finished stream (`edit_channel_meaning`, `forge metadata`), shows them (`forge probe`) and
sets the dialogue norm from a measurement (`forge normalize`) and compares it with one (`forge qc`), all provisional and all as raw numbers in the widths given here;
the open points are questions 51 to 56. The field order and widths are from the specification alone, so a mistake in reading them
would be in the encoder and in every decoder of this library alike. The extension that follows when `extra_present` is set (§14.2) is read
as question 44 describes; the encoder does not write it and `forge metadata` keeps it, byte for byte, under the new CRC. The 8 bytes after `substream_info` of an MLP major
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
is carried as opaque bytes (read, provisionally, as an evolution frame: "Atmos, spec v3.6", F). `terminatorB` (0x1234) is read right after the last block's flag of substream 3. No stream
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

As a decoder, then: a matrix under noise_type 1 whose `matrix_noise_shift` (TH spec §17.3) is non-zero injects noise from
generator 1's table. Until TH spec v3.7 supplied the table (§17.4) the decoder refused such a matrix, and LSB bypass under that sync
(`ErrorKind::kUnsupported`); entry 44 is the decoding now. `hb_noise_type1` (every shift zero) and `hb_noise_type1_shift` (a shift of 3)
both decode, in the decoder, in `tools/references/truehd_syntax.py` and in FFmpeg's strict decode (`verify_truehd_interop.py
--handbuilt`).

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

**39. Channel assignment codes (§13.1, §13.2).** §13.2 listed 0x0B as 5.1 and 0x0F as 7.1. §13.1's bit table, which the
streams follow, makes 0x0F L R, C, LFE, Ls Rs (bits 0 to 3: 5.1), 7.1 0x4F (with Lrs Rrs, bit 6) and 0x0B L R C Ls Rs
(5.0). FFmpeg's 5.1 streams carry 0xF, and its strict decode of this library's 7.1 streams (0x4F) is exact. The library
follows §13.1. 0x13 ("additional heights") and 0x17 ("full Atmos configuration") name no layout the bit table can say and are
not used. TH spec v3.9 §13.2 now says the same: the field is a bitmask, 0x0B is 5.0, 0x0F 5.1, 0x4F 7.1, and the "Atmos codes" do
not exist (TH-16). Every mapping of an assignment to channels, to a WAV mask or to a count goes through
`channels_of_assignment` and `channel_count_of_assignment`, which read the bits; nothing in the tree used the old table
(`test_checks.cpp` holds the four codes and shows 0x13 and 0x17 as plain bit sets). **Confirmed by TH spec v3.9 §13.2.**

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

**42. Which way `ch_assign` reads (§7.1, §7.4).** §7.4: "The 6-bit `ch_assign` field maps matrix channels to output
channels." The library first read `ch_assign[output channel]` as the matrix channel that output channel takes. The two
readings are one permutation and its inverse, and they are the same whenever the permutation is its own inverse: the
identity, which is every stream FFmpeg's encoder writes, and a swap, which is the permutation `hb_matrices` carries
(`[1, 0, 2, 3, 5, 4]`). No fixture told them apart. The differential fuzzer found a stream that does: `truehd_c40_s16.thd`
with the third substream's `ch_assign` changed to `[0, 1, 2, 2]` (and the checks put right). FFmpeg's strict decode gave
the outputs 0 to 3 the matrix channels 0, 1, 3 and 0: for each matrix channel `m` in order, output `ch_assign[m]` takes
matrix channel `m` (so the later of two assignments to output 2 wins), and the output nobody was assigned (3) keeps its
initial matrix channel 0. That is the reading of §7.4's sentence taken literally, `ch_assign[matrix channel]` being the
output channel. The library now reads it so: output channel `p` takes the matrix channel `m` with `ch_assign[m] == p`, and
the lossless check shifts by that `m`. For a permutation that is not its own inverse (`[1, 2, 0]`) the two readings
decode differently. Evidence: a clean three-cycle, `ch_assign` `[1, 2, 0]`, in a three-channel stream made with
`tools/references/truehd_writer.py` (two or more restart intervals, three FIR channels): FFmpeg's strict decode of it
gives the samples the new reading gives, the lossless check included (no warning), and not those of the first reading;
the committed `tools/references/truehd_syntax.py` agrees. `libs/truehd/tests/decoder/test_channel_assignment.cpp` builds
the same kind of stream and holds the output and the lossless check to the new reading. Is this the way the owner's
specification means §7.4? **Provisional, owner to confirm** (open question 41 is the related one). A `ch_assign` with a
repeated value is refused. Not added to `testdata/truehd/handbuilt`: that set is the fixtures' (a stream with a cyclic
`ch_assign` should join it).

**43. `output_shift` is indexed by the matrix channel (§8.2, §7.3; with entries 12 and 42).** The library shifted the
output channel in position `p` by `output_shift[p]`. On the owner's Blu-ray (a TrueHD Atmos stream, planning/truehd-disc-findings.md)
the second and third substreams carry a permuted `ch_assign` and a different shift for each channel, and the lossless check of every
restart interval failed in both of them, with the 6 and 8 channel presentations stopping at the second restart. The shift is the one of
the matrix channel the output takes (`output_shift[m]` with `m` the channel `output_source[p]` names), the same index the lossless
check's fold shifts by (entries 12 and 42). With that change the 2, 6 and 8 channel presentations of the whole 60 s excerpt (72,753
access units) decode with every lossless-check interval ok (568, 1,136 and 1,704), and the 8 channel output equals FFmpeg's strict decode
sample for sample (2.9 million samples a channel, FFmpeg's channel order of entry 33). With the shift by position, or with the fold by
position, the second and third substreams fail every interval. The streams that carry the same shift for every channel (FFmpeg's, the
encoder's, the hand-built ones) cannot tell the two apart, which is why no fixture caught it. `test_channel_assignment.cpp` and
`test_kernels.cpp` hold both; `tools/references/truehd_syntax.py` reads it the same way. The encoder writes one shift for all channels
and is unchanged. **Provisional, owner to confirm** (the specification's §8.2 does not say which index).
**44. Noise generator 1 (§17.3, §17.4; TH spec v3.7 and v3.9).** Restart sync 0x31EB is `noise_type` 1. Under it a substream fills a buffer of
int8 values for every access unit from the table of §17.4, indexed by `seed >> 15` (low 8 bits), with `seed = (seed << 8) ^ index ^ (index
<< 5)` in 32 bits, and a matrix with a non-zero `matrix_noise_shift` adds `buffer[index] * 2^(shift + 7)` to its sum before the sum is
shifted down by 14 (the quantiser mask and the explicit LSB of `lsb_bypass`, entry 11, come after). The table, the recurrence and the
scale are v3.7's. Three details v3.7 does not give (it names `access_unit_size_pow2` and `index_orig` and defines neither; question 43):

- The buffer has 64 entries at 44.1 and 48 kHz, 128 at 88.2 and 96, 256 at 176.4 and 192 (64 << the rate factor).
- One buffer is filled for each access unit of each substream under `noise_type` 1, whether or not a matrix of it draws noise: the seed
  the restart header loads runs on by that many steps for every unit, so the unit after a unit with no noise shift still starts where
  the buffer fill left it. A restart header loads the seed again. The restart header's own `noise_shift` is generator 0's alone.
- Matrix `k` of `N` (counting from 0 in the order the block header lists them) starts each access unit at buffer index `N - k` and takes
  `2 * (N - k) + 1` as its stride, one stride for every sample of the unit, wrapping at the buffer size; the index runs on across the
  blocks of a unit and starts again at the next unit. A matrix whose shift is zero draws nothing but still counts in `N`.

All three were measured, not read: FFmpeg 6.1.1's strict decode of hand-built streams (`hb_noise_generator_1`, `_96k`, `_192k`,
`hb_noise_type1_shift`, `hb_noise_type1_lsb_bypass`), whose expected samples `tools/references/truehd_writer.py` computes from the table
and the three rules above independently of the decoders, equals those samples in FFmpeg, in `tools/references/truehd_syntax.py` and in the
library, at 48, 96 and 192 kHz, with 1 to 3 matrices, shifts from 1 to 15, a quantised output channel and a restart interval that loads the
seed three times. **Provisional, owner to confirm** (question 43). Held by `libs/truehd/tests/decoder/test_handbuilt.cpp`, `test_kernels.cpp`
and `tools/checks/test_truehd_noise_table.py` (the table typed twice).

**45. The output shift is listed by matrix channel: what the other oracles say (§7.3, §8.2; TH spec v3.9; with entry 43).** Entry 43 is
the owner's Blu-ray; the same fault was found a second way. `output_shift[ch]` for `ch` = 0 to `max_matrix_chan` is the shift of *matrix
channel* `ch`, not of output position `ch`: the output position `p` takes the matrix channel `m` with `ch_assign[m] == p` (entry 42) and
is shifted by `output_shift[m]`; the lossless fold takes the shifted sample (entry 12). TH spec v3.9 §7.3 writes `output_shift[mat_ch]`,
and FFmpeg 6.1.1's strict decode of `hb_output_shift_permuted` (`ch_assign` [1, 2, 0], shifts 1, 2, 3) shifts by matrix channel, which the
FFmpeg-made streams and `hb_matrices` (whose one shifted channel is a fixed point of its `ch_assign`) could not show. The DEE gold set
showed it too: the 6 and 8 channel legs of `testdata/truehd-dee` stopped at access unit 128 with a failing lossless check in the second
substream (`thd-fx-6ch-music-48k24`, `thd-fx-8ch-tones-48k24-dmx6-dmx2`) and decode to their sources, with every substream's check
passing, since this was changed. Held by `hb_output_shift_permuted`, `hb_output_shift_negative` and `test_kernels.cpp`.

**46. What follows the last block of a segment (§3.5, §16.5; TH spec v3.9).** Zero padding to the end of the segment, or a terminator:
terminatorA, `0xD234E` or `0x348D3`, followed by the 12-bit `zero_samples` count, as the last 32 bits of the segment of substream 0
to 2; terminatorB, `0x1234`, as the 16 bits after the last block or the last 16 bits of the segment of substream 3. The decoder accepts all
of them and a segment with none. A terminatorA right after the last block, before the padding, is counted by the probe and refused by the
strict decode (more than 15 bits after the last block), since no stream shows it and the spec's own placement is the end of the
segment. FFmpeg reads the end-of-stream marker as `0xD234E` only: `hb_marker_shifted` (0x348D3 in every unit) and
`hb_sixteen_channels_b_end` are refused by it (the manifest says so), and decode in the library and the Python reference. Held by
`test_handbuilt.cpp` and `test_substream3.cpp`.

**47. `extended_substream_info` is the low pair of the four bits (§4.1, §4.5; TH spec v3.9; the owner's Blu-ray).** §4.1 lists "2+2 bits (low 2 bits
used, upper 2 reserved)" after the substream count. The library read the upper pair as `extended_substream_info` and the lower as
reserved, which the FFmpeg-made streams cannot show (both are zero). The Atmos stream on the owner's Blu-ray (planning/truehd-disc-findings.md,
question 2) has 0x43 there: four substreams, the upper pair 0, the low pair 3, and its major sync carries the 16-channel extension of
§14.2, which the old reading never looked for. The pair is now read and written the other way round and any non-zero value of the low
pair means an extended substream; §4.5 lists 0, 1 and 2 and the disc has 3, which the table does not say. `test_access_unit.cpp` holds
the byte. **Confirmed by the Blu-ray; the meaning of 3 is open (question 46).**

**48. The restart sync of substream 1 (§7.2; TH spec v3.9).** v3.9 §7.2: a non-Atmos substream uses 0x31EA or 0x31EB whatever its index,
0x31EC marks the 16-channel Atmos substream only, and substream 1 SHALL carry 0x31EB unless it is the last substream of a 6 channel
presentation (the third, 8 channel substream of a non-Atmos 7.1 stream carries 0x31EB too). The owner's Blu-ray agrees: 0x31EA in
substream 0, 0x31EB in 1 and 2, 0x31EC in 3. The encoder wrote 0x31EB only for a substream with a channel above 5 (entry 32); it now
writes it for substream 1 of every stream that has a third substream (7 and 8 channels, with or without the downmix presentations) and
0x31EA for the last substream of a 5.1 or stereo stream. The matrices of such a substream carry the 4-bit noise shift, zero, and no noise
channel coefficients (entry 32); FFmpeg's strict decode of those streams is exact (`verify_truehd_encode_interop.py`, the 7 and 8 channel
cases), and `test_encoder.cpp` reads the syncs. The decoder takes 0x31EA, 0x31EB and 0x31EC in any substream and ties nothing but the
`huff_lsbs` limit (31) to 0x31EC; v3.9 says only that 0x31EC marks the Atmos substream, so it does not refuse it elsewhere. §2.3, §20.1
and Appendix A still show the earlier reading (one shared 13-bit sync 0x18F5; 0x31EB for substreams 0 and 1; 0x31EC for substreams 2 and
3): question 47. **Confirmed by TH spec v3.9 §7.2** (its stated confidence is medium-high).

**49. Downmix coefficients (§10.5; TH spec v3.9).** DEE range-checks the downmix coefficients it is given by magnitude: it rejects
`|c| >= 16`, or `>= 32` for an Atmos substream. The encoder's `DownmixGains` (centre, surround, LFE, rear to surround) are refused
outside 0 to 1.99, which keeps them inside DEE's bound and below the 2 the 16 bit matrix coefficient (`frac_bits` 14) can carry; a gain
between 2 and 16, or a negative one, would need a coarser `frac_bits` and the output shift DEE uses for headroom (question 48). Nothing
changed. `test_downmix.cpp` holds the refusal.

**50. Bytes ahead of the first access unit (§3.1; the DEE gold set's timecode streams).** A raw TrueHD file may start with something that is not
an access unit: DEE's streams made with an embedded timecode (`thd-fx-2ch-music-48k24-timecode` and four more) have a packet of 16 bytes
ahead of the first access unit, which `forge` took for an AC-3 file ("no 0x0B77 sync word where a frame should begin") because detection
wanted a major sync at byte 4. What the packet holds was not looked at and is not decided here. The library's `ElementaryReader` already
passed bytes to find a major sync; `find_stream_start` now gives the rule for the start, which the reader, `probe_stream` and `forge`'s
detection share: the first access unit has to read (its check nibble, its major sync with a good CRC-16, its directory), and when it is
not at byte 0 the unit after it has to read too (or the data ends inside it with a header that reads), the search stopping at
`kMaxLeadingBytes` (64 KiB) so that bytes which only look like a major sync are not a stream. The bytes passed are
`ElementaryReader::leading_bytes()`, a part of `skipped_bytes()` that is a warning ("N byte(s) ahead of the first access unit are not
part of the stream and are skipped"), not an error: `forge decode` exits 0 and its strict decode goes on, `probe` exits 0 and its JSON
has `leading_bytes` beside `skipped_bytes` (which stays the loss after the first unit). `probe` looks at the first 80 KiB of a file or
pipe to tell what it is, where it looked at 4 KiB. A stream with an AC-3 core in front or between its units is not changed: the unit after
the first has to read, and an AC-3 frame is not one. Held by `test_elementary.cpp` (the rule, with decoys, a single unit, the limit and
noise) and `test_cli_truehd.cpp` (a fixture with 1, 28 and 4096 bytes of three fills in front: same PCM, a warning, exit 0). The
timecode fixture scores PASS. **Provisional, owner to confirm** (question 49).

**60. Matrix coefficients to `frac_bits` 15 (§10.2; entry 8).** §10.2 gives `frac_bits` as 4 bits and a coefficient of `frac_bits + 2`
bits, so 15 is a legal value and a 17 bit coefficient, finer than the 2^-14 the decoder scaled every coefficient to (the decoder refused
`frac_bits` above 14, and `1 << (14 - 15)` has no value). The decoder now scales the coefficients to 2^-15 and shifts the sum by 15; for any
`frac_bits` up to 14 that is the same output sample for sample (the sum is twice as large and shifted one more), the matrix noise of
§17.3 is scaled to match, and the limit is gone. The encoder still writes `frac_bits` 14 or less and is unchanged. The owner's discs are
reported to carry `frac_bits` from 0 to 15 in the 12 channel substream (restart sync 0x31EC); whether those values are real or the sign of
a block header read at the wrong width is not settled (question 60). Held by `test_kernels.cpp` (a coefficient with the last bit), `test_syntax.cpp`
(17 bit coefficients round-trip), the hand-built `hb_substream3_fine_matrices` (twelve channels, `frac_bits` 15, 14, 13 and 0, decoded
sample for sample and by the Python reference). **Correct for streams that use it (the arithmetic is exact); provisional for the disc
until step 1 of WP1 passes every lossless-check interval.**

**61. Substream 3 as objects (§15.6, §15.16; TS 103 420 §5.5, §5.6.4.8).** `forge decode truehd-presentation=all` gives the channels of
substream 3 (12 to 16) in the order the stream carries them, and `objects_dir=` and `adm_out=` read them as objects: channel i is object i
of the object audio metadata in the stream's evolution frames (payload id 11, the EMDF reading), a dynamic-object-only programme with an
LFE having the LFE as object 0, and a bed programme the Table 12 labels in payload order. Channel i's audio is `object_NN.wav`; the ADM master
has the beds as DirectSpeakers and the dynamic objects as Objects, each update block timed from the first sample of the access unit whose
extra data held it, plus the payload's `smploffst`, plus the update's sample offset, plus 32 samples a unit of the block's offset factor
(TS 103 420 §5.5.7). Nothing is written when the programme and the channel count disagree, or the programme uses intermediate spatial
format, several bed instances or a non-standard bed assignment (the warning says which). `forge decode ... adm_out` without `all` is the
presentation's bed alone (§15.20), each channel a DirectSpeakers channel at the room position of its label. The first programme in the
stream decides the plan; a later payload that describes another programme is counted and not followed. Held by `test_cli_truehd_objects.cpp`
(channel for channel against the multichannel decode, over `hb_atmos_objects`, whose payloads `build_payload` made). **Provisional, owner
to confirm** (questions 61 and 62): only E-AC-3's reader (`libs/objects`) and the stream's own channel count check the metadata.

**63. The Atmos stream the encoder writes (§4.5, §6.4, §7.2, §14.2, §15.3; entries 17, 46, 47, 60 and 61).** `EncoderConfig::object_channels`
(1 to 16, with `channels` 8 and 48 kHz) adds a fourth substream: restart sync 0x31EC, `min_chan` 0, `max_chan` and `max_matrix_chan` the
channel count less one, the identity `ch_assign`, no matrix, a FIR filter per channel as for the others, no terminatorB (the discs have none,
entry 3). The major sync has four substreams, `substream_info` 0xFC, `extended_substream_info` 3, `extra_present` and the 16-channel extension
(`object_meaning`): the 4 bit length is the number of 16-bit words the fields take less one, so a dynamic-object-only programme is the 32 byte
major sync of the discs. `encode()` takes `channels` + `object_channels` values a frame, the bed first. The bed's substreams are the stream a
7.1 encode of the bed alone writes, byte for byte (`test_encoder_atmos.cpp`), so a decoder that reads three substreams finds it. The decode of
the stream by this library and by `tools/references/truehd_syntax.py` (which reads the longer major sync by its CRC, as the library does) is
the bed and the object channels, sample for sample, for the real-audio programme of the test. `verify` decodes each unit with both presentations.
**Provisional, owner to confirm** (question 63); no decoder outside this repository reads restart sync 0x31EC.

**64. The metadata `truehd-atmos` writes (§15.4, §15.5.3, §16.1; entries 61 to 63).** Frame k of the object audio metadata starts at sample
1536 k. It is carried in the extra data of the access unit that holds that sample (40 samples a unit at 48 kHz), the update's own sample offset
(0 to 31) and the block's offset factor (32 samples a unit) adding up to the sample's place in the unit, which is how the reader adds them back
(`forge decode` and `ObjectScene` positions evaluated at the frame's end). The evolution frame is `iclforge::objects::emdf::build_container`
without the 32 bits of sync and length that introduce it in E-AC-3's skip field; the payload configuration is Table 56's, the protection codes
are 32 and 8 bits of zeros. The 7.1 bed is the objects placed on 7.1 speakers by `iclforge::render`'s layout renderer from the positions as the
payload codes them (so it equals `forge decode truehd-render=`'s output exactly). An ADM master (BW64, or a RIFF file with an `axml` chunk) is
read as `atmos-adm` reads one: its LFE channels summed into channel 0 and every other channel, bed or object, a dynamic object on the path the
master gives it (a bed channel pinned at its speaker's position; the stream's own beds, §15.6, are not written); the master's 24 bit samples
survive. **Provisional, owner to confirm** (question 64).

**65. What the owner's first disc shows of substream 3 (§7.2, §10.2, §15.3; entries 60 and 61).** One excerpt of one disc, 12 channels (the
probe's and the search script's own output; nothing of the audio). (a) The restart header reads cleanly: sync 0x31EC (noise type 0),
`min_chan` 8, `max_chan` 11, `max_matrix_chan` 11, a 12 entry `ch_assign` that is a permutation of 0 to 11, a restart header check. So
substream 3 codes the channels 8 to 11 and its matrices read the 7.1 bed's rows 0 to 7 from substreams 0 to 2; it is not 12 independent
channels, and the `ch_assign` order is the row order of the 12 outputs. (b) The first block header sends 13 primitive matrices with
`frac_bits` 15, 11, 7, 13, 0, 12 and others, every coefficient the width `frac_bits + 2`, and a presence bit for each of the 12 matrix
channels and the two noise channels; several matrices have output channel 15, above `max_matrix_chan`. The decoder refused that
("matrix output channel above max_matrix_chan"); the field is 4 bits and a row buffer has 16 rows, so the limit is gone. (c) The fields after
the matrices do not read: the output shifts and quantiser steps come out as 13, 14, 9, ... and the next filter has a 19 bit coefficient, so
the parse is off by then, at a field before it or in the matrices past what was printed. (d) The evolution frames read as the EMDF container
in 229 of 229 access units that carry extra data (payload id 11, 50 or 51 bytes), 227 of them a dynamic-object-only programme with an LFE,
11 dynamic objects and one update block: the 12 channels are the LFE and 11 objects, as entry 61 reads them. The check nibble, framing and
parity of all 229 pass. **Provisional**: (a), (b) and (d) are what the disc shows; (c) is the open part of question 60.

**66. AC-3 core frames between the access units of a Blu-ray track (§3.3, §21.2; planning/truehd-disc-findings.md).** A TrueHD track of a
Blu-ray transport stream carries the programme's AC-3 core (here 448 kbit/s, 1792 bytes, one every 38 or 39 access units) between its
access units. `forge decode` and `probe` set the frames aside when they read the transport stream itself, or a bare stream that begins with
one, but not for the bare stream `forge demux` writes from it, which begins with a TrueHD unit. `ElementaryReader` then took the first frame
for a unit that does not read and went on to the next major sync: of the owner's first 9 MB excerpt it read 5,476 units and skipped 7.4 MB,
and `truehd_substream3_search.py` read the frames as units, so its search, its `--directory` table and its restart at unit 5 were of
bytes that are not TrueHD. `ElementaryReader`, `probe_stream`, `find_stream_start` and `tools/references/truehd_syntax.py` now pass over a
frame (the A/52 sync word, 48 kHz, a frame size code of the table, a bit stream id of 8 or less, and the frame inside the data) and count it in
`core_frames()` and `core_bytes()`, not in `skipped_bytes()`. Held by `test_elementary.cpp` and a hand-built stream with a frame every five
units, which decodes to the same samples as without. **Correct.** Not changed: the Hearth player's own index (`truehd_index.cpp`) still walks
units by length alone. Of entry 65, only the restart header, the first block header (a unit with a major sync) and the object audio metadata
stand; the rest of what it says of later units has to be read again on the full stream.

**67. The first block of a restart of substream 3, from the owner's excerpt (§8.2, §10.2; entries 60 and 65).** On the full stream (28,993
units, 33 restarts of substream 3): units 0 and 1 are start-up restarts whose first blocks differ from each other and from the rest; from unit 79
on the 31 restarts, 128 units apart, agree bit for bit on the 1,714 bits after the restart header (the first bit sent is 0, so no new
presence flags and no block size, then a matrix group with a count of 14), after which the sample data differs. The continuation blocks of the
substream read as §8.2 has them for channels 8 to 11 (units 2 and 3: new block size 40; a channel with only `huff_type` 0 and `huff_lsbs` 8). So
what is not read is inside the first block's matrices, or between them and the sample data. Not the layout: no combination of 0 to 63 bits ahead of
the block, 8 to 23 presence bits a matrix, 0 or 4 bits of noise shift, a 1 to 4 bit coefficient width over `frac_bits`, a 4 or 5 bit count, a 4 or 5
bit output channel, the output shift and quantiser step counts of 4, 8, 12, 14 or 16, 4, 12 or 16 channels, and with or without the FIR state flag,
gives matrices whose coefficients stay near the unit range and a tail that is two zero flags and valid filters (coefficient shift 8 or more, widths
to 16, orders to 8 and 4). **Open (question 60).** What would settle it is a controlled input: an Atmos stream from the owner's encoder whose objects
are silent but for one, and a second with that object moved, so that the bits that change are the matrix and filter fields that the one change
touches. Held by `tools/checks/truehd_substream3_search.py` (`--common-prefix`, `--dump-header`).

**68. What DEE's own encodes show of substream 3 (entries 65, 66, 67; question 60; TH spec v3.15 §15.17).** DEE 6.5.4 encodes TrueHD Atmos from the
Dolby Sollevante master at 12, 14 and 16 elements (and with a 6 s silence pad): substream 3 has `min_chan` 8, `max_chan` and `max_matrix_chan` of
elements - 1, restart sync 0x31EC, `extended_substream_info` 3, `substream_info` 0xFC. DEE's own log says the 8 channel bed is a dynamic downmix of the
objects (its matrices come from a file it builds and deletes), the 6 channel and 2 channel presentations folds of that, so the matrices of substream 3
are the inverse of an object-to-bed rendering and differ from restart to restart: the restarts of a DEE encode share 5 to 55 bits after the restart
header, even over silence, where the disc's steady restarts shared 1,714. **Reads:** the matrix layout of entry 10.2 (4 bit output channel, 4 bit
`frac_bits`, 1 bit bypass, 14 presence bits, `frac_bits` + 2 bit coefficients) reads the first matrices of a restart identically in restarts of the same
state 1,100 units apart (a noise-only matrix, an integer matrix with `frac_bits` 0, and a matrix of nine equal coefficients at `frac_bits` 11, whose
14 bit period is one flag and one coefficient). **Does not read:** the rest. No layout of 10 to 18 presence bits, with or without a 4 bit noise shift, and
a coefficient width of `frac_bits` + 1 to 3, fits the count of 12 matrices into the two complete silent restarts (656 and 672 bits), so the matrices are
coded more compactly than the standard form. **Corrections:** entry 65 (c) and entry 67 took repeated stretches for sample data; they are runs of equal
coefficients, and the 1,714 bit steady header on the disc is probably the whole matrix and channel-parameter section of 14 matrices (the standard-form parse reaches
bit 1,133 and then fails), so its length is not evidence of anything about the sample data. No reference exists in the repository: DEE deletes its matrix file, and its debug
log has no per-restart counts. **Open (question 60), parked as WP1b.** What would settle it: a licensed decoder's parse log for one restart interval, or the matrix file.

## Atmos, spec v3.6

TH spec v3.6 (released 2026-10-29) rewrites §15 and leaves every other section as v3.5 has it. It now gives the bit-level syntax of the
object audio metadata (OAMD, §15.5 to §15.15), the program assignment (§15.6), constants (§15.8 to §15.14), a bit-level `evo_frame()`
container (§15.4) and constraints (§15.16, §15.17). The owner said the object data follows ETSI TS 103 420 and §15.1 says the OAMD bitstream
is the one that standard defines. This section sets §15.4 to §15.15 (and the constants of §15.16 and §15.17) against the public text of
ETSI TS 103 420 V1.2.1, statement by statement, and collects what is left for the owner as questions 22 to 39.

**The owner's answer of 2026-10-29 (third round).** Real streams carry both layouts, nested: the bit-level EMDF-style `evo_frame()` of §15.4 is
the outer container, and the Thrift `ContentSignature` of §16.3 sits inside one of its payloads. The answer cites two third-party projects as its
basis (not fetched or read here) and says an HMAC-SHA-256 signs the OAMD and JOC structures, which does not agree with §16.6 (ECDSA P-256) unless
the HMAC is the bits of `evo_protection()` and the ECDSA signature is the nested `ContentSignature`. Only the nesting is taken as the owner's
reading; which payload id carries the Thrift blob, where in the payload it starts, what its digest covers, what the protection bytes cover and
whether a signature comes with every frame or once per stream are **unconfirmed** and are questions 22 and 37. The library follows the nesting
(it reads the §15.4 container, carries any payload that is not id 11 as bytes with its id and size, and does not parse Thrift: `libs/truehd` has
no Thrift reader). `forge probe detail=evolution` walks a Thrift payload with the repository's reader in `libs/base` and prints field ids, types
and lengths only (section H).

**What the comparison rests on.** The standard is E-AC-3's: its scope is E-AC-3 only, and it never mentions TrueHD or MLP. What it can confirm
is therefore the syntax and semantics of the OAMD payload itself (its §5 and Annex A) and the constraints it puts on the EMDF container
the payload travels in (§8.2, Tables 55 and 56). It cannot confirm anything about TrueHD: that the payload is carried in the extra data, which
access unit carries it, how often, what substream 3 holds. Those statements rest on the owner's word alone. Two limits of the method: the
EMDF container's own syntax is ETSI TS 102 366 Annex H, which was not available as text here, so the container rows compare §15.4 with
`libs/objects/src/emdf.cpp` (which cites Annex H's clauses and reads real E-AC-3 streams) and with what TS 103 420 itself says; and the
extracted text of TS 103 420 lost some formula glyphs, so a formula is taken from the text where it survived and from
`libs/objects/src/oamd.cpp` (written from the full document and checked on E-AC-3 streams) where it did not.

**Verdicts.** *agree*: the two say the same. *disagree*: they differ; the standard's reading is stated. *v3.6 silent*: the standard specifies
something v3.6 leaves out. *standard silent*: the public text says nothing (a TrueHD matter, or an identifier the standard reserves). Rows
marked **Q** are in the numbered questions below.

**Headline.** §15.5 to §15.15 do not describe the syntax of TS 103 420 §5.5. Their field widths for the version, object count and element
count extensions, the tables of sample offsets, ramp durations, divergence, extended position precision and trim levels, the ISF object counts
and the bed channel groups agree with the standard. Their structure does not: the program assignment, the element framing, the timing block,
the object info block (status indices), the basic and render info, the trim element, the extended object element and the way variable-length
numbers are coded all differ, and a parser written from §15.5 to §15.15 as printed could not read a payload that `iclforge::objects`' parser
reads. The questions recommend taking the standard's syntax and treating §15.5 to §15.15 as a summary of it.

### A. `evo_frame()` (§15.4) against the EMDF container

The repository reads the EMDF container for E-AC-3 (`libs/objects/src/emdf.cpp`; `libs/ac3/src/emdf/frame_layout.cpp` walks the same shape).
"EMDF" below means that reader and the Annex H clauses it cites.

| # | TH spec v3.6 | The standard / the EMDF reader | Verdict |
|---|---|---|---|
| A1 | `evo_frame()` has no sync word and no length field; the length is `evo_frame_byte_length` in §16.1 | EMDF: `emdf_sync` 0x5838 and a 16-bit `emdf_container_length` in bytes precede the body; TS 103 420 §8.2 requires the payloads "in the EMDF container" | v3.6 differs by design: §16.1's framing stands in for sync and length. Not a conflict if §15.4 is the container body |
| A2 | `evo_version` 2 bits, `+= variable_bits(2, max_ext=16)` when 3 | `emdf_version` 2 bits, `variable_bits(2)` when 3. Annex H defines the container's fields for version 0 only (the reader refuses any other) | agree on width; v3.6 silent on what versions 1 and up change. **Q24** |
| A3 | `key_id` 3 bits, `+= variable_bits(3, max_ext=10)` when 7 | `key_id` 3 bits, `variable_bits(3)` when 7; 0 is "no authentication key" | agree on width; v3.6 silent on what a key id selects. **Q37** |
| A4 | the payload list ends at `evo_payload_id == 0` | the same | agree |
| A5 | `evo_payload_id` is 5 bits, no escape | 0x1F is an escape: the id is 0x1F plus `variable_bits(5)` (the reader implements it) | disagree (v3.6 silent on the escape). **Q24** |
| A6 | OAMD is `evo_payload_id` 11 | TS 103 420 Table 55: OAMD is payload id 11 (JOC 14), "frequency: 1 frame" | agree on the id; v3.6 silent on "once per frame". **Q33** |
| A7 | `smploffst` is `variable_bits(11, max_ext=2)` | `smploffst` is 11 bits followed by one reserved bit | disagree. **Q24** |
| A8 | `duration` `variable_bits(11, max_ext=2)`, `groupid` `variable_bits(2, max_ext=16)` | `duration` `variable_bits(11)`, `groupid` `variable_bits(2)`; Table 56: `duratione` 0, `groupide` 1, `groupid` equal for the OAMD and JOC payloads | agree on the fields; v3.6 silent on Table 56's values and on what `groupid` ties together when there is no JOC payload. **Q24** |
| A9 | `b_codedcdata_present`, then `codedcdata` 8 bits | `codecdatae`, then 8 reserved bits. Table 56 says `codecdatae` 1; Annex H (as the reader quotes it) says 0 for a conforming configuration, and Dolby's E-AC-3 streams send 0 | agree on width; v3.6 calls the byte data (and misspells the name); the standard calls it reserved and contradicts itself on the flag. **Q24** |
| A10 | `discard_unknown_payload`; when 0: `payload_frame_aligned` only without a sample offset, `create_duplicate` and `remove_duplicate` when aligned, `priority` (5) and `proc_allowed` (2) when a sample offset or alignment | the same, bit for bit; Table 56: `discard_unknown_payload`, `create_duplicate`, `remove_duplicate`, `priority`, `proc_allowed` all 0 | agree |
| A11 | `evo_payload_size` is `variable_bits(8, max_ext=4)`, in bytes | `emdf_payload_size` `variable_bits(8)`, in bytes | agree on width and unit |
| A12 | "`variable_bits(n, max_ext)`: read n bits; if the value is 2^n - 1, add it and read n more; repeat up to max_ext times" | TS 103 420 §5.5.1 (`variable_bits_max`), TS 102 366 Annex H and TS 103 190-1 clause 4.2.2: n bits, then a 1-bit read-more flag after every group; each further group adds 2^n to the shifted value. `max_num_groups` in §5.5.1 plays the part of `max_ext` | **disagree**. Under the standard's coding even a small size costs one flag bit, so every field after the first `variable_bits` sits one bit later than under v3.6's text. **Q23** |
| A13 | `evo_payload_byte[size]`: size x 8 bits, not aligned to a byte boundary of the frame | the same: the payload's bits are not byte-aligned in the container | agree |
| A14 | `evo_protection()`: two 2-bit length codes, then the primary and the secondary bytes; `SIZE_TABLE` 0, 1, 4, 16 bytes | `protection_length_primary`, `protection_length_secondary`, then the two protection fields: 0, 8, 32, 128 bits for the codes 0 to 3 (Tables H.2.5 and H.2.6) | agree on the structure and the sizes |
| A15 | primary code 0 means no primary bytes | the primary code 0b00 is reserved (the reader refuses it); only the secondary code 0b00 means none | disagree. **Q24** |
| A16 | the protection is an "HMAC" | the content is "implementation dependent and not defined" (Annex H); TS 103 420 says nothing | standard silent. **Q37** |
| A17 | what follows the protection up to `evo_frame_byte_length` | EMDF: zero bits to the byte boundary implied by the container length | v3.6 silent. The library reports whether the bits after the protection are all zero |
| A18 | the container sits in the extra data region and is `evolution_payload` of §16.1 | TS 103 420 §8.2: the container is carried in the last dependent substream of an E-AC-3 stream; nothing about TrueHD | standard silent: rests on the owner's word |
| A19 | §16.3 says the same bytes are a Thrift Binary Protocol container holding the typed entries and a nested ContentSignature | -- | **internal contradiction** with §15.4 in v3.6; the owner's answer (above) reads them as nested, §15.4 outside and the Thrift blob inside one payload. Which payload, where, and what it covers: **Q22** |

**Field for field.** §15.4's `evo_frame()` is the EMDF container body with `evo_` for `emdf_`: every field of v3.6 has a counterpart in Annex H,
in the same order, and the repository's reader holds it.

| TH spec v3.6 | Annex H (EMDF) | `iclforge::objects::emdf::parse_container` | Differs from v3.6 |
|---|---|---|---|
| (none: §16.1 frames it) | `emdf_sync` 0x5838, `emdf_container_length` | found by scanning for the sync word; the length bounds the container | v3.6 has neither (A1) |
| `evo_version` (+ `variable_bits(2)`) | `emdf_version` | read; any value but 0 is refused (`kUnsupportedConfig`) | refuses 1 to 3 (A2) |
| `key_id` (+ `variable_bits(3)`) | `key_id` | read, the value discarded | -- |
| `evo_payload_id` | `emdf_payload_id` (0x1F escapes) | `DecodedPayload::id`; the escape is read | no escape in v3.6 (A5) |
| `b_smploffst_present`, `smploffst` | `smploffste`, `smploffst` (11 bits) + 1 reserved bit | `PayloadConfig::sample_offset`, -1 when absent | `variable_bits` in v3.6 (A7) |
| `b_duration_present`, `duration` | `duratione`, `duration` | `duration`, -1 when absent | -- |
| `b_groupid_present`, `groupid` | `groupide`, `groupid` | `group_id`, -1 when absent | -- |
| `b_codedcdata_present`, `codedcdata` (8) | `codecdatae`, 8 reserved bits | `codec_data`; the 8 bits skipped | v3.6 calls them data (A9) |
| `discard_unknown_payload` | `discard_unknown_payload` | `discard_unknown` | -- |
| `payload_frame_aligned`, `create_duplicate`, `remove_duplicate` | the same | `frame_aligned`, `create_duplicate`, `remove_duplicate` | -- |
| `priority`, `proc_allowed` | the same | `priority`, `proc_allowed` (0 when absent) | -- |
| `evo_payload_size`, `evo_payload_byte[]` | `emdf_payload_size`, `emdf_payload_byte[]` | `DecodedPayload::bytes`, copied (not byte-aligned in the frame) | -- |
| payload id 0 | `emdf_payload_id` 0 | ends the list | -- |
| `protection_length_primary`, `protection_length_secondary` | the same | read; a primary code of 0 is refused (`kUnsupportedConfig`) | v3.6 allows it (A15) |
| `protection_bits_primary`, `protection_bits_secondary` | the same, implementation defined | skipped, not verified, not returned | -- |
| (none) | zero bits to the container's byte boundary | ignored | -- |

Three further differences between the repository's readers. `parse_container` reads the whole configuration clause and reports it;
`libs/ac3/src/emdf/frame_layout.cpp`, which only needs to know where a container ends, reads the clause in the one shape Table 56 allows
(it reads `smploffste` and `duratione` as flags and does not read the values they gate, so a configuration with either set desyncs it),
and so it is narrower than v3.6's syntax. Neither returns the protection bytes. And the repository's writer (`build_container`) follows
Table 56 except `codecdatae`, which it writes as 0 (question 25).

**What TS 103 420 §8.2 constrains.** The payloads "shall be carried in the EMDF container" (so the container of ETSI TS 102 366
Annex H, which §5.1 names as the one that carries the OAMD bitstream); OAMD is payload id 11 and JOC 14, each once per frame (Table 55);
and the configuration data of Table 56 (`duratione` 0, `groupide` 1 with the same `groupid` for the OAMD and JOC payloads, `codecdatae` 1,
`discard_unknown_payload`, `create_duplicate`, `remove_duplicate`, `priority` and `proc_allowed` 0), "as specified in ETSI TS 102 366
clause H.2.2.3". The names v3.6 gives `evo_payload_config()` are Annex H's with `evo_` for `emdf_` and the flag fields renamed
`b_..._present`. So the standard confirms the claim that §15.4's container is the EMDF container, for E-AC-3. It does not confirm that
TrueHD carries it, in what bit layout around it, or that its configuration follows Table 56 (v3.6's syntax allows every configuration).

### B. The OAMD payload (§15.5 to §15.15) against TS 103 420 §5

| # | TH spec v3.6 | TS 103 420 | Verdict |
|---|---|---|---|
| B1 | `oamd_version` 2 bits, `+3` bits when 3 (0 to 10) | `oa_md_version_bits` 2, `+3` when 0x3 (§5.5.2); the semantics say only "the syntax version" | agree on the coding. The standard defines syntax for no version but 0 (the reader refuses others) |
| B2 | `object_count_code` 5 bits, `+7` bits when 31; count = code + 1 (1 to 159) | `object_count_bits` 5, `+7` when 0x1F; `object_count = bits + 1` (§5.5.2, §5.6.0.2) | agree. The standard caps nothing at 159; 159 is the most the coding can say. Its own cap is 16 objects for E-AC-3 (§8.3.2.2; §6.1: up to 16 essences). **Q34** |
| B3 | `b_dyn_object_only_program` 1 bit | the same (§5.5.3) | agree |
| B4 | the dynamic-only branch reads `content_description` (4 bits) and nothing else | the dynamic-only branch reads `b_lfe_present` (1 bit) and no `content_description` | disagree. **Q26** |
| B5 | the other branch reads `content_description` (4 bits, never used) and then every bed, ISF and dynamic field | `content_description[]` is four flags (3 bed, 2 ISF, 1 dynamic objects, 0 reserved), and each group of fields is present only if its flag is set (§5.5.3, Table 11a) | disagree: v3.6 reads the ISF index and the dynamic count even for a program without them. **Q26** |
| B6 | order: `b_multiple_bed_instance`; per instance `b_standard_bed`, the assignment, `b_lfe_only`, `b_chan_distribute` | order: `b_bed_chan_distribute` once, `b_multiple_bed_instances_present`, `num_bed_instances_bits` (3 bits, count = bits + 2) when set; then per instance `b_lfe_only`, and when it is clear `b_standard_chan_assign` and the assignment (absent when `b_lfe_only` is set) | disagree in order, in where the two flags sit and in the missing instance count. **Q26** |
| B7 | `b_multiple_bed_instance` SHALL be 0 | the standard allows 2 to 9 instances | standard silent on the constraint (a TrueHD matter); the syntax disagrees (B6). **Q26** |
| B8 | `bed_assign`, 10 bits; bit k of the mask selects a group: stereo, centre, LFE, side surround, rear surround, front height, top surround, rear height, wide, LFE2 | Table 12: ten flags L/R, C, LFE, Ls/Rs, Lb/Rb, Tfl/Tfr, Tsl/Tsr, Tbl/Tbr, Lw/Rw, LFE2, numbered 9 down to 0 and sent index 0 first, so v3.6's bit k is the standard's index 9 - k | agree on the groups, their order and counts, given that reading (the standard's text, taken literally, orders the labels from index 0 up; `libs/objects/src/oamd.cpp` records why the other reading is right) |
| B9 | the examples: 7.1.4 is `0x07F`, 7.1.2 `0x05F`, "9.1.6" `0x0FF` | -- | v3.6 inconsistent with itself: groups 0 to 7 are 14 channels (7.1.6); 9.1.6 needs the wide group too (`0x1FF`, 16 channels). The 7.1.4 example takes the top-side pair where E-AC-3's reference streams take the top-back pair. **Q34** |
| B10 | not standard: `custom_speaker_count` and one `speaker_label_idx` per channel, "variable" | `nonstd_bed_channel_assignment[]`: 17 flags, Table 13 (L, R, C, LFE, Ls, Rs, Lb, Rb, Tfl, Tfr, Tsl, Tsr, Tbl, Tbr, Lw, Rw, LFE2), no count, no indices | disagree; the 17 labels and their order (§15.11.3) agree. **Q27** |
| B11 | `isf_config` 3 bits, always present; 0 none, 1 to 6 give 4, 8, 10, 14, 15, 30 | `intermediate_spatial_format_idx` 3 bits, present only if `content_description[2]`; 0 to 5 give 4, 8, 10, 14, 15, 30 (Table 11b); 6 and 7 reserved | counts agree; the coding differs by one and by the condition. **Q26** |
| B12 | `num_dynamic_objects` 5 bits, `+7` when 31 | `num_dynamic_objects_bits` 5, `+7` when 0x1F, present only if `content_description[1]`; count = bits + 1 (§5.6.0.12) | extension agrees; v3.6 silent on "+ 1" and the condition. **Q26** |
| B13 | -- | `content_description[0]` set: `reserved_data_size_bits` (4), `reserved_data`, padding | v3.6 silent. **Q26** |
| B14 | `b_alternate_object_data_present` 1 bit | the same; when set, each element carries `alternate_object_data_id_idx` (§5.6.0.3) | agree on the bit; v3.6 silent on its effect. **Q28** |
| B15 | `oa_element_count` 4 bits, `+5` when 15 (0 to 46) | the same (§5.5.2) | agree |
| B16 | an element is a 4-bit id followed by its body | `oa_element_md()`: `oa_element_id_idx` (4), `oa_element_size_bits` = `variable_bits_max(4,4)` (size in bytes = bits + 1), `alternate_object_data_id_idx` (4, if flagged), `b_discard_unknown_element` (1), the element, padding to the size (§5.5.4, §5.6.4.3) | disagree: v3.6 omits the size, which is what lets a reader skip an element it does not know. **Q28** |
| B17 | ids: 1 object, 2 trim, 3 headphone, 4 object description (reserved, "not parsed"), 5 extended object, 6 bed object | Table 26: 1 object, 2 trim, 5 extended object; 0, 3, 4 and 6 to 15 are reserved | agree for 1, 2, 5; 3 and 6 are reserved in the standard, so it is silent on them. **Q28** |
| B18 | the object element loops block by block, then object by object | `object_element()`: `md_update_info()`, `b_reserved_data_not_present` (1) and, if clear, 5 reserved bits, then for each object `object_data()` = all its blocks (object-major) | disagree in order; v3.6 omits the reserved-data flag. **Q29** |
| B19 | `sample_offset_code` 2 bits: 0 is 0, 1 is a 2-bit table index, 2 is 5 bits, 3 reserved | Table 22: the same; Table 23 offsets 8, 16, 18, 24; `sample_offset_bits` 0 to 31 | agree |
| B20 | `num_obj_info_blocks_code` 3 bits, count = code + 1 (1 to 8) | the same (§5.6.2.4) | agree |
| B21 | `block_offset_factor` (6) and `ramp_duration_code` (2) for blocks 1 and up only | `block_update_info()` for every block including block 0 | disagree. **Q29** |
| B22 | `ramp_duration_code` 0 to 2 are the first table entries; 3 adds 2 more bits (code 0 to 6) | codes 0, 1, 2 are 0, 512, 1536 samples (Table 24); code 3 reads `b_use_ramp_duration_idx`, then a 4-bit index into Table 25 or an 11-bit `ramp_duration_bits` (0 to 2047) | disagree. The 16 table entries agree (32, 64, 128, 256, 320, 480, 1000, 1001, 1024, 1600, 1601, 1602, 1920, 2000, 2002, 2048). **Q29** |
| B23 | `object_info_block`: `b_object_not_active`; block 0 has `b_default_basic_info` and `b_default_render_info`; later blocks have `basic_info_status_idx` and `render_info_status_idx` ("variable") | `b_object_not_active`; `object_basic_info_status_idx` (2 bits; forced 0 when inactive, forced 1 in block 0, so no bit); basic info present for status 1 and 3; `object_render_info_status_idx` (2 bits; forced 0 when inactive or when the object is in a bed or is an ISF object; forced 1 in block 0); render info present for 1 and 3; `b_additional_table_data_exists` and its data (§5.5.9, Tables 28 and 29) | disagree: the default flags do not exist, the status indices are 2 bits, bed and ISF objects carry no render info, and the additional data is missing. **Q29** |
| B24 | `b_default_basic_info` set: gain 0 dB, no priority | status 0 ("default", an inactive object): gain -inf dB, priority 0 (Table 28) | disagree |
| B25 | `gain_code` 2 bits: 0 is 0 dB, 1 a 6-bit value giving -0.5 x (v + 1) dB (-0.5 to -32), 2 is -inf, 3 reserved | `object_gain_idx` 2 bits: 0 is 0 dB, 1 is -inf, 2 is followed by `object_gain_bits` (6) giving 15 - bits dB for 0 to 14 and 14 - bits dB for 15 to 63 (+15 to -49 dB in 1 dB steps), 3 is "the previous object's gain in this block" (Tables 18 and 19) | disagree: codes 1 and 2 swapped, a different gain law and range, code 3 not reserved. **Q30** |
| B26 | `b_priority_present`, then a 5-bit priority (0 to 31) | `b_default_object_priority` (set means 1.0), else `object_priority_bits` (5), priority = bits / 32 | disagree: the flag has the opposite sense and the scale is not given. **Q30** |
| B27 | `render_info_mask` 4 bits, bit 3 position, 2 zone, 1 size, 0 screen reference | `obj_render_info[]` 4 flags (all four implied by status 1): Table 31 numbers position 3, zone 2, size 1, screen 0; the §5.5.11 pseudocode numbers them 0 to 3 in the same syntax order. Sent index 0 first, Table 31 puts the screen flag first, the pseudocode the position flag first | standard inconsistent with itself, v3.6 follows the pseudocode; no stream decides (the E-AC-3 reference streams the repository reads send status 1, which implies all four). **Q30** |
| B28 | `b_absolute_position`: set means x, y, z follow, clear means three 3-bit signed differences; read in every block | `b_differential_position_specified`, read only in blocks after the first (block 0 is absolute): set means differences. Differences are signed, in 1/62, 1/62, 1/15 steps added to the previous standard-precision value | disagree: the flag has the opposite sense and block 0 has none. **Q30** |
| B29 | `x` 6 bits (/ 62), `y` 6 bits (/ 62), `z` 1 sign bit + 4 bits (/ 15); sign bit 0 is positive | the same widths and divisors, with `min(1, ...)`; `pos3D_Z_sign_bits` 0 means -1 (§5.6.1.1.10) | agree on widths and scale; **disagree on the sign**: v3.6 has it inverted. **Q30** |
| B30 | `b_distance_specified`, `b_at_infinity`, `distance_factor_idx` 4 bits | the same widths (§5.5.11); Table 15 gives the 16 factors (1.1 to 50.1) | agree; v3.6 silent on the table |
| B31 | `zone_constraints_idx` (3) and `b_enable_elevation` (1) follow the position group, and are read again under mask bit 2 | the pair belongs to the zone flag only (Table 31), with Tables 20 and 21 for the meaning | disagree: v3.6 puts them in two places. **Q30** |
| B32 | `size_code` 0: one 5-bit size; 1: width, depth, height 5 bits each; 2 and 3 reserved | `object_size_idx` 0: zero size, nothing follows; 1: one 5-bit size; 2: width, depth, height (5 bits each); 3 reserved; each value is bits / 31 (Table 17) | disagree (shifted by one). **Q30** |
| B33 | `b_use_screen_ref`, `screen_factor` 3 bits, `depth_factor` 2 bits; `b_object_snap` 1 bit | the same widths; `screen_factor = (bits + 1) / 8`, Table 16 depth factors 0.25, 0.5, 1, 2; `b_object_snap` ends every render info | agree on widths; v3.6 silent on the scale and the table |
| B34 | the trim element: `warp_mode` 2, 2 reserved, `global_trim_mode` 2, then 9 x `trim_value_idx` (4 bits) into a 16-entry level table | `warp_mode` 2, 2 reserved, `global_trim_mode` 2; only for mode 0b10 (custom) a loop over `NUM_TRIM_CONFIGS` of `b_default_trim`, `b_disable_trim`, a 5-flag `trim_balance_presence`, then centre, surround and height levels (4 bits each) and two balance values (sign + 4 bits); then `b_disable_trim_per_obj` and one flag per object (§5.5.12). `NUM_TRIM_CONFIGS` has no value in the standard | disagree. The 16 levels agree with Table 35 (`trim_centre`: +6, +3, +1.5, +0.75, -0.75, -1.5, -3, -4.5, -6, -7.5, -9, -10.5, -12, -13.5, -16, -36 dB) but surround and height use Tables 36 and 37, whose first four codes are reserved. **Q31** |
| B35 | the headphone element (id 3): "exact field layout to be confirmed from ETSI TS 103 420" | id 3 is reserved in Table 26: there is nothing to confirm it from | standard silent. **Q28** |
| B36 | the extended object element: per object `b_divergence_present` and a 2-bit index; then per block and object `b_ext_position` and three 2-bit codes | `b_obj_div_block` (1), then per object and block, for dynamic objects only: `b_object_divergence`, `object_div_mode` (2 bits: table, reuse the previous, 6-bit code), `object_div_table` (2) or `object_div_code` (6, Table 42); `b_ext_prec_pos_block` (1), then per object and block, dynamic objects only: `b_ext_prec_pos`, a 3-flag presence mask, and a 2-bit code per flag set (§5.5.13 to §5.5.15) | disagree: no block-level flags, no divergence mode or code, no presence mask, a different loop order and object scope. **Q32** |
| B37 | `OBJECT_DIV_TABLE`: 0.500755, 0.608529, 0.704833, 1.0; `EXT_PREC_POS3D_LUT`: +1, +2, -1, -2 | Table 41 and Tables 44 to 46: the same values | agree |
| B38 | the position becomes "base + ext_offset / (resolution of base field)" | `pos3D_X = min(1, bits/62 + ext/(62 x 5))`, likewise Y with 62 x 5 and Z with 15 x 5 | disagree: v3.6 leaves out the factor 5, so read literally it makes the refinement five times too large. **Q32** |
| B39 | the bed object element (id 6): "the same ObjectInfoBlock applied to each bed channel, position fixed to the speaker" | id 6 is reserved in Table 26; for a bed object the render info is not coded at all (status forced 0) | standard silent; v3.6 gives no bit syntax. **Q28** |
| B40 | §15.11.1: x/62, y/62, z/15 as normalised coordinates; §15.11.2: the room mapping X = 2(x - 0.5), Y = 2(0.5 - y), Z = z | §4.2.1: x from the left wall (0) to the right (1), y from the front wall (0) to the back (1), z from the floor (-1) to the ceiling (1); the front-wall centre is (0.5, 0, 0) | agree on the axes and their direction; the mapping to a signed cube is v3.6's own |
| B41 | §15.11.3: 17 speaker positions, e.g. L (0.134, 0.134, 0), LFE (0.134, 0.134, 0), Ls (0, 0.366, 0), Lb (0, 0.866, 0), Lw (0.067, 0.25, 0) | §4.2.3 gives speaker-anchored objects no numbers. The informative Annex B (Table B.10) has L (0, 0, 0), LFE (0, 0, -1), Ls (0, 0.5, 0), Lb (0, 1, 0), Lw (0, 0.161, 0), in the same normalised axes | disagree with Annex B for 14 of 17 labels (C, Lts and Rts agree). Not wire format. **Q39** |
| B42 | §15.16: SHALL: `object_count` within 159; `b_chan_distribute` and `b_lfe_only` not both set; `bed_assign` consistent across frames; sample offsets under the access unit's samples; timestamps increasing; active objects not muted (`gain_code != 2`); payload within the frame's capacity | none of these is in the standard. The standard's gain code for -inf is 1, not 2, and `b_bed_chan_distribute` is one flag for the whole program | standard silent; the `gain_code` and the two flag names do not match the standard's syntax (B6, B25). **Q36** |
| B43 | §15.6.3 / §15.16: `object_count` = beds + ISF + dynamic, and equals the channel count of substream 3 | §5.6.4.8: objects are ordered beds, ISF, dynamic; §8.3.2.2: for E-AC-3 the same total is `complexity_index_type_a`, at most 16 | agree on the order and the total; the link to substream 3 is TrueHD's (standard silent). **Q34** |

### C. Contradictions inside v3.6

| # | Where | What |
|---|---|---|
| C1 | §15.4 against §16.3 | the same bytes (`evolution_payload`) are a bit-level container in §15.4 and a Thrift container in §16.3; §16.3.1 calls the payload "a single Thrift blob". Not related anywhere in v3.6; the owner's answer nests them (above). **Q22** |
| C2 | Appendix E and §15.16 against §15.6 | Appendix E still uses the v3.5 names: `multiple_bed_instances_present` (now `b_multiple_bed_instance`), `bed_object_chan_distr` (no such field now; the nearest is `b_chan_distribute`), and "`interm_spatial_format` SHALL be NULL", which forbids what §15.6 describes in detail (ISF objects through `isf_config`, 4 to 30 of them). Whether a TrueHD Atmos program may carry ISF objects is therefore open. **Q36** |
| C3 | §15.2 against §2.3, §7.2, §15.3, §17.5 | §15.2: Atmos is 48 kHz only. §7.2 and §17.5: restart sync 0x31EC marks substreams 2 and 3; §15.3: substream 3; §2.3: all substreams share one 13-bit sync (0x18F5). No rate condition appears in §7.2, §15.3, §8.3 or §9.3. The library follows the sync and does not test the rate. **Q35** |
| C4 | §8.3, §9.3 against §15.3 | `huff_lsbs` up to 31 and a Filter B range of 2^31 are for "substreams 2/3 in Atmos"; §15.3 lists them for substream 3. The library keys on the 0x31EC sync (any substream carrying it): 31 and a 32-bit filter there, 24 and 24 bits elsewhere. **Q35** |
| C5 | §15.17 against §14.2 | "Max Channel Count" 12, 14, 16 per Atmos presentation; §14.2's `sixteench_channel_count` is 5 bits and its use is not stated. Is it 12, 14 or 16, the substream 3 channel count, or `object_count`? §15.17 does not say whether a program with fewer objects is padded to 12, 14 or 16 channels. **Q34** |
| C6 | §15.6.1 | the example "9.1.6" is a 14 channel mask (B9). §15.6.2 lists a 30 object ISF layout, which cannot fit a 16 channel substream. **Q34** |
| C7 | §15.4 against §16.6 | `evo_protection()` is called an HMAC; §16.6 authenticates the frame with a Thrift ContentSignature (ECDSA). Whether a frame carries both is not said. **Q37** |
| C8 | §16.1 | `if (evolution_frame_present)` is a condition no field defines. The library treats any extra data region that passes its framing checks as a candidate evolution frame and reports the checks. **Q38** |

### D. Questions for the owner

Numbering continues the list at the top of this file. In each, "the standard" is TS 103 420 §5 and "recommendation" is what the library does
until told otherwise. The library's evolution frame reader is **provisional, owner to confirm** (question 22): it is written and tested, but
no TrueHD stream here has an evolution frame, so nothing decides it.

22. **How the two layouts nest (A19, C1).** The owner's answer: the §15.4 container is the outer one and the Thrift `ContentSignature` is inside one
    of its payloads. Still open, and not to be guessed: which payload id carries the Thrift blob (one of the ids other than 11, presumably);
    whether the blob starts at the first byte of that payload or after a header; what the signature's digest covers (§16.3.3, §16.6.1: the
    other payloads, the whole frame, or something else); what `evo_protection()` covers (the owner's text says an HMAC over the OAMD and JOC
    structures, §16.6 says ECDSA); and whether each frame carries a signature or one frame in many does (§16.3.3 point 3). The library reads
    the outer container (`parse_evolution_frame`, `libs/truehd/include/iclforge/truehd/io/evolution.hpp`), carries every payload as bytes with
    its id and size, and does not look inside a payload that is not OAMD. **Recommendation:** `forge probe DISC detail=evolution` on a disc's
    TrueHD stream settles the first three structural points and the last from the stream (section H lists the fields); the digest scope and what
    the protection covers need the signing material or the owner's source and are not visible in a probe, which prints no value.
23. **How `variable_bits(n, max_ext)` is coded (A12).** v3.6 §15.4 describes an escape value (all ones, add, read n more). The standard
    (§5.5.1), ETSI TS 102 366 Annex H and TS 103 190-1 all use a read-more flag after every group. The two disagree from the first
    `variable_bits` of the frame on. `max_ext` matches `max_num_groups` of §5.5.1, which suggests the source code uses the standard's coding and
    the prose is a paraphrase. **Recommendation:** the standard's coding (`EvolutionReading::kEmdf`). The reader also has the literal v3.6
    reading (`kSpec36`), and `forge probe` reports which of the two parses a stream, so a real stream can settle it.
24. **The EMDF details §15.4 omits or changes (A2, A5, A7, A8, A9, A15).** Version other than 0; the payload id escape at 31; `smploffst` as 11
    bits plus a reserved bit against `variable_bits(11, 2)`; the 8 bits after the codec data flag, which the standard calls reserved; and a
    primary protection length code of 0, which Annex H reserves and v3.6 allows. **Recommendation:** the standard's reading for each, as
    `kEmdf`; `kSpec36` differs in exactly these and in question 23's coding.
25. **The payload configuration a TrueHD writer sends (A6, A8, A9).** Table 56 fixes it for E-AC-3: one OAMD payload per frame, frame
    aligned, no sample offset or duration, `groupid` shared with JOC (there is none in TrueHD), priority 0. Table 56 also says
    `codecdatae` 1 where Annex H says 0. **Recommendation:** write what `libs/objects` writes for E-AC-3 (Table 56 with `codecdatae` 0,
    `groupid` 0) and read anything; nothing is written yet (planning/truehd-atmos.md).
26. **The program assignment (B4 to B7, B11 to B13).** The standard's syntax (§5.5.3): `b_lfe_present` in the dynamic-only branch;
    `content_description` as four flags that gate the bed, ISF, dynamic-object and reserved-data groups; `b_bed_chan_distribute` first,
    then the instance count (bits + 2), then `b_lfe_only` before the assignment; the ISF index and the dynamic object count only when
    flagged, the count being bits + 1. §15.6 differs in all of these. The "SHALL be 0" on multiple bed instances is a TrueHD rule the
    standard cannot confirm. **Recommendation:** the standard's syntax (it is what `iclforge::objects` parses); §15.6 read as a summary.
    Is it?
27. **Non-standard beds (B10).** `custom_speaker_count` and `speaker_label_idx` are "variable" in §15.6; the standard has 17 flags (Table 13)
    and no count. **Recommendation:** Table 13.
28. **Element framing and the reserved ids (B14, B16, B17, B35, B39).** The standard frames each element with its size in bytes, the
    alternate-data id, a discard flag and padding, so that an unknown element can be skipped. §15.7 has none of that. Ids 3 (headphone) and 6
    (bed object) are reserved in Table 26; §15.13 asks for the headphone layout to be confirmed from a document that reserves the id, and §15.15
    gives no bit syntax. **Recommendation:** the standard's framing; ids 3, 4, 6 and above skipped by size. If real streams carry a headphone
    or bed object element, the owner supplies the layout.
29. **Timing and object info blocks (B18, B21 to B24).** Block 0 has a `block_update_info` too; ramp codes 0, 1, 2 mean 0, 512, 1536 samples;
    code 3 reads a flag and then a 4-bit table index or an 11-bit value; the info status indices are 2 bits (§15.8.2's "variable" is 2 bits)
    and forced to 0 for inactive objects and for bed and ISF objects' render info, to 1 in block 0; objects are coded object by object, not
    block by block; `b_reserved_data_not_present` and the additional table data exist. **Recommendation:** the standard.
30. **Basic and render info (B25 to B29, B31, B32).** Gain: code 1 is -inf, code 2 carries the 6 bits (1 dB steps, +15 to -49 dB), code 3
    repeats the previous object's. Priority: `b_default_object_priority`, scale bits / 32. Position: the differential flag is read only
    after block 0 and means differences when set; the z sign bit 0 is negative. Zone and elevation belong to the zone flag only. Size code 0 is
    zero size. The presence-flag order of `obj_render_info[]` is ambiguous inside the standard (B27) and only matters for status 3.
    **Recommendation:** the standard; for the flag order, Table 31's numbering as `iclforge::objects` reads it, until a stream with status 3 shows
    otherwise.
31. **The trim element (B34).** The standard's loop is conditional on the global mode and its length `NUM_TRIM_CONFIGS` is not given
    (`iclforge::objects` finds 6 or 9 from the element's own size). §15.12's nine fixed 4-bit levels do not exist in it. What is the count in
    TrueHD, and are the surround and height levels Tables 36 and 37? **Recommendation:** the standard's syntax, the size-driven count.
32. **The extended object element (B36, B38).** Block-level flags, a divergence mode and code (Table 42), a 3-flag presence mask for the
    position refinement, dynamic objects only, and a refinement of 1/5 of the base step. **Recommendation:** the standard.
33. **The time base (A6, B42).** The standard's OAMD payload covers one E-AC-3 frame (1536 samples): up to 8 updates, each at
    `sample_offset + 32 x block_offset_factor` samples, ramps to 2048. A TrueHD access unit is 40 samples (§3.3). §15.16 says sample offsets
    must be below the access unit's duration, which fits `sample_offset` (at most 31) but not a block offset factor up to 63. How many access
    units does one evolution frame cover, how often is one sent, and what do `sample_offset`, `block_offset_factor` and the ramp refer to?
    **Recommendation:** the owner states it; the probe reports the access unit index of every frame so the cadence can be read off.
34. **Channel accounting (B2, B9, B43, C5, C6).** `object_count` up to 159 against a substream of at most 16 channels; a 30-object ISF layout
    cannot fit; 12, 14 and 16 (§15.17) against `sixteench_channel_count` (§14.2) and `object_count`; the LFE and the bed channel order in the
    substream; the "9.1.6" mask that is 14 channels. **Recommendation:** treat 16 as the limit of `object_count` for TrueHD, state the channel
    order as the standard's (beds in Table 12's order with L/R first, then ISF, then dynamic), and say whether a 7.1.4 program with fewer
    objects is padded to 12 channels.
35. **Sample rate, sync and limits (C3, C4; G below).** Is Atmos 48 kHz only, and is the rule for the encoder or for the decoder? Does
    0x31EC mark substream 3 only (§15.3) or 2 and 3 (§7.2, §17.5)? Do the 31-bit `huff_lsbs` and the 2^31 filter range apply to every
    substream with sync 0x31EC or by index? What width is an Atmos channel's output (the decoder keeps the low 24 bits of a value, as
    the lossless check's three bytes do)? **Recommendation:** the decoder keys the `huff_lsbs` limit on the sync, accepts any rate and
    refuses nothing the sync does not allow; the encoder writes 48 kHz only. Section G says what the decoder does and the tests that hold it.
36. **Appendix E and §15.16 (B42, C2).** Appendix E uses the v3.5 field names and says ISF SHALL be NULL, which §15.6 contradicts.
    Can an Atmos program carry ISF objects? **Recommendation:** treat ISF as allowed (the standard defines it), and the rest of §15.16 as
    rules for an encoder that this library's encoder will keep (nothing is enforced on the read side).
37. **Integrity (A3, A16, C7).** What is `key_id`, what does `evo_protection()` hold, and does a frame carry both it and a `ContentSignature`? The
    owner's answer says an HMAC-SHA-256 with a secret key signs the OAMD and JOC structures; §16.6 says ECDSA P-256 over the Thrift
    `SignedInfoWire`. The two agree if the HMAC is the protection bits (a 4 or 16 byte field is shorter than an HMAC-SHA-256, so it would be a
    truncation, which is a further question) and the ECDSA is the nested signature. A secret key cannot be checked here in any case. Out of
    scope for now: the library reports the protection lengths and bytes and verifies nothing, and the probe says so and prints no byte of either.
38. **`evolution_frame_present` (C8).** §16.1 conditions the frame on a flag no field defines. **Recommendation:** any extra data region is
    a candidate; the nibble, the framing and the parity are reported, not enforced.
39. **Speaker positions (B41).** §15.11.3's positions differ from the standard's informative Annex B for 14 of 17 labels. They are not
    wire format; they matter to a renderer. **Recommendation:** do not use §15.11.3 until the owner says where its numbers come from; the
    renderer uses `libs/render`'s own.

### E. What the public standard confirms, and what rests on the owner's word

Confirmed by TS 103 420: the widths and extension rules of the version, object count and element count (B1, B2, B15); `b_dyn_object_only_program`
and `b_alternate_object_data_present` (B3, B14); the sample offset, ramp duration, divergence, extended position and trim level tables
(B19, B22's 16 entries, B37, B34's centre levels); the ISF object counts (B11); the bed groups and the 17 channel labels (B8, B10);
the widths of the distance, screen and snap fields and of the position and the z magnitude (B29, B30, B33); the room axes (B40); OAMD as
payload id 11 (A6); the shape of the EMDF payload configuration, size and protection (A4, A10, A11, A13, A14, in the container reader's
terms).

Rests on the owner's word alone, because the standard is silent on TrueHD: that the payload travels in the extra data and as
`evo_frame()` (A18, A19); its protection being an HMAC (A16); everything about substream 3 (restart sync 0x31EC, 15 matrices, 32-bit Filter B,
`huff_lsbs` 31, terminator B, the 12, 14 and 16 channel presentations); the program rules that tie `object_count` to the substream's
channels; the constraints of §15.16; the extra data framing, its check nibble and parity (§16.1, §16.2, §16.4).

Where the owner's text and the public text differ, nothing but a stream or the owner decides: A5, A7, A12, A15 (the container) and B4 to B6, B10 to
B12, B16, B18, B21 to B26, B28, B29 (the z sign), B31, B32, B34, B36, B38 and B41 (the payload). The library follows the standard in all of them
(`iclforge::objects` already does) and says so in the question.

### F. The reader (provisional)

`libs/truehd/include/iclforge/truehd/io/evolution.hpp` reads what questions 22 to 24 leave open, in the form the owner's text and the
standard agree on, so that a real stream can say which is right. **Provisional, owner to confirm**: no stream here has an evolution
frame. The owner's reading (2026-10-29, above) is that §15.4's container is the outer one with the Thrift ContentSignature nested in one of its
payloads, which this reader follows: it reads the outer container and carries every payload as bytes with its id and size.

- `read_extra_data(bytes)` is `parse_access_unit`'s own extra data reading, for bytes held apart from their unit. `read_evolution(extra)`
  reports the check nibble (§16.2), the framing, the parity (§16.4), `evo_reserved` zero (§16.8) and the capacity rule (§16.8, with
  `extra_data_length` as the words less one, question 19), none enforced, and reads `evolution_payload` as `evo_frame()`.
- `parse_evolution_frame(bytes, reading)` returns the version, the key id, each payload (id, configuration, bytes), the protection field
  (the two length codes and the bytes), the length in bits and whether the bits after it are all zero, or an `EvolutionError` of one of ten
  kinds, each with the bit offset of the field that failed. `EvolutionFrame::oamd()` is the payload with id 11. The protection bytes are
  returned, not verified; no signature is looked at.
- Two readings. `kEmdf` (the default) is the EMDF container as `iclforge::objects` reads it for E-AC-3; `kSpec36` is §15.4 to the letter
  (rows A2, A5, A7, A12, A15). `forge probe` reads every frame both ways and says which read.
- The library links `iclforge::base` alone. The OAMD payload is parsed by a program that links `iclforge::objects` too, through
  `iclforge::objects::oba::parse_payload(bytes)`, which takes a bare payload: **no change to libs/ac3 or libs/objects is needed**. (The
  EMDF reader there, `parse_container`, wants the sync word and a length in front of the body; the tests give it those four bytes, and
  `forge` has no need of it.)
- Tests: `libs/truehd/tests/io/test_evolution.cpp` reads the vectors `tools/references/truehd_evolution.py` writes
  (`testdata/truehd/evolution/vectors.json`: 133 hand-built extra data regions, each read under both readings by the Python transcription,
  with a vector for each of the ten error kinds, every version 0 to 3, key ids 0 to 7 and beyond, every configuration flag, payload sizes at
  the edges of the size coding, every combination of protection codes, padding, a bad nibble, a bad parity and truncations), a frame
  written out bit by bit, `iclforge::objects::emdf::build_container`'s output (whose body is an `evo_frame()` under `kEmdf`), and 6,000
  random frames of random shape, whole and damaged, and 40,000 random byte strings, each compared with `parse_container`: the same payload
  ids, sizes, bytes and configuration, or both refuse. The two differ only where Annex H defines nothing (a version other than 0, which
  `parse_container` refuses and this reads) and that is asserted, not allowed silently. The last test takes an OAMD payload from
  `iclforge::objects`, frames it with the encoder, decodes the stream, reads the extra data back and parses payload 11.
- Each check was removed in turn (22 mutations: the primary protection code refusal, the payload id escape, the reserved bit after
  `smploffst`, the read-more flag, the terminator and payload size checks, the trailing-bits scan, the parity, nibble, reserved and capacity
  reports, the escape cap of `kSpec36`, the protection order, the discard flag, the version and key id escapes, the first-payload rule,
  the widths of `proc_allowed`, the group id and the codec data, the error kind of a missing terminator and the two-byte minimum of
  `read_extra_data`) and `ctest -L truehd` failed each time.
- `forge probe` (`apps/forge/cli/src/commands/truehd_evolution.cpp`) is the check to run on a real Atmos track. For a stream whose units
  carry extra data it prints, and writes under `truehd.evolution` with `json=1`: the units that carry it and the cadence (first and last unit,
  the smallest and largest gap), how many pass the check nibble, the framing, the parity, `evo_reserved` and the capacity rule, how many
  read as a frame under each reading and how many of those are clean (a payload, and only zero bits after the protection field), the
  payload ids, the protection length codes, and for payload id 11 what `iclforge::objects` reads from it: the object count, the bed
  channels, the dynamic objects and the update blocks. It says that the protection bytes and any signature are not verified, and prints
  the first four bytes of the first frame. No audio and no payload byte
  beyond those four. The tests (`apps/forge/cli/tests/test_cli_truehd.cpp`) run it on a stream the encoder makes with an OAMD payload in
  every other unit, on one with a wrong check nibble and a wrong parity byte, on a frame written in the letter of §15.4 (only that reading
  reads it cleanly), on the hand-built stream whose extra data is opaque, and on a stream with no extra data; 13 mutations of the survey
  were tried and 11 fail the tests (the two that do not change what is chosen only when both readings read the same bytes cleanly). The
  JSON of a stream inside Matroska, MP4 or a transport stream is still invalid, as it was before this change (the probe writes the `container`
  key twice, a defect being fixed on another branch and not touched here): with that key repaired the `evolution` object is valid in all three.
- What this does not show: that any TrueHD stream looks like this. The vectors, the Python transcription and the C++ reader share one
  author's reading of §15.4; the E-AC-3 reader does not, and is why `kEmdf` is the default.

### G. Substream 3 against the decoder (TH spec v3.6 §7.2, §8.3, §9.3, §10.3, §15.3)

What a stream whose substream 3 uses restart sync 0x31EC, a 32-bit Filter B range, up to 15 matrices and `huff_lsbs` up to 31 does in
this decoder, and what holds it. The library and v3.6 agree on every point below, because the decoder is more permissive than v3.6
wherever the two differ; nothing needed fixing. (The brief named `hb_eight_channels`; the tree has no such stream. The eight channel case
is entry 32: three substreams, the third with sync 0x31EB, made by the encoder and checked in FFmpeg's strict decode.)

| v3.6 | The decoder | Held by |
|---|---|---|
| restart sync 0x31EC marks the object substreams (§7.2: 2 and 3; §15.3: 3) | read in any substream: the three syncs 0x31EA, 0x31EB and 0x31EC are accepted everywhere and only the sync's own rules differ; an MLP stream is not held to 0x31EA | `hb_sixteen_channels`, `hb_substream3_limits` (0x31EC in substreams 2 and 3), `test_decoder.cpp` (0x31EC in substream 0) |
| `min_chan`, `max_chan`, `max_matrix_chan` at most 15 in substream 3 (§15.3); 7 for TrueHD and 5 for MLP otherwise (§10.3) | by substream index: 15 in substream 3, 7 or 5 in the others, `kBadParameter` above | `test_substream3.cpp` (8 refused in substream 0 under 0x31EA and 0x31EC, 7 accepted), `hb_substream3_limits` (15 accepted in substream 3) |
| up to 15 primitive matrices in substream 3 (§10.3: 2, 6, 8 and 15 by substream, 6 or 8 globally) | the 4-bit count, up to 15, in every substream; the smaller limits are not enforced (a superset of v3.6) | `hb_substream3_limits` (15 matrices in one block header, in a 16-channel substream; FFmpeg's decode refuses 0x31EC, so none outside this repository) |
| `huff_lsbs` at most 24, 31 for the object substreams (§8.3) | at most 24, or 31 when the restart sync is 0x31EC, whatever the substream index; above that `kBadParameter` (the message now says which limit) | `test_substream3.cpp` (31 accepted under 0x31EC and decoded, 25 refused under 0x31EA and 0x31EB, 24 accepted under all three), `hb_substream3_limits` (residuals that need `huff_lsbs` 29 to 31) |
| Filter B (and A) output ±2^23 for substreams 0 and 1, ±2^31 for 2 and 3 (§9.3) | 32-bit samples and 64-bit sums in every substream; no 24-bit limit is applied anywhere (entry 41), so the 32-bit range is read as it is sent | `hb_substream3_limits`: a prediction filter of gain about -6 and an IIR filter whose residual reaches 2^28 or more, and coded channels of 25 bits before 15 matrices, decode to the 24-bit signal they were built from (the Python reference decodes it too) |
| terminatorB (0x1234) ends substream 3 (§15.3, §16.5.2) | read after the last block of substream 3 | `hb_sixteen_channels`, `hb_substream3_limits` |
| Atmos is 48 kHz only (§15.2) | not tested; any rate is decoded | -- (question 35) |

The stream `hb_substream3_limits` is made by `tools/references/truehd_writer.py`: four substreams, the last of 16 channels with sync 0x31EC,
`max_matrix_chan` 15 and 15 primitive matrices on channels 8 to 15, two channels with a FIR filter of gain -5.9 and two with an IIR filter whose
residual reaches 2^28, `huff_lsbs` 29 to 31, terminatorB. `python3 tools/checks/verify_truehd_interop.py --handbuilt --forge forge --reference`
holds `forge decode` and the Python reference to its signal (29 hand-built streams, 0 failing) and FFmpeg 6.1.1 to refusing it, as the manifest says.
Only PCM of 24 bits is given back: a value of an object channel that needs more would lose its top bits in the output (and in the lossless check,
which folds the low three bytes), and v3.6 says nothing about the width (question 35).

### H. What the probe prints, and which question each field answers

For the owner, on a disc's TrueHD stream, on the owner's machine. Metadata only: nothing derived from the audio, no payload byte, no digest,
certificate, signature or protection byte, and no value of any Thrift field is printed. The file's path is (the `file` member). The command, for
a bare `.thd` or a `.mkv`, `.mp4` or `.ts` / `.m2ts` that holds the track:

    forge probe DISC_STREAM detail=format json=1 > probe.json
    forge probe DISC_STREAM detail=format > probe.txt

(`detail=evolution` is the first part alone and does not decode; `detail=structure` is the second part and decodes the whole stream, lenient, and
discards the samples.) Paste back `probe.txt`, or `probe.json` if the text is not enough. If the track is in a Blu-ray `.m2ts` that forge
does not read, extract the TrueHD stream with any tool first and probe that.

| Question | Fields (JSON path under `truehd`) | What they show |
|---|---|---|
| 2: the extension of the channel meaning (§14.2) | `structure.major_syncs[]`: `size_bytes`, `extra_channel_meaning`, `extra_channel_meaning_length`, `sixteench_channel_count`, `dyn_object_only`, `content_description`, `channel_assignment`, `isf`, `dynamic_object_count`, `substream_info`, `extended_substream_info` | the size the major sync's CRC confirmed for the extension (28 bytes without one) against the length field: bits, bytes or words |
| 51 to 55: the channel meaning (§12.3, §12.5, §14.1) | `channel_meaning`: `presentations.2ch|6ch|8ch` (`in_stream`, `control_enabled`, `dialogue_norm`, `dialogue_norm_dbfs`, `mix_level`, `source_format`), `heavy_drc_start_up_gain`, `drc_start_up_gain`, `distinct_in_stream` | the fields as the disc's encoder wrote them, per presentation: whether the dialogue norm is ever above 31 for 2 channels, whether a stream with a non-zero dialogue norm sets its control bit, what the mix level and source format take for a known programme, and whether the major syncs of one stream differ |
| 3: terminators (§3.5, §16.5) | `structure.decode.segment_tails[]` per substream: `zero_bits_only`, `terminator_a_at_once` (0xD234E or 0x348D3 right after the last block), `terminator_b_at_once` (0x1234, substream 3), `end_marker_at_the_end` (terminatorA and the 12-bit count ending the segment), `other`, `bits_after_last_block_min/max` | whether every segment of substreams 0 to 2 ends with terminatorA, whether substream 3 ends with terminatorB, and whether the count follows. A stream with terminators in every segment fails a strict decode; this pass is lenient and counts them |
| 10: heavy DRC (§7.1, §12.2) | `structure.restart_headers[]`: `tail_16_bits` (value and count), `tail_distinct_values`, `failures` (a restart header whose CRC-8 fails), `max_shift`, `max_lsbs`, `max_bits`, `max_bits_repeat`, `data_check_present` | a non-zero tail is where heavy DRC's gain and time would be; a header whose CRC fails would mean the header is longer than the library reads. `structure.decode.lossless_check_failures` and `check_failures` say whether the stream still decodes |
| 11: where a bypassed LSB comes from (§10.2) | `structure.decode.matrices.matrices_with_lsb_bypass`, `block_headers_with_matrices`, with `structure.decode.lossless_checks_ok` and `lossless_check_failures` | if matrices bypass LSBs and every restart interval's lossless check passes, the library's reading (one bit a sample, from the sample data) is right for that stream; failures say it is not |
| 5, 6, 18, 19 (marked correct by the owner; provisional until scored) | `structure.decode.lossless_checks_ok` / `lossless_check_failures` over a whole disc stream; `evolution.detail.frames[].extra_data_length`, `evo_frame_byte_length`, `length_fits` | a stream that decodes with no lossless failure supports the readings of noise, the channel shift and the filter history; the extra data lengths against the frame length show whether `extra_data_length` is the words less one |
| 20, 35: how the layouts nest, signing | `evolution.detail.shapes[]`: `payload_ids`, `frames`, `first_unit`, `last_unit`, `payload_bytes` (min and max per payload), `primary_protection_code`, `secondary_protection_code`, `thrift_walks[]` (`payload_id`, `walk.offset`, `walk.consumed`, `walk.fields[]` with `id`, `type`, `element`, `count`) | which payload id holds a Thrift struct and at what offset; its fields by id, type and length; whether a shape with that payload is on every frame or on one frame in many (a signature per frame or per stream); the protection field's size |
| 21, 22, 23: the coding of the container | `evolution.readings.emdf` and `.spec36` (`frames`, `clean_frames`, `errors`), `evolution.detail.frames[]`: `reading`, `emdf_error`, `spec36_error`, `version`, `key_id`, `payloads[].config`, `bits_used`, `trailing_zero` | which reading reads every frame cleanly (a payload, and only zero bits after the protection), the configuration flags the payloads carry, the version and key id |
| 24 to 30, 32: the object metadata | `evolution.oamd.programs[]`: `object_count`, `dynamic_only`, `lfe`, `bed_assignment`, `bed_channels`, `isf_index`, `dynamic_objects`, `update_blocks`; `evolution.oamd.unread` | whether `iclforge::objects` reads the payload (and so whether it follows TS 103 420's syntax), the bed and the object count against the channels of substream 3 (`structure.restart_headers[3]`, `max_chan`) |
| 31: the time base | `evolution.cadence` (`first_unit`, `last_unit`, `min_gap_units`, `max_gap_units`), `evolution.detail.shapes[]` | how often a frame comes, in access units of 40 samples |
| 33: rate, sync, limits | `structure.major_syncs[].sample_rate`, `structure.restart_headers[]` (`syncs` per substream, `max_matrix_chan`, `max_lsbs`), `structure.decode.matrices` | the rate, the sync of each substream, the largest `huff_lsbs` and matrix channel the stream uses |

The tests of these fields are in `apps/forge/cli/tests/test_cli_truehd.cpp` (a stream with a Thrift payload in one frame and OAMD alone in the others, whose output is
checked for not containing any of its values; the FFmpeg fixtures; the hand-built 16-channel and LSB bypass streams) and in
`libs/truehd/tests/decoder/test_substream3.cpp` (the decoder's counters).
