# Karaoke, phase 2: findings

!!! note "Status as of 2026-10-11: read from the code on `main` at `c6aef2b00`; nothing here is built"
    The research behind [the phase 2 plan](karaoke-phase2.md): [word timing](#w3-word-timing-the-shapes)
    (W3), [lyrics for a network group](#w4-lyrics-for-a-network-group) (W4) and
    [Android and a browser page](#w5-android-and-a-browser-page) (W5). Each ends in a
    recommendation with a size, and the plan's decisions D2 to D4 are the owner's answers to them.
    Where a claim was measured it says so; the rest is read from the source and says that, so a
    later reader can tell which to trust and which to run. The sizes are the plan's: **S** is one
    small pull request, **M** one of a few hundred lines with its tests, **L** two to four stacked.

## W3: word timing, the shapes

Word-level lyrics (phase 1's decision 3) need the enhanced LRC form, which the reader takes
(`<mm:ss.xx>` before each word) and which is the rarer one in the wild. The owner's songs will mostly
arrive as line-timed files or as plain text, so the question is where word times come from.

### The shapes

| Shape | What it makes | What it costs | Verdict |
|---|---|---|---|
| **(i) A tap editor** | Word starts recorded as a person presses a key while the stream plays | A writer beside the reader, a tap model, a page; no new dependency | **Build it** |
| **(ii) An estimate** | A line's words spread over the line's span by their letters | A few dozen lines; the guess is wrong by up to a few hundred ms a word | **As the editor's starting point only**, marked as a guess |
| **(iii) Forced alignment** | Word times from the audio against the text | A speech model and its runtime, a language list, and a non-oracle: singing is the hard case for a speech aligner | **A documentation recipe, not code here** |

**On (iii).** A karaoke stream does make alignment easier than most, since the lead vocal is a
channel of its own, but an aligner is a model of tens to hundreds of megabytes and a runtime to run
it, in a project whose libraries have no such dependency, and there is no oracle for whether its
times are right: the only check is a person listening. What the repository can do is read the file
an outside tool wrote. A page of the user guide that names one workflow (extract V1 with
`forge decode ... karaoke=v1` to a WAV, align it with the tool of the reader's choice, put the
`.lrc` beside the stream) is the whole of it.

**On (ii).** Phase 1 refused to show estimated word times as if known (spec decision 3), and that
stands: nothing estimated is ever drawn as sung. Inside an editor it is a different thing: the
estimate is where the words sit before the person has corrected them, drawn differently from a
tapped word, and what makes a second pass a matter of nudging rather than of tapping every word
again.

### The recommended shape

An editor with two passes over one stream:

1. **Lines.** Plain text (or a line-timed file) is shown with the next line highlighted; a key
   press stamps that line's start as the stream plays, one press per line. This alone turns an
   untimed lyric into a line-timed file, which the reader and the lyric view already take.
2. **Words.** Each line's words start at the estimate (by letters across the line's span), drawn as
   a guess. The person plays the line again and taps each word's start, or drags a word's start on
   a strip, and the guess becomes a tap. A line with no taps stays a line-timed line and is saved
   as one.

Three details that decide whether it is usable:

- **Reaction time.** A person presses 100 to 250 ms after the sound. The editor subtracts a
  reaction offset (a setting, with a default and a two-line calibration: press with a metronome)
  from every tap before it is stored, and shows it. Stored times are in *audio time*, the engine's
  heard position, which is the clock every player follows, so the file plays correctly on any
  device.
- **A tap's clock.** The engine's heard position is published every 60 ms and `LyricClock`
  interpolates it per frame (`LyricClock`, PR #1284); a tap reads that interpolated time, which
  is good to a frame. The person's jitter is larger than the clock's.
- **The file.** The writer emits what the reader reads (`[mm:ss.xx]` per line, `<mm:ss.xx>` per
  tapped word, the id tags it was given, `[offset:]` untouched) at its 10 ms resolution, and a
  round trip (read, write, read) is the test. A word tap never changes the text.

### Where it lives

**In Hearth**, as a mode of the Karaoke page. It needs two things only Hearth has: the engine's
heard position and a player that follows the stream from the same decode a singer hears. A program
may not use another program (`tools/checks/projects.json`: `hearth` and `forge` list each other in
neither `may_use`), so an editor in the Forge GUI would first need the lyrics module moved out of
`apps/hearth/engine/src/lyrics.*` into `apps/shared` as a new app-library project with its row,
its lanes and its test binary: the cost of the move is the cost of a second user, and there is
none today. The writer goes in `lyrics.*` beside the reader, Qt-free.

Hearth's own rule that it plays and the Forge GUI creates (phase 1's decision 2) gets one
exception, and it is honest to say so: *timing lyrics to a recording* is not encoding, it is
playing along, and it is the one authoring function that needs a player's clock.

### Size and order

**L**, in three pull requests: (1) the writer and a tap model in the engine, with the round-trip
and estimate tests (Qt-free, no UI); (2) `KaraokeController` taking taps and saving, with the
reaction offset; (3) the editor on the Karaoke page, its strings, its catalogue and its
screenshots. The estimate is part of (1).

## W4: lyrics for a network group

### Which position the lyric clock follows

Locally, the lyric clock is `PlayPosition::heard`: the device's frames played less the output
path's latency (`Player::heard_frames()`), polled every 60 ms and interpolated per frame
(`LyricClock`). That is the sound leaving the device, which is what a person hears.

For a **network group** the output is `NetworkGroupSink`, and its `position()` is different in kind
(`apps/hearth/engine/src/network_group_sink.cpp`, read, not measured):

- `frames_played` is the group's timeline run through: `(server clock now - group start_time())`
  at the sample rate, never more than the frames taken, less what a flush skipped. The group plays
  frame *n* at a fixed time on its own timeline and every member plays it then, so "taken" is not
  "heard": the source is read well ahead (a second and a half, and the members' lead).
- `latency_frames` is **0**. Nothing in the position knows a member's own output path.

So for a group, `heard` is the frame every member is *scheduled to put out at its audio port*
now, on the server's clock. Is that the right clock for a lyric view on the host's screen? Yes, to
within what the group's own alignment already promises: the role says a sink plays a chunk's first
sample "at its audio port at that time" with `output_delay_ms` subtracted
([timing](hearth-sendspin-extension.md#timing)), so a person who has set each sink's output delay
to line the room up (a receiver's lip-sync delay, say) has already moved every port's time the way
the sound needs, and the host's clock is the room's. Three things it does not give:

1. **A seek runs ahead of the sound.** `flush()` restarts the count from zero, and members
   "briefly finish what they were already sent": the group's own comment on `flush()`. The cursor
   jumps to the new position at once, and the sound follows once the old audio is out, up to the
   buffered lead. The lyrics lead the sound by that long after a seek, and then agree.
2. **A screen on a sink is not the host's.** A display on a board should follow the board's own
   playout clock, which knows its own latency; see below.
3. **It was not measured.** The words follow `PlayPosition::heard` for any output, so a group works
   without a line of code; that the group's clock and the room's agree to the 50 ms of L6 has not
   been checked with a person and a camera. It is the first check W7 makes.

### How a sink with a screen would get the line and the word

What exists on the wire (`libs/sendspin`, read): the upstream roles `metadata@v1` (title, artist,
album, artwork URL, year, track, and a **progress anchor**: the position at a server timestamp and
a playback speed, which is exactly "where in the item at this time"), `artwork@v1` (an image per
channel, timestamped) and `visualizer@v1` (timestamped frames to show at a local time, with a
buffer capacity). **None of them has a lyrics field.** And none runs today: the server half
(`Group::set_metadata`, `set_artwork`, `push_visualizer`) is exercised by
`apps/hearth/engine/tests/test_group.cpp` only; Hearth's engine never calls it, and no sink lists
those roles (`player_session.hpp`: "which Hearth's sinks do not").

| Shape | How the sink gets the line | For | Against |
|---|---|---|---|
| **A. The document and an anchor** | The lyric document (lines, word starts, once per item) and `metadata@v1`'s progress anchor; the sink runs the cursor on its **own playout clock** | Exact to the board's own latency; survives a lossy link (one document, then anchors); a pause and a seek are one anchor | A parser and cursor on the board; an extension for the document |
| **B. "Now" events** | The host sends the current line and word as timestamped events, as `visualizer@v1` sends frames | No parser on the board; reuses a role's shape | Many small messages; a lost one shows as a wrong word until the next; the host's clock, not the board's |
| **C. Pictures** | Each line rendered on the host and sent as `artwork@v1`, timestamped | No protocol change at all; a board that shows artwork already works | A line is a few tens of kilobytes of JPEG; no word fill beyond one image a word; the board's fonts and language are bypassed |

**Recommended: A**, with C as a prototype that proves the screen before the protocol exists. Two
findings shape A:

- **The reader cannot be compiled into the firmware as it stands.** `lyrics.*` is in
  `apps/hearth/engine`, and `firmware/hearth-sink` may use `ac3`, `device`, `esp-idf`, `render` and
  `sendspin` only (`tools/checks/projects.json`). The cursor and the reader would move to
  `libs/device` (header-only, host-tested, "the device-side logic that needs no SDK", which is
  already where the sink planner and the pairing records live) or to `libs/sendspin`, and Hearth's
  engine would include it from there. That is a move with its tests, **S** to **M**, and it is
  what a firmware parser needs whichever shape is chosen.
- **The anchor is the upstream one.** Feeding `metadata@v1` with a progress (Hearth sets it today
  for nobody) makes every Sendspin player that lists the role show the title and a progress bar, as
  a side effect; the lyric document is the only part that is ours, a small object beside
  `_iclforge_player@v1`'s others or a role of its own, versioned as the role is.

### The choice reaching a sink that decodes for itself

`choose_sink_form()` has no notion of karaoke: `StreamNeeds` carries the stream's form, rate,
coded channels, objects and default layout, and nothing about whether the item is a karaoke stream
or whether the singer has chosen a reproduction (`apps/hearth/engine/src/sink_form.hpp`). What
that does, read from the code:

- A sink that takes AC-3 coded is sent the stream **as it is**. Its own decoder runs with the
  library's defaults, which are `KaraokeReproduction::kOff`: the vocals come out of a 5.1 layout's
  surrounds, or are folded into a 2.0 layout at the stream's surround level. The singer's choice
  is not applied there, and the Karaoke page says so (requirement P7).
- A sink that is sent PCM gets the host's decode, which **does** apply the choice, since the
  player's decoder takes it.
- So **a mixed group plays two different things**: its coded members as coded, its PCM members
  reproduced. That is the finding that matters, since a group is how the owner will use it.

Two ways to close it:

| Option | What | Size | Notes |
|---|---|---|---|
| **(a) Host-side** | `StreamNeeds` gains "the reproduction is applied here"; for such an item `choose_sink_form` prefers PCM (or Opus where the sink is set to it), so every member hears one reproduction | **S** | A table row and a flag; the group already renders each width once, and a reproduction is 2.0 or 3.0 (+ LFE), no new width. A C6 is offered PCM with its caveat as it already is; passthrough cannot do it at all (P7) |
| **(b) Sink-side** | `karaoke` and `karaoke_vocals` join the role's `decoder_settings` (11 keys today, none karaoke), the sink lists which it supports, and its decoder takes them | **M** | A protocol revision, firmware and library changes, Hearth's per-sink settings, tests; its payoff is a coded stream's bandwidth and a sink with an Atmos decode, neither of which a 2.0 karaoke stream needs |

**Recommended: (a) first**, then A above. A P4 with a panel that is fed the host's PCM and a lyric
document needs nothing more from the audio side, and (b) stays an option for the day a sink must
decode coded. This is W7 in the plan: **S** to **M**, the flag, the rule, the table, a
mixed-group test and the 50 ms check above.

## W5: Android and a browser page

### What each surface has

| Surface | Karaoke today | What it lacks |
|---|---|---|
| The library (`libs/ac3`) | The whole of it: `OutputConfig::karaoke` and `karaoke_vocals`, `is_karaoke`, `output_channel_count` and `output_mode` | Annex C's re-levelling (W10) |
| `forge decode` | `karaoke` and `karaoke=none\|v1\|v2\|v1+v2`, `channels=2\|1` | `play` and `monitor` do not take it |
| C API (`libs/capi`) | `bsmod` and `acmod` of a scanned stream are readable, so a caller can tell it is karaoke (`bsmod` 7, `acmod` above 1/0) | **The AC-3 decoder has no output configuration at all** (no fold, no karaoke): only AC-4 has `iclforge_ac4_output_config_t`. The encoder takes no `bsmod`, so a stream cannot be authored through it either |
| Rust | Over the C API | The same |
| Python | `bsmod` and `bsmod_present` on the frame headers and the decoded frame | No output stage and no encoder `bsmod` in the binding |
| WebAssembly (`apps/demos/wasm`) | `PushDecoder(foldTarget, applyDialnorm, mixLfe)` builds an `OutputConfig` | No `karaoke` or `karaoke_vocals`; the result carries no `bsmod`; `output_channel_count` is called without the karaoke flag |
| The npm package (`bindings/js`) | `FoldOptions { target, applyDialnorm, mixLfe }` | The two fields; `PushMetadata` has no `bsmod` |
| The Android demo (`apps/demos/android`) | None | It is a passthrough and object-motion demo for the Shield: it **does not decode to PCM**, so there is nothing to apply a reproduction in; the receiver reproduces |

### The smallest first slice

**The browser decode demo**, which already runs the library's decoder in the page and already
builds an `OutputConfig`:

1. `decoder_bindings.cpp`: two more constructor arguments (reproduction, vocals), `OutputConfig`
   filled with them, the frame's `bsmod` passed to `output_channel_count`, and `bsmod` (or a
   `karaoke` flag) in the push result. About 30 lines.
2. `bindings/js`: `FoldOptions` takes `karaoke` and `karaokeVocals`; `PushMetadata` gains the
   flag; its types and tests. About 40 lines.
3. The demo page (`demo.js`, 620 lines; the §7.8 fold is already there): a Karaoke control that
   appears when the stream is karaoke, three reproductions and four vocal choices, and the channel
   labels from the result. About 80 lines of JavaScript and markup.

**S** to **M**, with no lyrics: the WebAssembly build is CI's (the `wasm` lane), so it is not
verified from a Windows machine here. Lyrics on that page are a second slice: a JavaScript port of
the reader (the format is a page of rules; the Qt-free C++ is the oracle) and the cursor, **M**.

### What Android would need

A **player**: an app that decodes to PCM and plays it (AudioTrack), with the library through the
NDK, and then the same two fields and a view. The project has no such app, and the Shield demo
cannot be bent into one, since its job is HDMI passthrough, where an AV receiver reproduces and a
karaoke stream plays as coded unless the receiver is karaoke-capable (requirement P7). That is a
new program with its own row in `projects.json`, its CI lane and its distribution decision, not a
slice of this phase; the plan's D4 says so.

## W13: timed text beyond LRC, and Crucible

The owner asked, later on 2026-10-11, for Crucible to import LRC and for **CD+G (the subchannel,
ripped), SubRip, Advanced SubStation Alpha and "mini karaoke"** as stretch goals "for getting the
text/lyric meta data and events". This section is what each would take. **The format facts below are
from general knowledge of the formats, not from anything in this repository** (the project has
none of their specifications): each is to be checked against the format's own documentation when
its step starts.

### The formats

| Format | What it carries | How it is timed | Word level | Notes |
|---|---|---|---|---|
| LRC (`.lrc`) | Lines of text, and id tags (title, artist, album, by, length, offset) | `[mm:ss.xx]` a line; the enhanced form adds `<mm:ss.xx>` before each word | Only in the enhanced form | Read today by `apps/hearth/engine/src/lyrics.*`, UTF-8 only |
| SubRip (`.srt`) | Numbered cues of text | A start and an end for each cue, `hh:mm:ss,mmm --> hh:mm:ss,mmm` | No | The simplest to read; a few styling tags to strip; the encoding is often not UTF-8, which the reader has no answer to (phase 1's decision 4) |
| Advanced SubStation Alpha (`.ass`) | `Dialogue:` events with a style, a layer and text | A start and an end for each event, and inside the text the karaoke override tags (`\k`, `\kf`, `\ko`) that give each syllable's duration in hundredths of a second | **Yes**, by syllable | The format subtitle-karaoke tools write; the override blocks (`{...}`) carry the timing and a great deal of styling that is to be skipped |
| MIDI karaoke (`.kar`, a Standard MIDI File) | The music as MIDI, and the words as Lyric and Text meta events, with `@T` and `@K` header lines and `/` and `\` for a new line and a new paragraph | MIDI ticks, turned into time through the file's tempo map | **Yes**, by syllable | Read here as what "mini karaoke" means; see D10. A `.kar` is a backing as well as its words, and only the text and its events are wanted |
| CD+G (`.cdg`, a CD's subcode) | **Graphics, not text**: tile and palette commands that draw a 300 by 216 picture in 16 colours | 300 packets a second (75 sectors a second, four packets each), against the CD's own audio | No | There is **no lyric text to extract**: the words are pixels. What an import can give is the picture, timed with the audio, and the audio is ripped separately |

CD+G is the one that does not fit the others. Everything else produces the same thing the reader
already produces (lines of timed words), and a new parser is a new function. CD+G produces *picture
events*, a second kind of cue with its own view, and "getting the text" from it would be optical
character recognition, which is a model and a runtime and is not proposed. D11 asks for the picture.

### What Crucible can know

Crucible "captures applications separately and places each one in a Dolby Atmos scene"
(`docs/crucible/index.md`). It has each application's audio and its name. It has no file and no
track: a search of `apps/crucible` and `libs/audio` for a media session (MPRIS, Windows' System
Media Transport Controls, macOS's now-playing information) finds none. So an imported `.lrc` has
nothing to follow until a position is supplied, and the candidates are:

| Position source | What it gives | Against |
|---|---|---|
| **The application's OS media session** | The track's position, where the player publishes one (MPRIS `Position` on Linux; the media transport controls' timeline on Windows; the now-playing information on macOS: *named from general knowledge, to verify*) | Not every player publishes it, a browser tab may not, and macOS needs a permission; three platform back ends |
| **Start and nudge** | The person presses a key as the song starts and nudges with a delay control (Hearth's lyric delay is the model) | Drifts the moment the player pauses or seeks; honest about being manual |
| **The tap's own sample count** | Time since the application began producing audio | Wrong after a pause the application does not signal, or a seek; the tap sees silence, not a pause |

**Recommended** (D9): the strip in Crucible's window, the position from the media session where
there is one and from start-and-nudge where there is not, and nothing derived from the tap. What the
events could drive beyond a strip (an OSC message a visual rig reads, say) is the owner's call; the
library's cue type is the same either way.

### Where the module lives

A **library under `libs/`**. `apps/shared` would reach Hearth, Crucible and the Forge programs and
not the firmware (`firmware/hearth-sink` may use `ac3`, `device`, `esp-idf`, `render` and `sendspin`)
or WebAssembly. `libs/device` is the sink firmware's own place for logic with no SDK and could
hold the cursor alone, but the reader and the other four parsers are not device logic. A new
library named `lyrics` under `libs`: kind library, `may_use` nothing (it needs only the standard library, as
`lyrics.hpp` does today), internal (never installed) at first, namespace `iclforge::lyrics`, with
its row, its generated project page and its test binary. The reader's `read_lyrics(path)` takes a
`std::filesystem` path; the firmware wants only `parse_lyrics(text)`, which is already separate.

### Sizes and order

| Step | Size | Notes |
|---|---|---|
| W13a: the module into a `lyrics` library under `libs` | **S** to **M** | A move: the 14 `[lyrics]` cases go with it, Hearth's includes change, a project row and a namespace entry are added; no behaviour changes |
| W6's writer into it | in W6 | Written in the library, beside the reader, once W13a is in |
| W13b: Crucible imports `.lrc` | **M** to **L** | M for start-and-nudge and the strip; L when the media-session back ends are in |
| W13c: SubRip | **S** | A parser that fills the same structure; line-level only |
| W13d: ASS | **M** | The event lines, the karaoke override tags, the styling skipped, an encoding answer |
| W13e: MIDI karaoke | **M** | A Standard MIDI File reader (variable-length quantities, running status, the tempo map) for the meta events only |
| W13f: CD+G | **M** | The packet decoder and a picture view; a second cue type |

**L** in all, six pull requests, and W13a is the only one the rest of the phase also wants.

## What the findings add to the plan

| Plan item | What the findings say |
|---|---|
| W3, W6 | The tap editor, lines then words, with an estimate as its start; Hearth; writer beside the reader; **L** |
| W7 | (a) the host decodes a karaoke stream for a sink, with the mixed-group test; the 50 ms check; **S** to **M** |
| After W7, if the owner wants a screen | The lyric module moves to `libs/device`; the document and the `metadata@v1` anchor; **M** to **L**. This is W12's design, ready for the board |
| W8 | The browser decode demo: two arguments, a flag, a panel; **S** to **M** |
| Android | Not a slice: a player app first |
| W13 | The lyric module to a `lyrics` library under `libs` first (S to M), before W6's writer; Crucible has no position source, so its import needs one (D9); CD+G is a picture, not text; "mini karaoke" read as MIDI karaoke (D10) |
