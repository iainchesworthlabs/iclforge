# The Hearth sink screen: one design for the desktop, Android and the P4X panel

!!! note "Status as of 2026-10-12: a draft for the owner; the panel column is built, the Android column is proposed"
    The owner asked for a **common design across the Hearth family**: desktop Hearth (Windows,
    macOS, Linux), the Android TV app ([the plan](hearth-android.md)) and the ESP32-P4X board with
    its 7-inch panel. This is the one document. It was started by the Hearth-for-Android session;
    the panel's column is the P4X session's ("Karaoke support on P4X"), who owns what the panel
    draws and reviews this page, and whose built design this page takes as its starting point
    (their plan, `planning/karaoke-p4x-panel.md`, and its screens, are on the branch
    `feature/karaoke-p4x-support` and not yet on `main`). On 2026-10-12 they agreed the pins, the
    narrowed viewing distances, the script rule and 70% for the board's sung line, as marked below. Nothing here changes what the desktop
    draws today: where this page and the desktop differ, [the table below](#what-the-three-do-today)
    says which side moves.

## What this page fixes

A sink's screen is the same thing on three surfaces. This page fixes, once: **what it can be told**
([the model](#the-model)), **which screens it has** ([screens](#the-screens)), **how a screen is
laid out and in what units** ([layout](#layout)), **the tokens** ([tokens](#tokens)), **the rules
every renderer keeps** ([rules](#rules)), and **where the three must differ and how**
([differences](#where-the-three-must-differ)). It does not fix code: the desktop is Qt Quick, the
Android app a native Kotlin view over a JNI core ([D2 of the plan](hearth-android.md#decisions)),
the panel LVGL. What the three share in code is the **view-model**: `libs/lyrics`' `Stage`
(`make_stage(lyrics, position_ms, delay_ms, options)`), written for the panel by the P4X session.
It returns indices into the document: which lines are past, current and next, which word is being
sung and how far through it is, whether the singer is in an instrumental gap and how long until the
next line. A renderer is a drawer of that.

## What the three do today

Read from `LyricView.qml`, `LyricScreen.qml`, `Theme.qml`, the Shield demo's `Theme.kt`, and the P4X
session's plan and message of 2026-10-12. Details and sources are in
[the findings](hearth-android-findings.md#f9-the-three-screens-as-they-are-today).

| | Desktop (Qt) | Android demo | P4X panel |
|---|---|---|---|
| Is there a lyric screen | Yes: the Karaoke page, and the full-screen singer view (F11) | No | Yes: built, drawn by its own code on a computer, **never on a panel** |
| Palette | Signal | Its own, blue | Signal dark |
| Word fill, three states, duet tags, line-timed, plain, no-lyrics | Yes | No | Yes |
| Count-in before a line | `♪` on an empty line only | No | Dots for a gap of 4 s or more |
| Right to left | The UI follows the language; the line's own direction is an owed check | No | Per line, by the first strong letter |
| Arabic, CJK | Whatever the OS has | n/a | **No** (no shaping; no room for a face) |
| Idle screen with pairing | n/a (it is the server) | No | Yes (name, address, network, code, paired or waiting) |
| Format and route line | The Play page's signal path | No | The footer's layout chip; no stream format |

The Android app is new, so it **adopts the family's design and does not inherit the demo's palette**.
Two things on the existing screens are proposed to move, both for the owner ([open items](#open-and-whose)):
the desktop gains the count-in, and both existing screens draw the sung line at 55% accent, which
is **2.61:1** against the background ([tokens](#tokens)).

## The model

What a sink screen is told, whichever program draws it. A field a platform cannot supply is absent
and its element is not drawn; no platform invents one.

| Field | Meaning | Desktop | Android | Panel |
|---|---|---|---|---|
| `name`, `address`, `network` | The sink's name, its address, the network it is on | n/a | the service | the board |
| `pairing` | `unpaired` (with the code or the `SP:1` QR), `paired`, `paired and connected`, `in use by another server` | n/a | `PlayerSession` and `Arbiter` | the board |
| `server` | The name the connected server gave itself | n/a | `PlayerSession::server_name()` | the board |
| `group` | The group's name | the Network page | `group/update` `group_name`, which the player is handed | not drawn today |
| `item` | `title`, `artist`, `album` | the queue | **see [D7](hearth-android.md#decisions)**: `metadata@v1`, or the document's tags | the document's `[ti:]` and `[ar:]` tags |
| `position`, `duration` | Where the sound is | `LyricClock` | the playhead; `[length:]` or `metadata@v1` | the playhead |
| `route` | `passthrough` (bits reach the receiver untouched), `decoded` (this sink decodes and applies), `pcm` (the server sent PCM) | the Play page | the sink core | `decoded` always |
| `format` | What the stream is: "Dolby Digital 5.1", "Dolby Digital Plus 7.1.4 JOC", the reproduction "aware" or "capable" with its vocals | the Media page | `decoder` in the role's state (the sink parses the first syncframe) | the board's layout chip only |
| `levels` | volume, mute, `output_delay_ms` | n/a | the role's state | the board |
| `lyrics` | `none`, `pending`, `plain`, `line` (timed lines), `word` (timed words), with the document and a `Stage` | `KaraokeController` | `Follower` | `Follower` and `Stage` |
| `attention` | A reason to say aloud: the receiver does not accept the format, the stream was refused, the link is lost, `why`, `settings_error` | the log and notices | the sink core | `why` |

`route` and `format` are the two the panel does not draw because its role state does not carry
them. For the Android sink the route is **new information**: a sink that can both pass through and
decode (the plan's phase 4) must say which. The role's state object accepts an added optional
key without a version change, so an `output` key (`passthrough`, `pcm`) is the proposal
([D6](hearth-android.md#decisions)); the panel could draw `format` the same day.

## The screens

The numbering is the panel's (built, with pictures); S0, S10 and S11 are added.

| # | Screen | When | Shows |
|---|---|---|---|
| S0 | **Starting** | The service is up and there is no network, or the output is not ready | The sink's name, "Waiting for the network" or "Waiting for the receiver", and what to check |
| S1 | **Idle** | No stream; connected or not | The name large, the address, the network; "Paired with *server*" or "Waiting for a server"; when unpaired, the code and the QR; the layout and output |
| S2 | **Singing, word-timed** | Lyrics held and the current line has word times | The grid below: the past line, the current line with the fill, two to three next lines |
| S3 | **Duet** | The document's lines carry singers | V1's lines at the start edge, V2's indented 18%, both across; a tag above each line ("Shantyman · guide", "Crew · you") |
| S4 | **Instrumental gap** | Between lines for 4 s or more | A note and four dots that go out one a second; the next line under it |
| S5 | **Line-timed** | The file times lines and not words | The line whole, in the accent, bold. Nothing invented for the words |
| S6 | **Plain text** | The file has no times | The text, scrollable where there is a pointer or a D-pad, not followed; a chip says "not timed" |
| S7 | **No lyrics** | A stream is playing and none are held | The item's title if there is one, "No lyrics for this item" (after a grace of one second, so a document that is a moment behind the stream does not flash it) |
| S8 | **Right to left** | A line whose first strong letter is right to left | Right-aligned, words flow right to left, the fill runs right to left within a word |
| S9 | **Controls** | A tap, or the D-pad's centre | Lyric delay −/+, text size, brightness where the surface has it; hides itself after 8 s |
| S10 | **Attention** | The route cannot play what was sent, the stream was refused, the link dropped | One sentence of what happened and what the person can do. The last screen's lyric stays dimmed behind it |
| S11 | **Picture** | Reserved. A CD+G picture ([findings, F10](hearth-android-findings.md#f10-cdg-and-a-sink)) | Not planned on any surface |

## Layout

**One reference canvas, 1024 by 600 units (`u`), scaled by height.** A surface sets
`u = height in pixels / 600`; the panel's `u` is one pixel, a 1080p TV's is 1.8, a 1440p desktop
window's 2.4. Widths follow: a 16:9 surface is 1,067 `u` across, so the stage keeps the panel's
32 `u` margin and a TV adds its overscan margin on top ([differences](#where-the-three-must-differ)).
Because the sizes are in `u` and the width is nearly the same in `u`, **the number of characters
that fit a row is the same on every surface**, which is why the length rule below can be shared.

```
 0 ┌──────────────────────────────────────────────────────────────┐
   │ HEADER 64u: title - artist (start)              1:12 / 3:05  │
64 ├──────────────────────────────────────────────────────────────┤  accent hairline = progress
   │ STAGE 472u                                                   │
   │   past line        36u, accent at the sung opacity, 1 row    │
   │   CURRENT LINE     40 to 72u by length, bold, 2 rows at most │
   │   next line        40u, muted                                │
   │   next line + 1    40u, muted      (+ 1 more on a TV)        │
536├──────────────────────────────────────────────────────────────┤
   │ FOOTER 64u: chips: format · reproduction │ vocals │ delay  ⚙ │
600└──────────────────────────────────────────────────────────────┘
```

The current line's size is chosen by its length in characters, the panel's rule (the heuristic of
`current_size()` in its `lyric_screen.cpp`, not a measurement per line), which this page adopts
for all three: **up to 18 characters 72u, up to 36 64u, up to 44 56u, up to 54 48u, longer
40u, and at most two rows**. The line is never scrolled; a word wider than the stage is cut at the
margin and the footer says so. Context lines are 36u (past) and 40u (next); header and footer
text 24u; **the smallest touch or focus target is 72u** (the panel's 72 px; the TV's D-pad focus
ring is proposed at the same 72u).

The footer's chips, in order and each dropped when its field is absent: the **format and
reproduction** ("Dolby Digital 5.1 · passing through", "Dolby Digital 5.1 · aware, decoded here"),
**whose vocal sounds** (V1, V2, both, none), and the **delay** (shown when not 0), then the gear.
The panel's chip today is its layout ("5.1 · aware"); the format half needs `format` and `route`
in its state ([the model](#the-model)).

## Tokens

The values below are written once, in `docs/hearth/design/sink-screen-tokens.json`, and held there by
`tools/checks/sink_screen_tokens.py` (the Android app's `Tokens.kt` is generated from the file; the
desktop's palette and the panel's screen are checked against it).

The family's tokens are `apps/shared/theme/assets/qml/Theme.qml`'s, in the Signal palette's dark
mode, which is also what `LyricScreen.qml` hard-codes and the panel takes. A sink is a thing in a
dim room and **always draws dark, whatever the OS theme is** (the singer view's own rule).

### Colour (dark)

| Token | Value | Use | Contrast on `bg` (computed here) |
|---|---|---|---|
| `bg` | `#151313` | Background | n/a |
| `surface` | `#1d1a1a` | Footer, sheet | n/a |
| `text` | `#ece7e5` | The word being sung; titles; the line being read | 15.10:1 |
| `accent` | `#ff5c40` | Sung words, the fill, the underline, progress, focus ring | 6.04:1 |
| `muted` | `#8b8583` | Not yet sung; secondary lines | 5.10:1 |
| `faint` | `#4b4645` | Dividers only | 1.99:1 |
| `rule` | `#353130` | Hairlines | 1.44:1 |

**Open: the sung line.** Both existing screens draw it as the accent at 55% opacity, which
composites to `#963B2C`, **2.61:1**. At 65% it is 3.17:1, at 70% 3.51:1, at 75% 3.86:1. A sung line
is not needed to sing, so the colour is a choice and not a defect; but 3:1 is the floor for large
text and the three surfaces should not each pick their own. Proposed: **70%**. Owner and P4X
session to confirm ([plan, D13](hearth-android.md#decisions)).

### Spacing, shape, type

| Token | Value |
|---|---|
| Spacing | 4, 8, 12, 16, 24, 32 `u` (`Theme.space1` to `space8`) |
| Radius | 0, everywhere |
| Faces | Archivo (Regular, Medium, SemiBold, ExtraBold) for Latin **only** (its `cmap` has 398 Latin codepoints, four Greek letters and no Cyrillic, checked on all four weights 2026-10-12), with a Noto Sans for Greek and Cyrillic where the surface has no system face to fall back to (the ESP32-P4 image carries one, 473 KB for its three faces; the desktop and Android fall back to the system's until one is bundled); Noto Sans Hebrew (Regular and SemiBold static instances; the bundled file is variable and its default instance is Thin) for Hebrew and Yiddish; Noto Sans Arabic where the surface can shape; the system's CJK face where there is one |
| Type size | the `u` sizes in [layout](#layout), times the surface's text-size step (S, M, L; `Theme.fontScale` on the desktop) |
| Case and tracking | header and chips: uppercase, tracked at `trackingWide` (1.2 `u`), as `LyricScreen.qml` draws them |
| Icons | Material Symbols Sharp, the handful of glyphs `Theme.qml` lists |

### Motion

| What | Rule |
|---|---|
| The word being sung | The fill grows left to right (right to left in a right-to-left line) by time, redrawn at the surface's rate: 30 frames a second while a word is being sung, 5 otherwise, 1 when idle (the panel's figures; the desktop is vsync) |
| A line change | Rebuilt at once; no slide. A fade of the previous line at most 150 ms |
| The count-in | One dot goes out a second, the four-second rule above |
| The idle screen | After a quiet period it dims and moves a few `u` every minute, so a screen on for hours does not hold one image (an OLED TV is the reason; the panel's backlight PWM is the means) |
| Reduced motion | The system's setting is honoured: no fade, no dot animation; the fill still moves, since that **is** the information |

## Rules

Each is a rule every renderer keeps and each has a fixture ([conformance](#conformance)).

| # | Rule |
|---|---|
| SS1 | **The current line is whole.** It is never scrolled or cut for length; its size steps down by the length rule and it wraps to two rows at most |
| SS2 | **Colour is never the only cue.** The word being sung is underlined; a duet's singer is told by name and by side; the line the singer sings is said as "you", the guide as "guide" |
| SS3 | **No word times are invented.** A line-timed file shows whole lines; a plain file is shown whole and not followed |
| SS4 | **Direction belongs to the line.** The base direction is the line's first strong letter, not the UI language; word order, alignment and the fill's direction follow it; a duet's indent is from the line's start edge |
| SS5 | **A script a surface cannot draw is said, not drawn as boxes.** A line it cannot draw is replaced by the sentence **"This line is in a script this screen cannot show."**; a title in such a script gets the same sentence or nothing; the sink's name, the footer and the clock always show. Detection is a code-point class test beside the library's right-to-left test (the P4X session builds it) |
| SS6 | **The clock is the sink's own,** the playhead from the server's anchors, with the person's **lyric delay** (−1,000 to +1,000 ms in 50 ms steps, kept per sink, drawn in the footer when not 0) |
| SS7 | **A karaoke choice the sink cannot apply is shown as not applied.** On a passed-through stream the vocals chip reads "as coded", not "V1" |
| SS8 | **Text size is the person's.** S, M and L scale every `u` size; nothing is a literal pixel size |
| SS9 | **The screen is a guest of the audio.** Drawing never costs a frame of sound: no underrun and no late chunk attributable to the screen (the panel measured 1.7 to 3.7 ms a refresh beside an AC-3 karaoke stream on a board with no panel) |
| SS10 | **Touch and D-pad change only what the sink owns** (the lyric delay, the text size, brightness). The vocals and the reproduction are the server's settings; the sink may *request* them only once the role has a request path ([plan, D8](hearth-android.md#decisions)) |

Scripts, by tier, so SS5 has something to test against:

| Tier | Scripts | Desktop | Android | Panel |
|---|---|---|---|---|
| T1 | Latin, Greek, Cyrillic | Yes (Greek and Cyrillic through the system's fallback face) | Yes (the same) | Yes (Noto Sans in the image) |
| T2 | Hebrew, Yiddish (right to left, no joining) | Yes | Yes | Yes |
| T3 | Arabic (right to left, joining, shaping) | Yes | Yes | **No** |
| T4 | CJK (no spaces; a face of 5 to 16 MB) | Yes (system face) | Yes (system face) | **No** |

## Where the three must differ

| | Desktop (Qt Quick) | Android TV (Kotlin views) | P4X panel (LVGL) |
|---|---|---|---|
| Surface | A window, or full screen (F11) | A TV at 1080p or 4K (Android TV's layout baseline is 960 by 540 dp) | 1024 by 600, RGB565, about 170 ppi, 7 in |
| `u` | Window height / 600 | 1.8 px at 1080p; 3.6 at 4K | 1 px |
| Viewing distance | 0.5 to 1 m | 2.5 to 4 m | to 2 m |
| What `u` is physically | Varies | About 1.14 mm on a 55-inch set | 0.15 mm |
| The current line (64u) as an em, angular size | Varies | 73 mm: 1.4° at 3 m | 9.6 mm: 0.27° at 2 m, 0.55° at 1 m |
| A context line (36u) | | 41 mm: 0.78° at 3 m | 5.4 mm: **0.15° at 2 m**, 0.31° at 1 m |
| Safe area | None | **A 5% overscan margin on each side** on top of the 32u (Android TV's design guidance; to check on the set) | None (own bezel) |
| Input | Mouse, keyboard | D-pad and OK, back, media keys; a game controller | Touch (GT911) |
| Focus | Keyboard focus ring (2 `u`, offset 2) | The same ring on D-pad focus; no pointer | Touch targets of 72u; no focus |
| Text engine | Qt (HarfBuzz) | Android's (bidi, shaping, system fonts) | LVGL `tiny_ttf` and its bidi; no shaping engine |
| Fonts | Bundled, plus the system's | Bundled Archivo, Hebrew and Arabic; the system's CJK | Archivo, Noto Sans Greek and Cyrillic and Noto Sans Hebrew, each at two weights, in the image (473 KB) |
| Scripts | T1 to T4 | T1 to T4 | T1 and T2 |
| Frame rate | vsync | 30 while a word is sung | 30, 5, 1 |
| Strings | Hearth's `.ts` catalogues | `strings.xml` per language | English in the built screens; whether a catalogue is wanted is the P4X session's to say |
| Test harness | `qml.exe` with mocked singletons; the QML tests | The emulator's screenshot, or a host render of the `Stage` | The preview tool: the firmware's own screen code against LVGL's software renderer |
| Where it can run before the product exists | Anywhere | The emulator | A computer; a v1.3 board with no panel |

Two things the angular figures say. On a TV, `u` can be **smaller** and still read: the length rule
is in characters, so the stage could hold more context lines (the table's "+ 1 more on a TV"). On
the panel the **current line is readable at 2 m only just, and the context lines are not** by the
usual 0.25° rule of thumb for text of a reading size; its own plan said "arm's length to about
two metres". The P4X session agreed on 2026-10-12 to narrow it, from its own arithmetic (a 9.7 mm
em, 0.28° at 2 m): **the current line to about 2 m, the context lines to about 1 m, the header and
footer within arm's length.** That is a statement about the screen's use, not a defect in the
layout.

## Conformance

One set of fixtures, one expected `Stage` sequence per fixture, and a picture per surface compared
with the design sheet by eye and by pixel probes at fixed positions, as the panel's preview tool
does.

| Fixture | Exercises | Exists |
|---|---|---|
| `examples/karaoke/auld-lang-syne/auld-lang-syne.lrc` | S2, word fill, S4 gaps | Yes |
| `examples/karaoke/leave-her-johnny/leave-her-johnny.lrc` | S3, V1 and V2, names in the tags | Yes |
| `.../*-lines-only.lrc` | S5 | Yes |
| The same text with its time tags stripped | S6 | Derive in the test |
| A Hebrew line and a Yiddish line, UTF-8 and UTF-16 | S8, SS4 | In `libs/lyrics/tests` as generated text |
| A line of 18, 36, 44, 54 and 55 characters | SS1's size steps | To write |
| An Arabic line, a Japanese line | SS5 on the panel; T3 and T4 on the others | To write |
| A gap of 3.9 s and of 4.0 s | S4's threshold | To write |

The expected sequences are the `Stage` of each fixture at fixed positions, computed by the library;
they are checked in beside the fixture so a renderer's test and the library's agree.

## Open, and whose

| # | Item | Whose |
|---|---|---|
| 1 | The sung line's opacity: 55% (today, 2.61:1) or 70% (3.51:1). **The P4X session accepts 70% for the board (one number in `lyric_screen.cpp`)**; the desktop's `0.55` and the TV's choice are the owner's | Owner |
| 2 | The count-in on the desktop and the TV | Owner (a change to the signed-off desktop design) |
| 3 | SS5's sentence and the title rule: **agreed with the P4X session 2026-10-12**; the sentence is pinned in SS5 above | Settled |
| 4 | The viewing distance the panel is designed for: **narrowed, as stated above, 2026-10-12** | Settled |
| 5 | `format` and `route` in the role's state, for the panel's footer. The P4X session will draw them when they exist on `main` | The plan's D6 |
| 6 | A request path from a sink to the server for the vocals and the reproduction | The plan's D8; a protocol PR, shared with the panel's Q6 |
| 7 | Where tokens live: one file generating QML, Kotlin and a C header, or this page only | **Settled** (the plan's D13): `docs/hearth/design/sink-screen-tokens.json`. The Android app's `Tokens.kt` is generated from it, and `tools/checks/sink_screen_tokens.py` holds the desktop's Signal dark palette and the panel's screen to the same values without writing to either; moving either to a generated file is its owner's |
| 8 | Where `title` and `artist` come from for an item with no lyric document | The plan's D7 |
| 9 | Greek and Cyrillic have no face bundled with the family: Archivo does not cover them. The panel bundles Noto Sans for both; the desktop and Android rely on the system's fallback, which changes the look of a Cyrillic line | Owner; the P4X session's panel PR corrects its own rows |
