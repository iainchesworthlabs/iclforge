# Karaoke: playback in Hearth, creation in the Forge GUI

!!! note "Status as of 2026-10-11: Phase 0 (scoping) answered; the specification is next"
    Scoped on 2026-10-11 in one round of questions and answers, recorded under
    [Decisions](#decisions). Nothing in this plan is built. The live status board is
    [ROADMAP.md](../ROADMAP.md); each phase below carries its own status line.

    | Phase | What | State |
    |---|---|---|
    | 0 | Scoping decisions | Answered 2026-10-11 |
    | 1 | Specification (in this page) | Not started; needs the owner's sign-off |
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

- **Library** (PR #1267, stacked on #1263; both open on 2026-10-11, see
  [Dependencies](#dependencies-and-state-on-2026-10-11)): `OutputConfig::karaoke`
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

These follow from decision 2 and are open to correction at the specification's sign-off.

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
  the coding mode) and **PR #1267** (`feature/ac3-karaoke-3-0-reproduction`, stacked on it:
  Annex C's aware and capable decoders) are both **open, not merged**. `main`'s
  `docs/library/decoding.md` still says Annex C is not covered. Phase 3 either waits for them or
  stacks on #1267's branch; the specification and the design do not need them.
- **Other sessions are editing near this** on 2026-10-11: the E-AC-3 Annex E mixer and decoder,
  the GUI stream player and `forge monitor` (one E-AC-3 programme, associated-service mixing),
  and Hearth's network code. Each file a karaoke PR touches in `apps/hearth`, `apps/forge/gui`
  or `libs/ac3/src/decoder` is checked against the open PRs first and its owner is messaged
  before it is edited.
- **Hearth decides per sink whether a stream is sent coded or as PCM** (`choose_sink_form`, PR
  #1253 and the PRs stacked on it). That decision matters for karaoke over Sendspin; see
  [Later](#later-not-v1).

## v1 scope

Stated here as the scope the specification will make precise; nothing below is a requirement yet.

### Playback (Hearth, desktop)

- Open a karaoke AC-3 stream: raw `.ac3`, and AC-3 in MP4 or Matroska.
- Show that the stream is karaoke (`bsmod` 7, the coding mode, which of M, V1, V2 it carries).
- Reproduction: off (as coded), aware, or capable with the singer's choice of none / V1 / V2 /
  both, at 3/0, stereo or mono, taking the library's levels as they are.
- Timed lyrics from a sidecar `.lrc` file next to the stream, shown in step with playback.
- A reference-grade view of what the decoder did, in the manner of Hearth's Media and Decoder
  pages, and a singer-facing view with large lyrics.

### Creation (Forge GUI)

- A Karaoke page that builds a `bsmod=7` stream from separate parts (backing L and R, the guide
  melody, one or two vocals) through the existing encoder, with the level and the layout the
  annex gives, and a preview that plays it back through the same reproduction.
- What the page takes as input, how parts are mapped to L M R V1 V2, and how the result is
  checked are specification work (Phase 1).

### Not in v1

Stem files as a playback source, microphone input, scoring and recording; Annex C's optional
re-levelling controls; karaoke in E-AC-3 or AC-4 (their specifications give it no mode, and the
decoder never sets the flag for them).

## Later, not v1

Recorded so a later phase starts from the owner's words and the open questions, not from nothing.

- **ESP32 sink with a screen.** The owner's idea: an ESP32 dev kit that drives a display (the
  words used were HDMI, "UMD" and a TFT; which interface and which board are not pinned down), I2S
  for the audio, so the screen shows the karaoke view and the sink plays the sound. Whether the
  existing Hearth sinks support karaoke is "possibly a choice to make later". Questions it raises:
  where the reproduction happens (the host sends PCM, or the sink decodes coded AC-3 and needs the
  singer's choice sent to it), how lyrics and the current line reach the board over Sendspin
  (the `_ac3forge_player@v1` extension role already carries the coded stream), and which board
  has the display interface.
- **Android and a browser page.** Stretch goals. The browser page would need the karaoke fields in
  the C API and the WebAssembly binding.

## Open questions

1. **Microphone.** "How we deal with the microphone input is a question we need to understand."
   Two things to start the note from: a synchronised Sendspin group plays each chunk at a time
   after it is sent, so a singer monitoring their own voice would have to hear it locally and not
   through the group; and `libs/audio` has a live capture path, built for capturing a programme,
   whose fit for a microphone (latency, echo, where the voice is mixed) is part of the
   question. This is a research note in the specification, not v1 work.
2. **What the screen shows for MP4 / MKV.** v1 shows lyrics from a `.lrc` file. A karaoke MP4 may
   carry a video track; whether Hearth should play it is not decided and is not in v1.
3. **The reference view and the singer view: one page or two** (and how the Karaoke page relates
   to the Decoder page's controls). Design question for Phase 2.
4. **Default reproduction.** Annex C leaves a decoder free to be aware or capable. Which Hearth
   starts in, and whether a non-karaoke stream ever shows the controls, are specification
   questions.
5. **Authoring input.** What the Forge page takes: one multichannel file whose speakers are
   already L M R V1 V2 (the WAV speaker mask in #1263 makes that unambiguous), or separate mono
   and stereo files.
6. **Lyrics format limits.** LRC is informal: enhanced (word-level) tags, several time stamps per
   line, offsets, encodings. The specification names what v1 reads and what it ignores.

## Verification

There is no outside oracle. Checks are against Annex C's tables as transcribed independently in
the tests, the library's round trip through `forge encode ... bsmod=7` and `forge decode ...
karaoke`, QML tests of the pages, and a recorded manual pass of the player with a generated
stream. The specification states which requirement is checked how.

## Working rules for this programme

- Agent usage is billed: no wave of agents without asking, and `get_usage` is read first.
- One branch type per change, named from `pr-gate.yml`'s pattern; PRs are pushed before they are
  opened; auto-fix is turned on at once; no attribution lines.
- New `qsTr()` and `tr()` strings regenerate the translation catalogues and satisfy the lupdate
  location gate in the same PR.
