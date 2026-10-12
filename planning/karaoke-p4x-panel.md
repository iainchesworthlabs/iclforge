# Karaoke on an ESP32-P4 (chip revision v3.x) with a 7-inch panel

!!! note "Status as of 2026-10-12: built and drawn on a computer; the kit has not arrived"
    This is W12 of [karaoke phase 2](karaoke-phase2.md#w12-the-esp32-p4-with-a-panel), taken
    off the shelf because the owner's development kit is on its way and the firmware can be ready
    for it. The owner's brief, on 2026-10-12: a P4 of chip revision v3.2 or newer ("P4X") at
    400 MHz with 32 MB of PSRAM, 16 MB of flash and a 7-inch 1024 by 600 capacitive touch panel on
    MIPI DSI; it receives the multichannel stream and its lyrics, decodes on the chip, writes the
    decoded audio out of I2S in TDM to two DACs that are not connected yet, and draws the lyrics
    with the colour moving as the song does. Until the kit is here the firmware runs on the
    owner's FireBeetle 2 boards (revision v1.3, no panel) so that the compute can be measured.

    Nothing on this page has run on the kit. What was measured on a v1.3 board says so; what was
    read from ESP-IDF or a datasheet says so; the rest is a decision or a guess and is marked.

    | Step | State |
    |---|---|
    | W12a `Stage` in `libs/lyrics` | Built, 13 host test cases, mutation-checked |
    | W12b the panel, the screen, the preview tool | Built; the screens are in [the design sheet](../docs/hearth/sink-esp32-p4.md#the-screen), drawn by the firmware's own code on a computer |
    | W12c the DSI panel and touch | Written against ESP-IDF v6.1 and the vendor drivers' headers and built by CI; **never run** |
    | W12d the rev3 image | Built (a local build found and fixed a bootloader 576 bytes over its window: Warn log level, 22,448 bytes); the update chooser picks it by chip revision (tests); **never run on a v3.x chip** |
    | W12e measured on the FireBeetle with no panel | See [Measured](#measured) |
    | W12f docs | [An ESP32-P4 sink](../docs/hearth/sink-esp32-p4.md) |
    | W12g bring-up on the kit | Waits for the kit |

## What exists, and what is missing

| Part | State | Where |
|---|---|---|
| The sink plays Sendspin and decodes AC-3, E-AC-3 and (P4) AC-4 on the chip | Built, on a v1.3 P4 board | `firmware/hearth-sink`, [sink tiers](esp32-sink-tiers.md) |
| The board holds the item's lyrics and follows them with the sound on its own clock | **Built and verified on a real S3** (W7d, #1340) | `libs/lyrics` (`Follower`, `Playhead`), `main/sendspin/player/sendspin.cpp` |
| The board applies the singer's karaoke choice itself (reproduction, vocals) | Built (W7, #1310) | the `_iclforge_player@v1` role's settings |
| TDM above two channels on the P4 | **Not reachable on a v1.x chip**; a v3.x chip has the clock it needs | [sink tiers](esp32-sink-tiers.md#product-shape), `sink/i2s_wide` |
| A screen | **Missing**: this page | |
| A firmware image for a v3.x chip | **Missing**: v1.x and v3.x images are not interchangeable | `hearth-sink-esp32p4-rev1` is the only P4 image |

The board has no screen code at all today. `g_lyrics` in `sendspin.cpp` says so in a comment:
"there is no screen to draw them on yet". Everything that has to arrive at the board already
arrives; this work is the last metre.

## The kit

The owner described it; the model was not named. **Assumption, to confirm when it arrives:** it
is [Espressif's ESP32-P4-Function-EV-Board](https://components.espressif.com/components/espp/esp32-p4-function-ev-board)
with its 7-inch panel, which is exactly that shape (32 MB of PSRAM in the package, 16 MB of flash,
EK79007 panel at 1024 by 600, GT911 touch, an onboard ES8311 codec, 100 Mbps Ethernet, and an
ESP32-C6 for Wi-Fi). Waveshare and others sell similar 7-inch P4 boards with other controllers
and pins, so the panel and the pins are a **board profile**, chosen in Kconfig, with the
Function-EV profile as the default and every pin marked "check against the schematic".

| Fact | Value | Source |
|---|---|---|
| CPU | two cores, **400 MHz** on v3.x (360 MHz is the v1.x ceiling) | ESP-IDF `Kconfig.cpu`; v3.x user guide §1.1 |
| ESP-IDF's switch | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` **off** (the default); default minimum revision v3.1 | ESP-IDF v6.1 `Kconfig.hw_support` |
| I2S | a **160 MHz PLL clock source** (`I2S_CLK_SRC_PLL_160M`, "only supported on P4 hw_ver3") | ESP-IDF `clk_tree_defs.h`; v3.x user guide §1.8 |
| PSRAM | HEX mode, 200 MHz by default (250 MHz is offered on v3.x) | ESP-IDF `Kconfig.spiram` |
| MIPI DSI | APLL as a clock source and RGB565/666/888 conversion added | v3.x user guide §1.24 |
| Panel (Function-EV) | EK79007, 1024 by 600, 2 data lanes, DSI PHY powered by LDO channel 3 at 2.5 V | `espressif/esp_lcd_ek79007` |
| Backlight / reset (Function-EV) | PWM on GPIO26, reset on GPIO27 (jumpers on the adapter board) | Espressif's BSP header, espp BSP readme |
| Touch / codec I2C (Function-EV) | SDA GPIO7, SCL GPIO8, shared by the GT911, the ES8311 and anything on the header | Espressif's BSP header |
| On-board codec I2S (Function-EV) | MCLK 13, BCLK 12, WS 10, DOUT 9, DIN 11; amplifier enable 53 | Espressif's BSP header |
| SDIO to the C6 | GPIO14 to 19, C6 reset GPIO54 | what `sdkconfig.p4` and `esp_hosted` already use |

Two caveats the BSP's own header carries and this page does not hide: its touch reset and
interrupt lines are `GPIO_NUM_NC` (the GT911 is polled or uses its own address-select sequence),
and a pin comment in it was copied from another board. Nothing above is wired to this firmware's
I2S pins; those are chosen at the bottom of [the pins](#pins).

### What the new silicon changes for the sink

- **400 MHz**, not 360: the decoders scale with the clock almost exactly
  ([AC-4 timing](ac4.md)), so a v1.3 board's measurements are a floor.
- **TDM opens**. The `i2s_wide` sink pins `I2S_CLK_SRC_APLL`, whose 125 MHz ceiling is ~13 slots at
  32 bits: the sink's full 16-slot frame (512 bits) at 48 kHz needs a ~147 MHz source. The 160 MHz
  PLL clears it. The source is chosen **at build time** by `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`,
  because the two images are separate builds anyway.
- **PSRAM at 200 MHz hex** (and 250 as an experiment) instead of the v1.3 board's measured 18
  cycles a word for a cold sequential read: the decoders keep their code in PSRAM
  (`CONFIG_SPIRAM_XIP_FROM_PSRAM`), so this is the single largest setting after the clock.
- **The errata**: v3.1 fixed the MSPI power-on fault; a `CONFIG_ESP32P4_REV_MIN_301` image refuses
  anything older, which keeps the workarounds out of the build.

None of these is measured on a v3.x chip. The first thing the kit does is run the existing
probe and the I2S TDM open ([Bring-up on the kit](#bring-up-on-the-kit)).

## Requirements

| # | Requirement | Why |
|---|---|---|
| R1 | The screen shows the item's lyrics against the sound the **board** is playing, on the board's own clock, not the host's | The board knows its output delay; W7d put the anchors there on purpose |
| R2 | Word-level fill: sung, being sung (filled left to right by time), not yet sung, in three colours and one cue that is not colour | The desktop view's rule (L4); a view that only colours cannot be read by everyone |
| R3 | A duet's two singers are told apart by name and by side, not only by colour | Same as the desktop (W11b) |
| R4 | Hebrew and Yiddish draw right to left and the fill runs right to left | The owner's D8 |
| R5 | When nothing is playing the screen says what the board is, where it is and what it waits for | A sink with a screen is a thing people stand in front of |
| R6 | A board with no panel, or a panel that did not start, plays exactly as it does today | The same image runs on a P4 module with nothing attached |
| R7 | Drawing never costs the audio a frame: no underrun, no late chunk attributable to the screen | The product is the audio; the screen is a guest |
| R8 | The screen is readable at arm's length to about two metres | A prompter on a table or a stand |
| R9 | The design is verified without the kit: the same code draws on a board with no panel and the picture can be read back | The kit is two weeks away |
| R10 | Touch changes only what the board owns (its lyric delay, text size, brightness); the singer's vocal choice stays the server's until the role has a way to carry it back | The role's settings flow from the server |

## Decisions

### D1. The graphics library: LVGL 9

| Option | For | Against | Verdict |
|---|---|---|---|
| **LVGL 9** through `espressif/esp_lvgl_port` | The library every P4 BSP and example uses; MIPI-DSI and touch glue exist; anti-aliased text, flex layout, clipping and **bidirectional text** are in it; PPA hooks exist; one task, one lock | A dependency from the component registry (the firmware already takes three); a heap to keep out of internal RAM; word fill is a small custom widget, not a stock one | **Chosen** |
| Qt (QML) on the chip | The desktop view is QML | Needs Linux; not a microcontroller target | No |
| A hand-written renderer (glyph atlas, rectangles) | Smallest, fully ours | We would write text layout, wrapping, bidi and a font loader; Hebrew is a stated requirement | No; revisit only if LVGL's footprint is a problem |
| Espressif's `esp_lvgl_adapter` | Newer: tearing control, PPA fill and blend | Pre-1.0 and its own API; `esp_lvgl_port` is the established path | Watch; the panel module hides the choice |
| LVGL 8 | Mature | End of the road; v9 has the PPA and DSI work | No |

LVGL 9.6.0 and `esp_lvgl_port` 2.9.0 are on the registry (read 2026-10-12). The firmware pins a
version range in `idf_component.yml`, not a floating tag.

The panel code is behind one seam, `main/panel/panel.hpp`, which **CMake picks as a directory**,
exactly as the firmware picks its sink and its source (`main/audio_sink.hpp`):

| Directory | What it is |
|---|---|
| `panel/none` | No screen; today's behaviour. Every image but the P4 rev v3 one |
| `panel/null` | LVGL drawing into a PSRAM frame buffer with no panel behind it, a frame counter and a read-back route. **What runs on the FireBeetle** |
| `panel/dsi` | The same drawing code on a MIPI DSI panel, with touch. **What runs on the kit** |

The drawing (`lyric_screen.cpp`) is one file for both of the last two, so what the FireBeetle
measures is what the kit draws, less the scan-out.

### D2. The view model is a library, not firmware code

What a view shows is a function of the document, the position and a few options: which lines are
the past, the current and the next, which word is being sung and how far through it is, whether the
singer is in an instrumental gap, and how long until the next line. The desktop view does this in
QML and the controller; the board needs it in C++ and tests for it need it without a chip.

`libs/lyrics` gets `iclforge/lyrics/stage.hpp`: `make_stage(lyrics, position_ms, delay_ms,
options)` returns a small `Stage` of indices into the document (no strings copied: a board holds
the document once). It is standard-library only, like the rest of the module, and is tested on the
host with the same cases the desktop view's QML test has. The firmware's drawing reads a `Stage`
and nothing else.

### D3. Fonts: TrueType in the image, rendered by LVGL's `tiny_ttf`

No build-time font conversion (it would add Node to the CI image) and no per-size C arrays. The
fonts are the ones the apps already bundle and license (SIL OFL): **Archivo** (Regular and SemiBold,
110 KB each) and **Noto Sans Hebrew**. Archivo covers Latin only (Basic, Latin-1, Extended-A, most of
Extended-B and the Vietnamese block, punctuation and currency; checked against its `cmap` on
2026-10-12: **no Cyrillic, four Greek letters, no U+266A**), which is an earlier version of this
page's claim corrected. The Hebrew file the apps bundle
is a *variable* font whose default instance is Thin (`tiny_ttf` has no axes, and drew it as
hairlines), so the firmware carries two static instances cut from it with `fontTools`' instancer
(Regular and SemiBold, 48 KB each). They are embedded in the application image
(`target_add_binary_data`), 318 KB of flash, and not in a partition of their own: an update over the
network carries them, and no partition table or package changes. LVGL's `tiny_ttf` rasterises
glyphs at the size asked and caches them; Hebrew is each Archivo's fallback font.

Not in this build, and said so: **Greek and Cyrillic** (a Noto Sans for them is about 300 KB more;
a decision for the owner), **CJK** (a usable face is 5 to 16 MB, which the 4 MB slots have no room
for; a subset face is a build step to write), **Arabic** (Noto Sans Arabic is 845 KB and needs
shaping LVGL does not do; its joining option exists and is untried), and the Devanagari and Thai
families (no shaping engine). **A line with a letter outside the fonts' coverage is replaced by
one sentence**, "This line is in a script this screen cannot show." (the sentence pinned with the
Hearth family's shared sink-screen spec), and not drawn as the missing-glyph boxes; a title in
such a script is left out; a music note, which neither font has, is dropped from a line. The test is
`iclforge::lyrics::text_is_covered()` against a table of the code point ranges the four font files
carry (`main/panel/lvgl/glyphs.cpp`).

### D4. Rendering mode: partial updates into the panel's PSRAM frame buffer

1024 by 600 at 16 bits per pixel is 1.2 MB. The DPI frame buffer is in PSRAM (one buffer;
two if tearing shows on the kit). LVGL renders partial areas through two 1024 by 40 line buffers,
also PSRAM, because the AC-4 decoder leaves 1 to 11 KB of internal RAM
([AC-4 memory](ac4.md)) and a screen is the last thing that should take it.

The scene is mostly still. Per frame only the **word being sung** changes: its overlay's width and
the underline. Everything sung before it is a colour change made once. The renderer asks LVGL to
invalidate those rectangles and nothing else, which is about 1,024 by 90 pixels at its widest and
usually a quarter of that.

**The cost this adds that a desktop never pays:** the DPI engine reads the whole frame out of
PSRAM sixty times a second: 74 MB/s of reads on the same bus the decoder fetches its code and
tables from. That is not a CPU cost and a board with no panel cannot show it, so `panel/null`
can **emulate it** (an `esp_async_memcpy` of the frame buffer sixty times a second, no CPU); see
[Measuring](#measuring-on-the-board-with-no-panel).

### D5. Where the board's clock comes from, and the frame rate

The Sendspin host owns a time filter; `Follower::cursor(server_time)` already answers a
`LyricPosition` for a server time, and `g_lyrics` is guarded by `g_mutex`. The screen takes one
**snapshot** under the lock each frame (a copy of the document pointer is not safe across a new
document, so the snapshot is the `Stage` built under the lock; it holds indices and a few
numbers) and draws outside it.

30 frames a second while a word is being filled, 5 otherwise, 1 when idle. The render task is
below the decoder and the I2S writer in priority and takes the core the decoder does not.

### D6. Touch: local things only, in v1

| Control | Owned by | v1 |
|---|---|---|
| Lyric delay, -1000 to +1000 ms in 50 ms steps | The board (it applies the delay to its own cursor) | Yes |
| Text size S / M / L | The board | Yes |
| Brightness | The board | Yes |
| Next lines shown (0 to 3) | The board | Yes |
| Which vocals sound (V1, V2, both) and the reproduction | **The server**: they are role settings | **Read-only on the screen**; touch waits for a board-to-server request in the role (a protocol change, its own PR) |

The GT911 is read by `esp_lcd_touch` and reaches LVGL as a pointer device; the screens have no
control smaller than 64 px.

### D7. A separate image, named by revision

`hearth-sink-esp32p4-rev3`, built from `sdkconfig.defaults;sdkconfig.hw;sdkconfig.p4;sdkconfig.p4-rev3;
sdkconfig.sendspin;sdkconfig.panel`. `sdkconfig.p4-rev3` comes **after** `sdkconfig.p4` and overrides
the four revision lines in it (`ESP32P4_SELECTS_REV_LESS_V3`, `ESP32P4_REV_MIN_100`, the 360 MHz
clock, nothing else); the rest of `sdkconfig.p4` (PSRAM, hosted buffers, XIP from PSRAM, queue
sizes) is as true on a v3.x part and stays one file. The update chooser picks it for a v3.x board
by the revision range the image's manifest carries; the rev1 image refuses a v3.x chip and the
rev3 image refuses a v1.x one, so a wrong flash cannot happen over the network.

### D8. Nothing else about the audio changes

2.0 plays on standard I2S as it does today; 3 or more channels open TDM in the `i2s_wide` sink at
the full 16 slots, with `CONFIG_ICLFORGE_EXAMPLE_I2S_FIXED_FRAME=1` for the ES9080 pair when it is
wired. **Not in this work:** the I2C programming of the DACs (nothing programs one today), and the
Function-EV's own ES8311 codec as an output so the kit makes sound before the DACs arrive
([Later](#later)).

## The screen

**Panel:** 1024 by 600, landscape, about 170 pixels an inch (7 inches across the diagonal):
one pixel is 0.15 mm, so 56 px type is 8.5 mm tall and reads at about two metres. **Theme:** the
desktop's Signal palette in its dark mode (the board is a thing in a dim room), the Archivo type
scale, zero corner radius, so it is recognisably Hearth's.

| Token | Dark value | Use |
|---|---|---|
| `bg` | `#151313` | Background |
| `surface` | `#1d1a1a` | The footer and the sheet |
| `text` | `#ece7e5` | The word being sung; titles |
| `accent` | `#ff5c40` | Sung words, the fill, the underline, the progress hairline |
| `muted` | `#8b8583` | Not yet sung; the secondary lines (contrast 5.1 : 1 on `bg`) |
| `faint` | `#4b4645` | Dividers; a sung line fading out |

### The grid

```
 0 ┌──────────────────────────────────────────────────────────────┐
   │ HEADER 64: title - artist (left)            1:12 / 3:05 (right)│
64 ├──────────────────────────────────────────────────────────────┤  accent hairline = progress
   │                                                                │
   │ STAGE 472                                                      │
   │   past line        (36 px, accent at 55 %, 1 line)             │
   │                                                                │
   │   CURRENT LINE     (64 px bold; up to 2 rows; word fill)       │
   │                                                                │
   │   next line        (40 px, muted)                              │
   │   next line + 1    (40 px, muted)                              │
536├──────────────────────────────────────────────────────────────┤
   │ FOOTER 64: chips  5.1 · aware │ V1 + V2 │ delay 0 ms     ⚙    │
600└──────────────────────────────────────────────────────────────┘
```

Margins 32 px. The current line starts at 64 px and steps down to 56, then 48, while it would
wrap past two rows, so a long line stays whole rather than scrolling.

### The screens

| # | Screen | When | What it shows |
|---|---|---|---|
| S1 | **Idle** | Nothing playing | The board's name large, its address, the network, "Paired with *server*" or "Waiting for a server", the layout and the output ("5.1 · I2S TDM 16 × 32") and a line that says what it is waiting for |
| S2 | **Singing, word-timed** | Lyrics held, a word-timed line is current | The grid above: past line, current line with the fill, two next lines |
| S3 | **Duet** | The file's lines carry singers | V1's lines at the left, V2's indented 18 %, both across; a tag line above each ("Shantyman · guide", "Crew · you"); the footer chip shows whose vocal plays |
| S4 | **Instrumental gap** | Between lines (or before the first) for 4 s or more | A single large music note and four dots that go out one a second, so a singer sees the next line coming; the next line is shown under it |
| S5 | **Line-timed** | The file times lines, not words | The current line whole, in accent, bold; nothing invented for the words (the rule since phase 1) |
| S6 | **Plain text** | The file has no times | The text, scrollable by touch, not followed; a chip says "not timed" |
| S7 | **No lyrics** | Playing, none held | The item's title if the server sent one, "No lyrics for this item" |
| S8 | **Right to left** | A line whose first strong letter is Hebrew | Right-aligned, words flow right to left, the fill runs right to left within a word |
| S8b | **A script the fonts lack** | A line with a letter the four font files do not carry (Cyrillic, Greek, Arabic, CJK) | One sentence in its place, "This line is in a script this screen cannot show."; the title in it is left out; a music note is dropped |
| S9 | **Controls sheet** | A tap on the gear | Lyric delay - / +, text size, brightness, next lines; 72 px targets; it hides itself after 8 s |

The pictures are in [the design sheet](../docs/hearth/screenshots/p4-panel-sheet.png) and on
[the guide's page](../docs/hearth/sink-esp32-p4.md#the-screen). They are not
mock-ups: `firmware/hearth-sink/preview` builds the firmware's own `lyric_screen.cpp` against
LVGL's software renderer and writes one picture a screen, and the same code draws them on a board
with no panel (the read-back route, below). A change to the design is a change to that file, and
the sheet is regenerated from it.

### How a word is drawn

One row per line in a wrapping flex container. For the current line each word is a cell with a
base label and, when it is being sung, an overlay container clipped to `fill x width` holding the
same word in the accent colour, and an underline. A file that times the parts of a word (the example
songs time syllables) gets one cell a part, and the cells of a word are a group the row does not
break between. A word's trailing space is the cell's padding, so the fill covers the letters and
nothing else. Words before the one being sung are one colour change to the accent; words after it
are `muted`. A line change rebuilds the rows (once every few seconds); a frame changes one overlay's
width. For right to left the row's base direction is flipped, which reverses the words' order and,
because LVGL mirrors an alignment under a right-to-left base, puts the overlay on the word's right
edge: checked in the picture of the Hebrew screen, which had the fill running from the wrong side
until the alignment was left to LVGL.

A line wider than the stage after the smallest size wraps; a word wider than the stage is cut at
the margin and the status line says so (no real lyric hits it).

## Pins

All of these are Kconfig, in a **board profile** (`CONFIG_ICLFORGE_EXAMPLE_PANEL_BOARD_*`).

| Signal | Function-EV default | Status |
|---|---|---|
| Backlight (PWM) | GPIO26 | From the BSP header |
| Panel reset | GPIO27 | From the BSP header |
| Touch / I2C SDA, SCL | GPIO7, GPIO8, I2C0 | From the BSP header |
| Touch INT, RST | not connected on the board | From the BSP header |
| DSI | 2 lanes, 1024 by 600 at 60 Hz, PHY LDO channel 3 at 2.5 V | From `esp_lcd_ek79007` |
| TDM BCLK, WS, DOUT | **GPIO 20, 21, 22 (the FireBeetle defaults), placeholders** | **Unchecked.** The BSP header lists GPIO19/20 as USB on this board while SDIO uses 19: pick from the header's free pins with the schematic |

The I2S pins are the only ones that cannot be chosen from a document today, because nothing
else on the board needs them and the DACs are not wired; they are decided when the kit is on the
bench.

## Measuring on the board with no panel

The FireBeetle 2 is a v1.3 chip at 360 MHz with PSRAM at the speed v1.3 allows, so it is
**slower than the kit in every number that matters**. What it can say is whether the screen is
cheap enough to be a rounding error on a slower part, which is an upper bound for the kit.

| Measure | How | Pass |
|---|---|---|
| Time to render a frame (per word step, per line change) | The panel task times `lv_timer_handler` and the flush; reports min / mean / max | Mean under 8 ms, line change under 30 ms |
| Cost to the audio | The existing counters: `underruns`, `late_chunks`, `dropped_chunks`, the decode time per frame, with and without the screen | No change beyond noise over a 5-minute play of the karaoke example |
| Internal RAM | Least free internal heap, as the AC-4 notes measure it | Not more than 2 KB lower than without the screen |
| PSRAM contention | `panel/null` scan-out emulation on and off | Decode time per frame moves by less than 5 % |
| Does the picture match the design | `GET /panel` returns the frame as a BMP; compared against the sheet by eye and by a few pixel probes in a test | Matches |

## Measured

**2026-10-12, a DFRobot FireBeetle 2 (P4b, chip revision v1.3, 360 MHz), `sdkconfig.p4` +
`sendspin` + `panel` + `panel-null`, image 2,248,576 bytes (46 % of the 4 MiB slot free).** The
stream is the one Hearth's live lyrics test sends (a synthetic AC-3 karaoke programme, decoded to
2.0 on the standard I2S) for 60 s over Wi-Fi hosted on the onboard C6, 1,875 bursts of 32 ms.
"Screen filling" is the demo mode (`POST /panel/demo?mode=2`: a word-timed song looping, the fill
redrawn at 30 fps while the stream plays); "screen idle" is the idle screen, one refresh a second.
The counters are the board's own (`GET /status`, `sendspin`), polled once a second.

| Run | Screen | Underruns | Late | Dropped | Burst, mean | Burst, worst | Refreshes / min | Late cycles / min |
|---|---|---|---|---|---|---|---|---|
| A | idle | 1 | 5 | 0 | 4.51 ms | 12.9 ms | 62 | 1 |
| 1 | idle | 0 | 0 | 0 | 4.51 ms | 5.7 ms | 59 | 1 |
| 3 | idle | 0 | 0 | 0 | 4.49 ms | 6.0 ms | 60 | 0 |
| B (overlays made at the word) | filling | 0 | 0 | 0 | 4.69 ms | 9.9 ms | 997 | 79 |
| D | filling | 0 | 0 | 0 | 4.58 ms | 6.2 ms | 958 | 32 |
| 2 | filling | 0 | 0 | 0 | 4.58 ms | 6.0 ms | 960 | 37 |
| 4 | filling | 0 | 0 | 0 | 4.58 ms | 5.8 ms | 956 | 40 |
| C | filling | 6 | 1,331 | 0 | | | | |

- **The drawing's cost is small**: a refresh took 1.7 ms on average (the whole 1024 by 600 first
  frame 76 to 87 ms, once), about 960 of them a minute while a word fills, which is 1.6 s of one
  core in 60 s, **2.7 %**. A line's rows are rebuilt in 28 to 48 ms, once a line. The task's stack
  had 9 KB of 16 KB free. **A burst took 0.07 ms longer with the screen filling than with it idle
  (4.58 against 4.50 ms, +1.6 %)**, and the counters were clean.
- **Pre-creating each word's fill when the line is built** (the last build) halved the refresh time
  (3.7 to 1.7 ms) and the late cycles (79 to about 38 a minute), against creating them when the
  word's turn came (run B). The late cycles that remain are the cycles that rebuild a line or draw
  a word's first frame, each a few milliseconds past a 33 ms period, not an audible thing.
- **One run in five with the screen filling broke, and the cause was not found (run C).** Some 15 to
  20 s into the stream the board's player counted 2 and then 6 underruns and 6 resyncs, its `bursts`
  stopped at 544, and every chunk after that arrived late (1,331 by the end, 19,676 silence frames)
  until the server ended the stream at 60 s; the lyrics test passed, because the lines follow the
  server's clock and not the audio. It did not repeat in the three runs of the same image that
  followed (D, 2, 4), nor in the one filling run of the build before it (B). The first screen-idle
  run (A) had an underrun, five late chunks and a resync about ten seconds in, and recovered, with
  the screen doing nothing. Several other sessions were building on the host and using the same
  Wi-Fi at the time. **Not shown to be the screen, and not shown not to be**: a stall that never
  recovers is worth finding on its own (the player's recovery after a resync), and the check that
  would settle it is ten runs with the screen off against ten with it on, which is the board's
  time and was not spent.
- **Not in these numbers**: the DSI engine reading the frame buffer out of PSRAM 60 times a second
  (74 MB/s on the bus the decoder runs its code from, with `CONFIG_SPIRAM_XIP_FROM_PSRAM`) and the
  copy of a rendered area into the frame buffer by the 2D engine. Those are the kit's to measure; a
  frame buffer in PSRAM with no panel behind it has neither.
- **The standard 5.1 AC-4 decode** (the P4's heaviest, about a whole frame) was not run with the
  screen: the live test's stream is a light one, 14 % of a frame. The margin the screen has to fit
  into is the AC-4 5.1 profile's, and its measurement is the kit's first run of the heavy streams.

## Work

| Step | What | Size | Depends on |
|---|---|---|---|
| **W12a** | `libs/lyrics` `Stage` + tests | S | - |
| **W12b** | `panel` seam, `panel/none`, `panel/null` with LVGL, `lyric_screen`, fonts partition, the idle screen, the read-back route | L | W12a |
| **W12c** | `panel/dsi`: EK79007 + GT911, the board profile, backlight | M | W12b; the kit to *verify* |
| **W12d** | The rev3 image: `sdkconfig.p4-rev3`, `i2s_wide` PLL source, CI step, release list, update chooser rule, installer entry | M | - |
| **W12e** | Measurement on the FireBeetle with `panel/null`, results into this page and the docs | S | W12b: **done**, [Measured](#measured); the heavy AC-4 streams and the scan-out are the kit's |
| **W12f** | Docs: a P4 sink guide (the S3 and C6 have one), the karaoke guide's board section, matrices, ROADMAP | S | W12b, W12d |
| **W12g** | Bring-up on the kit ([below](#bring-up-on-the-kit)) | M | the kit |

### Bring-up on the kit

In this order, each with its own go/no-go; none of it can be done before the kit is on the bench:

1. Flash `hearth-sink-esp32p4-rev3` over USB (it is a v3.x chip, so the rev1 image refuses it).
   Confirm `/hardware` says revision v3.x and **no** revision notice.
2. Run the existing probe at 400 MHz; compare with the v1.3 numbers in
   [the P4 page](../docs/platforms/bare-metal/esp32-p4.md).
3. Open TDM at 16 × 32 on the three chosen pins and read the clocks with a scope or a logic
   analyser: BCLK 24.576 MHz at 48 kHz, WS 48 kHz. This is the check the whole TDM claim waits for.
4. Panel: backlight, then the colour bars from `panel/dsi`'s self-test, then the idle screen.
5. Touch: the GT911's address and a calibration-free tap on the gear.
6. A karaoke play from Hearth with the example song; compare against the FireBeetle numbers.
7. The ES9080 pair, when wired: the I2C map and the per-slot levels
   ([sink tiers, criterion 3](esp32-sink-tiers.md#exit-criteria-board)).

## Risks and open questions

| # | Item | What decides it |
|---|---|---|
| Q1 | Which board exactly (pins, panel controller, touch address) | The kit's model; the board profile is a Kconfig choice for this |
| Q2 | LVGL's right-to-left flex and `lv_label` bidi with Hebrew in a wrapping row | **Answered**: it works, with two things found (the default theme's 10 px gap between flex children, and LVGL mirroring an alignment under an RTL base, which put the fill on the wrong side until the alignment was left to LVGL); the Hebrew picture is the check |
| Q3 | Internal RAM: LVGL's allocator, the DSI driver's descriptors and the touch driver's buffers against a decoder that leaves 1 to 11 KB | **Half answered**: LVGL's heap is PSRAM (`lv_memory.cpp`), the frame buffer, render buffers, glyph caches and the task's stack are PSRAM, and the panel starts after the player (which came up with 289,687 B of internal heap free). What the panel then takes from internal RAM (its task's control block, the read-back server's 6 KB stack, small structures, the DSI and touch drivers' own) was **not measured**, and nor was the least-free figure during an AC-4 play: that is the next number to take |
| Q4 | Tearing with one frame buffer | The kit; two buffers cost 1.2 MB of PSRAM |
| Q5 | PSRAM bandwidth under the decoder's XIP fetches | The scan-out emulation, then the kit |
| Q6 | A touch control for the vocals needs the role to carry a request back to the server | A protocol decision, its own PR |
| Q7 | Auto-dim and a screensaver for a screen that is on for hours | Product, after the kit; the backlight PWM is there |
| Q8 | CJK and Arabic | A font subset and shaping; not in this work |
| Q9 | The image now links LVGL (MIT) and two OFL fonts and ships no notices file, as it does not for mbedTLS or `esp_hosted` | A notices fragment for the sink images, if the owner wants one: the apps' are generated and the firmware's have never been |
| Q10 | The lyric **document** the player holds is in internal RAM, about six times its text once held and ten at the peak (measured by the lyrics verification session on 2026-10-12: P4a at 233 KB free, a 16,000-byte document left 66 KB and once broke the play; S3 and C6 far less). The screen's own copies are rows of text in the heap and LVGL is in PSRAM; the document is the player's, and a PSRAM arena for it is the fix on a board that has the PSRAM | The player's lyrics budget (that session's change in `sendspin.cpp`), then an arena |

## Later

- The Function-EV's ES8311 codec as a sink output, so the kit makes sound with no DACs (needs the
  I2C init of the codec and the MCLK it asks for).
- The touch control of the vocals, with the role's request path (Q6).
- A subset CJK face in the fonts partition.
- An image for other 7-inch P4 boards: a profile plus a CI step each.
