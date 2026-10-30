# WP5: encoder and decoder quality against DEE and FFmpeg, and the tests and documents that show it

Goal: the TrueHD encoder and decoder are at least as good as DEE and FFmpeg, and better where the data allows, and the repository shows it with measurements that anyone can re-run:
stream size and speed for the encoder, speed, strictness and resilience for the decoder, with the performance tests, badges and documents updated to match. No claim goes into a
document without the number behind it.

Read first: [README.md](README.md), [../truehd-parity-performance.md](../truehd-parity-performance.md) (the performance matrix, arithmetic tiers, SIMD, the seven questions), [../truehd.md](../truehd.md)
("Oracles", "Phases"), `libs/truehd/ERRATA.md` entries 30 to 41 (the encoder), `docs/verification.md`, `docs/library/truehd.md`, `tests/performance/`.

## Starting data (a spot check on 2026-10-30, not a benchmark)

The 13 DEE sources in `testdata/truehd-dee/sources` are 0.25 to 1.5 s excerpts. Encoded by `forge truehd-encode`, by FFmpeg 6.1.1's encoder, and by DEE (the committed `dee.mlp`), in bytes; all of forge's streams decode bit-exactly:

| Source | DEE | forge | FFmpeg |
|---|---|---|---|
| 2.0 music, 48 kHz 24-bit | 16,384 | 16,218 | 19,650 |
| 2.0 music, 96 kHz 24-bit | 47,602 | 54,624 | 58,620 |
| 2.0 tones, 48 kHz 16-bit | 9,140 | 9,116 | 13,896 |
| 5.1 music, 48 kHz 24-bit | 58,688 | **101,314** | 63,254 |
| 7.1 tones, 96 kHz | 87,880 | 46,532 | 58,020 |

The 5.1 music row is the finding: forge is 1.7 times DEE's size and 1.6 times FFmpeg's, with the same two substreams (2 and 6 channels). On synthetic tones forge is smaller than DEE (7.1: three substreams each, 40 to 47 per cent smaller), which says nothing about music. A fuller table is in the owner's report of 2026-10-30.

## Scope, in this order

1. **A comparison harness in the tree**, `tools/checks/compare_truehd_encoders.py` (or similar): for a corpus of sources it encodes with forge, with FFmpeg (the pinned version, `.github/toolchain-versions.json`), and reads DEE's streams from a set directory
   when given one (`--set`, as `score_truehd_decode.py` does; DEE's licence ends 2026-11-06, so the full set stays on the owner's PC and the 13 committed streams are the CI part). It reports bytes, ratio, and that every forge stream decodes bit-exactly. The corpus
   needs real programme material at 2.0, 5.1 and 7.1, 44.1 to 192 kHz, 16 and 24 bits, longer than the 0.25 s excerpts (the repository has 5.1 reference programmes under `testdata/audio`; ask the owner for more, and for speech and film). Use only sizes and the summary fields `forge probe` prints;
   do not inspect how DEE chose its filters or matrices (the owner's rule on Dolby streams, README rule 4). If you think you need that, ask.
2. **Find and close the 5.1 music gap.** Measure before changing anything: which of the encoder's choices costs the bytes (matrix search, FIR order, the IIR filter and LSB bypass that the encoder does not write, codebook and offset choice, quantiser step, restart interval, the lossless-check and FIFO overhead; forge's stream has 3 major syncs in 300 units, FFmpeg's has 19). Each change keeps every
   existing test and the FFmpeg strict decode of the encoder-space fuzz (`tools/ci/fuzz_truehd_encoder_space.py`) green. IIR, LSB bypass and a negative output shift are the unwritten encoder tools (owner scope table: accept noise generator 1 and LSB bypass, defer IIR and negative shift unless the measurement says they are the gap).
3. **Decoder measurements**: speed against FFmpeg's decoder (times real time, 2.0 to 7.1, 48 to 192 kHz, 24-bit), memory, behaviour on damaged input (lost units, the resync after a lost unit, the 64 KiB leading-byte search), and the checks FFmpeg only warns about (the lossless check per interval, CRCs, parity). Strictness and resilience are where the decoder can be better than FFmpeg's;
   say so only with a test that shows it.
4. **Performance tests**: the four workloads in `iclforge-bench`, `iclforge-membench`, `iclforge-perf`'s guards and `iclforge-kernelbench` exist. Add 7.1, 96 kHz and 192 kHz workloads, the encoder's gap-closing changes as regressions guards, and the float, double, fixed-point and SIMD tiers the audit lists (P27 SIMD seam, P28 AVX2 tier);
   Windows, macOS and ARM numbers come from CI legs (WP7), not from guesses. The Cortex-M3 instruction count and the board numbers belong to WP2's ESP32 measurement phase; share the probe.
5. **Documents**: a page (`docs/`) that shows the comparison with the method, the corpus, the tool versions and the date, and links to the harness; the performance cards and badges; `docs/library/truehd.md`, `docs/verification.md`, the capabilities table and the README claims. Replace "0.95 times FFmpeg's size" wherever it appears with the measured table. If forge is worse somewhere, the page says so.

Out of scope: new bitstream features that need a specification answer (heavy DRC, objects), Atmos (WP1).

## Questions to put to the owner

1. What does "better than DEE" mean for the encoder: size at equal losslessness, speed, or both? Recommended: size first (bytes at the same bit-exact result), then speed; DEE's speed is not comparable (it is a licensed binary on another machine).
2. Which corpus? Recommended: ask for 5 to 10 programme excerpts of 30 to 60 s that the owner is free to commit or to keep local, in three groups (music, film, speech), plus the 13 committed DEE sources.
3. DEE's 20-bit tones stream is 16,298 bytes, forge's 9,954 and FFmpeg's 19,598, while the 16-bit versions are 9,140 (DEE) and 9,116 (forge). Is there a DEE option or behaviour that explains DEE's larger 20-bit stream? Recommended: ask the owner, who has DEE's options; do not inspect the stream.

## Proof required

A harness run with numbers in the report and in the page; each encoder change shown to move the number and not to change the decode; new performance guards that fail when the speed or size regresses; documents that match the measurement. The ERRATA numbers for this package are 90 to 99.
