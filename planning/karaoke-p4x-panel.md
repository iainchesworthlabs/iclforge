# Karaoke on an ESP32-P4 (chip revision v3.x) with a 7-inch panel

!!! note "Status as of 2026-10-20: on the kit, the screen, touch, a 16-slot TDM play, a karaoke play with lyrics and the AC-4 5.1 decode all run; the glass costs the heaviest stream a tenth; the clocks on the pins need a scope and tearing was only looked at"
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
    | W12c the DSI panel and touch | Written against ESP-IDF v6.1 and the vendor drivers' headers and built by CI. **First run on the kit 2026-10-20**: it aborted in `esp_lcd_new_dsi_bus` (fixed); then the bus, the EK79007 and the GT911 (at 0x14) start and an idle frame is drawn into the frame buffer. The glass, a tap and a karaoke play are checked ([First run on the kit](#first-run-on-the-kit)); the scan-out costs the AC-4 5.1 decode about 10% ([AC-4 5.1 at 400 MHz](#ac-4-51-at-400-mhz)) |
    | W12d the rev3 image | Built (a local build found and fixed a bootloader 576 bytes over its window: Warn log level, 22,448 bytes); the update chooser picks it by chip revision (tests). **First run on a v3.2 chip 2026-10-20**: boots at 400 MHz, joins Wi-Fi, `/hardware` says revision 3.2; TDM on the pins is not measured |
    | W12e measured on the FireBeetle with no panel | See [Measured](#measured) |
    | W12f docs | [An ESP32-P4 sink](../docs/hearth/sink-esp32-p4.md) |
    | W12g bring-up on the kit | Done on 2026-10-20 except the two steps that need hardware that is not here: steps 1, 4, 5 and 6 of [the list](#bring-up-on-the-kit) ran, step 2 is the board suite at 400 MHz including AC-4, step 3 (the clocks on the pins) needs a scope and step 7 (the DACs) is not wired. See [First run on the kit](#first-run-on-the-kit) and [the second session](#the-second-session-on-the-kit) |

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

### D9. The kit's cable beside its radio (2026-10-20)

The owner's plan for the kit is a PoE splitter: its Ethernet leg into the RJ45 and its USB-C leg into
the *power* input, with no USB console and no USB flashing. So the sink takes the RJ45 as a network
(`net/wired/ethernet.cpp`: the chip's EMAC and the board's IP101GR on RMII, DHCP, the board's name as
its host name) next to the Wi-Fi the C6 gives it, and everything that answers on Wi-Fi answers on the
cable. The decisions, each one a line to change if the owner would have it otherwise:

- **Both are up, the cable is preferred.** The cable's interface has route priority 200 and the
  station's is 100; ESP-IDF's own number for Ethernet (50) would have put a board with both on Wi-Fi.
  A cable pulled falls back to the station (kept joined for it) and a cable put back returns the
  board to the cable, with no restart. A board with the cable in and no Wi-Fi network stored comes
  up on the cable alone.
- **A stored `network preference`: auto, ethernet or wifi** (NVS, `GET` and `PUT /network-preference`,
  and `network.preference` in `GET /status`). `auto` is the policy above. `ethernet` and `wifi` **pin**
  the board to one interface and switch the other off (the radio idle, or the EMAC stopped), which is
  what a person who wants the radio off near the DACs, or the cable out of a mixed network, asks for.
  The reading not taken: all three as a priority with the other interface kept as a fallback, in which
  case `ethernet` would be `auto` under another name. Because a pin has nothing to fall back to, a
  change made over REST is verified: the named interface is brought up first and the other switched
  off only once it holds an address; otherwise the change is undone.
- **The pins are Kconfig's, defaulted to the board's** (`ICLFORGE_EXAMPLE_ETH_*`): SMI MDC 31, MDIO
  52, PHY reset 51, PHY address 1, the reference clock in on GPIO50 (the PHY makes it), TX_EN 49,
  TXD0 34, TXD1 35, CRS_DV 28, RXD0 29, RXD1 30. They are ESP-IDF v6.1's `examples/ethernet/basic`
  defaults for the P4 and agree with Zephyr's devicetree for this board; the first run on the board
  reads the PHY's ID over SMI and logs it.
- **The PHY driver is ESP-IDF's generic 802.3 one**, the one its example uses on this board. `esp_eth`
  in v6.1 ships only that driver (the vendor ones, the IP101's among them, are registry components),
  so there is nothing more to fetch.
- **The profile locks it**: `CONFIG_ICLFORGE_EXAMPLE_NETWORK_ETHERNET=y` in `sdkconfig.p4-rev3` and
  in `p4-rev3`'s locked keys, so the profile's hash moved and its baseline is recorded again on the
  kit. The FireBeetle P4s, the S3s and the C6 do not have the key and their profiles are unchanged.
- **Not built** (the owner's separate tasks): a diagnostics view opened by three taps on the name,
  which would show the network and a control for the preference, and a screen for songs with no
  lyrics. What they read is `GET /status`'s `network.preference` and `network.interfaces`, and what
  they write is `PUT /network-preference`.

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
| PSRAM contention | `panel/null` scan-out emulation on and off | Decode time per frame moves by less than 5 % (**kit, 2026-10-20: not met** by AC-4 5.1, +10%, nor by E-AC-3 while the song fills, +12%; met by the scan-out alone on AC-3, +1.3%; see [the second session](#the-second-session-on-the-kit)) |
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
  **Run on the kit 2026-10-20**: [AC-4 5.1 at 400 MHz](#ac-4-51-at-400-mhz).

## Work

| Step | What | Size | Depends on |
|---|---|---|---|
| **W12a** | `libs/lyrics` `Stage` + tests | S | - |
| **W12b** | `panel` seam, `panel/none`, `panel/null` with LVGL, `lyric_screen`, fonts partition, the idle screen, the read-back route | L | W12a |
| **W12c** | `panel/dsi`: EK79007 + GT911, the board profile, backlight | M | W12b; the kit to *verify* |
| **W12d** | The rev3 image: `sdkconfig.p4-rev3`, `i2s_wide` PLL source, CI step, release list, update chooser rule, installer entry | M | - |
| **W12e** | Measurement on the FireBeetle with `panel/null`, results into this page and the docs | S | W12b: **done**, [Measured](#measured); the heavy AC-4 streams and the scan-out were run on the kit on 2026-10-20: [the second session](#the-second-session-on-the-kit) |
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
| 2. The probe at 400 MHz | The board suite at 400 MHz, with AC-4 on a scratch image: [below](#the-board-suite-on-the-kit) and [AC-4 5.1 at 400 MHz](#ac-4-51-at-400-mhz) |
| 3. TDM at 16 x 32 on GPIO 20, 21, 22 | The sink **opens 16 slots of 32 bits (TDM)** and plays a 5.1 stream through them in real time ([TDM](#tdm)). **The clocks on the pins are not measured**: that needs a scope or a logic analyser |
| 4. The panel | A crash first, then the bus and the EK79007 start. The owner's photos show the screen as designed ([The glass](#the-glass)); tearing is not checked |
| 5. Touch | The GT911 answers at **0x14**. Its axes were turned half a turn from the display's and it was read too seldom: both fixed, and every button of the controls sheet answers a tap ([Touch](#touch)) |
| 6. A karaoke play | Run (second session): Auld Lang Syne with its word-timed lyrics, 3,096 bursts, no underrun or late chunk, the words on the glass and the Adjust sheet used by the owner, who said it appeared and looked fine ([The karaoke play with the screen](#the-karaoke-play-with-the-screen)) |
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
the second session's [measurement](#what-the-scan-out-costs-the-decoder) says what the scan-out costs the decoder.

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

(Recorded again with the Ethernet image: 57, 40 and 142 permille, see [the cable](#the-cable-on-the-kit-2026-10-20).)

The kit takes 7 to 8% less time to decode where the clock is 11% higher. Why is not isolated: the
decoders run their code from PSRAM, which is 200 MHz hex here and does not scale with the clock. The
Sendspin rows and the AC-4 profile were not played in the first session on the kit; the second
session's are [below](#the-second-session-on-the-kit).

**What the first run answers, and does not.** The display's driver is the EK79007 and its touch
controller is a GT911 at 0x14 and not 0x5D, turned half a turn from the display (Q1). Not measured
in the first session: the I2S clocks on the pins, the scan-out's cost to the decoder (Q4, Q5),
tearing, the AC-4 5.1 decode, and a karaoke play with the screen. The second session measured all of
those but the clocks on the pins, which still need a scope.

### The second session on the kit

**2026-10-20, the same day.** Five images of one tree (`b2cff5ee1`), each built into its own
directory: the kit's image with the glass (profile `p4-rev3`) and with `panel-null` in place of
`panel-dsi`, and the AC-4 shape of the kit with the glass, with `panel-null` and with no panel; and
`hearth-testserver` built from the same tree. Every image was flashed over USB to COM7 after a check of the chip's MAC, with the bench lock
held, and nothing was restarted by HTTP. The PC served the four stream files it needed from a
folder of its own on one address for the length of the runs, and `hearth-testserver` dialled out to
the kit; neither is left running.

**Corrected the same day, [below](#the-third-session-on-the-kit).** The Sendspin rows of this
section that say layout 2.0 ran at the kit's own layout, 9.1.6 (sixteen outputs), and the three things
it left unexplained were the sixteen outputs (the slower burst), the TDM DMA queue (the internal
heap) and a stall at the start of some plays (the glitches), and none of them was the glass.

#### The karaoke play with the screen

The programme is the repository's `examples/karaoke/auld-lang-syne`: an AC-3 3/2 stream of
`bsmod` 7 (karaoke) at 384 kbit/s, played over and over, with its word-timed `.lrc` (586 bytes)
sent as the lyrics, and the playhead anchors the server sends every two seconds. The server paired
with the kit by the code it shows (Music Assistant had been connected to the kit before, and the
test server's pairing takes the board from a server it is not paired to); the server's own pairing
record was forgotten at the end of each run.

| | Play 1: layout 2.0, 99 s | Play 2: layout 5.1 sent by the server, 49 s |
|---|---|---|
| Bursts played of sent | 3,096 of 3,096 | 1,548 of 1,548 |
| Underruns, late, dropped, invalid, resynchronisations | 0, 0, 0, 0, 0 | 0, 0, 0, 0, 0 |
| Worst error of the board's clock against the server's | -310 µs | -364 µs |
| A burst (decode and render), mean, worst | 4.36 ms, 5.42 ms | 5.17 ms, 7.76 ms |
| Least internal heap while it played | 217,547 bytes | 46,443 bytes (largest block 28,672) |

- **The words reached the screen.** The board's log shows each line of the document taken in turn
  (`sendspin: lyrics item 1 line N`) and the panel was on the lyric screen for the whole play and on
  the idle screen after it.
- **The layout setting reached the board.** The server logged `took layout 5.1`, the report has six
  outputs (five with sound, the sixth, the LFE, silent) and the board went back to 2.0 when the
  server left. The lyric delay on the sheet is the board's own and the server cannot set it;
  `hearth-testserver` has no option for the output delay either, so **the server's delay setting was
  not exercised**.
- **What the screen cost, from `GET :8081/panel` before and after play 1:** 2,319 refreshes in 106.6
  s (22 a second); 2.3 ms a refresh on average over the play, which includes the owner's taps on the
  Adjust sheet (the demo song alone drew in 1.6 ms); the slowest refresh 84.7 ms; a line rebuild 48.9
  ms at most (the plan's 30 ms is not met: it was 38.7 ms in the demo song and 28 to 48 ms on the
  FireBeetle); 82 cycles longer than their 33 ms period (46 a minute); the panel task's stack 8,968
  bytes free. The audio had no late chunk beside any of it.
- **What the owner saw.** The owner watched the glass during play 1 and used the Adjust sheet (the
  board counted 42 presses during the play, the last eight inside the sheet and the footer), and
  said the test appeared and looked fine. That is all that was reported: **no one filmed it, and
  tearing, flicker and a word's timing against the sound were not measured**, only not noticed.
- **The internal heap at 5.1 is a finding.** The same stream at layout 2.0 left 217,547 bytes at its
  least; at layout 5.1 it left 46,443. The AC-4 play below leaves about 10 KB at either layout.

#### What the scan-out costs the decoder

Two images of one tree: the glass (`panel-dsi`: the DSI engine reads the frame buffer out of PSRAM
at 60 Hz, 74 MB/s, and the touch controller is polled 25 times a second) and `panel-null` (the same
drawing into the frame buffer, no DSI driver and no scan-out). On each, 30 s plays of the board
suite's two 2.0 streams over `POST /play`, six rounds in an alternating order (idle, filling,
filling, idle, ...), the screen switched between plays with `POST :8081/panel/demo?mode=0|2` (2 is
the demo song filling the screen, about 24 refreshes a second), a warm-up play first and dropped.
The figure is the decoder's time a frame, `/status`'s `us_per_frame` less the sink's wait and the
renderer's, the board suite's own definition. The median of six, with the least and greatest in
brackets, in microseconds of a 32 ms frame:

| Stream | `panel-null`, idle | `panel-null`, song filling | Glass, idle | Glass, song filling |
|---|---|---|---|---|
| AC-3 2.0, 192 kbit/s | 1,746 [1,743-1,750] | 1,827 [1,823-1,834] | 1,768 [1,766-1,773] | 1,862 [1,857-1,869] |
| E-AC-3 2.0, 96 kbit/s | 1,132 [1,122-1,135] | 1,265 [1,257-1,272] | 1,199 [1,190-1,209] | 1,339 [1,329-1,353] |

| Cost | AC-3 2.0 | E-AC-3 2.0 |
|---|---|---|
| The scan-out and the touch polling alone (glass idle against `panel-null` idle) | +22 µs, +1.3% | +67 µs, +5.9% |
| The drawing alone (`panel-null` filling against idle) | +81 µs, +4.6% | +133 µs, +11.7% |
| The song filling on the glass against the glass idle | +94 µs, +5.3% | +140 µs, +11.7% |
| Both, against a board with the panel off (glass filling against `panel-null` idle) | +116 µs, +6.6% | +207 µs, +18.3% |

- **The spread of six plays is under 1% of a figure, so these moves are real.** None of the 52 plays
  (48 measured and four warm-ups) had an underrun, a failed allocation or a resynchronised byte,
  and every one ended with `result=pass`.
- **Against the plan's pass (a decode time that moves by less than 5%) the answer is "no" for these
  two streams while the song fills, and "yes" for the scan-out alone on AC-3 and "just over" on
  E-AC-3.** Most of the move is the drawing and not the scan-out: the drawing alone is about two
  thirds of the largest figure, the scan-out alone a fifth to a third. The light streams are where a
  percentage of the decode time is the least telling: the most any of it is, +207 µs, is 0.65% of the 32 ms
  frame, against a decoder that uses 3.5 to 5.8% of it. The heavy stream, where 5% of the decode
  time is 30 permille of a frame, is the case that decides it, [below](#ac-4-51-at-400-mhz).
- **Not isolated:** why E-AC-3 moves twice as much as AC-3 for the same screen (the two decoders'
  tables sit in different places in PSRAM, which was not looked at), and what the touch polling
  alone costs (it is in the glass-idle figure, not separated from the scan-out).

#### AC-4 5.1 at 400 MHz

Three scratch images of this tree with the AC-4 decoder on (`sdkconfig.ac4` and the PCM-hash
`sdkconfig.measure` after the `p4-rev3` fragments and Sendspin): with the glass (`panel-dsi`), with
`panel-null`, and with no panel at all. The board suite's own `p4-rev1-ac4` rows were played on each
unchanged, five plays a row, under a scratch profile `p4-rev3-ac4` and a scratch copy of the suite
file (nothing in the repository was changed, and no such profile was added). The A-CPL and
companding streams were not played: they were not in the one folder the PC served. The v1.3 column
is the `p4-rev1-ac4` baseline (`firmware/hearth-sink/baselines/p4-rev1-ac4.json`: P4a, 360 MHz,
2026-10-12). A frame is 32 ms for AC-3 and E-AC-3 and 42.667 ms for AC-4.

The decoder alone (`POST /play` rows; permille of 32 ms; AC-4's are three frames long, so each is
good to about 6%):

| Row | v1.3, 360 MHz | Kit, no panel | Kit, `panel-null` | Kit, glass |
|---|---|---|---|---|
| AC-3 2.0 | 61 | 55 | 55 | 56 |
| E-AC-3 2.0 | 40 | 35 | | 38 |
| AC-4 2.0 music | 240 | 199 | 199 | 215 |
| AC-4 5.1 music, folded to 2.0 | 619 | 512 | 524 | 572 |
| AC-4 5.1 music onto `9.1.6`, 16 slots | | 597 | 614 | 669 |

The whole burst over Sendspin (the product's path; the board's mean time for a burst less its wait
in the sink, with its share of the frame in permille; 20 s of the three-frame AC-4 streams, looped;
the kit's rows at its own layout, **9.1.6, sixteen outputs**, which this section first called 2.0, and
the v1.3 board's at 2.0, the two slots its sink has):

| Row | v1.3, 360 MHz | Kit, no panel | Kit, `panel-null` | Kit, glass |
|---|---|---|---|---|
| AC-3 2.0 | 2,724 µs (85) | 4,871 µs (152) | 4,906 µs (153) | 5,071 µs (158) |
| E-AC-3 2.0 | 2,141 µs (67) | 4,299 µs (134) | | 4,489 µs (140) |
| AC-4 2.0 music | 8,075 µs (189) | 11,674 µs (274) | | 12,668 µs (297) |
| AC-4 5.1 music (the kit at 9.1.6) | 25,176 µs (590) | 28,369 µs (665) | 28,719 µs (673) | 31,669 µs (742) |
| Least internal heap, AC-4 5.1 | 27,687 bytes | 9,715 | 10,255 | 10,339 |
| Plays that glitched, of 5 a row, in the order above | 0, 1, 1, 1 | 0, 0, 0, 0 | 0, 0 (two rows run) | 0, 0, 0, 0 |

And the same AC-4 5.1 stream with the server asking for layout **5.1** (six outputs in the 16
slots), which is what the kit is for, 20 s a play, alternating the screen idle and the demo song
filling where there is a screen:

| Image | Plays | Glitched (an underrun and a resynchronisation, up to 27 late chunks) | Burst, median |
|---|---|---|---|
| No panel | 18 | 1 | 25.96 ms (61% of the frame) |
| `panel-null`, idle and filling | 6 and 6 | 0 | 26.40 and 26.53 ms |
| Glass, idle and filling | 9 and 9 | 2 and 3 | 29.26 and 29.57 ms (69%) |

- **The PCM is the v1.3 board's.** The hashes of the 2.0 and the folded 5.1 streams are the same on
  all three images and equal the baseline's (`f860e51f602ae753`, `7162259f3e596916`); the `9.1.6`
  row's hash (`6d36d7d2e8a070e9`) is new and equal on the three.
- **The decoder is faster than the v1.3 board's, the Sendspin path is slower.** With no panel the
  AC-4 rows decode 17% faster (512 against 619) at 11% more clock, and the AC-3 row 10% faster, but
  a burst over Sendspin takes 13% longer for AC-4 5.1 (28.4 against 25.2 ms) and 79% longer for
  AC-3 2.0 (4.87 against 2.72 ms), with a decode that takes 1.75 ms against 1.95. **Not
  isolated, and not the panel's** (the no-panel image is as slow): the baseline is from 2026-10-12
  and this tree is some 400 commits on, so it may be the tree and not the silicon; the same tree on
  the v1.3 board would say, and was not run (only the kit was flashed). **Explained by the third
  session: neither.** The kit's rows were at 9.1.6 and the baseline's at 2.0, and a burst costs about
  176 µs for each output; at 2.0 the kit's bursts are 3 to 6% shorter than the baseline's.
- **The glass costs the heavy stream a tenth, and the scan-out is nearly all of it.** `panel-null`
  against no panel is +0.35 ms a burst (+1.2%: LVGL's own presence); the glass against
  `panel-null` is +2.95 ms (+10.3%): the DSI engine's reads out of PSRAM and the touch polling,
  which were not separated. The song filling adds almost nothing on the glass (+0.1% at layout 2.0
  over four plays each, +1% at 5.1, and +0.5% on `panel-null` at 5.1). The decoder's code and tables run out of PSRAM, so the contention
  that Q5 feared is there, about 10% for AC-4 5.1 and 1 to 6% for AC-3 and E-AC-3. **Against the
  plan's pass, a decode time that moves by less than 5%, the heavy stream fails it**, with its margin
  still 26% of the frame at layout 2.0 on the glass.
- **At layout 5.1 the glass makes the stream glitch.** At layout 2.0 all 13 plays on the glass were
  clean (five in the suite, eight alternating with the screen filling or not). At 5.1, 5 of 18 plays
  on the glass glitched (an underrun and a resynchronisation each, one with 27 late chunks and
  about 1.1 s of lost audio) against 1 of 30 on the two images without a scan-out (Fisher's exact
  test, p about 0.02; 0.18 against the no-panel image alone). It happened with the screen idle as
  often as with the song filling (2 of 9 and 3 of 9), so the drawing is not what separates the
  images; the scan-out is, if the panel is the cause at all. The v1.3 baseline had the same kind of
  glitch once in five with no panel at layout 2.0, so this is a stream that runs close to its edge on
  both, and the cause of any of them is not found. **Explained by the third session: not the glass.**
  The 13 clean plays were at 9.1.6 with no settings message; every glitch began with the stream's
  first chunk reaching the board 300 to 600 ms late, with and without a scan-out, at about one play
  in five.
- **Internal RAM.** The AC-4 5.1 play leaves 9.7 to 10.4 KB at its least on all three images and
  the v1.3 board left 27.7 KB, so the kit is the tighter of the two; the panel changes that by
  nothing a play can see (Q3). Under AC-3 and E-AC-3 plays the panel (the screen task, its HTTP
  server, the drivers) takes 14 to 17 KB of the least-free internal heap (87.4 to 70.3 KB, and 65.7
  to 48.9 KB), against the plan's pass of 2 KB. At boot (the budget line, one boot of each) the
  three images read 295,779 to 300,675 bytes free, which is inside the boot-to-boot spread of the
  baselines, and the glass and `panel-null` a largest block 8,192 bytes smaller than the no-panel
  image's (270,336 against 278,528). **The 10 KB and the difference from the v1.3 board are the TDM
  DMA queue (196,608 bytes of internal RAM at every layout from three channels up, 24,576 at 2.0);
  fixed in PR #1607, [below](#where-the-internal-ram-goes-the-tdm-dma-queue).**
- **Is a `p4-rev3-ac4` profile worth adding?** As a bench-only profile with no published image (as
  `p4-rev1-ac4` is), **yes**: the rows play on the kit, the PCM matches, and its baseline would hold
  the figures above. Whether the kit should ship AC-4 is a different question and the numbers are
  mixed: 2.0 and the stereo fold are clean at 74% of a frame, 5.1 to the TDM slots glitches one play
  in five or six with the glass on and the heap is 10 KB. **No profile was added**; it is the
  owner's call.

### The cable on the kit (2026-10-20)

D9 built and run on the kit: the owner's cable to their router (100 Mbit/s full duplex), the Wi-Fi
and the USB cable on as well, image `v0.10.0-beta.1-5131-gbb6d603fd` (profile hash `ff9b27a6c4fd`).
The order was the owner's: the update path first, over Wi-Fi with the USB attached, then the cable.

**Before the cable: does the kit restart and update?** (`bench.json` said "not tried"; on the
v1.x boards a restart once left the C6 link dead, #1434.)

| Tried | Result |
|---|---|
| `ota.py push` of p4-rev3 images, ten times: eight accepted (the baseline image, a second one, the Ethernet image six times) and two that were meant to fail (below) | each accepted one restarted, came back on Wi-Fi 7 s after the restart, ran its 30 s trial and was accepted |
| `ota.py rollback` | restarted into the image before, on trial, accepted |
| An image that never reports healthy (`ICLFORGE_FIRMWARE_TEST=unhealthy`) | rolled back at the 300 s deadline, Wi-Fi up |
| An image that panics on its trial (`=panic`) | the bootloader rolled it back in about 25 s, Wi-Fi up, the core dump readable |
| `POST /restart`, eight times | the C6 link was back 10.3 s after each (reset reason `wdt`: the system reset of #1434) |
| A USB flash, 20 s, an OTA push (the sequence that failed 8 times in 16 on the v1.x boards), four times | all four accepted |

So the kit's restart and update are as good as the old boards' with #1434, and `bench.json` no longer
marks it as unknown.

**The cable.**

| Tried | Result |
|---|---|
| The PHY on the chosen pins | answers at SMI address 1 with ID `0243 0c54` (the IP101's); the pins are ESP-IDF's example defaults for the P4 and Zephyr's for this board |
| A cable put in while the board runs (auto) | the link seen, DHCP done and the board using the cable within about 6 s of each other, no restart; the Wi-Fi stays joined |
| Boot with the cable in and a Wi-Fi network stored | the cable up and answering 9.9 s after reset, the Wi-Fi joined behind it at 14 s |
| Boot with the cable out | waits 4 s for a link, then the Wi-Fi as before: network at about 16 s |
| Boot with the cable in and **no Wi-Fi network stored** (NVS erased, an image with none built in) | up on the cable alone, address at 8.9 s (the router's DHCP took 4.4 s of that), the player and mDNS started, `network: "wired"` in `GET /firmware`; Improv said *ready*, and a network handed over USB joined while the cable stayed in use (Improv answered with the cable's page address) |
| The page, `/api`, `/hardware`, `GET /log` | answer on the cable's address and the Wi-Fi's |
| mDNS | `hearth-e753f9.local` has an A record for each address |
| A Sendspin play (`hearth-testserver` dialling the board, the suite's way, its 30 s AC-3 2.0 stream) | four plays on the cable (three with the Wi-Fi joined as well, one with no Wi-Fi stored), 938 of 938 bursts and no underrun each, one of them with 1 late burst and 1 resync at its start; one on the Wi-Fi address, 938 of 938, clean |
| `ota.py push` to the cable's address | the same image with `--force`: the board restarted, answered on the cable 9.9 s after reset, the trial was accepted |
| `PUT /network-preference` wifi / ethernet / auto with the cable in | each took effect in a few seconds: `wifi` left only the Wi-Fi (the cable's address stopped answering), `ethernet` only the cable (the Wi-Fi off), `auto` both again; the setting survived a restart |
| `PUT /network-preference ethernet` with no cable | not kept: the preference stayed `auto` and `wifi` stayed on throughout (the first design stored it first and undid it, and `/status` read "no address" for five seconds; the order was changed) |
| **The cable pulled and put back**, four times by the owner (the board idle, three times; playing, once) | the Wi-Fi was reported as the interface in use at the first poll after each pull (half a second apart), with no restart (uptime ran on unbroken); each return to the cable took 4.1 to 4.9 s from the link to the address, same address. A Sendspin play over the Wi-Fi with the cable out: 938 of 938, clean. `PUT ethernet` with the cable out: not kept, the Wi-Fi stayed on. A play over the cable pulled out from under it: 926 bursts and no underrun until the pull; the board was `idle` with no connection afterwards, and the next play (cable) was 938 of 938 (one underrun and a resync at its start) |
| A bad **Ethernet** image pushed to the cable's address: one that panics on its trial and one that never reports healthy | the first was rolled back by the bootloader (about 25 s), the second at the 300 s deadline; the board was back on the cable each time. (The earlier bad-image rows used the images without the cable) |
| **A kit on the cable alone**: `PUT ethernet`, then an update to the cable's address | the Wi-Fi off (its address stopped answering), the update restarted the board, which answered on the cable alone and passed its trial with the preference still `ethernet`; `PUT auto` brought the Wi-Fi back in a few seconds |
| The board suite, `p4-rev3`, three plays a row, the HTTP source fetched over the cable | AC-3 2.0 57 permille (56 before the cable), E-AC-3 2.0 40 (38), AC-3 5.1 on sixteen slots 142 (138): no underrun, no resynchronised byte, the decode stack 18.4 KB free; boot: 278,659 bytes free, largest block 253,952 (worst of three boots). The baseline is recorded again |

What it costs: the image is 2,518,416 bytes against 2,466,880 (the budget is 2,600,000); at boot the
internal heap reads 279,103 to 284,487 bytes free with a largest block of 253,952 to 258,048,
against 295,779 to 307,147 and 270,336 to 278,528 for the image without it, so the cable's
driver, its DMA buffers and its task take about 15 to 25 KB, and the headroom the budget line prints
(the largest block less the biggest single ask) is 221 to 225 KB against 245 KB.

A Sendspin play starts with a late burst or an underrun and a resync about one time in four, on
either interface (cable 3 of 9 plays, Wi-Fi 1 of 5 in this session); the suite counts it as a
glitched play, not a failure, and it is not the cable's. The third session found it: lwIP's TCP receive
mailbox refused the first segments ([below](#what-the-glitches-were-the-first-chunk-comes-late)), and it
was worse over the cable (16 plays in 20) than over Wi-Fi (7 in 20).

**Not tried:** a PoE splitter (the owner's next step: the USB cable stays in the drawer, but the
update path above has been run with the cable alone); 10 Mbit/s or half duplex; a static address;
IPv6; a long soak; a DHCP server that is slow (boot waits 15 s for the cable's address under `auto`
and then joins the Wi-Fi, the cable going on asking).

### The third session on the kit

**2026-10-20, the same day.** The three findings the second session left unexplained, and what
each was. Scratch images of `main` at `c7d2e126c` (built into their own directories, flashed to
COM7 after the MAC check with the bench lock held; the PC's `hearth-testserver` dialled out to the
kit, with the owner's yes, and was stopped after each series; nothing was restarted by HTTP):
the AC-4 shape of the kit with the glass, with `panel-null`, and with the fix below, with a trace of
how a stream starts (`sendspin.start:` in the console) added to the player. The v1.3 board P4a was
**not** flashed: it was off the bench (no COM10, no answer on its address), and the first finding
made the run unnecessary.

#### The layout the Sendspin rows ran at (a correction)

The second session's Sendspin rows say "layout 2.0". They were played with the server asking for no
layout, and then the kit's player plays at the layout it has, which is **9.1.6: sixteen outputs**.
The sink line says so (`16 channels: 16 slots (tdm)`), the report's `layout` is `null`, and the
server logs `states ... layout 9.1.6`. The `POST /play` rows are at 2.0 as labelled, and the
karaoke play 1 was at 2.0 (two levels in its report, two outputs on standard I2S). So "AC-4 5.1 clean at layout 2.0, glitched at 5.1" compared 9.1.6 with no settings message
against 5.1 with a settings message each play, and "the heap a wider layout takes" compared 2.0
(standard I2S) with 5.1 (TDM).

#### Why the Sendspin burst was slower than the v1.3 board's: sixteen outputs

A burst's decode is the same work at any layout; the rest of it (the renderer, the patch, the trims
and delays, the level meter and the copy into the playout) is done for every output of the layout,
and costs **about 176 µs a burst for each output** (29 µs a 256-sample block). The v1.3 board's
sink has two slots and ran its rows at 2.0. The same rows on the kit at 2.0 are faster than the
baseline's, in line with the decoder alone (microseconds a burst, the median of the plays in the
row):

| Row | v1.3, 360 MHz, 2 outputs | Kit, 2.0 (2 outputs) | Kit, 5.1 (6) | Kit, 9.1.6 (16) |
|---|---|---|---|---|
| AC-3 2.0 | 2,715 | 2,576 (-5%) | 3,282 | 5,034 to 5,135 |
| E-AC-3 2.0 | 2,145 | 2,057 to 2,083 (-3%) | | 4,564 |
| AC-4 2.0 music | 8,075 | 7,675 (-5%) | | |
| AC-4 5.1 music, folded to 2.0 | 25,176 | 23,733 (-6%) | 27,652 (glass, after the fix below) | |

Two to five plays a cell, 20 s each (the baseline's are its five plays). The gap was the layout, and neither the tree (400 commits on) nor
the silicon: no flash of the v1.3 board was needed. Four outputs more than 2.0 add 0.70 ms to the
AC-3 row (3,282 against 2,576) where 14 more add 2.46 ms (5,034): the cost is linear in outputs. A
stream that fills 2 of 16 outputs still pays for 16: skipping outputs that carry nothing is the saving
on offer, and not taken here.

#### Where the internal RAM goes: the TDM DMA queue

A TDM line on `sink/i2s_wide` clocks sixteen 32-bit slots whatever the layout, and its slot mask
enabled all sixteen, so ESP-IDF sized the 64 ms DMA queue (96 descriptors of 32 frames) at 16 x 4
bytes a frame in internal RAM: **196,608 bytes at every layout from three channels up**, 24,576 at 2.0
(standard I2S). That is the whole of the 171,104-byte gap between the 2.0 and the 5.1 play (172,032 of
queue), and of the AC-4 plays' 10 KB: the decoder (whose allocations below 16 KB try internal RAM
first) takes whatever is left.
The fix (PR #1607) enables the layout's channels in the mask and keeps `total_slot` at sixteen, so
the frame on the wire is the same and the queue holds the channels it carries (4 bytes a frame each).
Least free internal heap during a 20 s karaoke play (`sendspin.stream` line), the same programme:

| Layout | Queue, before | Queue, after | Least free, before (same day) | Least free, after |
|---|---|---|---|---|
| 2.0 (standard I2S) | 24,576 | 24,576 | 218,671 | 219,955 |
| 5.1 | 196,608 | 73,728 | 46,139 | **171,151** |
| 9.1.6 | 196,608 | 196,608 | the same (the 2.0 stream: 68,831) | 44,655 (the 2.0 stream: 69,219) |

An AC-4 5.1 play ends with 11 to 15 KB free on either image: the decoder takes what the queue gave
back, which is faster memory than its PSRAM spill, and its Sendspin burst at 5.1 on the glass falls
from 29.65 ms to **27.65 ms** (-6.7%; ten plays and thirty-two). The pacing is evidence, not proof,
that the controller takes one sample per enabled slot: the plays whose first chunk was on time
ran in real time with no underrun, as on the images before it.
**The slots on the pins were not looked at** (no scope).

#### What the glass costs the decoder, and what was tried

AC-4 5.1 at layout 5.1, the burst's median, 20 s plays, the same image apart from the scan-out:

| | Burst | Against `panel-null` |
|---|---|---|
| `panel-null`, after the fix (8 plays) | 25.27 ms | |
| The glass at 60 Hz, after the fix (32 plays, idle and the song filling) | 27.65 ms | +2.38 ms, +9.4% |
| The glass at 40 Hz: the pixel clock at 34.5 MHz instead of 52 (10 plays) | 26.91 ms | +1.64 ms, +6.5% |

* **The cost is the scan-out's bytes**: 2.4 ms at 60 Hz and 1.6 at 40 (a linear 0.04 ms a hertz), and
  the fix to the queue did not change it (the second session had +2.9 ms, +10.3%, on the other
  image). The decoder's code and tables are in PSRAM behind the L2 cache, and the DSI engine reads
  the frame buffer from it at 74 MB/s at 60 Hz.
* **The AXI interconnect's arbitration priorities do nothing.** `axi_icm_ll` can raise the cache's,
  the CPU port's (SDMMC, the AHB DMAs) or the DSI DMA's QoS; four settings (cache 8 and 15, CPU 8,
  cache and CPU 15), two plays each, alternating with four blocks of the default, all gave 27.57 to
  27.75 ms (the default 27.58 to 27.77). Nothing in the tree sets them and nothing was kept.
* **A slower refresh buys a third of the cost back** and was run for the burst's sake only: **what it
  does to the picture was not seen** (flicker, the panel's tolerance of a pixel clock under its
  datasheet's, tearing on a moving fill). The 40 Hz image is a scratch build, `p4fx-hz40-dsi-ac4`, and
  nothing in the tree sets the clock; it is the owner's to look at before it is a setting.

#### What the glitches were: the first chunk comes late

Every glitch of the second session's AC-4 5.1 series, and of the three series here, began in the
first five seconds of a play, with the stream's first chunk reaching the player **300 to 600 ms after
the stream's start message** (the server stamps it 360 to 390 ms after the start; the board states
250 ms of lead). The player had nothing to do in that time: the settings that came with the start
are applied in 0.5 to 3 ms, the sink and the stream are ready 1 ms after the start, the ring waits
for the chunk, and the first burst's long "decode" in the trace is the player padding the sink with
silence until the chunk's time. In a late play the silence is 256 to 4,000 frames where a normal one
has 15,000 to 18,000, the chunks that follow find their time gone (`late`, then an underrun and a
resynchronisation) and the first half second to second of the song is lost.

It does not depend on the glass. Plays whose first chunk was late, of all the Sendspin plays of the
session:

| Image | Plays | First chunk late | Glitched |
|---|---|---|---|
| The glass, after the fix | 118 | 24 (20%) | 14 |
| The glass, before it | 24 | 4 (17%) | 4 |
| `panel-null`, after the fix | 38 | 6 (16%) | 3 |
| The glass at 40 Hz | 10 | 1 | 1 |

nor on the codec (AC-3, E-AC-3 and AC-4 plays), nor on whether the server sends a settings message
(15 and 15 plays alternating: 4 and 4 late). The second session's "5 of 18 against 1 of 30" was
different hours of a rate that moves from one hour to the next (6 of 12 plays in one 12-play series
here, 1 of 12 in the next). 
The trace puts the delay between the PC's server and the board's server task. The player's ring wait is
0, the long handlers of the server task are the pairing steps (once, at connect), and the small
WebSocket frames of the first second (126 to 1,862 bytes) show reads of 300 ms and more with the rest
of the frame missing. It is no worse over Wi-Fi than on a wire: on the Ethernet image the same
programme's 4 s plays were late 7 times in 20 over Wi-Fi and **16 times in 20 over the cable**.

**The cause is lwIP's.** It posts each TCP segment it takes in to a per-socket mailbox and refuses the
segment when the mailbox is full (the help of `CONFIG_LWIP_TCP_RECVMBOX_SIZE` says so). The default,
6, is sized for full segments, the window over the segment size, 5,760 / 1,440, plus two. A sender that
writes a WebSocket frame's header and its payload as separate segments puts small ones in it, and at
the start of a play the chunks arrive faster than the server task reads them. The refused segment
comes again 250 ms or more later, and the first chunk with it. PR #1610 sets the mailbox to 32 entries
(128 bytes a socket) in `sdkconfig.p4`:

| 20 pairs of 4 s plays, one programme, the Ethernet image | Late first chunk | Glitched |
|---|---|---|
| Before, over Wi-Fi | 7 | 5 |
| Before, over the cable | 16 | 13 |
| After, over Wi-Fi | **0** | 0 |
| After, over the cable | **0** | 0 |

**Not shown:** how the PC's server writes a frame (no packet capture: the session has no rights for
one, and lwIP's statistics are off in the image), so the mechanism rests on lwIP's description and on
the result; and the same default on the S3 and the C6, which were not played.

#### What was not done

* The v1.3 board (P4a) was not flashed: it was off (no COM10, no answer), and the burst's cost per
  output made the run unnecessary.
* The slots on the pins, and the I2S clocks, are not looked at: no scope.
* The 40 Hz scan-out was run for its burst only; **what it does to the picture was not seen.**
* Skipping the outputs a stream does not fill (176 µs a burst each) was not tried.

## Risks and open questions

| # | Item | What decides it |
|---|---|---|
| Q1 | The board's panel controller, touch controller and codec pins | **Mostly answered 2026-10-20.** The board is the ESP32-P4X-Function-EV-Board; its user guide gives the backlight and reset GPIOs and the J1 header, and does not name the display's driver IC (it links EK73217BCGA and EK79007AD), the lane count or the touch controller. The kit's own demo logs the `EK79007` driver, and the GT911 answers at **0x14** on GPIO7 and GPIO8 (not 0x5D). Two lanes at 900 Mbit/s initialise without error; whether the glass is right is not yet seen. The codec's GPIOs are still the older board's BSP values, not read from this kit. The board profile is a Kconfig choice |
| Q2 | LVGL's right-to-left flex and `lv_label` bidi with Hebrew in a wrapping row | **Answered**: it works, with two things found (the default theme's 10 px gap between flex children, and LVGL mirroring an alignment under an RTL base, which put the fill on the wrong side until the alignment was left to LVGL); the Hebrew picture is the check |
| Q3 | Internal RAM: LVGL's allocator, the DSI driver's descriptors and the touch driver's buffers against a decoder that leaves 1 to 11 KB | **Half answered**: LVGL's heap is PSRAM (`lv_memory.cpp`), the frame buffer, render buffers, glyph caches and the task's stack are PSRAM, and the panel starts after the player (which came up with 289,687 B of internal heap free). What the panel then takes from internal RAM (its task's control block, the read-back server's 6 KB stack, small structures, the DSI and touch drivers' own) was not measured on 2026-10-12. **Answered on the kit 2026-10-20**: under an AC-4 5.1 play the least-free internal heap is 9.7 to 10.4 KB with the glass, with `panel-null` and with no panel at all (the v1.3 board left 27.7 KB: the TDM DMA queue, see the third session), so the panel takes nothing a play can see at the decoder's floor; under AC-3 and E-AC-3 plays it takes 14 to 17 KB (the pass was 2 KB); at boot the largest block is 8,192 bytes smaller with the panel ([AC-4 5.1 at 400 MHz](#ac-4-51-at-400-mhz)) |
| Q4 | Tearing with one frame buffer | The kit; two buffers cost 1.2 MB of PSRAM. **Looked at 2026-10-20**: the owner watched a karaoke play, with the Adjust sheet in use, and said it appeared and looked fine; no tearing was reported, and it was not filmed or measured. Not settled for a fast-moving fill |
| Q5 | PSRAM bandwidth under the decoder's XIP fetches | **Answered on the kit 2026-10-20**: the glass costs the decoder a standing 1.3% (AC-3), 5.9% (E-AC-3) and about 10% (AC-4 5.1) against `panel-null`, and filling the screen adds drawing cost that is 5 to 12% on the light streams and 0.1 to 1% on AC-4 5.1 ([the second session](#the-second-session-on-the-kit)). The glitches are not the glass's ([the third session](#the-third-session-on-the-kit)); after the DMA fix the glass costs the AC-4 5.1 burst 2.4 ms at 60 Hz (+9.4%) and 1.6 ms at 40 Hz (+6.5%) against `panel-null`, and no AXI priority setting changes it |
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
