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
4. **A hook for a caller-made chain.** `Encoder` takes the element planes today and applies its fixed bed render (`make_object_bed_chain`). My plan reaches a stream only when the matrix side takes `RenderPlan` and `bed_mix_matrices`. Recommendation: the matrices branch adds `EncoderConfig` a pointer to a plan; until then streams are checked at chain level (`invert_object_chain` and `apply_object_chain`).
5. **Ramp semantic.** I give begin and end gains per section and leave the wire form to the stream session. PROVISIONAL (ERRATA, coefficient ramp). Recommendation: ramp over the block, i / block, as ERRATA's question 65 says.
6. **Headroom.** A bed row is an element plus a share of others and can leave 24 bits; `kBedMatrices` solves that by lowering the whole mix a step per restart interval, which steps the gains at the interval boundary. Recommendation: a smooth reduction envelope (the minimum of neighbouring sections, linearly between), computed where the elements are known; not done here.
7. **LFE.** `lfe_send` of a dynamic object and the LFE object's filtering are not handled; the LFE is whatever is pinned to row 3.
8. **Height.** 7.1.2's top-middle pair are ordinary free elements here; the bed they make has no height. Nothing says DEE does otherwise.
9. **Section length.** 480 samples. DEE's metadata cadence is 1,536 (38.4 units, WP1 status); a longer section means a longer straight line between boundaries and a larger panning error, a shorter one more matrix sections on the wire.
