# The lyric screen on a computer

`preview.cpp` runs `main/panel/lvgl/lyric_screen.cpp` as the board runs it, fed a document and a
position instead of the Sendspin player, drawn by LVGL's software renderer into a 1024 by 600
buffer, and writes one 24-bit BMP a screen (idle, waiting, singing, a duet, a gap, a line-timed
file, plain text, no lyrics, right to left, the controls sheet) and the developer view, which it
opens as a person does, with three taps on the board's name (what it shows is a fixture in
`preview.cpp`: a healthy kit, one in trouble, one with nothing playing, and the confirming step of
the network preference).

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
