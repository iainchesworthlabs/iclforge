# Moving objects in the encoder's object substream

Branch `feature/truehd-atmos-encoder-matrices` (from `feature/truehd-atmos-bed` 56b4346). Owner of the
files: `libs/truehd/src/encoder/object_bed.{hpp,cpp}`, the object cases of
`tools/references/truehd_writer.py`, `tools/checks/verify_truehd_object_bed.py`.

**Agreement with this library's decoder is not agreement with Dolby's.** Every statement below that
says a stream "decodes" means this library's decoder and the Python reference (`truehd_syntax.py`),
which read the sections as ERRATA 69 and `SUBSTREAM3.md` section 2 describe them. No decoder outside
the repository reads restart sync 0x31EC, and the owner's discs and DEE's encodes have not been run
through anything this branch writes. The extension is a reading of one controlled encode (`ctl_c`),
marked provisional in `SUBSTREAM3.md` section 6.

## What the encoder writes now

`ObjectLayout::kBedMatrices` took one static chain for the whole stream (ERRATA 70). The chain is now a
*timeline*: for each block of a restart interval, the matrices in force and the section that block's
header sends to get there. `EncoderConfig` gained (all `kBedMatrices` only):

| Field | Meaning |
|---|---|
| `object_paths` | per object (channel 8 up) a path of knots; a knot is a sample of the stream and the object's gain into each of the 8 bed rows, in units of 2^-16. Linear between knots, holding outside them, a repeated sample is a step. Empty: the fixed render, as before |
| `object_update_units` | without a ramp, new coefficients every this many access units (counted from the stream's start, so where a path steps does not depend on where the restarts fall) |
| `object_ramp` | `kNone`, `kEnd`, `kStart`: the provisional extension (below). Needs an update at every block |
| `object_drop_idle_matrices` | a mixing matrix whose coefficients are all 0 in a block is left out of that block's list |
| `object_split_mixes` | one mixing matrix for each (object, bed row) pair instead of one for each bed row, so that a stream can carry the section's 16 matrices |
| `object_bypass_bits` | 1 to 3 bits a sample for the LSB bypass matrix (its own coefficient is 2^n, `field_2b` is n) |

Without `object_paths` (and with the other fields at their defaults) the stream is byte for byte what the
base commit writes: six configurations of the encode tool (12/16 objects, noise shifts 0 to 5, bypass
on and off, restart intervals of 8 and 128 units) were compared with a tool built from 56b4346.

**The mix.** Object k's mixing coefficient on bed row r is minus its gain, rounded, at 2^-12
(`frac_bits` 12, `f_shift` 0); the matrix's own coefficient on the bed row is exactly +1.0 (primitive).
The order of the list is: the sign flip of row 8, the bypass matrix, then the mixing matrices in the
order of the first object that reaches them (the same order as the fixed render when nothing moves).
Every integer rule (rounding halves away from zero, anchors, widths) is the same in
`object_bed.cpp` and in `truehd_writer.py`; the paths are integers on purpose.

**The sections.** `plan_object_section(before, after)` sends nothing when the decoder already holds
`after`, else the cheapest of: A clear with new widths/deltas (the set stands), A set with B clear
(new coefficients on the headers that stand), or new headers (B set; widths follow the matrix at the
same place, so C2 is sent whenever they differ). A block whose matrices did not change has no matrix
field at all. The planner checks its own output with a model of the decoder's rules
(`apply_object_section`), and the tests hold that model to the real reader.
`write_object_matrices` (`core/syntax.cpp`) now writes all of these forms (`MatrixSet::form`, writer
only) and writes `field_2b` as the bypass bit count (it wrote `field_2b | 1`, which sent 3 for 2).

**The lossless check.** The fold shifts a sample by its matrix channel modulo 8 (ERRATA 69). The
encoder already did; `test_encoder_object_motion.cpp` now recomputes the fold of every interval from the
decoded objects, compares it with the byte each restart header stores, and requires that the plain
shift by the channel would not have matched. Replacing the modulo in `lossless_fold_step` by the plain
shift makes that test fail (checked by hand; 48 assertions).

## The extension (provisional)

Reading used: a delta is the change of the coefficient across the block, in eighths of the raw value's
last place; at sample `i` of a block of `size` samples the coefficient is
`raw + delta / 8 * (i + 1) / size` (`kEnd`) or `* i / size` (`kStart`). Both fit `ctl_c` equally
(`SUBSTREAM3.md` section 6). The decoder reads this today only under
`DecoderLab::object_extension` 4 (end) or 5 (start) with `object_extension_fixed` 19; the encoder's own
verifying decode, the tests and `object_plan_tool` set those. **The decoder's rule in force is
different** (a flat refinement of 2^-(w+1) for the whole block), so a stream written with `object_ramp`
fails the lossless check under a default decoder. That is deliberate and documented in the header; when
the decoder session settles what a delta means the encoder's model (`matrix_value` in `object_bed.cpp`
and `moving_value` in the writer) is the one place to change.

One update per block, as agreed: the generator never relies on what a delta means over several blocks.
Anchors: `kEnd` takes raw from the path one sample before the block and the end value at the block's
last sample, so a path that is a straight line is followed exactly (to half a place of rounding); a
block that starts on a knot where the path turns is off by up to twice the path's step a sample.
`kStart` anchors at the block's first sample and one past its last. A step inside a ramped block is
smeared across the block (no test uses a ramp with steps for the bed).

Widths: for each matrix the smallest width that holds every delta of the interval (1 to 15), sent once
with the restart block's section; the 2 reserved bits after each width are written 0.

## Checked how

* `libs/truehd/tests/encoder/test_encoder_object_motion.cpp` (15 cases): hand-worked trajectories,
  the ramp's arithmetic as literals (`25600 + 148 * (i + 1)`), the section forms against the real
  reader over 300 random lists, 16 matrices that change from block to block with a ramp, noise and
  2 bypass bits (backwards then forwards exact), seven stream configurations (static hold, held for 3
  units, steps, both ramps, noise, bypass 2 and 3 bits, idle matrices dropped) that decode exactly with
  every lossless check passing and a bed within a stated tolerance of the paths', chunking
  independence, paths equal to the fixed render equal its bytes, refusals.
* Mutations that make it fail (done by hand, then restored): the ramp scale 3 to 4 bits (6 cases fail),
  the generator's delta scale (4 cases fail), the fold shift (1 case, 48 assertions).
* `tools/checks/verify_truehd_object_bed.py --encoder ... --plan-tool ...`: the reference decoder reads
  the encoder tool's streams to the objects (static, slow paths, step paths, both ramps) and the bed is
  inside the tolerance; and four streams made by `truehd_writer.py generate-moving` (12 objects with
  the ramp, 14 objects with the ramp from the first sample and a changing list, 16 objects with 16
  matrices, 16 objects whose list shrinks and grows) are planned by the C++ planner with the same
  sections bit for bit, the same rows and bypass bits, and read by this library's decoder to the same
  objects.
* The existing hand-built streams regenerate unchanged. `iclforge-truehd-tests` passes entirely
  (188 cases before this file, 15 added).

## Not done, and questions

1. **What a delta means over several blocks** (ERRATA question 65). Not needed here (one update per
   block). Recommendation: leave it to the controls `ctl_e` to `ctl_h`; if a delta turns out to span
   blocks the generator can send fewer updates, and `plan_object_section` already sends the cheapest
   form.
2. **Should the decoder's rule become the ramp?** Recommendation: only after a control shows it; then
   remove the lab settings from the encoder's verifier (`Impl::Impl`) and the tests' `decoder_for`.
3. **`kEnd` or `kStart`?** Both fit `ctl_c`. Recommendation: `kEnd` (exact on a straight path, and
   the delta then reaches the value of the next section). A control with a path that changes slope
   inside a block would separate them.
4. **The 2 reserved bits after each width** are written 0. DEE's values are unknown (the hand-built
   stream sends 0 to 3 and the decoder ignores them).
5. **The own coefficient of a bypass matrix with 2 or 3 bits** is written 2^n (4.0, 8.0). Only n = 1 has
   a stream behind it (`ctl_c`, 2.0); for n = 2 the block data of `soll_e12` unit 11319 says the bits
   are read, not what the coefficient is. Provisional.
6. **Noise numbering when the list changes.** The decoder numbers the matrices from the list in force,
   so dropping a matrix renumbers the others' noise. The encoder follows (the tests hold it); whether
   DEE does is unknown.
7. **Dolby's matrices are not these.** The mix is still a placeholder render (ERRATA 70): the matrices
   rebuild the objects from a bed that is a function of them; a real renderer's bed would be the
   input. Nothing here reads object metadata (`forge truehd-atmos` still writes `kIndependent`).
8. **A step inside a ramped block** is smeared (above). Recommendation: place a step on a block boundary
   and use `object_update_units` without a ramp for steps, as the tests do.
9. **Style.** The brief asks for Allman braces, CAPITAL_SNAKE constants and `m_PascalCase` members; the
   repository's `.clang-format` (Google, 4 spaces) and the code around these files use attached
   braces, `kPascalCase` constants and `name_` members, so the new code follows the surroundings.
   The brief also names `origin`; this checkout's remote is `github`.

## Files outside the owned list that changed

`libs/truehd/include/iclforge/truehd/core/syntax.hpp` and `src/core/syntax.cpp` (writer forms, the
`field_2b` fix; the reader is untouched), `encoder/encoder.hpp` and `src/encoder/encoder.cpp` (config,
the timeline in place of the chain), `encoder/object_paths.hpp` (new, public), `tests/CMakeLists.txt`,
`tests/tools/encode_tool.cpp` (motion presets), `tests/tools/object_plan_tool.cpp` (new),
`tools/references/truehd_syntax.py` (an opt-in `extension` argument; the default reading is unchanged).
Not touched: `libs/truehd/src/decoder/`, `ss3_lab.cpp`, `ERRATA.md`, `SUBSTREAM3.md`, `CHANGELOG.md`.

## Running it

    cmake --build BUILD --target iclforge-truehd-tests iclforge-truehd-encode-tool iclforge-truehd-object-plan-tool
    BUILD/bin/iclforge-truehd-tests "[object-motion]"
    python3 tools/checks/verify_truehd_object_bed.py --encoder BUILD/bin/iclforge-truehd-encode-tool \
        --plan-tool BUILD/bin/iclforge-truehd-object-plan-tool
    python3 -m unittest tools/checks/test_verify_truehd_object_bed.py
