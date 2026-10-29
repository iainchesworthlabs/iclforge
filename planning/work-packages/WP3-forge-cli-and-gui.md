# WP3: forge (command line and window)

Goal: `forge` and `forge-gui` treat TrueHD as DD+ is treated: every command that reads an AC-3 family stream reads TrueHD or refuses it by name; the tools that work
on a finished stream do; the GUI opens, plays and checks TrueHD, then encodes it. Tests run on Qt 6.12 and the GUI has a coverage floor.

Read first: [README.md](README.md), [../truehd-parity-forge.md](../truehd-parity-forge.md) (the matrix R, P, T, C, L, X, M, U, D; phases 1 to 7; questions 1 to 13),
`docs/forge/`, `apps/forge/gui/FEATURE_COVERAGE.md`.

## Where it stands

Done since the audit: options a TrueHD run cannot apply are refused by name; `forge probe` is valid JSON in every container and has `detail=evolution|structure|format`;
it shows the channel meaning; `forge metadata` patches the channel meaning in every major sync and re-stamps the CRC; `normalize` writes each presentation's dialogue
norm from its measured loudness; `qc` reads TrueHD (integrity gate, per-presentation loudness, the stored dialogue norm beside it, presets, exit 6); `decode strict`
exits 2 when frames or units were lost; `truehd-encode` takes per-presentation channel-meaning options; bytes ahead of the first major sync are skipped with a warning.
Not done: `cut` and `cat`, `transcode` from TrueHD, test-stream makers, output devices (`monitor`, `record`, `play`), the project file's command for TrueHD
(`apps/shared/project` does not name `truehd-encode`), fMP4/HLS/DASH (refused, no source), and the whole GUI: nothing in `forge-gui` names TrueHD. The probe report still
lives in `commands/truehd.cpp`; moving it to `apps/shared/media` is audit row M1 (Hearth and the GUI call it).

## Scope, in this order

1. **Re-tally the audit.** The matrix is dated 2026-10-29. Walk it row by row against the tree and mark what has landed; correct the page. Report the new count of missing and partial rows.
2. **Inspect and measure** (audit phase 2): move the TrueHD report to `apps/shared/media` (M1); add what the library already parses (channel names, bit depth, duration, measured and
   peak rate against TH spec §18 using the confirmed `peak_data_rate` scale, restart spacing, presentation list, the lossless-check result); `detail=frames`; golden JSON for every
   fixture; the JSON key names (owner question 8: keep the `truehd` member, additive).
3. **Tools on a finished stream** (phase 3): `cut` and `cat` (start at the first major sync at or after the requested time and print the time used; refuse a `cat` whose second
   input does not open with a major sync or differs in shape; owner question 4); the stream-metadata half of `truehd-encode`/`normalize`; `transcode` from TrueHD to AC-3 or E-AC-3 with
   `downmix=` as for DD+ (question 5; no E-AC-3 to TrueHD). Prove each with FFmpeg's strict decode (a cut stream decodes to the right slice; a joined stream decodes to the two decodes in order).
4. **GUI, first three** (phase 6): link `iclforge::truehd` into `forge-gui`; open, play and QC for TrueHD (the player with a bounded streaming decode, not an 11 GB in-memory decode for a two-hour film;
   WP2 builds the streaming demuxer, so coordinate); add the TrueHD fixtures to `apps/forge/gui/tests/fixtures` and the rows to `FEATURE_COVERAGE.md`; QML tests for each; `forge-gui --smoke` on a TrueHD input.
5. **GUI encode** (U1, the encoder tab as its own tab, not a fourth entry of `plan::Codec`), the command bar line, and projects (`.forgeproj` names `truehd-encode`).
6. **Output devices** (phase 5): `monitor` (decoded PCM through the playback layer, with 8 channels at up to 192 kHz; check `MonitorSink` first) then `record` to a file; `play` (MAT to a receiver) waits for a receiver and for `libs/audio` to carry the format.

Out of scope: Atmos and signing (WP1), fMP4/HLS/DASH for TrueHD until a source exists.

## Open questions with recommendations

Does the GUI encode (the owner said yes on 2026-10-29: open, play, QC and encode now). QC is the confirmed integrity gate plus loudness presets; exit 6 on either. `cut`/`cat` rule above.
Refuse, not warn, for an option the run does not use. One channel-mask convention for the CLI's WAVs (TrueHD 5.1 writes side surrounds 0x60F, E-AC-3 5.1 back surrounds 0x3F): settle it with ERRATA
question 16 and apply it in `decode`, `truehd-encode` and `levels` together; ask the owner. `monitor` before `play`.

## Proof required

Mutation-proof CLI tests that need no FFmpeg (built from `truehd-encode` output) plus the codec-matrix legs; for the GUI, the QML suites and `forge-gui --smoke` on Qt 6.12 (README), and a coverage floor for
`apps/forge/gui`'s C++ in the lane WP7 builds. Help text, man pages, the banner and `docs/forge` name TrueHD wherever E-AC-3 is named.
