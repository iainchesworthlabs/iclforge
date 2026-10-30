# WP1: the Atmos object layer

Goal: TrueHD Atmos is as complete in this repository as Dolby Digital Plus Atmos (E-AC-3 JOC) is today: the bed and the objects decode, the objects
render, a stream can be written from objects, and the tools and tests around it match. This is the largest item in the appraisal (XL) and the
one with the weakest sources, so it starts with what can be proved and stops at the owner's decisions.

Read first: [README.md](README.md) (rules, environment), [../truehd-atmos.md](../truehd-atmos.md) (the plan and phases W0 to W8),
[../truehd-disc-findings.md](../truehd-disc-findings.md) (what the owner's two discs show), `libs/truehd/ERRATA.md` ("Atmos, spec v3.6", questions 22 to 39,
and question 44), `docs/library/atmos-joc.md` and `docs/library/spatial-and-atmos.md` (what DD+ has), `planning/truehd-parity-library.md` (the 12 Atmos rows).

## What DD+ Atmos has, which is the bar

Decode: OAMD and JOC read into object streams, `forge decode objects_dir=`. Render: `libs/render`'s bed and object renderer to any layout. Write:
`iclforge::ac3::oba::AtmosEncoder` from placements, ADM (`forge atmos-adm`), IAB (`atmos-iab`) and live input; `strip-objects`. Probe: the EMDF payloads
and the object count. Signing: the keyed tag the licensed decoder checks. Tests: Dolby's E-AC-3 reference streams under `libs/ac3/tests/oba`. Host
integration: the GUI live room, Hearth, Crucible.

## What TrueHD has now

The extra data and the evolution frame are read into plain structures (`libs/truehd/include/iclforge/truehd/io/evolution.hpp`), provisionally, and
`forge probe detail=evolution` reports them. OAMD in payload id 11 parses through `iclforge::objects::oba::parse_payload`. Substream 3 is accepted by
the syntax layer and by hand-built streams but is not decoded from a real stream: the owner's discs carry a 12-channel substream whose matrices use `frac_bits`
up to 15, which `forge` refuses above 14, and whose block parse then breaks at a filter order. The decoder gives the 7.1 bed (substreams 0 to 2) for
every disc stream, bit-exact. The encoder writes no substream 3, no `extended_substream_info`, no extra channel meaning, no terminatorB, no evolution frame (an `extra_data` hook
carries bytes it does not read). Nothing renders TrueHD objects.

## Scope, in this order

1. **Decode the bed of substream 3** (restart sync 0x31EC, 12 and 16 channels), with no object metadata. This needs only TH spec §7.2, §9.3, §10 and §15.3 and
   the owner's discs as the oracle: the lossless-check line of `forge decode truehd-presentation=all truehd-check=lenient` on a disc excerpt is a check of the decoder
   against Dolby's encoder that needs no second decoder. Fix the `frac_bits` limit and the block parse, extend `tools/references/truehd_syntax.py` to match,
   and add a hand-built stream that exercises what the discs show. Done when the owner's 12-channel substream passes every lossless-check interval.
2. **Make the bed useful**: `forge decode truehd-presentation=all` writes the full channel set; a bed-only ADM or WAV export (`DirectSpeakers` only; TH spec
   §15.20) needs no object decode.
3. **OAMD to objects** (read side): `forge decode objects_dir=` for a TrueHD Atmos track, through `iclforge::objects`, rendered by `libs/render`. Check the
   result against the disc's own bed where the objects are at bed positions, and against the E-AC-3 container reader. The evolution frame's two readings
   (EMDF `evo_frame`, and the Thrift ContentSignature as a separate PACE layer) are settled by v3.9: the payload is the bit-packed `evo_frame`; keep the
   reader's provisional marks until the owner's disc probes confirm.
4. **Write** (only after 1 to 3 and the owner's yes on scope): the 16-channel substream in the encoder (W3), the presentations the other substreams carry
   (W4: caller supplies the 7.1 bed, or `libs/render` makes it), rate control for the larger stream (W5), the evolution frame writer (W1, about 60 lines if the
   container is EMDF), the OAMD cadence (W2; DEE writes one frame per access unit with timing re-based to 40 samples, TH spec §15.5.3), then the `forge` surface (W7).
5. **Tests and oracles** (W8): see "Tests and oracles, honestly" in the plan. The audio is lossless, so a written stream must decode sample for sample with
   every interval's check passing; the object metadata's outside oracle is E-AC-3.

Out of scope: signing evolution frames (PACE layering, digest scope and the meaning of `evo_protection` are unconfirmed; drop until sourced), heavy DRC, a
renderer for headphone or binaural elements (ids 3, 4 and 6 are reserved per the ETSI standard).

## Questions to put to the owner first (with recommendations)

1. Is the object layer in scope at all, to the DD+ bar above? Recommended: yes for 1 to 3 (read and render); decide 4 (write) after 3 shows the objects make sense on a disc.
2. Where does the extension's 4-bit length nibble sit in the major sync (ERRATA 44), and what does `extended_substream_info` 3 mean (ERRATA 46)? Recommended: read both from
   `forge probe DISC detail=structure` on one more disc; keep the CRC search until then.
3. What the 12, 14 and 16 channel presentations are, and their channel order (questions 34, 35). Recommended: read from the discs' channel meaning (`sixteench_*` fields).
4. Input for a write: ADM master and IAB (both exist for DD+), or a 7.1 bed plus an object file. Recommended: ADM and IAB first; they reuse the front halves of `forge atmos-adm` and `atmos-iab`.
5. Does the 7.1 bed come from the caller or from the renderer? Recommended: from the caller first; render later.
6. Should a written stream be signed? Recommended: unsigned and labelled as not accepted by every licensed decoder, until the owner has seen one play.

## Proof required

Tests that fail when the check is removed; the disc lossless-check results pasted back by the owner (they stay on the owner's machine); the DEE gold set cannot
make Atmos legs (no input format), so there is no DEE oracle. The ERRATA numbers for this package are 60 to 79 (questions and entries).

## Status (2026-10-30, branch `feature/truehd-atmos-bed`)

Steps 2, 3 and 4 are built and tested on hand-built and encoder-made streams; step 1 is not done, because it needs the owner's disc.

- Step 1: the decoder scales matrix coefficients to 2^-15, so `frac_bits` 15 decodes (ERRATA 60). Whether the disc's 12 channel substream
  then decodes is not known: a `frac_bits` spread over 0 to 15 is also what a block header read at the wrong offset looks like, so the first
  check is the owner's, on one disc excerpt: `python3 tools/checks/truehd_substream3_search.py EXCERPT.thd --units 300`. It tries 3,168 readings
  of the open fields (coefficient count, matrix noise shift, FIR state flag, output-shift and quantiser counts, bits after the restart header),
  prints parameters and counts only, and decodes the first restart intervals of all four substreams under each reading that parses. Paste
  its output back (ERRATA question 60). `forge decode DISC.thd out.wav truehd-presentation=all truehd-check=lenient` is the lossless-check line.
  Result (2026-10-30, ERRATA 65 to 68): **step 1 is not done.** The restart header of substream 3 reads (CRC passes, `min_chan` 8, `max_chan` =
  `max_matrix_chan` = objects - 1), its continuation blocks read as TH spec §8.2 has them, the evolution frames read 757 of 757, and the
  matrix layout (4 bit output channel, 4 bit `frac_bits`, bypass bit, 14 presence bits, `frac_bits` + 2 bit coefficients) reads the first
  matrices of a restart identically from restart to restart, on the owner's disc and in DEE's own encodes. It does not read the rest: no layout in the
  family tried fits the 12 matrices the count says into a silent DEE restart, so the matrices of substream 3 are coded more compactly than the
  standard form in some way the repository cannot find without a reference. `forge decode` fails at every restart (filter order or
  coefficient width), and nothing of substream 3 below the restart header decodes. What would end it: a parse log of a licensed decoder for
  one restart, or the matrix file DEE builds and deletes (`matrix_info_mx.mx`). Park it as WP1b.
  Found on the way and fixed (ERRATA 66): the AC-3 core frames between the units of a Blu-ray track, which lost 80% of an excerpt for the bare
  stream `forge demux` writes, and a matrix output channel above `max_matrix_chan` (ERRATA 65).
- Step 2: `forge decode truehd-presentation=all` writes the full channel set; `adm_out` with another presentation is the bed as DirectSpeakers.
- Step 3: `objects_dir`, `adm_out` and `truehd-render=` (ERRATA 61, questions 61, 62). The render of a stream's objects equals its bed
  when the stream was written by `truehd-atmos`; on a disc the same comparison (`truehd-presentation=8ch` against `truehd-render=L,R,C,LFE,Ls,Rs,Lrs,Rrs`)
  is the check of the channel-to-object mapping that question 61 asks for.
- Step 4 (the owner said yes to the write side on 2026-10-30): `EncoderConfig::object_channels`, `forge truehd-atmos` (ERRATA 63, 64). ADM master input is
  there (decision 5). Left: IAB input, signing (decision 4), the project file and the GUI.

