# feature/truehd-atmos-encoder-render: clustering and the bed render

Branch `feature/truehd-atmos-encoder-render`, from `feature/truehd-atmos-bed` at 56b4346. New files in `libs/truehd/src/encoder/`:
`render_plan.hpp` (the interface with the matrix side), `render_bed.{hpp,cpp}` (the panner, the gain trajectory, the bed-row matrices),
`cluster_elements.{hpp,cpp}` (objects to elements). Tests: `tests/encoder/test_render_bed.cpp`, `test_cluster_elements.cpp`.
Nothing here is reachable from `forge` yet.

Evidence tags as in [SUBSTREAM3.md](../SUBSTREAM3.md): **STREAM** is something an owner's stream shows (through the repository's own
documents, not through anything I opened); **GUESS** is a choice of this branch with no stream behind it; **PROVISIONAL** rests on a
reading another document marks provisional.

## 0. What I had, and did not have

No lab output on `ctl_b`, `ctl_c` or `ctl_e` to `ctl_h` has reached this session, and I opened none of the owner's files. Everything
about *how DEE clusters* below is therefore a GUESS. What the repository's documents say the streams show, and nothing more, is used:

| Fact | Source | Tag |
|---|---|---|
| substream 3 has N = 12, 14 or 16 channels, `min_chan` 8, the coded rows are 8 and up | SUBSTREAM3.md section 1 | STREAM |
| the matrices rebuild the output channels from bed rows 0 to 7 (as substreams 0 to 2 decode them) and the coded rows | SUBSTREAM3.md section 1 | STREAM |
| in DEE's encodes the bed is a dynamic downmix of the elements; the matrices are its inverse and differ from restart to restart | SUBSTREAM3.md section 1, ERRATA 68 | STREAM |
| the matrices are primitive: a coefficient of exactly 1.0 on the matrix's own output (ten of ten in the steady restart of the disc) | ERRATA 69 | STREAM |
| `ch_assign` is a permutation, output channel p = element p | SUBSTREAM3.md, ERRATA 65 | STREAM |
| one of the 12 to 16 channels is the LFE, the rest dynamic | WP1 status 2026-10-31 | STREAM (owner's facts) |
| a delta in the matrix extension is the change of the coefficient over one block: raw + delta / 8 = next section's raw | ERRATA, "Extension deltas as a coefficient ramp" | PROVISIONAL |

## 1. The model (GUESS, chosen because it is the one the integer chain can invert)

The 7.1 bed rows 0 to 7 of substreams 0 to 2 are

    bed[j] = element[j] + sum over k of gain[j][k] * element[8 + k]

Elements 0 to 7 are **speaker-anchored** (element j sits at bed row j's wall position); elements 8 to N-1 are **free**. The decoder's
matrices turn that back into the elements: one matrix per bed row, own coefficient +1.0, coefficient -gain on row 8 + k. A general
panning matrix does not have a unit diagonal and cannot be inverted in integers; this one does, and it is the form the existing
`kBedMatrices` chain already has with a fixed gain. Why this and not DEE's: ERRATA 69 shows DEE's matrices are primitive too, so the
structure is not unlikely, but which elements DEE anchors to speakers (if any) is open (question 3).

The LFE is bed row 3 and element 3. Nothing is panned to it. A bed-channel object (the master's 7.1.2 bed arrives as objects) is
**pinned** to its row's element whatever its keys say. `lfe_send` of a dynamic object is ignored (question 7).

## 2. The clustering rule (GUESS)

Input: objects with PCM and a key track (sample, x, y, z, gain; straight lines between keys, held outside). Room coordinates are
TS 103 420 section 4.2.1's. Sections of `section_samples` (default 480 = 12 access units); every decision below is made per section.

1. **Pinned** objects (`bed_row` 0 to 7) go to the element of their row.
2. A dynamic object whose position at the section's centre is within `snap_radius` (0.12 room units, height counted half) of a speaker's
   wall position joins that speaker's element. The LFE row is never a candidate. An object that was in that speaker's element the section
   before stays while it is within `snap_radius + hysteresis` (0.08).
3. The other dynamic objects go to the free elements. With as many free elements as objects, each keeps the element it had, and a new
   one takes the lowest free one. With more objects than free elements: the loudest (by energy in the section, ties by index) seed one
   element each (keeping their previous element where free), the others join the nearest, and the elements move to the energy-weighted
   centre of their members, three passes; an object's own previous element gets a `hysteresis` discount in the distance.
4. An element's **place** at each section boundary is the energy-weighted centre of the objects that are in it in either of the two
   sections that meet there, at the boundary's instant. Hence the place follows the objects, and a newly active element is already in the
   right place when its signal rises. An element with no object either side holds its place.
5. The element **signal** is the sum of its objects' PCM times their gains. An object that changes element is faded out and in over
   `handover_samples` (40) centred on the boundary, with weights k / (handover + 1), so that the two parts add to the object and no element steps.
   The sum is clipped to `bits` and the clipped samples counted.

Not done on purpose: an object split between two elements (amplitude panning between elements); the master's size, divergence, snap and
zone constraints (they travel in the object metadata, not in the clustering); elements rescaled for headroom.

## 3. The render

`pan_to_bed(x, y, z)` pans by azimuth, counter-clockwise from the front, atan2(0.5 - x, 0.5 - y), on the 7.1 ring (L 30, C 0, R -30, Ls 90, Rs -90,
Lrs 150, Rrs -150), constant power between the enclosing pair. Height is ignored: a legacy 7.1 bed has no height, so a high element is heard at full
level on the ring. The test holds it to `iclforge::spatial::pan_ring` over 440 room positions to 1e-9. `libs/truehd` links `base` alone (its
CMakeLists says so), so the panner is its own code and the test is what keeps it from drifting from `libs/render` (question 1).

The plan: each element's place at the section boundaries gives gains at the boundaries; between two boundaries the gains move in a straight
line, and a section's end is the next one's begin, so there is no step. `gains_at(plan, sample)` is the trajectory. The matrix side quantises it:
`quantise_mix_gain` (frac_bits 12, 14 bit signed range) and `bed_mix_matrices` (the 8 bed-row matrices). For the extension's deltas (decoder session, `39f303f`, PROVISIONAL until a
stream confirms it): a coefficient moves by delta / 2^ext_field2 raw places in every 40-sample access unit, and a raw place is
2^-(frac_bits - f_shift) (not 2^-frac_bits). So per matrix and coefficient the slope in raw places per unit is
(end - begin) * 2^(frac_bits - f_shift) * 40 / section_samples, and delta = round(slope * 2^ext_field2) with ext_field2 0 to 3 and the width
chosen so the largest delta fits in width + 1 signed bits. The matrix side derives this from the plan; nothing here changes.

### What ctl_c shows about the anchoring (relayed by the decoder session, not seen by me; PROVISIONAL)

The first matrix list of `ctl_c` has one matrix that writes row 8 with its own coefficient -1.0 (f_shift -1), then matrices that write outputs
0, 1, 2, 4, 5 and 6 and 11 with coefficients on the coded rows 8 to 11, the same value on rows 8, 9 and 10 in each (e.g. -2774 at frac 14 in the
output-2 matrix). The tone is carried by coded row 11 (rows 8 to 10 are silent). Reading: DEE's bed rows 0, 1, 2, 4, 5, 6 are indeed
"element j plus a mix of the coded rows" as in section 1 (the speaker-anchored form), and the rows are not an object's own: one coded row carries
the object and the matrices mix every coded row, silent ones included, into each bed row. This supports the anchored-elements-0-to-7 guess for
the LFE-less rows, and says nothing yet on how DEE picks the coded row for an object. Row 3 (LFE) has no matrix in that list.

### What the analysis session's lab dumps show (2026-11-01; read from `D:/ac3bld/thd-ss3-lab-out29`, parameters and counts only; I opened no master)

Channels below are **matrix channels** as the lab prints them, not output positions: `ch_assign` of each stream was not in the dumps, so "element 11"
in one file and "element 11" in another need not be the same speaker or slot. All numbers are STREAM (the lab's), the readings are mine and PROVISIONAL.

1. **A lone moving object sits on one coded row at full amplitude** (`ctl_e`, a 1 kHz tone moving over 10 s): matrix channel 11 carries it at
   +-838,880 (unit gain, its own amplitude) in coded and output; matrix channels 0, 1, 2 hold large coded values (+-255k, +-983k, +-800k) and output 0. So the matrices
   subtract the object's share from three bed rows using row 11: bed[j] = element[j] (here silent) + g_j(t) * element[11], the `[I | G]` form of section 1,
   for a *single free element*. That g_1 peaks at 1.17 of the object means the shares are not a unit-power pan.
2. **The shares move by the extension, with sections far apart.** `ctl_e` sends sections at 0, 4640, 4800, 5160, 6160, 9880 (over 10 s); between them blocks carry no matrices. At 0, 4800
   and 5160 the extension is on and the coefficients that carry the object start at base 0 or a small base with a delta (e.g. out 2: rows 3 to 10 base 0, delta 827, widths 11 to 14);
   at 4640, 6160 and 9880 it is off and the bases are fresh values. This is the ramp/slope the decoder session solved. For my plan it says sections are hundreds to
   thousands of samples apart (not 480), so `section_samples` should be a parameter in the thousands for slow moves, with the slope fitting the longer line.
3. **The bed matrices are dense, not just `[I | G]`.** Every first section has an `out 0` matrix of the form row 0 less about 0.316 (`ctl_b`, -5181/16384 = -1/sqrt(10)) or 0.3015
   (`ctl_e`, -4940/16384 = -1/sqrt(11)) of every other row from 3 (or 2) up, and `out 11` matrices with `f_shift` 1 or 2 that add a multiple of row 0 (2.16 in `ctl_b`). With nothing in the
   other rows these cancel the encoder's dither (as ERRATA 69 found in `z_short`); with two objects (`ctl_c`) ten matrices mix the bed rows with each other and with the coded rows (out 2 has
   coefficients on rows 0, 1, 2, 4, 5, 6, 7, 8, 9, 10 and 11, the same -2774 on rows 8 to 10). So DEE's "bed" is a decorrelated basis, and `[I | G]` is the simplest member of it, not DEE's own form.
4. **A static object can sit on a bed row with no coded row at all** (`ctl_b`: matrix channel 1 carries the 1 kHz object at +-838,880, no matrix reads or writes channel 1, the coded rows 2 to 10 are
   silent): the anchored case of section 2 step 2 (an object at a speaker position is that element, unchanged).
5. **An object can be spread over several elements** (`ctl_c`, objects 3 at 1 kHz and 8 at 3 kHz, both static): channel 0 carries object 3 alone (+-52,430 x 16 = 838,880, output shift 4), and the
   rest of the output energy, which can only be object 8 (+-26,277 after the shift), is on channels 7 (20,097), 4 (10,384), 6 (6,430), 2 (6,393), 11 (3,849), 5 (1,823) and 1 (817): shares of 0.77, 0.40, 0.24, 0.24, 0.15, 0.07 and 0.03 of its amplitude, whose squares add to
   about 0.89. That is an amplitude pan of one object over seven elements, which my rule (one element per object, section 2 step 3) never does. It is the one finding that argues against the hard assignment; whether
   it is because the position is between elements, because both objects share the stream, or because 7 of the 12 are fixed virtual speakers is not settled by these files.
6. The same object 3 is on matrix channel 1 in `ctl_b` and on channel 0 in `ctl_c`: either `ch_assign` differs, or DEE's slot choice depends on the other objects.

Effect on this branch: nothing is changed in code. Question 3 is partly answered (anchored form supported, DEE's own form is richer); a new question 10 is open.

### Second reading, with `ch_assign` and the objects' positions (analysis session, 2026-11-01; PROVISIONAL)

`ch_assign` of unit 0 (`ch_assign[m]` = output channel of matrix channel m): `ctl_b` [3,1,0,4,5,6,7,8,9,10,11,2], `ctl_c` [4,8,1,0,7,6,2,3,9,10,11,5], `ctl_e` [2,0,3,4,5,6,7,8,9,10,11,1]
(later restart headers not checked). Positions (DAMF x -1 left to 1 right, y -1 rear to 1 front, z 0 to 1): `ctl_b` and `ctl_c` object 3 at [1, 0.85, 0], size 0; `ctl_c` object 8 at [-1, 0.5, 1],
size 0.15, decorrelation on; `ctl_e` object 3 from [-1, 1, 0] to [1, 1, 0] along the front wall in 480,000 samples, a keyframe every 1,000.

1. **The slope rule holds on DEE data, exactly.** `ctl_e`, matrix out 1, sections at 4800 and 5160 (9 access units apart), `ext_field2` 3: base(4800) + 9 x delta / 8 = base(5160) for every coefficient
   of the row, as raw places: m0 -3907 - 9 x 15120 / 8 = -20917 (5160 has -20917); m3 to m10 2765 + 9 x 4551 / 8 = 7885 (7885); m11 -1196 - 9 x 1992 / 8 = -3437 (-3437). Across a change of `f_shift` it still holds in real
   values (out 0, m1: -5.1423 at 4800 plus 9 x 9793 / 8 / 4096 = -2.4526; 5160 has -20091 / 8192 = -2.4525). Any other `ext_field2` from 0 to 5 misses by thousands. So a section's deltas are the
   path to the next section's base, and DEE sends the next section when it wants a new line.
2. **Those coefficients are not the object's pan.** m0 in out 1 goes from -0.24 to -1.28 (real) in 360 samples while the object moves 0.0015 of the room (x = -1 + 2 s / 480,000). The out 1 and out 0 matrices change on a scale
   of hundreds of samples with a path that takes ten seconds; they carry something the signal drives (the decorrelating basis of the bed and the dither cancellation), not a position. So
   the time constant of DEE's matrix updates is not the object metadata's, and my plan (gains from positions on a section grid) matches DEE's wire form (sections, deltas) but not what DEE puts in it.
3. **The slot order is stream-specific.** The same object 3 at the same position is on matrix channel 1 (output 1) in `ctl_b` and on matrix channel 0 (output 4) in `ctl_c`; the moving object of `ctl_e` is on matrix
   channel 11 (output 1). So "element j is speaker j" is **not** a property of the output order, and the anchoring of section 1 is a choice of this branch, not DEE's slot convention. What the streams do show is that
   a static object at a speaker position (`ctl_b`, [1, 0.85, 0], the right front) can ride one bed row alone at unit gain, and that a moving object rides one coded row (m11 in `ctl_e`) with the bed rows
   holding a time-varying mix of it.
4. **The spread of object 8 in `ctl_c`** (seven elements) can be size 0.15 plus decorrelation on rather than a pan; its position [-1, 0.5, 1] is the left wall at the ceiling, so the squares-add-to-0.89 pattern is
   not evidence that DEE splits a point object. Question 10 stays open but its recommendation weakens: do not add a split until a point object (size 0) at a place between elements has been seen.

Effect on this branch: none in code. `RenderSection`'s begin/end gains map to DEE's wire form (a section, deltas to the next base) with `ext_field2` 3 as DEE's choice for 9 units.

### Third reading: the static point objects `ctl_i` and `ctl_j` (analysis session, lab-out30; PROVISIONAL)

`ctl_i` (object 3 at DAMF [-1, 0.5, 0], size 0): ch_assign [2,0,3,4,...,11,1]; output on matrix channel 11 alone (+-838,880); coded rows m0 +-322,984, m2 +-774,180, m11 +-9,488. `ctl_j` (object 3 at [0.5, 0.3, 0]):
ch_assign [1,3,0,4,...,11,2]; output on matrix channel 0 alone (+-209,720 = 838,861 / 4, output shift 2, quantiser step 2 on m0); coded rows m0 +-37,904, m1 +-206,257, m11 +-49,446. One section at 0, no extension, in the first 5,120 samples of each.

1. **Question 10 is answered for a point object: one element.** In `ctl_b`, `ctl_e`, `ctl_i` and `ctl_j` (every case with one size-0 object) exactly one output channel is non-zero, and the object is on **output 1** in all four
   (`ch_assign[m] = 1` for m = 1, 11, 11, 0), wherever it is. So the hard one-element-per-object assignment is what DEE does for a lone point object, and the output slot is not a function of the position.
   The seven-element spread of `ctl_c` object 8 (size 0.15, decorrelation on) is therefore a size/decorrelation effect, not a pan; the split is not added.
2. **The bed shares of a point object are unit-power across two or three rows.** `ctl_i`: 0.385 and 0.923 of the object on m0 and m2 (squares 1.00). `ctl_j`: 0.181, 0.983 and 0.236 on m0, m1 and m11 (squares 1.06). `ctl_b`: all of it on one row (m1).
3. **DEE's pan is not my pan.** With the room mapped as x_room = (x + 1) / 2, y_room = (1 - y) / 2, my panner gives `ctl_b` R 0.89 + Rs 0.46, `ctl_i` L 0.63 + Ls 0.78 and `ctl_j` Rs 0.69 + R 0.73, against DEE's one row for
   `ctl_b` (a position 0.15 from the front-right corner, which looks like a snap to the corner speaker) and a 0.39 / 0.92 split for `ctl_i` where the position is half way up the left wall. So DEE pans in the room
   (cuboid) and not on the azimuth ring, at least near walls and corners. The matrix channel to speaker mapping is not known (m0, m2 for `ctl_i` are not L and Ls in any order I can name), so this is a comparison of the shape of
   the shares, not of the speakers. The test only holds my panner to `libs/render`'s, which is the decode-side panner, and nothing here claims DEE's bed.
4. **The matrix side's interface is compatible.** `feature/truehd-atmos-encoder-matrices` (24d14585d, 51c839c00) takes `object_paths`: per object from channel 8 up, knots of (sample, gain into each of the 8 bed rows in units of 2^-16), linear between knots.
   My plan maps onto that one for one: object 8 + k is free element k, a knot at every section boundary with gain[row][k] * 65536. I have not merged that branch or written the adapter (it needs their public header); that is the next step once both are merged.

### Fourth reading: the 7.1 presentation of the controls (analysis session, 8ch decode, `ctl_b` to `ctl_l`; PROVISIONAL)

The analysis session decoded each control to the 8 channel presentation (L R C LFE Ls Rs Lrs Rrs, the decoder's own labels) and gave whole-file or 4,800-sample-window RMS per channel as a ratio of the 1 kHz sine's RMS (593,164). Statistics only.

1. **A lone object's shares on a wall are a sine/cosine in the position along the wall.** `ctl_e` (front wall, y = 1, x from -1 to 1): at x = -0.8, -0.4, 0.4, 0.8 the shares of (L, C) or (C, R) are (0.950, 0.314), (0.579, 0.815),
   (0.588, 0.809) and (0.954, 0.300), against cos and sin of t x 90 degrees for t = 0.2, 0.6 (0.951 / 0.309, 0.588 / 0.809): within 0.01. `ctl_k` (left wall, y from 1 to -1 through the side-left speaker at y = 0): the shares of (L, Ls) and
   (Ls, Lrs), normalised, are within 0.02 of the same law. The level is 1.000 on the front wall and 0.97 to 0.84 on the left wall: the surrounds (Ls, Lrs) carry a gain below 1 (about 0.87 for Ls alone, 0.83 to 0.85 for Lrs; not fitted here).
2. **The azimuth-ring pan of `libs/render` is off by up to 0.2 from those shares** (e.g. at x = -0.8 on the front wall it puts 0.19 on Ls where the stream has 0.31 on C, because the ring pans by azimuth from the middle of the room and the
   speakers are at the wall). `PanLaw::kWall` (new, opt-in, ring stays the default) projects the place from the middle onto the wall and pans by that law: worst deviation 0.014 over the nine positions of the two walls, in `test_render_bed.cpp`.
3. **Inside the room DEE spreads over three or four speakers** (`ctl_l`, y = 0.5 forward of the middle, x from -0.5 to 0.5: at x = 0 the shares are C 0.68, Ls 0.46, Rs 0.46, nothing on L, R or the rears; at x = -0.4 L 0.38, C 0.56, Ls 0.58, Rs 0.30;
   `ctl_j` [0.5, 0.3]: R 0.29, C 0.27, Ls 0.30, Rs 0.75), with a level of 0.82 to 0.87. Triangle barycentric coordinates fit the middle point of `ctl_l` (C 2 : Ls 1 : Rs 1 in power, observed 2.2 : 1 : 1) and not `ctl_j` or the other `ctl_l` points. The wall
   projection is therefore only a wall law; the interior is not fitted. `ctl_b` ([1, 0.85]: R 0.945, Rs 0.245) is the wall law within 0.03 as well, which corrects the earlier reading that DEE puts `ctl_b` on one row: that was the matrix channel, not the speaker.
4. **Matrix rows are not speakers.** The same object is the largest in R in `ctl_b`, in L plus Ls in `ctl_i` (0.68 and 0.64), while its coded matrix channels are 1, (0, 2, 11) and so on. The decoder's own labels for the presentation are the only
   speaker names the data gives; the mapping of matrix channels 0 to 7 to them is still not known.
5. **`ctl_k` and `ctl_l` matrix sections** (analysis session): 193 sections (143 with the extension) and 299 (224), unevenly spaced, in bursts of eight to twelve sections 120 to 160 samples apart around some moments (`ctl_l` 217080 to 217880, 233640 to 234760, 249000 to 250000, ...) and a single section every 5,120 samples or so otherwise;
   no section at the crossing instant (240000) in either. `ctl_k` changes its matrix set from {out 2, out 0} to {out 1, out 0} at 238080 with the extension off, shortly before the crossing of the side-left position. So DEE's update times are driven by something other than the object's position (the bursts follow the signal), as in the second reading.

Effect on the branch (this commit): the matrices branch (`feature/truehd-atmos-encoder-matrices`, 24d14585d and 51c839c00) is merged in; its `object_paths` is the hook (a caller-made chain is not accepted), and `object_paths_of(RenderPlan)` writes my plan as paths (a knot at every boundary,
gains at 2^-16, clamped to +-1.98). The chain-level tests use `build`-less timelines (one block of the bed-row matrices) with `invert_object_timeline` and `apply_object_timeline`. The crossing is written with the plan's paths in 12, 14 and 16 element streams: the elements decode back sample for sample and the 7.1 bed is -43.0 dB from the intended one.

## 4. Tests, and what each guards

See the test names; the ones that would pass for a wrong render are the closeness tests, with thresholds set from the measured values (section 6).

## 5. Interface with the other branches

- `render_plan.hpp` is the whole contract: `RenderPlan { elements, sections[ {start, samples, begin, end} ] }`, gains as `double[8][8]` (bed row, free element).
- `bed_mix_matrices` gives the 8 bed-row matrices only. The flip of row 8 and the LSB bypass of `make_object_bed_chain` are the matrix side's.
- `ClusterResult::signals` are what substream 3 outputs (the element planes); `placements` are what the object metadata writer needs.
- The test target compiles `object_bed.cpp`, `render_bed.cpp` and `cluster_elements.cpp` itself (as it does `coding.cpp`) because the library hides them in a shared build.
- Not touched: `object_bed.*`, `encoder.cpp`, anything under `src/decoder/`, the lab, ERRATA, SUBSTREAM3, CHANGELOG.

## 6. Measured

Release build, MSVC, 2026-11-01. Error is 10 log10 of the error power over the intended bed's power, all eight rows.

| Scene | Result |
|---|---|
| two objects crossing in front of the listener over 0.5 s (A 0.2,0.25 to 0.8,0.45; B the mirror), 480-sample sections, real-valued gains | -48.7 dB |
| the same, gains held at each section's start instead of ramped (one object) | -22.5 dB |
| the same crossing through the 8 bed-row matrices, gains frozen per 40-sample block, run backwards and forwards in integers | -46.3 dB; the elements come back sample for sample |
| the same crossing over 0.1 s (six times faster), real-valued gains | -22.1 dB (480-sample sections are too coarse for a fast move) |
| a path through the middle of the room | not followable: the direction there turns half a circle (-11.8 dB) |

12, 14 and 16 elements from 8 pinned bed objects and 10 moving objects: the element planes go through `kBedMatrices` with the encoder's own
decode on (`verify`) and decode back sample for sample with every restart interval's check passing; the bed there is the writer's fixed render, not mine.

Mutation checks (one change at a time, rebuilt, `[render]` run): killed - L/R swapped in the ring, bed-matrix sign, quantiser clamp, a section's end
taken from its own begin (no ramp), no snap hysteresis, no handover, plain instead of energy-weighted centroid, no clip, placement ignoring the section
before / after a boundary, pinned objects not pinned, object gains not applied, merge seeds by index. A literal "no ramp" does not compile (an unused
variable) and is covered by the section-end mutation. An equivalent mutant: skipping the LFE row in the snap search changes nothing, because the LFE's
wall position is the centre speaker's and the centre speaker is tried first.

## 7. Questions

1. **Layering.** `libs/truehd` links `base` alone, so it cannot call `libs/render` or `libs/objects`; the render is its own 7.1 panner, held to `libs/render`'s by a test. Recommendation: keep it so; the forge front end, which has both, converts OAMD positions to `ClusterKey`.
2. **Brace style.** The brief says Allman braces; the repository's `.clang-format` (Google, 4 spaces, 100 columns) and every file around mine are attached braces, and the precheck runs clang-format. I followed the repository. Recommendation: keep.
3. **Which elements does DEE anchor to speakers?** I assumed 0 to 7. For `ctl_c` (two objects): which coded rows carry each object's tone, which output rows, and do bed rows hold anything but the downmix? Ask the analysis session; the answer fixes section 1 or retires it.
4. **(Answered: `object_paths` is the hook; see the fourth reading.) A hook for a caller-made chain.** `Encoder` takes the element planes today and applies its fixed bed render (`make_object_bed_chain`). My plan reaches a stream only when the matrix side takes `RenderPlan` and `bed_mix_matrices`. Recommendation: the matrices branch adds `EncoderConfig` a pointer to a plan; until then streams are checked at chain level (`invert_object_chain` and `apply_object_chain`).
5. **Ramp semantic.** I give begin and end gains per section and leave the wire form to the stream session. PROVISIONAL (ERRATA, coefficient ramp). Recommendation: ramp over the block, i / block, as ERRATA's question 65 says.
6. **Headroom.** A bed row is an element plus a share of others and can leave 24 bits; `kBedMatrices` solves that by lowering the whole mix a step per restart interval, which steps the gains at the interval boundary. Recommendation: a smooth reduction envelope (the minimum of neighbouring sections, linearly between), computed where the elements are known; not done here.
7. **LFE.** `lfe_send` of a dynamic object and the LFE object's filtering are not handled; the LFE is whatever is pinned to row 3.
8. **Height.** 7.1.2's top-middle pair are ordinary free elements here; the bed they make has no height. Nothing says DEE does otherwise.
9. **Section length.** 480 samples. DEE's metadata cadence is 1,536 (38.4 units, WP1 status); a longer section means a longer straight line between boundaries and a larger panning error, a shorter one more matrix sections on the wire.
10. **(Answered for a point object, see the third reading: one element; recommendation withdrawn.) Split an object over elements?** Item 5 above suggests DEE pans an object over several elements. Recommendation: add an optional second-nearest-element split to `cluster_elements` (gain-weighted, power-preserving) only after the analysis session gives `ch_assign` and the objects' positions for `ctl_c`, so the split can be compared with the shares in item 5.
11. **Pan in the room, not on the ring?** DEE's shares for `ctl_b`, `ctl_i` and `ctl_j` differ from the azimuth ring's (third reading, item 3). Recommendation: after `ctl_k` and `ctl_l` (a wall path and an interior path), fit a cuboid pan (speakers at the room's corners and wall midpoints, power-normalised between the nearest) to the shares, and replace `pan_to_bed` only if it explains all four static and moving cases; keep the `libs/render` agreement test for the ring either way, since that is what the decoder side renders.
12. **Switch the default to `PanLaw::kWall`?** It fits DEE's walls (0.014) and not the ring's decode-side law, and the interior is open (item 3 of the fourth reading). Recommendation: keep `kRing` the default until the interior is fitted or the owner says the legacy bed should follow DEE; callers that want DEE's walls pass `kWall` to `make_render_plan`.
