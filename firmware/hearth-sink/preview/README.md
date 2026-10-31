# The lyric screen on a computer

`preview.cpp` runs `main/panel/lvgl/lyric_screen.cpp` as the board runs it, fed a document and a
position instead of the Sendspin player, drawn by LVGL's software renderer into a 1024 by 600
buffer, and writes one 24-bit BMP a screen (idle, waiting, singing, a duet, a gap, a line-timed
file, plain text, no lyrics with its screen off, the speakers in five rooms, the title card (a song, a long
title, one with no album, a title only, a title the fonts cannot draw, none said, and the speakers' header
with a title), right to left, the controls sheet, and the speakers and the title card dimmed and woken by a
touch, which it checks as well as draws: it exits 3 when the screen stays lit, stays dark or does not wake)
and the developer view, which it opens as a person does,
with three taps on the board's name (what it shows is a fixture in `preview.cpp`: a healthy kit, one in
trouble, one with nothing playing, and the confirming step of the network preference).

It then draws the **Adjust** and **Preferences** sheets at four display sizes (1024 by 600, 800 by 480, 480 by 320 and
320 by 240: `adjust-`, `prefs-`, `prefs-greyed-` (a board whose screen has no touch), `prefs-dark-` and `footer-` BMPs
named for the size) and checks them, which fails the run (exit 3): text that does not fit its label, a control that
sticks out of what holds it or off the screen, a touch target under 48 px or closer than 6 px to another, a tap that
does nothing, a greyed choice that answers a tap, a list longer than the glass that does not scroll under a finger,
and the dim time and the minute's idle close behaving with Preferences open. It also drives the **editors** of the
Preferences sheet (the night schedule's two times, held buttons included, and the time zone's regions and places, America's
144 among them) and the **night schedule** on the screen itself (dark at 23:00, a touch lights it for 30 seconds, lit with no
time known, the idle screen dark only when told); each editor is held to the same rules, and writes `prefs-night-times-`,
`prefs-zone-regions-`, `prefs-zone-cities-` and `prefs-zone-reopened-` BMPs for the size. Buttons are found by what they say, not by
where they are, so a change of layout does not break the taps. The boards drawn are `iclforge/board_variants.hpp`'s.

It exists because the screen is a file that changes, and a firmware build is ten minutes and a
flash. What it leaves out is the board: the Sendspin player, the DSI host, the PSRAM and the
timing. It reads the example songs from `examples/karaoke` and the fonts from `main/panel/fonts`.

```
# LVGL is the registry's copy that any P4 build of firmware/hearth-sink puts under managed_components
cmake -S firmware/hearth-sink/preview -B build-preview -G Ninja -DLVGL_DIR=<firmware/hearth-sink/managed_components/lvgl__lvgl>
cmake --build build-preview
build-preview/panel_preview <output directory>
```

Not part of the project's own build and run by no CI job: it needs LVGL's source, which only a P4
firmware configure fetches. The pictures in `docs/hearth/screenshots/p4-panel-*.png` are this
program's output converted to PNG.

`speakers_options/` is a project of its own for the speakers view (planning/p4x-speakers-options.md): the old glow, the
responsive glow and the pulse style drawn over a synthetic groove in 2.0, 5.1, 7.1.4 and 9.1.6 as stills and frame
sequences, with the pixels each flushes counted, and `speakers_tune`, which chooses the glow's constants without LVGL.

`meters_options/` is a project of its own for the Meters view (planning/p4x-meters-view.md): `meters_options` draws the first round's
three variants and `meters_adaptive` the rule that picks a representation from the display's size and the room's outputs (every size
class in every room, as stills and frame sequences, with the pixels each flushes counted at three sizes of LVGL's invalid-area table), and
`meters_tune` holds the spectrum analysis (`iclforge/dsp/spectrum.hpp`, the code a board runs) to a sine, to noise and to one long FFT and
prices it. They run on the made-up groove of `speakers_options/scene.hpp`.

## Scripted taps

`panel_preview --drive` is the same screen code with a **virtual pointer** and no scenarios of its own: it reads a command a
line on stdin and answers a line of JSON (`init`, `tap LABEL [ROW]`, `tap-at`, `swipe`, `wait`, `shown`, `prefs`, `ui`,
`shot`). A button is found by what it says, as a person finds it, and one off the glass is reached by swiping the list with
the pointer, not by calling LVGL; the answer's `events` are the touches made. `prefs` is `GET /preferences`, written by the
firmware's own `iclforge::prefs::to_json` over the preview's backend, so a script reads the same state here and on a board.

`tools/hearth/screen_script.py` is the driver. A **script** is a few lines of taps and expectations
(`scripts/*.screen`, grammar in the tool's header and in `planning/screen-test-harness.md`):

```
board p4-rev3-kit
display 1024 600
scene lyrics

tap "Preferences"
tap "+" in "Brightness"
expect pref brightness = 90
expect shown "90%"
```

```
python tools/hearth/screen_script.py check                                   # the grammar, no preview needed
python tools/hearth/screen_script.py run --preview build-preview/panel_preview   # every script, on the preview
ctest --test-dir build-preview --output-on-failure                           # the same, registered with this project
```

`tools/hearth/test_screen_script.py` (the CI "Oracle unit tests" step) holds the grammar and the runner with a stand-in
preview and a stand-in board, and runs every script on the real screen code when `ICLFORGE_PANEL_PREVIEW` names a built
`panel_preview`. The same script can also be run on a board in step with the preview (`--board`, a lease, `probe` for a
read-only look first): see `planning/screen-test-harness.md`, "Hardware in the loop". That path has not yet been run on a board.
