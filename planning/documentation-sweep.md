# Documentation sweep: TrueHD, AC-4 content signing and Common Encryption

Status: **round 1 done, 2026-10-29; round 2 waits for the parity audits.** The owner asked that TrueHD and E-AC-3 (DD+) be equal first-class
citizens, so wherever E-AC-3 appears in a table, a list, a navigation entry, a diagram or an example, TrueHD appears as its peer with an honest
status. This page is the inventory: every page that is a roadmap, changelog, capability or feature table, completeness or status matrix,
support matrix, comparison, index or navigation, what it said about TrueHD, AC-4 content signing and Common Encryption before the sweep, and what
was done. Status of the work itself is in [ROADMAP.md](../ROADMAP.md#truehd-and-e-ac-3-as-equals-proposed).

## Basis

The tree is `integ` at 7a095779 (the TrueHD stack, AC-4 content signing, the MP4 reader's protected-audio refusal, the Online Delivery Kit
checks and the AC-4 decoder fixes from Dolby's streams). Every claim written in this round was checked against the code, a test or a run of
the binary built from that commit; counts were measured again, not copied:

| Measured | Result | How |
|---|---|---|
| `forge` commands | 49 (the docs said 46) | `forge --help`, names after `forge `, `--version` left out |
| Libraries under `libs/` | 17 (CONTRIBUTING said 12) | `kind: library` rows of `tools/checks/projects.json` |
| Tests labelled `truehd` | 138 | `ctest -N -L truehd` in the build of this commit |
| TrueHD fixtures and hand-built streams | 12 and 12 | `testdata/truehd/`, `testdata/truehd/handbuilt/` |
| Open questions for the owner | 19 (TrueHD), 15 (AC-4 content signing) | `libs/truehd/ERRATA.md`, `libs/ac4/ERRATA.md` |
| Refusals of TrueHD by `forge` | `transcode`, `qc`, `fmp4`, `monitor` and `play`, exit 1 | run on `testdata/truehd/truehd_m51_s24.thd` for the first three; `monitor` and `play` are unavailable in this build, and are read from the code (`refuse_truehd` in `live_audio.cpp` and `audio_io.cpp`) |
| `forge truehd-encode x.mlp` | refused, exit 1 | run |
| TrueHD in the speed, memory and kernel benchmarks | none | `grep -ri truehd tests/performance` |
| TrueHD harnesses in `tools/fuzz/run.sh`'s default targets | none | read |

The status words are the repository's own: the roadmap's In progress, Partial, Proposed, Blocked and Out of scope; the development status
page's 🟢 🟡 🔴 🔵; and the support catalogue's Source, Limited, In development, Unavailable with its evidence levels. Nothing new was added.

## The inventory

"Now" is what the page said before the sweep. "Signing / CENC" is what it said about AC-4 content signing and Common Encryption.

### Root files

| Page | Kind | TrueHD now | Signing / CENC now | Action | Done |
|---|---|---|---|---|---|
| README.md | Summary, status, layout | Library row says TrueHD is in the C interface only; the intro, trademarks, limits, Forge, Crucible and Hearth rows, validation, layout and documentation list name E-AC-3 and AC-4 only | Nothing | Intro, Forge, Crucible and Hearth rows, trademarks (and that TrueHD has no published standard), a TrueHD limits paragraph, a content-signing and protected-files paragraph, validation, conformance-vector sentence, `libs/truehd` in the layout, documentation list | yes |
| ROADMAP.md | Status board | A TrueHD section in progress, with an authenticity note that points at the superseded branch; a Blocked row (non-public MLP material); the black-box line under Out of scope | A Partial-tails row for AC-4 content signing (15 questions); a Proposed row for Common Encryption without TrueHD | TrueHD section rewritten as a table by layer; "TrueHD and E-AC-3 as equals" section with one row per area; Blocked row corrected; Dolby reference content row; AC-4 signing and CENC rows corrected (option names, the sample's variable, effort, `mlpa`). The Out of scope black-box line is the owner's and is untouched | yes |
| CHANGELOG.md | Release notes | Entries for the decoder, encoder, containers, `forge`, C API and the DEE scorer; two sentences contradicted the later entries ("`forge` has no encode command yet", "the encoder ... is phase T2") | Entries for AC-4 content signing, the protected-track refusal and the Online Delivery Kit checks, and for the AC-4 decoder fixes | Two sentences corrected; three entries added | yes |
| CONTRIBUTING.md | Contributor rules | None in layout or oracles; "12 libraries" | Oracle 6 covers the Online Delivery Kit tools; nothing on signing or CENC | Library count, `libs/truehd` in the layout, a TrueHD oracle ladder. The clean-room section is the owner's and is untouched | yes |
| SECURITY.md | Policy | The untrusted-input list names no TrueHD | Nothing | TrueHD/MLP streams and AC-4 content signatures added to the list | yes |
| mkdocs.yml | Navigation | One nav entry, "TrueHD and MLP decoding"; no Concepts entry; the site description names three codecs | Nothing | Concepts entry; library entry renamed "decoding and encoding"; description | yes |
| overrides/main.html | Home page | Kicker, lede and cards name E-AC-3 and AC-4 only | Nothing | Kicker, lede, Library, Forge, Crucible and Hearth cards | yes |

### Site pages: index, concepts, library

| Page | Kind | TrueHD now | Signing / CENC now | Action | Done |
|---|---|---|---|---|---|
| docs/index.md | Home, index | Not mentioned | Nothing | Intro, goal table, trademark note, and that the measurement strip has no TrueHD series | yes |
| docs/quickstart.md | Guide | `decode` "takes AC-3, E-AC-3 or AC-4" | Nothing | TrueHD in the sentence and an encode example, run against the binary | yes |
| docs/concepts/index.md | Concepts index, diagram | The chain diagram and "one family" text are AC-3 to Atmos; no TrueHD or AC-4 in the navigation sentence | Nothing | A TrueHD section, trademark note, the clean-room paragraph, FFmpeg paragraph, links. The diagram is a lineage of E-AC-3 and stays as it is: TrueHD is not part of it | yes |
| docs/concepts/truehd.md | Concept page | Did not exist | n/a | Written | yes |
| docs/concepts/ac3-eac3.md, atmos-joc.md | Concept pages | None | None | None: they define E-AC-3 and JOC; TrueHD's Atmos is on its own page | n/a |
| docs/concepts/ac4.md | Concept page | None | None | None | n/a |
| docs/concepts/object-signing.md | Concept page | A "Sibling: TrueHD Evolution" section whose first paragraph described a truncated HMAC and named the superseded branch, contradicted by its second paragraph | The AC-4 section describes the content signature | Section rewritten: both accounts, which the code follows, that nothing is built | yes |
| docs/library/index.md | Library overview, target table | Prose mention; no row in the target table | Nothing | `iclforge::truehd` row, containers and AC-4 rows, header directory | yes |
| docs/library/capabilities.md | **Capability table (authority)** | A TrueHD section with an encoder and containers; the Encoding and Decoding sections point only at AC-4; no warning | AC-4 content signature row; MP4 refusal in the container row | Pointers, rows for `forge`, signing, protected `mlpa`, bindings and applications, an Other row, a TrueHD warning | yes |
| docs/library/development-status.md | **Status matrix** | TrueHD section: no containers, `forge`, bindings, applications, signing detail or verification; a cross-check table with "eleven questions"; "carriage is phase T2c" | AC-4 signing rows; Spott and kit rows; CENC rows for MP4 and IAMF | TrueHD section rebuilt by category; container, transport and cross-check rows | yes |
| docs/library/application-coverage.md | Support matrix | Includes a generated table without TrueHD | Not in the generated table | Source is `docs/assets/data/support-catalogue.json`; rows added and regenerated | yes |
| docs/library/truehd.md | Library page | Two "Encoding" sections; "`forge` has no encode command yet" | Not mentioned | Sections merged, command named, rows for signing, protected tracks, applications | yes |
| docs/library/signing.md | Library page | A seam paragraph that did not say nothing is built | AC-4 content signing section | Seam paragraph states nothing is built | yes |
| docs/library/ac4.md | Library page | None | No mention of the content signature or of the refusal | A section pointing at both | yes |
| docs/library/decoding.md, encoding-ac3.md, encoding-eac3.md, metadata.md | Library pages | E-AC-3 and AC-4 only | None | Pointers to the TrueHD decoder and encoder; TrueHD's major-sync fields in metadata.md | yes |
| docs/library/muxing-and-sinks.md | Library page, tables | A TrueHD-in-containers section; the `PassthroughSink` section names AC-3, E-AC-3 and AC-4 | The MP4 refusal | A TrueHD paragraph in the sink section (none) | yes |
| docs/library/c-api.md | Reference | Full TrueHD section | None | Intro says "decoder and encoder" | yes |
| docs/library/python-api.md, rust-api.md | Binding references | Not mentioned | None | A statement in each that TrueHD is not wrapped | yes |
| docs/library/header-map.md | Table | No `truehd` headers, no MAT, no crypto headers | None | Rows added | yes |
| docs/library/examples.md | List of examples | The two C TrueHD examples are missing | `ac4_content_signature` listed | Rows and the build options | yes |
| docs/library/api-stability.md | Policy | TrueHD in the tiers | None | None | n/a |
| docs/library/file-io.md, quality.md, channel-plans-and-routing.md, spatial-and-atmos.md, station-broadcast.md | Library pages | None | None | None: they are about WAV, the AC-3 family's quality model, E-AC-3 routing and objects | n/a |
| docs/library/iab.md, adm.md, adm-bridge.md, iamf.md | Library pages | None | iamf.md: Common Encryption refused | None | n/a |

### Forge, Crucible, Hearth, Karaoke

| Page | Kind | TrueHD now | Signing / CENC now | Action | Done |
|---|---|---|---|---|---|
| docs/forge/index.md | Product index | None | None | TrueHD, signing and the refusal in the `forge` bullet; the GUI bullet says what it lacks | yes |
| docs/forge/cli/index.md | CLI index | None; "46 commands" | None | "49 commands" (four places), TrueHD sentence | yes |
| docs/forge/cli/commands.md | **CLI reference, usage block** | Detailed TrueHD section; the usage block predated it ("46 commands", no `.thd` in `decode`, `levels`, `loudness`, `spdif`, `unspdif`, `mkv`, `mp4`, `ts`, `demux`) | `ac4-sign` and `ac4-verify` had a section and no usage lines | Usage block taken from `forge --help`; count 49 | yes |
| docs/forge/cli/metadata-options.md | Option reference | TrueHD decode options | Signing options | None | n/a |
| docs/forge/gui/index.md, open-stream.md, qc.md, format-and-channels.md, live-session.md, objects-and-motion.md, loading-a-source.md, projects.md, source-assignment.md, coding-tools.md, metadata.md, inspect-objects.md, karaoke.md, localisation.md, accessibility.md | GUI guide | None; the GUI has no TrueHD route | None | The product index (forge/index.md) states it; the pages describe features the GUI has | n/a |
| docs/hearth/index.md | Product index | None | None | A note: not played, with what the file picker and sinks do | yes |
| docs/hearth/desktop-player.md, android.md | User guides | None | None | One sentence each | yes |
| docs/hearth/sink-esp32-s3.md, sink-esp32-c6.md, sink-esp32-p4.md, sink-firmware.md, sink-installer.md, sink-layouts.md, sink-speakers.md, player.md | Sink guides | None | None | None: board guides for what the firmware decodes; the bare-metal index and the generated tables carry the TrueHD row | n/a |
| docs/crucible/index.md, signal-path.md | Product index | None (a receiver's list in troubleshooting.md names TrueHD) | None | Codecs section and the mode sentence | yes |
| docs/crucible/install.md, room.md, settings.md, accessibility.md, localisation.md, troubleshooting.md, design/promotion.md | Guides, record | None beyond a quoted receiver list | None | None | n/a |
| docs/karaoke/index.md, forced-alignment.md | Guide | None | None | None: karaoke is AC-3, E-AC-3 and, as options, AC-4 | n/a |

### Platforms

| Page | Kind | TrueHD now | Signing / CENC now | Action | Done |
|---|---|---|---|---|---|
| docs/platforms/index.md and the generated `docs-snippets/generated/platform-*.md` | **Support matrix** | No TrueHD row anywhere | None | Source is `support-catalogue.json`: TrueHD decode, encode, MAT output, AC-4 content signing and TrueHD content signing rows; a detail row on each platform page; regenerated | yes |
| docs/platforms/windows.md, linux.md, macos.md, raspberry-pi.md, android.md, wasm.md | Platform pages | linux.md mentions a receiver's TrueHD entry | None | The generated detail tables carry the TrueHD row | yes |
| docs/platforms/bare-metal/index.md | Platform index | None | None | A paragraph: not built, not measured | yes |
| docs/platforms/bare-metal/cortex-m3.md, esp32-s3.md, esp32-p4.md, esp32-c3.md, esp32-c6.md, esphome.md, memory-layout.md, build-profiles.md, board-bench.md | Measurement and capability pages | None | None | None now: per-board figures for what the parts decode. Round 2 adds TrueHD where the ESP32 audit finds a feasibility figure | round 2 |
| docs/platforms/windows-demo.md, windows-driver-acx.md | Phase records | None | None | None | n/a |

### Validation, reference and project pages

| Page | Kind | TrueHD now | Signing / CENC now | Action | Done |
|---|---|---|---|---|---|
| docs/verification.md | **Validation, check tables, gap table** | A TrueHD section with three tiers; the checks-by-codec table and the gap table lacked TrueHD rows | A content-signature paragraph; a long Online Delivery Kit and CENC section | Rows in the "where each check runs" table and the gap table; a hardware bullet; a TrueHD sentence in the signature paragraph | yes |
| docs/conformance-vectors.md | Comparison, index | "AC-3 and E-AC-3 only; no AC-4" | None | "and no TrueHD vector" | yes |
| docs/threat-model.md | Trust-boundary table | None | A fuzz sentence names the signature harnesses | Rows for TrueHD streams, MAT and containers, signature blobs, protected MP4 boxes | yes |
| docs/landscape.md | Comparison | "AC-3 and E-AC-3 encoders"; AC-4 explained as absent | None | TrueHD explained as absent, with the size comparison that exists | yes |
| docs/performance-quality.md | Performance index | "per codec (AC-3, E-AC-3, AC-4)" | None | A paragraph: TrueHD is in none of the series. The trend pages and `hero-stats.js` are fed by the data branch and are not edited | yes |
| docs/performance-trend.md, quality-trend.md, tool-comparison-trend.md, object-quality-trend.md | Generated trend pages | None | None | Not edited by instruction; TrueHD has no series, so there is nothing for them to show | n/a |
| docs/projects.md and the generated project READMEs | Generated index | `truehd` purpose said "the lossless decoder"; `forge` purpose omitted TrueHD | `base` purpose omitted the signing core | Purposes edited in `tools/checks/projects.json`; `generate_project_pages.py` run (4 pages written) | yes |
| docs/building.md | Options table | No `ICLFORGE_BUILD_TRUEHD` row | None | Row added | yes |
| docs/releasing.md | Packages | None | None | One sentence: no vcpkg feature or Conan option | yes |
| docs/ci-lanes.md, ci-agentic.md, ci-self-hosted-runners.md | CI structure | None | None | None: the CI task owns them; the TrueHD legs are described on the verification page | n/a |
| docs/history.md, renamed.md | Records | One historical mention | None | None: history is not maintained against the current state (CONTRIBUTING) | n/a |
| docs/security.md, roadmap.md, contributing.md | Snippet includes of SECURITY.md, ROADMAP.md, CONTRIBUTING.md | n/a | n/a | Follow their sources | n/a |
| docs/wasm-demo.md, wasm-encode-demo.md | Demo pages | None | None | None: they say what the page decodes and encodes | n/a |

### Planning, bindings, libraries, header comments

| Page | Kind | TrueHD now | Signing / CENC now | Action | Done |
|---|---|---|---|---|---|
| planning/README.md | Index | TrueHD row said T1 in progress and T2 not started | No row for signing; the CENC rows exist | TrueHD row; AC-4 row points at the kit items and the signing questions; this page; library count | yes |
| planning/roadmap-inventory.md | Snapshot | IM5 and IM6 as they stood on 2026-09-17 | None | Three rows in the "since overtaken" block; the snapshot is not rewritten | yes |
| planning/truehd.md | Plan | Current | A signing paragraph | None | n/a |
| planning/cenc-conformance.md | Audit | G24 said `libs/containers` has no `mlpa` | The audit | G24 and D7 updated for `mlpa` | yes |
| planning/cenc-read-side.md | Plan | Lists TrueHD as out of scope | The plan | None | n/a |
| planning/ac4.md | Plan | None | Kit and Spott items (open list) | None | n/a |
| planning/esp32-sink-compatibility-matrix.md, karaoke-support-matrix.md | Matrices | None | None | None now. Round 2 folds the parity audits' matrices here | round 2 |
| bindings/python/README.md, bindings/js/README.md, bindings/rust/README.md | Package READMEs | None | None | A paragraph each | yes |
| libs/truehd/README.md | Library README | Current | None | None | n/a |
| libs/base/README.md, containers/README.md, apps/forge/README.md | Generated READMEs | Missing TrueHD and signing | n/a | Regenerated | yes |
| libs/capi/include/iclforge_c/iclforge.h | Header comments | Opening comment named no codec; the TrueHD section said "a lossless decoder" | n/a | Comment-only edits | yes |
| libs/capi/README.md | Generated README | Names TrueHD | n/a | None | n/a |

## Found and not changed

Documentation depends on these, and each belongs to another task.

1. `forge probe`'s usage line (and the usage block that reproduces it) does not name `.thd` or `.mlp`, although `probe` reads them. The help banner calls the tool a "clean-room AC-3 / E-AC-3 and AC-4 encoder/decoder".
2. `docs/library/truehd.md` and `libs/truehd/README.md` have C++ snippets with no compiled example behind them; CONTRIBUTING.md asks for one. `examples/` has only the two C programs.
3. `libs/truehd/fuzz` has no `seeds/` or `regressions/` directory, and `tools/fuzz/run.sh` does not list the four TrueHD harnesses.
4. `cmake/InstallLibrary.cmake` describes `truehd` as "reader and lossless decoder" and says it is left out of the vcpkg port and the Conan recipe "until a program that ships links it"; `forge` links it now.
5. No fixture or test covers a protected `mlpa` track.
6. `docs/hearth/sink-*.md` and the ESP32 pages are measurement pages; whether they should carry a TrueHD row is for the ESP32 audit.

## Round 2

The parity audits write one planning document per area. When they land, the matrices fold in as table edits:

| Matrix | Where | Source |
|---|---|---|
| Capability, per feature | `docs/library/capabilities.md` (TrueHD and MLP table) and `development-status.md` (TrueHD / MLP section) | the library audit |
| Application coverage and platform support | `docs/assets/data/support-catalogue.json`, then `generate_support_matrices.py` | the forge, crucible, hearth and ESP32 audits |
| Roadmap rows | the table "TrueHD and E-AC-3 as equals" in `ROADMAP.md` | each audit's size and plan |
| Provisional Priority and Criticality | the TrueHD rows of `development-status.md` | the owner's definition of "first class" |
