# The Meters view: an instrument display for the no-lyrics screen

!!! note "Status as of 2026-10-26: decided, built, and run once on the kit: the audio is fine and the drawing is too slow"
    **Decided by the owner, 2026-10-26:** the adaptive rule (the pair on a small screen or in a 2.0 room, the surround sum on a middle one, every output
    in groups on a large one; a tap on a channel to focus it is not built yet); the spectrum in green, amber and red zones; a range of -60 to 0 dB (no
    -80 to -20 setting); 20 updates a second if the panel's task can keep up and 10 if not; and *measuring and characterising the performance is
    definitely required*. **Built** in five PRs: the analysis and the rules (#1682), the player's tap (#1683), the screen, the choice and the route
    (#1685), `GET /panel-stats` with the rate governor and the kit protocol (#1686), and the preview's checks at every size and room with the pictures
    (5 of 5). **The first kit run (2026-10-26) is in [Kit results](#kit-results-2026-10-26):** the stream is whole in every row and the Meters view costs the decode
    task 0.4 to 0.8 ms a burst, but an update costs 130 to 440 ms on music (against a 50 ms step), so the view falls to 10 updates a second and still does not keep up;
    the estimates of [What it costs](#what-it-costs) for the drawing were 40 to 140 times too low. [Measuring it](#measuring-it) and the [kit protocol](#kit-protocol) are what measured it.

    The page below is the study that led to it, as it was written. The pictures of the built view are in `docs/hearth/sink-esp32-p4.md`.

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

Measured on this computer unless it says estimate. **The kit's own numbers are under [Kit results](#kit-results-2026-10-26); where they differ from an estimate below, they are right.**

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

## What the pick would build (as built, 2026-10-26)

- `View::kMeters = 5` in `libs/device/include/iclforge/now_view.hpp` (`kDark` is 4; never renumbered). A board that stores 5 and runs an older image keeps its default and says so.
- The registry's *Screen without lyrics* gains a **Meters** choice (`libs/device/include/iclforge/preferences.hpp`, `kNowViewChoices`) with an availability: a screen of at least
  240 x 240 **and a Sendspin player** (`caps.sendspin`), greyed with that reason elsewhere. *Speakers style* is inert while Meters is shown.
- The route's word list, **in four places** that must agree: `firmware/esp-idf/iclforge/src/control.cpp` (the `GET /api` text and the 400 text) and two copies in
  `apps/demos/wasm/tests/device-ui/stub.js` (the same two sentences), plus the docs row of the setting (`docs/hearth/sink-esp32-p4.md`) and the firmware READMEs.
- The player: the tap, the flag, a 50 ms peak (`BurstPlayerLevels`), `sendspin_now_levels()` and `read_now_levels()` carrying them; the panel: a `meters_screen.cpp` beside `now_screen.cpp`
  (not `lyric_screen.cpp`), the analysis in `libs/render` or `libs/dsp` as a header, host-tested (the calibrations above become tests), the preview scenarios and the pictures.
- **`LV_INV_BUF_SIZE` 256** in the panel build.
- A touch on the Meters view clears the peak holds (an ADI-2 does the same); the dim and dark rules of the no-lyrics screen apply to it unchanged.

## Measuring it

The owner's requirement (2026-10-26): *measuring and characterising the performance is definitely required*. So the view is built to be measured on
the board over port 80 (the capture port is off in a release image), and its rate is a decision the board takes from what it measures.

### What the board reports: `GET /panel-stats`

JSON, on a board with a screen (`404 this board has no screen` on one without, `404 the screen is not running` before the panel is up). The
shape and the text are `libs/device/include/iclforge/panel_stats.hpp`, held by a host test and by the device-ui contract spec, which reads the
header and holds the stand-in's keys to its order. Durations are microseconds. A `timing` object is `{"last", "mean", "max", "n"}`: the last, the
mean since the count began, the largest, and how many.

| Object and key | What it is |
|---|---|
| `panel.width`, `panel.height`, `panel.refreshes` | the glass, and how many times LVGL has drawn since the panel started |
| `panel.refresh_us` | every LVGL refresh, whichever view: from the refresh's start to its end, **the flush to the panel included** (so it holds a wait for the display as well as work) |
| `panel.task_stack_free_min` | the panel task's stack never had fewer bytes than this free (16,384 bytes; 8,968 free on the kit before the Meters view) |
| `panel.psram_free`, `psram_free_min`, `psram_largest` | the PSRAM, which is also LVGL's heap and the panel task's stack: now, the least since the board started, and the largest block |
| `panel.internal_free`, `internal_free_min`, `internal_largest` | the same for internal RAM, which the decoder and the network share |
| `meters.showing`, `representation`, `outputs`, `width`, `height` | the Meters view is up now; `pair`, `sum` or `grid`; the outputs of the room; the stage it was built for |
| `meters.opens`, `open_ms` | times it has been opened since boot, and how long this opening has lasted. **Everything below is since the opening** (and is the last opening's after it is closed) |
| `meters.rate_hz`, `mean_cost_us` | the rate in force (20 or 10), and what an update costs on average over the governor's window: analysis + widgets + the refreshes since the last update (none if no pixel moved: a steady tone is a still picture) |
| `meters.updates`, `late`, `dropped`, `falls`, `rises` | updates run; those that started more than 15 ms later than the panel's loop could have started them (one 30 fps frame is allowed for); whole steps that went by with none; twenty-to-ten falls and rises |
| `meters.analysis_us` | the tap read, the transform, the power average and the ballistics |
| `meters.widgets_us` | setting the bars, ticks and labels that moved (LVGL only invalidates; nothing is drawn yet) |
| `meters.render_us` | LVGL's refreshes while the view shows |
| `meters.tap.blocks`, `us_per_burst`, `us_max_block`, `us_pass_per_burst`, `us_pack_per_burst`, `us_per_clock_read`, `reads`, `torn`, `short_reads` | what the audio path pays: the player's block tap, in microseconds for a burst of 1,536 frames (six blocks), on its decode task; the same split into the pass over the outputs and the pack into the ring (every sixteenth block is timed in parts), and one clock read; reads the screen made and the ones the writer overran or that came before enough was written |

The developer view (three taps on the footer's name) shows the same figures in its PERFORMANCE section once the Meters view has been opened: the
rate and shape (`20 Hz · grid 12 · showing`), the three means (`analysis`, `widgets`, `render`), and `late`, `dropped` and the tap's cost.
`GET /status` carries the stream's own counters (`sendspin.underruns`, `late`, `dropped`, `resyncs`, `burst_us`, `worst_burst_us`, `error_us`,
`decode_stack_free`), which are what the audio path's side of every comparison is read from.

### The rate: 20 updates a second, 10 when the panel task cannot keep up

The owner's rule: 20 Hz if possible, else 10 Hz. `iclforge/meters_governor.hpp` (host-tested) is "if possible":

- **Cannot keep up** is judged over the last 20 updates (a second at 20 Hz), never on one: the **average cost** of an update
  (analysis + widgets + the refreshes since the last update) over **70 % of a 50 ms step (35 ms)**, **or 4 of the 20** started more than **15 ms later than the loop
  could have started them**, or a whole step missed. The allowance for lateness is one beat of the panel's loop: a frame (33 ms at 30 fps) at twenty a second, because a 30 fps
  loop serves a 50 ms step up to 33 ms after it is due and that is nobody's fault, and the resting 100 ms at ten, where an update can be due any time in the beat before
  the loop looks (the first kit run of the canvas drawing counted half of the updates at ten a second late against a 33 ms allowance).
- **The first two seconds of a view are not judged** (they are counted: `updates`, `late`, `dropped` include them; the window does not). The kit's first run
  showed why: the opening's first draw of the stage (110 ms) and the first run of the code from the flash, cold in the cache, missed ten steps in the first second,
  the view fell to ten, and could not rise because it never saw a cost under 45 %. Waking from dimming starts the grace again.
- **It rises again** only after **10 s** at ten, with the average cost **under 60 % of a fast step (30 ms)** and not one late or missed update in
  the window (the canvas drawing's steady cost on the kit is 26 to 31 ms in a 5.1 room and a 9.1.6 one, so 45 % was a limit it could never come back under). The gap between 70 and 60 % and the 10 s are the hysteresis; a fall within a minute of a rise doubles the 10 s (to 160 s at most), and
  a minute at twenty a second puts it back.
- At ten a second the panel's loop goes back to its resting beat of 100 ms (`filling` false), which is exactly the step: a view at 10 Hz costs the
  task nothing between updates. At twenty it asks for the panel's frame rate (30 fps) and keeps its own beat inside it (due times 50 ms apart), which
  a 30 fps loop serves at 67, 33, 67 ... ms: twenty a second on average.
- The limits are one struct (`GovernorLimits`): the numbers are the plan's guesses until the kit has measured the real costs, and a measurement
  changes numbers, not code.

### Kit protocol

What a session does with the kit, in order, unattended; about **an hour and a quarter of the kit and three flashes**. The program is
`tools/hearth/panel_measure.py` (`plan`, `run`, `report`, `compare`; stdlib only; tested on the host against a scripted board). **It
flashes nothing, and it puts back every setting it changes** (the screen's view and speakers style, the layout, the time to dim, the night
schedule), also when a row fails; it forgets only its own pairing record.

**Before.** The coordinator books the window: the kit's lock (`P4X`, `python tools/hearth/bench.py lock`) and a heads-up to the screen-family
coordinator (`D:\ac3bld\BOARD-LOCKS.md`, rule 5); a peer that is building or testing near the board is asked to wait. The owner's Music Assistant
holds the kit while it is paired: the test server displaces it for each play and it reconnects afterwards. Nothing else plays on the kit.

**Three images**, all `p4-rev3` for the kit (`P4X`, 192.168.1.251, COM7), built with the bench's wrapper and put on it by OTA (the kit takes OTA, and a
restart keeps its C6 link):

| Image | What | Build |
|---|---|---|
| **I0** "as it was" | the speakers view before the Glow and Pulse styles: `main` at `eadb09da8`, the commit before #1642 | `python tools/hearth/bench_flash.py build --tag mv-i0 --worktree <a checkout of eadb09da8> --wifi p4-rev3` |
| **I1** "now" | `main` with the Meters PRs: the speakers view (Glow by default, Pulse by `PUT /speakers-style`) and the Meters view | `python tools/hearth/bench_flash.py build --tag mv-i1 --wifi p4-rev3` |
| **I2** "AC-4" | I1 with `sdkconfig.ac4`: AC-4 is a build option on the P4 and the locked profile has none, so this is a bench build and is **not held to the profile**. Built and proved on the host side on 2026-10-26: **3,799,936 bytes** in the 4 MB app partition (395,392 free) | the recipe below |

**I2's recipe** (the bench's build wrapper takes a locked profile and its named overlays, and AC-4 is neither): the profile's own
`SDKCONFIG_DEFAULTS`, then `sdkconfig.ac4`, then the bench's Wi-Fi fragment, in a build directory of its own, from a checkout of the commit under test:

```
set IDF_TOOLS_PATH=D:\esp\tools && D:\esp\esp-idf\export.bat
python tools/hearth/sink_profile.py defaults p4-rev3
:: prints sdkconfig.defaults;sdkconfig.hw;sdkconfig.p4;sdkconfig.p4-rev3;sdkconfig.sendspin;sdkconfig.panel;sdkconfig.panel-dsi
idf.py -C firmware/hearth-sink -B D:\esp\build\iclforge\mv-i2-p4-rev3 -DSDKCONFIG=D:\esp\build\iclforge\mv-i2-p4-rev3\sdkconfig ^
  -DIDF_TARGET=esp32p4 "-DSDKCONFIG_DEFAULTS=<each file above as firmware/hearth-sink/<file>>;firmware/hearth-sink/sdkconfig.ac4;D:/esp/build/ac3forge/wifi_credentials.sdkconfig" build
```

Delete the build directory's `sdkconfig` first if it exists (an old one overrides a changed default). The image is not a profile's, so `sink_profile.py verify`
does not apply to it and `bench_flash.py ota` is given its `--build-dir`.

Put an image on with `python tools/hearth/bench_flash.py ota --build-dir D:\esp\build\iclforge\mv-i1-p4-rev3 --session <id> P4X`, then confirm with
`bench_flash.py verify`. Record each image's commit and size in the report's label.

**The rows** (the stream is sent over Sendspin by `hearth-testserver`, the way the product plays and the only path the tap listens to):

| Row | Stream | Room | The screen draws | Image |
|---|---|---|---|---|
| `pcm-2.0` | two tones, 48 kHz stereo PCM (`--pcm --tone`) | 2.0 | `pair` | I1 (I0) |
| `eac3-5.1` | `testdata/external-baseline/eac3-51-256/ffmpeg.ec3`, looped | 5.1 | `grid` of 6 | I1 (I0) |
| `eac3-5.1-on-9.1.6` | the same on a 16-slot room (`PUT /layout 9.1.6`) | 9.1.6 | `grid` of 16: the heaviest | I1 (I0) |
| `joc-7.1.4` | an E-AC-3 JOC (Atmos) file given with `--atmos` (`D:\ac3bld\atmos768.ec3`, 768 kb/s) | 7.1.4 | `grid` of 12 | I1 (I0) |
| `ac4-5.1` | `testdata/external-baseline/ac4-51-music-192/dee.ac4`, looped | 5.1 | `grid` of 6 | I2 |

The kit's glass is 1024 by 600, so **only `pair` (a 2.0 room) and `grid` can be measured on it; `sum` (400 to 799 px) and the small `pair` need a
smaller screen, and are bounded by the grid** (six bars against twelve or sixteen, on a stage a fifth the size). The preview's host figures
(`What it costs`) say how much cheaper they are; the kit cannot say it. A 7.1.4 stream of a form the board's decoder does not carry (AC-4 7.1.4,
`ac4-514-tones-768`) is a second AC-4 row to add if the first passes.

**The schedule of one row** (`panel_measure.py plan` prints it; every segment 60 s with the first 10 s not counted): *settle* 10 s on the speakers
view; **`speakers-glow`**; **`meters-a`**; **`speakers-pulse`**; **`meters-b`** (the Meters view a second time is the repeatability);
then 20 cycles of the Meters view 3 s and the speakers view 3 s (the **leak** test, with the stream playing). 390 s a row with a margin, about 33
minutes for the five. I0 is run with `--views speakers` (no Meters view, no cycles) over the first four rows, 150 s each.

**Commands.** Take the lock, put I0 on, run; put I1 on, run; put I2 on, run:

```
python tools/hearth/panel_measure.py run --board P4X --session <id> --label "I0 eadb09da8" --views speakers --rows pcm-2.0 eac3-5.1 eac3-5.1-on-9.1.6 joc-7.1.4 --atmos D:\ac3bld\atmos768.ec3 --out D:\ac3bld\panel-measure\<date>-i0
python tools/hearth/panel_measure.py run --board P4X --session <id> --label "I1 <sha>" --rows pcm-2.0 eac3-5.1 eac3-5.1-on-9.1.6 joc-7.1.4 --atmos D:\ac3bld\atmos768.ec3 --out D:\ac3bld\panel-measure\<date>-i1
python tools/hearth/panel_measure.py run --board P4X --session <id> --label "I2 <sha> ac4" --rows ac4-5.1 --out D:\ac3bld\panel-measure\<date>-i2
python tools/hearth/panel_measure.py compare D:\ac3bld\panel-measure\<date>-i0 D:\ac3bld\panel-measure\<date>-i1
```

Each run writes `<row>.samples.json` (a sample a second: the stream's counters and the whole of `GET /panel-stats`), `results.json` and
`report.md`. The exit status is 1 if any criterion failed or a row could not be measured.

**What is measured and the pass criteria.** These are fixed here, before a measurement, and are one number each in `panel_measure.py`:

| Criterion | Pass if |
|---|---|
| **stream** | no underrun, late or dropped burst, resync or invalid burst in **any** segment, whichever view shows |
| **burst** | the Meters view's mean `burst_us` is at most the speakers view's (glow) plus the larger of 15 % and 250 us |
| **tap** | the tap costs the audio path at most 150 us a burst up to 8 outputs, 500 us above; and not one history read torn |
| **rate** | **20 Hz if possible, else a steady 10 Hz** (the owner, 2026-10-26), and the report says which cell holds which. A cell holds 20 Hz if it ran at 20 with no fall. It holds a steady 10 Hz if it fell (or could not hold 20), no more than 1 % of its steps went by with no update, and an update costs at most half of the 100 ms step. Anything else fails: the glass would show a few updates a second |
| **cost** | an update's mean cost (analysis + widgets + the refreshes since the last update) is at most 50 % of the step in force (25 ms at 20 Hz, 50 ms at 10 Hz) |
| **dropped** | at most 1 % of the steps went by with no update (the same 1 % as a steady 10 Hz) |
| **stack** | the panel task's stack had at least 4 KiB free at its least |
| **internal** | the internal heap's low point under the Meters view is within 8 KiB of the speakers view's |
| **leak** | PSRAM free after the 20th return to the speakers view is within 16 KiB of its value after the first |
| **sync** | the largest sync error (`error_us`) under the Meters view is within 1 ms of the speakers view's |
| **as it was** (`compare`) | the glow's mean burst against I0's speakers view is within 5 % or 100 us; the pulse is reported |

The *as it was* difference is **not only the speakers view's style**. I1 also has the Meters tap in the player (one pointer load per block while no view
wants it) and the responsive-levels read the speakers view makes of the player (`read_now_levels`), and an earlier window measured Glow against Pulse within
1 %, so when a burst differs between I0 and I1 the style is not where to look first. The *stream* criterion's first-opening finding below is a second thing to
repeat: the first, cold opening of the Meters view against the second (`meters-a` against `meters-b`), which the report now names when every fault is in the first.

**After.** Collect the three `report.md`s and the comparison into the plan page as **Kit results** (the date, the commits, the tables), replace the
estimates under *What it costs* by the measured costs and say which numbers moved, and set the governor's limits (`GovernorLimits`) to what the kit
shows (the 70 % and 60 % thresholds and the 15 ms allowance are the plan's guesses). A failed criterion is a finding with its samples attached, not a
reason to loosen a number; the levers, in the order the plan gives them, are 10 updates a second (already automatic), the 15 bars of the 480-px
screen on the large one, and one custom-drawn widget for the spectrum in place of 62 objects.

**What the kit replaces.** The P4's cost for the analysis (101 kflops at 0.15 to 0.4 flops a cycle: 0.7 to 1.9 ms, 1.4 to 3.7 % of a core) and for
LVGL (5 to 8 times the host's) are assumptions; `analysis_us` and `widgets_us` and `render_us` are those numbers measured, and **the first thing to
read** is `meters.analysis_us.mean` in the `eac3-5.1-on-9.1.6` row.

### Kit results, 2026-10-26

The first run of the protocol, on the kit (`P4X`, 1024 by 600, 400 MHz, over its Ethernet address), booked 18:07 to 19:03 local. Images: **I0** `eadb09da8` (main before #1642, the speakers view as it was), **I2** `e4a61acea` with `sdkconfig.ac4`, **I1** `e4a61acea` (main `d1ff66ee2` and the five Meters PRs), release-like `p4-rev3`. Each row is 390 s (60 s segments, the first 10 s of each not counted, then 20 open and close cycles); the numbers are the segments' means, in microseconds unless a unit says otherwise. The raw samples, the reports and the settings found and put back are in the session's notes folder (`meters-kit-window-2026-10-26`).

**The view as built does not keep up on music at this size: it is 2.6 to 8.7 times over its 50 ms step and falls to 10 updates a second, where it is still 1.3 to 4.4 times over that step. It does not hurt the audio.**

| Row (room, what the screen draws) | Burst, speakers view | Burst, Meters view | Tap, a burst | Analysis, mean (max) | Widgets, mean (max) | Render, mean (max) | An update costs | Rate held |
|---|---|---|---|---|---|---|---|---|
| stereo PCM, two tones (2.0, `pair`) | 272 | 320 to 338 | 368 | 5,000 (18,000) | 160 (23,000) | build only | 5,400 to 5,800 | **20 Hz** |
| E-AC-3 5.1 (5.1, `grid` of 6) | 4,055 | 4,445 to 4,588 | 679 | 5,400 (17,000) | 43,600 (69,000) | 81,000 (229,000) | 129,000 to 131,000 | 10 Hz, 1 fall |
| E-AC-3 5.1 on a 9.1.6 room (`grid` of 16) | 4,646 | 5,072 to 5,234 | 692 | 5,300 (10,000) | 59,400 (79,000) | 105,000 (366,000) | 165,000 to 173,000 | 10 Hz, 1 fall |
| E-AC-3 JOC, 7.1.4 (`grid` of 12) | 21,751 | 22,357 to 22,577 | 858 | 9,600 (24,000) | 137,000 (179,000) | 288,000 (598,000) | 432,000 to 437,000 | 10 Hz, 1 fall |
| AC-4 5.1 (5.1, `grid` of 6; image I2) | 25,940 | 26,527 to 26,760 | 691 | 7,900 (28,000) | 76,000 (118,000) | 150,000 (459,000) | 231,000 to 237,000 | 10 Hz, 1 fall |

- **The audio is whole in every row with music** in both views: no underrun, late or dropped burst, resync or invalid burst, sync error unchanged (the largest 85 to 250 us either way). The Meters view costs the decode task **0.4 to 0.8 ms more per burst** (9 to 13 % in the two E-AC-3 rows, 3 % on the JOC and AC-4 rows), inside the allowance. The one finding in the audio: **the first opening of the view on the stereo PCM stream cost one underrun and one resync** (`meters-a`; the second opening, and every other row, had none). It needs repeating before it is a finding about the view and not the first build.
- **The tap costs 370 us a burst for 2 outputs and 680 to 860 us for 6 to 16** (the plan estimated 0.1 to 0.5 ms with 16 live outputs): the criterion (150 us up to 8 outputs, 500 above) failed in every row. It is 1 to 3 % of the 32 ms a burst lasts; the cost is in the player's decode task and is wall time, which an interrupt or another task adds to.
- **The analysis is 3 to 14 times the plan's estimate** (0.7 to 1.9 ms estimated, 5 to 10 ms measured, up to 28 ms at its worst): its tables and scratch are 63 KB in the PSRAM, shared with the decoder.
- **The drawing is what does not keep up.** A music spectrum moves all 31 bars every update; as 3 rectangles and a tick each, with the channels' bars, that is 100 to 200 small invalid areas a refresh, and **LVGL's refresh takes 80 to 290 ms on the P4 where the speakers view's takes 2 to 8 ms** (the same refresh took 1 to 3 ms on the host). The widgets (setting sizes and positions) take 44 to 137 ms: the host's cost is about 1 ms, and the plan's 5 to 8 times that was **44 to 137 times**. The pair under a spectrum that hardly moves (the two tones) costs 5.4 to 5.8 ms and holds 20 Hz: it is the number of bars that move, not the pixels, and the grid's cost rises with its bars (6, 12, 16) as the stream's does.
- **The governor did its job**: every music row fell to 10 Hz once, within a window, and stayed there; but a step is 100 ms, an update costs 130 to 440 ms, and 200 to 560 steps went by without one in each 60 s segment, so what the screen shows is 2 to 8 updates a second.
- **Memory is not the problem**: 20 open and close cycles lost at most 2.4 KB of PSRAM in any row (the JOC row gained 55 KB); the panel task's stack never had fewer than 10,880 bytes free (8,968 before); the internal heap's low point is the same under both views. `GET /panel-stats` and the developer view work on the board and say what is above.
- **The speakers view as it was against as it is** (I0 against I1): the glow's mean burst is within -2.2 to +1.6 % in three rows and +6.6 % (16 us) on the stereo PCM tones, all inside the allowance (5 % or 100 us); the pulse is reported at +2.9 to +29 % (0.6 ms on the heaviest row), and its refresh costs 2.0 to 6.8 ms, as the glow's does.

What this changes: the levers the plan listed, in the order it gave them, are now needed and are not enough in the first form: **10 updates a second is already automatic and is not enough**. The cost is the number of small invalid areas and objects, not the pixels (1.3 to 1.8 million a second), so the next step is **one custom-drawn object for the spectrum and one for each group of bars**, which makes an update two or three invalid areas and a handful of draw calls instead of 100 to 200; then the tap's loop (its second pass over the outputs' floats, which the level meter already reads once) and the analysis' placement (its tables in the PSRAM). The kit protocol runs unchanged on the result.

### The rework: one canvas a chart (2026-10-26)

What the speakers view's drawing does that the Meters view's did not: it has **a few objects** (at most 16 circles), changes an opacity only **in quantised steps and
only when a step changes**, runs at about 10 Hz and **never changes a geometry**, so a refresh is a few small invalid areas. The Meters view had a rectangle for
each part of each bar (three and a tick a bar, 100 to 200 objects), every one with a geometry that moved every update; LVGL then walks the whole object tree once
for each invalid area it flushes and each flush is a DSI transfer of its own (1 to 1.5 ms on the P4 whatever its size).

- **The spectrum plot is one object**, an RGB565 canvas (`lv_canvas`, a draw buffer in the PSRAM), and so are **the channel bars** (every group, one canvas) and **the pair of
  meters** (one canvas). `iclforge/meters_draw.hpp` is the pure drawing (the zones, the peak ticks, the faint line, the troughs) into any 16-bit buffer, host-tested; an update works out
  which rows of a bar changed (`rows_touched`), draws only those and tells LVGL. **The first form invalidated one rectangle**, the plot's width by the rows of the bar that moved most:
  **an update was 4 to 6 invalid areas** (the panel preview counts the flushes an update makes: **58 to 118 before, 4.3 to 5.7 at 1024 by 600**) but 2.5 to 3 million pixels a second, and the
  kit's first run of it (build a37bf4281, `Kit results, the canvas drawing`) measured the render at **21 ms an update in the 5.1 room and 47 ms with a JOC stream decoding: 135 ns a pixel**, the
  PSRAM's bandwidth shared with the decoder (about 12 bytes a pixel: the background, the canvas and the copy to the glass). **The second form plans the rectangles**
  (`plan_cells()`, `libs/device/include/iclforge/meters_draw.hpp`): neighbouring bars that changed share one rectangle (their columns, the union of their rows) when that costs less than one
  each, by the least total of 8,000 pixels an area (the kit's about 1 ms) and one for each pixel, over at most 64 bars by dynamic programming; the preview flushes **0.7 to 1.2 million pixels a
  second in 6 to 7 areas** at 1024 by 600. A test redraws a bar chart by random walk, in the planned cells and from scratch, and holds the two bit for bit.
- **The numbers** under the bars (the dB figures) are text and change at **4 Hz**, not every update.
- **The analysis is cheaper**: the spectrum's power average, the band levels, the downmix and the ballistics work in `float` (the P4 has no double unit: a `pow`, `log10` or `exp`
  in double is software, microseconds each; they are `exp2f`/`log2f` now), and the low half of the bands (the 2048-point transform at 12 kHz) is analysed **every other update**, the high half
  every update; the halves are held equal to the whole by test, and a slowed view (the governor at 10 Hz) analyses both every update.
- **-O2** for `meters_screen.cpp` and `panel.cpp` (the player's tap is header-only and runs in panel.cpp's translation unit); the project's -Os does not inline or unroll.
- **The refresh means are exact** (`panel.refresh_us` was a mean of means).
- **The tap's cost is measured in parts**: every sixteenth block times its pass over the outputs, its pack into the ring and its clock reads (`tap.us_pass_per_burst`, `us_pack_per_burst`,
  `us_per_clock_read`); the kit showed 370 to 860 us a burst for about 45 ns a sample, the same as the level meter's own pass, and the parts say where it goes.
- **Not changed**: the representations, the zone colours, the governor (20 to 10 Hz) and the protocol. A **lighter mode** (fewer bars, a smaller matrix) is deliberately not built: it is added only if the
  reworked view cannot hold a steady 10 Hz on a 9.1.6 room or a JOC 7.1.4 stream on the kit, so that the glass never shows two to eight updates a second.

**Measured on the board twice, below** (the first form of it, one rectangle for a chart, and the planned cells): **every row holds 20 Hz**. No lighter mode was built.

### Kit results, 2026-10-26, second run: the canvas drawing

Booked 20:44 to 21:27 local. Images: **I1c** `eb9677196` (the canvas drawing with `plan_cells`, the governor's grace and its 60 % rise, `p4-rev3`) and **I2c** (the same with `sdkconfig.ac4`),
both put on by OTA, **over the kit's Ethernet address**; the protocol of *Kit protocol* above, five rows, no I0 (the comparison with the speakers view as it was stays that of the first run). The raw samples and
reports are in the session's notes folder (`meters-kit-window-2026-10-26/i1c-canvas-planner-eb9677196`, `i2c-ac4-canvas-planner-eb9677196`; `i1b-canvas-a37bf4281` is the first form of the drawing, over the Wi-Fi address by mistake: its
PCM row has underruns in both views and is not used here). Times are the two Meters segments' means, in milliseconds unless a unit says otherwise.

**Every cell holds 20 updates a second with no fall to ten, in the first (cold) opening of the view and the second. The first run's 130 to 440 ms an update is 11 to 29 ms. The audio is whole.** What still fails is the plan's own tap limit in
every row, and in the JOC row the cost and the steps missed, by a little.

| Row (room, what the screen draws) | Burst, speakers view (us) | Burst, Meters view (us) | Rate held | An update costs (analysis + widgets + render) | Steps with no update | Tap, a burst (pass + pack, us) |
|---|---|---|---|---|---|---|
| stereo PCM, two tones (2.0, `pair`) | 247 | 290 to 305 | **20 Hz** | 1.7 to 2.3 (1.9 + 0.1 + 4 refreshes in 60 s: a still picture) | 0.8 % | 329 (112 + 174) |
| E-AC-3 5.1 (5.1, `grid` of 6) | 4,037 | 4,413 to 4,542 | **20 Hz** | 11.3 to 12.5 (1.8 + 2.2 + 7.4) | 0.3 % | 573 (352 + 210) |
| E-AC-3 5.1 on a 9.1.6 room (`grid` of 16) | 4,620 | 4,995 to 5,125 | **20 Hz** | 11.7 to 12.9 (1.9 + 2.3 + 7.7) | 0.4 % | 578 (356 + 211) |
| E-AC-3 JOC, 7.1.4 (`grid` of 12) | 21,552 | 22,287 to 22,569 | **20 Hz** | 23.8 to 29.3 (3.3 + 4.8 + 16.5) | **1.7 %** | 723 (487 + 217) |
| AC-4 5.1 (5.1, `grid` of 6; image I2c) | 25,937 | 26,495 to 26,688 | **20 Hz** | 14.1 to 16.2 (4.8 + 3.3 + 8.6) | 0.8 % | 584 (356 + 219) |

Against the first run (the drawing as 100 to 200 objects): an update in the 5.1 room 129 to 131 -> 11 to 12; 9.1.6 165 to 173 -> 12 to 13; JOC 432 to 437 -> 24 to 29; AC-4 231 to 237 -> 14 to 16; the widgets 44 to 137 -> 2 to 5; the render
81 to 288 -> 7 to 17 (a refresh 7.4 ms in the 5.1 room, by the preview's count for 38,000 pixels in six areas, where the first form's one rectangle cost 21 ms for 124,000 in four, and 16.5 ms against 47 ms under a JOC stream); the analysis 5.4 to 9.6 -> 1.8 to 4.8.

- **The criteria**: *rate*, *stack*, *internal*, *leak*, *sync* and *burst* pass in every row; *cost* (at most 50 % of the step) and *dropped* (at most 1 %) pass in all but the JOC row (59 % and 1.7 %: 40 steps of 2,300 with no update, 13 and 11 updates late; the cell
  is the closest to the limit because the decoder takes 22 of the 32 ms of a burst and shares the PSRAM, and the view's render goes from 7 ms in the 5.1 room to 16.5; the governor's own limit of 70 % (35 ms) is 6 ms away); *tap* fails everywhere (below).
- **The tap costs the audio path 329 to 723 us a burst, 1 to 2.3 % of one**, against a limit of 150 us up to 8 outputs and 500 above, which was a guess. The sub-timers say where: the pass over the outputs 112 us (2 outputs), 352 (6),
  356 (a 5.1 stream on 16 slots), 487 (12 outputs under JOC), 40 to 60 us an output, and the pack into the ring 170 to 220 us whatever the room; a clock read is 1.1 us. The first run's 370 to 860 us at -Os became 330 to 720 at -O2: the loops are what is left.
  The audio does not mind it: no underrun, late or dropped burst, resync or invalid burst in any music row, the burst grows 2 to 12 % in the music rows (17 to 23 % in the PCM row, 43 to 58 us), and the largest sync error is within 1 ms of the speakers view's in every row.
- **The stereo PCM row's stream is unclean in every segment, the speakers view's too**: 6 underruns and 6 resyncs in `speakers-glow`, before the Meters view had been shown once, then 1, 1 and 3 (and one overrun history read). It is not the view; the
  first run's one underrun at the first opening of the view is the same thing seen once. Not explained: the dev Music Assistant was paired and connected this time and was not in the first run, and the MA/Atmos coordinator restarted that add-on at 20:50, during this row (it ran about 20:46 to 20:52; the six underruns of `speakers-glow` were at about 20:47, before the restart,
  the 1, 1 and 3 after it); the add-on dialled the kit again from about 20:56 and the four rows after it have no stream fault. To repeat with the add-on stopped before it is called a stream problem.
- **The first, cold opening no longer falls to ten.** In the first form it fell in the first second of the 9.1.6 and JOC rows (the first draw of the stage, 110 ms, and the code cold from the flash missed ten steps) and could not rise; the governor does not judge the first
  two seconds, and rises under 60 % of a fast step. A rise is still never seen on the kit, because nothing falls.
- **Memory is not the problem**: 20 openings and closings lost at most 2.1 KB of PSRAM in the 5.1, 9.1.6 and PCM rows (the JOC and AC-4 rows gained 230 and 455 KB, the decoder's own allocation); the panel task's stack never had fewer than 10,812 bytes free; the
  internal heap's low point is the same under both views in each row.
- **The planner's cost model** (8,000 pixels an area) was set from the first form's 21 ms for 124,000 pixels in 4.3 areas (the preview's counts), about 135 ns a pixel and 1 ms an area if the pixel cost held; the second form's 7.4 ms for 38,000 pixels in 5.9 areas says 70 ns a
  pixel and 0.8 ms an area, so a pixel is cheaper than assumed and an area dearer than a tenth of a refresh: the constant is in the right place (the plan is two or three cells of several hundred pixels' width) and a measured one would change a number in `meters_draw.hpp`, not code.

What this leaves: the JOC row's two marginal criteria and the tap's cost are the open items; the ways to move them are in the order of effort the tap's two loops (a few hundred us for the audio path, not the glass), the draw buffers' rows (`ICLFORGE_EXAMPLE_PANEL_LINE_BUFFER_ROWS`, 40:
smaller buffers stay in the cache and the render of every view gets cheaper; not tried), and a lighter mode (fewer bars), which the owner's rule asks for only if a room cannot hold 10 Hz: none of the five rooms comes near it now.

### Kit results, 2026-10-28: the copy into the frame buffer by 2D-DMA

Booked by the owner for the question "does PIE, PPA or the DMA help the drawing without hurting the audio". PIE does not (the P4's is integer-only: s8 to s32, u8 to u32; the v3.x guide's one PIE change is "updated for saturation and rounding"), and the bar fills are about 3 % of an update. What the code showed instead: `display_dsi.cpp` never called `esp_lcd_dpi_panel_enable_dma2d()`, so ESP-IDF 6.1 copied every flushed area into the frame buffer on the CPU (`memcpy` line by line, PSRAM to PSRAM, and a cache sync over whole rows) and told LVGL it was done before returning. `ICLFORGE_EXAMPLE_PANEL_DMA2D_FLUSH` (default on) turns that into a 2D-DMA job; if the DMA will not start, the CPU copy stays and the boot log says so.

Same code both arms (the canvas drawing, `eb9677196` against `8e827333d`), each put on the kit by OTA in one window (over the Ethernet address, the protocol above, 306 s a row, six open and close cycles), the control first. Times are the two Meters segments' means, in milliseconds; the extra burst is the Meters segments' mean burst less the speakers-glow segment's, in microseconds.

| Row | Render, CPU copy -> 2D-DMA | An update (analysis + widgets + render) | Meters view's extra burst, us | Underruns, late, dropped | Internal heap low point, KB |
|---|---|---|---|---|---|
| E-AC-3 5.1 | 7.60 -> 7.10 (-7 %) | 11.2 -> 11.4 | 448 -> 447 | 0 | 86.8 -> 82.3 |
| E-AC-3 5.1 on 9.1.6 | 7.87 -> 7.41 (-6 %) | 11.3 -> 11.5 | 421 -> 408 | 0 | 9.4 -> 9.7 |
| E-AC-3 JOC 7.1.4 | 15.99 -> 14.41 (-10 %) | 23.9 -> 24.7 | 931 -> 873 | 0 | 12.7 -> 12.5 |
| AC-4 5.1 | 8.64 -> 7.70 (-11 %) | 16.5 -> 15.5 | 604 -> 560 | 0 | 14.4 -> 14.9 |

**Correction (2026-10-28, the same day): the "extra burst" column is wrong by a factor of two to four.** The board's `burst_us` is the mean of every burst since the stream began, so the mean of its readings over a minute is a mean of the whole history, and a stretch that costs 1 ms a burst moves it by a quarter of that. Read between two readings of it, the Meters view's extra burst in these four rows is 1,209 -> 1,185 (5.1), 1,182 -> 1,144 (9.1.6), 2,420 -> 2,157 (JOC) and 1,948 -> 2,016 us (AC-4): the 2D-DMA arm is no worse for the audio (the extra changes by -2 %, -3 %, -11 % and +3 %, within the run-to-run noise of about 200 us in the heavy rows; JOC's -263 us is the one change that size), but the extra is what the next section says it is, not 400 to 900 us. Every burst figure above this line is of the running mean, and the qualitative findings stand (the audio is whole in every music row).

- **A real but small gain, and none for the audio to pay.** The render falls 6 to 11 % (0.5 to 1.6 ms an update); an update's own cost moves inside the run-to-run noise (about 0.5 ms); AC-4's speakers-view refresh falls 6.8 -> 6.0 ms and the other rows' stay within noise; the audio burst, the sync error (about 250 us in every row) and the internal heap are unchanged within the same noise. The image is 14.5 KB bigger (2,843,120 B against 2,828,224 on the p4-rev3 profile, 56.9 KB under its 2,900,000 limit); the 5.1 room's internal heap low point is 4.5 KB lower, the others' are not.
- **The picture is right.** `GET :8081/panel.bmp` of a capture build, at two openings of the Meters view in the 9.1.6 room, shows clean bars, axes and labels (no stale or misplaced cell); the idle screen read back twice is identical (0 of 614,400 pixels differ).
- **What a flush costs** (a capture build with a probe, 512 flushes a print): about 4,400 pixels a flush and 6.5 flushes an update; the CPU is in the driver call for 120 to 150 us, and the DMA takes 190 to 250 us from the call to the interrupt. So the copy is 0.85 ms of CPU an update, and what is left of the 7 ms render, about 6.5 ms, is LVGL's own drawing of the area (the background and the canvas blit) into PSRAM buffers. That part doubles under the JOC decoder for much the same pixels (14 to 16 ms against 7), which points at a bus the decoder and the scan-out (104 MB/s) share, not at instructions.
- **Render buffers in internal RAM do not pass the audio gate where it matters.** Two buffers of 8 rows (32 KB) in internal RAM: the 5.1 room, which has 80 KB spare, renders in 6.6 ms (7 % under the 2D-DMA arm, 13 % under the CPU copy); in the 9.1.6 room and under JOC, which have 9 and 13 KB spare at their low points, the harness's HTTP connection to the board was reset at the first request of the first row and again in the JOC row after three segments, so neither row produced a verdict (the cause was not read from the board's log). Not taken. The probe and this variant are in `2026-10-28-experiments-probe-and-sram.patch` beside the raw runs.
- **What is left, in the order of effort:** LVGL's direct mode into the frame buffer (no render buffers, no flush, no copy: 160 KB of PSRAM, 0.85 ms of CPU and the copy's bus traffic an update; tearing as today); fewer, larger areas (the planner's 8,000-pixel cost of an area was set against the CPU copy and wants re-measuring); LVGL's PPA backend (off in this build; its draw-buffer handler syncs the whole buffer's cache, `lv_draw_ppa_buf.c`, which for a chart's canvas is hundreds of KB, so look at that first). PIE is not on the list.

### What the screen costs the audio path (2026-10-28, owner: "free cycles for decoding")

**The measurement, corrected.** `GET /status`'s `burst_us` is the mean of every burst since the stream began (`decode_us_total / timed_bursts`, reset when the stream starts), so `panel_measure.py` has been averaging a running mean, and every burst figure before this section is diluted by the history before its segment. It now reads a segment's mean between two readings, `(mean_b * n_b - mean_a * n_a) / (n_b - n_a)` with the burst counts (good to about 10 us in a minute of a quarter-hour-old stream, the board keeping whole microseconds), and its tail from windows of ten seconds. `--views baseline` puts the dark view (nothing drawn) either side of each other view, so a view's cost is its burst less the dark stretches around it.

**What each view costs a burst** (microseconds of the decode task's burst of 1,536 frames, the kit, p4-rev3, 40 s segments, two Meters segments a row; the first tap is `203e560b4`, the cheaper one is this change):

Glow and pulse: the first tap's run, then the cheaper tap's. Meters: its two segments, a and b.

| Room | The burst with the dark view | Glow | Pulse | Meters, first tap | Meters, cheaper tap | The tap itself, first -> cheaper |
|---|---|---|---|---|---|---|
| E-AC-3 5.1 | 3,910 | +16; +25 | +36; +63 | +1,199, +1,202 | +945, +943 | 564 -> 345 |
| E-AC-3 JOC 7.1.4 | 21,450 | -172; -173 | +46; +78 | +1,911, +2,210 | +1,619, +1,836 | 701 -> 429 |
| E-AC-3 5.1 on 9.1.6 (older run, against the speakers view) | 4,350 | | | +1,182 | | 573 |
| AC-4 5.1 (older run, against the speakers view) | 25,780 | | | +1,948 | | 582 |

- **Only the Meters view costs the decoder anything.** The glow and the pulse are within the noise (a few tens of microseconds in the 5.1 room, about 200 in the heavy ones) of the dark view, which is nothing drawn: the render, the flush and the scan-out of the other views do not show in the burst. The Meters view adds 24 to 30 % to a 5.1 burst, 8 to 10 % to a JOC one and 7.5 % to AC-4's, which at 25.8 ms of the 32 is the room with the least left (4.2 ms with the view, 6.2 without).
- **The tap is a quarter to a third of that, not all of it.** 345 us of 945 in the 5.1 room and 429 of 1,730 under JOC. The rest is the panel's own work slowing the decoder while the two run at once; see the placement below. The plan's *burst* criterion (at most the speakers view's plus the larger of 15 % and 250 us) was written against the diluted figure and now fails the 5.1 room (+24 %, +30 % with the first tap) and passes the others; it is not changed here.
- **The tap: 39 % cheaper, bit for bit the same numbers.** It was scalar code at 14 to 25 instructions a sample, in a loop an output at a time that stored and loaded the mix between them, a staging array the ring was copied from afterwards, a logarithm a window on the decode task, and six 64-bit atomics a block, each a critical section on both cores (RV32 has no 64-bit AMO; `__atomic_*_8` takes `s_atomic_lock`). Now the outputs go four at a time with the mix and four peaks in registers, the conversion writes the ring in place, the head is 32 bits, the writer's counters are 32-bit AMOs that the reader drains into 64-bit totals, and the peaks are published as magnitudes. `test_meters_tap.cpp` holds it to the first version's loops over 165 combinations of outputs, live masks and block lengths, and across the 32-bit wrap of the head. On the kit: pass 349 -> 189 us a burst (5.1) and 470 -> 254 (JOC); pack 208 -> 150 and 216 -> 158. The pack was 39 cycles a sample for about 11 instructions (one chain of eight dependent float operations an iteration on an in-order core), so it now converts four samples side by side before storing any: 150 -> 101 us a burst in 5.1 and 158 -> 107 in JOC. The tap is then 303 us (5.1), 355 (JOC 7.1.4) and 307 (AC-4 5.1), where the first version took 564, 701 and 582: 46 to 49 % less. What is left is the pass, about 185 to 250 us, and 12 reads of the clock a burst at 1.2 us each (15 us).
- **Where the panel runs matters, and not the way to hope.** The same image with the panel task pinned to core 0 (the network's, and the decode lane's worker), to core 1 (the decode task's) and as it is (any core), the Meters view's extra burst against the dark view:

| Panel task | 5.1: extra burst | 5.1: update | JOC: extra burst | JOC: update, rate |
|---|---|---|---|---|
| any core (as it is) | +945, +943 | 10.9 to 11.0 ms, 20 Hz | +1,619, +1,836 | 20.9 to 21.7 ms, 20 Hz |
| core 0 | +963, +967 | 10.7 to 11.0 ms, 20 Hz | +1,702, +1,802 | 20.9 to 25.3 ms, 20 Hz |
| core 1 | +840, +856 | 11.7 to 12.4 ms, 20 Hz | +508, +685 | 53.9 ms, 10 Hz, 12.5 % of the steps with no update |

  On core 1 the decoder gives back 1.1 ms of a JOC burst (5 % of it) because the panel can only run while the decode task sleeps in the sink, never beside it; and the screen pays for it, since in a room whose decoder holds both cores for 21 ms of the 32 the panel's gaps are 9 ms on one core: its render goes from 14.5 to 38 ms and the view falls to 10 Hz and does not hold that either (the cost criterion and the dropped steps fail). Core 0 changes nothing. So the interference is the panel's memory traffic while the decoder runs, not the scheduler, and it is not recovered by placement without the screen's rate. (Inferred from the pinning; no bus counters were read.) The 5.1 room gets 100 us back and loses nothing. FreeRTOS in this build (IDF's, not the SMP kernel) cannot move a task between cores after it is made, so the placement is fixed when the panel's task starts.
- **The audio first (owner, 2026-10-28: "audio wins over graphics"): `ICLFORGE_EXAMPLE_PANEL_YIELD`, default 1.** The panel reads the player's mean burst once a second; at 14 ms or more of the 32 (in at that, out at 70 % of it: JOC 7.1.4 is 21 ms, AC-4 5.1 is 26, a 5.1 E-AC-3 room 4) the Meters view is held at ten updates a second (`RateGovernor::set_hold_slow`, which is not counted as a fall). Level 2 also makes the panel task on the decode core, always. The unrolled tap, the baseline schedule, the Meters view's extra burst against the dark view and what the screen did (the kit, `2026-10-28-y-{ty1,ty2,ta0,ta1,ta2}`):

| Room | 0: no policy | 1: held at ten (default) | 2: held, and the panel on the decode core |
|---|---|---|---|
| E-AC-3 5.1 (never busy) | +929, +926; 20 Hz, 11.0 to 11.1 ms | the same: it does not engage | +824, +790; 20 Hz, 11.5 to 12.8 ms |
| E-AC-3 JOC 7.1.4 | +1,619, +1,836 (the tap then 429 us, now 355) | +914, +1,211; 10 Hz, 25 to 31 ms an update, 2.5 % of the steps missed | +469, +741; 10 Hz asked, 50 to 61 ms, 11.3 % missed |
| AC-4 5.1 | +1,473, +1,563 | +1,143, +1,128; 10 Hz, 20 ms, 3.4 % missed | +631, +667; 10 Hz asked, 21 to 27 ms, 16.3 % missed |

  Level 1 takes 0.4 to 0.7 ms a burst off the decoder in the two heavy rooms (a quarter to two fifths of the view's cost) for a steady ten updates a second. Level 2 takes 0.9 to 1.1 ms off (to +0.6 to +0.7 ms in AC-4, 0.5 to 0.7 under JOC), but its view misses 11 to 16 % of its steps (about 8.5 to 9 updates a second in practice), and because the pin is for every screen the speakers view under JOC refreshes in twice the time (the glow 8.1 -> 16.8 ms, the pulse 6.8 -> 13.7; AC-4's did not move). So level 1 is the default, and 2 is a setting for a board where the audio has to have every microsecond. The AC-4 burst with the view is 27.1 ms of the 32 at level 0, 27.05 at 1 and 26.2 at 2 (25.6 with the dark view).
- **What is left for the decoder, in the order of effort:** fewer reads of the clock (about 15 us a burst); the panel's own traffic while a burst runs, which is the rest (about 0.6 ms in 5.1, 0.9 under JOC and 1.1 in AC-4 even at level 1): the analysis's 63 KB of tables and scratch and the canvases are in PSRAM, the render buffers are in PSRAM (internal RAM does not fit the tight rooms, above), and the DSI scan-out takes 74 MB/s of it all the time. A burst-aware panel (do the analysis and the drawing in the sink's wait, not beside the decode, without pinning every other screen to the decode core) would get level 2's gain without its cost to the other views, and wants the update to get cheaper than the gap, about 14 ms of every 50 under JOC. That needs a cheaper update (the analysis is 4 to 6 ms and the widgets 5 to 9 under JOC) and is not done.
- **The harness:** `panel_measure.py` reads a segment's burst between two readings, has `--views baseline`, and says each view's cost against the dark stretches around it; its tests hold both. The raw runs are in `D:\ac3bld\panel-measure\2026-10-28-tap-{c,n,p0,p1}` (the first tap, the cheaper tap, the panel on core 0 and on core 1; the core knob that made the last two is `2026-10-28-panel-task-core-knob.patch`, not in the tree) and `2026-10-28-y-{ty1,ty2,ta0,ta1,ta2}` (the unrolled tap and the policy, on the normal and the AC-4 images); `2026-10-28-asfound-image-run-{a,b}` are two runs of the board's older image (an update of 135 ms, the drawing before #1693) that were meant to be the first two and are only a record of what a flash refused for want of the bench's lock leaves behind.

## Questions for the owner (answered 2026-10-26)

The owner chose the adaptive rule of round 2 (which takes the place of "which"), the **zones** (green, amber and red) for the spectrum, **-60 to 0 dB**
with no second range, and **20 updates a second if possible, else 10**. The questions as they were asked:

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
