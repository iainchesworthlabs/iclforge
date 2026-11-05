# feature/truehd-measured-bed-render: the bed render by the measured law

Branch `feature/truehd-measured-bed-render`, on `main` after PR #1735 (TrueHD Atmos substream 3), whose clustering and bed-render
code (`render_bed`, `cluster_elements`, `forge truehd-atmos`) it changes.

New: `include/iclforge/truehd/encoder/bed_pan.hpp` and `src/encoder/bed_pan.cpp` (the law, the trim, the limiter, exported so that
`forge` can use them); `PanLaw::kMeasured` in `render_bed` (now the default of `pan_to_bed`, `mix_gains`, `build_render_plan` and
`make_render_plan`); the forge renderer (`truehd_render.{hpp,cpp}`) uses the law for the stream's 7.1 bed; tests
`tests/encoder/test_bed_pan.cpp`; a dump tool `tests/tools/render_dump_tool.cpp` (`iclforge-truehd-render-dump`).

Evidence tags as before: **MEASURED** is read off the owner's DEE controls through the specification answers the owner shared
(`spec-answers-2026-11-04`, parts A and B, cleared for this work) and through `ctl_b`, `ctl_e`, `ctl_k` decoded by this repository's own
decoder (the three streams in `D:\ac3bld\drp-acceptance\dee-controls`); **INFERRED** is the report's own tag; **GUESS** is a choice of
this branch. No DEE or disc content is in the repository: the tests carry the report's hand values and three places named by number.

## 1. What the law is (MEASURED unless marked)

The place is the metadata's 6 bit code, never a position between two codes: x 0..62 (left to right), y 0..62 (front to back), z 0..15.

* **Rows.** y is bracketed between the front row (y 0: L at x 0, C at 31, R at 62), the side row (y 31: Ls at 0, Rs at 62) and the rear row
  (y 62: Lrs, Rrs); the two rows get cos and sin of pi/2 times the fraction of the way. **Columns** likewise within a row (the side and rear
  rows have two speakers 62 apart). A place between rows is the outer product, so the squares add up to 1.
* **Height.** A 7.1 bed has no upper layer: z does not move the place and enters only the trim.
* **Trim,** the only fixed gain: `T * w` dB, `w = min(1, y / 37.2 + max(0, (z - 3) / 12))`, T = -4.5 (3.1), -3.0 (5.1), -2.25 (5.1.2),
  -1.5 (7.1, 9.1, 5.1.4), -0.75 (7.1.2, 9.1.2), 0 (7.1.4/6, 9.1.4/6). The stream's bed is 7.1: T = -1.5. y is checked below.
  **The height term is MEASURED** (it was the report's medium-confidence `(z / 15 - 0.2) / 0.8`, which is the same number as `(z - 3) / 12`,
  so the code did not change): the investigator measured it on the Reference Player's own renderer to 7.1.4, 5.1.4 and 7.1.2
  (`spec-answers-2026-11-05c`, P1-4). z 0 to 3 is a dead zone and from z 4 the attenuation is one twelfth of T per code; the fit is
  0.005 to 0.006 dB, where `T * z / 15` is off by up to 0.6 dB. The elevation pan in those layouts is exactly cos and sin
  (top / floor = `tan(pi/2 * z / 15)`, implied scale 1.000 at all 14 non-degenerate points). Caveats: a centred front object, one object,
  12 element encodes, the z axis only (not z together with x or y), and heights in the three layouts above, not in the 7.1 bed itself, which
  has no upper layer, so there only the trim applies.
* **No object-count headroom**, no renormalisation after the pan. **LFE** at unity, not filtered; no object feeds it.
* **Limiter** 0 dBFS, 32 samples of look-ahead (the report's player renderer). **Still PROVISIONAL.** Only the ceiling and the look-ahead
  are reported, and the investigator's test (P1-5) did not confirm them: through the player's `dlboar` the limiter on and off are
  sample-identical after the 32 sample delay, no gain reduction was seen (the masters' low level was a 24 to 29 dB dialnorm cut, and with
  `skip:dialnorm=-31` both clip at 1.0), so the constants are unverified. The attack and release shape (a mean of the look-ahead minimum over the 32 samples behind, one gain for all channels) is this branch's
  choice (GUESS) and is not delayed, because the encoder's bed has to stay aligned with the elements.
* **Ramps.** Linear in the per-speaker gain in time, per 32 or 64 sample block, over the metadata's 1536 samples, to the value at the END
  of the frame. A plan with 1536 sample sections and the placement at each boundary is exactly that, and the first update takes
  effect at once. **Now MEASURED on the player's renderer** (P1-3, step masters on one object, steps of 1, 2, 8 and 15 codes at sample
  48,000): the transition starts within 32 samples and completes about 1,536 later, with 10 to 90 % widths of 1,104 to 1,424 samples against
  1,229 for a linear 1,536 ramp (the 2 code 2,208 is an RMS envelope artefact); the target is the value at the END of the ramp and the step
  size changes only the amplitude. It is the same ramp for p16 and p8 (p8 on the same master: 10 to 90 % width 1,273). The 5,120 sample
  ramps of the DEE sweeps were an artefact of those sweeps, not the renderer's rule (this note's earlier 1.6 k and 5.1 k are DEE's own beds).

Element 0 is the LFE and elements 1 to N-1 are the dynamic objects in payload order (report B1): in this branch's model that is the
LFE on bed row 3 and the free elements' pans through `bed_pan_gains`.

## 2. What was checked, and what the numbers are

All on the 7.1 bed as this repository's decoder gives it (`iclforge-truehd-render-dump`; the streams pass their lossless checks, 48,028 of
48,028 segments), **and checked against the Dolby Reference Player's presentation 8** of the same three streams (the owner's runner,
`D:/ac3bld/drp-acceptance/dee-controls-p8`, 2026-11-05): the player's WAV is 16 bit, the channels are in the decoder's row order (the
y sweep of `ctl_k` plays on channels 0, 4, 6 in that order), and sample for sample it is the decoder's bed times 10^(-8/20) (a
constant, probably the dialogue normalisation, not checked) rounded to 16 bits: the largest difference over 470,000 frames is 0.47 (`ctl_b`) and 0.51 (`ctl_e`,
`ctl_k`) of a 16 bit step. So the decoder's bed is the player's bed, and the numbers below hold for either; the last column of the
table gives the error against the player's WAV in 16 bit steps. Comparisons are of the bed row `r` against `gain_r(t) * element_1(t)` (the
one object; the tone peaks at 0.1 FS, 1305 steps in the player's WAV), with the gain from the payload's codes by the law.

| Control | What it holds | Result |
|---|---|---|
| `ctl_b` | one object, static at x 62, y 5, z 0 | max error 164 LSB of 24 bits = 1.96e-5 FS (0.02 % of the tone); R 0.946, Rs 0.245 (law 0.9459, 0.2449). Against the player's WAV: max 0.7 step, rms 0.16 |
| `ctl_e` | y 0, x 0..62 in 62 code steps, one per 5 frames | in 533 of 533 steady windows the bed holds the law's value for the payload's code; gain residual to the nearest code median 1.6e-4, max 4.7e-3 |
| `ctl_k` | x 0, y 0..62 | 452 of 481 steady windows on the payload's code, 29 one code lower; median 5.0e-4, max 1.06e-2 |
| `ctl_k`, trim | `w` read from the bed's power against `y / 37.2` | 0.079 vs 0.081 (y 3), 0.427 vs 0.430 (y 16), 0.749 vs 0.753 (y 28), 0.991 vs 0.995 (y 37), 1.00 above: within 0.01 |
| `ctl_e`, `ctl_k` through the sweep, sample by sample | the 1536 ramp model | max 28,544 and 30,319 LSB (3.4e-3 and 3.6e-3 FS, about two thirds of a code step of gain against the tone's 0.1 FS peak), rms 1445 and 2998 LSB. Against the player's WAV: max 44.5 and 47.6 steps (3.4 % and 3.7 % of the tone), rms 2.3 and 4.7 steps |

The last row is the honest limit. The DEE controls' own beds do not ramp over the metadata's 1536 samples: the slope of the bed's gain
shows ramps of 1.6 k samples (a code step at the start of the stream) and of 5.1 k samples (a restart interval, 128 units) centred on the
code change, chosen by DEE's matrix updates, so a sample-for-sample match of a moving object needs DEE's section schedule. The law (the
values the gain moves between) matches; the path between them is the metadata's, as the player's 16 channel render has it. No change to the
ramp was made to chase the bed (it would not be a rule, and the object metadata is what the player's renderer follows).

**This branch's own encoder against the law.** Three streams from `forge truehd-atmos` (a LFE and one object, 6 s: static at the ctl_b
place, a front-wall sweep, a left-wall sweep) decoded and compared with the law and the ramp read back from their own metadata: max error
0.48, 0.50 and 0.50 LSB of 24 bits (rounding), so the encoder writes the law it states, trim and ramps included.

Cross-checks in the tests: the report's hand values (x 0, y 16: 0.689 / 0.725; x 31, y 31: 0.7071 / 0.7071), squares adding to 1 on all
63 x 63 places and to 10^(T w / 10) with a trim, the layout trims, `w` at its corners, height moving only the trim, the limiter's
ceiling, look-ahead, linked gain, transparency below the ceiling and absence of delay.

## 3. What changed in existing behaviour

* `make_render_plan`, `build_render_plan`, `mix_gains` and `pan_to_bed` default to `PanLaw::kMeasured`. The ring (held to `libs/render`)
  and wall laws stay for the tests that hold them; the chain and ramp tests of `test_cluster_elements.cpp` name `kRing` so that they stay
  about the matrices (with the measured law the codes make the intended bed a staircase and the −43 dB figure −36 dB, which says nothing
  about the chain).
* `forge truehd-atmos` renders its bed with the law and the limiter (it printed the clip count; it now prints the limiter's count and
  clips only what the 24 bit grid cannot hold). `forge decode truehd-render=L,R,C,LFE,Ls,Rs,Lrs,Rrs` uses the same law and no limiter, so
  that the bed of the stream and the render of its objects are the same programme where nothing exceeds the ceiling (a test holds it).
  Other layouts keep the layout renderer.
* The encoder's own default render (an object with no path is mixed at -3 dB into its row, `ObjectPath`) is unchanged: it has no place to
  apply a law to, and it is the matrices session's file. Every path the render tool or `forge` makes is the law's.

## 4. Questions

1. **Which bed row is which speaker (answered by the player's WAVs).** The rows are the decoder's L R C LFE Ls Rs Lrs Rrs, and the
   player's WAV has the same channel order: in `ctl_k` channel 4 carries the place at y 31 and channel 6 the place at y 62. The report's
   B1 says the p8 PCM order is L R C LFE Lrs Rrs Ls Rs (positions 4 and 5 the rear corners): for these controls that would put y 62 on
   channel 4, which the player does not do. Either B1's order describes the file's channel mask (0xC3F) and not the samples, or its names
   for y 31 and y 62 are swapped. Recommendation: have the analysis session look at B1; this branch follows the samples.
2. **Interior** (height is answered above: P1-4 measured the z trim and the elevation pan). The interior is measured only as the outer
   product along y at x 0 and at the front, and height only for a centred front object.
   `ctl_j` and `ctl_l` (not available to this session) are the ones to check the outer product against. Recommendation: run
   `iclforge-truehd-render-dump` on them and compare as above.
3. **The ramp schedule of DEE's bed.** 1.6 k versus 5.1 k sample ramps, centred on the code change. If the owner wants the p8 bed to match
   DEE's sample for sample, the matrix side needs DEE's section schedule (SUBSTREAM3.md, "restart anchors"); the metadata's 1536 is not it.
   Recommendation: do not chase; the player's 16 channel render follows the metadata.
4. **The limiter's shape.** Only the ceiling and the look-ahead are reported, and P1-5 could not confirm even those (no gain reduction
   visible through `dlboar`; the limiter numbers must come through the player's own graph). Recommendation: leave until a control drives a
   sum over full scale through it.
5. **Where the trim belongs for pinned (bed) elements.** The report says speaker-anchored (bed) objects play at unity, so a pinned element
   takes no trim here; a dynamic element does. Not checked against a control with a bed object.
