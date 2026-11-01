# feature/truehd-atmos-encoder-stream: rate control, restart and major sync layout, OAMD packaging

Owner of this branch: the encoder session for rate control, access-unit sizing and the container.
Base: `feature/truehd-atmos-bed` at 56b4346 (merged forward to the tip noted at the end).
Nothing here is confirmed by a stream of the owner's; every rule below is PROVISIONAL unless it says
otherwise, and "held by" names the test that fails when the rule is broken.

## What was built

| Piece | Where | Held by |
|---|---|---|
| The arrival model (`RateModel`, `RateStep`, headroom) and the per-interval figures (`IntervalRate`, `IntervalMeter`) | `include/iclforge/truehd/encoder/ratectl.hpp`, `src/encoder/ratectl_model.cpp` | `test_ratectl.cpp`: hand-worked numbers for units of 1875 bytes (40 samples at 18 Mbit/s), a late unit at -768 samples, microseconds rounded down, a backlog that drains |
| `analyse_rate()`: the same figures read back out of a stream, intervals cut at the units whose first substream carries a restart header | same | `test_ratectl.cpp`: equal, field for field, to what the encoder kept (`Encoder::interval_rates()`), for a stereo stream with a part interval and an Atmos one; late units equal the encoder's violations; another rate re-runs the model |
| `RateControl`, `RateMode::kStrict` / `kShape` on `EncoderConfig` | `encoder.hpp`, `src/encoder/encoder.cpp` (`process_interval`, `code_interval`), `src/encoder/ratectl_shape.{hpp,cpp}` | `test_ratectl.cpp`: strict refuses, shape fits and leaves the 7.1 bed bit-exact, objects within half a step, exact in the quiet start; same bytes whatever the size of the calls; `verify` agrees; both object layouts; shaping stops at the limit |
| `LayoutCadence`, `layout_has_major_sync`, `major_sync_interval_units` | `include/.../layout.hpp`, `src/encoder/layout_cadence.cpp` | `test_layout.cpp`: truth table, refused cadences, units 0/32/64 carry the major sync and every 8th unit restarts, strict decode with the right restart count, a decoder joining at a restart without a major sync waits for the next major sync |
| `layout_seal_segment`, `layout_directory_entry`, `audit_layout` | same | `test_layout.cpp`: a flipped parity byte, a flipped CRC byte, a stream cut inside a unit, one substream's restart flag turned round (with a right check nibble), no major sync in the first unit, and nothing at all are each found; a good Atmos stream has 4 checked segments in every unit |
| OAMD packaging: `oamd_frame_in_unit`, `split_oamd_offset`, `oamd_extra_data`, `layout_frame_extra_data` | `layout.hpp`, `src/encoder/layout_oamd.cpp`; `forge truehd-atmos` now calls them | `test_layout.cpp`: over 100,000 units one frame starts in 2605 of them, in order, none twice, every gap 38 or 39 units, offset below 40; payload bytes, check nibble and parity in the stream; the extra data counts in its unit's size |
| forge: `truehd-bitrate=`, `truehd-rate-mode=`, `truehd-shape-bits=`, `truehd-major-sync=`, `probe detail=rate` (text and `json=1`) | `apps/forge/cli/src/commands/truehd_rate.{hpp,cpp}`, `support.*`, `usage.cpp`, `truehd.cpp`, `truehd_encode.cpp`, `truehd_atmos_encode.cpp`, docs `commands.md` | `test_cli_truehd_rate.cpp`: the probe's table (one row per interval, numbered, 16 units each), the same text twice, JSON equal to `analyse_rate()` of the same bytes, the model at another rate, major sync every 64 units, shape against strict, refused spellings |

`forge probe FILE detail=rate` prints one summary line and one row per restart interval: interval number, first unit, units, kbit/s, peak
kbit/s (largest unit over its own duration), headroom in milliseconds with three decimals, late units. All integers (headroom in whole
microseconds, printed as ms.uuu), so the output does not vary between runs or machines.

### Acceptance (10 s, 2 min, 20 min)

`test_ratectl.cpp` runs the mixed test signal (7.1 bed plus 12 object channels; quiet tones, objects taking a mid tone, 14 or 15 bit
noise, silence, a loud tone, bed noise; six phases of half a second) through `Encoder` with `verify` on, in two configurations, and
feeds every unit to an independent `RateModel`:

- lossless in 18 Mbit/s (objects' noise 14 bits): no late unit, headroom at least 0, no shaped interval;
- shaped to a bitrate 20% below what the loudest interval needs lossless (objects' noise 15 bits): no late unit at that rate, at least one
  shaped interval.

10 s runs in the default test set. 2 min and 20 min are tagged `[.long]` (run them with `iclforge-truehd-tests "[.long]"`):
RESULTS_PLACEHOLDER

## Questions for the owner (numbered after ERRATA's 60 to 79; the decoder session owns ERRATA.md, so these wait here)

**71. Is a lossy object substream acceptable at all?** `RateMode::kShape` is the only lever rate control has on a lossless stream: the
objects of substream 3 give up low bits (the output shift and quantiser step send the cleared bits for nothing) in the restart intervals
that do not fit. The bed is untouched. It is off by default, labelled lossy in the status line, and it is not what Dolby's encoder does
(that spends the budget on clustering, `cluster_*`, which belongs to another session). Recommendation: keep it as an opt-in for the
`truehd-atmos` tool until clustering exists, then make clustering the first lever and this the last.

**72. Which reading of TH spec 3.4 is right: may a late unit's lag be made up?** The encoder's model said a unit starts "no sooner than
one unit duration after the previous start". Taken with slots that advance by one unit duration, that keeps the lag of a late unit for
the rest of the stream (the offset between start and slot never shrinks), so a stream of any length ends when its bursts add up to 75 ms.
`RateModel` now starts a unit at the later of its slot and the end of the previous arrival, so the backlog drains. Streams that never had
a late unit are byte for byte what they were. Recommendation: keep the draining reading; confirm with `forge probe detail=rate` on a disc
track: if the real streams' `input_timing` never moves back toward the slot after a burst, the other reading is the specification's and
rate control would need a bound on the sum of bursts instead.

**73. Where do restarts and major syncs fall on the disc?** ERRATA 67 has the substream 3 restarts 128 units apart; whether the major sync
is at every restart there is not known. `major_sync_interval_units` makes the cadence a parameter (default: a major sync at every
restart, what the encoder always wrote), and a decoder that joins partway waits for a major sync. Recommendation: read
`forge probe DISC detail=structure` (major syncs) against `detail=rate` (interval starts) on one disc and set the default to what it shows.

**74. Is the bitrate the caller chooses a bound on a 75 ms window, or on the whole interval?** `truehd-bitrate=` runs the same arrival
model at the chosen rate, so a stream within it never needs more than 75 ms of buffering at that rate; an interval's mean can exceed it by
what the buffer holds (the shaped tests show intervals above the target by up to a tenth). Recommendation: keep the window reading; a
per-interval mean bound would refuse streams a decoder can play.

**75. Two tests of the base abort in a Debug (assertion) build.** `a FIR filter with state and a filter of order 9 are refused`
(`tests/core/test_syntax.cpp`: MSVC's array subscript assertion) and `the extension of the channel meaning has a body of length + 1 words`
(`tests/io/test_channel_meaning.cpp`: `BitWriter::put` asserts `bits <= 32`). Neither touches this branch; both are in files the decoder
session or the earlier encoder sessions own. Also `tests/decoder/test_object_robustness.cpp` used `getenv`, which MSVC refuses with
/WX; atmos-bed has since replaced it. Recommendation: the owner of those files fixes them; this branch's tests are clean in the same
build.

## Notes for the sessions that touch the same files

- `encoder.cpp`: the rate model that sat inline in the unit loop moved to `ratectl_model.cpp`; `process_interval` is now a loop around
  `code_interval` (the old body) that restores the stream's counters and arrival models and tries again when `RateMode::kShape` allows it.
  The `extra_data` callback may therefore be called more than once for a unit. The decoder (`verify`) now sees an interval after it is
  accepted, not unit by unit. The segment sealing, directory entry and extra data framing call `layout_*`. Rebase conflicts with the matrices
  session are most likely in the AU loop at the end of `code_interval` and in the `Impl` members.
- Shaping keeps a reserve: while the objects can still give up bits, an interval that ends with less than half of the 75 ms buffer free
  is coded again with one more bit, because an interval that only just fits leaves the next one late in its first units however much it
  gives up (found by the 2 minute run: second 103 of the mixed signal failed without it).
- Shaping rounds the object planes in `code_interval` right after the input is copied, before `object_bed` or the program's `prepare`, so
  both object layouts see already-rounded objects and the chain's self-check still holds.
- `EncoderConfig::peak_data_rate_bps` is now taken through `declared_peak_rate_bps()` (what a reader gets back from the major sync field),
  not as typed; for 18 Mbit/s the two are equal.

## Not done

- Per-substream budgets (substream 3 against 0 to 2 other than by shaping the objects), a rate-distortion choice of which objects give up
  bits (all do, equally), noise shaping or dither of the rounding, and an adaptive restart interval (shorter intervals under pressure).
- Round trip with the matrices session's chain: nothing in rate control depends on the chain, but "run the whole round trip" waits for that
  branch to land.
- `forge probe` does not yet report the model at the chosen bitrate next to the declared one in one run (two runs do).
