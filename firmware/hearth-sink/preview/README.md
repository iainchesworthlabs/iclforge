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
