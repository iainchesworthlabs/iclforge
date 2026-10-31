# TrueHD and MLP: what the fixtures and tests cover

As of 2026-10-29. This is the inventory behind the fixture work of phase T2c: for each feature of the
bitstream and of its carriage, which fixtures and checks exercise it, and for each empty cell what could
fill it. "Writer" is `tools/references/truehd_writer.py`, "encoder" is `iclforge::truehd::Encoder`, and
"FFmpeg" is the installed `ffmpeg` used as a program (its encoders write streams, its decoder reads
them; its source is not read). "Dolby only" means a stream only Dolby's tools make (the DEE gold set
of `tools/generators/gen_dee_gold.py`, which stays on the owner's machine until DEE's licence ends on
2026-11-06). The streams are in `testdata/truehd/` (25 made by FFmpeg, 14 by the writer, one
joined from two) and `tools/checks/check_truehd_coverage.py` fails when a row marked "required"
stops being carried by any of them; `--list` prints the streams behind each row.

## The bitstream

| Feature | Carried by | Empty cells, and what fills them |
|---|---|---|
| Sample rates 44.1, 48, 88.2, 96, 176.4, 192 kHz | FFmpeg fixtures, one each (88.2 and 176.4 added); the encoder's round trips at all six | none |
| Bit depth 16, 20, 24 | 16 and 24: FFmpeg fixtures. 20: `truehd_st48_s20` (added; the restart header's `max_shift` is 4). Depth is not a field of a TrueHD stream: it shows as the output shift | none |
| Channels 1, 2, 3, 4, 5, 6 | FFmpeg: mono, stereo, 6 (original), 3, 4, 5 in two assignments each (added) | none |
| 7 and 8 channels | `hb_eight_channels` (writer, added; FFmpeg decodes it in its own channel order, `columns` in the manifest); the encoder's 7.1 round trips (`test_encoder.cpp`, `verify_truehd_encode_interop.py`) | no FFmpeg-encoder stream (it writes 6 at most) |
| 16 channels (Atmos bed and objects) | `hb_sixteen_channels` (writer; FFmpeg refuses it) | no outside reader: Dolby only |
| TrueHD and MLP (`0xF8726FBA`, `FBB`) | 8 and 6 FFmpeg fixtures; the writer can make MLP but no committed stream does | an MLP stream with an IIR filter or matrices: writer |
| One, two, three, four substreams | 1 and 2: FFmpeg; 3: `hb_eight_channels`; 4: `hb_sixteen_channels` | none |
| Restart interval | every 16 units (original), 8 and 128 (added: FFmpeg's `-max_interval`, the least and most it writes) | a gap under 8 (the encoder writes one, FFmpeg's decoder reads it; not committed) |
| FIR filters, every order | orders 1 to 8: the 6 channel FFmpeg streams | none |
| IIR filters, every order | `hb_filters` (order 2), `hb_filter_orders` (1, 3, 4; FIR 4 with IIR 4, FIR 5 with IIR 3; added) | IIR `new_states` (question 8): the writer does not write it; it could, FFmpeg's decode would then be the oracle |
| Huffman codebooks 1, 2, 3 and raw residuals | FFmpeg streams carry all four | none |
| Matrices | one (stereo, FFmpeg), three with a permuted `ch_assign` and an output shift (`hb_matrices`) | matrices with `frac_bits` other than 8 and 14: writer |
| LSB bypass | 12 FFmpeg streams set the flag (every 24 bit stereo stream), `hb_lsb_bypass` | none (ERRATA question 11 says no stream sets it: the census says otherwise) |
| Noise shaping and generators | generator 0 and `noise_type` 1 with shifts of zero: `hb_noise_generator_0`, `hb_noise_type1` | generator 1 with a non-zero shift: needs the table (question 5), Dolby only |
| Lossless check (shift by matrix channel) | `hb_matrices`; every FFmpeg fixture's restart headers | none |
| End-of-stream marker and padding | `hb_marker_last`, `hb_marker_every`, the 44.1 kHz FFmpeg streams | none |
| Evolution frames, extra data | `hb_extra_data`, carried through opaque (`test_handbuilt.cpp`) | the payload on the wire (question 1): Dolby only |
| DRC word, block data check | `hb_drc_word`, `hb_data_check` | the CRC byte's algorithm (question 9): Dolby only |
| Parity and CRC fields | every stream; flipped bits in `test_decoder.cpp`, `test_decoder_checks.cpp` | none |
| Damaged and truncated streams | in-memory damage to FFmpeg fixtures (`test_ffmpeg_fixtures.cpp`, `test_elementary*.cpp`); `fuzz_truehd_*` | none |
| Concatenated streams | `truehd_cat_st48_s24` (added): FFmpeg decodes through the join, the library's strict decode stops there | see question 20 below |

## The carriage

| Carriage | Carried by | Empty cells |
|---|---|---|
| MP4 `mlpa`/`dmlp` | `test_mp4_truehd.cpp`, FFmpeg both directions (`verify_truehd_containers.py`) | no document (containers ERRATA C1 to C6) |
| Matroska `A_TRUEHD` | `test_matroska_truehd.cpp`, FFmpeg both directions | none |
| MPEG-TS type `0x83` | `test_mpegts_truehd.cpp`, FFmpeg both directions; BDAV 192-byte packets: the self-test of `verify_truehd_local_streams.py` (added) | a disc's own layout, with an AC-3 core in the stream: only a disc shows it (C9) |
| MAT / IEC 61937 | `test_mat.cpp`; MAT equals FFmpeg's `spdif` output | FFmpeg does not read MAT; no receiver |
| Raw `.thd` / `.mlp` | every fixture | a raw file saved with its AC-3 core (C9): the stand-ins only |

## Beside E-AC-3

E-AC-3 has `testdata/external-baseline/` (seven E-AC-3 streams from DEE and FFmpeg with their scores, the
transient pre-noise stream), the FFmpeg-made stream `testdata/audio/reference_51_eac3_448k_cplbndstrce0.ec3`,
`testdata/object-fixture/dee_joc_*.ec3` (JOC), 99 platform and mode hashes of the encoder's output
(`testdata/bitstream-hashes.json`), decode PCM hashes, table goldens, eight FATE excerpts fetched by hash
(`verify_fate_interop.py`) and 1,086 local DEE streams. A TrueHD counterpart for each:

| E-AC-3 | TrueHD today | Counterpart that is missing |
|---|---|---|
| Committed DEE streams with scores | none; the DEE TrueHD legs stay local | a few small DEE TrueHD streams with their sources, if the owner allows them in the tree |
| Encoder output hashes per platform and mode | none | one SHA-256 per quick-matrix input of `verify_truehd_encode_interop.py`; the stream should not depend on the platform, and the hash would show whether it does |
| FATE excerpts by hash | none | FFmpeg's FATE archive has TrueHD and MLP samples; `verify_fate_interop.py` could take them (not fetched here) |
| Local gold set | `gen_dee_gold.py` `truehd` group, scored by `score_truehd_decode.py` | the owner's discs (`verify_truehd_local_streams.py`) are a second set |

TrueHD is already stronger where the codec is lossless. A decode is the source sample for sample, so there
is no SNR floor to argue over: FFmpeg's strict decode of 43 encoder cases and of about 800 fuzzed ones must
return every input sample; the library checks every restart interval with the stream's own lossless check;
and a second transcription of the syntax reads the same fixtures. No E-AC-3 check is that exact. It is
weaker where nothing third-party is committed (no DEE TrueHD stream, no FATE excerpt, no encoder hash).

## Open questions this inventory raised

20. **Lossless check across a join.** In `truehd_cat_st48_s24` the second stream's first restart header
    carries a lossless check that checks nothing (ERRATA 12). The library reads it as the check of the
    first stream's last interval, fails it and, strict, stops; FFmpeg logs "Lossless check failed" as a
    warning and decodes on, to the samples the lenient decode gives. Should a decoder skip the check at a
    major sync that starts a new stream (and how does it tell), or is a join an error? Recommendation:
    keep the strict stop and the lenient count, since a join in one file is not a stream a disc carries.
