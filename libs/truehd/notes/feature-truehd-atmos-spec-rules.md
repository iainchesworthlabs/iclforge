# feature/truehd-atmos-spec-rules (B8), stacked on feature/truehd-atmos-substream3 (#1735)

Source of the rules: TH spec v3.53 (the owner cleared reading v3.52 or newer for this task), read for sections 7.1, 7.1.1, 8.1,
8.2, 10.2.1a, 10.2.1c and 19.3 only. Only the passages tagged VERIFIED or stated as a rule of the decoder were used; INFERRED passages
were not taken as rules. Not read: `dee-analysis/output/findings/*`, `drp-verified-streams.md`, the `wpc` streams (the owner's answer
covered the spec only).

## Done

1. **`param_presence_flags` bit 0 gates `new_guards`, not the channel loop** (spec 8.2, 10.2.1a, 10.2.1c (b)). The reader and writer in
   `core/syntax.cpp` read and write the bit that replaces the flags only while bit 0 is set, and the loop over the channels'
   parameters is always read. The enum member `presence::kChannels` is now `kGuards`. The spec says two decoders in this project
   read the bit unconditionally; this was the one in `read_block_header`, shared by every substream including 0x31EC. Tests: the old
   "gate" test, a new one for flags `0xFE`, and the hand-written 0x31EC streams in `test_syntax.cpp` that cleared bit 0 now keep it.
2. **`max_bits` is the true bound** (spec 7.1.1, 19.3): bit length of the largest output sample after the output shift, plus the sign,
   per restart interval and substream, 0 for silence; `max_bits_repeat` equals it. It was 24 in both, below the truth for a sample of
   -2^23. `max_shift` (the largest per-row shift, 0 to 7) and `max_lsbs` (largest `huff_lsbs` in the interval) were already the true
   values and are now covered by `test_restart_bounds.cpp`.
3. **`data_check_present`** (spec 8.1): `block_data_bits` excludes its own 16 bits, and the block header CRC (poly 0x1D, init 0xA2,
   bits of `block_header()` only, an empty header gives 0xA2) is checked; a mismatch counts a check failure and is refused in strict
   mode. `block_header_crc()` is in `core/crc.hpp`. The decoder's own hand-built tests moved from "+16" to the spec's count.
   ERRATA entry 21 is the decoder session's file and still says "+16": it needs to say the spec's convention now.
   The hand-built `hb_data_check.thd` and the Python reference (`tools/references/truehd_writer.py`, `truehd_syntax.py`) followed
   FFmpeg's count (+16) and a zero CRC byte; they now follow the spec's, and the manifest records FFmpeg's strict decode of the
   stream as `refused` (it checks the other count; ERRATA 21 recorded exactly that mismatch), so the FFmpeg leg of
   `verify_truehd_interop.py --handbuilt` must be run with FFmpeg 6.1.1 to confirm (not run here: only 8.0.1 is installed).

## Not done (B8 items I could not do from the spec text read)

- The orchestrator's ss3 rules (downmix rows with at most 15 primitives, no zero rows in substream 3, restart positions): the spec
  text I read does not state them as rules (restart positions are "any 8 to 128 access units apart"); they came from `ss3-closeout.md`,
  which was not given to me.
- B7 items 1, 2, 4, 7, 8, 9 (extended_substream_info gating, bypass matrix not masked, one 32 bit termination word, kIndependent layout
  pairing, `sixteench_channel_count`, `normalize`): not started; items 1, 2 and 4 change decoder files the original brief keeps out
  of this session's hands.
- DRP re-check: nothing here has been run through the Dolby Reference Player; every test is `[no-external-oracle]`.
- `libs/truehd/ERRATA.md`, `SUBSTREAM3.md`: untouched, as before.

## Open

- 81. Does the decoder session want `presence::kChannels`'s old meaning kept anywhere (ERRATA entry for param_presence_flags says
  "provisional")? The spec now confirms the gated reading by rewritten streams on the Dolby decoder.
