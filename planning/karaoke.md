# Karaoke: playback in Hearth, creation in the Forge GUI

!!! note "Status as of 2026-10-11: built (K0 to K6), user guide (K7) written"
    Scoped on 2026-10-11 in one round of questions and answers, recorded under
    [Decisions](#decisions); the specification's seven decisions were answered the same day
    ([Specification decisions](#specification-decisions)). The build is K0 to K6 of
    [the design's work breakdown](karaoke-design.md#work-breakdown), and [As built](#as-built)
    records what each PR did and where the build departed from this page. A requirement the build
    changed carries an *As built* note. What phase 1 left is planned in
    [karaoke-phase2.md](karaoke-phase2.md). The live status board is [ROADMAP.md](../ROADMAP.md).

    | Phase | What | State |
    |---|---|---|
    | 0 | Scoping decisions | Answered 2026-10-11 |
    | 1 | Specification (in this page) | Signed off 2026-10-11 (decisions 1 to 7 answered); corrected to what was built |
    | 2 | Design: architecture, UX sheet, work breakdown into stacked PRs ([karaoke-design.md](karaoke-design.md)) | Signed off and recorded 2026-10-11; corrected to what was built |
    | 3 | Build, as small PRs | K0 to K6 built; the user guide (K7) is `docs/karaoke` |

## What this is

Annex C (informative) of ETSI TS 102 366 describes karaoke streams: `bsmod` 7 with an `acmod`
above 1/0 carries L and R, a guide melody M where a centre would be, and one or two vocals V1
and V2 where the surrounds would be (Table C.2.1: 3/0 = L M R, 2/1 = L R V1, 3/1 = L M R V1,
2/2 = L R V1 V2, 3/2 = L M R V1 V2). A karaoke-aware decoder reproduces L, C, R with the vocals
at the stream's surround level (Table C.2.2); a karaoke-capable decoder lets the listener choose
none, V1, V2 or both (Table C.2.3).

This plan adds the application side: a way to **play** such a stream with the singer's choice of
vocals and the words on screen, and a way to **create** one from separate parts. It is a set of
features in two existing programs, not a new program.

### What already exists

- **Library** (PR #1267, stacked on #1263; both merged on 2026-10-11, see
  [Dependencies](#dependencies-and-state-on-2026-10-11)): `OutputConfig::karaoke`
  (`KaraokeReproduction::kOff`, `kAware`, `kCapable`) and `OutputConfig::karaoke_vocals`
  (`KaraokeVocals::kNone`, `kV1`, `kV2`, `kBoth`), `MixLevels::karaoke`, `meta::is_karaoke()` and
  the four-argument `output_channel_count()`, in `libs/ac3/src/decoder/output.cpp` and
  `libs/ac3/include/iclforge/ac3/decoder/output.hpp`. `FrameDecoder` sets the flag from the
  frame's own `bsmod`. When this was written E-AC-3 and AC-4 never did; since 2026-10-12
  `Eac3Decoder` does, for a programme of one independent substream with no object layer
  ([karaoke-eac3.md](karaoke-eac3.md)), and AC-4 still does not.
- **CLI:** `forge decode in.ac3 out.wav karaoke` (aware) and `karaoke=none|v1|v2|v1+v2`
  (capable), `channels=2|1` for the 2/0 reproduction; `forge encode ... bsmod=7` with a 3/0 to
  3/2 layout writes a karaoke stream (L, C, R, Ls, Rs = L M R V1 V2).
- **Verification:** no outside decoder was known to reproduce karaoke when this was written (Dolby's
  does, for AC-3 and E-AC-3: [the findings](karaoke-eac3-findings.md)), so the library is checked
  against the annex's own tables (`libs/ac3/tests/decoder/test_output_stage.cpp`,
  `apps/forge/cli/tests/test_cli_encode_paths.cpp` `[karaoke]`).

### What does not exist

Any application around this: no playback page, no vocal chooser outside the CLI, no lyrics, no
authoring page. Annex C's "additional flexibility" (re-levelling or moving V1, V2 or M) is left to
implementers and is not offered by the library.

## Decisions

Asked and answered on 2026-10-11. The owner's answers are quoted where their wording carries the
decision.

| # | Question | Answer | What follows |
|---|---|---|---|
| 1 | What is v1? | **Player and authoring**, both | Two tracks of work, each with its own exit; they meet at a stream made in one and played in the other |
| 2 | Where does it live, and what does it reuse? | **Hearth plays, Forge creates.** No new executable: "the karaoke sides, the playback is in Hearth and the creation is in Forge and their respective platforms". Streams made in Forge are then played through Sendspin into the local or the multi-platform players | No new program directory under `apps`, no new project row, installer or CI lane. Hearth's engine stays where it is (not lifted into `apps/shared`). Playback is a feature of `hearth`; creation is a page of `forge-gui` over the existing encoder |
| 3 | Which platforms? | Playback: **desktop, Qt 6 QML** (Windows, Linux, macOS), in Hearth. Creation: Forge's platforms. **Android and a browser page are stretch goals.** An **ESP32 sink** with a screen "would be an interesting one" | v1 is desktop only. The ESP32 sink, Android and the browser are later phases, listed in [Later](#later-not-v1) with the questions each raises |
| 4 | Which content? | **.lrc lyrics** and **MP4 / MKV carriage** are in. Microphone, scoring, recording and stem files as a source were **not selected** | Out of v1: stem files, scoring, recording, live microphone. The microphone is "a question we need to understand" and stays an [open question](#open-questions) |
| 5 | Who is it for? | **Both**: "My primary use case is the reference for codec users but singers at a tv or laptop sounds like an ideal outcome for this with the esp32 plus a screen" | The first target is a correct, visible demonstration of Annex C that a codec user can inspect. The singer-facing view (large lyrics, full screen) is designed in from the start and built after the reference works |
| 6 | Name | **"Karaoke" everywhere** | A Karaoke page in Hearth and one in the Forge GUI; user guide under `docs/karaoke/` spanning both; this plan is `planning/karaoke.md` |

### Defaults taken, not asked

These follow from decision 2; the specification's sign-off took them as drafted.

- **Release channel.** The features ship in Hearth's and Forge's existing packages and the
  existing GitHub pre-releases. No new artifact, package name or tap formula. What is published
  today is in the project's release notes ([releasing](../docs/releasing.md)).
- **CI cost.** A change under `apps/hearth` or `apps/forge` already lights the three desktop
  lanes, so each PR is kept small and uses the tiered runs (the `verified` label); no full
  dispatch of `ci.yml`.
- **Spec home.** The specification is a section of this page, as Hearth's was
  ([hearth-reference-player.md](hearth-reference-player.md)); `docs/` describes what ships, so the
  user guide is written under `docs/karaoke/` as each feature lands, not before.

## Dependencies and state on 2026-10-11

- **PR #1263** (`feature/ac3-coding-modes-follow-a-wavs-speakers`: a WAV's speaker mask decides
  the coding mode) **merged** on 2026-10-11 at 03:40Z, and **PR #1267**
  (`feature/ac3-karaoke-3-0-reproduction`, stacked on it: Annex C's aware and capable decoders)
  at 04:14Z. The library side is on `main`; no karaoke PR needs stacking.
- **Other sessions are editing near this** on 2026-10-11: the E-AC-3 Annex E mixer and decoder,
  the GUI stream player and `forge monitor` (one E-AC-3 programme, associated-service mixing),
  and Hearth's network code. Each file a karaoke PR touches in `apps/hearth`, `apps/forge/gui`
  or `libs/ac3/src/decoder` is checked against the open PRs first and its owner is messaged
  before it is edited.
- **Hearth decides per sink whether a stream is sent coded or as PCM** (`choose_sink_form`, PR
  #1253 and the PRs stacked on it). That decision matters for karaoke over Sendspin; see
  [Later](#later-not-v1).

## As built

The programme is built as K0 to K6 of [the design's work breakdown](karaoke-design.md#work-breakdown);
K7 is the user guide (`docs/karaoke`). The PR numbers are the record: K5 and K6 were open
when this page was corrected.

| PR | What | Where |
|---|---|---|
| K0 #1274 | Hearth opens MP4, Matroska and MPEG-TS (P11) | `apps/hearth/ui/src/item_loader.*` |
| K1 #1276 | The layout a reproduced karaoke frame comes out in: `ac3::output_mode()` | `libs/ac3`, decoder output stage |
| K3 #1277 | The LRC reader and the lyric cursor (L1 to L5) | `apps/hearth/engine/src/lyrics.*` |
| K2 #1280 | Hearth's karaoke settings, the bed laid out from `output_mode()`, the report fields (P1 to P5, P10) | `decoder_settings.*`, `stream_decoder.*` |
| K4 #1282 | The Karaoke page, its notices (P6 to P8) and the persisted choice (P9) | `KaraokePage.qml`, `Main.qml`, `hearth_controller.cpp` |
| K5 #1284 | The lyrics: `KaraokeController`, `LyricClock`, the lyric view, the singer view (F11) and the lyric delay (L4, L6) | `karaoke_controller.*`, `Lyric*.qml` |
| K6 #1286 | The Forge GUI's Karaoke flow: the parts rules, the flow and the stream player's reproduction choice (A1 to A8) | `karaoke_authoring.*`, `KaraokeFlow.qml`, `stream_player_controller.*` |
| K7 | The user guide, the capability row, the roadmap row | `docs/karaoke/` |

### Where the build departed from this page

- **The reproduced layout was a finding, and became a library function.** A karaoke stream played
  under a reproduction comes out as L, C, R (and the LFE), not as the coded layout, so a caller
  that lays channels out from the coded `acmod` is wrong. `output_mode()` (K1) says what the layout
  is; Hearth's decoder (K2) and the Forge GUI's stream player (K6) both ask it.
- **P9 is narrower than written** (corrected above): the karaoke choice is the only decoder
  setting Hearth keeps across starts.
- **The page needed no controller; the lyrics did.** The design put the stream facts, the choice
  and the notices in a `KaraokeController`. K4 read them from `HearthController.decoderSettings`
  and `currentMedia`, which already carried them, and K5 added `KaraokeController` for what is new:
  the lyrics of the item, the cursor, and the delay.
- **No new Theme roles.** The design said the lyric colours would be three new roles in the shared
  theme. The lyric view takes its three colours as properties (the accent, the muted neutral and
  the text colour from the existing roles), and the singer view's own dark constants; nothing in
  `apps/shared/theme` changed.
- **The decoder corrected a table.** The Forge GUI's table of which part reaches which speaker is
  held to the library's decoder for every mode and choice (K6). It found that a stream with a
  single vocal under capable, both, puts V1 on the left alone (and on the left only in the Lo/Ro
  fold), not in the centre: Hearth's Karaoke page had the same cell wrong (the sound was right) and
  was corrected in K4.
- **The Forge preview is a built stream.** A6 asks that the author hear the encoded stream. The
  Hear step encodes to a temporary file and opens it in the stream player; the run strip lists
  that preview as a run.
- **Documentation screenshots** were not taken with `--smoke-shot` as the design planned: Hearth
  had no `--smoke-shot`, and its `--shot` could not open an item. They were taken by a throwaway QML
  script run in the test binaries, with a generated stream and a fake output, and the bundled
  fonts on `QT_QPA_FONTDIR`. *Since:* Hearth has `--smoke-shot` (`--shot` with network discovery
  off, so it raises no firewall prompt), `--open <file>` and `--seek <ms>` (PR #1361), and the
  guide's Karaoke page screenshot is the running window's. The singer view is still the harness's,
  since a full-screen window is resized by an offscreen platform to its 800 px screen.

### Not measured

- **P5**'s wait (the queued audio), **L6**'s 50 ms against a real output, and **Q1**'s 5% cost.
- **The listening pass** (S2, S3, S7): the owner on a desktop, recorded in a PR.
- **Lyrics against a network sink.** The words follow the engine's heard position; whether that
  is the position a synchronised Sendspin group plays was not checked.
## Specification

Phase 1. Drafted and signed off on 2026-10-11, and built as [As built](#as-built) records.
[Specification decisions](#specification-decisions) records what the owner answered, with the
alternatives that were on the table.

### Users and scenarios

Three kinds of user, in the order the answers to question 5 give them:

- **The evaluator** is a codec user or developer checking what an Annex C decoder does with a
  stream, and wants every step visible: what the stream is, which reproduction ran, what each
  output carries.
- **The singer** is at a TV or a laptop and wants the words, the backing without the lead vocal,
  and few controls.
- **The author** makes karaoke streams from separate recordings and wants to hear the result under
  each reproduction before giving it to anyone.

| # | Scenario | Who |
|---|---|---|
| S1 | Opens a karaoke `.ac3`. The app says it is a karaoke stream, its coding mode, and which of M, V1 and V2 it carries. Switches between as coded, aware, and capable with none / V1 / V2 / both; the output meters show which channel carries what. | Evaluator |
| S2 | Opens `song.ac3` with `song.lrc` beside it. The words follow the audio. Chooses "no vocals" and sings. | Singer |
| S3 | A duet: hears V1 only, or V2 only, while the other singer takes the other part. | Singer |
| S4 | Builds a stream from a stereo backing, a mono melody guide and one or two vocal files. Hears it under each reproduction, then saves the stream; a `.lrc` goes beside it by hand. | Author |
| S5 | Opens an ordinary 5.1 stream, or an E-AC-3 or AC-4 file. The karaoke controls say they do not apply and why, and playback is unchanged. | Anyone |
| S6 | Sends a karaoke stream by passthrough to a receiver, or as coded AC-3 to a network sink. The page says the reproduction is the receiver's or the sink's, not Hearth's. | Evaluator |
| S7 | Plays a karaoke stream to a stereo pair, a mono speaker or a 3.0 / 5.1 layout and hears the matching Annex C reproduction. | Singer, evaluator |

### Requirements

Each requirement is a single statement a test or a recorded check can pass or fail. The IDs are
used by the verification table below.

**Playback (Hearth)**

- **P1 Identify.** For an AC-3 item, Hearth states whether it is a karaoke stream
  (`meta::is_karaoke`: `bsmod` 7 with an `acmod` above 1/0), its coding mode, and which of M, V1
  and V2 it carries (Table C.2.1).
- **P2 Choose.** The listener chooses the reproduction: *as coded* (the library's `kOff`), *aware*
  (`kAware`, Table C.2.2), or *capable* (`kCapable`, Table C.2.3) with the vocals *none*, *V1*,
  *V2* or *both*.
- **P3 Apply.** The choice reaches the decoder through `DecoderSettings` and `decoder_setup()`,
  the one place Hearth's decoder options become a library configuration, and nowhere else.
- **P4 Layout.** The reproduction's outputs reach the speakers through Hearth's existing
  routing: 3/0 (L C R, then the LFE if the stream has one) to a layout that has them, the 2/0
  reproduction to a stereo pair, mono to one speaker.
- **P5 Live.** Changing P2 while an item plays applies to that item without restarting it, as the
  other decoder settings do: `Player::set_decoder_settings` reaches the playing item at its next
  unit, and audio already decoded plays out as it was. How long the singer waits to hear the
  change is therefore the queued audio; the design measures it and says whether to shorten it.
  *As built:* a player test holds that a new reproduction reaches the playing item at the next
  unit; how long the singer waits (the queued audio) was not measured.
- **P6 Not karaoke.** For a stream that is not karaoke, and for E-AC-3 and AC-4 (which the
  decoder never flags), the controls are shown disabled with the reason, and playback is as
  without them.
- **P7 Coded outputs.** When the output is passthrough or a network sink that takes the coded
  stream, the page says the reproduction is not applied by Hearth and what will play.
- **P8 Stereo fold.** Annex C defines no Lt/Rt reproduction. With the stereo fold set to Lt/Rt the
  page says so (the library leaves the Lt/Rt fold as it is) and offers Lo/Ro.
- **P9 Persist.** The reproduction and the choice of vocals are kept and restored on the next
  start, and apply to every item, as the decoder settings do, not per item. *As built:* Hearth
  kept no decoder setting across starts before this, and keeps no other now: the karaoke choice
  (the QSettings keys `karaoke/reproduction` and `karaoke/vocals`) is the one decoder setting that
  persists, with the lyric delay beside it (`karaoke/lyricDelayMs`).
- **P10 Report.** The signal path and the diagnostics say which reproduction ran and, for
  capable, which vocals (`describe(DecoderSettings)` names it).
- **P11 Carriage.** A karaoke AC-3 plays from a raw `.ac3`, from MP4 and from Matroska. *Finding:*
  Hearth's file loader reads only raw `.ac3`, `.ec3` and `.ac4` today
  (`apps/hearth/ui/src/item_loader.cpp` answers "not yet playable" for a container), although
  `app-media`'s container input reads them for Forge. P11 therefore includes the loader change,
  which also serves every other stream in a container; it is the programme's first PR.

**Lyrics (Hearth)**

- **L1 Find.** For an item `name.ext`, a file `name.lrc` in the same folder (extension matched
  without regard to case) is its lyrics.
- **L2 Read.** Time-tagged lines `[mm:ss.xx]text`, several tags on one line, `[offset:±ms]`, the
  id tags `ti`, `ar`, `al`, `by` and `length` (shown or kept for display), and the enhanced word
  tags `<mm:ss.xx>`, one before each word. A word runs from its tag to the next word's tag, and
  the last word of a line to the next line's tag (to the item's length for the last line). Tags
  are never shown as text.
- **L3 Encoding.** UTF-8, with or without a byte-order mark. A file that is not valid UTF-8 is
  reported as unreadable; it is not guessed at.
- **L4 Show.** Each word is drawn in one of three states, in different colours: *sung*, *being
  sung* and *not yet sung*. The word being sung fills in proportion to the time elapsed within
  its span, so a partly sung word is partly one colour and partly the other. Colour is not the
  only cue (the word being sung also differs in weight or underline). The line being sung is
  emphasised, with the previous and the next lines in view, and a seek moves everything to the
  new position. A line whose words carry no tags is emphasised as a whole and is not given
  invented word times; a file with no time tags at all is shown as plain text and is not
  followed.
- **L5 Fail soft.** A missing, empty or unreadable lyrics file shows "no lyrics" (with the reason
  where there is one) and never stops or delays playback.
- **L6 Time.** A word changes state within 50 ms of the audio it belongs to being heard on a
  local output (the engine's heard position, not the decoded position). The fill of the word
  being sung is redrawn at the display's frame rate from that position, advanced by the time
  since the engine last published it, so it moves smoothly between publications. For a network
  sink the lyrics follow the same clock the sink is synchronised to (checked in design; see
  [Later](#later-not-v1)).
  *As built:* the clock is `LyricClock` (the last position the engine published, plus the time
  since, never more than two polls (120 ms) ahead, snapped by every poll, pause and seek), and a
  test drives it by hand. The 50 ms was not measured against a real device, and lyrics for a
  network sink were not checked (see [Later](#later-not-v1)).

**Creation (Forge GUI)**

- **A1 Page.** A Karaoke page of `forge-gui` builds a karaoke AC-3 from parts.
- **A2 Parts.** The parts are a backing (L and R), a guide melody M, and one or two vocals V1 and
  V2, each from its own file, or all from one multichannel WAV whose speaker mask names the
  channels (PR #1263 makes that mask the source of the layout).
- **A3 Mode.** The coding mode follows what is supplied, as Table C.2.1: L M R is 3/0, L R V1 is
  2/1, L M R V1 is 3/1, L R V1 V2 is 2/2, L M R V1 V2 is 3/2. The page shows the mode it chose and
  why.
- **A4 Stream.** The encoder is called with `bsmod=7` and the mode above; the page shows the
  equivalent `forge encode` command, as the Forge GUI does for its other plans.
- **A5 Levels.** The centre level (`cmixlev`, which the aware decoder applies to M at 2/0) and the
  surround level (`surmixlev`, which it applies to the vocals) are set on the page with Annex C's
  meaning stated beside them; the defaults are the encoder's.
- **A6 Hear.** The page plays the encoded stream back through the library's decoder under each
  reproduction of P2, so the author hears what a singer would. The preview is of the encoded
  stream, not of the source files.
- **A7 Save.** The output is a raw `.ac3`, or MP4 or Matroska where the encoder page offers
  them today.
- **A8 Check.** Parts of different lengths or rates, an LFE part (not in v1), a source that is
  already a 5.1 bed, and a vocal supplied without the mode that carries it are named, not
  silently fixed.

**Library and shared code**

- **D1** Neither program changes the decoder's arithmetic. If the design finds a gap in
  `libs/ac3` (for example, no way to ask which of M, V1 and V2 a frame carries without
  re-deriving it from `acmod`), the fix is a separate library PR, reviewed with the AC-3
  session that owns the decoder.
- **D2** The probe JSON and Hearth's media information name a karaoke stream's service as
  `karaoke` (`bsmod_label` already does) and need no new field unless the design shows one is
  missing.

### Not in v1

Stem files as a playback source, microphone input, scoring and recording; an LFE part in
authoring; Annex C's optional re-levelling of V1, V2 and M; a lyrics editor or a tool that times
words against a stream; estimated word times for a line that has none; karaoke in AC-4 (its
specification gives it no mode; E-AC-3 is [its own plan](karaoke-eac3.md)); video playback of a
karaoke MP4's picture; Android, a browser page and an ESP32 sink (see [Later](#later-not-v1)).

### Quality targets

- **Q1 Cost.** Decoding a karaoke stream under any reproduction takes no more than 5% longer than
  decoding the same stream as coded, measured on one generated 3/2 fixture on the existing
  `[hearth]` test engine.
- **Q2 Sync.** L6.
- **Q3 Accessible.** Every control reachable and operable from the keyboard and given an
  `accessibleName`, as Hearth's other controls are and as each page's own QML test checks; the
  lyric view's text scales with the window's text-size setting.
- **Q4 Localised.** Every new user-visible string goes through `qsTr()`/`tr()`, the catalogues
  are regenerated in the same PR, and the lupdate location gate passes.
- **Q5 CI.** No new lane or workflow; tests run under the existing `hearth`, `forge` and `ac3`
  labels.

### Verification

There was no outside oracle when this was written: no decoder known then reproduced karaoke, so a
misreading of an Annex C table would have been invisible to any comparison with another program.
Dolby's Reference Player's decoder does reproduce it, for AC-3 and for E-AC-3
([the findings](karaoke-eac3-findings.md)), and `tools/checks/verify_karaoke_oracle.py` compares
against it, locally. What stands in for it in CI:

| Checked | How | Requirements |
|---|---|---|
| The reproductions' coefficients | The library's existing tests, which transcribe Tables C.2.2 and C.2.3 independently of the code (the table pages were read as images) | D1 |
| Hearth plays what the library decodes | An engine test pushes a generated karaoke stream through `Player` on a fake device under each reproduction and compares the PCM with the library decode of the same stream, sample for sample | P2 to P5, P10 |
| What each output carries | Generated streams whose parts are tones at distinct frequencies, so an output's spectrum names the parts in it and their gains | P1, P4, S1 |
| Not karaoke, and coded outputs | Engine tests: an ordinary 3/2 stream with the controls set decodes as without them; passthrough and a coded sink leave the stream's bytes alone | P6, P7 |
| Fold and settings | `tst_decoder_settings`-style QML and controller tests for the disabled states, the Lt/Rt notice and persistence | P6 to P9 |
| Containers | The loader test opens the same stream raw, in MP4 and in Matroska and compares the decode | P11 |
| Lyrics | Unit tests of the reader over a file of cases (tags, offsets, several tags, no tags, bad encoding, word tags, a line without them) and a test of each word's state and fill at a given position, including a seek | L1 to L6 |
| Authoring | Build a stream from generated parts through the page's controller, probe it (`bsmod`, mode, levels), decode under each reproduction and check each part's place and gain against Annex C's tables | A1 to A8 |
| Pages | `qml.exe` or offscreen QML tests for each state, and screenshots re-cropped from the running app, not recalled | Design sheet |
| A listening pass | The owner on a desktop, recorded in the PR | S2, S3, S7 |

**Content.** No recording is bundled. Test and sample streams are generated (tones for the
parts, a synthetic `.lrc`), so there is no licence question and the generator is the record of
what is in them.

### Specification decisions

Put to the owner on 2026-10-11 with the answer the draft assumed; six were accepted as drafted,
and decision 3 was changed the same day from line level to word level. The alternatives are kept
so any of them can be reopened.

| # | Decision | Taken | Alternatives |
|---|---|---|---|
| 1 | Default reproduction for a karaoke stream | **Aware**: Hearth starts a karaoke stream with the vocals at the level the programme provider set, and the singer's chooser moves it to capable. The library's own default stays *off* | *As coded*: V1 and V2 then play from the surround speakers, which is wrong for a karaoke stream on a 5.1 layout. *Capable with the vocals muted*: what a singer most often wants, but it hides what the stream says |
| 2 | One page or a section (P2) | **A seventh Hearth page**, "Karaoke" (Ctrl+7), with the lyric view also reachable full-screen from Play; the design sheet settles the layout | A section of the Play page: fewer places to look, but the Play page is already 750 lines of QML and a lyric view wants the whole window |
| 3 | Lyrics timing (L2, L4, L6) | **Word level**, the owner's call (the draft had assumed line level): the enhanced LRC tags `<mm:ss.xx>` colour each word as sung, being sung or to come, and the word being sung fills as it is sung | *Line level*: a line is shown when its tag is reached; simpler, and what most LRC files in the wild carry. *Word level with estimated times for untagged lines*: spreads a line's words evenly over its duration, which looks like karaoke but is a guess. *None*: show the text as a static block |
| 4 | Lyrics encoding (L3) | **UTF-8**, with or without a byte-order mark; anything else is reported as unreadable | *UTF-8, and UTF-16 only when it carries a byte-order mark*: unambiguous and cheap, since the mark states the encoding. *Legacy code pages guessed* (Windows-1252, GBK, Shift-JIS are common in older LRC files): often wrong, and a wrong guess shows as garbled text. *A per-file encoding picker* in the UI |
| 5 | Authoring input (A2) | **Separate part files first**, one multichannel WAV second | The WAV first (it needs the speaker mask #1263 added, now merged); both together |
| 6 | LFE part in authoring (A8) | **None in v1** | An optional LFE part: the encoder supports it, the page and its checks grow |
| 7 | First PR | **The container loader (P11)**, not karaoke: it helps every Hearth user and needs no karaoke library code. It is PR #1274 | Karaoke first |

**Still open from decision 4.** UTF-16 with a byte-order mark (the alternative that is cheap and
unambiguous) was recommended and has not been answered; the reader takes UTF-8 only, as taken
above, and says so for a file that is not.

### Risks

- **Hearth cannot open MP4 or Matroska today** (P11). Small in code, but it is a change to the
  loader every item goes through.
- **The library's karaoke fields were new.** The reproduction had not been run end to end by
  anyone, and running it found two things: the layout a reproduced frame comes out in (the design's
  first finding, [karaoke-design.md](karaoke-design.md#the-reproduced-layout-the-finding), fixed in
  K1) and a cell of the part-to-speaker table (above).
- **Hot files.** `hearth_controller.cpp` (about 2,200 lines), Hearth's `Main.qml`, and the Forge
  GUI's `Main.qml` (about 7,700 lines) and `encoder_controller.cpp` are edited by many sessions;
  the Forge GUI's translation catalogues carry a line-number gate that restamps on unrelated
  edits. Each karaoke PR is small and rebased on a fresh `main`.
- **No oracle.** A shared misreading of an annex table would pass every test here. The table pages
  were read as images when the library was written; the tests were transcribed from them again.
  The applications' own tables are now decoded against the library for every mode and choice, which
  holds them to the decoder, not the decoder to anyone.
- **Passthrough surprises.** A singer who sends the stream to a receiver hears the vocals from
  the rear speakers unless the receiver is karaoke-capable. P7 states it rather than hiding it.
- **Stereo fold.** Hearth's Lt/Rt fold has no Annex C reproduction (P8); a user with it set
  will find karaoke "not working" unless the page says why.

## Later, not v1

Recorded so a later phase starts from the owner's words and the open questions, not from nothing.

- **ESP32 sink with a screen.** The owner's idea: an ESP32 dev kit that drives a display, I2S for
  the audio, so the screen shows the karaoke view and the sink plays the sound. The board is not
  chosen; on 2026-10-11 the owner said to **assume an ESP32-P4 with a panel connected directly**
  (the words used were HDMI, "UMD" and "ums" for the connector, read here as the P4's MIPI DSI
  panel interface, to be confirmed when the phase starts) and I2S audio as the sinks have today.
  Whether the existing Hearth sinks support karaoke is "possibly a choice to make later".
  Observations to start from: the P4 sink outputs stereo today (TDM waits for a later board
  revision), so the 2/0 reproduction fits it and the 3/0 one does not; and the questions it raises
  are where the reproduction happens (the host sends PCM, or the sink decodes coded AC-3 and needs
  the singer's choice sent to it), and how lyrics and the current line reach the board over
  Sendspin (the `_ac3forge_player@v1` extension role already carries the coded stream).
- **Android and a browser page.** Stretch goals. The browser page would need the karaoke fields in
  the C API and the WebAssembly binding.

### Found while building

Not in v1, and not on the lists above; each is a place a later phase can start, and
[phase 2](karaoke-phase2.md) is where they were taken up.

**Hearth**

- **Lyrics for the ESP32 and for a network group.** The board is still unchosen (a P4 with a panel,
  I2S, assumed); what is not known is where the singer's choice is applied (the host sends PCM, or
  the sink decodes coded AC-3 and is sent the choice) and how the current line and word reach it.
- **A Hearth option that opens an item for a capture.** `--shot` opens nothing, so a screenshot of a
  playing page needs a harness; a `--open <file>` would let the documentation be regenerated.
  *Built in PR #1361:* `--open`, `--seek` and `--smoke-shot`
  ([Hearth's command line](../docs/hearth/index.md#command-line)).
- **A choice kept per item** (P9 says every item; some singers want a duet's vocals remembered per
  song).
- **Seeking and a queue in the singer view**, and a lyric view beside the queue on the Play page.
- **Translations.** The 81 new Hearth strings and 106 Forge GUI strings are in the catalogues
  untranslated.

**Forge GUI**

- **The header and the rail still say "5.0".** A karaoke draft is a 5-channel AC-3 to them; they
  should say karaoke and the mode, as the design sheet drew it.
- **The default bit rate** for a 5-channel karaoke stream is the encoder's 192 kbps; the sheet
  showed 384.
- **The Parts step's mode card is below the fold** at 900 px, where the sheet had it in the rail.
- **The preview is a run** in the run strip, which a person did not ask for.
- **An LFE part** (decision 6) and **Annex C's re-levelling** of M, V1 and V2 (the library offers
  none).
- **A lyrics file with the stream.** Authoring writes the stream; the `.lrc` goes beside it by hand.
- **Edits in Expert between steps are overwritten** when a part changes, since the flow reloads
  its sources.

**Library, CLI and bindings**

- **The C API, the Python and Rust bindings and the WebAssembly build carry no karaoke fields**, so
  a browser page (a stretch goal above) has nothing to call.
- **`forge decode` is the only command that applies a reproduction**; `play` and `monitor` do not
  take `karaoke`.
- **The window and `forge` write a different last frame** for a source that is not a whole number
  of AC-3 frames (610 of 48,384 bytes, seen with 2.0 s parts); not investigated, and not specific to
  karaoke so far as is known.

**Docs**

- ROADMAP.md's Hearth table has two rows each for "Desktop player" and "ESP32 sinks", an older and
  a newer text of each.

## Open questions

Research, not specification: none of these is a v1 requirement.

1. **Microphone.** "How we deal with the microphone input is a question we need to understand."
   Two things to start the note from: a synchronised Sendspin group plays each chunk at a time
   after it is sent, so a singer monitoring their own voice would have to hear it locally and not
   through the group; and `libs/audio` has a live capture path, built for capturing a programme,
   whose fit for a microphone (latency, echo, where the voice is mixed) is part of the
   question.
2. **The picture of a karaoke MP4.** A karaoke MP4 may carry a video track; whether Hearth should
   play it is not decided and is not in v1.
3. **Lyrics inside the container.** A timed-text track in MP4 or Matroska is another source of
   lyrics; v1 reads the sidecar only.
4. **The ESP32 display.** The panel's interface and size, and whether the screen shows generated
   lyrics or a video, once the owner has chosen the hardware (the assumption is a P4; see
   [Later](#later-not-v1)).
5. **Where word-timed lyrics come from.** Word level (decision 3) needs the enhanced LRC form,
   which is rarer in the wild than line-level files. A tool that marks each word against the
   playing stream is the natural companion, and an estimate of word times for an untagged line is
   a guess the spec does not make; neither is in v1. The v1 test content is generated, with word
   tags.

## Working rules for this programme

- Agent usage is billed: no wave of agents without asking, and `get_usage` is read first.
- One branch type per change, named from `pr-gate.yml`'s pattern; PRs are pushed before they are
  opened; auto-fix is turned on at once; no attribution lines.
- New `qsTr()` and `tr()` strings regenerate the translation catalogues and satisfy the lupdate
  location gate in the same PR.
