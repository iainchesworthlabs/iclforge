# Speaker trim and the identify tone

Task E1 of the Preferences programme (Preferences and Adjust for a board with a screen, started
2026-10-25 and kept by its coordinator). The owner asked for
two things under Preferences: *Identify speakers*, a test tone out of each output, and a
*per-speaker level and distance trim*. This page is the design of both: the board's side (what
was already there, what was decided, what it costs), [what is on the screen](#on-the-screen), and what
is left to check on a board. The user page is [docs/hearth/sink-speakers.md](../docs/hearth/sink-speakers.md).

## What was already there

The output stage is old. `BurstPlayer` (`firmware/esp-idf/iclforge/src/burst_player.cpp`) routes each
rendered block to the sink's outputs, then trims and delays each output (`iclforge::render::TrimDelay`,
delay lines in PSRAM), replaces it with the identify tone while one plays
(`iclforge::render::IdentifyTone`, pink noise, with an idle mode that writes the tone straight to the
sink when nothing plays), meters it, scales it by the volume and hands it to the playout. That is
after the decode and the renderer and before the interleave, for PCM, AC-3, E-AC-3, AC-4 and objects
alike, so nothing about where the new numbers apply needed deciding.

What was missing is the board's own side. Only a Sendspin server drove the stage: its `trim_db` and
`delay_ms` per output, and its identify command, with the server's settings replacing the board's
whole ("a key left out takes the board's own value", and the board had no value of its own). The tone
a server starts is the server's to stop, with a level it chooses up to -12 dB RMS.

## Decisions

**D1. A trim is kept by speaker role, not by output.** The output a speaker is wired to is the board's
business (the layout's slot order, a server's patch); where the speaker stands and how loud it is
against the others is the room's. A trim kept against `Ls` is still the left surround's when the layout
goes from 5.1 to 7.1.4 or a server patches it elsewhere, and a speaker the layout does not have is kept
for the day it has. The roles are Table E2.5's 22 locations plus `S1` to `S16` for a slot a list places
by angle. Their stored numbers are frozen (a test pins them); a new role takes the next number.
Everything unset is 0 dB and no distance.

**D2. Level: -12 to +12 dB in 0.5 dB steps.** Applied as a float gain per output. A change while a
song plays ramps in a straight line over the next 256-frame block (`TrimDelay::ramp_trim_db`), so a
step is not a click. A boost can clip at full scale; the sink clips and does not limit.

**D3. Distance: 0.1 to 10 m, in centimetres; the delay is relative to the farthest speaker.** A speaker
with no distance counts as being at the farthest one's, so setting one distance moves nothing and each
further one moves only its own speaker (the alternative, an unset distance of 0 m, would delay every
other speaker by the first one set). The delay is `(farthest - distance) / 343 m/s` at the stream rate,
to the nearest sample (`delay_samples_for`: 100 cm is 140 samples at 48 kHz).

**D4. How the delay meets the Sendspin clock: it does not move the play-time stamp.** The playout
places each decoded frame at the time the server asked for, and measures the error from the block the
sink took. The delay is applied per output inside the block sequence, after the decode and before the
playout, so the playout's timeline and its error measure see the undelayed signal. The farthest speaker
has no delay, so the frame the server stamped plays out of it at that time; every nearer speaker is
later than the stamp by the travel time it makes up, never earlier. Two boards in a group each align
their own farthest speaker, so a group stays together however each room is measured. A delay across
boards (one board's room is deeper than another's) is the static `output_delay_ms` of the Sendspin
player, which this does not touch. A new stream empties the lines, so a near speaker is silent for its
delay at the start, which is the delay.

**D5. The server wins per key, and the table is the board's own value.** A server's `trim_db` replaces
the levels in force, its `delay_ms` the delays, each on its own; a key it leaves out is the table's
(`OutputTrim::set_server`). Summing them was rejected: the server's numbers come from its own
calibration and a sum would make a setting no one chose. `GET /trim` says `server_level` and
`server_delay`, and the Preferences row says the numbers are the server's.

**D6. Delay lines are PSRAM only, 20 ms, and a board without them says so.** The lines were already
one buffer per output of `CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_DELAY_MS` samples, in PSRAM when the
part has it, and the published profiles lock 20 (the C6 and the CI shapes lock 0). 20 ms is 6.86 m of
sound, so the nearest and the farthest speaker can be that far apart, each at any distance from
0.1 to 10 m; the owner's 10 m would want 30 ms (10.29 m, 92,160 bytes of PSRAM for 16 outputs, 30,720
more than now). **That is not done here**: a locked value in `profiles.json` re-hashes five profiles
and, by the rule in [esp32-platform-profiles.md](esp32-platform-profiles.md), re-records their
baselines on a board. It is the one Kconfig and `profiles.json` edit, and the range the board reports
(`max_span_m`) follows it. A part with no PSRAM has no delay lines whatever the build asks: the support
object says 0 and the delay is unavailable with the reason "this board has no PSRAM to hold delay
lines" (a build with 0 says "this build has no delay lines"). Level trim needs nothing and stays
available. `PUT /trim` refuses a change that would put the nearest and the farthest distance further
apart than the lines hold (409, with the span and the milliseconds), unless it makes the span smaller
or leaves it alone; a span left by a build with longer lines is cut to what this one holds and the
speaker named (`clamped`).

**D7. A live change moves only what it changes.** A level ramps. A delay that differs changes that
output's line, which drops that one speaker for the new delay's length (20 ms at most), once; an output
whose delay is unchanged is not touched and its line plays on. A new stream resets the lines and ends a
ramp. A change of layout or patch sets everything up from scratch.

**D8. The tone is a sequence with its limits in the code.** `IdentifySequence` plays the existing pink
noise (the low band for an LFE or subwoofer feed) on one speaker, or on each in turn:

| Limit | Value | Why |
|---|---|---|
| Level | at most -24 dBFS RMS (pink noise peaks about 12 dB higher), default -30 | It comes out of real speakers. The server's `IdentifyTone` allows -12. |
| A burst | 1.5 s, 20 ms smoothstep fades | No speaker is clicked. |
| Between speakers | 0.5 s | |
| The whole of a start | 60 s, loops included | Nothing a request says leaves it playing. |

It takes the speaker's level trim (an AVR's test tone does: it is how levels are matched by ear) and
the player's volume, and goes through the same output stage as the music, so a tone out of the left
surround proves slot, DAC channel and speaker. Words: `off`, `all`, a role, a role and `loop`.

**D9. When it is refused and when it stops.** Refused (409 with the reason) while a stream plays
(`BurstPlayer::active()`), while the player is held (a play from the control surface, an update) or going,
and while a server's tone is on; a role the layout does not have at an output is refused as well. It
stops at once (the next block is not the tone) when a stream's start message reaches the decode task,
when the player is held (`wait_while_held`), and when a server's tone starts; `off` fades over 20 ms.
The web page sends `off` as it is closed (`sendBeacon`), and the 60 s limit covers a client that
vanishes. The tone never opens or closes the sink: the player had it open at the layout's outputs, a
tone writes blocks to it as the idle identify always did, and when the sequence ends the player waits
as before. The last block it writes is silence.

**D10. A bug found on the way, and fixed.** `render_reached()` leaves the slots a block does not reach
as they were. `live_outputs()` kept every output live whenever any delay was set, and the patch then
copied those slots' stale audio into outputs nothing was feeding (by reading the code; it has not been
heard on a board): a server delay on a 5.1 stream on a 7.1.4 layout would play the last block an earlier
object stream left in the heights, again and again. An output that is live without a reached slot
feeding it (a delay line's tail, a server's tone) now reads silence. And only the outputs that have a
delay stay live beyond the fed ones, where before all of them did, which is also cheaper.

## Where things are

| Piece | File |
|---|---|
| Roles, the table, its text and stored form, the resolve through layout and patch, `GET /trim`'s JSON | `libs/render/include/iclforge/render/speaker_trim.hpp` |
| The stage: table and server, live changes | `libs/render/include/iclforge/render/output_trim.hpp` |
| The tone's sequence and limits | `libs/render/include/iclforge/render/identify_sequence.hpp` |
| `POST /identify`'s words, refusals, `GET /identify`'s JSON | `libs/render/include/iclforge/render/identify_request.hpp` |
| Ramped level, delayed-output mask | `libs/render/include/iclforge/render/trim_delay.hpp` |
| The player's use of them | `firmware/esp-idf/iclforge/src/burst_player.cpp` |
| The NVS key `speaker_trim` (a blob of the non-default roles, at most 154 bytes), the Kconfig default `CONFIG_ICLFORGE_EXAMPLE_SPEAKER_TRIM` | `firmware/hearth-sink/main/settings.{hpp,cpp}` |
| The seam (`sendspin_sound_caps`, `sendspin_set_speaker_trim`, `sendspin_speaker_trim_status`, `sendspin_identify`, `sendspin_identify_status`) | `firmware/hearth-sink/main/sendspin.hpp` and `sendspin/{player,none}/` |
| The routes `GET` and `PUT /trim`, `GET` and `POST /identify`: their words in `control.cpp`, their code in `sound.cpp` (the same code the screen goes through), wired in `hearth_sink.cpp` | `firmware/esp-idf/iclforge/src/control.cpp`, `firmware/hearth-sink/main/sound{,_routes}.{hpp,cpp}` |
| The device page's *Levels and distances* section | `firmware/esp-idf/iclforge/ui/` |
| The registry entries `identify-speakers` and `speaker-trim`, and the kind `sheet` | `libs/device/include/iclforge/preferences.hpp`, the firmware's side in `firmware/hearth-sink/main/preferences.cpp` |
| The steps and words of an editor: `step_half_db`, `step_distance_cm`, `level_text`, `distance_text`, `summary_text`, the statements | `libs/render/include/iclforge/render/speaker_trim.hpp` |
| The Speakers sheet | `firmware/hearth-sink/main/panel/lvgl/sound_sheet.{hpp,cpp}`, opened from `prefs_screen.cpp` |

The route word list is in four places, as it always was: `control.cpp`'s `GET /api` text, the stand-in's
copy of it and its route list in `apps/demos/wasm/tests/device-ui/stub.js` (the contract spec holds the
stand-in to the firmware's strings and routes), and the page's request list in `contract.spec.js`.

## On the screen

*Identify speakers* and *Speaker levels and distances* are the two entries of the **Sound** group of Preferences
(`libs/device/include/iclforge/preferences.hpp`, the contract of the programme's first task). Both are available where the build
has a Sendspin player and say *This build has no player, so it has no speakers to play a tone through.* where it has not.

- **Identify speakers** is an action whose button says *Identify*. A press asks the board for the tone through every speaker in
  turn (`POST /identify`'s `all`) and **opens the Speakers sheet**, where the speaker the tone is on is lit: the first version
  made the button *Play each* and then *Stop*, and the owner, on the kit, found that a button that only said Stop did not say which
  speaker was which. The entry still has its two choices, `run` and `running`, so that `GET /preferences` says when the tone plays,
  but the screen's button no longer changes with them; a press while it plays starts it over (the sheet's *Stop* is the stop).
  A song playing, or a play from the web page, is said under the row (the entry's `inert` text) and on the sheet, and a press
  the board refuses is refused in the same words as `POST /identify`'s; the sheet opens either way and says why.
- **Speaker levels and distances** is the new kind `sheet` (added at the end of `Kind`, as the header asks): a row that says what is
  set (*none set*, *1 speaker set*, *3 speakers set*) and an *Edit* button that opens the Speakers sheet. Its setter takes the
  words of `PUT /trim`, so the row, the route and the sheet are one path. A sheet has no choices to list.

**The Speakers sheet** (`panel/lvgl/sound_sheet.cpp`) is a full-screen page over the Preferences list, built when it opens and
again when the layout changes under it (a `PUT /layout` from the web page). It has:

- the room, drawn by `room_plan.hpp` (read-only, as the lyric screen uses it) for the layout in force, only the speakers the
  layout has at an output, **each with its ID inside it**, the speaker the tone is on lit from `GET /identify`'s `role` and the
  chosen one ringed; a tap on a speaker chooses it and plays the tone through it. The room's height is as tall as the layout needs
  for no two shapes to be closer than 6 px, no taller: at the kit's width a 2.0 room is 120 px, a 5.1 180, a 7.1.4 240 and a
  9.1.6 260. A display narrower than 760 px has no room: the speakers are a row of buttons in output order, which are the same
  touch targets;
- for the chosen speaker, *Level* (- and + a half decibel, from -12 to +12 dB) and *Distance* (- and + a tenth of a metre, and
  -1 m and +1 m; the first + on a speaker with no distance starts it at the farthest distance any speaker has, so that setting
  it changes nothing until it is moved, and 3 m when none has one; a - from 0.1 m takes it away again);
- **a bar at the foot, fixed**: *Play*, *Repeat* (the chosen speaker over and over until it is pressed again, so that a level can
  be set by ear), *Play each*, *Stop* and *Reset* (the chosen speaker back to 0 dB and no distance). The first version put these
  at the end of the page that scrolled, and on the kit's 9.1.6 room they were below the screen. Now the title's bar, this bar
  and what is between them are three objects: the room and the speaker scroll between the bars where they are more than the
  screen holds, and neither bar moves. Where five buttons do not fit one line the bar wraps them onto two (a 240 by 240 module) and
  is as tall as that needs, and where only shorter words fit one line it uses them (*Loop*, *All*);
- what is really in force for the chosen speaker (*In force: -1.5 dB, 1.2 ms later*), and under the controls, why a control does
  nothing: a song is playing; this board has no delay lines (the distance is greyed, the level is not); a Sendspin server is
  setting the levels or the delays now; the speaker's delay is cut to what the lines hold.

A refusal (*the nearest and the farthest speaker can be at most 6.86 m apart on this board*) is said in the page above the
controls, in the accent, for six seconds. Every step is one statement to `PUT /trim`, so it is stored (NVS) and handed to the
player exactly as the route does; a person tapping is at most three writes a second. **The tone stops when the sheet is
left**: *Back*, the Preferences being closed, and the Preferences putting themselves away after a minute untouched each send
`off`; and the player ends it of its own accord at once when a stream starts and after a minute whatever is asked.

The same code serves the screen and the web page: `main/sound.cpp` holds `GET /trim`, `PUT /trim`, `GET /identify` and
`POST /identify` (`hearth_sink.cpp` hands them to the control surface), and `sound_sheet()`, `sound_set_trim_text()` and
`sound_identify_request()` for the panel. `sound.hpp` has no LVGL and no control surface in it, so that the preview tool supplies
its own. **Trims survive Reset preferences**: the key is a `setup` row of `settings_keys.hpp` (what the room is, not what a person
likes), and when the registry grows a reset flag the two entries say *keep*.

The web page keeps its own *Levels and distances* section (a table with a field for each number) rather than rendering a generic
Sound section from `GET /preferences`: the table is what the page needs and the listing now says what is set and the route.

## What it costs

**Zero when nothing is set.** With every speaker at 0 dB and no distance, `OutputTrim::process` finds
unity gain and no delay on every output and writes nothing: the samples that reach the sink are the
samples that left the patch, bit for bit (a test holds it), and the meter keeps its integer shortcut
(`trim.active()` is false). The hash of a PCM stream at the sink is therefore unchanged; the kit check
below compares it on the board.

**Host figures** (MSVC Release, x64, 256-frame blocks, data in L1; `[output-trim-bench]`, run by name):
neutral 5 ns an output, level 30 ns, delay 40 ns, both 70 to 130 ns. They give the proportions: a
delay costs about 1.4 times a level and both about 3 times, on a CPU with vector units and no PSRAM.

**Boards (estimates; the kit measures).** The S3 (240 MHz, single-precision FPU): a level is about 3
cycles a sample, 3 us an output a block; a delay swaps a block with its line, about 5 cycles a sample
plus the PSRAM traffic, since sixteen 5.6 KB lines do not fit the 32 KB data cache: about 1 KB read and
1 KB written back an output a block, up to 20 us with the octal PSRAM. The P4 (360 MHz, larger cache)
is a third of that. Against the 176 us an output the burst already spends on the meter and the playout
copy ([PR #1618](https://github.com/iainchesworthlabs/iclforge/pull/1618)), a speaker with both is
2 to 12 % more, and only the speakers that have either. A delay keeps its output live for the line's
tail, so an output a stream does not fill loses the skip in PR #1618 only if it has a delay.

**Memory.**

| | Where | Bytes |
|---|---|---|
| Delay lines, 16 outputs, 20 ms, 48 kHz (unchanged) | PSRAM | 61,440 |
| Delay lines, 8 outputs (the C6's ceiling, if it had PSRAM) | PSRAM | 46,080 |
| The table in `Settings` | `.bss`, internal | 152 |
| `OutputTrim`, the sequence and the pending copies, in the player | internal heap | about 1 KB more than the stage had |
| `BurstPlayerStatus` | stack of whoever reads it | 8 more (it is copied by value ten times a second; the trim report is a read of its own, `BurstPlayer::trim_status()`, about 190) |
| The page | flash | +4.7 KB (66,563 of the 69,632 budget, which was 65,536) |

The S3 has no delay line in internal RAM, and none at all without PSRAM.

**Flash.** The images built from main on 2026-10-25 (0e45f23a7) and from this branch, with the profile
tool's `verify` on each (no board):

| Profile | Main | This branch | Budget on main |
|---|---|---|---|
| `s3-n16r8` | 1,591,216 | 1,614,192 (+22,976) | 1,600,000 |
| `p4-rev3` | 2,604,576 | 2,631,184 (+26,608) | 2,600,000 (main was already over it) |
| `c6-16mb` | 1,874,608 | 1,901,472 (+26,864) | 1,880,000 |

All three were over the budgets main had. The budgets are growth alarms and the guard that reads them
runs on pushes to `main` only; #1645 raised them for everything in flight (1,750,000, 2,000,000 and
2,900,000), which covers these sizes with room, and the C6's 1,901,472 stays under the 1,966,080 slot of
the 4 MB table. This branch leaves `profiles.json`'s budgets alone. Built again after merging main
(2026-10-25 17:53): `s3-n16r8` 1,621,008, `p4-rev3` 2,638,688 and `c6-16mb` 1,908,912 bytes, each held by
`verify` with the new budgets.

Where the S3's 23 KB went, by object file: `burst_player.cpp` +6.4 KB (the stage, the sequence,
the role tables and the report), `hearth_sink.cpp` +6.4 KB (the two JSON writers, the statement
parser, the words of `POST /identify`), the device page +4.7 KB, string data +2.6 KB, `sendspin.cpp`
+0.8 KB, `control.cpp` +0.8 KB, `settings.cpp` +0.7 KB. Static internal RAM on the S3 is
161,445 -> 161,597 bytes (the table in `Settings`; the budget is 185,000). The delay-line default is unchanged at 20 ms
for the reason in D6, so the boot heap on a board is the player's extra kilobyte and nothing else:
the kit run below reads it against main's.

## The routes

```text
GET  /trim       JSON: the layout, what the build can do (level, delay: available, reason, max_span_m),
                 server_level, server_delay, farthest_m, and for each speaker its role, label, output,
                 the level and distance set, and the level, delay and clamp in force.
PUT  /trim       body: statements, one wrong and none is taken.
                   L level=-1.5 distance=2.4; Ls distance=1.9; R reset; reset
GET  /identify   JSON: available (and why not), active, the output and role sounding, remaining_ms,
                 the level, the cap, the burst, the gap and the total limit, the speakers.
POST /identify   body: off | all | <role> | <role> loop
```

`GET /trim` and `GET /identify` answer 404 on a build without a Sendspin player; so do the others.
A refusal is 400 for words the route does not read, 409 for what the board cannot do now.

## Tests

- `libs/render/tests`: `test_speaker_trim.cpp` (frozen role numbers, every location's role, the text
  grammar and its refusals all-or-nothing, the stored form, the delay to the sample at 44.1, 48 and 96
  kHz, the farthest speaker untouched, a trim following its speaker through a layout change and a patch,
  the report's JSON), `test_output_trim.cpp` (zero trim is bit-exact, a delay across blocks of 256, 100,
  7 and 1 samples, the server's keys one by one, a live change moving only what it changes, a new stream,
  a build with no delay lines, storage too small), `test_identify_sequence.cpp` (the level cap by
  measurement at 0, -6 and +100 dB, the burst length, the fades against the raw noise, block-size
  independence, each speaker in turn, the 60 s limit however much is asked, stop with and without the
  fade), `test_identify_request.cpp`, and the ramp and mask cases of `test_trim_delay.cpp`.
- `apps/demos/wasm/tests/device-ui/sound.spec.js`: the page against the stand-in, and the stand-in's JSON
  against the headers that write it, key for key.
- `libs/device/tests/test_preferences.cpp`: the Sound entries against every known board (a reason wherever greyed, a default
  where on), the Identify button starting the tone (and starting it over when pressed again), a board that refuses, a build with
  no player, the sheet's summary and its text going to the board whole, and the listing's JSON.
- `libs/render/tests/test_speaker_trim.cpp` (`[editor]`): the level's 49 stops, the distance's steps, seed, ends and "not set",
  the words, and the statements the sheet sends being ones the table's own parser takes for every role.
- `firmware/hearth-sink/preview` (the screen on a computer): the Preferences list with the Sound group and the sheet at 1024 by 600,
  800 by 480, 480 by 320, 320 by 240 and 240 by 240: every touch target 48 px and 6 px apart, text not cut off, Identify opening the
  sheet with the tone running and each speaker lit in turn (every ID on a speaker, exactly one lit), a tap on a speaker playing it
  and lighting it, the level and distance steps, a refusal said, Repeat, Play each, Stop (which leaves the controls), Reset, a song
  playing, a layout changing under the sheet, no delay lines, a server's levels, and the tone going with the sheet.
  **The room is drawn in every layout the sheet knows, 2.0, 5.1, 7.1.4 and 9.1.6, at every size, and `audit_on_glass()` holds
  the sheet to the glass**: the bars at the top and the foot to the screen, and what scrolls between them to its own viewport where
  it is to be seen without a finger (all four layouts at 1024 by 600; all but 9.1.6 at 800 by 480). The first version of
  the sheet passed `audit()`, which lets a list that scrolls run off the glass, with its buttons below the kit's screen in a
  9.1.6 room (the check ran 5.1 and 7.1.4 and never the kit's layout); a room made taller on purpose now fails the new check.
- Firmware builds, no board: the P4X profile (`p4-rev3`), the S3 profile (`s3-n16r8`) and the C6 (`c6-16mb`), and the
  no-player seam syntax-checked with the same flags.

## Left for the kit (a window the coordinator books)

1. A tone out of each of the 16 TDM slots into the scope or the DAC: `POST /identify` with each of
   `L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE` on a 7.1.4 layout and `all` on 9.1.6; the level on the scope
   is about -30 dBFS RMS and never above -24.
2. A level and a distance on a playing stream: the burst time (`worst_burst_us` in `GET /status`) with
   every speaker at 0, then with all sixteen at a level and a distance; the delay on the scope between
   two speakers at 1.0 m and 2.0 m is 2.9 ms; the play-time error of the stream (`error_us`) does not move.
3. The PCM hash at the sink with every trim at 0, on an AC-3 and on a PCM play, against a build of main.
4. A tone refused while a stream plays; a stream started while a tone sounds ends it in the next
   block (look for the scope trace stopping, not fading); the page closed during a loop.
5. `GET /hardware` and the free internal heap at boot against main's, S3 and P4X.
6. On the kit's panel: *Identify* in the list opens the sheet and plays each speaker in turn, ending at the last, the light on
   the speaker you hear (on the kit's layout, 9.1.6, with all sixteen labelled); a tap on each speaker of the sheet plays it and
   lights it; the five buttons are on the glass; *Repeat* while the level is stepped is heard to
   change by half a decibel; *Back* and a stream starting each end a tone in the next block; a distance on two speakers is a delay on the
   scope; the sheet is not slower to draw than the list (`prefs_build_us_max` in `GET :8081/panel`, which counts the list's build; the
   sheet's own build is in the log line).

## Not done here

- Nothing is run on a board: no flash, no OTA, no bench lock.
- The two entries do not say *kept by Reset preferences* in the registry, because the registry has no such field yet
  (the System entries' branch adds `Pref::on_reset`); the keys table keeps the trims already, which a host test holds.
- A held + or - that repeats: a step is a tap. The coarse button covers the distance, and the level has 48 stops.
- The page's Sound section is not generated from `GET /preferences`.
