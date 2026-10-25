# The speakers view: options for something more dynamic

!!! note "Status as of 2026-10-25: the owner chose none of these; stage 5 of p4x-now-playing.md was built instead"
    **Outcome.** The owner saw the seven options below and chose none of them: *"rather than a full redesign, the
    today visualisation would be more responsive to the audio, and perhaps a variant of the rings where the audio
    levels cause a growing/shrinking of the speaker circle (when it was filled in) but not excessively so. None of
    the designs you showed really inspire me."* What was built is a more responsive glow, and a `pulse` style:
    [Stage 5](p4x-now-playing.md#stage-5-the-speakers-follow-the-sound). The three findings below (the 5 Hz read,
    the volume, the 32-rectangle line) were used. The recommendation at the end is the one that was not taken, kept
    as the record; the pictures of the seven are not in the repository (the three overviews are).

    The speakers view of [the no-lyrics screen](p4x-now-playing.md) is quiet on purpose: each speaker glows in five steps,
    five times a second, falling slowly. The owner, 2026-10-25: *"In speakers mode you get a little bit of visualisation on
    each channel when there's audio there. I want to be able to do something a little more dynamic with those speakers,
    reacting ... and want to think about what choices we have."* This page is what the board knows while a song plays, what
    each idea needs, seven options drawn over a synthetic music scene with what each costs, and a recommendation. Nothing
    here is built into the firmware; the prototypes are `firmware/hearth-sink/preview/speakers_options/` (not committed).

Pictures: `planning/p4x-speakers-options/overview_2-0.png`, `overview_5-1.png` and `overview_7-1-4.png` (every option
drawn in the same room; 2.0 is stereo PCM from Music Assistant, 7.1.4 has seven Atmos objects). The stills of each
option, the eight-frame sheets and the animations at 10 frames a second were made with the prototype and are not
kept in the repository. Raw measurements: `measure-inv32.csv` and `measure-inv128.csv` in the same folder.

## What the screen can know today, and what it costs to know more

| Fact | Where | Cost to the audio path |
|---|---|---|
| Peak and RMS of each output over the last 100 ms (dB of full scale) | `BurstPlayerStatus::peak_db/rms_db`, closed every 4,800 frames (`kLevelWindow`), copied by `sendspin_now_playing()` | Already measured; 72 us per live output per burst (#1618 notes). Silent outputs cost nothing |
| Where each speaker is | `OutputLayout` -> `rp::plan_nodes()` | None |
| What a stream is (codec, bed, objects declared) | decoder report | None |
| **Free, derived on the screen from the above:** crest factor (peak - RMS: punchy or sustained), level flux between windows (an onset), the power-weighted centre of the room (where the sound is), the LFE level (a room-wide pulse) | the 10 Hz levels | None |
| Three bands (bass / mid / treble) per output | **not measured** | A decimated one-pole split: **1.23x the cost of the existing meter** on a computer (`speakers_options <dir> bench`), so about **+60 to +90 us per live output per burst** (5.1: +0.35 to +0.55 ms, which is 8 to 12% of an E-AC-3 5.1 burst and 1.3 to 2% of an AC-4 5.1 burst; stereo: +0.12 to +0.18 ms). Estimate, not measured on a board. Cheaper: measure one burst in three (a display estimate), and none while an AC-4 stream runs near its period |
| Objects: position (x, y, z), size, and the renderer's gain of each object into each slot | the decoder and the renderer have them (`describe_objects()` once a unit, `LayoutRenderer::object_gain()`), **the panel cannot see them** | A new seam: a fixed table of up to 12 objects (about 430 bytes) copied into the status under its own lock at the 100 ms window: under 2 us. An object's *loudness* is not known: metering each object's audio is one meter per object (12 objects = 0.86 ms a burst); on every 8th sample it is 0.11 ms. Without it a dot cannot brighten with its own sound |
| A real waveform / scope | **nothing published** | 64 raw samples per output per window is nearly free to copy, but a 10 Hz snapshot of a 100 ms window flickers; the honest picture is the rolling envelope the board already can give (option 5) |
| Faster windows | `kLevelWindow` 4,800 -> 2,400 frames | One constant; the publish is a lock and 3 floats per output. Gives 20 Hz data |

### Three things found on the way that matter whichever option is chosen

1. **The panel reads the levels at 5 Hz (`lyric_screen.cpp`, every 200 ms while the speakers show); the player closes a
   window at 10 Hz.** Half the windows never reach the screen today. Any more reactive view has to read every window, or it
   will look half as alive as the data allows. The player's status has `levels_serial` (it moves when a window closes);
   `sendspin_now_playing()` does not pass it on yet, a two-line change, and with it the screen can read at 10 Hz and know
   whether the data is new.
2. **The levels are after the player's volume.** At 30% volume the glow shrinks with it. A view that should "react" the
   same at any volume has to judge a level against the programme's own recent loudest (the prototypes' `Ceiling`: rises at
   once, falls 1.5 dB a second; free, on the screen). Today's view does not: its glow is absolute.
3. **LVGL redraws the whole screen when more than 32 separate rectangles are invalid in one refresh** (`LV_INV_BUF_SIZE`
   defaults to 32 in `lv_display_private.h`; `sdkconfig.panel` does not set it). A busy view (a few dozen bars or dots
   changing in a 7.1.4 room) goes from tens of thousands of pixels a second to a full 1024 x 600 frame (614,000 pixels)
   every window: the bands prototype flushed **5.5 million pixels a second** in a 7.1.4 room and **269,000** with the
   buffer at 128 (2 KB). A compile definition on the `lvgl` component fixes it; so does drawing each speaker into one
   canvas, so that it is one rectangle. Today's view stays far below 32, which is why it never showed.

## The options

All seven are drawn on the same stage (1024 x 472) as today's view, from a synthetic scene: a groove (kick, snare, hats,
bass, a pad, an arpeggio, a clap behind the listener) panned onto the room, and for the Atmos room seven objects that fly
(a fly-over, a bee circling the listener, three rain objects overhead, a hopping bell, the arpeggio circling). The levels
are measured as the board measures them. The audio is made up, not decoded, and the drawing is prototype code; what is
exact is the count of pixels LVGL flushes.

**Today** is the real `now_screen.cpp`, for scale.

### 1. Rings
Each speaker is a disc inside up to three rings. A ring lights when the level reaches it and then fades over four windows,
so a hit leaves rings going out; a thicker ring marks the loudest peak, holds, then falls. Levels are against the
programme's own loudest, so a beat 8 dB down is a third of the rings at any volume. *Needs nothing new.*

### 2. Bands
Each speaker is three bars (bass, middle, treble) with caps that hold and fall; the LFE is one wide bar. The most
informative picture of the music, and the only one that needs a new meter. *Needs the band meter.*

### 3. Objects
The bed is the speakers, quiet; each object is a dot with a short trail, drawn where the renderer is putting it in this
room (the power centre of the speakers it feeds), lifted off the floor by its height with a shadow left behind (a square
for overhead), larger and brighter when louder. It never draws a dot the stream does not have: with no objects it shows
today's glow. *Needs the object seam.*

### 4. Phantom image ("where is the sound")
One bright image sits at the power-weighted centre of the room and glides as the mix moves, with a trail; its size follows
the loudness. On a stereo stream it is a slider between L and R (the stereo image); on 5.1 it wanders the room with the
lead and the clap behind. *Needs nothing new.*

### 5. Waveform tiles
Each speaker is a tile with its name and the last five seconds of its output scrolling by, drawn the way an audio editor
draws an overview (peak as a pale band, RMS inside). *Needs nothing new* (peak and RMS every 100 ms). Tiles shrink to fit
and at 9 or more speakers need a sparkline fallback.

### 6. Edge light
The edges of the screen take their colour from the speakers nearest them, a few steps from deep blue (quiet) to the accent
(loud): 6a a thin line, 6b a soft wash coming in from the edge. *Needs nothing new.* The least like the rest of the screen,
and the one most likely to be unwanted in a dim room.

### 7. Image and objects
4 behind quiet speakers, with 3 on top when the stream has objects. On a stream with none it is 4.

## Honest on which streams

| | 2.0 stereo (PCM from Music Assistant) | 5.1 bed | Atmos, objects reconstructed | Data the board lacks |
|---|---|---|---|---|
| 1 Rings | Yes: L and R pulse, the rest of the room absent | Yes | Yes (the mix of bed and objects; it cannot tell them apart) | none |
| 2 Bands | Yes, and the largest bars | Yes (the LFE has a bass bar only) | Yes | **band meter** |
| 3 Objects | **Nothing to show** (falls back to the glow) | **Nothing to show** | Yes, but only while the board reconstructs objects: a stream that declares more than the board's cap (12) plays as its bed (`objects_past_limit`), and then there are no dots | **object seam** |
| 4 Phantom | Yes: the stereo image | Yes | Yes: the net direction of everything | none |
| 5 Tiles | Yes | Yes | Yes (crowded at 9.1.6) | none |
| 6 Edge | Yes, but top and bottom edges just follow L and R together | Yes | Yes | none |
| 7 Mix | = 4 | = 4 | 4 + 3 | object seam |

The common case for the owner is stereo PCM from Music Assistant, where most of the room is empty: today it is two 58 px
discs on a black stage. Options 1, 2, 4 and 5 scale up to fill it; 3 is silent; 6 is decoration. The Atmos case depends on
the object cap: the preview fixture for a real Atmos programme says "5.1 + 15 objects" (a declared count, from the
programme's complexity index), which is more than the P4 and P4X's 12 and would play as a bed. **What the owner's Atmos
tracks declare has not been checked** (the Atmos programme's test tracks can say); if it is 15, option 3 would not appear
for them until the cap is raised, and that is the question for the boards, not for the screen.

## What each costs

The pixels are exact (LVGL's flush callback on a computer, 54 s of the synthetic music at the screen's 30 frames a second,
the window every third frame); no time on the P4 was measured. One yardstick the kit has measured: the
lyric screen's song filling flushes **192,000 pixels a second** and cost the light streams 5 to 12% of their decode time
and the heavy one 0.1 to 1% ([the second session on the kit](karaoke-p4x-panel.md)); today's speakers view cost +0.4%
of a 5.1 burst. A full 1024 x 600 frame is 614,000 pixels. A rough conversion that agrees with the lyric fill: 25 to 60 ns
a blended pixel on the P4, so 200,000 pixels a second is 5 to 12 ms of CPU a second.

| Option | Pixels flushed a second, in thousands: 2.0 / 5.1 / 7.1.4 | Flushes a second x pixels each (5.1 / 7.1.4) | LVGL objects (5.1 / 7.1.4) | Memory (KB, LVGL heap; canvases included) | Extra from the audio path |
|---|---|---|---|---|---|
| Today (real code) | 2 / 22 / 30 | 4 x 5367 / 7 x 4542 | 19 / 37 | 26 / 24 / 20 | none |
| 1 Rings | 131 / 524 / 135 | 23 x 22573 / 16 x 8282 | 29 / 41 | 2 / 10 / 14 | none |
| 2 Bands | 99 / 72 (**1724** with today's buffer) / 269 (**5544** with today's buffer) | 197 x 364 / 329 x 819 | 55 / 115 | 6 / 17 / 35 | **band meter**: +60..90 us per live output per burst (5.1: +0.35..0.55 ms) |
| 3 Objects | 3 / 18 / 56 (**357** with today's buffer) | 4 x 4982 / 74 x 758 | 13 / 74 | 2 / 5 / 24 | **object seam**: about 430 B a window; per-object level +0.11 ms a burst (12 objects, every 8th sample) or none |
| 4 Phantom image | 316 / 199 / 214 | 37 x 5321 / 41 x 5158 | 24 / 36 | 5 / 8 / 12 | none |
| 5 Waveform tiles | 1155 / 407 / 431 | 68 x 5995 / 144 x 3005 | 19 / 37 | 227 / 83 / 89 | none |
| 6a Edge light, line | 211 / 211 / 212 | 81 x 2619 / 83 x 2554 | 37 / 49 | 10 / 13 / 17 | none |
| 6b Edge light, wash | 663 / 684 / 668 | 78 x 8763 / 79 x 8418 | 37 / 49 | 10 / 13 / 17 | none |
| 7 Image and objects | 316 / 199 / 245 (**836** with today's buffer) | 37 x 5321 / 101 x 2421 | 24 / 85 | 5 / 8 / 27 | the object seam, for the dots |

"With today's buffer" is `LV_INV_BUF_SIZE` 32, as the firmware has it; the other figures are with 128. Where the two differ, the view
is past the 32-rectangle line and LVGL redraws the whole screen. Host render time is not tabled: on a computer it is noise.

The pixel rate follows how much moves how often: *more dynamic is more pixels*. The knobs that buy it back are the update
rate (5 Hz instead of 10 halves it), the size of what moves (smaller tiles, fewer rings), and quantising the steps.

## Recommendation

1. **Build 4 and 3 together as the more dynamic view** (option 7), on top of today's calm glow. 4 needs no new data and costs
   about 200,000 to 300,000 pixels a second (a lyric fill and a bit; halve it with a 5 Hz update); on stereo it is a real
   picture of the stereo image, and 5.1 and Atmos get a wandering centre. 3 is the one thing no other player's screen does
   (objects flying through the room), is the cheapest of all in pixels (3 to 56 thousand a second) and needs only a small seam
   from the decoder; it appears when the stream has objects within the cap.
2. **Keep today's glow as the quiet style** and make the choice a *Speakers style* setting: *calm* (today), *image* (7). Add
   *rings* (1) as a third style if the owner likes it: it needs nothing new, but at 5.1 it is 520,000 pixels a second as drawn
   and wants thinning (two rings, onset only).
3. **Hold 2 (bands) and 5 (tiles).** Bands is the best look but needs a new meter on the audio path (+0.35 to 0.55 ms a burst
   at 5.1) and the `LV_INV_BUF_SIZE` change; tiles cost 0.4 to 1.2 million pixels a second. Both are worth an A/B on the kit
   before the owner commits, and the AC-4 5.1 glitches are the reason to be careful.
4. **Not 6** unless the owner asks: it is decoration, costs 210,000 to 680,000 pixels a second, and is the least quiet.
5. Whatever is chosen: read every 100 ms window (10 Hz), judge levels against the programme's own loudest (so volume does not
   shrink the show), and set `LV_INV_BUF_SIZE` to 128.

## What stage 2 would have been, had the recommendation been taken (it was not)

- `libs/render/include/iclforge/render/room_plan.hpp`: the centre of the room (power-weighted) and the object dot's position (the centre of the
  slots an object feeds), both host-tested; `now_view.hpp`: the `SpeakersStyle` enum and its words.
- `BurstPlayerStatus`: an `ObjectScene` snapshot (id, x, y, z, slot gains), filled where `renderer.set_objects()` is called.
- `now_screen.cpp`: the image and the dots, built on the calm glow; `lyric_screen.cpp` is not touched (prefs-core owns it).
- A new NVS key `np_style`, never renumbered; a route `GET`/`PUT /speakers-style` with its word list in **four** places
  (`control.cpp` text and 400 text, and two copies in `apps/demos/wasm/tests/device-ui/stub.js`); a registry entry in
  Preferences once prefs-core's PR merges, REST only until then.
- Docs (`docs/hearth/sink-esp32-p4.md`, `p4x-now-playing.md`), preview scenarios and pictures (`pictures.json` names the commit).
- An A/B on the kit once the coordinator books it: the `burst_us` of E-AC-3 5.1, stereo PCM and AC-4 5.1 with the style
  calm and image, in alternating order, against the budget the speakers view was measured to (+0.4% of a 5.1 burst).

## How this was measured, and what it is not

- A synthetic scene, fixed, so that every option sees the same music (`scene.hpp`). Real tracks will move differently,
  mostly less evenly; the pictures show how each option reacts, not how a track would look.
- Pixels flushed, redraws a second, LVGL objects and heap: exact for LVGL 9.5 with RGB565 and a 40-row partial buffer, as
  the preview tool draws. Heap is LVGL's allocator after the build (about 20 KB of every figure is the label font's glyph
  cache, loaded by the first option). Canvases are counted.
- The band meter's cost is a host ratio applied to the kit's measured 72 us per output per burst. **No board was used.**
- The prototypes are not firmware: the dots, the blob and the canvases are written to compare, not to ship.
