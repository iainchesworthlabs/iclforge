# WP6: validation, and closing the provisional marks

Goal: every reading in `libs/truehd` that is marked provisional is either confirmed by evidence and the mark removed, or corrected, or left marked with a stated reason and a named way to settle it. Much of this is the
owner's time (the owner's PC, the owner's discs, the owner's receiver); the agent prepares each run so that it takes the owner minutes, reads what the owner pastes back, and updates the repository.

**Time-critical:** DEE's licence ends on 2026-11-06 and is not renewed. The last day to start a new DEE leg is 2026-11-04. Items 1 and 2 below come first and must be prepared in the first hours.

Read first: [README.md](README.md), `libs/truehd/ERRATA.md` (every entry and question marked provisional), [../truehd-disc-findings.md](../truehd-disc-findings.md), [../truehd-fixtures.md](../truehd-fixtures.md),
`docs/verification.md` ("Scoring DEE's TrueHD streams"), `tools/checks/score_truehd_decode.py` (statuses PASS, ALTER, UNEXPLAINED, FAIL, SKIP), `tools/generators/gen_dee_gold.py`.

## What the owner has run so far

The DEE TrueHD gold set of 2026-10-29 (131 streams, 128 scored, three 300-second programmes, 13 small ones committed in `testdata/truehd-dee`), which scored 26 PASS, 5 ALTER, 97 FAIL before the decoder fixes; of the 13 committed
fixtures 11 now decode bit-exactly and two (96 kHz) are UNEXPLAINED. Two Blu-ray discs: *Blade Runner 2049* (one Atmos stream) and *Dune: Part Two* (three), 60 s excerpts, giving `planning/truehd-disc-findings.md`.
The scorer's UNEXPLAINED status means: the decode passes the stream's own lossless check but is not the source and no option of the leg says why; it does not fail the run.

## Scope, in this order

1. **The full DEE rescore** (owner, on their PC, after the next push): exact commands for `score_truehd_decode.py --set <dir> --forge <forge>` and for `gen_dee_gold.py --only thd-` top-up; read the report the owner pastes back; update `docs/verification.md`,
   `planning/truehd.md` ("Oracles"), the scorer's docstring and `planning/ac4.md` (the five documentation sentences the owner's local agent listed); classify every FAIL as a decoder bug, a scorer limit or a DEE effect with the evidence.
2. **Last DEE legs** before 2026-11-04. Prepare the list and the exact commands for the owner by 2026-11-02: two encodes of one programme with dialnorm 0 and with another value per presentation, to settle the units of the dialogue-norm field (ERRATA 51), the 20-bit
   and 96 kHz questions, a stream with DRC profiles that differ, and an Atmos leg if the owner has a Dolby-authored master (DEE takes no evolution-frame input). Each leg is a test input; nobody analyses its bytes.
3. **Disc probes** (owner): the exact `forge probe DISC detail=structure|evolution|format` commands and what to paste, for two or three more discs including a 5.1 Atmos one and a non-Atmos TrueHD one. They settle ERRATA 9 (block CRC coverage), 10 (where the heavy-DRC gate sits), 44 (where the extension's length nibble sits), 46 (`extended_substream_info` 3),
   the width of `zero_samples`, whether MP4/MKV/TS carry an Atmos flag, and give a second sample of `twoch_control_enabled`, `peak_data_rate`, the DRC start-up gain. Update the findings page with each result.
4. **The provisional marks.** For each mark in `libs/truehd/ERRATA.md`, record what would remove it and whether that evidence now exists. The owner's rule: TH-5 (16-bit `seed_shr7`), TH-6 (lossless-check shift by matrix channel), TH-18 (filter history survives a restart) and TH-19 (extra-data length is words less one) stay marked until the DEE set scores them; remove them only when the rescore shows it,
   citing the numbers. The same for the readings the discs and v3.9 have confirmed.
5. **A list of specification issues for the author** in `planning/truehd-spec-issues.md`, so the owner can hand it over: §2.3, §20.1 and Appendix A still show the old restart-sync reading that §7.2 withdrew; §7.4 words `ch_assign` backwards; §17.3 omits the noise buffer size, start index and stride (ERRATA 43); §10.5's coefficient scale (ERRATA 48); §4.5 value 3 (ERRATA 46);
   §19.1 and the 96 kHz legs; where the extension length nibble sits (ERRATA 44); §16.8's capacity rule against the observed frames; anything else you find. One line each with the section, the contradiction and the reading the library uses.
6. **Metadata semantics** (ERRATA 51 to 55): once item 2's encodes exist, read the fields with `forge probe` (the library reads what the stream sets) and write down what they show; propose the units; leave `normalize` and `qc` as built until the owner confirms.
7. **Fixtures and counts**: add the owner-approved small DEE streams; fix the fixture counts that disagree between `planning/truehd-fixtures.md` and the library page; keep `check_truehd_coverage.py` at zero missing.

Out of scope: any change to the decoder that a mark's removal does not need (that is WP1 or WP5).

## Questions to put to the owner

1. The scorer's UNEXPLAINED legs (96 kHz, and the stepped-tone DRC legs on the owner's set): does DEE's default for 96 kHz input apply a filter, or can anything in its DRC or loudness path change a sample by one LSB? Until answered they stay UNEXPLAINED; the stream's own lossless check is the evidence the decoder is right.
2. Which discs may be probed, and who pastes what. Recommended: two or three, the probe output only (it prints no audio).
3. A Dolby-authored Atmos master for one DEE leg by 2026-11-04, or none.

## Proof required

Every removed mark cites a score or a probe result in the ERRATA entry; the verification page's numbers are the rescore's; the spec-issues page is complete enough to hand over; the report lists what the owner still has to run. The ERRATA numbers for this package are 100 to 109.
