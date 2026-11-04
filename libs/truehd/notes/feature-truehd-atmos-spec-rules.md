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

4. **The Atmos flag and the evolution frame** (spec 4.1.1, 15.4.1; independent audit F13): the major sync carries exactly `0x1000` for a
   stream with object channels, and the evolution frame is the fixed TrueHD configuration (`layout_evolution_frame`). DRP run on two
   streams (12 and 16 objects, `drp-acceptance/results-b8.md`): presentation 16 is still not offered, now with "Consecutive Evo frames fail
   security check" (0xfe) because the protection byte is zero; presentations 0/2/6/8 decode but log the same error 282 times and are
   2,480 frames short. The same run raised no 0xE7/0xA6/0xA7/0xB8, so DRP accepted the tight `max_bits`, `max_shift`, `max_lsbs` and the
   repeat. The owner's policy: the flag stays on by default, the frames stay at the normal cadence, unsigned unless the operator signs.
5. **`forge truehd-atmos sign-objects signing-key=`** signs the evolution frames through `apply_truehd_object_signing()` (ICL Forge's own
   scheme, not a Dolby tag); `truehd-evo-frames=all|every:N|first|none` is an optional extra, `all` by default.

## Read for this branch (owner-cleared in this session)

Spec v3.53 and, after the owner's word, `ss3-closeout.md` (sections 7, 10), the independent audit's REPORT.md and the spec-answers
addendum relayed by the coordinator. Nothing else under `dee-analysis` was opened.

## Checked and found already right in the #1735 code

- B7 item 2 (bypass matrices are not masked): `kernels.hpp` already adds the bypass bits to the unmasked value (ERRATA 69), as the spec's reading C.
- B7 item 4 (terminators): the decoder accepts terminatorA in both patterns and a 12 bit count, which equals the spec's 13 bit count for
  every count the encoder can write (at most 3,600); the encoder writes `0xD234E` and the count. No rule of D10 was found broken; the
  tighter rejections (a non-last unit carrying it, 0xE5) are not enforced, which is the lenient side.
- Restart positions: the encoder restarts on the interval grid (8 to 4,096 units), which the spec allows ("any restart positions 8 to 128
  access units apart"). It does not place anchors at channel-order changes; that rule is DEE's, not a requirement.

## Not done (and why)

- **Chain limits of substream 3** (ss3-closeout 7a/7b: four or more single-element downmix rows in 15 primitives; a zero downmix row
  through a lower substream's matrix): a rewrite of the matrices session's chain construction in `object_bed.cpp`, larger than this
  branch; the placeholder render does not produce either case. Needs its own task.
- **B7 items 1, 7, 8, 9** (extended_substream_info gate on `substream_info` bit 7; the default `kIndependent` layout pairing ext 3 with
  `min_chan` 8 and matrices; `sixteench_channel_count` without the LFE; `normalize` dialnorm): each changes goldens or fixtures across the
  suite and wants its own change and DRP check. Item 1 matters only for an Atmos stream with extended_substream_info 0.
- **96/192 kHz Atmos output.** Substream 3 is legal at both rates (spec-answers A3), the ramp step is `(0x666 >> r) * 4`, and the width of
  the scaled fine sum is not settled. Enabling it would need: the encoder's ramp/step per rate and a rate-aware object bed; the rate model
  at 4/8 times the arrival; the OAMD cadence (1536 samples at 48 kHz) and the evolution framing per rate; hand-built fixtures at both rates
  decoded by the Dolby player; a decision on the fine-sum width (an encoder must keep it inside 32 bits). Question for the owner: is the
  96/192 kHz object stream wanted before the sum width is settled?
- **Rate law (audit F2):** the library `RateModel` drains a backlog; spec 18.2 and PR #1748 use the per-unit law. To be made consistent
  after #1748 lands (on #1735's merge).
- DRP re-run of the final head, FFmpeg 6.1.1 leg for `hb_data_check`.

## Open

- 81. Does the decoder session want `presence::kChannels`'s old meaning kept anywhere? The spec now confirms the gated reading (DRP and an
  independent decoder read it so; the old reading failed `v_gate_c0/c2/ec`).
