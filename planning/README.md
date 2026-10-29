# Planning documents

Design proposals and phase plans, kept in the repository and **not published to the
documentation site**. They hold the *how* and *why* behind work that is proposed, partly done, or
decided against.

## How this relates to the roadmap

| Document | Role |
|---|---|
| [ROADMAP.md](../ROADMAP.md) | **Status board** — in-flight, partial, proposed, blocked, and out-of-scope work only. Plain-English names; no new numeric IDs. |
| [roadmap-inventory.md](roadmap-inventory.md) | Reconciliation of roadmap and planning claims against the tree as of 2026-09-17 (working record, not kept current). |
| `planning/*.md` (this folder) | **Design depth** — phases, decisions, exit criteria, measurements. Link from the roadmap; do not duplicate the status board. |
| Product `docs/*/index.md` | **What ships today** — status callouts and guides. |
| [CHANGELOG.md](../CHANGELOG.md) | **What shipped in each release**. |

When a plan lands, update the product index and CHANGELOG; trim the roadmap row; leave the plan as
a record or mark it superseded — see [SUPERSEDED.md](SUPERSEDED.md).

**Names.** These plans were written while the family was called AC3Forge, and they keep the names of
their time: `ac3cli`, `ac3gui`, `ac3hearth`, `ac3crucible`, `ac3forge`, the `ac3::` namespaces and
the `AC3FORGE_` variables. The family is ICL Forge now, and its programs are `forge`, `forge-gui`,
`hearth` and `crucible`; [Renamed](../docs/renamed.md) puts each old name beside its new one.

---

## Active plans

| Page | What it is | State |
|---|---|---|
| [hearth-reference-player.md](hearth-reference-player.md) | Hearth desktop app (`ac3hearth`) and ESP32 Sendspin sinks (`hearth_sink`) | As of 2026-10-13: built, with three hardware exits open; the window passes AC-3 and E-AC-3 to a receiver; plays AC-4 (I2, I5), and an ESP32-P4 or ESP32-S3 sink built with the decoder takes AC-4 in a group (I6, #1446). See [ROADMAP.md](../ROADMAP.md) Hearth section. |
| [hearth-sendspin-extension.md](hearth-sendspin-extension.md) | Sendspin conformance, Music Assistant compatibility, `_ac3forge_player@v1` | As of 2026-10-13: built as written, and carries AC-4 since D11 (an ESP32-P4 or ESP32-S3 sink built with the decoder lists it); the extension role has since gained a sink's report of how it plays the stream, what is playing for a group's members and a sink's request for the karaoke choice (D6 to D8 of [hearth-android.md](hearth-android.md)); aiosendspin's client and server stand in for Music Assistant in CI, and no run has been made against Music Assistant |
| [karaoke.md](karaoke.md) | Annex C karaoke streams in the applications: playback (Hearth, desktop) and creation (a Forge GUI page); the ESP32 sink with a screen later. Design: [karaoke-design.md](karaoke-design.md) | As of 2026-10-11: built as K0 to K6 (Hearth's Karaoke page, lyrics and singer view; the Forge GUI's Karaoke flow), the user guide written; corrected to what was built, with what was left for a later phase listed in the plan |
| [karaoke-phase2.md](karaoke-phase2.md) | Karaoke phase 2: the last-frame defect, the Forge GUI flow's polish, word timing, lyrics for a network group, Android and a browser, an LFE part, re-levelling, duets, timed-text import (Crucible, SubRip, ASS, MIDI karaoke, CD+G); the ESP32-P4 with a panel, built and not yet run on its kit. Research: [karaoke-phase2-findings.md](karaoke-phase2-findings.md) | As of 2026-10-12: signed off by the owner on 2026-10-11; W1 to W11 and W13 are merged (W13's Crucible items are merged), W12 is built and not yet run on its kit |
| [karaoke-microphone.md](karaoke-microphone.md) | Karaoke: a microphone, scoring and recording. The requirements, the latency budget, where the voice is mixed, a pitch score, a recording, the decisions and the build plan; the research is in [karaoke-microphone-findings.md](karaoke-microphone-findings.md) | As of 2026-10-13: decisions D1 to D18 approved by the owner; the desktop path (pitch, capture times, scoring, the microphone card, takes, the pitch ribbon and per-line marks) and the browser demo's microphone card are built and merged; the board path waits on who builds it (D3) |
| [karaoke-microphone-input-paths.md](karaoke-microphone-input-paths.md) | Karaoke: how a microphone's audio reaches Hearth when the singer is at a TV, and where the voice is mixed: a USB microphone in the Shield Pro or the P4X kit, an ESP32 codec, the PC, a phone app on iOS and Android joined by a QR, a browser page as a test; the latency of each, the echo, the decisions D19 to D28, the spikes and the build steps | As of 2026-10-13: a design for the owner's decisions, with the owner's first answers and the Shield's read-only queries recorded; nothing built |
| [karaoke-eac3.md](karaoke-eac3.md) | Karaoke in E-AC-3 (part one, to AC-3's parity) and in AC-4 (part two, options only): the plan, its work items E0 to E9 and decisions D1 to D9. Evidence: [karaoke-eac3-findings.md](karaoke-eac3-findings.md); what exists and who has each gap: [karaoke-support-matrix.md](karaoke-support-matrix.md); AC-4: [karaoke-ac4-options.md](karaoke-ac4-options.md) | As of 2026-10-13: part one, E-AC-3, is built and merged (E1 to E9: the decoder #1362, Hearth #1394 and #1421, authoring and the WebAssembly decoder #1398, the boards #1413, the Forge GUI #1427, the documentation #1429 and #1483); the S3 reproduces it, the C6 is decoded for by Hearth, and a v1.3 P4 reproduces it at 2.0; part two, AC-4, is options and nothing of it is built |
| [karaoke-p4x-panel.md](karaoke-p4x-panel.md) | Karaoke on an ESP32-P4 (chip revision v3.x) with a 7-inch panel: W12 of phase 2, the lyric screen a board draws | As of 2026-10-12: built and drawn on a computer; the kit has not arrived |
| [hearth-speaker-trim-identify.md](hearth-speaker-trim-identify.md) | A sink's own level and distance for each speaker and a bounded test tone out of each, in the output stage after the decode: the design, the memory, the cost and what the kit has to check. User page: [sink-speakers.md](../docs/hearth/sink-speakers.md) | As of 2026-10-25: built and host-tested, the S3, P4X and C6 images built and held to their profiles; nothing run on a board; the Preferences entries wait for the registry |
| [karaoke-sync-measurement.md](karaoke-sync-measurement.md) | Measuring lyric alignment and a group's clock against the room | A plan, with its protocol-level half measured; the other half needs a person in the room, a phone that films at 240 frames a second and a sink with a DAC |
| [hearth-android.md](hearth-android.md) | Hearth for Android: a Sendspin sink app for Android TV that passes the coded stream through to the receiver as the Shield demo does and shows the karaoke lyrics; the requirements, the design, the decisions and a phased plan. Research: [hearth-android-findings.md](hearth-android-findings.md). The screen the desktop, the app and the P4X panel share: [hearth-sink-screen.md](hearth-sink-screen.md) | As of 2026-10-13: phases 0 to 3 and the protocol additions D6 to D8 built and merged; phases 4 and 5 planned on [their own page](hearth-android-phases-4-5.md), with 4a merged (#1509); a Shield run with a receiver on is the open hardware step |
| [hearth-android-phases-4-5.md](hearth-android-phases-4-5.md) | Hearth for Android, phases 4 and 5: decoding on the device for karaoke and for routes that refuse a format (one routing output over the pass-through and a decoding half extracted from the test sink), then the sink as an ordinary `player@v1` player; the pull requests, the spikes, the risks, the calls made and the small leftovers | As of 2026-10-13: reviewed with the owner (#1501); 4a, the decode as a library with a `PcmSink` seam, is built and merged (#1509); the rest is not built |
| [karaoke-formats-options.md](karaoke-formats-options.md) | Karaoke formats, item D of the owner's triage list after phase 2: a MIDI karaoke file in SMPTE time (built), and options with costs for ASS styling and positions, words in a legacy code page, and lyrics and the picture inside MP4 and Matroska | As of 2026-10-13: D15 built (#1347). Of D16, Forge reads a lyrics file in a named code page and writes the words out as UTF-8 (#1418, #1423, #1428); Hearth and Crucible read UTF-8 and UTF-16. Of D14, an ASS duet and a script made with Aegisub's karaoke templater are read (#1416, #1481); styling and positions are not. D17 is options and awaits the owner |
| [esp32-ota.md](esp32-ota.md) | Firmware updates over the network for `hearth_sink` boards (S3, C6, P4): A/B slots, rollback, integrity checks, flash mode; firmware published by CI, and a user guide | As of 2026-10-25: O1 to O5, O8 and O9 built and merged; O6 (the P4's co-processor firmware) is a study; O7 (signed images) is not built and is replaced by O10 (a board checks GitHub for a newer release and installs it from an index signed with the owner's own certificate; a pushed image stays unsigned), which is designed, with some decisions taken on 2026-10-25. See [Sink firmware](../docs/hearth/sink-firmware.md) |
| [hearth-late-night.md](hearth-late-night.md) | Late-night mode on a Hearth sink: a listener's choice of dynamic range (`GET` and `PUT /dynamic-range`), what it means for AC-3, E-AC-3, JOC and AC-4, and who wins against a server's decoder settings | As of 2026-10-25: built and host-tested, builds for the P4X and S3 profiles; not run on a board; the Preferences entry is in the registry |
| [hearth-firmware-signing-ejbca.md](hearth-firmware-signing-ejbca.md) | Step-by-step for issuing the firmware sub-CA and the two firmware signing leaves (stable, beta) in EJBCA, with the screenshots to check first | 2026-10-25, for the owner; part of [esp32-ota.md](esp32-ota.md) O10 |
| [recasting.md](recasting.md) | Library / Forge / Crucible family naming and docs | As of 2026-09-30: phases 1 to 6 largely built, phase 7 (driver signing) not started; its naming decisions were overtaken by N1 ([SUPERSEDED.md](SUPERSEDED.md)) |
| [ac4.md](ac4.md) | AC-4 in full: a decoder beside the inspector, an encoder, and both in the applications; the oracles, the phases and the decisions | As of 2026-10-13: every phase is merged. G0, G1, D1–D11, E1–E10 and I1–I5b by 2026-09-30; D14a–D14i, the decoder on the ESP32-P4, the ESP32-S3 and (fixed point) the C6, by 2026-10-11, with the S3 and C6 board runs on 2026-10-10; I6, the ESP32 sinks taking AC-4 in a Sendspin group, on 2026-10-12 (#1446). DEE's licence ends 2026-11-06. N1 (program names and layout) was built on 2026-09-30 and 2026-10-01; the SonarCloud project key and the first release under the new names are left: see [N1](ac4.md#n1-the-names). Its [state table](ac4.md#state-on-2026-09-30) is of 2026-09-30, with the later phases added |
| [eac3-programme-mixing-metadata.md](eac3-programme-mixing-metadata.md) | `mixmdate` reporting/API completeness and decode-time associated-service mixing, scoped alongside two sibling efforts (CLI `programmeN=` authoring, MPEG-TS `mainid`/`asvc`) | As of 2026-10-11: phases 1, 2, 5 and 6 built (reporting in `forge decode` and in Hearth's media information, #797; mixing an associated service into the main programme, and its CLI option); phases 3 and 4 not: the C API and Python bindings, and a GUI summary. See [ROADMAP.md](../ROADMAP.md#partial-tails-on-shipped-work) |
| [esp32-sink-tiers.md](esp32-sink-tiers.md) | C6 / C61 / S3 / P4 good·better·best modules on one dual-ES9080 PCB | As of 2026-09-30: P0, P1 and P3 built, P4 partly, P2 and both C61 phases not built. The P4 sink plays stereo; everything that needs TDM waits for a v3.x board and the DACs. See [ROADMAP.md](../ROADMAP.md#proposed) |
| [esp32-platform-profiles.md](esp32-platform-profiles.md) | One locked build configuration (a profile) per ESP32 board, read by CI, packaging, the docs and the board tools, with a guard on its locked keys and a suite that plays audio on the release shapes and keeps a baseline | As of 2026-10-13: built and merged. The profiles file with its reader and guard (#1403), CI building each board from its profile (#1406), the boot budget line (#1412), the board suite (#1419) and a baseline for every profile a board has run (`s3-n16r8`, `s3-n16r8-ac4`, `c6-16mb`, `p4-rev1`, `p4-rev1-ac4`; `c6-4mb` shares the C6's rows), with AC-3 and E-AC-3 over Sendspin on the S3 and the P4 (#1476) and AC-4 over Sendspin (#1497); `p4-rev3` has its guard and budgets (#1452) and, since the P4X kit ran the image on 2026-10-20, a play list with a 16-slot TDM row and a baseline; a Kconfig key a change adds is held to the profiles at the pull request (#1504). Started from an HTTP `POST /play` that failed on both S3 boards and the C6 on main while CI was green |
| [esp32-board-bench.md](esp32-board-bench.md) | The five ESP32 boards on the bench as a table, the booking rule (five locks, `ListAgents`, one atomic lock line, restore and release), a session's order of work, the hazards each with where it was learned, and the tools that build, flash, verify and read a board with the network's values taken out | As of 2026-10-12: built (`tools/hearth/bench.py`, `bench_flash.py`), replacing the scripts in `D:/ac3bld/esp-reflash` |
| [esp32-sink-compatibility-matrix.md](esp32-sink-compatibility-matrix.md) | What each ESP32 part accepts, what it refuses, and where PCM comes in: the AC-3, E-AC-3 and AC-4 matrices, the criteria, the build order, and the C6's PCM limit measured | As of 2026-10-11: the PCM fallback is built in the engine and the library (per-sink coded or PCM, one render per width, the sink's stated limits); the C6's limit is its 192 ms ring against the WiFi link's gaps, and whether to offer it PCM anyway is the owner's decision |

---

## Studies and framework (not started or decision-only)

| Page | What it is | Roadmap |
|---|---|---|
| [topology.md](topology.md) | Source, transport, sink roles; HLS/CMAF transport | Hearth sinks use Sendspin instead ([SUPERSEDED.md](SUPERSEDED.md)); none of the HLS/CMAF transport is built, and its frame still applies elsewhere |
| [iab-object-clustering.md](iab-object-clustering.md) | Fitting a master of more than 15 (E-AC-3) or 64 (AC-4) beds and objects into an encode: the cap following the codec, then clustering, with the decisions for the owner | Proposal, 2026-10-10: not started; [`kTooManyChannels`](../docs/library/development-status.md#iab-smpte-st-2098-2) in the IAB and ADM bridges |
| [host-plugin.md](host-plugin.md) | DAW/NLE metering/QC plugin feasibility | [Proposed — DAW/NLE host plugin](../ROADMAP.md#proposed) |
| [qc-report.md](qc-report.md) | Delivery-shaped QC report file | [Proposed — QC delivery report file](../ROADMAP.md#proposed) |
| [cenc-conformance.md](cenc-conformance.md) | Common Encryption (ISO/IEC 23001-7) against the tree: what the standard requires of Dolby audio, what the readers and writers do with protected input, a gap register, measurements with FFmpeg and GPAC, and the decisions for the owner | Audit, 2026-10-29: nothing built; [Proposed — Common Encryption](../ROADMAP.md#proposed) |

---

## Design records (Hearth UI and karaoke)

| Page | What it is | State |
|---|---|---|
| [hearth-design.md](hearth-design.md) | Signed UI design mockups (A0); palette, page layout, Sendspin extension page | Signed off 2026-09-22, and built (the images are mockups, not screenshots of the app); superseded for capability claims by [docs/hearth/index.md](../docs/hearth/index.md) |
| [karaoke-design.md](karaoke-design.md) | Architecture, work breakdown, test and documentation plans for karaoke playback in Hearth and creation in the Forge GUI | Signed off and built 2026-10-11; corrected to what was built |

---

## Measurement and phase records (ESP32, library and repository layout)

Built work; kept for evidence. Current user-facing docs supersede these for setup and capability
claims.

| Page | What it is | State |
|---|---|---|
| [esp32-714-realtime.md](esp32-714-realtime.md) | 7.1.4 E-AC-3 real-time on ESP32-S3 | Built 2026-09-11 |
| [esp32-device-ui.md](esp32-device-ui.md) | Board web UI beside REST API | Built 2026-09-11, and extended in nine more pull requests to 2026-09-26 |
| [esp32-stream-set.md](esp32-stream-set.md) | The 38 AC-3 and E-AC-3 streams the player's `http` source is served | Built and in use: CI plays them under QEMU, and board measurements use them; Sendspin replaced the `http` source only for playing to a board |
| [esp32-player.md](esp32-player.md) | Component layer and ESPHome path | As of 2026-09-30: phases 0 to 2 built; phase 6 built as Hearth B3; phases 3 to 5 (ESPHome, upstream, the encode direction) not built; phase 7 replaced |
| [arithmetic-tiers.md](arithmetic-tiers.md) | Decode arithmetic tiers and platform matrix | As of 2026-10-13: three tiers built for AC-3 and E-AC-3 (the fixed-point tier on the C3 under QEMU only, and on a board on the C6); AC-4's decoder shares `double` and `float` through `libs/base` (D14a) and decodes in real time on an ESP32-P4 to 5.1 and on an ESP32-S3 at 2.0 (D14b to D14i); D14d adds its fixed-point tier, which does not fit a C6 beside WiFi |
| [consolidation.md](consolidation.md) | One shape for every codec (AC-3's), AC-4's four libraries as one, the copies of codec-blind code removed, and `src/` from 22 libraries to 12, in stages C0 to C6 | Built: C0 to C6 and the merges M1 to M3, decisions 1 to 23; merged with the monorepo move as #1199 on 2026-10-09 |
| [monorepo.md](monorepo.md) | C7: the repository as a monorepo of self-contained projects (`libs/`, `apps/<product>/`, `bindings/`, `firmware/`), a project graph that `check_layering.py` enforces over the whole tree, a test binary per project | Built: C7-1 to C7-7, decisions 1 to 15; merged as #1199 on 2026-10-09. The tree is `libs/` (16 libraries), `apps/`, `bindings/` and `firmware/` |
| [layout.md](layout.md) | The layout and names of `src/` (N1B): codecs as peers over a codec-blind base, three layouts, the migration stages and a prototype, with an [inventory](layout-inventory.md); built as L2 in the stages S0 to S6 and N1A (see [what the runs found](layout.md#what-the-runs-found-that-the-plan-did-not)) | Built as L2 in the stages S0 to S6 and N1A (2026-09-30 and 2026-10-01), and taken further by the monorepo move; see [N1 in the AC-4 plan](ac4.md#n1-the-names) |

---

## Superseded records

| Page | Superseded by |
|---|---|
| [player-appliance.md](player-appliance.md) | [hearth-reference-player.md](hearth-reference-player.md) |

Full index: [SUPERSEDED.md](SUPERSEDED.md).

---

## Meta

| Page | What it is |
|---|---|
| [roadmap-inventory.md](roadmap-inventory.md) | A snapshot of 2026-09-17, made for the roadmap rewrite and not kept current; its status block lists the rows the tree has since overtaken |

---

## On the published site

Two phase records live under `docs/` because reference pages cite them as evidence, not as plans:

- [Crucible promotion record](../docs/crucible/design/promotion.md)
- [Windows demo record](../docs/platforms/windows-demo.md)

Each page's status block says what has landed. `tools/checks/check_doc_paths.py` resolves the
links here and exempts prose paths that name directories the tree does not have yet.
