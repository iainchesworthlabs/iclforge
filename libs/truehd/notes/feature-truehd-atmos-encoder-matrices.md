# Moving objects in the encoder's object substream

Branch `feature/truehd-atmos-encoder-matrices`. First written against `feature/truehd-atmos-bed`
56b4346, then merged with 522d9b934 and reworked on the slope rule the decoder session found
(`SUBSTREAM3.md` sections 2 and 3, ERRATA 69's last paragraph). Owner of the files:
`libs/truehd/src/encoder/object_bed.{hpp,cpp}`, the object cases of `tools/references/truehd_writer.py`,
`tools/checks/verify_truehd_object_bed.py`.

**Agreement with this library's decoder is not agreement with Dolby's.** Every statement below that
says a stream "decodes" means this library's decoder and the Python reference (`truehd_syntax.py`),
which read the sections as ERRATA 69 and `SUBSTREAM3.md` describe them. No decoder outside the
repository reads restart sync 0x31EC, and the owner's discs and DEE's encodes have not been run
through anything this branch writes. The slope rule rests on the owner's corpus and the decoder
session's controls, not on anything written here.

## What the encoder writes now

`ObjectLayout::kBedMatrices` took one static chain for the whole stream (ERRATA 70). The chain is now a
*timeline*: for each block of a restart interval, the matrices in force, the decoder's running state
for them at the block's first sample, and the section that block's header sends to get there.
`EncoderConfig` gained (all `kBedMatrices` only):

| Field | Meaning |
|---|---|
| `object_paths` | per object (channel 8 up) a path of knots; a knot is a sample of the stream and the object's gain into each of the 8 bed rows, in units of 2^-16. Linear between knots, holding outside them, a repeated sample is a step. Empty: the fixed render, as before |
| `object_update_units` | without slopes, new coefficients every this many access units (counted from the stream's start, so where a path steps does not depend on where the restarts fall) |
| `object_extension` | `kNone` or `kSlopes`: the extension (below). Needs an update at every unit |
| `object_drop_idle_matrices` | a mixing matrix whose coefficients are all 0 in a unit is left out of that unit's list |
| `object_split_mixes` | one mixing matrix for each (object, bed row) pair instead of one for each bed row, so that a stream can carry the section's 16 matrices |
| `object_bypass_bits` | 1 to 3 bits a sample for the LSB bypass matrix (its own coefficient is 2^n, `field_2b` is n) |

Without `object_paths` (and with the other fields at their defaults) the stream is byte for byte what
56b4346 writes (checked again after the merge: four configurations of the encode tool against a tool
built from 56b4346, bypass and noise included).

**The mix.** Object k's mixing coefficient on bed row r is minus its gain, rounded, at 2^-12
(`frac_bits` 12, `f_shift` 0); the matrix's own coefficient on the bed row is exactly +1.0 (primitive,
and never given a slope). The list is: the sign flip of row 8, the bypass matrix, then the mixing
matrices in the order of the first object that reaches them (the fixed render's order when nothing
moves). The rounding and every comparison of the generator are integer, and written again in
`truehd_writer.py` (`motion_blocks`).

**The slopes.** The decoder keeps each coefficient at 2^-18 and moves it by `delta / 2^ext_field2` raw
places in every access unit while the extension stands; inside a unit it adds the slopes' share as a
ramp (0 at the unit's first sample, 6552 a sample at 48 kHz). The generator follows the decoder's
state unit by unit (`ObjectRun`, the same fields as the kernel's `ObjectMatrixState`):

* at each unit start it compares each coefficient with the path (tolerances in units of 2^-18:
  jump 32, half a raw place at 12 fractional bits; slope 16);
* a coefficient more than the jump tolerance off is sent again (A set, B clear), as the raw value
  nearest the path, and every other coefficient as the raw value nearest where it has got to;
* a slope whose coefficient would end the unit more than the slope tolerance from the path's is
  replaced (A clear, A2 and B2 set, C2 clear) by `round(move / 8)`; a move of more than a quarter of
  a gain in one unit is a step of the path, not a slope, and gets delta 0 and then a jump;
* otherwise the unit has no section: a straight path is one section and then plain blocks;
* a list that changes (a matrix dropped or back) sends new headers, new coefficients and new slopes
  together (the slopes of the matrix at the same place would otherwise be another matrix's); C2 is
  sent when the widths or scales differ from the matrix at that place;
* the width of each matrix's slopes is fixed for the interval from the path (the largest move in a
  unit that is not a step, plus what a jump leaves); `ext_field2` is 3, the finest the two bits give
  (a delta is an eighth of a raw place a unit). A path that does not move gets width 0 and no
  slope at all.

With the extension off, A2 is clear and every slope is cleared; a coefficient holds from one update to the next, as
before. `apply_object_section` is the encoder's model of the decoder's `apply_block_header`; the
generator checks that a section gives the state it meant, and the tests hold the model's forms to the
real reader (`every form of section is read back as written`). `write_object_matrices`
(`core/syntax.cpp`) writes all forms (`MatrixSet::form`, writer only) and writes `field_2b` as the
bypass bit count (it wrote `field_2b | 1`, which sent 3 for 2).

**The lossless check.** The fold shifts a sample by its matrix channel modulo 8 (ERRATA 69). The
encoder already did; `test_encoder_object_motion.cpp` recomputes the fold of every interval from the
decoded objects, compares it with the byte each restart header stores, and requires that the plain
shift by the channel would not have matched. Replacing the modulo in `lossless_fold_step` by the plain
shift makes that test fail (checked by hand).

## Checked how

* `libs/truehd/tests/encoder/test_encoder_object_motion.cpp` (16 cases): hand-worked trajectory (the
  first slope 328, replaced by 326 in unit 6, the running coefficient at each unit start), the slope
  arithmetic as literals (`25600`, `25631`, `25663`, `25919`, `26847`, then `26880` in the next unit),
  the section forms as bit strings and over 400 random sections against the real reader, the model of
  the decoder's rules, 16 matrices (14 mixing matrices from split mixes) run backwards and forwards
  with and without slopes, a list that loses a matrix and gets it back, seven stream configurations
  (hold per unit, held for 3 units, steps, slopes, slopes with noise, two bypass bits and idle matrices
  dropped, steps with slopes and three bypass bits) that decode exactly with every lossless check
  passing and a bed within a stated tolerance of the paths', slopes against hold (the slopes keep the
  bed within 0.75 of a place, hold per unit is up to 97 places off), slopes cost fewer bytes than
  new coefficients every unit, chunking independence, paths equal to the fixed render equal its
  bytes (with and without slopes on), refusals.
* Mutations by hand, restored afterwards: the ramp step 6552 changed to 6553 in the encoder's model
  (4 cases fail), the coefficients' move at the end of the unit left out (2 cases fail), the lossless
  fold's modulo 8 replaced by the plain shift in `lossless_fold_step` (the independent fold test
  fails). The model's constants are the decoder's, so a change in one without the other shows in
  every stream test.
* `tools/checks/verify_truehd_object_bed.py --encoder ... --plan-tool ...`: the reference decoder
  reads the encoder tool's streams to the objects (static, slow paths, step paths, slopes) and the bed
  is inside the tolerance; and four streams made by `truehd_writer.py generate-moving` (12 objects
  with slopes, 14 objects with slopes and a changing list, 16 objects with 16 matrices, 16 objects
  whose list shrinks and grows) are planned by the C++ planner with the same sections bit for bit,
  the same rows and bypass bits, and read by this library's decoder to the same objects. While the
  Python writer plans, it holds its own running state to the decoder session's `object_trace` at every
  sample (coefficients, scale, slopes, ramp).
* `iclforge-truehd-ss3-lab` on a stream written with slopes, noise, 2 bypass bits and idle matrices
  dropped: `ss3 ok 32 failed 0`, `ss0-2 ok 96 failed 0`.
* The existing hand-built streams and their manifest regenerate unchanged. `iclforge-truehd-tests`
  passes entirely (207 cases).

## Not done, and questions

1. **How DEE chooses its slopes.** The generator's policy (a section where the path turns or the
   coefficient has drifted; tolerances of half a place and a quarter of a place) is this branch's, not
   a reading of DEE. In `ctl_e` DEE sends the slopes in few blocks with widths 11 to 14 and scales
   that differ by matrix; ours has one scale (3) and a width for the interval. Recommendation: leave
   it; the stream decodes and is smaller than sending coefficients every unit.
2. **A step on a unit boundary** is a jump of the coefficient (a new raw value) and the slope of the
   unit before is 0. DEE's way with a point object that jumps is not known (`ctl_i`, `ctl_j` may say).
3. **The own coefficient of a bypass matrix with 2 or 3 bits** is written 2^n (4.0, 8.0). Only n = 1
   has a stream behind it (`ctl_c`, 2.0); for n = 2 the block data of `soll_e12` unit 11319 says the
   bits are read, not what the coefficient is. Provisional.
4. **Noise numbering when the list changes.** The decoder numbers the matrices from the list in
   force, so dropping a matrix renumbers the others' noise. The encoder follows (the tests hold it);
   whether DEE does is unknown.
5. **Dolby's matrices are not these.** The mix is still a placeholder render (ERRATA 70); the render
   session's element layout and gains (`render_plan.hpp`) can be given as `object_paths`. Nothing here
   reads object metadata (`forge truehd-atmos` still writes `kIndependent`).
6. **Slopes and the 44.1 / 48 kHz restriction.** The ramp step depends on the unit length
   (`0x666 >> r`); the encoder only writes 48 kHz Atmos, so the other steps are not exercised.
7. **Style.** The brief asks for Allman braces, CAPITAL_SNAKE constants and `m_PascalCase` members; the
   repository's `.clang-format` (Google, 4 spaces) and the code around these files use attached
   braces, `kPascalCase` constants and `name_` members, so the new code follows the surroundings.
   The brief also names `origin`; this checkout's remote is `github`.

## Files outside the owned list that changed

`libs/truehd/include/iclforge/truehd/core/syntax.hpp` and `src/core/syntax.cpp` (writer forms, the
`field_2b` fix; the reader is untouched), `encoder/encoder.hpp` and `src/encoder/encoder.cpp` (config,
the timeline in place of the chain), `encoder/object_paths.hpp` (new, public), `tests/CMakeLists.txt`,
`tests/tools/encode_tool.cpp` (motion presets), `tests/tools/object_plan_tool.cpp` (new),
`tools/references/truehd_writer.py` (the moving-object code, a `bypass` bit count in the section
state and `Substream.object_bypass`; the decoder session's sections are otherwise untouched).
`invert_object_chain` and `apply_object_chain` stay as thin wrappers over the timeline, for the render
session's tests. Not touched: `libs/truehd/src/decoder/`, `ss3_lab.cpp`, `ERRATA.md`, `SUBSTREAM3.md`,
`CHANGELOG.md`, `truehd_syntax.py`.

## Running it

    cmake --build BUILD --target iclforge-truehd-tests iclforge-truehd-encode-tool iclforge-truehd-object-plan-tool
    BUILD/bin/iclforge-truehd-tests "[object-motion]"
    python3 tools/checks/verify_truehd_object_bed.py --encoder BUILD/bin/iclforge-truehd-encode-tool \
        --plan-tool BUILD/bin/iclforge-truehd-object-plan-tool
    python3 -m unittest tools/checks/test_verify_truehd_object_bed.py
