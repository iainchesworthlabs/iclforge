# Karaoke, phase 2

!!! note "Status as of 2026-10-12: signed off by the owner; W1 to W11 and W13 are merged, W13's Crucible items (the CD+G picture, the media-session position source and events out over OSC) are merged, and W12 is built and not yet run on its kit"
    [Phase 1](karaoke.md) built Annex C karaoke as K0 to K6 (Hearth's Karaoke page, lyrics and
    singer view; the Forge GUI's Karaoke flow). This page is the plan for what phase 1 left, in
    the order the owner gave on 2026-10-11 ([Order](#order)). Each item below was a small pull
    request or a written finding; the builds that conflicted with one another landed together
    ([How it landed](#how-it-landed)). The owner signed the order off and answered
    the decisions on 2026-10-11 ([Decisions](#decisions)); two answers changed the plan (D3, where
    the sink decodes, and D8, UTF-16 for `.lrc`). The live status board is
    [ROADMAP.md](../ROADMAP.md).

    | Step | What | State |
    |---|---|---|
    | 0 | This page: scope, order, decisions | Signed off 2026-10-11 |
    | 1 | First items W1 to W3 (the last frame, the GUI polish, word timing as a design) | W1 merged (#1290); W2 merged (#1293); W3 designed in [the findings](karaoke-phase2-findings.md#w3-word-timing-the-shapes) |
    | 2 | Research W4 and W5 (lyrics for a network group; Android and a browser page) | Read from the code; written up in [the findings](karaoke-phase2-findings.md) |
    | 3 | Builds in [the approved order](#order), in small pull requests | W6 to W11 and W13a to W13f are merged, with W7d, the lyrics to a board ([the table below](#work-items)); the last of them landed on one branch ([How it landed](#how-it-landed)) |
    | 4 | W12, the ESP32-P4 with a panel | Planned and built on 2026-10-12 in [karaoke-p4x-panel.md](karaoke-p4x-panel.md): the screen is drawn and measured on a board with no panel. The v3.x kit arrived on 2026-10-20 and the image ran on it: the panel and touch start, and the glass, a tap and TDM are not yet checked |

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
4. **W12.** The ESP32-P4 with a panel: parked until the board was two weeks away, then planned
   and built in [its own page](karaoke-p4x-panel.md) (2026-10-12).

## How it landed

Most of the builds merged on their own: W1 (#1290), W2 (#1293), W13a (#1299),
W6's library and writer (#1300), W7's wire keys (#1305), host (#1308) and firmware (#1310), W8
(#1311), W9 (#1312), W10's library gains (#1313), W6's controller (#1303) and W11a (#1326). The rest
conflicted with one another on the CHANGELOG, the Hearth catalogues and the controller, so they
were merged into each other once, on one branch, and that branch is PR #1339: W6's timing dialog
(#1304), W10's sliders and the levels to a sink (#1317, #1327), W11b (#1328), W11c, W13b (#1329) and
its import of every text format, W13c (#1331), W13d, W13e and W13f (#1334). GitHub marked each of
those pull requests merged with it, except #1332 (ASS) and #1333 (MIDI karaoke), which were stacked
on other branches and were closed with a note that their commits are in `main`. W7d, the lyrics to
a board (#1340), merged on top of it.

## Work items

Size is in pull requests: **S** is one small PR, **M** is one PR of a few hundred lines with its
tests, **L** is two to four stacked PRs. "Needs" is what the item cannot start without.

| ID | Item | Size | Needs | State |
|---|---|---|---|---|
| W1 | **The last frame.** The Forge GUI and `forge encode` write a different last frame for a source that is not whole AC-3 frames; fix the side that is wrong | S | Nothing | **Merged, PR #1290**: the GUI was the side that is wrong ([below](#w1-the-last-frame)) |
| W2 | **Flow polish.** The header and rail say "5.0"; 192 kbps where the sheet showed 384; the mode card below the fold at 900 px; the Hear preview a run; Expert edits overwritten when a part changes; no `.lrc` beside a saved stream | M | Catalogue regeneration | **Merged, PR #1293**, each item held by a test that fails without it ([below](#w2-flow-polish)) |
| W3 | **Word timing, as a design.** Which of a tap editor, an estimate and an alignment, and where it lives | S | The owner's choice of shape | Evaluated in [the findings](karaoke-phase2-findings.md#w3-word-timing-the-shapes); recommendation [below](#w3-word-timing) |
| W4 | **Findings: lyrics for a network group.** Which position the lyric clock follows for a group, how a sink with a screen would get the line and the word, and how the choice reaches a sink that decodes for itself | S | Nothing | Read from `NetworkGroupSink`, `choose_sink_form` and the Sendspin roles: [the findings](karaoke-phase2-findings.md#w4-lyrics-for-a-network-group) |
| W5 | **Findings: Android and a browser page.** What the C API, the bindings and WebAssembly lack, and the smallest first slice | S | Nothing | Read from `libs/capi`, `bindings/*`, `apps/demos/*`: [the findings](karaoke-phase2-findings.md#w5-android-and-a-browser-page) |
| W6 | **Word timing, built** (shape per W3, D2) | L | W13a | **Merged.** The library and the writer (#1300), Hearth's controller (#1303) and the timing dialog (#1304, in #1339). The dialog also sets a line's singer (W11c), held by `test_theSingerOfALineAndTheNamesAreSetOnTheDialogAndSaved` in `apps/hearth/ui/tests/qml/tst_karaoke_timing.qml` |
| W7 | **The sink decodes locally where it can** (D3): the role's settings carry the karaoke choice, the board applies it and runs the lyrics itself; the host sends PCM only to a sink that cannot | M to L | W13a for the lyrics; D3 | **Merged.** The wire keys (#1305), the host's rule (#1308), the firmware (#1310) and, as W7d, the lyrics to the board (#1340): the document and an anchor on `_iclforge_player@v1`, followed on the board's own playout clock, which reports the line it is in. The firmware was verified on a real S3 against the host's decode, within 0.25 dB for each reproduction. W7d has a hidden live case, `network sinks live: a board that holds lyrics follows them with the sound` (`apps/hearth/engine/tests/test_network_live.cpp`; skipped without `ICLFORGE_HEARTH_LIVE_LYRICS_SINK`), which compares when the board reports each line with when its outputs first carry sound. The panel to draw the lines is built and drawn on a board with no panel ([below](#w12-the-esp32-p4-with-a-panel)). What an S3's player needs to start is in [the findings](karaoke-phase2-findings.md#what-the-build-found-on-an-s3-board) |
| W8 | **Karaoke in the browser decode demo** (the smallest slice W5 finds) | S to M | D4 | **Merged, PR #1311**: the WebAssembly build was run in Node against `forge decode`, all eight cases sample for sample |
| W9 | **An LFE part in authoring** (phase 1's decision 6) | M | Nothing; the encoder and the output stage already carry an LFE | **Merged, PR #1312** |
| W10 | **Annex C's re-levelling of M, V1 and V2** | M to L | A library change, reviewed with the AC-3 decoder's owner session | **Merged.** The library's gains (#1313), Hearth's levels and sliders (#1317) and the levels to a sink that decodes for itself (#1327, verified on a real S3) |
| W11 | **Duets** beyond the V1 and V2 choice that exists | M | The owner asking for it | **Asked for by the owner on 2026-10-12; merged.** The marker in `libs/lyrics` (W11a, #1326), Hearth's view (W11b, #1328) and the timing dialog's singer and names (W11c, in #1339) ([below](#w11-duets)) |
| W12 | **The ESP32-P4 with a panel** | L | The board | **Built; first run on the kit 2026-10-20, glass, touch and TDM not yet checked**: [the plan](karaoke-p4x-panel.md); the five questions are [answered below](#w12-the-esp32-p4-with-a-panel) |
| W13 | **Timed-text import.** The lyric module in a library, Crucible importing LRC; SubRip, Advanced SubStation Alpha, MIDI karaoke and CD+G as stretch | L (six PRs; the first is S to M) | D9 to D12; the owner's answer on what Crucible does with the text | Scoped in [the findings](karaoke-phase2-findings.md#w13-timed-text-beyond-lrc-and-crucible). W13a is merged (#1299), and so are W13b, Crucible's lyric strip with start-and-nudge as the position (D9, #1329), W13c SubRip (#1331), W13d ASS and W13e MIDI karaoke (in #1339) and W13f CD+G (a picture, no text, D11; #1334). Crucible's import takes all of the text formats: `test_aSubRipFileIsImportedAsTimedLines`, `test_anAssScriptIsImportedWithItsSyllableTimes` and `test_aMidiKaraokeFileIsImportedFromItsBytes` in `apps/crucible/ui/tests/qml/tst_lyrics.qml`. The three items the owner left after that (2026-10-12) are merged: the CD+G picture as shared code (#1360) and drawn in Crucible's strip (#1364), the media-session position source (#1365: Windows and Linux readers, each run against a stand-in player, and macOS's private framework, which no Mac has run) and events out over OSC, off by default (#1368); what each platform's media session gives is in [the findings](karaoke-phase2-findings.md#the-media-session-back-ends-as-found) |

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

**The fix, merged as PR #1290.** One function for "what a source supplies at sample `n`" (silence
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

**Since (2026-10-12).** The aligner recipe is written and was run end to end on one song:
`docs/karaoke/forced-alignment.md`. The lyric module did move, to `libs/lyrics` (W13a), so what a
GUI editor would still need is the session and the dialog, not the module; the three ways of giving
the Forge GUI a timing tool, with what each means for the lyric file it writes, are
[E19 in the findings](karaoke-phase2-findings.md#e19-timing-the-words-in-the-forge-gui-the-options),
and the choice is D13.

## Risks

- **The same hot files as phase 1.** `encoder_controller.cpp`, the Forge GUI's `Main.qml` and its
  catalogues (the lupdate location gate restamps on unrelated edits) are edited by many sessions;
  each PR here is small and merges a fresh `main` first.
- **Other sessions near this.** PR #1284 (the lyrics and the singer view) merged before this
  phase's builds; the example projects (#1291, #1319) and the project files (#1301, #1306) were
  other sessions' work in the same area, and are merged.
- **A changed byte stream.** W1 changes the last frame of what the GUI writes for a source that is
  not whole frames. A test that pinned the old bytes would be the only casualty, and none is known;
  the GUI suites' byte comparisons use whole frames.
- **W10 is a library change.** The output stage is shared by the CLI, Hearth, the GUI's player, the
  WebAssembly build and the ESP32 sinks; a new field is additive and defaults to today's behaviour,
  and it was reviewed with the AC-3 decoder's owner session before it was built (PR #1313).
- **No oracle for re-levelling either.** Annex C leaves it to implementers, so there is nothing to
  compare against; the tests hold the gains to the arithmetic and the table to the library's own
  decoder, as phase 1 did.

## Decisions

Put to the owner on 2026-10-11 with the answer the plan assumed, and answered the same day:
approved as assumed except **D3** (the sink decodes locally, with the server's PCM as the
fallback) and **D8** (UTF-16 is adopted), and **D10** was confirmed. Each item they govern was
built as answered; W12's questions are the only ones still open.

| # | Decision | Answer (2026-10-11) | Alternatives |
|---|---|---|---|
| D1 | The GUI pads a short last frame as `forge` does (holds the last sample), so the GUI's files for such a source change by their last frame (PR #1290, merged; it is a fix, asked because it changes bytes) | **Approved**: one policy for both, with a measured reason | Leave the GUI alone and document the difference: two programs of one family write different bytes from one plan, and the window's last frame is lower on some material |
| D2 | Word timing's shape: a Hearth tap editor, lines then words, with an estimate as the starting point; the aligner as a recipe | **Approved** | The editor in the Forge GUI (needs the lyrics module in `apps/shared` first); an estimate with no editor; nothing |
| D3 | Where a karaoke stream is reproduced for a sink | **Changed by the owner: the sink decodes locally where it can.** The stream goes to it as coded with the singer's choice in the role's settings, and the lyrics go to the board, which runs its own cursor; the server converts to PCM only for a sink that cannot decode karaoke (an older board, or one that lists no karaoke setting) | The first draft: Hearth always decodes a karaoke stream and sends PCM, so the group hears one reproduction (it stays as the fallback, and keeps a mixed group consistent) |
| D4 | Browser first, then nothing for Android until there is a player to put it in | **Approved**: the WebAssembly decoder demo gets the two fields and a panel; lyrics after | Skip the browser; or an Android player as a phase of its own |
| D5 | LFE part: one optional mono part, low-passed as the encoder does today | **Approved** | An LFE from a 5.1 source's own LFE channel only |
| D6 | Re-levelling: gains for M, V1 and V2 in the library's output stage and sliders on Hearth's Karaoke page | **Approved**, after the first items | Not built; Annex C calls it optional |
| D7 | Duets: no per-song memory, and nothing beyond V1 and V2 until asked | **Approved**; **asked for by the owner on 2026-10-12**, so the slice below is built (still no per-song memory) | The costed duet slice below |
| D8 | UTF-16 with a byte-order mark, in either byte order, for `.lrc` beside UTF-8 (carried over from phase 1's decision 4; the reader took UTF-8 only) | **Changed by the owner: adopted**, so that Hebrew and Yiddish files saved as "Unicode" are read. Built in W13a, with Hebrew and Yiddish test files | UTF-8 only, as phase 1 built it |
| D9 | What Crucible does with imported lyrics, and where their position comes from | **Approved: a lyric strip in Crucible's window for the application it is attached to; the position from that application's operating-system media session where it publishes one, and a start-and-nudge where it does not** | Events out (OSC) instead of, or as well as, a strip; the strip only, with no position source |
| D10 | "Mini karaoke" is read as **MIDI karaoke** (`.kar`, lyrics as MIDI meta events) | **Confirmed: MIDI karaoke** | Another format: say which |
| D11 | CD+G is read as an already-ripped `.cdg` file and shown as its picture; no drive ripping and no text extracted | **Approved**: a CD+G carries a 300 by 216 picture, not text | OCR of the picture for text (a model and its runtime; not planned) |
| D12 | The lyric reader and cursor move from `apps/hearth/engine` into a library before word timing's writer and Crucible's import are built | **Approved** | Leave them in Hearth and give Crucible its own parser: two readers of one format that cannot share a test |
| D13 | Whether the Forge GUI gets a word-timing tool, and how (added 2026-10-12) | **Open: the owner's call.** Recommended A now, B when Hearth opens a file it is handed ([E19 in the findings](karaoke-phase2-findings.md#e19-timing-the-words-in-the-forge-gui-the-options)) | A: leave it and point the GUI's Save step at Hearth (S). B: a button that opens the stream in Hearth (M). C: the session and dialog shared, the GUI timing against its own Hear step (L) |

### W11: duets

Beyond the V1 and V2 choice that exists, a duet could want three things, each separable: **names**
for the two singers on the page ("Anna", "Ben" in place of V1 and V2, typed, not remembered);
**a singer per lyric line**, which LRC has no standard marker for, so a convention of ours (a prefix
or a tag) that the reader would have to learn and the word-timing editor would have to write; and
**a duet view** that colours a line by singer and says which part the chosen vocal covers. Names
are S; the line marker and the view are M together. None of it needs a per-song memory, which the
owner thinks is "probably not a thing", and none is planned.

**Asked for on 2026-10-12**, with a second example, "Leave Her, Johnny", built by another
session as the fixture (V1 the shantyman, V2 the crew; PR #1319, merged). The slice, in three
pull requests:

- **W11a, the marker and the names in the library (#1326).** A line's singer is `V1:`, `V2:` or `V1+V2:` as
  the first thing after the line's time tags (or after its first word tag), in either case, with
  spaces round the `+` and after the colon. It is not part of the line's text. Anything else
  before a colon is text (`V3:` is a word), and a line with no marker is for nobody in particular,
  which a solo's every line is. A plain, untimed file may carry the markers too, so a duet's text
  can be marked up before it is timed. The names are `[v1:name]` and `[v2:name]` id tags. The
  writer (W6) emits both and `set_line_singer()` is what an editor calls. Chosen over `M:`/`F:`
  because the markers say which *vocal of the stream* sings the line, which is what the page needs
  to say which lines the playing vocal covers; and over a bracket tag (`[v1]`) because other
  players show an unknown bracket tag as text, where they show `V1: Leave her` as a legible line.
- **W11b, Hearth's view (#1328).** Two typed names on the Karaoke page (kept for the session, never per
  song; a typed name comes before the file's tag, and `V1` and `V2` before neither), the lyric view
  telling each line's singer by name and by side as well as by colour (colour is never the only cue,
  L4), and a line saying who sings it: the guide vocal that is playing, or you.
- **W11c, the editor (in #1339).** The timing dialog sets a line's singer as it times it, so a duet's file is
  written whole by the tool that times it.

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

### W12: the ESP32-P4 with a panel

Taken off the shelf on 2026-10-12, when the owner's kit (a P4 of chip revision v3.2 or newer at
400 MHz with a 7-inch 1024 by 600 capacitive panel on MIPI DSI) was on its way. The plan, the design
and the work are [karaoke-p4x-panel.md](karaoke-p4x-panel.md); the questions this section kept
before are answered there:

1. **Where the reproduction happens: decided (D3).** The sink decodes coded AC-3 and applies the
   singer's choice itself, with the server's PCM as the fallback for a sink that cannot. The output
   was the open part: a chip of revision v3.x opens TDM (the 160 MHz I2S clock), so the 3/0
   reproduction fits it where the v1.x board's stereo did not.
2. **How the words reach it: built (W7d).** `lyrics` and `lyrics_position` on
   `_iclforge_player@v1`.
3. **Which clock the board's cursor runs on: the board's own**, through the Sendspin host's time
   filter, which is what `iclforge::lyrics::Follower` answers on.
4. **What the screen shows when nothing is playing, and what a person at the TV controls from the
   board:** the board's name, address, network and pairing code; touch sets the lyric delay and the
   text size, and which vocals sound stays the server's until the role can carry a request back.
5. **The panel's size and its fonts:** 1024 by 600, Archivo, a static Noto Sans for Greek and
   Cyrillic and a static Noto Sans Hebrew in the image; a CJK face does not fit the 4 MB slots and
   is not in it.
