# Karaoke, phase 2

!!! note "Status as of 2026-10-11: signed off by the owner; W1 is merged, W2 is in review, the builds start with W13a"
    [Phase 1](karaoke.md) built Annex C karaoke as K0 to K6 (Hearth's Karaoke page, lyrics and
    singer view; the Forge GUI's Karaoke flow). This page is the plan for what phase 1 left, in
    the order the owner gave on 2026-10-11 ([Order](#order)). Each item below is a small pull
    request or a written finding, never a big bang. The owner signed the order off and answered
    the decisions on 2026-10-11 ([Decisions](#decisions)); two answers changed the plan (D3, where
    the sink decodes, and D8, UTF-16 for `.lrc`). The live status board is
    [ROADMAP.md](../ROADMAP.md).

    | Step | What | State |
    |---|---|---|
    | 0 | This page: scope, order, decisions | Signed off 2026-10-11 |
    | 1 | First items W1 to W3 (the last frame, the GUI polish, word timing as a design) | W1 merged (#1290); W2 in review (#1293); W3 designed in [the findings](karaoke-phase2-findings.md#w3-word-timing-the-shapes) |
    | 2 | Research W4 and W5 (lyrics for a network group; Android and a browser page) | Read from the code; written up in [the findings](karaoke-phase2-findings.md) |
    | 3 | Builds in [the approved order](#order), one pull request each | Approved; W13a first |
    | 4 | W12, the ESP32-P4 with a panel | **Parked** until the board arrives; only its questions are kept |

## What this phase is

Phase 1's own list of what it left ([Found while building](karaoke.md#found-while-building) and
[Later](karaoke.md#later-not-v1)) is the scope: the Forge GUI flow's rough edges and one real
defect, the lyrics' timing, three places the karaoke choice does not reach (a network group, a
browser, a phone), and the two authoring items phase 1 excluded on purpose (an LFE part, Annex C's
re-levelling). Nothing here changes the specification of phase 1; a requirement phase 1 set stays
as it was, and an item here that changes one says which.

The owner's words on 2026-10-11, which fix the order: "Start phase two, as a separate thing: grab
all of the phase two candidates." The ESP32 work is parked because the owner's ESP32-P4 with a
panel "is in the post" (about two weeks). Then, in this priority: lyrics for a network group
understood; the Android and browser page understood; word timing ("because I want to do that");
the LFE part in authoring and Annex C's re-levelling of M, V1 and V2; and maybe duets ("I don't
know if we need to remember that per song; probably not a thing"). "All the things listed as first
in phase two should be first."

Added later the same day, and "for the next phase that we're planning": **Crucible should allow the
importing of LRC / timed text** (the standard for karaoke), with **CD+G (the subchannel, ripped),
SubRip, Advanced SubStation Alpha and "mini karaoke"** as stretch goals "for getting the
text/lyric meta data and events". That is W13.

## Order

1. **First items (W1 to W3).** The three things phase 1's wrap-up named as first: a defect to
   find, the flow's polish, and word timing as a design.
2. **Research (W4, W5).** The two "understood" items. They are findings with sizes, not builds;
   what they recommend becomes a build (W7, W8) only on the owner's say.
3. **Builds, in the order the owner approved on 2026-10-11:** W13a (the lyric module into a
   library, reading UTF-8 and UTF-16); W6 (word timing, its writer written in that library); W7
   (the sink decodes locally where it can); W8 (the browser demo); W9 (the LFE part); W10
   (re-levelling, reviewed with the AC-3 owner session before it is built); W13b to W13f
   (Crucible's import, then SubRip and ASS, MIDI karaoke and CD+G); W11 (duets) only if asked.
   W13a is first because the writer, the sink firmware and Crucible all need the module where
   they can reach it ([W13](#w13-timed-text-import)).
4. **Parked (W12).** The ESP32-P4 with a panel.

## Work items

Size is in pull requests: **S** is one small PR, **M** is one PR of a few hundred lines with its
tests, **L** is two to four stacked PRs. "Needs" is what the item cannot start without.

| ID | Item | Size | Needs | State |
|---|---|---|---|---|
| W1 | **The last frame.** The Forge GUI and `forge encode` write a different last frame for a source that is not whole AC-3 frames; fix the side that is wrong | S | Nothing | **Merged, PR #1290**: the GUI was the side that is wrong ([below](#w1-the-last-frame)) |
| W2 | **Flow polish.** The header and rail say "5.0"; 192 kbps where the sheet showed 384; the mode card below the fold at 900 px; the Hear preview a run; Expert edits overwritten when a part changes; no `.lrc` beside a saved stream | M | Catalogue regeneration | **Built, PR #1293**, each item held by a test that fails without it ([below](#w2-flow-polish)) |
| W3 | **Word timing, as a design.** Which of a tap editor, an estimate and an alignment, and where it lives | S | The owner's choice of shape | Evaluated in [the findings](karaoke-phase2-findings.md#w3-word-timing-the-shapes); recommendation [below](#w3-word-timing) |
| W4 | **Findings: lyrics for a network group.** Which position the lyric clock follows for a group, how a sink with a screen would get the line and the word, and how the choice reaches a sink that decodes for itself | S | Nothing | Read from `NetworkGroupSink`, `choose_sink_form` and the Sendspin roles: [the findings](karaoke-phase2-findings.md#w4-lyrics-for-a-network-group) |
| W5 | **Findings: Android and a browser page.** What the C API, the bindings and WebAssembly lack, and the smallest first slice | S | Nothing | Read from `libs/capi`, `bindings/*`, `apps/demos/*`: [the findings](karaoke-phase2-findings.md#w5-android-and-a-browser-page) |
| W6 | **Word timing, built** (shape per W3, D2) | L | W13a | Approved; after W13a |
| W7 | **The sink decodes locally where it can** (D3): the role's settings carry the karaoke choice, the board applies it and runs the lyrics itself; the host sends PCM only to a sink that cannot | M to L | W13a for the lyrics; D3 | Approved; scoped in [the findings](karaoke-phase2-findings.md#sink-local-decode-the-owners-requirement); the audio side can be verified on the S3 boards before the panel arrives |
| W8 | **Karaoke in the browser decode demo** (the smallest slice W5 finds) | S to M | D4 | Approved |
| W9 | **An LFE part in authoring** (phase 1's decision 6) | M | Nothing; the encoder and the output stage already carry an LFE | After sign-off |
| W10 | **Annex C's re-levelling of M, V1 and V2** | M to L | A library change, reviewed with the AC-3 decoder's owner session | After sign-off |
| W11 | **Duets** beyond the V1 and V2 choice that exists | M | The owner asking for it | Not planned; costed [below](#w11-duets) |
| W12 | **The ESP32-P4 with a panel** | L | The board | **Parked**; [questions only](#w12-parked-the-esp32-p4-with-a-panel) |
| W13 | **Timed-text import.** The lyric module in a library, Crucible importing LRC; SubRip, Advanced SubStation Alpha, MIDI karaoke and CD+G as stretch | L (six PRs; the first is S to M) | D9 to D12; the owner's answer on what Crucible does with the text | Scoped in [the findings](karaoke-phase2-findings.md#w13-timed-text-beyond-lrc-and-crucible); not started |

## First items

### W1: the last frame

**What was seen.** With 2.0 s parts (62.5 AC-3 frames) the Forge GUI's encode and `forge encode`
of the same plan differed in the last frame only: 610 of 48,384 bytes (the count depends on the
signal; the fixture below gives 696). The byte-equality suites avoid it by using whole-frame
sources (1.024 s, 32 frames).

**Reproduced without karaoke.** A stereo 16-bit 48 kHz WAV of two tones, encoded at the default
192 kbps by `forge encode` and by `EncoderController::encodeTo` (the window's own Encode button
runs the same function), compared byte for byte:

| Source | AC-3 frames | `forge encode` | GUI | Differing bytes |
|---|---|---|---|---|
| 1.024 s, 49,152 samples (32 whole frames) | 32 | 24,576 | 24,576 | 0 |
| 2.0 s, 96,000 samples (62.5 frames) | 63 | 48,384 | 48,384 | 696, all in frame 63 |

**Cause.** The frame count and the encoder's state are the same on both sides; what differs is
the value a short final frame is filled with. `forge` holds the last real sample of each channel
for the rest of the frame (`apps/forge/cli/src/commands/encode.cpp`, `gather_frame` in
`multi_source.cpp`), and the GUI fills with zero (four loops in `encoder_controller.cpp`: the
channel encode, the object encode, and the two previews). Pre-padding the 2.0 s WAV by hand
settles it: `forge encode` of the WAV with 768 zeros appended is byte-identical to the GUI's
file, and `forge encode` of the WAV with its last sample appended 768 times is byte-identical to
`forge`'s own. The library's `FrameEncoder` pads nothing: it takes 1,536 samples, and the caller
decides what the tail is.

**Which side is wrong.** The GUI. The command line's padding was a deliberate, measured fix
(commit `e89f5f6c7`, August 2026): a hard drop to silence at the end of a file is itself a
transient the encoder's §8.2.2 detector spends a block switch on, which starved the last frame's
mantissa budget (that commit measured 23 to 28 dB a channel zero-padded against 67 to 88 dB held
on the 5.1 reference file). The GUI has no such reasoning for its zeros, and it is not karaoke's:
it applies to AC-3 and E-AC-3 and to the Atmos object encode alike.

**What it is worth, measured here** (source against decode over the last 1,536 real samples, the
decoder's 256-sample delay removed), because the commit's figures are one file's:

| Source | Zero-padded | Held |
|---|---|---|
| 5.1 reference file (78.125 frames), L / R / C / LFE / Ls / Rs, dB | 41.7 / 43.9 / 50.0 / 32.9 / 21.1 / 22.3 | 45.5 / 52.1 / 50.1 / 49.3 / 21.2 / 22.3 |
| Two-tone stereo, 2.0 s, L / R, dB | 72.4 / 68.2 | 71.4 / 66.5 |

Holding is never lower on the multichannel music file and up to 16 dB higher (the LFE); on a
pure-tone stereo file the two are within 2 dB, the zero-padded one slightly higher. So the case is
one policy for both programs, with a measured reason, and a real gain on the material it was found
on: not a claim that every file improves.

**The fix, built as PR #1290.** One function for "what a source supplies at sample `n`" (silence
before its offset, its own samples, then its last sample held), `fill_source_frame()` in
`apps/shared/media`, which `forge` and the GUI both compile. The GUI's four loops and `forge`'s four
whole-file copies call it, so they cannot drift again; `forge`'s streaming read paths are
untouched, and eight `forge` encodes (single, offset, multi-source, E-AC-3) are byte-identical to
the build before. The tests that hold it, and why the earlier suites missed it:
`test_source_frame.cpp` holds the function to its cases and fails with a zero hold; the karaoke flow
suite compares the window's bytes with `forge`'s for 2.0 s parts and fails with the GUI's encode
loop put back to zeros, while its 1.024 s parts pass either way. One decision for the owner
([D1](#decisions)): the GUI's files for such sources change by their last frame.

### W2: flow polish

Each item is from the phase-1 screenshots and is small; they are one pull request unless a catalogue
conflict forces two.

| Item | Where | What changes |
|---|---|---|
| The header and rail say "5.0" | `EncoderController::channelShapeName()` and `Main.qml`'s `planLineCore` | A karaoke draft is named for what it is, "Karaoke 3/2" or "Karaoke 2/1", as the sheet drew it |
| 192 kbps for a 5-channel stream; the sheet had 384 | `KaraokeFlow.qml`, where the parts decide the mode | The flow sets the rate when the mode is chosen, and leaves a rate the author has set alone |
| The Parts step's mode card is below the fold at 900 px | `KaraokeFlow.qml` | The card moves into the rail, where the sheet had it |
| The Hear preview shows up as a run in the run strip | `stream_player_controller.*`, `encoder_controller.*` | The preview is not recorded as a run |
| Edits made in Expert between steps are overwritten when a part changes | `KaraokeFlow.qml` reloads its sources | A part change touches the sources and the mode, and leaves the draft's other settings |
| No `.lrc` goes beside a saved stream | `KaraokeFlow.qml` | An offer, not a copy: if a `.lrc` of the part's name is beside a part, the page says so and writes it beside the stream, as Hearth's loader finds it |

The translation catalogues change: `forge-gui_lupdate` runs before the push, because the CI gate is
`git diff --exit-code -I '<location'` after it.

### W3: word timing

Phase 1's open question 5: word-level lyrics (decision 3) need the enhanced LRC form, which is the
rarer one in the wild, so the owner's own songs will mostly have line times or none. The three shapes
and the recommendation are in the findings; the shape of it:

- **A tap editor** plays the stream and records a key press per word, saved as enhanced LRC. It
  makes the data; nothing else does.
- **An estimate** spreads a line's words over the line's span by their letters. Phase 1 refused to
  show that as if it were known; as a starting point inside an editor, marked as a guess to be
  corrected, it is what makes the tapping bearable.
- **A forced alignment** against the audio needs a speech model and its runtime, a dependency this
  project has none of, and singing is its hard case. A karaoke stream does help (the lead vocal is
  a channel of its own), but the answer is a recipe for an outside tool that writes a `.lrc`, not
  code here.

**Recommended:** the editor in Hearth, tapping lines first and words second, with the estimate as
the words' starting point; the aligner as a documentation recipe only. Hearth is where the engine's
heard position is, and a program may not use another program (`tools/checks/projects.json`), so an
editor in the Forge GUI would have the lyrics module moved into `apps/shared` first, which only a
second user of it would justify.

## Risks

- **The same hot files as phase 1.** `encoder_controller.cpp`, the Forge GUI's `Main.qml` and its
  catalogues (the lupdate location gate restamps on unrelated edits) are edited by many sessions;
  each PR here is small and merges a fresh `main` first.
- **Other sessions near this.** PR #1284 (the lyrics and the singer view) is still open, and the
  session adding an Auld Lang Syne example project works in the same area; the owners are messaged
  before their files are touched.
- **A changed byte stream.** W1 changes the last frame of what the GUI writes for a source that is
  not whole frames. A test that pinned the old bytes would be the only casualty, and none is known;
  the GUI suites' byte comparisons use whole frames.
- **W10 is a library change.** The output stage is shared by the CLI, Hearth, the GUI's player, the
  WebAssembly build and the ESP32 sinks; a new field is additive and defaults to today's behaviour,
  and it is reviewed with the AC-3 decoder's owner session before it is built.
- **No oracle for re-levelling either.** Annex C leaves it to implementers, so there is nothing to
  compare against; the tests hold the gains to the arithmetic and the table to the library's own
  decoder, as phase 1 did.

## Decisions

Put to the owner on 2026-10-11 with the answer the plan assumed, and answered the same day:
approved as assumed except **D3** (the sink decodes locally, with the server's PCM as the
fallback) and **D8** (UTF-16 is adopted), and **D10** was confirmed. Each can still be reopened
before its item starts.

| # | Decision | Answer (2026-10-11) | Alternatives |
|---|---|---|---|
| D1 | The GUI pads a short last frame as `forge` does (holds the last sample), so the GUI's files for such a source change by their last frame (PR #1290, built; it is a fix, asked because it changes bytes) | **Approved**: one policy for both, with a measured reason | Leave the GUI alone and document the difference: two programs of one family write different bytes from one plan, and the window's last frame is lower on some material |
| D2 | Word timing's shape: a Hearth tap editor, lines then words, with an estimate as the starting point; the aligner as a recipe | **Approved** | The editor in the Forge GUI (needs the lyrics module in `apps/shared` first); an estimate with no editor; nothing |
| D3 | Where a karaoke stream is reproduced for a sink | **Changed by the owner: the sink decodes locally where it can.** The stream goes to it as coded with the singer's choice in the role's settings, and the lyrics go to the board, which runs its own cursor; the server converts to PCM only for a sink that cannot decode karaoke (an older board, or one that lists no karaoke setting) | The first draft: Hearth always decodes a karaoke stream and sends PCM, so the group hears one reproduction (it stays as the fallback, and keeps a mixed group consistent) |
| D4 | Browser first, then nothing for Android until there is a player to put it in | **Approved**: the WebAssembly decoder demo gets the two fields and a panel; lyrics after | Skip the browser; or an Android player as a phase of its own |
| D5 | LFE part: one optional mono part, low-passed as the encoder does today | **Approved** | An LFE from a 5.1 source's own LFE channel only |
| D6 | Re-levelling: gains for M, V1 and V2 in the library's output stage and sliders on Hearth's Karaoke page | **Approved**, after the first items | Not built; Annex C calls it optional |
| D7 | Duets: no per-song memory, and nothing beyond V1 and V2 until asked | **Approved** | The costed duet slice below |
| D8 | UTF-16 with a byte-order mark, in either byte order, for `.lrc` beside UTF-8 (carried over from phase 1's decision 4; the reader took UTF-8 only) | **Changed by the owner: adopted**, so that Hebrew and Yiddish files saved as "Unicode" are read. Built in W13a, with Hebrew and Yiddish test files | UTF-8 only, as phase 1 built it |
| D9 | What Crucible does with imported lyrics, and where their position comes from | **Approved: a lyric strip in Crucible's window for the application it is attached to; the position from that application's operating-system media session where it publishes one, and a start-and-nudge where it does not** | Events out (OSC) instead of, or as well as, a strip; the strip only, with no position source |
| D10 | "Mini karaoke" is read as **MIDI karaoke** (`.kar`, lyrics as MIDI meta events) | **Confirmed: MIDI karaoke** | Another format: say which |
| D11 | CD+G is read as an already-ripped `.cdg` file and shown as its picture; no drive ripping and no text extracted | **Approved**: a CD+G carries a 300 by 216 picture, not text | OCR of the picture for text (a model and its runtime; not planned) |
| D12 | The lyric reader and cursor move from `apps/hearth/engine` into a library before word timing's writer and Crucible's import are built | **Approved** | Leave them in Hearth and give Crucible its own parser: two readers of one format that cannot share a test |

### W11: duets

Beyond the V1 and V2 choice that exists, a duet could want three things, each separable: **names**
for the two singers on the page ("Anna", "Ben" in place of V1 and V2, typed, not remembered);
**a singer per lyric line**, which LRC has no standard marker for, so a convention of ours (a prefix
or a tag) that the reader would have to learn and the word-timing editor would have to write; and
**a duet view** that colours a line by singer and says which part the chosen vocal covers. Names
are S; the line marker and the view are M together. None of it needs a per-song memory, which the
owner thinks is "probably not a thing", and none is planned.

### W13: timed-text import

The owner's request is Crucible importing LRC, with four more formats as stretch goals. What reads
LRC today is Hearth's `apps/hearth/engine/src/lyrics.*`, and that is the first fact that shapes it:
**a program may not use another program** (`tools/checks/projects.json`), and Crucible's `may_use`
does not list Hearth. The module has to leave Hearth's engine to be used anywhere else, and the
places that want it are four: Hearth, Crucible, the sink firmware (W12) and the browser (W8).
Only a project under `libs/` is reachable from all four, which is also where the word-timing
writer (W6) should be written.

The second fact is about Crucible: it taps applications and places them in a room; it knows each
one's audio, and **nothing in Crucible or `libs/audio` reads a track's position** (no media
session, no file). Importing a `.lrc` there needs a position source before it can follow anything.
[The findings](karaoke-phase2-findings.md#w13-timed-text-beyond-lrc-and-crucible) set out the
candidates, the five formats (what each carries and how it is timed), and the sizes.

| Step | What | Size |
|---|---|---|
| W13a | The reader and cursor into a library (name, namespace, `projects.json` row, tests moved; Hearth includes it from there) | S to M |
| W13b | Crucible imports an `.lrc` and shows it against a chosen application, with the position source D9 settles | M to L |
| W13c | SubRip (`.srt`): cues with a start and an end, no words | S |
| W13d | Advanced SubStation Alpha (`.ass`): events, with the karaoke tags that give a syllable's duration | M |
| W13e | MIDI karaoke (`.kar`): the Lyric and Text meta events of a Standard MIDI File, through its tempo map | M |
| W13f | CD+G (`.cdg`): the packet stream decoded to its 300 by 216 picture and timed against the audio; no text exists in it | M |

W13c to W13f are the owner's stretch goals and each is independent of the others, so any can be
dropped; W13a is the only one the rest of the phase also wants.

**Hebrew and Yiddish** (the owner's reason for D8). W13a reads UTF-8 and UTF-16 (with a byte-order
mark, either order), and its tests carry generated Hebrew and Yiddish lines in both encodings, so
there is no licence question. UTF-8 already carried those scripts; what UTF-16 adds is the files an
editor saved as "Unicode". Two things are not covered by an encoding: an 8-bit legacy code page
(Windows-1255 is the Hebrew one) is still refused rather than guessed at; and the lyric view has
to draw right-to-left text with a word fill that runs right to left within a word, which is a
check in the Hearth lyric view and the board's panel (a font with the glyphs, and a layout that
shapes a right-to-left line), not a parser matter.

### W12: parked, the ESP32-P4 with a panel

Only the questions are kept; no firmware is started. The board's panel is assumed to be on the P4's
MIPI DSI interface, with I2S audio as the sinks have today (phase 1's [Later](karaoke.md#later-not-v1)).

1. **Where the reproduction happens: decided (D3).** The sink decodes coded AC-3 and applies
   the singer's choice itself, with the server's PCM as the fallback for a sink that cannot. What
   is left is the output: the P4 sink outputs stereo today (TDM waits for the v3.x board), so
   the 2/0 reproduction fits it and the 3/0 one does not until that board is in.
2. **How the words reach it.** The `_ac3forge_player@v1` role already carries the coded stream; it
   has no field for lyrics, and `metadata@v1`, the upstream role that carries the title and a
   progress anchor, has none either. The findings list the shapes.
3. **Which clock the board's cursor runs on.** Its own playout clock, which knows its own output
   delay, against the host's, which does not.
4. **What the screen shows when nothing is playing**, and what a person at the TV controls from
   the board itself (the choice of vocals, the delay).
5. **The panel's size and its fonts**: a CJK line needs a font the board has flash for.
