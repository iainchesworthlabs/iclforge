# Karaoke: playback in Hearth, creation in the Forge GUI

!!! note "Status as of 2026-10-11: scoping and the specification are signed off; the design is next"
    Scoped on 2026-10-11 in one round of questions and answers, recorded under
    [Decisions](#decisions); the specification's seven decisions were answered the same day
    ([Specification decisions](#specification-decisions)). Nothing in this plan is built. The live
    status board is [ROADMAP.md](../ROADMAP.md); each phase below carries its own status line.

    | Phase | What | State |
    |---|---|---|
    | 0 | Scoping decisions | Answered 2026-10-11 |
    | 1 | Specification (in this page) | Signed off 2026-10-11 (decisions 1 to 7 answered) |
    | 2 | Design: architecture, UX sheet, work breakdown into stacked PRs | Not started; needs the owner's sign-off |
    | 3 | Build, as small PRs | Not started; each wave of agents is asked about first |

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

- **Library** (PR #1267, stacked on #1263, which merged on 2026-10-11; #1267 was open that day,
  see [Dependencies](#dependencies-and-state-on-2026-10-11)): `OutputConfig::karaoke`
  (`KaraokeReproduction::kOff`, `kAware`, `kCapable`) and `OutputConfig::karaoke_vocals`
  (`KaraokeVocals::kNone`, `kV1`, `kV2`, `kBoth`), `MixLevels::karaoke`, `meta::is_karaoke()` and
  the four-argument `output_channel_count()`, in `libs/ac3/src/decoder/output.cpp` and
  `libs/ac3/include/iclforge/ac3/decoder/output.hpp`. `FrameDecoder` sets the flag from the
  frame's own `bsmod`; E-AC-3 and AC-4 never do.
- **CLI:** `forge decode in.ac3 out.wav karaoke` (aware) and `karaoke=none|v1|v2|v1+v2`
  (capable), `channels=2|1` for the 2/0 reproduction; `forge encode ... bsmod=7` with a 3/0 to
  3/2 layout writes a karaoke stream (L, C, R, Ls, Rs = L M R V1 V2).
- **Verification:** no outside decoder reproduces karaoke, so the library is checked against the
  annex's own tables (`libs/ac3/tests/decoder/test_output_stage.cpp`,
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
  the coding mode) **merged** on 2026-10-11 at 03:40Z. **PR #1267**
  (`feature/ac3-karaoke-3-0-reproduction`, stacked on it: Annex C's aware and capable decoders)
  was **open** two minutes later with `main` merged in, auto-merge on and its checks running.
  Until it lands, `main`'s `docs/library/decoding.md` still says Annex C is not covered. Phase 3's
  karaoke PRs wait for it or stack on its branch; the specification and the design do not need it.
- **Other sessions are editing near this** on 2026-10-11: the E-AC-3 Annex E mixer and decoder,
  the GUI stream player and `forge monitor` (one E-AC-3 programme, associated-service mixing),
  and Hearth's network code. Each file a karaoke PR touches in `apps/hearth`, `apps/forge/gui`
  or `libs/ac3/src/decoder` is checked against the open PRs first and its owner is messaged
  before it is edited.
- **Hearth decides per sink whether a stream is sent coded or as PCM** (`choose_sink_form`, PR
  #1253 and the PRs stacked on it). That decision matters for karaoke over Sendspin; see
  [Later](#later-not-v1).

## Specification

Phase 1. Drafted and signed off on 2026-10-11; nothing below is built.
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
- **P6 Not karaoke.** For a stream that is not karaoke, and for E-AC-3 and AC-4 (which the
  decoder never flags), the controls are shown disabled with the reason, and playback is as
  without them.
- **P7 Coded outputs.** When the output is passthrough or a network sink that takes the coded
  stream, the page says the reproduction is not applied by Hearth and what will play.
- **P8 Stereo fold.** Annex C defines no Lt/Rt reproduction. With the stereo fold set to Lt/Rt the
  page says so (the library leaves the Lt/Rt fold as it is) and offers Lo/Ro.
- **P9 Persist.** The choice is kept with the other decoder settings and restored on the next
  start; it applies to every item, as the decoder settings do, not per item.
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
- **L2 Read.** Time-tagged lines `[mm:ss.xx]text`, several tags on one line, `[offset:±ms]`, and
  the id tags `ti`, `ar`, `al`, `by` and `length` (shown or kept for display). Word-level tags in
  the enhanced form `<mm:ss.xx>` are not used for timing in v1; their text is shown without the
  tags.
- **L3 Encoding.** UTF-8, with or without a byte-order mark. A file that is not valid UTF-8 is
  reported as unreadable; it is not guessed at.
- **L4 Show.** The current line is emphasised, with the previous and the next lines in view. It
  follows the playing position, and a seek moves it. A file with no time tags is shown as plain
  text and is not followed.
- **L5 Fail soft.** A missing, empty or unreadable lyrics file shows "no lyrics" (with the reason
  where there is one) and never stops or delays playback.
- **L6 Time.** A line changes within 50 ms of the audio it belongs to being heard on a local
  output (the engine's heard position, not the decoded position). For a network sink the lyrics
  follow the same clock the sink is synchronised to (checked in design; see
  [Later](#later-not-v1)).

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
authoring; Annex C's optional re-levelling of V1, V2 and M; a lyrics editor; word-level lyric
timing; karaoke in E-AC-3 or AC-4 (the specifications give them no mode and the decoder never sets
the flag); video playback of a karaoke MP4's picture; Android, a browser page and an ESP32 sink
(see [Later](#later-not-v1)).

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

There is no outside oracle: no other decoder reproduces karaoke, so a misreading of an Annex C
table would be invisible to any comparison with another program. What stands in for it:

| Checked | How | Requirements |
|---|---|---|
| The reproductions' coefficients | The library's existing tests, which transcribe Tables C.2.2 and C.2.3 independently of the code (the table pages were read as images) | D1 |
| Hearth plays what the library decodes | An engine test pushes a generated karaoke stream through `Player` on a fake device under each reproduction and compares the PCM with the library decode of the same stream, sample for sample | P2 to P5, P10 |
| What each output carries | Generated streams whose parts are tones at distinct frequencies, so an output's spectrum names the parts in it and their gains | P1, P4, S1 |
| Not karaoke, and coded outputs | Engine tests: an ordinary 3/2 stream with the controls set decodes as without them; passthrough and a coded sink leave the stream's bytes alone | P6, P7 |
| Fold and settings | `tst_decoder_settings`-style QML and controller tests for the disabled states, the Lt/Rt notice and persistence | P6 to P9 |
| Containers | The loader test opens the same stream raw, in MP4 and in Matroska and compares the decode | P11 |
| Lyrics | Unit tests of the reader over a file of cases (tags, offsets, several tags, no tags, bad encoding, enhanced tags) and an engine test of the line at a given position | L1 to L6 |
| Authoring | Build a stream from generated parts through the page's controller, probe it (`bsmod`, mode, levels), decode under each reproduction and check each part's place and gain against Annex C's tables | A1 to A8 |
| Pages | `qml.exe` or offscreen QML tests for each state, and screenshots re-cropped from the running app, not recalled | Design sheet |
| A listening pass | The owner on a desktop, recorded in the PR | S2, S3, S7 |

**Content.** No recording is bundled. Test and sample streams are generated (tones for the
parts, a synthetic `.lrc`), so there is no licence question and the generator is the record of
what is in them.

### Specification decisions

Put to the owner on 2026-10-11 with the answer the draft assumed; all seven were accepted as
drafted. The alternatives are kept so any of them can be reopened.

| # | Decision | Taken | Alternatives |
|---|---|---|---|
| 1 | Default reproduction for a karaoke stream | **Aware**: Hearth starts a karaoke stream with the vocals at the level the programme provider set, and the singer's chooser moves it to capable. The library's own default stays *off* | *As coded*: V1 and V2 then play from the surround speakers, which is wrong for a karaoke stream on a 5.1 layout. *Capable with the vocals muted*: what a singer most often wants, but it hides what the stream says |
| 2 | One page or a section (P2) | **A seventh Hearth page**, "Karaoke" (Ctrl+7), with the lyric view also reachable full-screen from Play; the design sheet settles the layout | A section of the Play page: fewer places to look, but the Play page is already 750 lines of QML and a lyric view wants the whole window |
| 3 | Lyrics timing (L2) | **Line level.** A line is shown when its time tag is reached | *Word level*: the enhanced LRC tags `<mm:ss.xx>` fill each word as it is sung; needs a per-word layout and a finer clock, a later increment. *None*: show the text as a static block |
| 4 | Lyrics encoding (L3) | **UTF-8**, with or without a byte-order mark; anything else is reported as unreadable | *UTF-8, and UTF-16 only when it carries a byte-order mark*: unambiguous and cheap, since the mark states the encoding. *Legacy code pages guessed* (Windows-1252, GBK, Shift-JIS are common in older LRC files): often wrong, and a wrong guess shows as garbled text. *A per-file encoding picker* in the UI |
| 5 | Authoring input (A2) | **Separate part files first**, one multichannel WAV second | The WAV first (it needs the speaker mask #1263 added, now merged); both together |
| 6 | LFE part in authoring (A8) | **None in v1** | An optional LFE part: the encoder supports it, the page and its checks grow |
| 7 | First PR | **The container loader (P11)**, not karaoke: it helps every Hearth user and needs no karaoke library code | Karaoke first, stacked on #1267's branch |

### Risks

- **Hearth cannot open MP4 or Matroska today** (P11). Small in code, but it is a change to the
  loader every item goes through.
- **#1267 was not merged on 2026-10-11** (#1263 was). Hearth and the Forge GUI need the library's
  karaoke fields; stacking on #1267's branch works, and the first PR does not need them.
- **Hot files.** `hearth_controller.cpp` (about 2,200 lines), Hearth's `Main.qml`, and the Forge
  GUI's `Main.qml` (about 7,700 lines) and `encoder_controller.cpp` are edited by many sessions;
  the Forge GUI's translation catalogues carry a line-number gate that restamps on unrelated
  edits. Each karaoke PR is small and rebased on a fresh `main`.
- **No oracle.** A shared misreading of an annex table would pass every test here. The table pages
  were read as images when the library was written; the tests were transcribed from them again.
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

## Working rules for this programme

- Agent usage is billed: no wave of agents without asking, and `get_usage` is read first.
- One branch type per change, named from `pr-gate.yml`'s pattern; PRs are pushed before they are
  opened; auto-fix is turned on at once; no attribution lines.
- New `qsTr()` and `tr()` strings regenerate the translation catalogues and satisfy the lupdate
  location gate in the same PR.
