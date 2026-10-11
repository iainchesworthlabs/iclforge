# ESP32-S3

The minimum-footprint codec profile on an Espressif ESP32-S3: a 240 MHz dual-core Xtensa LX7 with
a single-precision FPU, 512 KB of internal SRAM and hardware I2S. The standalone decode and
encode probes run in internal SRAM with no PSRAM. The networked Hearth Sendspin sink requires
8 MB of PSRAM.

It is the second bare-metal target. The first is [`arm-none-eabi` on QEMU](cortex-m3.md), a
Cortex-M3 with no FPU. The S3 has hardware single-precision floating point, which is what makes
the float32 path worth having and real-time decode worth measuring.

## Status

| | |
|---|---|
| AC-3 decode | Correct. Mono, stereo and 5.1, and 5.1 folded to Lo/Ro stereo in line mode by the §7.8 output stage, every channel level exact against `testdata/baremetal/fixture.hpp` |
| E-AC-3 decode | Correct. 5.1, 2/0 and 7.1.4 (a bed and two dependent substreams), including AHT, spectral extension and §7.5.4 rematrixing; 5.1 and 7.1.4 folded to Lo/Ro stereo in line mode; and 5.1 in line mode from a stream carrying dynrng words and dialnorm 24 |
| E-AC-3 §E3.5 enhanced coupling | Correct, on its own fixture. Costs 3 allocations per frame, level with plain E-AC-3 |
| Atmos bed | Correct, decoded bed-only via `DecoderConfig::skip_object_reconstruction`. 11 allocations per frame |
| Atmos objects | **Correct, reconstructed on target.** 22 allocations per frame — see [Objects](#objects). **And placed**: the `eac3_atmos_render` row pans a height-object stream onto 7.1.4 through the block form, every level the host's — see [Placed on loudspeakers](#placed-on-loudspeakers) |
| Encode | AC-3 and E-AC-3, six rows: 5.1 and 2/0 through each encoder, 2/0 with coupling, spectral extension and AHT, and 2/0 §E3.5 enhanced coupling - six frames of synthesised programme each, byte count and FNV-1a hash checked against `firmware/baremetal/encode_fixture.hpp`, peak heap per row. One substream at a time; see [Encoding](#encoding) for what does not fit |
| AC-4 decode | **Correct on the board and under QEMU, with its state in PSRAM.** The six fixtures of the AC-4 probe (2.0, 5.1 and 5.1.4, with A-CPL and companding) equal the pins the Cortex-M3 leg and the host are held to in CI, and on a board with Wi-Fi up all twenty plays of the P4's table (2.0, 5.1, 5.1.4 and the four frame rates) give the P4's PCM hashes. **Real time only for 2.0 in SIMPLE mode** (0.87 of a frame) as D14c measured it; 2.0 in A-SPX mode took 1.03 to 1.09, 5.1 2.1 to 3.2 and 5.1.4 4.4 to 5.6. Since D14g (2026-10-11) the decoder's second core, and QIO flash, 64-byte data-cache lines, the code in PSRAM and a 64 KB data cache (`sdkconfig.s3-fast`, `sdkconfig.s3-dcache`), take 2.0 to 0.40 (SIMPLE) and 0.50 (A-SPX), the converter at every frame rate to 0.67 to 0.87, E-AC-3 7.1.4 to 0.75 and 5.1 in SIMPLE mode to 0.99, with the PCM unchanged (D14g; D14h's single precision A-CPL interpolation moved the PCM of streams with A-CPL and took 5.1 in A-CPL mode 3 to 1.44); in an image built without those two files, with the second core, 2.0 SIMPLE takes 0.80 and A-SPX 0.99 ([Playback speed](#playback-speed)). The decode task's stack is in PSRAM: with Wi-Fi up no internal block is over 31,744 bytes and a 5.1 A-CPL play uses 32,560. See [AC-4](#ac-4) (phase D14c of [`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#d14-ac-4-on-the-esp32s)). No ESP32 sink takes AC-4 in a Sendspin group |
| Standalone probe fits internal SRAM | Yes, without PSRAM. 195,025-byte peak heap (`eac3_atmos_render`; 194,655 with Atmos objects reconstructed, 173,794 for the 7.1.4 fixture folded to stereo, 167,386 as coded) against 304,680 free under QEMU on 2026-09-29. The board reported 316,196 free on 2026-09-11, when the peak was 237,206 — see [Memory](#memory) |
| Retained after teardown | 12 bytes, one `__cxa_thread_atexit` record, the spectrum scratch's pointer; 23,552 bytes while §E3.5 is in use |
| Audio output | Two examples drive real peripherals — see [Examples](#examples) |
| Sendspin sink | `hearth_sink` requires an ESP32-S3 board with 8 MB of PSRAM and plays as a Sendspin player on Wi-Fi. Two boards played one E-AC-3 JOC programme as a group for ten minutes with no underrun and their play times within 549 µs — see [As a Sendspin sink](#as-a-sendspin-sink) and [the sink guide](../../hearth/sink-esp32-s3.md) |
| Real time | **Decode, yes, on a board**, at 240 MHz, every one of the fourteen fixtures: from 0.07x for AC-3 mono to 0.92x for E-AC-3 7.1.4 folded to stereo, with objects placed onto 7.1.4 at 0.78x — see [Timing](#timing). The probe's board timings on this page are those of 2026-09-09 to 2026-09-11. **Encode: AC-3 2/0 and E-AC-3 2/0, yes**, 0.35x and 0.73x with the encoders in `float` end to end and the search made cheaper; AC-3 5.1 at 1.01x sits at the line, 2/0 with tools 1.3x to 1.6x and E-AC-3 5.1 1.7x over, what remains being the exponent-run planner and the allocation candidates — see [Encoding](#encoding) |
| ESPHome | An external component, `firmware/esphome/components/iclforge/` — an AC-3 decoder and framer, not a `speaker` source. See [ESPHome](esphome.md) |
| CI | `build-esp32s3` in `.github/workflows/_build.yml` under QEMU (the decode, encode and AC-4 probes and the example's shapes), and `hearth-esp32s3` after it, which plays to the Sendspin sink from the host; `esphome config` and the component pack in `esp-component.yml`. All are in the `esp` lane of `ci.yml`, which runs after a merge to main that changes the ESP32 trees or a tree its component ships (the [lane table](../../ci-lanes.md#lane-table) lists them), and nightly ([CI for many agents](../../ci-agentic.md#the-tiers)); a pull request's gate builds none of them |

Decode and encode are separate builds. They are mutually exclusive, and configure fails if both
are asked for, because neither fits beside the other in this memory.

## What the part can and cannot do

Everything the library does, against what this part has been shown to do with it. "Board" is
the ESP32-S3-DevKitC-1 at 240 MHz, 2026-09-09 and 2026-09-10, plain builds; the two emulated legs
agree with it and with the host to the digit on levels and hashes but say nothing about time on
this part. The x figures are fractions of a 32 ms frame.

| | On this part | How that is known |
|---|---|---|
| AC-3 decode, 1/0, 2/0, 3/2 + LFE, coupled and not, §7.5.4 rematrixing | Yes, real time: 0.07x, 0.11x, 0.31x | Board; `ac3_mono`, `ac3_stereo`, `ac3` |
| E-AC-3 decode, 2/0 and 5.1, with AHT, spectral extension, standard coupling | Yes, real time: 0.17x, 0.34x | Board; `eac3_stereo`, `eac3` |
| E-AC-3 §E3.5 enhanced coupling | Yes, real time: 0.62x | Board; `eac3_ecpl` |
| E-AC-3 7.1.4, a bed and two dependent substreams | Yes, real time: 0.90x | Board; `eac3_714` |
| The §7.8 output stage: dialnorm, Lo/Ro, Lt/Rt and mono folds, line and RF modes | Yes, real time: 0.28x folding AC-3 5.1, 0.37x folding E-AC-3 5.1, 0.92x folding E-AC-3 7.1.4; line mode with dynrng words and dialnorm 24, 0.36x at 5.1 | Board; `ac3_fold`, `eac3_fold`, `eac3_714_fold`, `eac3_line` |
| Atmos bed, objects skipped | Yes, real time: 0.28x | Board; `eac3_atmos_bed` |
| Atmos objects reconstructed, JOC in the MDCT-band domain | Yes, real time: 0.66x | Board; `eac3_atmos_objects` |
| Objects placed onto loudspeakers by their OAMD positions, 7.1.4 with heights, through the block form | Yes, real time: 0.78x, the render 3.3 ms of the 25.1; 195,025 bytes of peak | Board for the time, QEMU leg for the peak; `eac3_atmos_render` |
| JOC in the QMF domain (`Domain::kQmf`, the licensed decoders' domain) | No, in internal SRAM: 449,826 bytes of peak as first measured, in `double`. The reconstruction alone is about 233 KB of heap in `float`, which needs PSRAM | Host measurement, see [Objects](#objects); the example's README for the 233 KB |
| The full TS 103 420 §4.3 renderer (extents, zones, snap) | No: the pan is a point source per object | Not attempted; the extent metadata arrives and is not read |
| §3.7 transient pre-noise processing, concealment | Compiled in; no probe fixture uses either. The stream set's `51-tpn.ec3` plays the first under QEMU in CI with its levels held | Not timed |
| The direct-form reference transform | No, by design: `DecodeError::kNoReferenceTransform` | Every leg checks the refusal |
| AC-3 encode, 2/0 and 5.1 | Byte-exact with the host; **2/0 in real time at 0.35x**, 5.1 at the line (1.01x), the encoder `float` end to end and the search integer | Board; `ac3_stereo`, `ac3` |
| E-AC-3 encode, 2/0 plain, 2/0 with coupling + spectral extension + AHT, 2/0 §E3.5, 5.1 with no tool | Byte-exact; **2/0 plain in real time at 0.73x**, the others 1.6x, 1.3x and 1.7x over, the arithmetic `float` and the remaining cost the exponent-run planner, the allocation candidates and the `double` AHT | Board; `eac3_stereo`, `eac3_tools`, `eac3_ecpl`, `eac3` |
| E-AC-3 5.1 encode with AHT or coupling, or §E3.5 at 5.1 | No: 289,202 to 369,790 bytes of peak, as first measured | Host profile, see [Encoding](#encoding) |
| Any dependent-substream encode (7.1, 5.1.2, 5.1.4, 7.1.4) | No: three encoders resident, 601,954 bytes for 7.1.4, as first measured | Host profile |
| The Atmos object encoder | No: about 300 KB, `double`, and not in the profile | Bench estimate, see [Encoding](#encoding) |
| Decode and encode in one image | No: mutually exclusive builds | Measured, above |
| The second core, PSRAM | Not used by the AC-3 and E-AC-3 probe. The AC-4 probe puts the decoder's state in PSRAM. The Hearth sink uses both: its decode runs on core 1 and its large allocations go to PSRAM | [In the Sendspin sink](#in-the-sendspin-sink); [AC-4](#ac-4); [What is left](#what-is-left-and-what-would-move-it) |
| AC-4 decode, 2.0, 5.1 and 5.1.4 | Correct, with the decoder's state and the decode task's stack in PSRAM; real time at 2.0 in SIMPLE mode only (0.87) as D14c measured it; D14g's second core and memory files (`sdkconfig.s3-fast`, `sdkconfig.s3-dcache`) add 2.0 A-SPX, the converter, E-AC-3 7.1.4 and 5.1 SIMPLE ([Playback speed](#playback-speed)) | Board, `hearth_sink` with Wi-Fi up; QEMU leg, `run_esp32s3_probe.sh --ac4`; [AC-4](#ac-4) |

## Building

ESP-IDF owns the top-level build, as Gradle does for [Android](../android.md), so there is no
ICL Forge preset for this target and no entry in `cmake/toolchains/`.

### The ESP-IDF component

[`firmware/esp-idf/iclforge/`](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/esp-idf/iclforge/README.md)
is the profile packaged as a component. A project outside this repository
builds against it in two lines, without vendoring the source list:

```cmake
set(EXTRA_COMPONENT_DIRS "/path/to/iclforge/firmware/esp-idf")
set(ICLFORGE_ESP_PROFILE "decoder")   # or "encoder"
```

The component pre-seeds the root's `option()`s and `add_subdirectory()`s the repo root, the same
shape `apps/demos/android/app/src/main/cpp/CMakeLists.txt` uses. Re-listing
`libs/ac3/minimal.cmake`'s sources in an `idf_component_register(SRCS ...)` was rejected: two
copies of a source list drift, and the drift surfaces as a link error rather than a diff.

The decode arithmetic follows the part unless the project chooses: `float` where ESP-IDF's
`SOC_CPU_HAS_FPU` capability says the part has a floating-point unit, as this one does, and the
fixed-point tier where it has none, as on an [ESP32-C3](esp32-c3.md). To build the other one,
set `ICLFORGE_DECODE_SCALAR` to `float` or `fixed` above `project()`, or pass
`-DICLFORGE_DECODE_SCALAR=...` to `idf.py`.

`idf_component.yml` carries registry metadata and the list of targets, each of which
`tools/packaging/pack_esp_component.py --verify` builds against the packed archive. **It is not
published to the ESP Component Registry.** `.github/workflows/esp-component.yml` lints the
manifest and packs the archive, called from the `esp` lane of `ci.yml`, which runs after a merge to
main that changes the ESP32 trees or a tree its component ships, and nightly
([the lane table](../../ci-lanes.md#lane-table)). Its `compote component upload` job is gated to a
manual `workflow_dispatch` on a `v` tag — a published version cannot be replaced, so the upload is
a decision rather than a consequence of merging. Add `--with-ac4` to the packer and the archive
also carries the AC-4 decoder's sources, for `CONFIG_ICLFORGE_AC4`
([ESP32-P4](esp32-p4.md#building-with-ac-4)).

### The probes

`firmware/baremetal/platform/esp32s3/` is the footprint harness, and points `EXTRA_COMPONENT_DIRS` at
`firmware/esp-idf/`:

```bash
. $IDF_PATH/export.sh
cd firmware/baremetal/platform/esp32s3
idf.py set-target esp32s3
idf.py build
idf.py qemu                    # no board needed
idf.py -p <PORT> flash monitor # a real board
```

`tools/checks/run_esp32s3_probe.sh` drives that under QEMU and gates on the results;
`--encoder` runs the encode direction instead.

`firmware/baremetal/platform/esp32c3/` is the same harness for the ESP32-C3, which has no
floating-point unit and therefore decodes in the fixed-point tier instead — see
[ESP32-C3 → Building](esp32-c3.md#building) for that leg's setup and gate.

Verified against ESP-IDF v6.1.0, which ships Xtensa GCC 15.2.0 and defaults to `-std=gnu++26`.
The library's C++23 use — `std::expected`, `std::unreachable`, `std::byteswap`, `constexpr
std::vector` — compiles under `-fno-exceptions -fno-rtti`. The libstdc++ problem in
[esp-idf#18172](https://github.com/espressif/esp-idf/issues/18172) is specific to GCC 14.2 and
does not apply.

## Examples

`i2s_player` lives under `firmware/esp-idf/iclforge/examples/` and the Hearth sink is a project of its own,
`firmware/hearth-sink/`; CI builds both.

### I2S player

`i2s_player` decodes the AC-3 5.1 fixture linked into its own image, folds it to stereo through
the decoder's §7.8 output stage, and writes it to an I2S DAC at 48 kHz, 16-bit, on a loop. It
proves the codec works; it is not how anything real gets its audio.

Three GPIOs under `iclforge I2S player` in `idf.py menuconfig`, defaulting to BCLK 5, WS 6,
DOUT 7 — chosen to avoid the strapping pins, the USB pair and the console UART. No MCLK is
configured, so a DAC needing one has to have it added. Written against a MAX98357A and a PCM5102.

### Hearth sink

`hearth_sink` decodes AC-3 and E-AC-3 from a flash partition, an SD card, a FAT volume in flash
or an HTTP body. It reads the stream a piece at a time, through a ring between the player's fetch
and decode tasks (32 KB by default) and a 16 KB framing buffer. Where bytes come from and where
audio goes are directories CMake picks, not flags the player branches on — the player itself names
neither a partition nor I2S:

| Source | Sink |
|---|---|
| `partition` — flash (default) | `i2s` — an I2S DAC (default): standard I2S for one or two channels and TDM for three or more, in 32-bit or 16-bit slots, master or slave, on one line or two |
| `sd` — SD card over SDMMC | `i2s_wide` — the ESP32-P4's: one line with a 512-bit TDM frame, which the S3 does not have |
| `fatfs` — a FAT volume in flash | `capture` — converts and checks; what CI runs |
| `http` — an HTTP body over WiFi | `null` — counts blocks |

Chosen under *iclforge hearth sink* in `idf.py menuconfig`, along with the output layout — a
name such as `5.1.4` or a speaker list — that the player renders every stream onto
(`libs/render/include/iclforge/render/layout.hpp`, `render.hpp`). The `i2s` sink chooses standard or TDM
mode from the layout in force (`iclforge/sink_plan.hpp`) and reconfigures between plays, so
`PUT /layout` needs no rebuild. One S3 line carries four 32-bit or eight 16-bit slots and a second
line doubles that, to sixteen 16-bit slots ([Slot widths](../../hearth/sink-esp32-s3.md#slot-widths)).
The example decodes AC-3 and E-AC-3 here; its AC-4 decoder is a `CONFIG_ICLFORGE_AC4` build that
builds for this part with an AC-4 play's state and stack in PSRAM ([AC-4](#ac-4)) and that this
part and the [ESP32-P4](esp32-p4.md#ac-4) have played on a board.

It exists to exercise the incremental input path. `iclforge::ac3::split_frames` takes a span over a whole
stream, which nothing streaming can produce; `iclforge::ac3::io::AccessUnitAccumulator` applies the same
boundary rule over a caller-owned buffer, allocating nothing. It hands the decoder access units
rather than syncframes, because `decode_access_unit_by_block` wants an independent substream together
with the dependents that extend it (§E3.8.2).

CI runs the example under QEMU in seven shapes, each a step of `build-esp32s3` in
`.github/workflows/_build.yml` with its own overlay on `sdkconfig.defaults`. All seven write to the
`capture` sink, since QEMU has no I2S peripheral. The capture sink calls the same conversion
functions as the `i2s` sink (`libs/device/include/iclforge/interleave.hpp`) and checks what
they produce:

- `sdkconfig.ci`: the `partition` source and the AC-3 5.1 sample, folded to Lo/Ro. Two passes, so
  the rewind at the end of the stream runs as well.
- `sdkconfig.ci-tdm`: the `fatfs` source, into the TDM conversion on an 8-slot bus. QEMU has no SD
  host, so a FAT volume in flash stands in for the card. The `sd` and `fatfs` sources share their
  file code (`main/source/file_common.hpp`) and differ only in the mount, so the part of `sd` that
  CI does not run is its mount over the SDMMC host. The capture sink checks the integers: every
  sample left-justified 24-in-32, and zeros in the six slots a two-channel programme does not fill.
- `sdkconfig.ci-tdm` with `sdkconfig.ci-tdm16`: the same at 16-bit slots, the eight a TDM line
  carries at that width.
- `sdkconfig.ci-tdm` with `sdkconfig.ci-tdm916`: sixteen 16-bit slots, two lines of eight, onto a
  9.1.6 layout, the widest frame the part reaches. It holds the planner's two-line split and the
  16-bit interleave across all sixteen slots, with the six height slots silent because the stream
  has nothing for them.
- `sdkconfig.ci-render`: the `fatfs` source playing the probe's height-object fixture onto
  `7.1.4`, its objects reconstructed and placed, into twelve TDM slots. Each slot's level has to be
  within one unit of the probe's `eac3_atmos_render` row
  ([Placed on loudspeakers](#placed-on-loudspeakers)).
- `sdkconfig.ci-http`: the `http` source over QEMU's OpenCores Ethernet MAC in place of WiFi
  (`main/net/openeth/`), fetching the E-AC-3 demo stream (`apps/demos/wasm/assets/demo.ec3`)
  from a server on the runner; the guest is 10.0.2.15 and the host 10.0.2.2. The same step drives
  the control surface through a port forward: `GET /status`, `POST /volume` with 0.5, a replay
  through `POST /play` whose levels must come out at half, and `POST /stop`. A further step drives
  the board's web page against the same build with Playwright.
- `sdkconfig.ci-http` with `sdkconfig.ci-http714`: the [stream set](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/www/README.md)
  over the same network onto 7.1.4 in twelve slots, one `POST /play` a stream, each slot's level
  held to `www/streams.json` (`tools/checks/check_stream_set.py`); a stream the manifest marks
  refused has to fail for the reason it names.

Another step, *Build every sink and source combination*, builds `i2s`, `sd`, `http` and `null`,
one build each, and runs none of them. The `i2s` sink drives the I2S peripheral and `sd` the SDMMC
host, and `http` is built with WiFi (`main/net/wifi/`); QEMU emulates none of the
three. The conversion the `i2s` sink hands the peripheral is the one the capture sink checks, and it
is unit-tested on the host (`libs/device/tests/test_interleave.cpp`), because planar-to-interleaved indexing
with slot padding is where the bugs are; so is the arithmetic that picks standard or TDM mode and
the slot count from the layout (`libs/device/tests/test_sink_plan.cpp`). A 5.1 programme on an 8-slot bus leaves two
slots that must be written as zeros rather than skipped: the DMA buffer is reused, so whatever the
previous block left is what the DAC clocks out. The queue model the `i2s` sink keeps for its
`sink.*` line (`libs/device/include/iclforge/dac_queue_model.hpp`) is unit-tested on the host
as well (`libs/device/tests/test_dac_queue_model.cpp`), against a simulated DMA. The sink itself has run on
two S3 boards, in standard mode and in TDM on eight 16-bit slots, with no DAC on the pins
([On two boards](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/README.md#on-two-boards)).

CI compares the sink's per-channel RMS against the host's answer for the same file through the
same configuration (`forge decode … downmix=loro drcmode=line`). A `result=pass` alone would be
satisfied by a stream decoding to silence.

Nor does `result=pass` say the run was clean. QEMU runs on until a timeout, the HTTP step plays the
stream a second time, and a panic at any point resets the chip into a new run that can print
`result=pass` again. So every QEMU leg, the probes included, also fails if the console shows panic
output, a second boot after the first boot banner, or a failed allocation
(`tools/checks/check_esp_console.py`). An allocation can fail without being fatal — the Ethernet
driver drops a frame and the play goes on — so a run may print hundreds and still report
`result=pass`, which is how one that did abort went unnoticed for a day.

The stream set, the widest shape the part runs, is held to a floor on free internal RAM as well:
twelve channels out of three substreams share internal RAM with the Ethernet driver and the HTTP
server, so it is where a decoder or player that starts holding more is seen first. The step passes
`--min-heap-free` to the same check, and the step's own comment records what the shape measures
and why the floor sits where it does.

#### As a Sendspin sink

`sdkconfig.sendspin`, over the hardware and PSRAM overlays, makes the example a Sendspin player on
Wi-Fi ([the Hearth plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/hearth-reference-player.md), B3). It has:

- the Noise responder;
- pairing by token, or by a code shown on the console and the page;
- Sendspin's time filter;
- `player@v1`, for Music Assistant's stereo PCM;
- Hearth's `_iclforge_player@v1`, whose AC-3 and E-AC-3 bursts the component decodes and renders
  onto the board's layout. The player advertises `ac3` and `eac3` as its data types and not `ac4`:
  no ESP32 sink takes AC-4 in a Sendspin group yet (phase I6 of
  [`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#i6-the-esp32-sinks)).

[An ESP32-S3 sink](../../hearth/sink-esp32-s3.md) sets a board up.

The player schedules each block against the I2S channel's end-of-frame interrupts. It works from a
model of ESP-IDF v6.1's DMA ring (`libs/device/include/iclforge/playout.hpp`), which
`libs/device/tests/test_playout.cpp` tests on the host against a simulated ring. It corrects its error in
the decoded PCM, by dropping or repeating one frame in 256.

On 2026-09-16 two ESP32-S3-DevKitC-1-N16R8 boards on the same Wi-Fi, with no DAC wired, played the
E-AC-3 JOC fixture for ten minutes as one group from `hearth-testserver`. Each burst holds
32 ms of audio.

| | 2.0, 32-bit standard I2S | 5.1, eight 16-bit TDM slots |
|---|---|---|
| Bursts played | 18,774 of 18,774 | 18,774 of 18,774 |
| Underruns, late, dropped | 0, 0, 0 | 0, 0, 0 |
| Decode and render per burst, average (worst) | 20.9 ms (40.3 ms) | 24.3 ms (48.8 ms) |

At each of the 602 seconds the server compared, the boards' reported play times were within
549 µs of each other. The 2.0 board's levels were the test sink's to the digit. Four things on
the boards broke playback, and no host test or QEMU run showed any of them: Wi-Fi modem sleep,
Nagle's algorithm on the player's sockets, lwIP's task on the decoder's core, and clock replies
delayed behind a stream's chunks. [The example's README](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/README.md#on-two-boards) describes each.

CI runs the same player on QEMU's Ethernet (`sdkconfig.ci-sendspin`) in `hearth-esp32s3`, a job
that runs after `build-esp32s3`. `tools/checks/run_sendspin_qemu.sh` has `hearth-testserver`
pair with the emulated board, set it to 2.0, and play it the fixture in one group with a test
sink. It then holds the board's levels to the test sink's WAV
(`tools/checks/check_sendspin_levels.py`), and its console to one clean boot and the stream set's
heap floor. The test server is a GCC 16 and vcpkg build, and Espressif's image carries neither.
So the job runs in a container of its own and takes the emulated board's image from
`build-esp32s3` as an artifact. `build-esp32s3` also builds the board shape, which QEMU cannot run.

## Memory

The datasheet says 512 KB, `idf.py size` says 341,760, and the allocator says 304,680 in the QEMU
leg's run of 2026-09-29. All three are true and answer different questions.

| | Bytes | |
|---|---|---|
| Physical SRAM | 524,288 | the datasheet's 512 KB |
| − IRAM/Icache block | 32,768 | 0x40370000 to 0x40378000, on the instruction bus only: the instruction cache, 16,384 at `CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE`'s minimum, and 16,384 of IRAM |
| = DRAM-addressable window | 491,520 | `SOC_DRAM_LOW`…`SOC_DRAM_HIGH` |
| of which data cache | 32,768 | `CONFIG_ESP32S3_DATA_CACHE_SIZE`, taken from the top of the window |
| DIRAM pool `idf.py size` reports | 341,760 | after ROM reservations |
| **free at runtime** | **304,680** | what `heap_caps_get_free_size` returns before the probe decodes, under QEMU on 2026-09-29; a board read 316,196 on 2026-09-11 |

`idf.py size`'s "remain" is a linker estimate — 239,164 where the allocator reports 304,680. A
footprint budget quoted from it is a budget nobody checked.

Measured, decode direction, in the QEMU leg's run of 2026-09-29: the image uses 102,596 bytes of
DIRAM and the decode peaks at 195,025 bytes of heap, on the height-object fixture placed onto
7.1.4 (194,655 with Atmos objects reconstructed, 173,794 for the 7.1.4 fixture folded to stereo,
167,386 as coded). The peaks and allocation counts are the Cortex-M3 leg's to the byte, and the
[ESP32-P4](esp32-p4.md#memory)'s. The encode image uses 108,452 bytes of DIRAM and peaks at
158,911. `app_main` prints the runtime figures before and after.

### Contiguity

| | Free | Largest block |
|---|---|---|
| Before the decode | 304,680 | 241,664 |
| After it | 304,432 | 155,648 |

The total is not an allocation budget on this part: the heap is regioned, and the largest
contiguous run is what decides whether a large allocation succeeds. That distinction is what made
object reconstruction fail here while the same build passed on `arm-none-eabi`, whose newlib heap
is flat.

### IRAM and the caches

ESP-IDF donates unfilled IRAM to the heap as 32-bit-access-only memory, which `malloc` never
returns. Measured, that pool is 0 bytes — `idf.py size` reports IRAM as 16,384 of 16,384 used, so
there is nothing to donate. The instruction cache is already at its 16 KB minimum; the data cache
could go 32 KB → 16 KB and return 16,384 bytes, at the cost of slower reads from flash-resident
fixtures.

### Stack

The decode runs on the main task. `uxTaskGetStackHighWaterMark` left 11,280 bytes free of the
original 32,768 `sdkconfig.defaults` set, until PR #698 (legacy-core downmix levels) grew
`DecodedSubstream`/`DecodedAccessUnit` by `bsid`/`cmixlev`/`surmixlev`/`alternate_bsi` and the
runner measured that down to 8,096 — 96 bytes under its 8,192 floor.
`sdkconfig.defaults` now sets 40,960, which left 16,064. The decoder has since stopped keeping
extra copies of those two structs on the stack (it builds its results in place), and the decode
leaves 19,216 of the 40,960 in the QEMU leg's run of 2026-09-29: 11,024 in terms of the original
32,768, close to the figure before PR #698. The encode direction, less affected, left 32,768 of the
40,960 free in the same run. The decode margin is the one to watch — it was 14,000 before object
reconstruction ran here, then 11,280 before PR #698.

### In the Sendspin sink

The Sendspin sink is the tightest shape on a board. Wi-Fi, lwIP, two HTTP servers, mDNS, the
Sendspin session and the E-AC-3 decoder share internal RAM, and `sdkconfig.psram` keeps every
allocation under 16 KB there. These are the heap monitor API's figures while a stream played: the
ten-minute group run on the boards, and 315 bursts under QEMU.

| | QEMU (no PSRAM, 16 KB ring) | 2.0 board | 5.1 board |
|---|---|---|---|
| Least internal heap free | 43,700 bytes | 139 bytes | 23 bytes |
| Decode task's stack unused | 5,744 of 24,576 | 13,916 of 32,768 | 13,840 of 32,768 |
| Sendspin server's stack unused, of 8,192 | 2,972 | 2,960 | 2,960 |

Nothing failed in those ten minutes. In a one-minute run before them, one 108-byte internal
allocation did, with no effect on the stream. Two changes were tried and not kept:

- Sending allocations over 4 KB to PSRAM left 80 KB free, but a burst then took 28 ms where it
  had taken 20, and the board underran.
- A larger `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` changed nothing, because ESP-IDF v6.1 lets
  ordinary small allocations take that reserve.

Under QEMU, CI holds the shape to at least 20,480 bytes free, the stream set's floor.

## Timing

The probe reports `decode_us`, `us_per_frame` and `realtime_permille` per codec, and both example
players report per-frame timing of their own. A frame is 1,536 samples at 48 kHz, so the budget
is 32,000 microseconds and `realtime_permille` is 1000 at exactly real time.

Under `idf.py qemu` these figures do not describe hardware: QEMU is not cycle-accurate and
reports `cpu_mhz=40` against its own boot log's 160 MHz, so treat the QEMU leg as a correctness
and footprint check only; under the Hearth sink's `null` output the figure means less again,
since nothing paces the loop. The figures that follow are from a board, on 2026-09-09 to
2026-09-11. The decoder has changed since (its peak heap and its allocations a frame have fallen:
see [Memory](#memory) and [7.1.4, the widest programme](#714-the-widest-programme)) and this page
carries no later probe timing.

### Measured, on an ESP32-S3-DevKitC-1-N16R8

2026-09-09, chip revision v0.2, 240 MHz, PSRAM off, `-Os`. Every fixture
decoded to its expected levels - `result=pass`, every channel's RMS matching its
reference to the digit - so what follows is about speed alone.

As found, before any of the work below:

| Fixture | us/frame | x real time | |
|---|---:|---:|---|
| `ac3_mono` | 5,180 | 0.16 | fits |
| `ac3_stereo` | 14,290 | 0.45 | fits |
| `eac3_atmos_bed` | 27,197 | 0.85 | fits |
| `ac3` 5.1 | 32,149 | 1.00 | at the line |
| `eac3_stereo` | 34,739 | 1.08 | misses by 8% |
| `eac3` 5.1 | 78,824 | 2.46 | no |
| `eac3_atmos_objects` | 82,711 | 2.58 | no |
| `eac3_ecpl` | 217,493 | 6.80 | no |

The same build at 160 MHz - the clock this project inherited by never setting
`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`, until 2026-09-09 - ran 1.44x to 1.49x slower
across all eight fixtures, against an ideal ratio of 1.50. That near-linear
scaling says the decode is compute-bound, not stalled on the flash cache, so
configuration had nothing further to give; anything more had to come out of the
code. (The `ac3` row is from the stage-timed run described next, whose markers
cost it about 0.2 ms; the other seven are from a plain build.)

The instruction cache stayed at its default 16 KB for all of this, and in the
probe that held. In the Hearth sink's network shape, where WiFi and the
rest of the player run beside the decoder, it did not: at 32 KB a 7.1.4 frame
decoded 3.1 ms faster folded to 2.0 and 3.8 ms faster onto twelve slots
([7.1.4 in real time](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-714-realtime.md)).
The probe has not been measured at 32 KB.

### Where the time went

Nothing in the table says which stage is slow, and the estimates that had been
made about it were wrong. So before changing anything the decoder was
profiled on the board, through the `ICLFORGE_ZONE_SCOPED_N()` markers it already
carries for Tracy: `-DICLFORGE_STAGE_TIMERS=ON` routes them to an accumulator
in the probe (`firmware/baremetal/stage_timers.cpp`, the application half of
`libs/base/variants/profiling-stage_timers/`) and each fixture then prints a
`<fixture>.stage[<zone>]` line per stage with its inclusive and self time per
frame. A pair of markers costs 2.4 us on this part, and a frame passes through
at most 99 of them, so the breakdown carries under 0.25 ms of its own weight -
the stage-timed totals sit within 0.3% of the plain ones above.

Self time per frame, microseconds, as found:

| Stage | `eac3` 5.1 | `eac3_stereo` | `eac3_atmos_bed` | `ac3` 5.1 |
|---|---:|---:|---:|---:|
| spectral extension synthesis (`eac3_spx`) | 45,060 | 19,093 | - | - |
| AHT dequantisation and inverse (`eac3_aht`) | 20,723 | 8,489 | - | - |
| mantissa read and dequantisation | 1,194 | 1,596 | 14,006 | 14,980 |
| decoupling | - | - | - | 4,904 |
| bit allocation (`compute_bit_allocation`) | 3,704 | 1,416 | 4,559 | 4,350 |
| IMDCT and overlap-add | 3,403 | 1,228 | 3,401 | 4,847 |
| everything else | 4,983 | 3,082 | 5,591 | 3,043 |
| total | 79,067 | 34,904 | 27,557 | 32,124 |

The transform - the stage the earlier estimates had put first, and the one the
hand-written kernel tier would have targeted - is 4% of a 5.1 decode. Spectral
extension is 57%, and it is not arithmetic: an extension region is roughly
150 bins in each of five channels in each of six blocks, 4,500 bins a frame,
and they were costing 10 us each, 2,400 cycles a bin for a copy, a notch and a
two-term blend.

The cycles were going into the ROM. This part's FPU is single-precision, and
every `double` operation compiles to a call into the mask ROM's software
routines - `__muldf3`, `__divdf3` and the rest, some hundreds of cycles each,
with `__divdf3` the worst. The coefficient store had moved to `float` under
this profile (`decode_scalar_t`), but the arithmetic between the bitstream and
that store had not: the mantissa dequantiser divided in `double` and then
divided again by 2^exp, the dither and spectral-extension noise generators
mapped their state in `double`, the coupling coordinates came out of
`std::ldexp`, decoupling multiplied through `double`, the AHT's six-by-six
inverse ran thirty-six `double` multiply-adds per bin, spectral extension's blend
and band energies were `double` throughout, and JOC's object mixing summed
`double`s over a `float` state. A static census of the linked image found 1,197
call sites into those routines. None of it is visible on a desktop, where
`double` costs what `float` costs, or on the Cortex-M3 leg, where everything is
software floating point alike. It was visible only here.

### What changed

The arithmetic between bitstream and coefficient store now runs in the store's
own type, `iclforge::ac3::internal::decode_scalar_t`: `float` under this profile, `double`
in every other build, which is why no gold reference, bitstream hash or test
moved (the full Windows suite passes as before, 1,345 tests). The exported
`double` functions - `dequantize_mantissa`, `decode_coordinate`,
`spx_noise_ratio`, `aht_dequantize_mantissa`, `DitherGenerator::next`,
`SpxNoise::next` - are now the `<double>` instantiations of templates the
decoders call at their own scalar; `spx_attenuation` became a 96-entry table
filled once from the same `std::exp2`; `aht_inverse` gained a `float` overload
over the double kernel narrowed once; and the division by 2^exp everywhere
became a multiply by a table of exact powers of two, which is the same value in
either type. JOC's mixing follows `decode_scalar_t` too, narrowing the
frame's matrix once rather than at every read, and the float analysis window
it runs thirty times a frame stopped narrowing its 512 constants per sample.

Where the float result is the double one narrowed, and where it is not, is
stated at each site. Mantissas, coordinates and decoupling are bit-identical
to before on this profile - a small integer over a power of two rounds the
same way in either type. Spectral extension, the AHT inverse, the noise
generators and the JOC sum round in `float` now, which is the same class of
difference the float store already accepted at the transform; the probe's
RMS figures, printed to six digits, did not move on any of the eight fixtures.

The second lever is the optimiser. This profile compiles at `-Os`, and a board
run with the decode-critical sources at `-O2` (`ICLFORGE_MINIMAL_HOT_O2`, on
for this project, off in the profile's default) showed which files it pays for
and which it does not: bit allocation 4.55 to 2.09 ms and the JOC mixing 11.0
to 8.2 ms per frame, against nothing at all for the float32 IMDCT, which ran
in 3.40 ms either way. The five files it pays for cost 39,192 bytes of
flash code and no internal SRAM; `.bss`, `.data` and the DIRAM figure the
runner gates are unchanged.

After both, plain build, same board, same clock:

| Fixture | us/frame | x real time | was |
|---|---:|---:|---:|
| `ac3_mono` | 2,446 | 0.08 | 0.16 |
| `ac3_stereo` | 4,157 | 0.13 | 0.45 |
| `eac3_stereo` | 6,803 | 0.21 | 1.08 |
| `ac3` 5.1 | 11,772 | 0.37 | 1.00 |
| `eac3_atmos_bed` | 13,409 | 0.42 | 0.85 |
| `eac3` 5.1 | 14,169 | 0.44 | 2.46 |
| `eac3_atmos_objects` | 29,359 | 0.92 | 2.58 |
| `eac3_ecpl` | 23,784 | 0.74 | 6.80 |

Every E-AC-3 configuration this profile decodes now runs in real time on this
part, with the Atmos objects fixture the closest to the line. Where a 5.1
frame's time goes now, stage-timed: bit allocation 1.66 ms, the IMDCT 3.38,
the AHT 2.98, spectral extension 1.49, mantissas 0.45, and 2.3 ms at the
access-unit level outside every marker.

Enhanced coupling came last, on its own, because its routines are shared with
the encoder and the first pass stopped at that boundary. It was the same
disease at a larger scale - of 202 ms, 107 were the per-channel reconstruction
and 83 `ecpl_channel_spectrum`, all `double`: three double inverse transforms
and a double 512-point DFT per block, then `std::cos` and `std::sin` per bin of
every coupled channel, each a software routine of a thousand cycles or so on
this FPU. The §3.5.5 routines now exist in both scalars, the double forms
being the encoder's and the exported ones as before; the float forms run the
float inverses and a float `dft512`, take their sine and cosine from a short
series held to libm at float precision (`libs/ac3/tests/encoder/test_enhanced_coupling.cpp`
pins every float form against its double one), and write into the decoder's
store directly instead of round-tripping 512 conversions per channel per
block. Stage-timed, the spectrum is 6.9 ms a frame and the reconstruction
4.7; the fixture decodes in 23.8 ms. The float scratch is 23,552 bytes against
the double one's 32,768, and the bin-angle vector that used to be `thread_local`
is a stack array, so what stays retained after the probe hands the scratch
back is one registration record, 12 bytes, rather than two.

A third pass took the stages the profile left largest, every change of it
producing the values its predecessor produced: the host suite passes
unchanged and the probe's levels are to the digit. `BitReader::read()` had
been one loop iteration per bit - some eight cycles for each bit of every
mantissa, exponent group and GAQ codeword - and now serves a field from a
64-bit cache. A block whose exponents, allocation parameters and region are
its predecessor's keeps the allocation it already has rather than deriving it
again, which E-AC-3's once-a-frame exponents make the common case:
`compute_bit_allocation` ran 36 times a frame on the 5.1 stream and runs
6 now. The symmetric mantissa quantisers' values come from a table
filled at compile time by the division they used to perform, the asymmetric
ones scale by an exact power of two, and an AHT bin resolves its dequantiser's
constants once for its six codewords. JOC's mixing reads each (channel,
band)'s data points once per object per block and forms the ramp's fractions
once per block, and `fft.cpp` joined the `-O2` list once the float DFT was on
the hot path.

Same board, same clock, plain build:

| Fixture | us/frame | x real time | was |
|---|---:|---:|---:|
| `ac3_mono` | 2,188 | 0.07 | 2,588 |
| `ac3_stereo` | 3,420 | 0.11 | 4,236 |
| `eac3_stereo` | 6,171 | 0.19 | 6,814 |
| `ac3` 5.1 | 9,939 | 0.31 | 11,803 |
| `eac3_atmos_bed` | 10,914 | 0.34 | 13,416 |
| `eac3` 5.1 | 12,775 | 0.40 | 14,185 |
| `eac3_atmos_objects` | 23,191 | 0.72 | 29,337 |
| `eac3_ecpl` | 21,547 | 0.67 | 23,784 |

A 5.1 frame, stage-timed, now: bit allocation 0.4 ms (it was 1.6), the IMDCT
3.5, the AHT 2.7, spectral extension 1.5, mantissas 0.3. The access-unit
level, which the earlier profile could only report as 2.3 ms outside every
marker, is now four zones: 2.0 ms assembling the unit from its queued
substreams (`eac3_au_assemble`), 0.1 keying them (`eac3_au_key`), and
splitting and queueing under 0.05 between them. The assembly is the largest
cost the profile then named outside the decoders - at 36 KB of output PCM a
frame it was some 50 cycles a sample - and was the next thing read.

Reading it found a copy. `std::copy` of each channel's samples into the
caller's spans lowers to `memmove`, and on this part that is a mask-ROM
routine which measured some twelve cycles a byte, where the ROM's `memcpy` -
the call every fixed-size copy in the decoders already reaches - moves the
same 36 KB in 0.12 ms. The two ranges never overlap, so it is `memcpy` now,
and `eac3_au_assemble` is 0.25 ms, of which the copy (`eac3_au_pcm`) is
0.12. The same pass stopped copying a substream's object description into
the access unit - the substream is consumed there, so it is moved - which is
where the objects fixture's peak heap fell from 234,803 bytes to 210,203 and
its allocations a frame from 41 to 31, the bed's from 23 to 20. Every level
is unchanged to the digit, on the board and on the host suite.

Same board, same clock, plain build:

| Fixture | us/frame | x real time | was |
|---|---:|---:|---:|
| `ac3_mono` | 2,229 | 0.07 | 2,188 |
| `ac3_stereo` | 3,476 | 0.11 | 3,420 |
| `eac3_stereo` | 5,550 | 0.17 | 6,171 |
| `ac3` 5.1 | 9,963 | 0.31 | 9,939 |
| `eac3_atmos_bed` | 9,066 | 0.28 | 10,914 |
| `eac3` 5.1 | 10,988 | 0.34 | 12,775 |
| `eac3_atmos_objects` | 21,199 | 0.66 | 23,191 |
| `eac3_ecpl` | 19,768 | 0.62 | 21,547 |

A 5.1 frame, stage-timed, is 11.3 ms: the IMDCT 3.5, the AHT
2.7, spectral extension 1.5, bit allocation 0.4, mantissas 0.4,
and the access-unit level 0.4 in all.

### 7.1.4, the widest programme

A ninth fixture, added for the question of driving a 7.1.4 DAC from this
part: E-AC-3 7.1.4 at 640 kbit/s, a 5.1 bed and two dependent substreams
(`k71Rear`, `kTopQuad`), the widest programme the encoder makes and the first
fixture with more channels than one substream carries, so the access unit's
assembly - locations unioned, a dependent's surrounds replacing the bed's -
runs on the target for the first time. Every one of its twelve levels is
exact. Plain build, same board, same clock (the peak and the allocations are
the decoder's then; the table below has today's):

| Fixture | us/frame | x real time | peak heap | allocations/frame |
|---|---:|---:|---:|---:|
| `eac3_714` | 28,815 | 0.90 | 229,630 | 35 |

A 7.1.4 frame is 2.6 times a 5.1 frame, not 2 - the same ratio the
[instruction count](../../performance-trend.md#instructions-per-frame) gives on
the Cortex-M3 leg (33.8 M against 12.9 M), so the extra is the dependents'
own per-substream work rather than anything this part does badly. It was
also the new peak: 229,630 bytes against the 257,572 the probe left free
on the board that day, 27,942 to spare, with the probe's own PCM block at
twelve channels (73,728 bytes, static; the `_by_block` forms have since
removed it). The decoder has used less memory since. The peak by fixture,
the same on both legs, in the CI runs of 2026-09-29:

| Fixture | peak heap | allocations/frame |
|---|---:|---:|
| `ac3_mono` | 47,772 | 1 |
| `ac3_stereo` | 49,480 | 1 |
| `ac3` 5.1 | 56,597 | 3 |
| `ac3_fold` | 58,645 | 3 |
| `eac3_stereo` | 76,090 | 4 |
| `eac3` 5.1 | 102,342 | 3 |
| `eac3_line` | 102,450 | 3 |
| `eac3_atmos_bed` | 103,596 | 11 |
| `eac3_fold` | 108,730 | 3 |
| `eac3_ecpl` | 129,657 | 3 |
| `eac3_714` | 167,386 | 14 |
| `eac3_714_fold` | 173,794 | 14 |
| `eac3_atmos_objects` | 194,655 | 22 |
| `eac3_atmos_render` | 195,025 | 27 |

The AC-3 rows carry the block form's own frame since the `_by_block`
forms (10,652, 12,356 and 19,457 before them), the three fold rows and
`eac3_line` are [Folded to stereo](#folded-to-stereo)'s and the render row
is [Placed on loudspeakers](#placed-on-loudspeakers)'. The narrative sections
that follow keep the figures of the days they were measured; where one gives a peak or an
allocation count, this table has the current figure.

### Folded to stereo

The §7.8 output stage - dialnorm, the Lo/Ro, Lt/Rt and mono folds, the
Hilbert phase shift behind Lt/Rt and RF mode's overload protection - ran
its per-sample arithmetic in `double` until 2026-09-10, on the same
software floating point every other stage had been moved off; it now
follows `decode_scalar_t`, with the gains and mix coefficients still
`double`. Three fixtures fold on the target: `ac3_fold`, `eac3_fold` and
`eac3_714_fold` decode the two 5.1 streams and the 7.1.4 one to Lo/Ro
stereo in line mode, every level exact on every leg. A fourth,
`eac3_line`, decodes a 5.1 stream carrying dynrng words and dialnorm 24 in
line mode without a fold, the one fixture where line mode has work to do.

#### What the fold cost

On 2026-09-10 the E-AC-3 5.1 fold cost 3.2 ms here where the Cortex-M3
leg counted 10% of the frame, and the sink folding 7.1.4 to stereo
over WiFi fell behind
([`planning/esp32-stream-set.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-stream-set.md),
"On a board"). The
output stage had no stage-timer zones of its own. With zones inside
`OutputStage::apply` and around both decoders' §7.7 gain, stage-timed on
2026-09-11 in internal SRAM, a frame of `eac3_714_fold`:

| Stage | us/frame |
|---|---:|
| seating the twelve locations into §7.8's six (`output_seat`) | 1,684 |
| the fold of the seats (`output_fold`) | 869 |
| copying the fold into the seats' first two views (`output_copy`) | 625 |
| copying those into the caller's first two channels (`output_seat_copy`) | 619 |
| the rest of the stage | 121 |
| total (`eac3_output`) | 3,920 |

Three things cost it. Both copies were `std::copy` of 6,144-byte channels,
which on this part lowers to the mask ROM's `memmove` at some twelve
cycles a byte, the routine `eac3_au_assemble` had been reaching (see
[What changed](#what-changed)). The seating and the fold ran at about 22
cycles a multiply-add: `output.cpp` was compiled at `-Os`, off the `-O2`
list, and its loops reloaded both base pointers from memory on every
sample. And the stage kept six frame-long seats and two frame-long outputs,
49 KB of heap.

Line mode was not part of it. The fixtures' streams carry no dynrng words
and dialnorm 31, and for those §7.7.1's gain and §5.4.2.8's normalisation
do no per-sample work: the 7.1.4 stream decoded as coded and in line mode
took the same time and gave the same PCM. On a stream that carries both -
film-standard dynrng words, dialnorm 24 - line mode added 1.6 ms to a
7.1.4 frame without a fold, 0.7 ms of it the gain on every channel's
coefficients and 0.6 ms the normalisation, and 1.2 ms to one folded, where
the normalisation runs on six seats rather than twelve channels.

In the PSRAM shape (`sdkconfig.psram`'s policy, allocations of 16 KB and
more from PSRAM, nothing else running) the fold cost what it did in
internal SRAM, its buffers each being under 16 KB, while the rest of a
7.1.4 frame was 2.8 ms slower.

#### What changed in the stage

The stage works through a frame 256 samples at a time: a block of each
seat, and for the acmod form a block of the two outputs, rather than
frame-long buffers. The layout form folds straight into the caller's first
two channels; the acmod form, whose outputs are also two of its inputs,
copies each finished block over them with `memcpy`. The loops run over
local pointers, four samples to a pass, and `output.cpp` is on the `-O2`
list, for 2,688 bytes of flash. Both decoders resolve the §7.7 gain once
per programme per block rather than once per channel. None of it changes a
result: each sample is its own sum, taken in the same order, and the Lt/Rt
shifter's history and RF mode's per-frame gain carry across blocks as they
did across frames. The double build's decode of 24 streams under 17 output
configurations, 408 decodes, is identical byte for byte, the float build's PCM hashes
are unchanged on the Cortex-M3 leg and on this board, and so are the fixed
tier's pinned ones.

Plain build, same board, same clock, microseconds per frame:

| Fixture | Before | After | The fold's own, before | After |
|---|---:|---:|---:|---:|
| `eac3_fold` | 14,039 | 11,691 | 3,175 | 841 |
| `eac3_714_fold` | 32,453 | 29,554 | 4,063 | 1,121 |
| `ac3_fold` | 10,058 | 8,833 | | |
| `eac3_line` | | 11,520 | | |

The fold's own is the row less its stream's unfolded row: `eac3` 10,864
and 10,850, `eac3_714` 28,390 and 28,433. 7.1.4 folded to stereo is 0.92x
real time now, 1.01x before; `eac3_line` is 670 us over `eac3`, line
mode's work on that stream. In the PSRAM shape `eac3_714_fold` went from
35,329 to 32,290 (1.10x to 1.01x), the fold's own from 4,122 to 1,150,
and what is left over the line there is the rest of the decode.
Stage-timed, a 7.1.4 fold is now 0.55 ms of seating and 0.28 of fold.
The rest of the stage, about 0.3 ms of a probe frame, is per-frame setup:
working out the fold's coefficients in `double` (0.08 ms), `dialnorm`'s
gain through `std::pow` (0.015 ms at dialnorm 31, 0.18 ms at 24), and the
stage's first-frame allocations, which the probe's six-frame rows average
in and a stream of any length does not. Keeping the coefficients and the
gain between frames that do not change them would take the first two off;
it is not done here.

`ac3_fold` is faster than `ac3` in both columns. The as-coded AC-3 row's
transform-and-overlap zone takes 1.4 ms more than the folded row's; that
is the as-coded block form's, not the fold's, and it is not explained
here.

Peak heap: `eac3_fold` 217,574 to 174,566, `ac3_fold` 68,709 to 58,469,
and `eac3_714_fold` 237,206 against 280,214 for the same fold before. It
was the probe's peak then, under the 245,000 the ESP32-S3 runner gates; the fold rows are 108,730,
58,645 and 173,794 in [the table above](#714-the-widest-programme).

#### In the Hearth sink

The same comparison through `hearth_sink`, built from `main` and from
this change and run one after the other on one board on 2026-09-11: the
network shape (`sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram` with
WiFi), the stream set's `714-walk.ec3` served over the LAN, 2.0 on the I2S
sink in line mode - the play that fell behind in
[`planning/esp32-stream-set.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-stream-set.md)'s
"On a board":

| | Before | After |
|---|---:|---:|
| Decode per frame (the lap line less render and sink) | 35,305 us | 30,004 us |
| The whole frame, x real time | 1.15 | 1.00 |
| Blocks to an empty DMA queue | 149 of 900 | 18 of 900 |
| Wall time for 4.8 s of audio | 5,527 ms | 4,787 ms |
| Task watchdog | IDLE1 starved | quiet |
| Levels | 36,190 / 36,190 | 36,190 / 36,190 |

The player gains 5.3 ms a frame where the probe gains 2.9, so the stage
cost the player more than it cost the probe; why was not measured. Its
frame-long buffers landing in PSRAM behind the cache, on a heap WiFi
shares, would account for it, and a block's worth is small enough to stay
in internal RAM. What is left at 1.00x is the 7.1.4 decode itself.

### Encoding

The encode direction has printed its time per frame since 2026-09-10, on the
same terms as the decode rows; the board runs further down are that day's. The
table is what the other two legs give in the CI runs of 2026-09-29. The peaks
are the part's (identical under QEMU and on the Cortex-M3 leg), the instruction
counts are the Cortex-M3 leg's under `--encoder --icount`, and the decode
column is the same leg's count for the same layout:

| Row | peak heap | allocations/frame | instructions/frame | the decode row's |
|---|---:|---:|---:|---:|
| `ac3_stereo` 2/0 | 52,815 | 29 | 9,136,000 | 3,550,000 |
| `eac3_stereo` 2/0 | 80,186 | 29 | 12,673,000 | 4,844,000 |
| `eac3_tools` 2/0, cpl + spx + AHT | 143,297 | 38 | 16,917,000 | - |
| `eac3_ecpl` 2/0, §E3.5 | 131,191 | 33 | 48,201,000 | 28,863,000 |
| `ac3` 5.1 | 111,026 | 51 | 24,867,000 | 10,228,000 |
| `eac3` 5.1 | 158,911 | 55 | 33,179,000 | 12,942,000 |

Between 1.7 and 2.6 times the decode's count (the counts above are with the
encoders in `float` end to end and the search work below, see the two
sections after this one), and both directions are soft float on that leg. On this part they are not: the decode path is `float`
on the FPU, and the encoders were `double` throughout when the board first
ran them - every operation a call into the mask ROM's software floating
point, the arithmetic that had a 5.1 E-AC-3 decode at 78.8 ms before its
conversion. Measured on the board on 2026-09-10, plain build, 240 MHz, every
row's bytes and hash the host's, the front end still `double`:

| Row | ms/frame | x real time |
|---|---:|---:|
| `ac3_stereo` 2/0 | 75.0 | 2.34 |
| `eac3_stereo` 2/0 | 137.2 | 4.29 |
| `eac3_tools` 2/0, cpl + spx + AHT | 144.6 | 4.52 |
| `ac3` 5.1 | 199.9 | 6.25 |
| `eac3` 5.1 | 348.9 | 10.90 |
| `eac3_ecpl` 2/0, §E3.5 | 472.1 | 14.75 |

Nothing encodes in real time on this part, not even AC-3 2/0, and the
worst row is fifteen times over. That is the decoder's pre-conversion
picture - a 5.1 E-AC-3 decode was 2.46x over before its arithmetic moved
to `float`, and 0.34x after - with a wider gap. The
[capability table](#what-the-part-can-and-cannot-do) carries these figures.

#### Where the encode time goes

The same board, the same day, the front end still `double`, built with
`ICLFORGE_STAGE_TIMERS=ON` (the encode probe reporting its stages as the
decode probe does; 2.4 us a timed pair). Self time per frame, the stages that
matter, with the whole row for scale:

| Stage | `ac3` 5.1 (201.4 ms) | `eac3` 5.1 (352.2 ms) | `eac3_ecpl` 2/0 (475.1 ms) |
|---|---:|---:|---:|
| Forward MDCT, `double` | 71.9 ms | 70.9 ms | 23.9 ms |
| Transient detection, `double` | 57.4 ms | 57.5 ms | 23.1 ms |
| `choose_delta_segments` | 14.2 ms | 50.9 ms | 21.0 ms |
| `encode_frame`'s own arithmetic, unzoned | 12.9 ms | 86.2 ms | 35.3 ms |
| Exponent run planning | - | 28.2 ms | 13.1 ms |
| §7.2 bit allocation (integer), 141 to 237 calls | 5.6 ms | 13.2 ms | 5.1 ms |
| §E3.5 channel spectrum, `double` DFT, 12 calls | - | - | 160.0 ms |
| §E3.5 band fitting, 15 calls | - | - | 104.7 ms |
| §E3.5 angle-interpolation decision | - | - | 48.6 ms |

Two stages that are nothing but `double` arithmetic on the FPU-less path -
the forward transform and the transient detector - are 64% of an AC-3 5.1
frame and 36% of an E-AC-3 one, and both have `float` counterparts that
already exist or are trivial: the decoder's `float` inverse transform of
the same size runs six channels in 3.4 ms on this board. The enhanced
coupling encoder's spectrum is the same §3.5.5 routine the decoder runs in
`float` at a fraction of a millisecond a call, here 13.3 ms a call in
`double`. Behind those, what remains is the allocation search - the delta
segments, the run planning, `encode_frame`'s own quantisation and
comparisons, several hundred integer bit-allocation calls a frame - which
is arithmetic and decisions on `double` masking curves and coefficients,
not one hot loop.

#### The front end in float

The two stages that dominated - and the block gather and analysis window in
front of them - now run in the profile's scalar
(`iclforge/ac3/detail/encode_scalar.hpp`, `float` here; [Building](../../building.md#minimum-footprint-decoder-profile)
has the axis), the coefficients widened to `double` for the rest of the
encoder, which is unchanged. The same board, the same day, plain build:

| Row | ms/frame, front end `double` | ms/frame, front end `float` | x real time now |
|---|---:|---:|---:|
| `ac3_stereo` 2/0 | 75.0 | 28.3 | **0.88** |
| `eac3_stereo` 2/0 | 137.2 | 90.4 | 2.83 |
| `eac3_tools` 2/0 | 144.6 | 97.7 | 3.05 |
| `ac3` 5.1 | 199.9 | 71.0 | 2.22 |
| `eac3` 5.1 | 348.9 | 219.7 | 6.87 |
| `eac3_ecpl` 2/0 | 472.1 | 425.5 | 13.30 |

AC-3 2/0 is in real time. Every row lost what the stage table said it would -
the E-AC-3 5.1 frame lost 129 ms, the transform's 71 and the detector's 57 -
and every row's bytes and hash are the host's, since the float front end is
the same on x86, on the Cortex-M3 leg and here. The peaks fell too: the
overlap history and the transform scratch halved, 220,608 bytes to 202,760
for E-AC-3 5.1.

What remains is the search. The same stage-timed build with the front end
in `float`, self time per frame:

| Stage | `ac3` 5.1 (72.5 ms) | `eac3` 5.1 (222.7 ms) | `eac3_ecpl` 2/0 (428.9 ms) |
|---|---:|---:|---:|
| Forward MDCT, now `float` | 5.7 ms | 4.8 ms | 1.6 ms |
| Transient detection, now `float` | 4.3 ms | 4.3 ms | 1.7 ms |
| `choose_delta_segments` | 14.2 ms | 50.7 ms | 20.5 ms |
| `encode_frame`'s own arithmetic, unzoned | 12.8 ms | 86.1 ms | 35.2 ms |
| Exponent run planning | - | 28.2 ms | 13.1 ms |
| §7.2 bit allocation (integer), 141 to 237 calls | 5.6 ms | 13.1 ms | 5.0 ms |
| Dither flags, fixed exponents, mantissa bit counts | 17.1 ms | 20.3 ms | - |
| §E3.5 channel spectrum, `double` DFT, 12 calls | - | - | 161.4 ms |
| §E3.5 band fitting, 15 calls | - | - | 104.7 ms |
| §E3.5 angle-interpolation decision | - | - | 48.5 ms |

The two converted stages went from 129 ms of an E-AC-3 5.1 frame to 9. What
an E-AC-3 5.1 frame is now is `encode_frame`'s own quantisation and
comparisons, the delta segments, the exponent run planning and several
hundred integer bit-allocation calls - arithmetic and decisions on `double`
masking curves and coefficients, and an iteration count as much as an
arithmetic cost. The enhanced coupling encoder's 429 ms is its `double`
§3.5.5 analysis, the same routine the decoder runs in `float` at a fraction
of a millisecond a call. The next section is what converting the rest of
the encoder bought.

#### The rest of the encoder in float

The same day, the remainder followed the front end into the profile's
scalar: the coefficient store itself, the coupling, spectral-extension and
enhanced-coupling analyses and their fits, the dither and delta-segment
decisions, the fixed-point conversion and the rematrix, with the §3.5.5
spectrum now the decoder's `float` form. Two things had to exist for it. The
exported functions the store feeds gained `float` forms (`to_fixed25` is a
template, `to_fixed25_block`, `accumulate_peak_exponents`,
`choose_delta_segments` and `PerceptualModel::analyse` take either scalar,
`DitherBallot` is the `double` instantiation of `BasicDitherBallot<Scalar>`),
and the float path has its own `log2` and `exp`
(`libs/base/internal/iclforge/base/arithmetic/scalar_math.hpp`) rather than libm's: this profile's
fixture hashes are checked on the x86 host, the Cortex-M3 leg and this part,
and three C libraries' `logf` do not agree in their last bit. The `double`
overloads are libm's, called as before, so the ordinary build is unchanged.
What is still `double`: the adaptive hybrid transform - its six-block DCT and
vector quantiser - and the masking model's own arithmetic. The allocation
search is integer either way.

Measured on the board on 2026-09-10, plain build, 240 MHz, every row's bytes
and hash the host's - and the same hash the float front end alone had
produced: converting everything behind it moved no fixture's stream.

| Row | ms/frame, front end `float` | ms/frame, encoder `float` | x real time now |
|---|---:|---:|---:|
| `ac3_stereo` 2/0 | 28.3 | 12.1 | **0.38** |
| `eac3_stereo` 2/0 | 90.4 | 33.8 | 1.06 |
| `ac3` 5.1 | 71.0 | 35.1 | 1.10 |
| `eac3_ecpl` 2/0 | 425.5 | 54.7 | 1.71 |
| `eac3_tools` 2/0 | 97.7 | 56.4 | 1.76 |
| `eac3` 5.1 | 219.7 | 81.2 | 2.54 |

E-AC-3 2/0 and AC-3 5.1 are at the line, a frame and a half of margin short
of it; the enhanced-coupling row lost 371 ms, its §3.5.5 analysis being the
same routine the decoder had already had in `float`. The peaks fell with the
halved store: E-AC-3 5.1 from 202,760 bytes to 154,932, AC-3 5.1 from
144,754 to 107,166. The stage-timed build, self time per frame, the stages
that matter:

| Stage | `ac3` 5.1 (36.6 ms) | `eac3` 5.1 (84.2 ms) | `eac3_stereo` 2/0 (35.2 ms) | `eac3_tools` 2/0 (61.6 ms) | `eac3_ecpl` 2/0 (56.3 ms) |
|---|---:|---:|---:|---:|---:|
| Transient detection and forward MDCT, `float` | 8.4 ms | 7.5 ms | 3.0 ms | 3.0 ms | 3.0 ms |
| Exponent run planning (integer) | - | 28.2 ms | 11.3 ms | 4.3 ms | 13.1 ms |
| §7.2 bit allocation (integer), 19 to 237 calls | 5.6 ms | 13.1 ms | 6.3 ms | 2.1 ms | 5.0 ms |
| Mantissa bit counts (integer) | 4.0 ms | 8.9 ms | 2.9 ms | 1.2 ms | 3.3 ms |
| `choose_delta_segments`, now `float` | 1.4 ms | 5.0 ms | 2.1 ms | 0.7 ms | 2.1 ms |
| Dither flags, fixed exponents, now `float` | 4.0 ms | 2.6 ms | 0.7 ms | 0.2 ms | 0.8 ms |
| Spectral-extension coordinates | - | - | - | 13.5 ms | - |
| AHT: transform, gains, vector quantiser, bit counts, all `double` | - | - | - | 23.8 ms | - |
| §E3.5 channel spectrum, `float` DFT, 12 calls | - | - | - | - | 10.0 ms |
| §E3.5 band fitting and angle decision, `float` | - | - | - | - | 6.1 ms |

What an E-AC-3 5.1 frame is now is its search: the exponent-run planner, the
bit-allocation calls and the mantissa bit counts are 50 of its 84 ms, and all
three are integer arithmetic on the decoded exponents - the same routines the
decoder runs once a block, here a few hundred times a frame for the
candidates the search weighs. The `float` arithmetic that was the whole story
in the morning is 20 ms of it. The tools row's remaining `double` is the AHT,
a quarter of its frame; §E3.5's is nothing, that row being 13 ms of planning
and 10 of the twelve spectra. Real time for E-AC-3 5.1 on this part is now a
question about the planner - 28 ms for six streams - and the number of
allocation candidates, not about arithmetic; or the second core. The next
section is what that question was worth.

The same float encoder is a full build with `-DICLFORGE_ENCODE_SCALAR=float`,
which CI's `linux-gcc` leg builds beside the float decoder, in the nightly run's
extra passes, to run its streams through the gold-reference gate and hold its
worst channel to within 0.5 dB of the double encoder's
(`tools/checks/check_encode_scalar_quality.py`); on
the five gold streams the two encoders' worst channels are identical to the
hundredth of a decibel.

#### The search and the planner

The same day again, the search. Three things were found and four changed.

The rate-control search runs twice a frame - once with the §7.2.2.6 delta
segments and once without, keeping whichever reaches the higher offset - and
each pass was warm-started from the *other* pass's answer, some thirty
composite units away on ordinary material, so each marched ten probes to
cross that gap every frame. Each pass now starts from its own previous
answer, which moves by a handful of units. That change turned out not to be
the pure speed-up it was documented as: the frame's mantissa cost is not
monotone in the offset (`mantissa_bits_per_block` packs bap-1, bap-2 and
bap-4 mantissas three or two to a codeword, and a bin moving to a larger bap
can empty a partly filled group), so the fitting predicate has several
boundaries in rare frames and the probe sequence decides which one is found.
Every such answer is a fitting offset; the gold-reference margins did not
move; the E-AC-3 fixture hashes did, by a unit of offset here and there, and
were re-pinned. `snr_search.hpp` records the finding.

The other three are exact - the same candidates, the same answer, less work,
and gated by the hashes not moving. The exponent-run planner scores both of
Annex E's frame forms in one pass, skips a run whose exponent set alone
already costs more than the best plan found for its end block, walks only the
bins allocated any precision, and extends a run's waste by one block's row
when its minimum did not move (`libs/ac3/tests/encoder/test_exp_strategy.cpp` holds
the pass to a transcription of the old one over 400 random inputs). The
masking curve is computed once per run per search and only §7.2.2.7's offset
is applied per probe (`compute_masking_curve` / `allocate_from_curve`, held
to `compute_bit_allocation` over 1,800 offsets in `libs/ac3/tests/core/test_bitalloc.cpp`).
And blocks read by the same run of every stream are counted once.

Measured on the board on 2026-09-10, plain build, 240 MHz, every hash the
host's and the Cortex-M3 leg's:

| Row | ms/frame, encoder `float` | ms/frame, this section | x real time now |
|---|---:|---:|---:|
| `ac3_stereo` 2/0 | 12.1 | 11.1 | **0.35** |
| `eac3_stereo` 2/0 | 33.8 | 23.5 | **0.73** |
| `ac3` 5.1 | 35.1 | 32.2 | 1.01 |
| `eac3_ecpl` 2/0 | 54.7 | 42.3 | 1.32 |
| `eac3_tools` 2/0 | 56.4 | 52.6 | 1.64 |
| `eac3` 5.1 | 81.2 | 55.5 | 1.74 |

E-AC-3 2/0 is in real time with a quarter of the frame to spare, and AC-3 5.1
sits on the line. The stage-timed build, self time per frame, the stages
that moved:

| Stage | `eac3` 5.1 before | `eac3` 5.1 after | `eac3_stereo` 2/0 after |
|---|---:|---:|---:|
| Exponent run planning | 28.2 ms | 14.7 ms | 6.1 ms |
| Bit allocation (the curve once, the offset per probe) | 13.1 ms, 237 calls | 5.1 ms | 2.5 ms |
| Mantissa bit counts | 9.0 ms | 4.5 ms | 1.6 ms |
| `choose_delta_segments` | 5.0 ms | 4.8 ms | 1.9 ms |
| Probes per frame | 23 | 18.5 | 17 |

What remains of an E-AC-3 5.1 frame is the planner (15 ms), the allocation
probes (10 ms across the two passes), the delta segments (5 ms) and the
front end (7.5 ms), against 55 in all; the 6-frame fixture's probe count is
also higher than real material's, whose offsets move less from frame to
frame. The AC-3 5.1 fixture is a different case: its rate search saturates
at the maximum offset after the second frame (a 448 kbit/s frame of sines
has room to spare), so what its 11 probes a frame measure is the fixture's
own cold start, and its 32 ms is the allocation and the mantissa counts.

**The first effort level.** The search is where a platform can be given a
choice, and the choice this branch adds is `delta_allocation`
(`iclforge::ac3::EncoderConfig` and `iclforge::ac3::eac3::FrameConfig`, on by default; the CLI
spells it `delta=off`, or `nodelta` in `eac3-encode`'s tools string). Off, the
encoder chooses no §7.2.2.6 segments and runs no second search to weigh
them; the stream is a legal one with `dbaflde` clear. What that removes on
this part is the segments' own stage and the second pass - 4.8 ms and about
4.6 ms of an E-AC-3 5.1 frame, and the side-information re-measurement
between them - and what it costs on the five gold streams, decoded and
compared with the source, is 0.01 dB on the worst channel of the two E-AC-3
streams and nothing on the AC-3 ones: the race was already dropping the
segments on most of their frames. [The arithmetic-tiers plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/arithmetic-tiers.md)
is where the axis this is the first point of is written down.

What the part cannot encode, measured on the host profile ([Building](../../building.md#what-the-encode-direction-costs)
has the table): 5.1 with AHT (312,744 bytes) or standard coupling (289,202)
or both with spectral extension (369,790), any layout that needs a dependent
substream (7.1.4 peaks at 601,954 with three encoders resident), and the
Atmos object encoder (about 300,000, and not in the profile). 5.1 with
spectral extension alone (205,718) does fit and has no row yet. Those
figures are the host profile's as first measured, when the plain 5.1 row peaked at
223,020 bytes; that row peaks at 158,911 on the Cortex-M3 leg now, so the shapes
above have not been measured again and some may fit. The encode
build leaves 298,824 bytes free in internal SRAM, 233,472 in its largest
run, under QEMU on 2026-09-29.

### What is left, and what would move it

- **Objects.** JOC reconstruction is 12.3 ms of the objects fixture's 21.7:
  4.2 ms mixing, 3.5 ms re-analysing the bed with thirty forward transforms a
  frame, 2.8 ms synthesising six objects. The bed analysis exists because
  `oba::joc::reconstruct` takes the bed as PCM; the decoder holds that bed's
  MDCT coefficients already, one block at a time, and a reconstruction that
  took them would skip the analysis outright. Beyond that, this is the one
  place the second core is worth its complexity: JOC for frame N is
  independent of the bed decode of frame N+1, so a second task can run it a
  frame behind, at the cost of one frame of latency, and throughput becomes
  the larger of the two halves rather than their sum. Neither is done.
- **Enhanced coupling** is at 0.62x and has one cheap step left. Each block's
  spectrum runs three inverse transforms, and two of them are the neighbouring
  blocks' - the same transforms the previous and next block run for
  themselves, so eighteen a frame where eight are distinct; a cache keyed by
  block would take about a millisecond off the 6.7 the spectrum costs now.
  `fft.cpp` joined the `-O2` list in the third pass and was worth 0.1 ms:
  `ecpl_channel_spectrum` went from 6.9 ms to 6.7 and the reconstruction
  stayed at 4.7. The pass's gain on this fixture came from the bitstream
  side instead - its mantissas 2.4 ms to 1.6.
- **7.1.4 to a DAC.** The decode is in real time on one core at 0.90x, and
  the frame of output a player used to have to hold - 73 KB for twelve
  channels, whether the probe held it or a player did - is gone: the
  `_by_block` forms (`decode_access_unit_by_block`, see
  [Decoding](../../library/decoding.md#block-granular-output)) hand the
  programme over a 256-sample block at a time from the decoder's own
  storage, copying nothing, and the probe now holds no PCM at all. What is
  still structural: a dependent's channels go through the access unit's
  own vectors, 73 KB at the peak, where writing them straight into their
  output slots would remove them; PSRAM for the staging remains the blunt
  alternative. One S3 I2S line carries 128 bits a frame — four 32-bit TDM
  slots or eight 16-bit ones — so twelve need both controllers at 16 bits,
  which is the shape the sink opens now (sixteen slots in all); a
  twelve-slot DMA queue competes with WiFi for
  internal RAM; and at 0.90x the second core stops being optional for anything
  that decodes 7.1.4 and does something else
  ([`planning/esp32-714-realtime.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-714-realtime.md)).
- **The second core** was the lever the earlier estimates ranked first. It was
  not needed for stereo or 5.1, and the breakdown says why it would have
  disappointed: the stages that dominated were serial software floating point,
  not parallel work, and splitting them across two cores would have halved a
  cost that could be removed instead.
- **Per-frame allocations** are 1 to 27 per frame today (this profile's open
  zero-heap-traffic gap; the table under [7.1.4](#714-the-widest-programme) has each
  fixture's), where they were 1 to 36 when the passes above were made. At a
  few microseconds each they are not on the path to real time for any fixture
  here. The first three passes left the count the runner gates untouched on
  every fixture (the coupling-coordinate vector that allocated once per
  coupled channel per block went as a side effect, but no fixture couples);
  the fourth took the Atmos fixtures from 23 and 41 to 20 and 31 by moving the
  object description rather than copying it.
- **A hand-written kernel tier** (`madd.s`, which `-ffp-contract=off` forbids
  project-wide) would apply to the IMDCT, which is 3.5 ms of a 11.3 ms 5.1
  frame. That bounds what the tier could return at under a quarter of the
  remaining time, and it is not needed for anything that now fits.

### Running it yourself

    idf.py -p <PORT> flash monitor

with `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hw"` if the board is
reached through its native USB connector rather than the UART bridge - see
`sdkconfig.hw` for what that changes and why. Add `-DICLFORGE_STAGE_TIMERS=ON`
to the build for the per-stage lines, from either probe.

On a DevKitC-1 the host cannot reset the part into the application over
USB-Serial-JTAG: both `esptool` and `idf.py monitor` assert IO0 during their
reset sequence, so every host-initiated reset lands in `boot:0x0 (DOWNLOAD)` and
the application never starts. Attach with `idf.py monitor --no-reset` and press
the board's RESET button instead. Flashing over the same connector is
unaffected. The probe pauses three seconds before its first line so the
re-enumeration that follows a reset does not swallow the first fixture's
output, which it otherwise does.

One trap for a machine that builds both shapes: ESP-IDF keeps `sdkconfig` in
the PROJECT directory, shared by every `-B` build directory, and regenerates it
from `SDKCONFIG_DEFAULTS` only when it is absent. A QEMU build made after a
hardware build therefore inherits `sdkconfig.hw`'s USB console and prints
nothing under QEMU, which has no such device. Give each shape its own
`-DSDKCONFIG=<build dir>/sdkconfig`, or delete `sdkconfig` between them.

## Objects

`joc.cpp` and `oamd.cpp` are in `libs/ac3/minimal.cmake`'s source list and link into every build
of this profile, so object decode always compiled here. For a long time it did not fit: an
`atmos-encode` fixture (six objects, JOC over a 5.1 downmix, 448 kbit/s) peaked at 449,826 bytes
against 280,792 free, and `ReconstructionState` was a single 147,504-byte allocation — larger
than the 116,736-byte contiguous run a decode leaves free, so it failed on contiguity before any
budget was consulted.

Four changes, each measured on its own:

| | Peak heap | Largest single allocation |
|---|---|---|
| As found (`Domain::kQmf`, `double`, arrays at `kMaxObjects`) | 449,826 | 147,504 |
| `Domain::kMdctBand` | 386,770 | 147,504 |
| + `ReconstructionState` in float32 | 301,522 | 73,776 |
| + per-object scratches sized to the stream | 267,754 | 43,008 |
| + handing back the enhanced-coupling scratch | **233,546** | 43,008 |

Those are the figures of that week. The objects fixture peaks at 194,655 bytes today, and the
largest single allocation across the whole probe run is 36,864 bytes (the block form's AC-3
frame), with the enhanced-coupling scratch at 23,552.

The largest allocation was then the E-AC-3 decoder's own AHT buffer rather than anything JOC owns.
That buffer was split into one 6,144-byte buffer per stream on 2026-09-12, and since 2026-09-16
an AHT stream decodes straight into the per-block coefficient store instead, with no buffer of
its own.

That last row is the one that is easy to miss. 267,754 against 280,792 free looks like 13,038
spare, but the order of fixtures decided the result: objects run after an enhanced-coupling decode
failed with `out_of_memory bytes=6144`, while objects on a clean heap passed. The difference is
`eac3_tools.cpp`'s 32,768-byte spectrum scratch and 1,440-byte bin-angle vector, both
`thread_local` so §E3.5 neither allocates per call nor puts 32 KB on the stack. On a hosted
platform they are released at thread exit; here the only thread never exits.
`iclforge::ac3::eac3::release_ecpl_scratch()` hands them back, and the probe calls it between fixtures.
Retained at exit went from 34,232 bytes to 24, and to 12 once the bin-angle vector became a
stack array and the scratch took its float form, 23,552 bytes.

**The bed does not need any of this.** An Atmos bed is ordinary E-AC-3 5.1 and the objects are
side data, so `DecoderConfig::skip_object_reconstruction` decodes the bed without allocating
`ReconstructionState` at all — 20 allocations per frame against 31. `libs/ac3/tests/oba/test_atmos.cpp`
asserts the rendered channels are bit-for-bit what a full decode produces. `object_metadata`
still arrives, parsed out of a block's skip field.

### Placed on loudspeakers

Reconstructed objects are mono signals with a position each; a part driving a 7.1.4 DAC has to
pan them onto its loudspeakers, and since 2026-09-10 it can, on the target: `spatial.cpp` is in
the profile's source list, and the block form's `PcmBlock` carries the objects beside the bed -
a view per object onto the unit's own reconstruction, cut to the block, with the metadata that
places them ([Decoding](../../library/decoding.md#block-granular-output)). The probe's
`eac3_atmos_render` row is the sink a player would write: `iclforge::spatial::pan_direction` for
each object's gains onto the eleven panned targets, once per unit, the bed's LFE passed through
as the twelfth slot, and a block of float sums per target - twelve channels of one 256-sample
block, static. Its stream is the Atmos rows' source with three objects raised to the ceiling and
one half way (`tools/generators/atmos_height_scene.txt`), because the Atmos rows' objects all
sit on the listener plane and a render of them would leave the four height targets silent.

Every one of its twelve levels is the host's to the digit on both emulated legs, which is what
the row can say: no `forge` path writes a rendered layout to a WAV for the generator to
measure, so `render_fixture.hpp` is the host shape's own numbers and the row is a regression
reference, the standing the encode fixtures already have; `libs/render/tests/` is what says the
panner is right. What it costs, on the Cortex-M3 leg's count on 2026-09-29: 28,945,000
instructions a frame for decode and render together, the render itself 5%
of that; 195,025 bytes of peak - the objects row's plus the panner's target
tables - and 27 allocations a frame against the objects row's 22. The panner used to allocate eight
vectors per object per call; it has stack storage now.

On the board, 2026-09-10, plain build, 240 MHz: 25,105 us a frame for decode and render
together, 0.78x, the render 3,261 us of it - 13% of the row where the Cortex-M3 leg counts 5%.
The mix is float on the FPU; what is not is the pan, `pan_direction`'s trigonometry in
`double`, once per object per unit and every one of those operations a call into the ROM's
software floating point on this part. What would move it: a `float` panner, or recomputing an
object's gains only when its position or gain has changed, which for a stream whose objects
hold still - this fixture's do - takes the pan out of every unit but the first.

## Configuration

`firmware/baremetal/platform/esp32s3/sdkconfig.defaults` carries the settings and their reasoning. Two
are repeated here because neither failure mode points at its cause:

- **`CONFIG_ESP_MAIN_TASK_STACK_SIZE=40960`** (32,768 until PR #698 grew `DecodedSubstream`/
  `DecodedAccessUnit` past the runner's floor — see the Stack section above). IDF's default is
  3,584 bytes, which suits an application that configures peripherals and waits on queues and is
  far too small for a codec. The overflow does not report as a stack overflow: it surfaces as a
  `LoadProhibited` panic on the other core's idle task, because it corrupts a neighbouring
  structure. An integrator sizing a real decoding task should measure with
  `uxTaskGetStackHighWaterMark()` rather than copy this.
- **`CONFIG_ESP_TASK_WDT_INIT=n`.** The probe is a batch computation that runs the CPU flat out
  without yielding, which is what the watchdog exists to catch. A decoder in a product should keep
  the watchdog and give the decode its own task with a bounded per-frame budget.

PSRAM is off in the AC-3 and E-AC-3 probes, although the development board has 8 MB, so the
internal-SRAM budget is enforced rather than avoided. QEMU could not emulate S3 PSRAM when they
were written ([espressif/qemu#129](https://github.com/espressif/qemu/issues/129)); the QEMU that
ESP-IDF v6.1 installs (`esp_develop_9.2.2_20260417`) does, quad or octal with 32 MB, and the AC-4
probe runs on it ([AC-4](#ac-4)). The Hearth Sendspin sink is a separate networked shape and
requires the board's 8 MB of PSRAM.

## AC-4

The component's AC-4 decoder (`CONFIG_ICLFORGE_AC4`, in `float`) builds for this part and decodes
correctly under QEMU, with its state in PSRAM (phase D14c of
[`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#d14-ac-4-on-the-esp32s)).
It ran on a board on 2026-10-10 (D14c's board phase, [On the board](#on-the-board)): its PCM is the
host's and the P4's on every stream, and it keeps up with real time at 2.0 in SIMPLE mode and at no
wider or more coded stream (D14g's [Playback speed](#playback-speed) moved that line).

### Where its memory goes

A decode does not fit internal RAM beside what else the part runs. When D14c measured it, the probe's
2.0 fixtures peaked at 413,611 to 601,504 bytes of heap, where this part has 347,051 free and a
largest block of 249,856, and 5.1.4 peaked at 1,800,312. D14f has since cut the `float` decoder's
peaks to 286,365 to 418,110 bytes at 2.0, 696,375 to 859,616 at 5.1 and 1,494,319 at 5.1.4
([`planning/ac4.md`, D14f](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#d14f-the-decoders-memory)).
Decision 29 planned 2.0 in internal RAM; the owner decided on 2026-10-03 that the decoder's state
goes in PSRAM instead, and the CI row holds what is left in internal RAM.

ESP-IDF's heap places each `malloc` by its size: below `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` it
tries internal RAM first, at or above it PSRAM first, and either falls back to the other. The
decoder makes many blocks of 8 to 16 KB (65 of them, 534 KB, at the 5.1.4 fixture's peak), so the
limit decides where its state goes. Under QEMU, the internal RAM each fixture took at its worst
moment, by limit, with the decoder as D14c had it, before D14f (ESP-IDF's local-minimum monitor,
summed over the internal heap's regions, so a figure is at most what was in use at once):

| Fixture | Peak heap | 16,384 (ESP-IDF's default) | 4,096 | 512 | 0 |
|---|---:|---:|---:|---:|---:|
| `ac4_20_music`, 2.0 | 413,611 | 247,608 | 29,588 | 3,188 | 0 |
| `ac4_20_acpl`, 2.0 A-CPL | 601,504 | 256,856 | 31,528 | 5,032 | 0 |
| `ac4_20_companding`, 2.0 | 462,435 | 263,756 | 43,728 | 4,468 | 0 |
| `ac4_51_music`, 5.1 | 946,390 | 340,188 (1,903 left) | 67,844 | 12,012 | 0 |
| `ac4_51_acpl`, 5.1 A-CPL | 1,147,590 | 339,532 (2,559 left) | 48,704 | 8,612 | 0 |
| `ac4_514_tones`, 5.1.4 | 1,800,312 | 340,840 (1,291 left) | 108,452 | 14,140 | 0 |

At ESP-IDF's default the decoder fills internal RAM at 5.1 and wider, as it does on the
[ESP32-P4](esp32-p4.md#ac-4) (1 to 8 KB left there), and a board running Wi-Fi beside it would have
nothing for Wi-Fi and lwIP. At 512 bytes the decoder keeps 3 to 14 KB there and the rest, 0.42 to
1.83 MB, in PSRAM. That is the limit the AC-4 probe runs at
(`firmware/baremetal/platform/esp32s3/sdkconfig.ac4`) and the one `hearth_sink` gives an AC-4 play on
this part: the component's `CONFIG_ICLFORGE_AC4_INTERNAL_BELOW`, 512 here, which the player sets
when an AC-4 play starts and puts back to ESP-IDF's value when it ends, so AC-3 and E-AC-3 plays
keep the 16 KB their board figures were measured under. On the P4 the option defaults to ESP-IDF's
value and changes nothing.

The placement does not change the PCM: each of the six fixtures has the same hash at every limit,
equal to its pin. The peak heap, the allocations a frame (50 to 203) and the retained bytes (none)
were the Cortex-M3 leg's to the byte.

The table above has not been measured again on the decoder after D14f, which moves no PCM bit and
holds less. The CI row's ceilings (6,000 bytes of internal RAM at 2.0, 14,000 at 5.1 and 16,000 at
5.1.4) are upper bounds, so they should hold the smaller decoder; the next run of
`run_esp32s3_probe.sh --ac4` on a machine with ESP-IDF v6.1 gives the new figures.

### What stays in internal RAM, and why

- **The decode task's stack, in the probe.** A decode used 18,448 to 21,568 bytes of it in the probe
  (19,480 at most on the Cortex-M3, whose frames are smaller), in internal RAM, where FreeRTOS keeps
  a task's stack unless asked. `hearth_sink` is different: the stack is in PSRAM there, because with
  Wi-Fi up no internal block holds the 32,560 bytes a 5.1 A-CPL play uses
  ([The decode stack goes in PSRAM](#the-decode-stack-goes-in-psram)).
- **Allocations under 512 bytes**, 3 to 14 KB at the worst moment: the decoder's small vectors and
  their bookkeeping, which the heap keeps internal.
- **The image's own data**: 51,469 bytes of DIRAM for the AC-4 probe (`idf.py size`), 31,727 of it
  ESP-IDF's code in IRAM (interrupt handlers and what runs with the cache off), 13,454 `.data` and
  6,288 `.bss`. The decoder's tables are constants and stay in flash, read through the cache
  (451,856 bytes of `.rodata` in the image); the frame-rate converter copies its table into PSRAM
  when it is made (D14a5).
- **DMA.** The decoder does none. The sink's I2S DMA descriptors and buffers are internal
  (`hearth_sink`'s twelve descriptors of 256 frames, 24 KB at 2.0 in 32-bit slots) and are
  allocated by their caps, which the limit does not move. The player's ring and its held unit are
  in PSRAM already.
- **Interrupts and the cache-off paths.** No decoder code runs in an interrupt or with the cache
  off. A flash write (an update, NVS) turns the cache off and pauses the other core, so a decode
  waits for it, as it would with its state in internal RAM, since its code is in flash either way.
- **The hot kernels' working sets** (the QMF banks' delay lines and planes, the transform scratch)
  are in PSRAM at 512 bytes, behind the 32 KB data cache. What that costs the board measured:
  raising the limit to 4,096 bytes, which puts them in internal RAM, took 1 to 2% off a frame on
  five streams ([The 512-byte limit stays](#the-512-byte-limit-stays)), where on the P4 the 512-byte
  limit took 1.08 to 1.24 times as long over twenty plays (D14e).

### Against the ESP32-P4

| | ESP32-S3 (512-byte limit for AC-4; PCM and memory under QEMU, time on the board) | [ESP32-P4](esp32-p4.md#ac-4) (board, ESP-IDF's default) |
|---|---|---|
| PCM | the six fixtures' pinned hashes | the six fixtures' pinned hashes, and the host's on 52 plays |
| Peak heap | 0.29 to 0.42 MB at 2.0, 0.70 to 0.86 MB at 5.1, 1.49 MB at 5.1.4 since D14f (the probe; 0.41 to 0.60, 0.95 to 1.15 and 1.80 MB when D14c measured it) | 0.58 MB at 2.0 to 2.2 MB at 5.1.4 (`hearth_sink`) |
| Internal RAM at the worst moment | 3 to 14 KB used, 333 to 344 KB of 347 KB left | used up: 1 to 8 KB left of 344 to 350 KB |
| Decode stack | 18 to 22 KB | 19 to 30 KB |
| Real time | 2.0 SIMPLE at 0.87, A-SPX at 1.09; 5.1 at 2.1 to 3.2; 5.1.4 at 4.4 to 5.6 (board, Wi-Fi up; D14c; since D14g, with the memory fragments and the second core, 0.40, 0.50, 0.99 to 1.57 and 2.28 to 2.73) | 2.0 at 0.28 and 0.37; 5.1 at 0.64, 0.83 and 0.90; 5.1.4 at 1.55 to 1.89 (D14e; since D14g 0.15 and 0.21, 0.42 to 0.73 and 1.01 to 1.21) |

QEMU's times describe the emulator, as [Timing](#timing) says, so the time row is the board's, above.
The P4 runs at 360 MHz on RISC-V with a 128 KB L2 cache and hex PSRAM at 200 MHz; this part runs at
240 MHz with a 32 KB data cache and octal PSRAM at 80 MHz.

### On the board

The figures here are D14c's: `main`'s decoder on one core with the default memory configuration. [Playback speed](#playback-speed)
has D14g's.

Measured on 2026-10-10 on `hearth-eb2c64` (ESP32-S3 revision v0.2, 240 MHz, 8 MB of octal PSRAM at
80 MHz, 16 MB of flash), `hearth_sink` from the tree of that day, built with `sdkconfig.defaults`,
`sdkconfig.hw`, `sdkconfig.psram`, `sdkconfig.sendspin` and `sdkconfig.ac4` and the measurement
fragments the P4's figures used (a null sink that takes a block and returns at once, the PCM hash on,
`ICLFORGE_STAGE_TIMERS` on), Wi-Fi up, mDNS and the Sendspin player idle. The streams are DEE's
music streams from the local gold set, ten seconds each, served from a desktop over HTTP and played
with `POST /play` after `PUT /layout`, as [the P4's](esp32-p4.md#how-it-was-measured) were. The time
is the decode task's less the sink's write, the placing and the hash, against the audio a frame
carries (42.7 ms at 48 kHz); each figure is one play. A play's location can carry `?parallel=off` for a play on one core
(`CONFIG_ICLFORGE_EXAMPLE_AC4_PARALLEL`, on in `sdkconfig.ac4`, is the default) and `?hash=off` for a play without the hash;
[Playback speed](#playback-speed) has what each of the memory fragments gives.

| Stream | Codec mode | To | us/frame | x real time | P4 x real time | Worst frame ms | Stack left KB | Internal RAM least free KB | PSRAM peak MB |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `20-music-192` | SIMPLE | 2.0 | 37,167 | 0.87 | 0.28 | 70 | 18.9 | 80 | 0.40 |
| `20-music-96` | A-SPX | 2.0 | 46,402 | 1.09 | 0.37 | 69 | 15.8 | 77 | 0.43 |
| `51-music-384` | SIMPLE | 2.0 | 89,838 | 2.11 | 0.60 | 141 | 18.0 | 78 | 0.74 |
| `51-music-384` | SIMPLE | 5.1 | 91,133 | 2.14 | 0.64 | 151 | 18.1 | 78 | 0.78 |
| `51-music-192` | A-SPX | 2.0 | 104,216 | 2.44 | 0.76 | 156 | 15.6 | 72 | 0.77 |
| `51-music-192` | A-SPX | 5.1 | 110,979 | 2.60 | 0.83 | 155 | 15.6 | 72 | 0.82 |
| `51-music-128` | A-SPX, A-CPL 2 | 2.0 | 113,227 | 2.65 | 0.83 | 144 | 15.6 | 74 | 0.88 |
| `51-music-128` | A-SPX, A-CPL 2 | 5.1 | 120,093 | 2.81 | 0.91 | 155 | 15.6 | 74 | 0.92 |
| `51-music-96` | A-SPX, A-CPL 3 | 2.0 | 129,679 | 3.04 | 1.06 | 178 | 8.2 | 77 | 1.02 |
| `51-music-96` | A-SPX, A-CPL 3 | 5.1 | 137,156 | 3.21 | 1.14 | 184 | 8.2 | 76 | 1.06 |
| `514-music-256` | A-SPX, A-CPL 2 | 2.0 | 221,099 | 5.18 | 1.57 | 263 | 15.7 | 69 | 1.42 |
| `514-music-256` | A-SPX, A-CPL 2 | 5.1.4 | 237,137 | 5.56 | 1.83 | 291 | 15.6 | 68 | 1.71 |
| `514-music-512` | A-SPX, S-CPL | 2.0 | 213,617 | 5.01 | 1.66 | 268 | 15.7 | 64 | 1.49 |
| `514-music-512` | A-SPX, S-CPL | 5.1.4 | 239,215 | 5.61 | 1.90 | 299 | 15.6 | 62 | 1.78 |
| `514-music-768` | S-CPL | 2.0 | 185,744 | 4.35 | 1.34 | 249 | 17.0 | 77 | 1.54 |
| `514-music-768` | S-CPL | 5.1.4 | 208,963 | 4.90 | 1.57 | 288 | 17.1 | 75 | 1.82 |
| `ims-music-64-23976` | A-SPX, 23.976 fps | 2.0 | 78,295 | 1.88 | 0.68 | 564 | 15.7 | 76 | 0.69 |
| `ims-music-64-24` | A-SPX, 24 fps | 2.0 | 54,804 | 1.32 | 0.51 | 520 | 15.7 | 77 | 0.51 |
| `ims-music-64-25` | A-SPX, 25 fps | 2.0 | 56,553 | 1.41 | 0.53 | 84 | 15.8 | 78 | 0.46 |
| `ims-music-64-2997` | A-SPX, 29.97 fps | 2.0 | 62,685 | 1.88 | 0.69 | 458 | 15.8 | 76 | 0.65 |

- **The PCM is the P4's.** The hash of every play equals the hash the P4 gave the same stream, which
  equals the host's: decision 26 holds on the S3's Xtensa build on a board, with the state and the
  stack in PSRAM and Wi-Fi up.
- **Real time at 2.0 in SIMPLE mode only.** `20-music-192` takes 0.87 of a frame. A-SPX at 2.0 takes
  1.03 to 1.09 (the least from the placement sweep below, the most from the plays above), 5.1 takes
  2.1 (SIMPLE) to 3.2 (A-SPX with A-CPL mode 3), 5.1.4 in full decoding (to 5.1.4 or folded to 2.0) 4.4 to 5.6,
  and the converter's four frame rates 1.3 to 1.9. Against the P4 every play takes 2.6 to 3.5 times
  as long, on a part at two thirds of its clock, with a quarter of its cache and a slower PSRAM. A
  5.1 stream folded to 2.0 takes 1 to 7% less than the same stream played to 5.1.
- **AC-3 and E-AC-3 through the same image**, network and server, decoder time as above:
  `dee-ac3-51.ac3` at 5.1 takes 15,006 us a frame (0.47 of 32 ms), `dee-eac3-51.ec3` at 5.1 15,447
  (0.48) and `714-walk.ec3` at 7.1.4 34,453 (1.08).
- **The first frame** is the worst of a play: 69 to 70 ms at 2.0, 141 to 184 ms for the 5.1 streams
  and 249 to 299 ms for the 5.1.4 ones. The converter's streams spend 0.46 to 0.56 s in theirs at
  23.976, 24 and 29.97 fps and 84 ms at 25 fps, where the P4's are 0.24 to 0.30 s in all four; the
  25 fps figure is not explained.

#### The decode stack goes in PSRAM

The first play on the board did not start. With Wi-Fi and the Sendspin player up the part has about
95,000 bytes of internal RAM free and **no block over 31,744 bytes**, and the decode task's stack of
40,960 bytes (`sdkconfig.ac4`, chosen for the P4) could not be made. At 28,672 bytes a 2.0 play ran
with 3.7 KB to spare, and the 5.1 A-SPX with A-CPL mode 3 stream (`51-music-96`) overflowed it ("A
stack overflow in task ac3-decode") and restarted the board: that stream uses 32,560 bytes. The QEMU
rows have no Wi-Fi and met neither.

`PlayerConfig::decode_stack_in_psram` (`CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM`, on by default
for an ESP32-S3 image with AC-4 and PSRAM) makes the decode task with `xTaskCreatePinnedToCoreWithCaps`
and a stack in PSRAM, where the 40,960 bytes fit with 8.2 KB to spare at the widest. It costs 4 to 5%
of a 2.0 A-SPX frame (44.7 ms with the stack in internal RAM, 46.7 with it in PSRAM) and AC-3 and
E-AC-3 frames the same within a boot's variation (15.1 ms against 14.3, 15.2 against 16.8, 34.5
against 36.8), and it returns 28 KB of internal RAM: the least free during a play is 62 to 82 KB,
where it was 38 to 55 KB.

#### The 512-byte limit stays

The first thing the board's timers were to decide: the allocation limit of 512 bytes
(`CONFIG_ICLFORGE_AC4_INTERNAL_BELOW`) against larger ones, with the stack in PSRAM so that the
internal RAM was there to be used. One image, each play's `?below=N` giving its limit
(`PlayerConfig::Ac4Options::internal_below`); each cell is the decoder's us a frame, the multiple of
real time, and the least internal RAM free during the play:

| Stream | 512 | 2,048 | 4,096 | 8,192 | 16,384 |
|---|---|---|---|---|---|
| `20-music-192`, 2.0 SIMPLE | 37,542 (0.88), 79.7 KB | 37,124 (0.87), 60.3 KB | 37,182 (0.87), 54.0 KB | 33,988 (0.80), 1.6 KB | 36,341 (0.85), 2.1 KB |
| `20-music-96`, 2.0 A-SPX | 46,637 (1.09), 76.8 KB | 46,173 (1.08), 54.4 KB | 46,143 (1.08), 53.7 KB | 43,824 (1.03), 4.9 KB | 45,267 (1.06), 4.2 KB |
| `51-music-384`, 5.1 SIMPLE | 91,287 (2.14), 78.2 KB | 92,758 (2.17), 43.9 KB | 90,392 (2.12), 38.6 KB | 86,248 (2.02), 0.6 KB | 88,680 (2.08), 1.0 KB |
| `51-music-96`, 5.1 A-CPL 3 | 138,150 (3.24), 76.2 KB | 135,222 (3.17), 44.4 KB | 135,631 (3.18), 34.6 KB | 132,858 (3.11), 1.4 KB | 133,563 (3.13), 1.2 KB |
| `ims-music-64-2997`, 2.0 29.97 fps | 62,646 (1.88), 75.5 KB | 62,408 (1.87), 51.4 KB | 61,469 (1.84), 43.2 KB | 60,599 (1.82), 1.0 KB | 59,984 (1.80), 0.4 KB |

Every play has the same PCM hash at every limit. Up to 4,096 bytes the limit buys 1 to 2% of a frame
and spends 23 to 42 KB of internal RAM; from 8,192 up it buys 3 to 10% and leaves the part 0.4 to 5
KB, which Wi-Fi and lwIP cannot live on. The time is in the work and not in where the allocations
go. 512 stays.

#### What the board's timers leave

- **2.0 in SIMPLE mode keeps up; A-SPX at 2.0 is within 10% of it.** That is what an S3 decodes
  on its own with Wi-Fi up.
- **Nothing wider does.** 5.1 at 2.1 to 3.2 and 5.1.4 at 4.4 to 5.6 times real time are 2 to 5 times
  out of reach, not the few per cent the allocation limit moves. The stage timers show the time
  spread over the decode and not in one kernel (a 5.1 A-CPL mode 3 frame, in ms: transforms 27, QMF
  analysis 19, synthesis 23, A-CPL 41, parse 8, reconstruction 22 and A-SPX 7), and the PIE is
  integer where the decode is `float`, so decision 30's PIE kernel, which was to go where one kernel
  holds a stream back, has none to go to.
- **That is accepted.** What the S3 cannot decode in real time reaches it as PCM from Hearth, as
  decision 32 had it for the C6 ([decision 42](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#decisions-of-2026-10-10),
  2026-10-10). How a sink says what it decodes is I6's. D14g's second core and memory fragments moved the line
  ([Playback speed](#playback-speed), [decision 43](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#decisions-of-2026-10-10)).

### Playback speed

Measured on 2026-10-10 and 11 on `hearth-eb2c64` (ESP32-S3 rev v0.2, 240 MHz, 8 MB octal PSRAM, 16 MB quad flash), `hearth_sink` as in
[On the board](#on-the-board), Wi-Fi up, a null sink, the same plays. Every play of every configuration below, 40 for each of four images
with the second core off and on, gives the PCM hash D14c's plays gave, which is the P4's and the host's. Phase D14g of
[`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md): the decoder took 2.6 to 3.5 times the
P4's time on a part at two thirds of its clock, and the board's stage timers had it spread over the decode.

**Where the time was.** The inverse transform took 27.9 ms of a 5.1 A-CPL mode 3 frame here and 4.9 on the P4, 5.7 times, where the
clocks are 1.5. It reads the roots of its FFT's passes, its twiddles and its window, 56 KB for a 2,048-line block, through a 32 KB
data cache, and the flash was read in DIO mode: a 32-byte line came from the flash at 2 bits a clock, about 450 of the CPU's, against
the P4's 64-byte lines from a 128 KB L2. The decoder's code and its tables (0.45 MB of `.rodata`) are larger than the caches, so
every frame refilled them from the flash. A-CPL's interpolation, the other part that was out of proportion to the P4's, is the same
double operations in software on both.

**The memory the decoder runs from.** The decoder's time over a frame's duration on one core, each column adding to the one before it
except where it says otherwise:

| Stream | As before (DIO, 32-byte lines) | QIO flash | QIO, 64-byte lines | QIO, constants in PSRAM | QIO, code and constants in PSRAM | QIO, 64-byte lines, 64 KB cache | All of them |
|---|---:|---:|---:|---:|---:|---:|---:|
| `20-music-192`, 2.0 SIMPLE | 0.87 | 0.78 | 0.68 | 0.71 | 0.63 | 0.62 | 0.51 |
| `20-music-96`, 2.0 A-SPX | 1.09 | 0.92 | 0.81 | 0.87 | 0.79 | 0.72 | 0.61 |
| `51-music-384`, 5.1 SIMPLE | 2.13 | 1.85 | 1.60 | 1.70 | 1.63 | 1.46 | 1.30 |
| `51-music-96`, 5.1 A-CPL mode 3 | 3.24 | 2.87 | 2.55 | 2.73 | 2.60 | 2.34 | 2.16 |
| `514-music-768`, 5.1.4 S-CPL | 4.90 | 4.36 | 3.75 | 4.11 | 3.99 | 3.33 | 2.93 |
| `ims-music-64-2997`, 29.97 fps | 1.86 | 1.71 | 1.45 | 1.68 | 1.59 | 1.32 | 1.12 |
| E-AC-3 `714-walk`, 7.1.4 | 0.99 | 0.87 | 0.81 | 0.86 | 0.83 | 0.80 | 0.75 |

The first six columns were measured before D14g's changes to the converter and to the immersive element's tracks, which the last
column has; the second table has every configuration with them. What each is, and what it costs:

- **QIO flash** (`CONFIG_ESPTOOLPY_FLASHMODE_QIO`): 8 to 15% of an AC-4 frame, 11 to 19% of an AC-3 or E-AC-3 one, for nothing but
  the mode. The mode is the second stage bootloader's, so a board needs one USB flash with its bootloader (`idf.py flash`), and a
  network update leaves the mode it was in (a QIO application under a DIO bootloader ran at the DIO figures). A bootloader that
  cannot set a flash chip's quad-enable bit stays in DIO.
- **64-byte data-cache lines** (`CONFIG_ESP32S3_DATA_CACHE_LINE_64B`): 11 to 15% more, and no internal RAM: the cache stays 32 KB.
- **Constants and then code in PSRAM** (`CONFIG_SPIRAM_RODATA`, `CONFIG_SPIRAM_FETCH_INSTRUCTIONS`): 2 to 9% and then 3 to 13% more,
  7 to 20% together, for 2.3 MB of the 8. An update over the network and a flash write work as before; the part runs on through the
  write.
- **A 64 KB data cache** (`CONFIG_ESP32S3_DATA_CACHE_64KB`): 8 to 11% more on one core with the lines and QIO, and 12 to 24% on
  top of the three above with both cores. It is made of 32 KB of the heap: a play starts with 62 KB of internal RAM free instead of
  95 and its least is 31 to 49 KB where it was 64 to 81. Wi-Fi and lwIP cannot live on a few kilobytes
  ([The decode stack goes in PSRAM](#the-decode-stack-goes-in-psram)). The owner decided on 2026-10-11 to spend it
([decision 43](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#decisions-of-2026-10-10)) in the
images that carry the AC-4 decoder, and it stays its own file so that a build that needs the 32 KB back drops one line.
An image without the decoder is no place for it: the release shape (E-AC-3 and AC-3, no AC-4) with the cache and the decode
stack in PSRAM had 1,479 bytes of internal RAM free at the least in an E-AC-3 7.1.4 play, and the cache bought it nothing
(0.32 and 0.33 of a frame for AC-3 and E-AC-3 5.1 and 0.75 for 7.1.4 without it, 0.29, 0.33 and 0.75 with it).

`sdkconfig.s3-fast` has the first three (QIO, the lines, the code and constants in PSRAM) and `sdkconfig.s3-dcache` the fourth, each
over `sdkconfig.psram` (and `sdkconfig.ac4` where the decoder is in); the second goes after the first. The S3 board's release
image, which has no AC-4 decoder, is built with the first; the second goes with `sdkconfig.ac4`.

**The second core.** The decoder's stages that are per channel or per slot run on both cores (the player's worker task is on the core
the decode task is not on): each channel's inverse transform and QMF analysis, A-SPX's elements, A-CPL's slots and its three
decorrelators, the history's move and the copy of each channel's matrix, the downmix's outputs and each output's synthesis and
converter (`CONFIG_ICLFORGE_EXAMPLE_AC4_PARALLEL`, in `sdkconfig.ac4`; `?parallel=off` and `?parallel=on` in a play's location choose
for that play). The frame takes 8 to 27% less: the reconstruction, which is 76 to 95% of a one-core frame, runs 1.25 to 1.43 times as
fast, and not twice, because the two cores share the caches and the bus and because the dequantisation, the matrix and the parse
(5 to 22% of a one-core frame) stay on one. The converter's table is read in the order of its rows: each of 501 rows once for the 3.2
outputs of a frame that read it, in place of 41 rows' hop between one output and the next.

All twenty plays: the decoder's time over a frame's duration with `main`'s decoder, then with the second core on and the memory
configuration of an image built without the two files, with QIO and the lines, with `sdkconfig.s3-fast` and with `sdkconfig.s3-dcache`
after it (first on one core, then on two), the decoder's microseconds a frame and the worst frame there.

| Stream | Codec mode | To | `main` | Without the files, two cores | QIO + lines | `s3-fast` | `s3-dcache`, one core | `s3-dcache`, two cores | us/frame | Worst frame ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `20-music-192` | SIMPLE | 2.0 | 0.87 | 0.80 | 0.56 | 0.47 | 0.50 | 0.40 | 16,928 | 36 |
| `20-music-96` | A-SPX | 2.0 | 1.09 | 0.99 | 0.71 | 0.61 | 0.61 | 0.50 | 21,440 | 38 |
| `51-music-384` | SIMPLE | 2.0 | 2.15 | 1.85 | 1.34 | 1.14 | 1.28 | 0.98 | 41,996 | 65 |
| `51-music-384` | SIMPLE | 5.1 | 2.13 | 1.87 | 1.36 | 1.17 | 1.31 | 0.99 | 42,253 | 71 |
| `51-music-192` | A-SPX | 2.0 | 2.43 | 2.23 | 1.62 | 1.40 | 1.44 | 1.17 | 49,909 | 70 |
| `51-music-192` | A-SPX | 5.1 | 2.62 | 2.35 | 1.75 | 1.49 | 1.60 | 1.25 | 53,410 | 70 |
| `51-music-128` | A-SPX, A-CPL 2 | 2.0 | 2.68 | 2.36 | 1.76 | 1.56 | 1.63 | 1.31 | 55,895 | 73 |
| `51-music-128` | A-SPX, A-CPL 2 | 5.1 | 2.87 | 2.60 | 1.87 | 1.64 | 1.77 | 1.40 | 59,703 | 76 |
| `51-music-96` | A-SPX, A-CPL 3 | 2.0 | 3.00 | 2.58 | 2.03 | 1.84 | 1.96 | 1.48 | 63,106 | 77 |
| `51-music-96` | A-SPX, A-CPL 3 | 5.1 | 3.24 | 2.68 | 2.17 | 1.93 | 2.12 | 1.57 | 66,875 | 83 |
| `514-music-256` | A-SPX, A-CPL 2 | 2.0 | 5.01 | 4.27 | 3.19 | 2.88 | 2.95 | 2.38 | 101,725 | 124 |
| `514-music-256` | A-SPX, A-CPL 2 | 5.1.4 | 5.54 | 4.85 | 3.65 | 3.27 | 3.40 | 2.73 | 116,271 | 143 |
| `514-music-512` | A-SPX, S-CPL | 2.0 | 5.02 | 4.35 | 3.25 | 2.88 | 2.84 | 2.36 | 100,736 | 131 |
| `514-music-512` | A-SPX, S-CPL | 5.1.4 | 5.66 | 4.90 | 3.64 | 3.29 | 3.32 | 2.68 | 114,307 | 150 |
| `514-music-768` | S-CPL | 2.0 | 4.30 | 3.53 | 2.67 | 2.24 | 2.49 | 1.96 | 83,446 | 121 |
| `514-music-768` | S-CPL | 5.1.4 | 4.90 | 4.16 | 3.01 | 2.67 | 2.94 | 2.28 | 97,115 | 149 |
| `ims-music-64-23976` | A-SPX, 23.976 fps | 2.0 | 1.83 | 1.52 | 1.25 | 1.15 | 1.09 | 0.87 | 36,289 | 502 |
| `ims-music-64-24` | A-SPX, 24 fps | 2.0 | 1.32 | 1.17 | 0.96 | 0.78 | 0.93 | 0.69 | 28,721 | 488 |
| `ims-music-64-25` | A-SPX, 25 fps | 2.0 | 1.42 | 1.20 | 0.91 | 0.78 | 0.92 | 0.67 | 26,870 | 45 |
| `ims-music-64-2997` | A-SPX, 29.97 fps | 2.0 | 1.86 | 1.54 | 1.25 | 1.13 | 1.12 | 0.87 | 28,934 | 404 |

AC-3 5.1, E-AC-3 5.1 and E-AC-3 7.1.4 through the same images (the second core is not in their path): 0.41, 0.46 and 0.99 of a frame
on `main`; 0.42, 0.43 and 0.97 without the files; 0.35, 0.37 and 0.82 with QIO and the lines; 0.32, 0.33 and 0.75 with
`sdkconfig.s3-fast`; 0.29, 0.33 and 0.75 with `sdkconfig.s3-dcache`.

**What keeps up in real time.** Without the files, with the second core: 2.0 in SIMPLE mode (0.80) and in A-SPX mode (0.99, at the
line). With QIO and the lines, which cost no RAM: those at 0.56 and 0.71, the converter at 24 and 25 fps (0.96 and 0.91) and E-AC-3
7.1.4 at 0.82. With `sdkconfig.s3-fast`: 0.47 and 0.61, the converter at 24 and 25 fps (0.78), E-AC-3 7.1.4 at 0.75; 5.1 SIMPLE
(1.17), 23.976 and 29.97 fps (1.15 and 1.13) stay over. With `sdkconfig.s3-dcache` as well: 2.0 at 0.40 and 0.50, the converter at all
four frame rates (0.87, 0.69, 0.67 and 0.87), 5.1 SIMPLE (0.99, and 0.98 folded to 2.0), E-AC-3 7.1.4 (0.75). **What does not:** the
rest of 5.1 (A-SPX 1.25, A-CPL mode 2 1.40, mode 3 1.57, which is 1.44 with D14h) and 5.1.4 (2.28 to 2.73; 1.96 to 2.38 folded), from
2.1 to 3.2 and 4.3 to 5.7 on `main`.
This part has two thirds of the P4's clock, a quarter of its cache and a slower memory, and its second core is not twice a core when
both share the caches and the bus.

#### A-CPL's interpolation in single precision

D14h (2026-10-11): Pseudocode 109 is evaluated in single precision at the `float` tier, where it was `double`, which on this part is a
call into software for each operation ([the P4 page](esp32-p4.md#a-cpls-interpolation-in-single-precision) has the change and its
hashes, which this part gives too, for all twenty plays, with the second core on and off). With `sdkconfig.s3-fast` and
`sdkconfig.s3-dcache` the 5.1 A-CPL mode 3 frame takes 1.44 of its duration from 1.57 (1.35 from 1.48 folded to 2.0), and the other
nineteen plays move by 3% or less: mode 2 by 1% (1.39 from 1.40, 1.30 from 1.31 folded, and 5.1.4 2.69 from 2.73). The three columns before the last two of the
table above, which are the images without the two files, with QIO and the lines and with `sdkconfig.s3-fast`, were measured before D14h
and have those rows about as much above what the same images give now.

#### The next frame's syntax on the second core

D14i (2026-10-11): `Decoder::decode(frame, next)` reads the next frame's syntax on the second core while this frame is reconstructed, which the
player gives it ([the P4 page](esp32-p4.md#the-next-frames-syntax-on-the-second-core) has the change). With `sdkconfig.s3-fast` and
`sdkconfig.s3-dcache` the frame takes 0 to 8% less (the hash of every one of the forty plays is D14h's, the second core off and on):

| Stream | Codec mode | To | D14h, both cores | One core | Both cores | us/frame | Worst frame ms |
|---|---|---|---:|---:|---:|---:|---:|
| `20-music-192` | SIMPLE | 2.0 | 0.40 | 0.51 | 0.40 | 16,909 | 39 |
| `20-music-96` | A-SPX | 2.0 | 0.51 | 0.61 | 0.50 | 21,378 | 40 |
| `51-music-384` | SIMPLE | 2.0 | 0.98 | 1.29 | 0.94 | 39,938 | 68 |
| `51-music-384` | SIMPLE | 5.1 | 0.98 | 1.30 | 0.94 | 40,201 | 73 |
| `51-music-192` | A-SPX | 2.0 | 1.17 | 1.45 | 1.14 | 48,708 | 70 |
| `51-music-192` | A-SPX | 5.1 | 1.26 | 1.60 | 1.23 | 52,362 | 74 |
| `51-music-128` | A-SPX, A-CPL 2 | 2.0 | 1.30 | 1.58 | 1.25 | 53,416 | 66 |
| `51-music-128` | A-SPX, A-CPL 2 | 5.1 | 1.39 | 1.73 | 1.33 | 56,818 | 73 |
| `51-music-96` | A-SPX, A-CPL 3 | 2.0 | 1.35 | 1.73 | 1.33 | 56,608 | 73 |
| `51-music-96` | A-SPX, A-CPL 3 | 5.1 | 1.44 | 1.90 | 1.41 | 60,100 | 79 |
| `514-music-256` | A-SPX, A-CPL 2 | 2.0 | 2.37 | 2.89 | 2.29 | 97,511 | 118 |
| `514-music-256` | A-SPX, A-CPL 2 | 5.1.4 | 2.69 | 3.36 | 2.61 | 111,540 | 136 |
| `514-music-512` | A-SPX, S-CPL | 2.0 | 2.37 | 2.84 | 2.24 | 95,780 | 125 |
| `514-music-512` | A-SPX, S-CPL | 5.1.4 | 2.68 | 3.31 | 2.56 | 109,086 | 150 |
| `514-music-768` | S-CPL | 2.0 | 1.95 | 2.48 | 1.80 | 76,963 | 129 |
| `514-music-768` | S-CPL | 5.1.4 | 2.26 | 2.94 | 2.13 | 90,939 | 156 |
| `ims-music-64-23976` | A-SPX, 23.976 fps | 2.0 | 0.87 | 1.09 | 0.87 | 36,135 | 502 |
| `ims-music-64-24` | A-SPX, 24 fps | 2.0 | 0.70 | 0.92 | 0.69 | 28,710 | 490 |
| `ims-music-64-25` | A-SPX, 25 fps | 2.0 | 0.67 | 0.92 | 0.67 | 26,884 | 46 |
| `ims-music-64-2997` | A-SPX, 29.97 fps | 2.0 | 0.89 | 1.12 | 0.86 | 28,690 | 405 |

5.1 in SIMPLE mode is 0.94 (0.94 folded to 2.0), A-SPX 1.23, A-CPL mode 2 1.33, mode 3 1.41; 5.1.4 2.13 (S-CPL), 2.61 and
2.56; 2.0 0.40 and 0.50, and the converter 0.87, 0.69, 0.67, 0.86 at 23.976, 24, 25 and 29.97 fps.

### Running it

```bash
. $IDF_PATH/export.sh
tools/checks/run_esp32s3_probe.sh --ac4      # under QEMU, gated
cd firmware/baremetal/platform/esp32s3           # or on a board
idf.py -B build-ac4 -DSDKCONFIG=build-ac4/sdkconfig \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ac4;sdkconfig.hw" build
idf.py -B build-ac4 -p <PORT> flash monitor
```

The runner holds each fixture's hash to its pin, the internal RAM each took to a ceiling (2.0's
6,000 bytes the tightest), and the peak heap, allocations and stack to the Cortex-M3 leg's
ceilings. Each fixture prints `<fixture>.esp32s3.internal_peak_bytes`, `psram_peak_bytes` and
`first_frame_us` beside the probe's other lines. For `hearth_sink`, add `sdkconfig.ac4` to the
board's overlays ([the sink guide](../../hearth/sink-esp32-s3.md#build-and-flash)).

On a board with Wi-Fi, `hearth_sink` with `sdkconfig.ac4` plays an AC-4 stream from an HTTP source
(`POST /play` with the stream's address after `PUT /layout`) and ends the play with the `ac4.*` and
`stream.*` lines the tables above come from, which `GET /log` returns. A play's location can carry
`decoding=core`, `hash=off` and `below=<bytes>` (that play's internal-RAM allocation limit, in place
of `CONFIG_ICLFORGE_AC4_INTERNAL_BELOW`). The stack in PSRAM is the default for this image.

## What the port required from the library

**A `thread_local` that made the library unlinkable on any RTOS.** `eac3_tools.cpp`'s
enhanced-coupling scratch was a 32 KB `thread_local`. FreeRTOS carves each task's thread-local
area out of that task's own stack and sizes it from the linked image's `.tdata + .tbss` — the same
size for every task, including tasks that never call the decoder. ESP-IDF's IPC task has a 1 KB
stack, so it could not be created and the application failed an assert inside `esp_ipc_init()`
before `app_main`. Keeping only a `unique_ptr` in TLS took every task's area from 32 KB to one
pointer, and `.tbss` from 32,784 bytes to 24.

**float32 for the decode path.** The LX7's FPU is single-precision, so `double` coefficients are
wider than anything downstream can use, and memory was the binding constraint: the per-block
`coeffs` store was 100,352 bytes and the AHT's own buffer 86,016 (an AHT stream now decodes into
the per-block store, as [Objects](#objects) says).
`libs/ac3/variants/decode-scalar-{float32,float64}/` carries `decode_scalar_t` — `float` under the
minimum-footprint profile, `double` by default elsewhere, and selectable in any build with
`-DICLFORGE_DECODE_SCALAR=float`. Which profile a build is and which scalar its decoder carries
are independent CMake axes. The option's third value, `fixed`, is the tier for a part with no FPU
at all - an ESP32-C3 or C6 - and the one value the profile honours over its own `float`
default; [docs/building.md](../../building.md#minimum-footprint-decoder-profile) and
[`planning/arithmetic-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/arithmetic-tiers.md)
say what it is and what it measured. It is not this part's tier:
the S3's FPU makes `float` the right arithmetic here. Since 2026-09-09 the arithmetic between the bitstream and those
buffers — mantissa dequantisation, dither, coordinates, decoupling, spectral extension, the AHT
and JOC's mixing — follows the same scalar; it had stayed `double`, which on this FPU is
software, and [Timing](#timing) has what that cost. The output stage's per-sample arithmetic
followed on 2026-09-10 — see [Folded to stereo](#folded-to-stereo).

## Open work

- **Internal RAM in the Sendspin sink.** A board playing over Wi-Fi keeps under 150 bytes of
  internal RAM free at its lowest ([In the Sendspin sink](#in-the-sendspin-sink)). Moving the
  decoder's allocations between 4 and 16 KB to PSRAM made the board too slow, and Wi-Fi and lwIP
  already try PSRAM first. What is left to try is a decoder that makes fewer such allocations.
- **Heap traffic in the decode loop.** The minimum-footprint profile asks for zero; the steady state is 1–27 allocations per
  frame depending on fixture, from per-block geometry vectors and the `std::vector` members of the
  returned `DecodedFrame`. Reaching zero means those becoming fixed-capacity, which changes public
  types. The runner gates at 100 so the distance from zero cannot grow quietly.
- **A vectorised float32 path.** `libs/base/variants/` carries an `f32x4`, but it
  resolves to `arch-generic/` here and compiles to four scalar operations: PIE's vector ALU is
  integer-only. What `esp-dsp` uses instead is `EE.LDF.128.IP`, a 128-bit load filling four FPU
  registers feeding four scalar `madd.s` — load bandwidth and instruction-level parallelism rather
  than a four-wide multiply. That is not reachable from the arch seam, measured rather than
  assumed: `EE.LDF.128.IP` writes a consecutive quad of `f` registers, which GCC's Xtensa port
  cannot model as one value, so it spills every asm block's outputs (68 instructions scalar
  against 73 with 22 spills). Capturing it needs a hand-written assembly kernel tier, like
  `libs/ac3/src/internal/avx2/`, which would also need `madd.s` — a fused multiply-add of exactly
  the kind `-ffp-contract=off` forbids project-wide — and so its own bit-exactness argument.
- **The encoders' search is integer, and it is what is left.** The encoders run in the
  profile's scalar end to end since 2026-09-10 (AC-3 2/0 at 0.38x, E-AC-3 2/0 and AC-3 5.1 at
  the line, E-AC-3 5.1 at 2.5x); what an E-AC-3 5.1 frame spends now is exponent-run planning
  and the rate-control search's allocation calls, integer work whose cost is a count of
  candidates rather than an arithmetic type - see [Encoding](#encoding). The adaptive hybrid
  transform's DCT and vector quantiser are the one `double` island, a quarter of the tools row.

## Where to go next

- [ESP32-P4](esp32-p4.md) — the same component and float tier on a faster part with more RAM,
  and the one that decodes AC-4.
- [ESP32-C6](esp32-c6.md) — the fixed-point tier on a board, with WiFi running.
- [ESP32-C3](esp32-c3.md) — the same component, decoding in the fixed-point tier on a part with
  no FPU at all.
- [ESPHome](esphome.md) — the external component wrapping this decoder for ESPHome projects.
- [Cortex-M3 (QEMU reference)](cortex-m3.md) — the first bare-metal target, with no hardware
  floating point.
- [Bare metal overview](index.md) — how the pages in this section relate.
