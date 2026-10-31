# WP2: Hearth network output and the ESP32 sinks

Goal: Hearth plays TrueHD to rooms, not only locally. A TrueHD item can go to a group of Sendspin sinks (PCM, Opus or coded TrueHD, per sink), the ESP32
sinks that can carry it do, and the ones that cannot say so by name. The Hearth window shows what happens. Tests run on Qt 6.12 with coverage floors.

Read first: [README.md](README.md), [../truehd-parity-hearth-player.md](../truehd-parity-hearth-player.md) (phases H0 to H8, questions 1 to 12),
[../truehd-parity-esp32-sinks.md](../truehd-parity-esp32-sinks.md) (phases 0 to 7), [../hearth-sendspin-extension.md](../hearth-sendspin-extension.md),
[../esp32-sink-tiers.md](../esp32-sink-tiers.md), [../esp32-stream-set.md](../esp32-stream-set.md), [../hearth-reference-player.md](../hearth-reference-player.md).

## Where it stands

Done and in the tree: H0 (track choice stops Hearth choosing a TrueHD track it cannot play) and H1 to H3 (Hearth decodes and plays TrueHD and MLP locally; seeks
land on the unit that holds the place; joins are gapless to the sample; a lost unit plays as silence until the next restart; the window, the Media JSON
and the diagnostics say what the item is). The engine's output is bit-for-bit what `forge decode` writes on the 25 fixtures. The Qt parts (window, Decoder tab,
QML) were written without a Qt kit and built for the first time on 2026-10-30; check their tests before you build on them.

Not done: H4 (PCM and Opus members), H5 (coded TrueHD over Sendspin), H6 (local MAT passthrough), H7 (Android), H8 (a fallback for receivers that cannot),
all ESP32 phases beyond the feasibility audit, and a streaming demuxer.

## Owner decisions already on record (2026-10-29)

Coded TrueHD over the network to Hearth sinks is in scope. P4 definitely; S3 to be tested; C6 unlikely (no performance) but to be validated. The fallback
chain when a sink cannot take a stream is the sink's stated presentation (the core, or the highest it states), then stereo PCM, then refuse; no transcode.
MAT carrier: 8 channels at 192 kHz (48 kHz family) or 176.4 kHz (44.1 family), 16 bits; the owner's receiver is an Onkyo TX-RZ740 and the owner runs receiver tests.

## Scope, in this order

1. **Streaming demuxer and index** (you own `apps/shared/media`'s large-file path). Today a bare `.thd` is read whole once to index it and a container's TrueHD track is
   demuxed from a whole-file copy; a 4.5 GB film stream does not fit. An index of restart points and exact lengths, a demuxer that reads windows, a test with a
   file larger than memory budget set in the test. Forge (WP3) uses it afterwards.
2. **Per-output track preference** (owner question N-27): a remux with TrueHD first now plays TrueHD decoded; a receiver that was passed the AC-3 track now gets PCM.
   Add a preference per output, with the AC-3 or E-AC-3 track chosen for passthrough outputs until H6 exists.
3. **H4**: PCM and Opus members of a group, each served or told why. Rates above 48 kHz need a streaming sample-rate converter or a stated refusal (question 7 in the audit;
   recommended: add one, because 96 kHz masters are common).
4. **H5 and ESP32 phases 0 to 4**: coded TrueHD over Sendspin. Raw access units in groups of 24 (about 20 ms) with a new data type in the player role (an added
   value, not a new version), the support-object limits, `hearth-testsink` and `hearth-testserver` support, a play baseline per stream, ten-minute group plays with zero
   underruns, over-limit streams refused by name. ESP32: measurement first (Cortex-M3 instruction count, QEMU RISC-V, heap and stack with a PCM hash against the manifest),
   then the decoder footprint (behaviour-preserving restructure of `libs/truehd`'s decoder state; the audit says the stack frame is 25 KB and the fixed state 156 KB),
   then the component and file or HTTP play, then the board table (P4, S3, C6 if in scope). Ask the owner for board time.
5. **Sender's fallbacks** (ESP32 phase 5): drop substreams to the presentation the sink states; PCM for stereo; refusal otherwise.
6. **H6 local MAT passthrough** only after a receiver run: ALSA and PipeWire first, WASAPI, then Core Audio. Until then each output decision says "not run".
7. **H7 Android** and **H8 transcode fallback**: defer unless the owner asks.

Out of scope: TrueHD objects (WP1), a lossy transcode, bit-exact exclusive-mode PCM output (the audit recommends not in this round).

## Open questions with recommendations (from the owner document, rows N-23 to N-31)

Keep exact local seeks (unit-exact, under 50 ms of decode at the longest restart interval); best on an Atmos stream is the 7.1 bed until WP1; one block per access
unit rather than 256-sample repacking; default to the stream's own 2ch or 6ch presentation when the output layout equals it; lenient mode stays beside strict;
a streaming demuxer before Hearth is announced for films; the join-point rule (one major-sync unit in 256) is checked against the DEE fixtures.

## Proof required

Qt build and tests on Qt 6.12 (README), `hearth_controller_tests` and the QML suites green, the engine's equality tests to `forge decode`, the throttle and loopback
tests, and the coverage floor for `apps/hearth` raised to cover the new code (WP7's lane). Nothing is announced that no test or run shows. Hardware you do not have
(P4, S3, C6, a receiver, a Shield) is listed in your report as not run; ask the owner for the run.
