# The Meters view: an instrument display for the no-lyrics screen

!!! note "Status as of 2026-10-26: stage 1, round 2 (an adaptive layout); nothing is built into the firmware, and the owner has not picked"
    The owner, who turned down all seven speakers options, gave a reference instead: the displays of RME's audio
    interfaces (the ADI-2/4 Pro SE and the Fireface UCX II), *"another screen preference would be something like this"*.
    This page is the prototype of that as a new choice of what the screen shows without lyrics, **Meters**: what it
    looks like at 1024 x 600, 480 x 320 and 240 x 240, where its spectrum would come from, and what each part costs.
    Everything here was drawn by the firmware-style views of `firmware/hearth-sink/preview/meters_options/` through LVGL's
    software renderer, over the made-up music of `preview/speakers_options/scene.hpp`, with the analysis of
    `libs/dsp/include/iclforge/dsp/spectrum.hpp` (the code the board runs, held to the calibrations of this page by `libs/dsp/tests/test_spectrum.cpp`). **Not run on a board.** Pictures:
    `planning/p4x-meters-options/overview-1024x600.png`, `overview-480x320.png` and `overview-240x240.png` (A, B and C side by side,
    a row a room); the measurements are `measure-inv32.csv`, `measure-inv128.csv` and `measure-inv256.csv` beside them.

    **Round 2 (2026-10-26).** The owner saw round 1 (*"a good start but not quite there"*) and asked for an **adaptive layout
    system** instead of one design: the representation chosen from the display's size class and the number of outputs of
    the room. [Round 2](#round-2-an-adaptive-layout) is that rule, with a picture of every cell. Round 1's A is now the
    `pair`, its B the `flat` and `grid`, and its C is dropped (it is three analyses and is in neither reference).

## Round 2: an adaptive layout

The 31-band spectrum of the main mix is the same under every representation and always on top; what changes is **how the channels
under it are shown**, which has to survive a 240 x 240 screen with two meters and a 1024 x 600 one with sixteen. The prototype is
`firmware/hearth-sink/preview/meters_options/adaptive.cpp`; every picture below is from it, through LVGL's software renderer, over
the same made-up groove (2.0 and a stereo PCM stream, 5.1, 7.1.4 with flying objects, 5.1 played on a 9.1.6 room), and **none of it has
run on a board**. The audio path is the one tap of round 1: a mono mix of the rendered outputs. Everything new reads the per-output
peaks that already exist (every 50 ms), so it adds nothing to the audio path.

### The representations

| Name | What it shows |
|---|---|
| `pair` | A **stereo L/R master**: the room's own L and R in a 2.0 room, **the stereo downmix of the room** (L + 0.5 C + 0.5 Ls, powers) in a bigger one, two horizontal bars with a peak tick and the numeric peak, and a status line. |
| `none` | No meters: a line of text (the layout, the rate, the held peak). |
| `sum` | The **surround sum**: six bars, **L C R LFE Ls Rs**, whatever the room is: an output is folded into the master channel nearest its azimuth (a 7.1.4 room's rear speakers and heights go to the side they are on), the level the power sum of its members' peaks. |
| `top4`, `top6`, `top8` | A **fixed number of bars bound to the loudest active channels**, the name under each changing when its binding does (rules below). |
| `mx` | A **micro matrix laid out like the room**: one square for each output, on a top-down map (front at the top, the ear-level speakers on an ellipse by azimuth, the heights on an inner one, a ceiling speaker in the middle, the LFE behind), its colour the level (dark, green by -18 dB, amber by -6, red at full scale), its name inside when it fits. |
| `mxg` | The same squares in a **plain grid** of n. |
| `flat` | A bar for **every output**, in the room's order. |
| `grid` | A bar for every output, **grouped in space**: Front (with wides) / Surround / Rear / Height / LFE, a gap and a caption between groups, the numeric peak above each bar. |
| `focus` | **One channel larger**: its name, its meter and its number at the left, **its own spectrum** above, the other channels small beside it with the chosen one on a plate. A tap on a channel anywhere else focuses it; a tap on the plate returns. While it is focused the tap that feeds the spectrum is the one output's, not the sum (**no second analysis**: the same ring, the same 101 kflops, a different input). |

### The rule

It reads two things from the board's capabilities, never its name: **`display.width`** (the size class) and **the number of connected outputs**.

| Size class | 2.0 | 5.1 | 7.1.4 | 9.1.6 |
|---|---|---|---|---|
| **L**: width at least 800 (1024 x 600 stage 1024 x 472, 800 x 480) | `pair` | `grid` | `grid` | `grid` |
| **M**: 400 to 799 (480 x 320, stage 480 x 216) | `pair` | `sum` | `sum` | `sum` |
| **S**: under 400 (240 x 240, stage 240 x 144) | `pair` | `pair` | `pair` | `pair` |

Every cell has a still in the shared notes folder (`meters-view-options-2/cells/`, and all of them in `cells-overview.png`); the cell of a room with two outputs
is a pair everywhere because there is nothing to fold. At **L** the spectrum is stacked on top and the grouped multi-channel meters
at the bottom (the owner's *"that's probably how we'd do it"*): 6, 12 or 16 bars with their names and numbers. At **M** the six masters are what fits,
the heights folded into their sides so the spatial spread is still visible. At **S** two thin bars under the spectrum.

**Why these, from the pictures:**

- **S, `pair` or `none`?** `pair` reads better. Two 8 px bars take 20 px under the spectrum where the line of text of `none` takes 17, so the spectrum is 3 px shorter, and
  two thin bars are **not** the chaotic texture the owner expects of more than two; they give the left/right balance and the **clip warning** (amber, red)
  that a number alone does not, and the stereo downmix stays honest in a bigger room. `none` is calmer and cheaper (0.11 to 0.13 Mpx/s against 0.16 to 0.17, 41 objects against 53), and
  is the choice for a person who wants only the spectrum; both are in `overview-240x240-pair-or-none.png`.
- **M, `sum`, `top6`, `mx` or `mxg`?** `sum` is the one that reads: six fixed names, six bars, the same at 5.1 and 9.1.6 (a 5.1 room's six are all
  real channels). `top6` works but the names move (7 to 35 changes a minute, below) and a person has to read them each time. `mxg` (12 or 16 coloured squares
  with names in them) reads well and is the cheapest (0.19 to 0.31 Mpx/s against 0.31 to 0.48). `mx` as a room map is **too small at 480**: squares of 14 to 20 px with no names. It is a good
  second choice for a person who wants the spatial picture and the least drawing.
- **L, `grid`, `top8`, `mx`?** `grid` shows every output with its name and its number in its place in space, which is what a room of 12 or 16 has to say;
  `top4` and `top8` show the channels that matter now but not the ones that are quiet (a quiet height speaker is information too); `mx` is **half the pixels**
  (0.8 to 1.0 Mpx/s against 1.2 to 1.8) and shows the room, but a colour is harder to read to a decibel than a bar with a number.
- **`focus`** is worth having at every size above S: the tap on a bar is the only way to see one channel's spectrum, and it costs no more than a spectrum.

### Keeping the top-N bars still

A label that changes is a flicker unless it is rare. The rule (`Binder` in `adaptive.cpp`): the choice is made on each channel's **envelope** (a peak that falls 24 dB a second); a slot **keeps its channel for at least a second**;
a channel that is not bound **takes the place of the weakest bound one only after it has been louder by 6 dB for 0.4 s**; a bound channel **silent (under -50 dB) for two seconds** frees its slot and the bar says "-"; a label
changes **only** at a rebinding and the new bar starts from its channel's level with its hold cleared. Changes of a binding in a minute of the groove (`measure-adaptive-inv256.csv`):

| Room | top4 with the rule | top4 without | top8 with | top8 without |
|---|---|---|---|---|
| 5.1 | 7 | 417 | 6 | 6 |
| 7.1.4 (objects moving) | 20 | 830 | 34 | 1,291 |
| 9.1.6 (a 5.1 stream) | 17 | 931 | 34 | 1,611 |

Without it the bars rebind 14 to 27 times a second (a top-N that is just the loudest N of each step); with it, once in two to ten seconds. `top6` on the 480 screen is 0 to 35 a minute.

### What the pictures cost

Pixels LVGL flushed, a minute of each, 20 updates a second, `LV_INV_BUF_SIZE` 256 (the exact part of what a view costs; the speakers view is 0.02 to 0.17 Mpx/s):

| Representation | 1024 x 600 | 480 x 320 | 240 x 240 | Objects at 1024 | Heap at 1024 |
|---|---|---|---|---|---|
| `pair` | 1.8 to 2.0 Mpx/s | 0.48 to 0.52 | 0.16 to 0.17 | 116 | 37 to 51 KB |
| `none` | | | 0.11 to 0.13 | | |
| `sum` | 1.3 to 1.9 | 0.31 to 0.48 | | 140 | 44 to 47 KB |
| `grid` | 1.2 to 1.8 | | | 114 to 220 | 57 to 87 KB |
| `flat` | 1.4 to 2.0 | | | 112 to 210 | 35 to 66 KB |
| `top4` / `top8` / `top6` | 1.4 to 1.6 / 1.4 to 2.1 / | 0.33 to 0.47 (top6) | | 112 to 154 | 35 to 49 KB |
| `mx` | **0.8 to 1.0** | 0.19 to 0.24 | | 127 to 155 | 45 to 53 KB |
| `mxg` | 0.85 to 1.2 | 0.19 to 0.31 | | 127 to 155 | 39 to 51 KB |
| `focus` | 1.1 to 1.35 | 0.27 to 0.36 | | 115 to 199 | 46 to 62 KB |

The zone-coloured spectrum (below) has the same pixels as the red one and **62 more objects** (three rectangles a bar: 178 against 116 for `pair`; 55 KB).

**The added work of the new parts** (one step of 50 ms, this computer; the P4 is 20 to 40 times slower at transcendental floats, which still leaves them far under 0.1% of the step):
the surround sum of 16 outputs into 6 channels 0.63 us, the stereo downmix 0.80 us, the top-N selection 0.2 us, the colours of 16 squares 0.26 us. **They read the per-output peaks that exist already, so the audio path
stays the one tap**; with `focus` that tap is one output's instead of the sum, which is cheaper. Round 1's other costs stand: the analysis 101 kflops a step, the LVGL work on the P4 unmeasured,
`LV_INV_BUF_SIZE` 256, the 16 KB ring.

### The spectrum's colour

Two stills put the owner's open question side by side: the accent red (every other picture) against **green, amber and red zones in the bars** (green under -30 dB, amber to -15, red above, the colours of the meters): `1024x600_5-1_pair-z` and
`480x320_5-1_sum-z` (and `overview-1024x600-c-focus-and-zone-colours.png`, `overview-480x320-b-pair-focus-zone-colours.png`). The range (-60 to 0 dB) and the rate (20 or 10 updates a second; 10 halves every figure above) are as drawn; both are still the owner's.

### Recommendation (round 2)

**The rule above**: `pair` on the small screen, `sum` on the middle one, `grid` on the large one (`pair` in a 2.0 room everywhere), the spectrum always on top, and **`focus` on a tap** at M and L. Keep **`mx`** as the second choice
for a room of 12 or more at L and M (it is half the pixels), and **`top4`/`top8` only if a screen is ever too small for the six masters**: its labels move and its rule has to be explained. The price is round 1's: **1.2 to 2.0 Mpx/s at L**, LVGL's CPU on the P4 (the thing to measure first on the kit),
`LV_INV_BUF_SIZE` 256.

## What the references show

- **ADI-2/4 Pro SE:** a title at the top (*Main Output 1/2*); a **spectrum analyser** of about 31 third-octave bars in red, with
  a dB scale at the left (0 to -60 or -20 to -80) and thin **peak-hold ticks** above the bars; **two rows of frequency
  labels** under it (every fourth band on each row); under that **horizontal L and R level bars** with a peak-hold tick and the
  **numeric peak** at the right (-13.7, -1.7 dB); a **status line** at the bottom (the channel pair, the level, AES, the
  sample rate).
- **Fireface UCX II:** rows of **bar-graph level meters** one a channel, with peak hold, **colour zones** (green, yellow, red),
  and small **status chips** (sample rate, sync).

The feel the owner wants is a professional meter: a dB scale, fast attack and slow release, peak holds that hang and fall, a
number for the peak, and nothing that is decoration.

## The three variants

| | What it is | Reads from |
|---|---|---|
| **A** | The full instrument: a third-octave spectrum of the main mix (31 bars, two label rows, a dB scale, peak ticks), L and R meters with peak-hold ticks and the numeric peak, a status line (codec, layout, rate, bitrate, the held peak). | one analysis of a mono mix; the L and R outputs' peaks |
| **B** | The spectrum smaller on top, and under it **a bar meter for every output of the room** (green to amber at -18 dB, to red at -6), each with its name, a peak-hold tick and its numeric peak; status chips at the top right. Fireface-style. For 2, 6, 12 or 16 outputs. | one analysis; every output's peak |
| **C** | **A spectrum for each group of speakers** (front with the LFE, surround, height), each with its own peak meter: three small instruments in a 7.1.4 or 9.1.6 room, two in a 5.1 room, one (that is A) in a 2.0 room. | one analysis a group |

How they scale: at **1024 x 600** (a stage of 1024 x 472) the spectrum has 31 bars and two label rows; at **480 x 320** (480 x 216) 15 bars of
two bands; at **240 x 240** (240 x 144) 10 bars of an octave, one label row, and **B and C are A** (no room for a row of outputs). Boards with a bigger or
smaller panel take the nearest of the three by the width.

## Where the spectrum comes from

The board is the one decoding, so it already holds the samples; the work is to make a spectrum from them for nothing on the audio path.

1. **On the audio path, one mono mix.** In `BurstPlayer::meter()` (`firmware/esp-idf/iclforge/src/burst_player.cpp`), which already
   walks every live output's float block once to find its peak and its mean square, the same loop adds each sample into one
   accumulator, and the block's sum is stored as 16-bit integers into **one ring of 8,192 samples at 48 kHz** (171 ms, 16 KB). The
   sum is shifted down by half of log2 of the outputs, rounded up (an even divide by 2 for stereo, by 4 for 5.1 to 9.1.6), so that a
   room does not clip it. **That is all the audio path does**, and only while the Meters view is on the glass and a stream
   plays (a flag the screen sets; a board without a screen never sets it and the code is not in its image).
2. **On the panel side, every 50 ms, a two-rate analysis** (`iclforge/dsp/spectrum.hpp`), the family's own FFT
   (`iclforge/dsp/detail/fft_stockham.hpp`, the passes AC-4 and the MDCT run on the P4) in `float`:
    - for the **17 bands from 20 Hz to 800 Hz**, the last 171 ms with four samples summed to one (a 12 kHz stream) and a 2,048-point real FFT: 5.9 Hz a bin;
    - for the **14 bands from 1 kHz**, the last 21 ms at 48 kHz and a 1,024-point real FFT: 47 Hz a bin.
   A real frame of 2 N samples is a complex transform of N points, unpacked. The band's level is the power of the bins that fall in it
   (an edge bin shared by the overlap), in dB below a full-scale sine's power. The time average is in the power domain (80 ms), then the
   bars have **fast attack and a slow fall** (30 dB a second), the peak ticks **hold a second and fall 15 dB a second**.
3. **The meters** (A's L and R, B's every output, C's group peaks) take each output's **peak over 50 ms**, and **fall 24 dB a second**, with a
   tick held 1.5 s that falls 10 dB a second, and the number is the held peak.

**What is integer and what is float.** The mix is summed in `float` with the meter's own loop and stored as `int16`: one conversion a sample of the mix, not
of each output. Everything on the panel side is `float32` (the window, the transforms, the band sums, the ballistics); the P4's FPU is single precision and the
AC-4 decoder already runs there in `float`. Nothing is `double`.

**How exact it is** (`meters_tune`, over a made-up groove in 2.0, 5.1 and 7.1.4):

| Held to | Result |
|---|---|
| a full-scale sine, 63 Hz to 16 kHz | reads 0.0 dB in its own band (-0.3 at 63 Hz, -0.1 at 800 Hz); 25 Hz reads -2.9 because the band is one bin and a half wide |
| a sine at -20 dBFS | reads -20.0 in the band, the same at every frequency tried |
| one 8192-point FFT of the same ring (the "textbook" analysis, 5.9 Hz a bin at every band) | **identical below 800 Hz** (mean difference 0.01 dB over 600 windows, worst 0.97 dB); above 1 kHz the two-rate analysis sees only the last 21 ms where the long one integrates 171 ms, so on a groove they differ by 2 to 3 dB on average (after the 80 to 100 ms average) and by more on a hit: **time resolution, not an error**, and the livelier of the two; it is what a fast analyser shows |
| white noise | the same level per Hz should rise 1.0 dB a band (a band is 2^(1/3) wider than the one below): averaged over 400 independent frames it does, **+0.86 to +1.18 a band, within 0.1 dB of the long FFT at every band tried and across the crossover** (800 to 1,000 Hz: +1.01 against +1.03). A *single* 21 ms frame scatters by about 3 dB from band to band (few degrees of freedom), which the 80 ms power average takes out; a 2,048-point high frame is the lever if it is not enough (+33% operations) |

## What it costs

Measured on this computer unless it says estimate. **No part of this has been run on a board.**

**Pixels LVGL draws** (`meters_options measure`, a minute of each, the exact part of what a view costs; 20 updates a second; with `LV_INV_BUF_SIZE` 256):

| Screen | A | B | C | The speakers view for comparison |
|---|---|---|---|---|
| 1024 x 600 | 1.7 to 1.9 Mpx/s | 1.6 to 2.2 | 1.3 to 1.9 | 0.02 to 0.17 |
| 480 x 320 | 0.47 to 0.52 | 0.34 to 0.45 | 0.32 to 0.52 | |
| 240 x 240 | 0.16 to 0.17 | the same (it is A) | the same | |

- **LVGL's default `LV_INV_BUF_SIZE` of 32 is not enough.** A refresh in which more than 32 separate rectangles are invalid redraws the whole screen, and a
  spectrum changes 31 bars and 31 ticks at once: with 32 the 1024 x 600 views flush **11.5 to 12.3 million pixels a second**
  (ten whole screens a second; `measure-inv32.csv`). 128 brings A to 1.7 to 1.9 but B and C in a 7.1.4 or 9.1.6 room to 3.0 to 4.1; **256** brings
  every one to the figures above. It is a compile definition of LVGL (a display's table of invalid rectangles: 16 bytes each, about 4 KB at 256) and the
  speakers view does not need it (see [p4x-speakers-options.md](p4x-speakers-options.md)); the Meters view does.
- The rate is halved by updating at **10 Hz** (the meters then move in 100 ms steps, which is today's window), and cut a quarter by 15 Hz.
- **LVGL's own work** (update and render of a step, on this computer): A 0.28 ms and 0.7 ms; B and C in a 9.1.6 room about 1.0 ms and 2.0 to 2.4 ms (the
  per-output bars are 3 rectangles, a tick and two labels each). **The P4 is slower by a factor that is not measured**: at 5 to 8 times, A is 5 to 8 ms of
  every 50, **10 to 16% of the core the panel task runs on**, and B or C at 16 outputs two to three times that. That is the number that decides whether a
  multichannel Meters view can run next to AC-4 5.1, and it is the first thing for the kit.
- **Objects and heap:** 116 LVGL objects (A at 1024), up to 186 (B at 9.1.6); the heap of the views 37 to 60 KB at 1024 x 600 (the fonts' glyph
  caches are in it), 17 to 25 KB at 240 x 240.

**The analysis** (one pass, every 50 ms):

| | Operations | This computer | The P4 (estimate) |
|---|---|---|---|
| two-rate, 1024-point high frame (what A and B run) | **101 k flops** | 15 us | 0.7 to 1.9 ms: 101 k flops at 0.15 to 0.4 flops a cycle at 360 MHz, **1.4 to 3.7% of a core at 20 Hz** |
| two-rate, 2048-point high frame | 134 k | 23 us | 0.9 to 2.5 ms |
| one 8192-point FFT | 287 k | 52 us | 2 to 5 ms; 3.4 times the two-rate |
| C: a group each | 101 k a group: 2 or 3 groups, so 202 to 303 k | | 4 to 11% of a core |

The flop counts are the passes' (a radix-4 butterfly 34, a radix-2 one 10) and the unpacking, window and power; the rate of flops a cycle is a stated
assumption for an in-order single-issue core with an FPU, not a measurement: the kit measures it.

**The audio path (estimate; the kit A/B measures it).** The tap adds one load-and-add to the loop that already measures each live output (72 us an output a burst on the kit
today, 1,536 frames): about **5 to 15 us an output a burst**, and one scale-and-convert of the mix a sample (about 25 to 40 us a burst); **0.1 to 0.5 ms a burst
with 16 live outputs**. The stereo-PCM fast path (`meter_from_source()`, which reads no floats) is not used while the Meters view is up, which costs two outputs at
the float meter's price (about 0.15 ms a burst). **Off the Meters view: nothing**, a flag read once a block. A board without a screen pays nothing at all: no code, no ring.

**Memory:** the ring 16 KB; the analysis's working set about 24 KB (the frames, the real and imaginary parts, the transforms' scratch: stack or PSRAM), its tables about
30 KB (windows, twiddles, the unpacking factors; the transforms' own can be constants in flash); the ballistics a few hundred bytes. In the P4's PSRAM the working set
is mostly in the cache after the first pass; internal RAM is not needed.

**Data the view needs that does not exist today:** per-output **peak over 50 ms**. Today's levels are over 100 ms (`kLevelWindow` 4800 in `burst_player.cpp`) and the
speakers view's tuning follows that serial, so the window cannot simply be halved; the tap's loop would keep a second, shorter peak a block for the Meters view only
(`BurstPlayerLevels` grows an array and a serial), at no cost per sample beyond the compare it already does.

## What the pick would build

- `View::kMeters = 5` in `libs/device/include/iclforge/now_view.hpp` (`kDark` is 4; never renumbered). A board that stores 5 and runs an older image keeps its default and says so.
- The registry's *Screen without lyrics* gains a **Meters** choice (`libs/device/include/iclforge/preferences.hpp`, `kNowViewChoices`) with an availability: a screen of at least
  240 x 240 **and a Sendspin player** (`caps.sendspin`), greyed with that reason elsewhere. *Speakers style* is inert while Meters is shown.
- The route's word list, **in four places** that must agree: `firmware/esp-idf/iclforge/src/control.cpp` (the `GET /api` text and the 400 text) and two copies in
  `apps/demos/wasm/tests/device-ui/stub.js` (the same two sentences), plus the docs row of the setting (`docs/hearth/sink-esp32-p4.md`) and the firmware READMEs.
- The player: the tap, the flag, a 50 ms peak (`BurstPlayerLevels`), `sendspin_now_levels()` and `read_now_levels()` carrying them; the panel: a `meters_screen.cpp` beside `now_screen.cpp`
  (not `lyric_screen.cpp`), the analysis in `libs/render` or `libs/dsp` as a header, host-tested (the calibrations above become tests), the preview scenarios and the pictures.
- **`LV_INV_BUF_SIZE` 256** in the panel build.
- A touch on the Meters view clears the peak holds (an ADI-2 does the same); the dim and dark rules of the no-lyrics screen apply to it unchanged.

## Questions for the owner

1. **Which?** The recommendation is below; B's bars are what the speakers view cannot give (how loud each output is, the exact number), and A is the picture on the ADI-2.
2. **The spectrum's colour:** the accent red as drawn (the ADI-2), or green, amber and red zones in it as the Fireface's bars have.
3. **The range:** -60 to 0 dB as drawn; the ADI-2 also offers -80 to -20, which could be a setting.
4. **The rate:** 20 updates a second as drawn, or 10 (half the pixels and half the LVGL work, steppier meters).

## Recommendation of round 1 (superseded by the rule above)

**One Meters view that picks its own layout: A when the room has two outputs, B when it has more**, and A alone at 240 x 240. It is the ADI-2's picture for a stereo stream and the Fireface's
for a room, from **one analysis** (1.4 to 3.7% of a core, estimated), one 16 KB ring and a tap that costs the audio path a few percent of what its meter already does. **Not C:**
it triples the analysis, three small spectra of a made-up room say little the main spectrum does not, and it is not in either reference. The price to accept is the pixels
(1.6 to 2.2 million a second on the largest screen, ten to forty times the speakers view) and LVGL's CPU on the P4, which is unmeasured: if the kit puts the panel task over
what AC-4 5.1 can spare, the levers in order are 10 updates a second, fewer bars (the 15 of the 480 screen), and drawing each spectrum as one custom-drawn widget in place of 62 objects.
