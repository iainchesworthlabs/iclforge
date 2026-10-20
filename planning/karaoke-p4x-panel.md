# Karaoke on an ESP32-P4 (chip revision v3.x) with a 7-inch panel

!!! note "Status as of 2026-10-20: on the kit, the screen, touch and a 16-slot TDM play work; the clocks on the pins, a karaoke play and tearing are not yet checked"
    This is W12 of [karaoke phase 2](karaoke-phase2.md#w12-the-esp32-p4-with-a-panel), taken
    off the shelf because the owner's development kit was on its way and the firmware could be ready
    for it. The owner's brief, on 2026-10-12: a P4 of chip revision v3.2 or newer ("P4X") at
    400 MHz with 32 MB of PSRAM, 16 MB of flash and a 7-inch 1024 by 600 capacitive touch panel on
    MIPI DSI (the board is Espressif's ESP32-P4X-Function-EV-Board, whose user guide says chip
    revision **v3.1 or later**; this image's minimum, v3.1, covers it); it receives the multichannel stream and its lyrics, decodes on the chip, writes the
    decoded audio out of I2S in TDM to two DACs that are not connected yet, and draws the lyrics
    with the colour moving as the song does. Until the kit was here the firmware ran on the
    owner's FireBeetle 2 boards (revision v1.3, no panel) so that the compute could be measured.

    The kit arrived on 2026-10-20 and the image ran on it that day ([First run on the
    kit](#first-run-on-the-kit)): one crash, in this firmware's DSI code, and one fact about the
    touch controller, were found and are fixed. Only what that section lists has run on the kit.
    What was measured on a v1.3 board says so; what was read from ESP-IDF or a datasheet says so;
    the rest is a decision or a guess and is marked.

    | Step | State |
    |---|---|
    | W12a `Stage` in `libs/lyrics` | Built, 13 host test cases, mutation-checked |
    | W12b the panel, the screen, the preview tool | Built; the screens are in [the design sheet](../docs/hearth/sink-esp32-p4.md#the-screen), drawn by the firmware's own code on a computer |
    | W12c the DSI panel and touch | Written against ESP-IDF v6.1 and the vendor drivers' headers and built by CI. **First run on the kit 2026-10-20**: it aborted in `esp_lcd_new_dsi_bus` (fixed); then the bus, the EK79007 and the GT911 (at 0x14) start and an idle frame is drawn into the frame buffer. The glass and a tap are not yet checked |
    | W12d the rev3 image | Built (a local build found and fixed a bootloader 576 bytes over its window: Warn log level, 22,448 bytes); the update chooser picks it by chip revision (tests). **First run on a v3.2 chip 2026-10-20**: boots at 400 MHz, joins Wi-Fi, `/hardware` says revision 3.2; TDM on the pins is not measured |
    | W12e measured on the FireBeetle with no panel | See [Measured](#measured) |
    | W12f docs | [An ESP32-P4 sink](../docs/hearth/sink-esp32-p4.md) |
    | W12g bring-up on the kit | Started 2026-10-20: steps 1, 4 and 5 of [the list](#bring-up-on-the-kit) have a first result in [First run on the kit](#first-run-on-the-kit); steps 2, 3, 6 and 7 are not run |

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

The owner named it on 2026-10-12: [Espressif's ESP32-P4X-Function-EV-Board](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-function-ev-board/user_guide.html)
with its 7-inch 1024 by 600 capacitive panel. It is the ESP32-P4-Function-EV-Board with the chip
changed: its user guide's *Hardware Revision Details* say the main chip is **ESP32-P4 revision v3.1
or later**, and nothing else differs. So the rest of this page (16 MB of flash, an ES8311 codec and
its NS4150B amplifier, 100 Mbps Ethernet through an IP101GR, an ESP32-C6-MINI-1 for Wi-Fi, a
40-pin header J1) is read from the P4X user guide where it says it and from the older board's
BSP header where it does not. **One rule from that guide: Secure Download is not supported on
v3.1, so it is never enabled in this image.** The panel and the pins are a **board profile**,
chosen in Kconfig, with this board as the default and every pin marked "check against the
schematic"; Waveshare and others sell similar 7-inch P4 boards with other controllers and pins.

What the P4X guide does **not** say, and the bring-up list checks: which driver IC the screen
has (it links the datasheets of both the EK73217BCGA and the EK79007AD; this firmware has the
EK79007's driver and init), the number of DSI lanes, the touch controller (the older board's BSP
has a GT911), and the GPIOs of the codec.

| Fact | Value | Source |
|---|---|---|
| CPU | two cores, **400 MHz** on v3.x (360 MHz is the v1.x ceiling) | ESP-IDF `Kconfig.cpu`; v3.x user guide §1.1 |
| ESP-IDF's switch | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` **off** (the default); default minimum revision v3.1 | ESP-IDF v6.1 `Kconfig.hw_support` |
| I2S | a **160 MHz PLL clock source** (`I2S_CLK_SRC_PLL_160M`, "only supported on P4 hw_ver3") | ESP-IDF `clk_tree_defs.h`; v3.x user guide §1.8 |
| PSRAM | HEX mode, 200 MHz by default (250 MHz is offered on v3.x) | ESP-IDF `Kconfig.spiram` |
| MIPI DSI | APLL as a clock source and RGB565/666/888 conversion added | v3.x user guide §1.24 |
| Panel | EK79007, 1024 by 600, 2 data lanes, DSI PHY powered by LDO channel 3 at 2.5 V | `espressif/esp_lcd_ek79007` and the older board's BSP; **the P4X guide does not name the driver IC** |
| Backlight / reset | PWM on GPIO26 (J1 pin 31), reset on GPIO27 (J1 pin 38), software-configurable, to the LCD adapter's J6 | P4X user guide (both lines) |
| Touch / codec I2C | SDA GPIO7, SCL GPIO8 (J1 pins 3 and 5), shared by the GT911, the ES8311 and anything on the header | the older board's BSP header; the P4X guide lists the two GPIOs on J1 and names no touch controller |
| On-board codec I2S | MCLK 13, BCLK 12, WS 10, DOUT 9, DIN 11; amplifier enable 53 (also on J1, pin 35) | the older board's BSP header; **the P4X guide gives no codec GPIOs** |
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
fonts are SIL OFL: **Archivo** (Regular and SemiBold, 110 KB each), which the apps bundle, **Noto
Sans Hebrew**, which they bundle too, and **Noto Sans Greek and Cyrillic**, which only the firmware
has. Archivo covers Latin only (Basic, Latin-1, Extended-A, most of Extended-B and the Vietnamese
block, punctuation and currency; checked against its `cmap` on 2026-10-12: **no Cyrillic, four Greek
letters, no U+266A**), which is an earlier version of this page's claim corrected. The Hebrew file
the apps bundle is a *variable* font whose default instance is Thin (`tiny_ttf` has no axes, and
drew it as hairlines), so the firmware carries two static instances cut from it with `fontTools`'
instancer (Regular and SemiBold, 48 KB each). Greek and Cyrillic are cut the same way from Noto
Sans' variable font (Regular 400 and SemiBold 600 at normal width, subset to modern Greek with its
tonos, Cyrillic with its supplement and the combining marks they use; 77 KB each; the owner said yes
to them on 2026-10-12). They are embedded in the application image (`target_add_binary_data`),
473 KB of flash, and not in a partition of their own: an update over the network carries them, and
no partition table or package changes. LVGL's `tiny_ttf` rasterises glyphs at the size asked and
caches them; Greek and Cyrillic is each Archivo's fallback font and Hebrew is that one's. The two
Noto faces sit behind Archivo so that a Latin line never leaves the first file.

Not in this build, and said so: polytonic Greek and the Cyrillic Extended blocks, **CJK** (a usable
face is 5 to 16 MB, which the 4 MB slots have no room for; a subset face is a build step to write),
**Arabic** (Noto Sans Arabic is 845 KB and needs shaping LVGL does not do; its joining option
exists and is untried), and the Devanagari and Thai families (no shaping engine). **A line with a
letter outside the fonts' coverage is replaced by one sentence**, "This line is in a script this
screen cannot show." (the sentence pinned with the Hearth family's shared sink-screen spec), and not
drawn as the missing-glyph boxes; a title in such a script is left out; a music note, which none of
the fonts has, is dropped from a line. The test is `iclforge::lyrics::text_is_covered()` against a
table of the code point ranges the six font files carry (`main/panel/lvgl/glyphs.cpp`, regenerated
from the files' `cmap` tables). The licence of the new face is in Hearth's About, Licences
(`notices/fragments/hearth-fonts.txt`), with LVGL's (`hearth-lvgl.txt`).

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
P4X board's own ES8311 codec as an output so the kit makes sound before the DACs arrive
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
| S8b | **A script the fonts lack** | A line with a letter the six font files do not carry (Arabic, CJK, Indic, Thai, polytonic Greek) | One sentence in its place, "This line is in a script this screen cannot show."; the title in it is left out; a music note is dropped |
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

| Signal | P4X-Function-EV default | Status |
|---|---|---|
| Backlight (PWM) | GPIO26 | From the P4X user guide (J1 pin 31) |
| Panel reset | GPIO27 | From the P4X user guide (J1 pin 38) |
| Touch / I2C SDA, SCL | GPIO7, GPIO8, I2C0 | From the BSP header; **a GT911 answers on it at 0x14** (first run, 2026-10-20) |
| Touch INT, RST | not connected on the board | From the BSP header |
| DSI | 2 lanes, 1024 by 600 at 60 Hz, PHY LDO channel 3 at 2.5 V | From `esp_lcd_ek79007` |
| TDM BCLK, WS, DOUT | GPIO 20, 21, 22 (the FireBeetle defaults) | **On J1, pins 13, 11 and 12, where the P4X guide's table gives them no other function**; the older BSP header's note of GPIO19/20 as USB does not appear in it. Still to be checked with a meter against the schematic before a DAC is wired; GPIO54 (J1 pin 32) is the C6's reset and is not one to use |

The I2S pins are the only ones left to the bench: nothing else on the board uses 20, 21 and 22,
and the DACs are not wired, so they are confirmed when the kit is on the bench.

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

### First run on the kit

**2026-10-20, the kit on the bench**, a USB-C cable on the connector marked JTAG (USB-Serial/JTAG, so
`esptool` and a console need no buttons). What it is, read from the chip and its boot log:

| Fact | Value |
|---|---|
| Chip | ESP32-P4 **revision v3.2** (esptool computes it from the eFuses; ROM `esp32p4-eco7-20260109`; the vendor bootloader's `chip revision: v3.2`) |
| Flash, PSRAM | 16 MB (GigaDevice), 32 MB hex PSRAM; the vendor demo runs it at 250 MHz, this image at 200 MHz |
| MAC | `e8:f6:0a:e7:53:f9` (it is the USB serial number, so Windows names the port by it) |

esptool's feature line says "400MHz" for every P4: the string is fixed in its source, so it does not
tell a v1.x chip from a v3.x one. Read the revision from the eFuses or the boot log.

**What it came with.** Espressif's `esp_brookesia_demo` 1.2.0, built with ESP-IDF 5.5.2, with its
partition table at **0x9000** (Hearth's is at 0x8000). Its log names the display driver, `EK79007`,
and opens an ES8311 codec over I2C. Then it aborts in `bsp_display_indev_init`: the GT911 does not
answer at the address the BSP's default configuration uses (0x5D; Hearth's own probe of 0x5D fails
the same way), and an `ESP_ERROR_CHECK` ends the run. The same log has `ledc: GPIO 26 is not usable,
maybe conflict with others` for the backlight pin, which is the first thing to check on the glass.
The 16 MB of flash was read back before anything was written, so the vendor image can be put back.

| Bring-up step ([above](#bring-up-on-the-kit)) | First result |
|---|---|
| 1. Flash `hearth-sink-esp32p4-rev3` | Done over USB from main `fd3c51169`, profile `p4-rev3`. The vendor's table sits where Hearth's NVS partition is, so that region was erased first. The chip takes the image: minimum v3.1, chip v3.2, no revision notice; `/hardware` says revision 3.2, 400 MHz, 32 MiB PSRAM, `sink_max_slots_bits` 16 |
| 2. The probe at 400 MHz | Not run; the board suite's decode figures at 400 MHz are [below](#the-board-suite-on-the-kit) |
| 3. TDM at 16 x 32 on GPIO 20, 21, 22 | The sink **opens 16 slots of 32 bits (TDM)** and plays a 5.1 stream through them in real time ([TDM](#tdm)). **The clocks on the pins are not measured**: that needs a scope or a logic analyser |
| 4. The panel | A crash first, then the bus and the EK79007 start. The owner's photos show the screen as designed ([The glass](#the-glass)); tearing is not checked |
| 5. Touch | The GT911 answers at **0x14**. Its axes were turned half a turn from the display's and it was read too seldom: both fixed, and every button of the controls sheet answers a tap ([Touch](#touch)) |
| 6. A karaoke play | Not run |
| 7. The ES9080 pair | Not wired |

**The crash.** The image aborted on every boot that was watched, right after the Sendspin player
started. The abort was in ESP-IDF's `esp_lcd_new_dsi_bus`, in the switch that picks the DSI PHY's
reference clock. `display_dsi.cpp` set `phy_clk_src` to `MIPI_DSI_PHY_CLK_SRC_DEFAULT`, which in
ESP-IDF v6.1 is the alias for the v1.x source (PLL_F20M); the switch a v3.x build compiles lists the
crystal and the PLLs of the new silicon and has no case for it, so it calls `abort()`. Leaving the
field at zero makes ESP-IDF choose the default for the chip it was built for (the crystal on v3.x).
CI could not have found this: it builds the `panel-dsi` image and does not run it.

**After the fix**, the image boots at 400 MHz, joins Wi-Fi, is found by Music Assistant, and idles
with no underrun, late chunk or crash. The panel opens its bus on two lanes at 900 Mbit/s, the
EK79007 initialises, the GT911 is found, and `GET :8081/panel.bmp` returns the idle screen as the
design draws it (name, address, network, the server it is connected to, the firmware version, the
footer with the delay and *Adjust*). That read-back is the frame buffer in PSRAM, so it shows what
was drawn and not what the glass shows; the glass is next.

A first core dump, from the `ipc0` task (5,888 bytes), was already in flash when the DSI crash wrote
its own over it; it was not read, and no dump has appeared since the fix, over the hours the board
has been run.

#### The glass

The owner's photos, the same day, with the transparent protective film still on the panel (it is the
glare in them): the picture is upright and landscape, the background is dark, the words already sung
are in the accent orange, the word being sung is white with its underline, the progress hairline
runs under the header, and the footer has the board's name, the line, the delay and *Adjust* at the
right. That is the design's screen, in the demo song (`POST :8081/panel/demo?mode=2`). The backlight
is on. Not checked: tearing or flicker (stills do not show it), and the screen with the film off.
The panel's render time with the DSI engine reading the frame buffer out of PSRAM was 1.65 ms on
average over 3,627 refreshes (`GET :8081/panel`, a demo song filling; the first, whole frame took 60
ms), against 1.7 ms on the FireBeetle with no panel. The two are not the same chip or clock, and
nothing yet says what the scan-out costs the decoder.

#### Touch

The first taps did almost nothing: the owner tapped all over the glass and, once, *Adjust* opened,
and its buttons did not answer. Counters added to `GET :8081/panel` (`touch_reads`, `touch_failures`,
`touch_points` and where the last eight presses began) found two causes.

1. **The pointer was read too seldom.** LVGL reads it inside `lv_timer_handler()`, which the task
   called once a period: every 100 ms with nothing filling, on a 10 ms scheduler tick, while a finger
   is down for 50 to 150 ms. The controller answered every read (no failure) and reported 98 points
   over about three and a half minutes of tapping. The rest of each period is now waited out in 20 ms slices with
   a call after each, and the controller's bus runs at 400 kHz (the vendor BSP's speed): 25 reads a
   second on the idle screen, measured.
2. **The touch layer's axes are turned half a turn from the display's.** Presses came back at the
   opposite corner. A bottom-right tap was reported at the top left, bottom-left at the top right,
   top-right at the bottom left; the owner tapped bottom-right once, top-right twice and bottom-left
   three times and the board recorded one press at (20,17), three at (993,7), (982,8) and (994,34),
   and two at (21,585) and (30,587). `CONFIG_ICLFORGE_EXAMPLE_PANEL_TOUCH_MIRROR_X` and `_MIRROR_Y`
   are 1 for this board and `_SWAP_XY` is 0.

After both, the owner tapped *Adjust*, the delay's plus and minus, *Smaller* and *Larger* twice and
*Close*. The board recorded the last eight presses at (906,572), (395,469), (79,476), (628,469),
(807,464), (638,468), (803,455) and (896,567): each inside the button it was meant for, in that
order, on the 1024 by 600 layout. The sheet opened at once and the delay changed. *Smaller* and
*Larger* showed nothing on the idle screen, which has no lyric to resize, and the sheet did not say
what the size was: it now does ("Smaller", "Normal", "Larger", beside the delay's), and the line
being sung is rebuilt at the new size at once. Seen on the board's read-back with a simulated tap;
not yet seen on the glass with a song.

#### TDM

On the image carrying its profile, `PUT /layout 9.1.6` (sixteen slots) and a 30 s 5.1 AC-3 stream
(twelve copies of the suite's 5.1 fixture) played from this PC by `POST /play`:

- The sink logs `i2s-wide 48000 Hz 32-bit, 16 channels: 16 slots (tdm), master, bclk=20 ws=21
  dout=22`: the full 512-bit frame, which is the one that needs the 160 MHz PLL, and which a v1.3
  chip cannot open.
- 948 units, no resynchronised byte, no underrun, the least headroom in the 64 ms queue 4 ms.
- 30,336 ms of audio in 30,185 ms of wall clock. The sink blocks until the DAC has taken the samples,
  so this is the I2S controller's own pace, and it is the pace asked, to within 0.5% (the queue still
  draining at the end is some of the 151 ms).
- The levels: slots 0 to 4 (L C R Ls Rs) and slot 15 (LFE) carry audio and the ten others read zero,
  which is the ES9080 contract.

**What this does not show is the signal.** The clocks on GPIO 20, 21 and 22 (BCLK 24.576 MHz, WS
48 kHz) are not measured, and nothing yet says they leave the pins; that needs a scope or a logic
analyser.

#### The board suite on the kit

`p4-rev3` has a play list and a baseline, recorded on the kit with the image of this change
(`firmware/hearth-sink/baselines/p4-rev3.json`): the two 2.0 streams, as the other profiles play
them, and a row only this profile can play, the 5.1 stream on `9.1.6`, which fails if the sink opens
for fewer than sixteen slots. Three plays a row, no underrun, no resynchronised byte, the decode
stack 18 KB free:

| Row | Decoder, permille of a frame | The v1.3 board (360 MHz) |
|---|---|---|
| AC-3 2.0, 30 s | 56 | 61 |
| E-AC-3 2.0, 30 s | 38 | 41 |
| AC-3 5.1 on 16 slots, 2.5 s | 138 | not playable |

The kit takes 7 to 8% less time to decode where the clock is 11% higher. Why is not isolated: the
decoders run their code from PSRAM, which is 200 MHz hex here and does not scale with the clock. The
Sendspin rows and the AC-4 profile were not played on the kit.

**What the first run answers, and does not.** The display's driver is the EK79007 and its touch
controller is a GT911 at 0x14 and not 0x5D, turned half a turn from the display (Q1). Not measured:
the I2S clocks on the pins, the scan-out's cost to the decoder (Q4, Q5), tearing, the AC-4 5.1
decode, and a karaoke play with the screen.

## Risks and open questions

| # | Item | What decides it |
|---|---|---|
| Q1 | The board's panel controller, touch controller and codec pins | **Mostly answered 2026-10-20.** The board is the ESP32-P4X-Function-EV-Board; its user guide gives the backlight and reset GPIOs and the J1 header, and does not name the display's driver IC (it links EK73217BCGA and EK79007AD), the lane count or the touch controller. The kit's own demo logs the `EK79007` driver, and the GT911 answers at **0x14** on GPIO7 and GPIO8 (not 0x5D). Two lanes at 900 Mbit/s initialise without error; whether the glass is right is not yet seen. The codec's GPIOs are still the older board's BSP values, not read from this kit. The board profile is a Kconfig choice |
| Q2 | LVGL's right-to-left flex and `lv_label` bidi with Hebrew in a wrapping row | **Answered**: it works, with two things found (the default theme's 10 px gap between flex children, and LVGL mirroring an alignment under an RTL base, which put the fill on the wrong side until the alignment was left to LVGL); the Hebrew picture is the check |
| Q3 | Internal RAM: LVGL's allocator, the DSI driver's descriptors and the touch driver's buffers against a decoder that leaves 1 to 11 KB | **Half answered**: LVGL's heap is PSRAM (`lv_memory.cpp`), the frame buffer, render buffers, glyph caches and the task's stack are PSRAM, and the panel starts after the player (which came up with 289,687 B of internal heap free). What the panel then takes from internal RAM (its task's control block, the read-back server's 6 KB stack, small structures, the DSI and touch drivers' own) was **not measured**, and nor was the least-free figure during an AC-4 play: that is the next number to take |
| Q4 | Tearing with one frame buffer | The kit; two buffers cost 1.2 MB of PSRAM |
| Q5 | PSRAM bandwidth under the decoder's XIP fetches | The scan-out emulation, then the kit |
| Q6 | A touch control for the vocals needs the role to carry a request back to the server | A protocol decision, its own PR |
| Q7 | Auto-dim and a screensaver for a screen that is on for hours | Product, after the kit; the backlight PWM is there |
| Q8 | CJK, Arabic and polytonic Greek | A font subset and shaping; not in this work (Greek and Cyrillic are in, 2026-10-12) |
| Q9 | The image links LVGL (MIT) and three OFL fonts and ships no notices file of its own, as it does not for mbedTLS or `esp_hosted` | **Answered for the font and LVGL, 2026-10-12** (the owner: the font's licence goes where the licences are shown): Hearth's NOTICES.txt, which its About, Licences shows, names LVGL and the three typefaces (`hearth-lvgl.txt`, `hearth-fonts.txt`), and `check_hearth_package.py` requires them. The ESP-IDF components the images carry (Apache-2.0 and others, with picolibc's many BSD-style licences, LVGL's stb_truetype, and the Montserrat font LVGL's default theme links) are in the same notices, in `hearth-firmware.txt` and `notices/licences/ESP-IDF-components.txt` (made by `tools/generators/gen_esp_idf_notices.py` from an ESP-IDF checkout and the lvgl component; `tools/ci/test_firmware_notices.py` fails when the ESP-IDF the workflows build with is another) |
| Q10 | The lyric **document** the player holds is in internal RAM, about six times its text once held and ten at the peak (measured by the lyrics verification session on 2026-10-12: P4a at 233 KB free, a 16,000-byte document left 66 KB and once broke the play; S3 and C6 far less). The screen's own copies (the visible rows, a file with no times as plain text, the title and the names) are in LVGL's heap, which is PSRAM (`main/panel/lvgl/lv_alloc.hpp`: `LvString`, `LvVector`, `LvMap`), as is the plain-text label it builds, so a 12,000-byte text takes none of the internal RAM the decoder leaves; the document is the player's, and a PSRAM arena for it is the fix on a board that has the PSRAM (the lyrics verification session's `PsramScope`, #1396) | The player's lyrics budget (that session's change in `sendspin.cpp`), then an arena |

## Later

- The P4X board's ES8311 codec as a sink output, so the kit makes sound with no DACs (needs the
  I2C init of the codec and the MCLK it asks for).
- The touch control of the vocals, with the role's request path (Q6).
- A subset CJK face in the fonts partition.
- An image for other 7-inch P4 boards: a profile plus a CI step each.
