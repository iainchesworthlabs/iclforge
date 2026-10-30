# TrueHD Atmos: the write side

As of 2026-10-30. The first part is built, provisionally (WP1, `planning/work-packages/WP1-atmos-object-layer.md`: the status
section below); the rest is the plan of 2026-10-29. [truehd.md](truehd.md) is the plan for the codec as a whole (phase T2b is this page);
[libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md), "Atmos, spec v3.6", has the questions this page waits on (22 to 39) and the comparison of
the owner's specification with ETSI TS 103 420 that they come from.

## Status, 2026-10-30 (WP1)

| Phase | State |
|---|---|
| W0 | the owner's disc probes: `tools/checks/truehd_substream3_search.py` is the check for the block header of the 12 channel substream (ERRATA question 60); `forge probe DISC detail=format` answers 22 to 24 and 33 |
| W1 | built in `forge`: the evolution frame is `iclforge::objects::emdf::build_container` without its sync and length (`truehd_atmos_encode.cpp`); no writer in `libs/truehd` |
| W2 | built: one OAMD payload per 1536 samples, the cadence of the discs (ERRATA question 62, entry 64) |
| W3 | built: `EncoderConfig::object_channels`, substream 3 with restart sync 0x31EC, the 16-channel extension in the major sync (entry 63) |
| W4 | built, both ways: the caller's 7.1 bed (`truehd-bed=`) or the objects placed on 7.1 by `libs/render` (decision 6: the owner's call; both are there) |
| W5 | not measured beyond the real-audio test (15 Mbps over a unit for 7.1 + 12 channels); no rate model change |
| W6 | not built: signing |
| W7 | built: `forge truehd-atmos`, `forge decode` with `objects_dir`, `adm_out` and `truehd-render=`; not the project file or the GUI |
| W8 | built: `test_encoder_atmos.cpp`, `test_cli_truehd_objects.cpp`, `test_cli_truehd_adm.cpp`, the hand-built streams `hb_substream3_fine_matrices` and `hb_atmos_objects`, `verify_truehd_atmos_interop.py`; FFmpeg reads the bed of a written stream |

The decoder reads matrix coefficients to `frac_bits` 15 (entry 60). What is not done is the first thing WP1 asked: the owner's 12 channel
substream does not decode, because the fields of its block header are not known. The input an encode takes is a WAV of object channels with
a scene file, not yet ADM or IAB (decision 5).

## Where this stands

TH spec v3.6 (2026-10-29) is the first version of the owner's specification that gives the object audio metadata (OAMD) as bit-level syntax and
the evolution frame as a bit-level container (§15.4 to §15.15). Checked against the public standard, it agrees on the table values and the
widths of the fields the two share, and differs on the structure of nearly everything else (ERRATA, section B). The owner has said the object
data follows TS 103 420, so the plan takes the standard's syntax, which `iclforge::objects` already writes and reads for E-AC-3, wherever v3.6
differs. The owner's answer of 2026-10-29 is that real streams carry both layouts of v3.6, nested: §15.4's bit-level container outside and §16.3's Thrift
ContentSignature inside one of its payloads (which payload, where, what its digest covers and what the protection bytes cover are still open,
questions 22 and 37). The owner has also authorised settling these questions from the owner's own discs, run on the owner's machine with
`forge probe DISC detail=format` (ERRATA section H says which field answers which question). What rests on the owner's word alone is everything
about TrueHD itself: the extra data framing, substream 3, the 12, 14 and 16 channel
presentations, the signing.

What exists after this change:

| Piece | State |
|---|---|
| The extra data region (§16.1, §16.2, §16.4) and the evolution frame as the EMDF container or as the letter of §15.4 | read, provisionally, into plain structures: `libs/truehd/include/iclforge/truehd/io/evolution.hpp`. Tested against an independent Python transcription and against the E-AC-3 container reader |
| Payload id 11 | handed out as bytes; `iclforge::objects::oba::parse_payload` reads it (it takes a bare payload; no change to `libs/ac3` or `libs/objects` was needed) |
| `forge probe` | reports the frames, payload ids, object count, bed, cadence, and which reading read them; with `detail=evolution`, `detail=structure` or `detail=format` it lists the frames and their shapes, walks any Thrift payload by field id and type (never a value), and reports the major sync, restart header, decode-check and terminator facts that the open questions need. The check to run on a real Atmos track |
| Substream 3 in the decoder | accepts everything §7.2, §8.3, §9.3, §10.3 and §15.3 give it (ERRATA, section G); a hand-built stream holds it |
| `EncoderConfig::extra_data` | frames a payload into a unit's extra data and never looks inside; the chain "OAMD payload from `iclforge::objects`, in the E-AC-3 container's body, framed by the encoder, decoded, read back, parsed" is a test today |
| The encoder | writes substreams 0 to 2 (up to 8 channels, optional 2 and 6 channel downmixes); no substream 3, no `extended_substream_info`, no extra channel meaning (§14.2), no terminatorB |

## Phases

Sizes are the ROADMAP's: S an afternoon, M a day or two, L a focused week, XL several pull requests.

| Phase | What | Needs | Size |
|---|---|---|---|
| W0 | The owner's answers to questions 22 to 24, 33 and 34, from `forge probe DISC detail=format` on two or three of the owner's Atmos tracks (the output is metadata; the owner pastes it back) | the owner | the owner's time |
| W1 | An evolution frame writer. If the container is the EMDF one (`kEmdf`), none is needed in the library: `iclforge::objects::emdf::build_container` writes the body, a program that links both libraries hands it to `EncoderConfig::extra_data`, and the reader's test already shows the bytes survive. A writer in `libs/truehd` is then a convenience of about 60 lines whose only test is "the same bytes as `build_container`". If the stream proves to be the letter of §15.4, `libs/truehd` writes it (the layering rule keeps `objects` out) and the Python transcription already builds the vectors | W0 (question 22 to 24) | S |
| W2 | The OAMD writer: reuse `oba::build_payload_updates` (§5.5; `Program`, up to 31 objects, one to eight update blocks, bed and dynamic objects, ISF is read but not written). The new part is the mapping from the placements a front end supplies (`oba::ObjectPlacement`, ADM, IAB: the front halves of `forge atmos-adm`, `atmos-iab` and `atmos-cbi` exist) to evolution frames at the cadence TrueHD uses, which depends on question 33 (an access unit is 40 samples, an E-AC-3 frame 1536, and the standard's block offset factor runs to 2016) | W0 (31) | M |
| W3 | The 16 channel substream: channel order (beds in Table 12's order, then ISF, then dynamic objects, §15.6.3, question 34), padding to the 12, 14 or 16 channel presentations (§15.17), `extended_substream_info` and `substream_info` bit 7 in the major sync, the extra channel meaning (§14.2, whose length unit is question 2), restart sync 0x31EC, terminatorB, the matrices of §10.3 for substream 3. The encoder's substream splitter has to learn a fourth substream whose channels follow rows 0 to 7 | W0 (32), question 2 | L |
| W4 | The presentations the other substreams carry. A TrueHD Atmos stream is also a 7.1 stream (§20.2: 8 channels from substreams 0 to 2), so the encoder needs the bed mix: either the caller supplies it or `libs/render`'s bed and object renderer makes it from the objects, with the 2 and 6 channel downmixes the encoder already writes (`DownmixMode::kStandard`, ERRATA 38). Which is wanted is a design choice (decision 6) | W3 | M |
| W5 | Rate control for the larger stream. The §18 FIFO model and `input_timing` are kept per unit for up to 3 substreams (ERRATA 37); a fourth substream of up to 16 channels, and the evolution frames in the extra data (a frame with a few objects is tens of bytes; a signed one would be 3.5 to 4 KiB, §16.3.3), change the peak. What `peak_data_rate` means is question 12 | W3, W1 | M |
| W6 | Signing. On the owner's reading the ContentSignature (§16.3, §16.6) is a Thrift struct inside one payload of the §15.4 container, and `evo_protection()` is a further field (an HMAC, the owner says). Nothing is written until the payload id, the digest scope and what the protection covers are known (questions 22 and 37) and the owner decides whether a stream should be signed at all (decision 4). `libs/base/src/crypto` and `libs/ac4/src/io/content_signature.cpp` hold the primitives and the Thrift reader for AC-4; the same envelope is claimed for TrueHD | W0, decision 4 | L, if wanted |
| W7 | The forge surface. Either `forge truehd-encode` takes the object inputs (`objects=`, `adm=`, `iab=`) or a new `truehd-atmos` command does (the second keeps `truehd-encode`'s WAV-in rule simple); and `forge decode truehd-presentation=all` gains an object output beside the WAV (`objects_dir=`, as the E-AC-3 decode has). The project file's command (`apps/shared/project`) and the GUI follow (neither names TrueHD yet, [truehd.md](truehd.md), "Not done in T2c") | W2, W3 | M, then M |
| W8 | Tests and oracles (below) | all | M |

Total, without signing: about XL, most of it W3 and W4. With signing, a second XL.

## Tests and oracles, honestly

No decoder outside this repository reads these streams. FFmpeg's decoder refuses restart sync 0x31EC, decodes no object audio metadata, and has
no encoder for them; no other decoder is used here (the owner's rule). So:

- **The audio is lossless, and that is the strongest check there is.** A stream written with substream 3 and decoded with `Presentation::kAll`
  must give back every channel sample for sample, and each restart interval's lossless check (§7.3) must pass. That proves the substream 3
  coding and the matrices are self-consistent and agree with the independent Python reader; it does not prove that a licensed decoder reads the
  stream the same way.
- **The object metadata has one outside oracle: E-AC-3.** The OAMD syntax of TS 103 420 is read by `iclforge::objects`, which the repository's tests hold to
  Dolby's own E-AC-3 reference streams (`libs/ac3/tests/oba/test_dee_joc_fixture.cpp` and the fixtures it names). If the owner is right that TrueHD
  carries the same syntax, a payload that parses there parses here. The same payload in the TrueHD container is checked with the Python
  transcription and the E-AC-3 container reader (done for the read side).
- **The owner's Blu-ray Atmos tracks are the first outside check of the read side**, and they can be used without anything entering the
  repository: `forge probe DISC detail=format` prints the evolution frames (counts, payload ids, shapes, a Thrift walk, object counts, which reading parsed) and the stream syntax facts; `forge decode
  truehd-presentation=all truehd-check=lenient` on the same track writes a WAV the owner can discard and prints a `lossless check: N
  interval(s) ok, M failed` line, which for a real substream 3 is a check of the decoder against Dolby's encoder that needs no reference decoder at all.
  Neither command prints content. What to paste back: the `evolution` lines of the probe and the `lossless` line of the decode. This is the
  step that settles questions 22 to 24 and 33 to 35 for the read side, and the write side should not start before it.
- **A DEE gold set could prove the write side, if DEE writes it.** `tools/generators/gen_dee_gold.py` has an Atmos leg for TrueHD
  (`--presentation atmos` with an ADM master; whether DEE accepts it for TrueHD is not known, [truehd.md](truehd.md), "Oracles" item 3). A DEE
  stream of a known master, decoded here and scored against the master, would be the only external check of both the substream 3 layout
  and the container; the licence ends on 2026-11-06, so the set has to be made before that, on the owner's machine, and stays there.
- **What nothing proves:** that a licensed decoder or a disc player accepts a stream this project writes (the channel order, the cadence of the
  frames, the signing). That is the same position the container writers are in (`libs/containers/ERRATA.md`).

## Risks

1. The specification contradicts itself and the standard in §15 (ERRATA B and C); a writer built on the wrong side of question 22 or 23
   produces streams that parse here and nowhere else. Mitigation: no writing before the owner's answers (W0), and the reader's two readings.
2. The time base (question 33). A wrong cadence is invisible to our own decoder and audible to a real renderer.
3. Channel order and the 12, 14 and 16 presentations (question 34): the same, and the bed mix of W4 has to agree with the object mix or a
   legacy decoder plays a different programme from an Atmos one.
4. Signing: a licensed decoder may require the signature (§16.3.1 says verification happens before the payload is used, §16.6.4). If it
   does, an unsigned stream carries no objects for it and the work is for nothing without W6; if the signing material is Dolby's, it cannot
   be produced here at all (if the chain a licensed decoder trusts is Dolby's, this project cannot produce a stream it accepts;
   [object signing](../docs/concepts/object-signing.md) and the AC-4 signing work say what this project can and cannot sign).
5. Provenance: the specification derives in part from analysis of Dolby's encoder (the owner has said so and takes responsibility). Nothing
   from that analysis (offsets, internal names, messages) is copied; the plan stays on wire-format facts.
6. Size: the encoder's channel splitting and rate model were written for three substreams and 8 channels; W3 and W5 touch both.

## Decisions for the owner

1. Which container real streams carry, and the coding of variable-length numbers (questions 22 to 24). Settled by the probe on real tracks.
2. The cadence and time base of the object metadata (question 33) and what the 12, 14 and 16 channel presentations are (question 34).
3. Whether the write side should follow the standard's syntax where v3.6 differs (recommended; questions 26 to 32).
4. Whether a stream this project writes should be signed, with what key, and what an unsigned stream is for (question 37, question 1). Recommended:
   unsigned, labelled in the stream's documentation as not accepted by every licensed decoder, until the owner has seen one of the owner's
   decoders play it.
5. The input for an Atmos encode: an ADM BWF master and the IAB reader (both exist for E-AC-3 and AC-4), or a pair of a 7.1 bed WAV and an
   object file.
6. Whether the 7.1 bed (the legacy presentation) is supplied by the caller or rendered from the objects (`libs/render`), and with what downmix.
7. Whether to make the DEE gold set for TrueHD Atmos before 2026-11-06, and if so with which masters.
8. Which of the owner's tracks may be probed and decoded locally for W0, and what they paste back (the probe's evolution lines and the decode's
   lossless line, above). Recommended: two tracks, one with a 7.1.2 bed and objects and one 5.1 with fewer objects.

## Not in scope

Rendering the objects to speakers in `libs/truehd` (the decoder hands out bytes and channels; `libs/render` renders); headphone or binaural
rendering (the headphone element is reserved in the standard and has no layout in v3.6, question 28); 44.1 kHz-family Atmos (§15.2 allows 48 kHz
only); MLP; and anything that analyses a Dolby-made stream beyond what the owner's specification and the owner's own tracks, read by the
owner's own machine, show.
