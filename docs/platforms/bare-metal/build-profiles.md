# ESP32 build profiles

`hearth_sink`, in `firmware/hearth-sink`, is the firmware an ESP32 dev kit runs as a
[Hearth sink](../../hearth/index.md#esp32-sinks). Every board it is built for has one **build
profile**: a named ESP-IDF configuration, written down in one file, that CI holds every build of
that board to. This page covers what a profile holds, how CI enforces it, how to build a board from
its profile, and how to change one.

The other pages of this section use "profile" for the minimum-footprint build of the codec. A build
profile is the configuration of one board's firmware image.

## What a profile is

ESP-IDF configures a build from Kconfig keys (`CONFIG_...`). A project sets keys in `sdkconfig`
fragment files and gives the build an ordered list of them in `SDKCONFIG_DEFAULTS`. A key that no
fragment sets takes the default its Kconfig file declares. The result is the `sdkconfig` file in
the build directory.

A profile fixes three things for one board:

- the fragments and their order;
- the values of the keys that decide where memory goes and what is built in (the **locked** keys):
  each task stack and ring buffer with its size and whether it is in internal RAM or PSRAM, the
  caches, the flash and PSRAM modes, and the features;
- the **budgets** the built image is held to.

All profiles are in `firmware/hearth-sink/profiles.json`. `tools/hearth/sink_profile.py` is the
only reader: CI, the packager, the bench tools and the profile table in the firmware's README all
go through it. The file's top-level keys:

| Key | Holds |
|---|---|
| `format` | The file's format number, 1. The reader refuses any other |
| `note` | What the file is, for someone reading the JSON |
| `watch` | 25 name patterns. A key in a build's configuration that matches one has to be locked or free ([below](#watch-and-free)) |
| `free` | Four patterns for keys that match `watch` and are left to the build, each with its reason |
| `overlays` | Three named additions a build may take over a profile, each with the keys it may move ([below](#overlays-and-other-fragment-lists)) |
| `other_stacks` | Eight fragment lists the tree spells that are no board's image, each with what it is |
| `profiles` | The seven profiles |

Each profile has seven fields:

| Field | Holds |
|---|---|
| `target` | The ESP-IDF target: `esp32s3`, `esp32c6` or `esp32p4` |
| `board` | The board, in words |
| `fragments` | The fragments after `sdkconfig.defaults`, in order, without the `sdkconfig.` prefix |
| `boards` | The bench boards that run this profile, by name ([below](#the-boards-a-profile-names)) |
| `image` | The release image built from the profile and the rule it is published under (name, flash size, PSRAM, chip revisions), or `null` when no release publishes it |
| `budget` | Upper limits for the app image, the bootloader and static internal RAM |
| `locked` | The values the build's `sdkconfig` must have |

## Why there is one per board and codec set

On 2026-10-12 an HTTP play (`POST /play`) failed on both ESP32-S3 boards and on the ESP32-C6. The
boards ran `main` (`v0.10.0-beta.1-4065`), and CI was green. Sendspin streams played on the same
boards. The S3's console said:

```
heap: heap_caps_malloc could not allocate 32768 bytes (caps 0x804); internal free ..., largest 31744
player: could not start the decode task
```

The cause was a Kconfig default. `ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM` places the 32,768-byte
stack of the task that decodes an HTTP play. Its default put the stack in PSRAM only on an S3 image
that also had the AC-4 decoder. The S3 release image has no AC-4 decoder, so its stack was in
internal RAM, where no free block is over 31,744 bytes once Wi-Fi and the Sendspin player are
running. No fragment had changed. The C6 has no PSRAM, and its ring and stack did not fit beside
Wi-Fi either.

CI did not see it because no job ran the configuration the boards ship. The HTTP play under QEMU
used `sdkconfig.ci-http`, which has a 24,576-byte stack and no PSRAM, and the board images were
built and never played.

A profile is per board and per codec set for three reasons:

- The same fragment list can resolve to different placements when a default depends on another
  feature. AC-4 builds are therefore profiles of their own where the decoder moves a placement (`s3-n16r8-ac4`); on the P4 the AC-4 decoder
  is in both release profiles since 2026-10-29, its values locked where the AC-4 profile held them.
  `sdkconfig.ac4` raises both decode stacks to 40,960 bytes, and on the S3 `sdkconfig.s3-dcache`
  takes 32 KB of heap for a 64 KB data cache.
- A board's flash size and partition table cannot change over the network, so a C6 with 4 MB of
  flash and one with 16 MB are two profiles and two release images.
- An ESP32-P4 of chip revision v1.x and one of v3.x refuse each other's images at boot, so each
  revision has a profile.

The S3's default was fixed in
[#1387](https://github.com/iainchesworthlabs/iclforge/pull/1387), and `sdkconfig.psram` now sets
the key as well. The C6 refuses an HTTP play it has no room for and says `memory` as the reason
([#1395](https://github.com/iainchesworthlabs/iclforge/pull/1395)). The profiles came in the pull
requests after those: the file, its reader and the guard
([#1403](https://github.com/iainchesworthlabs/iclforge/pull/1403)), CI building from the file
([#1406](https://github.com/iainchesworthlabs/iclforge/pull/1406)), the profile id in the image and
the boot budget line ([#1412](https://github.com/iainchesworthlabs/iclforge/pull/1412)) and the v3.x
P4 profile ([#1452](https://github.com/iainchesworthlabs/iclforge/pull/1452)). The design record is
[`planning/esp32-platform-profiles.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-platform-profiles.md)
([#1369](https://github.com/iainchesworthlabs/iclforge/pull/1369)); it also has the board suite and
its figures, which this page leaves out.

## The profiles

| Profile | Target | Board | Release image | Bench boards |
|---|---|---|---|---|
| `s3-n16r8` | `esp32s3` | ESP32-S3-DevKitC-1 N16R8: 16 MB of flash, 8 MB of octal PSRAM | `hearth-sink-esp32s3` | S3a, S3b |
| `s3-n16r8-ac4` | `esp32s3` | The same board, with the AC-4 decoder | none | S3a, S3b |
| `c6-16mb` | `esp32c6` | An ESP32-C6 with 16 MB of flash, no PSRAM | `hearth-sink-esp32c6-16mb` | C6 |
| `c6-4mb` | `esp32c6` | Any ESP32-C6 module: 4 MB of flash | `hearth-sink-esp32c6` | none |
| `p4-rev1` | `esp32p4` | An ESP32-P4 of chip revision v1.x, such as the FireBeetle 2, with the AC-4 decoder | `hearth-sink-esp32p4-rev1` | P4a, P4b |
| `p4-rev3` | `esp32p4` | An ESP32-P4 of chip revision v3.x with the lyric screen: the ESP32-P4X-Function-EV-Board and its 7-inch MIPI DSI panel | `hearth-sink-esp32p4-rev3` | none |

There is no AC-4 profile for the C6: the decoder does not fit beside Wi-Fi on that part
([An ESP32-C6 sink](../../hearth/sink-esp32-c6.md)). The release images have the AC-4 decoder off.
`p4-rev3` has been built and has not run on a board.

`python tools/hearth/sink_profile.py list` prints the same list from the file, with each profile's
full `SDKCONFIG_DEFAULTS` string.

What each board plays and how fast is on the board pages:
[An ESP32-S3 sink](../../hearth/sink-esp32-s3.md), [An ESP32-C6 sink](../../hearth/sink-esp32-c6.md)
and [An ESP32-P4 sink](../../hearth/sink-esp32-p4.md) for the sink, and [ESP32-S3](esp32-s3.md),
[ESP32-C6](esp32-c6.md) and [ESP32-P4](esp32-p4.md) for the measurements.
[Sink firmware](../../hearth/sink-firmware.md#which-image) has which release image goes on which
board.

### The boards a profile names

`boards` lists the dev kits on the project's bench that run the profile, by the names
`tools/hearth/bench.json` gives them: two ESP32-S3 boards (S3a, S3b), one ESP32-C6 (C6) and two
ESP32-P4 boards (P4a, P4b). A test holds every name to a row of that file with the profile's
chip. Two profiles name no board: `c6-4mb`, whose build differs from `c6-16mb` in the flash size
and the partition table alone, and `p4-rev3`, which no board has run.

The two AC-4 profiles name the same boards as the profiles without AC-4. A board runs one of them
at a time, and [which one](#which-profile-a-running-board-is) is read from the board.

## The fragments and their order

Every profile starts with `sdkconfig.defaults` and adds its `fragments` after it:

| Profile | Fragments after `sdkconfig.defaults`, in order |
|---|---|
| `s3-n16r8` | `hw`, `psram`, `sendspin`, `s3-fast` |
| `s3-n16r8-ac4` | `hw`, `psram`, `sendspin`, `ac4`, `s3-fast`, `s3-dcache` |
| `c6-16mb` | `hw`, `sendspin`, `c6`, `sendspin-c6`, `flash16mb` |
| `c6-4mb` | `hw`, `sendspin`, `c6`, `sendspin-c6` |
| `p4-rev1` | `hw`, `p4`, `sendspin`, `ac4` |
| `p4-rev3` | `hw`, `p4`, `p4-rev3`, `sendspin`, `panel`, `panel-dsi` |

ESP-IDF reads the files left to right, and a later file's value replaces an earlier one's. The
order decides values in several places:

- `sdkconfig.sendspin` pins the decode task to core 1, and `sdkconfig.c6` after it removes the
  pin, because the C6 has one core.
- `sdkconfig.c6` chooses 4 MB of flash and `partitions_c6.csv`, and `sdkconfig.flash16mb` after it
  chooses 16 MB and `partitions.csv`.
- `sdkconfig.p4` admits chip revisions v1.x at 360 MHz, and `sdkconfig.p4-rev3` after it admits
  v3.01 to v3.99 at 400 MHz.

The guard compares the list exactly, so a build given the right files in another order fails,
whether or not the two files share a key.

| Fragment | In | What it sets |
|---|---|---|
| `sdkconfig.defaults` | every profile | Optimisation for size, an 8,192-byte main task stack, 16 MB of flash, the `partitions.csv` table with two application slots, bootloader rollback, 240 MHz |
| `sdkconfig.hw` | every profile | The console on the chip's USB-Serial-JTAG port |
| `sdkconfig.psram` | S3 | Octal PSRAM at 80 MHz, usable by `malloc`, with allocations of 16 KB and over tried there first; the I2S DMA queue; a 32 KB instruction cache; the HTTP play's decode stack in PSRAM; no core dump |
| `sdkconfig.sendspin` | every profile | The Sendspin player, Wi-Fi, the HTTP source and the control surface on port 80, nothing played at boot, 16 sockets, the decode task on core 1 and lwIP on core 0 |
| `sdkconfig.s3-fast` | S3 | QIO flash, 64-byte data-cache lines, code and constants run from PSRAM |
| `sdkconfig.s3-dcache` | `s3-n16r8-ac4` | A 64 KB data cache |
| `sdkconfig.ac4` | the AC-4 profiles | The AC-4 decoder, both decode stacks at 40,960 bytes, a frame's per-channel stages on both cores |
| `sdkconfig.c6` | C6 | The target, 160 MHz, QIO flash, 4 MB of flash with `partitions_c6.csv`, no core pinning |
| `sdkconfig.sendspin-c6` | C6 | The player sized for a part with no PSRAM: a 49,152-byte Sendspin ring, 4,096-byte chunks, a 24,576-byte Sendspin decode stack, no lyrics and no delay lines, two coded channels at most, 16-bit I2S slots, the floor under which an HTTP play is refused, Opus, and Wi-Fi's code kept out of IRAM |
| `sdkconfig.flash16mb` | `c6-16mb` | 16 MB of flash with `partitions.csv` |
| `sdkconfig.p4` | P4 | The target, chip revisions v1.x, 360 MHz, QIO flash, the wide I2S sink and its pins, PSRAM usable by `malloc`, code run from PSRAM, the hosted Wi-Fi buffers in PSRAM, the low-power SRAM kept out of the heap |
| `sdkconfig.p4-rev3` | `p4-rev3` | Chip revisions v3.01 to v3.99, 400 MHz, the bootloader's log level at Warn so that it fits its window |
| `sdkconfig.panel` | `p4-rev3` | LVGL with its heap in PSRAM, TrueType fonts and their caches, right-to-left text |
| `sdkconfig.panel-dsi` | `p4-rev3` | The MIPI DSI panel, and a read-back route on port 8081 |

Each fragment's own header comment has the reason and the measurement behind every line.

## What `locked` holds

`locked` maps a key to the value the build's `sdkconfig` must have. On 2026-10-13 the profiles
lock 95 keys (the two C6 profiles) to 136 (`p4-rev3`); 80 keys are locked in every profile.

`sink_profile.py snapshot` writes the map from a configured build. It takes two sets of keys:

- every key one of the profile's fragments sets, 38 to 50 of them;
- every key in the resolved configuration that matches a `watch` pattern and is on or has a value.

So most locked values are ones no fragment sets. They are defaults of ESP-IDF and of the project's
own Kconfig, recorded so that a default that changes is seen. That is the case of 2026-10-12.

The classes of key, with examples (names without `CONFIG_`):

| Class | Examples |
|---|---|
| Chip, flash and partition table | `IDF_TARGET`, `ESPTOOLPY_FLASHSIZE`, `ESPTOOLPY_FLASHMODE_QIO`, `PARTITION_TABLE_CUSTOM_FILENAME`, `ESP_DEFAULT_CPU_FREQ_MHZ_240`, `ESP32P4_REV_MIN_FULL`, `BOOTLOADER_APP_ROLLBACK_ENABLE` |
| PSRAM and heap policy | `SPIRAM`, `SPIRAM_MODE_OCT`, `SPIRAM_USE_MALLOC`, `SPIRAM_MALLOC_ALWAYSINTERNAL`, `SPIRAM_FETCH_INSTRUCTIONS`, `ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`, `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`, `ESP_COREDUMP_ENABLE_TO_NONE` |
| Caches | `ESP32S3_DATA_CACHE_SIZE`, `ESP32S3_DATA_CACHE_LINE_64B`, `ESP32S3_INSTRUCTION_CACHE_32KB` |
| Code kept in IRAM | `ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT`, `ESP_PHY_IRAM_OPT` |
| Task stacks, size and placement | `ESP_MAIN_TASK_STACK_SIZE`, `ICLFORGE_EXAMPLE_DECODE_STACK_BYTES`, `ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM`, `ICLFORGE_EXAMPLE_SENDSPIN_DECODE_STACK_BYTES`, `ICLFORGE_EXAMPLE_SENDSPIN_SERVER_STACK_BYTES`, and ESP-IDF's own (`ESP_TIMER_TASK_STACK_SIZE`, `LWIP_TCPIP_TASK_STACK_SIZE`, `MDNS_TASK_STACK_SIZE`) |
| Rings, queues and buffers | `ICLFORGE_EXAMPLE_RING_BYTES`, `ICLFORGE_EXAMPLE_RING_IN_PSRAM`, `ICLFORGE_EXAMPLE_SENDSPIN_RING_BYTES`, `ICLFORGE_EXAMPLE_SENDSPIN_MAX_CHUNK_BYTES`, `ICLFORGE_EXAMPLE_I2S_DMA_DESCRIPTORS`, `LWIP_MAX_SOCKETS` |
| Cores | `ICLFORGE_EXAMPLE_DECODE_CORE`, `ICLFORGE_EXAMPLE_FETCH_CORE`, `ICLFORGE_EXAMPLE_DECODE_LANE`, `LWIP_TCPIP_TASK_AFFINITY_CPU0` |
| Features | `ICLFORGE_SENDSPIN`, `ICLFORGE_AC4`, `ICLFORGE_SENDSPIN_OPUS`, `ICLFORGE_EXAMPLE_SENDSPIN_AC4`, `ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES` |

The keys behind the 2026-10-12 failure, as the file locks them on 2026-10-13 (names after
`ICLFORGE_EXAMPLE_`; the file is the authority):

| Key | `s3-n16r8` | `s3-n16r8-ac4` | `c6-16mb`, `c6-4mb` | `p4-rev1` | `p4-rev3` |
|---|---|---|---|---|---|
| `DECODE_STACK_BYTES` | 32768 | 40960 | 32768 | 40960 | 32768 |
| `DECODE_STACK_IN_PSRAM` | 1 | 1 | 0 | 0 | 0 |
| `SENDSPIN_DECODE_STACK_BYTES` | 32768 | 40960 | 24576 | 40960 | 32768 |
| `SENDSPIN_DECODE_STACK_IN_PSRAM` | 1 | 1 | 0 | 0 | 0 |
| `SENDSPIN_RING_BYTES` | 262144 | 262144 | 49152 | 262144 | 262144 |
| `PLAY_MIN_FREE_BYTES` | 0 | 0 | 160796 | 0 | 0 |

Two values need a word:

- `n` locks a key off. A key that is missing from the configuration also satisfies it.
- `<absent>` means the key must not be in the configuration at all. `snapshot` writes it when a
  fragment asks for a key and Kconfig drops it, as `p4-rev3` does for `ESP32P4_REV_MIN_100`:
  `sdkconfig.p4` asks for it and `sdkconfig.p4-rev3` chooses another revision.

### The profile's hash

`sink_profile.py identity <profile>` prints the id and a 12-digit hash: the first 12 hex digits of
a SHA-256 over the profile's `fragments` and `locked`. Any edit to either changes it. The budgets,
the board names and the image rule are not part of it.

```
$ python tools/hearth/sink_profile.py identity s3-n16r8
s3-n16r8 156f7310e8d0
```

With `--cmake` the command prints `-DICLFORGE_PROFILE=<id> -DICLFORGE_PROFILE_HASH=<hash>`. A
build configured with those two options compiles them into the image, and the board reports them
([below](#which-profile-a-running-board-is)).

## `watch` and `free`

`watch` is a list of shell-style patterns, matched against a key's name without `CONFIG_`. They
name the kinds of key that place or size something:

| Kind | Patterns |
|---|---|
| Stacks and placement | `*STACK*`, `*IN_PSRAM*` |
| The player's rings, queues and cores | `ICLFORGE_EXAMPLE_*RING*`, `ICLFORGE_EXAMPLE_I2S_DMA_*`, `ICLFORGE_EXAMPLE_*_CORE`, `ICLFORGE_EXAMPLE_DECODE_LANE`, `ICLFORGE_EXAMPLE_HOLD_FIRST_UNIT`, `ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES` |
| Sendspin and AC-4 | `ICLFORGE_EXAMPLE_SENDSPIN_*`, `ICLFORGE_SENDSPIN*`, `ICLFORGE_EXAMPLE_AC4_*`, `ICLFORGE_AC4*` |
| PSRAM, caches and IRAM | `SPIRAM*`, `ESP32S3_*CACHE*`, `*IRAM_OPT*`, `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`, `ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP` |
| Flash, table and chip | `ESPTOOLPY_FLASHSIZE*`, `ESPTOOLPY_FLASHMODE*`, `PARTITION_TABLE_CUSTOM_FILENAME`, `ESP32P4_*REV*`, `ESP_DEFAULT_CPU_FREQ_MHZ_*` |
| Bootloader and core dump | `BOOTLOADER_APP_ROLLBACK_ENABLE`, `BOOTLOADER_LOG_LEVEL_*`, `ESP_COREDUMP_ENABLE_TO_*` |

A key in a build's configuration that matches a pattern, and is on or has a value, has to be in
the profile's `locked` or match a `free` pattern. Otherwise the guard reports it as `[unplaced]`.
A new stack or buffer option then cannot reach a board without someone deciding its value for that
board.

`free` holds the exceptions, each with its reason in the file:

| Pattern | Why it is free |
|---|---|
| `ICLFORGE_EXAMPLE_WIFI_*` | A board's network is its own and is kept in NVS. A published image has none built in |
| `ICLFORGE_EXAMPLE_HTTP_URL` | The stream a build plays at boot: empty on a board, a test server's address in a CI build |
| `ICLFORGE_EXAMPLE_PANEL_STACK_BYTES` | The screen task's stack. It is declared for every target and does nothing in a profile with no screen. `p4-rev3` locks it by hand |
| `LV_SPAN_SNIPPET_STACK_SIZE` | An LVGL count of text snippets, which matches `*STACK*` and is no task's stack |

A key that is both free and locked is held to the locked value. `snapshot` skips free keys, so a
lock like `p4-rev3`'s is added by hand after a snapshot.

## Overlays and other fragment lists

An **overlay** is a declared addition to a profile's build. It lists the key patterns it may move
(`touches`) and may name one more fragment, which goes after the profile's own. A build made with
an overlay is verified with the same `--overlay` name, and the guard then skips the keys the
overlay touches.

| Overlay | Fragment | What it is for |
|---|---|---|
| `wifi-credentials` | a file of the board's network, given last (`defaults --wifi FILE`) | A build for a bench board. The packager refuses to publish an image with a network built in |
| `measure` | `sdkconfig.measure` | Turns on the PCM hash an AC-4 play prints, so a board's output can be compared with the host's |
| `panel-capture` | `sdkconfig.panel-capture` | The P4 profile with a screen, with its read-back and touch server on 8081 (no authentication), for a bench; a release image has it off and the packager refuses one with it on |
| `qemu-net` | `sdkconfig.qemu-net` | The S3 release profile under QEMU: Ethernet for Wi-Fi, the capture sink for I2S, quad PSRAM for octal, the console on UART0 |

`other_stacks` lists fragment lists that appear in the documentation and in fragment headers and
are no board's image: the bare-metal probes' builds, the lists a fragment's header gives as its
own example, and the P4 build that draws the screen into a frame buffer with no panel.
`sink_profile.py check-docs` reads the firmware's README and fragments, the Hearth pages, the
ESP32 platform pages and the ESP32 planning notes, and fails on a `sdkconfig.defaults;sdkconfig...`
list that is neither a profile's, nor listed here, nor a CI test configuration (`sdkconfig.ci*`).
The same command with `--write` regenerates the profile table in the firmware's README.

## Budgets

`budget` has three upper limits in bytes. `null` means the figure is not held.

| Profile | `app_bytes_max` | `bootloader_bytes_max` | `internal_static_bytes_max` |
|---|---|---|---|
| `s3-n16r8` | 1,750,000 | 32,768 | 185,000 |
| `s3-n16r8-ac4` | not held | 32,768 | not held |
| `c6-16mb` | 2,000,000 | 32,768 | 117,000 |
| `c6-4mb` | 1,966,080 | 32,768 | 117,000 |
| `p4-rev1` | 3,400,000 | 24,576 | not held |
| `p4-rev3` | 2,900,000 | 24,576 | 175,000 |

- **The app image** is `iclforge_hearth_sink.bin` in the build directory. `c6-4mb`'s limit is the
  size of an application slot in `partitions_c6.csv`.
- **The bootloader** is `bootloader/bootloader.bin`. On the P4 it starts at `0x2000` and the
  partition table is at `0x8000`, which leaves it 24,576 bytes.
- **Static internal RAM** is the `DIRAM` figure of `idf.py size --format json2`. Every byte of it is
  a byte the heap does not have at run time, on any board. The guard judges it only when it is
  given the size report (`--size-json`).

The budgets need a compiled build. A configured build is checked for everything else.

## The guard

`sink_profile.py verify <profile> --build-dir <dir>` holds a build directory to its profile. It
works on a directory that has been configured (`idf.py reconfigure`) or built. Each finding is one
line, `<profile>: [rule] message`, and the command exits 1. With none it prints
`<profile>: the build holds to its profile` and exits 0.

| Rule | Reported when | The message, for example |
|---|---|---|
| `fragments` | The `SDKCONFIG_DEFAULTS` in the build's `CMakeCache.txt` is not the profile's list, in order, with the fragments of the overlays named; or it has a file that no named overlay declares | `the build was given sdkconfig.defaults;sdkconfig.hw;... and the profile's are ...` |
| `identity` | The build carries no profile id, another profile's, or a hash from another definition of this one | `the build carries hash 9ed6f4eb5c70 and profile s3-n16r8 is 156f7310e8d0: it was configured from another definition` |
| `locked` | A locked key has another value in `sdkconfig` | `CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM is 0, the profile locks 1` |
| `unplaced` | A watched key is on or has a value, and the profile neither locks nor frees it | `CONFIG_ICLFORGE_EXAMPLE_NEW_STACK_BYTES=4096 matches a watch pattern and the profile neither locks nor frees it` |
| `budget` | The app image, the bootloader or static internal RAM is over its limit | `static internal RAM (DIRAM) is 190,112 bytes, over the profile's 185,000` |

`--github` prints the findings as GitHub annotations. `--warn-only` reports them as warnings and
exits 0.

## How CI enforces a profile

| Where | When | What it holds |
|---|---|---|
| PR Gate, the static checks (`_static.yml`) | Every pull request and merge-queue entry | `check-docs`; the tools' own tests, which hold `profiles.json` to the workflow's build steps, to the release rules in `tools/ci/check_firmware_package.py` and to the baselines; and `check-change` for the Kconfig ([below](#a-new-kconfig-key)) |
| PR Gate, the `ESP32 profiles` job (`_profiles.yml`) | A pull request that changes `firmware/hearth-sink/`, `firmware/esp-idf/` or the two profile tools; every merge-queue entry and push to `main` | Every profile configured with `idf.py reconfigure` in the `espressif/idf:v6.1` container and passed to `verify`: the fragments, the identity, the locked values and the unplaced keys. Nothing is compiled, so no budget is judged |
| The build jobs of `_build.yml`, called by `ci.yml` | After a merge to `main` whose changes reach the ESP builds, in the nightly run and in a dispatched run | Each profile built, measured with `idf.py size` and passed to `verify` with the budgets; the release images packaged |

The `ESP32 profiles` job is where a pull request that changes a fragment's value or the default of
a locked key fails before it merges, with the guard's own `[locked]` or `[unplaced]` line
([#1506](https://github.com/iainchesworthlabs/iclforge/pull/1506)). It also checks merge-queue
entries, which is where two changes that each pass alone first meet.

### The build jobs

Each board build in `_build.yml` is the same five commands with the profile's id:

1. `sink_profile.py defaults <profile>` gives the `SDKCONFIG_DEFAULTS` string.
2. `sink_profile.py identity <profile> --cmake` gives the two `-D` options that put the id and the
   hash in the image.
3. `idf.py ... build` builds into a directory of its own, with its own `sdkconfig`.
4. `idf.py size --format json2` writes the size report.
5. `sink_profile.py verify <profile> --size-json ...` holds the build to the profile, and a
   finding fails the step.

A profile with a release image then goes to
`tools/hearth/package_firmware.py package --profile <profile>`, which runs the guard again and
refuses a build that is not the profile, or a profile whose image has another name.

| Profile | Job in `_build.yml` | What CI does with it |
|---|---|---|
| `s3-n16r8` | `build-esp32s3` | Built, verified, packaged as `hearth-sink-esp32s3`. Built a second time with the `qemu-net` overlay and run under QEMU |
| `s3-n16r8-ac4` | `build-esp32s3` | Built and verified |
| `c6-4mb` | `build-esp32c3` | Built, verified, packaged as `hearth-sink-esp32c6` |
| `c6-16mb` | `build-esp32c3` | Built, verified, packaged as `hearth-sink-esp32c6-16mb` |
| `p4-rev1` | `build-esp32c3` | Built, verified, packaged as `hearth-sink-esp32p4-rev1` |
| `p4-rev3` | `build-esp32c3` | Built, verified, packaged as `hearth-sink-esp32p4-rev3` |
| `p4-rev1` (again) | `build-esp32c3` | Built with the stage timers on, and verified |

The QEMU run of `s3-n16r8` is the one place CI plays a board's release configuration. It checks
that `GET /hardware` reports the profile's id and hash, plays an AC-3 and two E-AC-3 streams over
`POST /play`, and looks on the console for the lines that say the rings and stacks are where the
profile locks them (`sink_profile.py expect-console s3-n16r8` prints those lines). QEMU has no
Wi-Fi, so it says nothing about the heap a board has left. The C6 and the P4 have no emulator and
are built without being run.

The job `package-esp32-firmware` then writes the manifest and runs
`tools/ci/check_firmware_package.py` over every image: chip, revisions, flash size, PSRAM and no
network built in. A test holds those rules to each profile's `image`.

Two details of the jobs:

- Every guard step has `if: !cancelled()`, so it runs when an earlier step of its job has failed
  ([#1491](https://github.com/iainchesworthlabs/iclforge/pull/1491),
  [#1499](https://github.com/iainchesworthlabs/iclforge/pull/1499)). The sink builds share their
  jobs with the minimum-footprint probes. While a probe was failing on `main`, the steps after it
  were skipped, and a wrong static-RAM budget on the C6 went unreported until a run on a board
  showed it.
- The repository variable `ESP32_PROFILE_GUARD=warn` turns the guard's findings into warnings in
  all of these jobs. It is not set.

### A new Kconfig key

A pull request that adds a Kconfig key the guard watches has to place it in the profiles in the
same pull request ([#1504](https://github.com/iainchesworthlabs/iclforge/pull/1504)). The static
checks run:

```bash
python tools/hearth/sink_profile.py check-change --base <base commit>
```

It needs no ESP-IDF. `tools/hearth/kconfig_scan.py` reads the two Kconfig files a `hearth_sink`
build configures, `firmware/hearth-sink/main/Kconfig.projbuild` and
`firmware/esp-idf/iclforge/Kconfig`, at the head and at the base. The command reports two things:

- **A key the change adds** that matches a `watch` pattern, is not `free`, and is in the
  configuration of a profile that does not lock it. Whether a key is in a profile's configuration
  is worked out from its `depends on` lines and the `if` blocks around it, against the profile's
  target and locked values. A bool counts only when it has a plain `default y`.
- **A lock on a key no Kconfig declares**: a `CONFIG_ICLFORGE_*` key locked to something other than
  `n`. That is a key that was removed, or a misspelt lock.

The message names the file, the key and the profiles. For a new `int` key with a default, on one
line:

```
kconfig: [kconfig] firmware/hearth-sink/main/Kconfig.projbuild: CONFIG_ICLFORGE_EXAMPLE_NEW_STACK_BYTES
is new and the guard watches it, and it is in c6-16mb, c6-4mb, p4-rev1, p4-rev3,
s3-n16r8, s3-n16r8-ac4, which neither lock nor free it: lock its value in profiles.json (the hash
moves: baselines are re-recorded or accepted, planning/esp32-platform-profiles.md 'Changing a
profile'), or name it in `free`
```

The fix is one of two edits to `profiles.json`: lock the key in every profile named, at the value
that profile's build resolves, or add it to `free` with the reason. Locking a key changes the
profile's hash, so the baselines need a decision too ([below](#baselines)).

Where the scan cannot tell whether a key is in a profile (a condition on a symbol the profile does
not lock), it reports nothing, and the `ESP32 profiles` job decides from the configured build.
`check-change --all` treats every key as new, which checks the whole tree.

## Which profile a running board is

`GET /hardware` answers it:

```bash
curl http://<board>/hardware
```

| Field of the reply | Holds |
|---|---|
| `profile` | The profile's id, such as `s3-n16r8` |
| `profile_hash` | The hash of the definition the image was built from |
| `version` | The image's version: a release's tag, or `git describe` of the checkout it was built from |
| `target`, `chip`, `revision`, `psram_bytes` | The chip, its revision and its PSRAM |

Compare `profile` and `profile_hash` with `sink_profile.py identity <profile>` in your checkout:

| `/hardware` says | Meaning |
|---|---|
| The id and the same hash | The image was built from the profile as your checkout defines it |
| The id and another hash | The image was built from another definition of the profile: an older or a newer `profiles.json` |
| Two empty strings | The image was built without the identity options, for example with the plain `idf.py` commands on the board pages |

Two more places help:

- **The boot line.** At boot, once the Sendspin player has started, the board prints one
  `budget:` line on its console, which `GET /log` also returns, and another if a network comes up
  later. It does not name the profile. It shows what the profile's placement asks of internal RAM
  on this boot. The line has this form:

    ```
    budget: when=boot net=1 internal_free=90931 largest=31744 ask_ring=0 ask_decode_stack=0 ask_fetch_stack=8192 ask_total=8192 ask_biggest=8192 headroom=23552 floor=0 verdict=ok
    ```

    `ask_ring` and `ask_decode_stack` are 0 when the profile puts the HTTP play's ring and stack in
    PSRAM. `floor` is the profile's `ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES`. `verdict` is `ok`,
    `short-block` (no free block as large as the biggest ask), `short-total` (not that much free in
    all) or `below-floor` (an HTTP play would be refused, which is what the C6 profiles say). The
    line is a lower bound: the decoder's own allocations come after it.

- **`GET /firmware`.** It reports the image in each of the two application slots (`running` and
  `other`, each with its `version` and `state`), the flash size and the partition table. It has no
  profile id. It tells a 4 MB C6 from a 16 MB one, and shows which build is in each slot.

## Building a board from its profile

These are CI's commands. They need ESP-IDF v6.1 and a checkout of the repository. Give each
profile its own build directory and its own `sdkconfig` in it, as below: ESP-IDF otherwise keeps
one `sdkconfig` in the project directory, and a second profile's build inherits the first's.
Build one profile at a time from one checkout, because the component manager downloads into the
project directory.

The target is the profile's, from [the table](#the-profiles).

### In WSL or on Linux

```bash
. $IDF_PATH/export.sh
cd firmware/hearth-sink

id=s3-n16r8
target=esp32s3
dir=$HOME/build/hearth-$id

defaults="$(python3 ../../tools/hearth/sink_profile.py defaults $id)"
read -r -a identity <<< "$(python3 ../../tools/hearth/sink_profile.py identity $id --cmake)"

idf.py -B "$dir" -DSDKCONFIG="$dir/sdkconfig" -DIDF_TARGET=$target \
  "-DSDKCONFIG_DEFAULTS=$defaults" "${identity[@]}" build
idf.py -B "$dir" size --format json2 > "$dir/size.json"
python3 ../../tools/hearth/sink_profile.py verify $id --build-dir "$dir" --size-json "$dir/size.json"
```

### On Windows

Use PowerShell 7 (`pwsh`). ESP-IDF refuses Git Bash and other MSYS shells
(`MSys/Mingw is not supported`), and the size report has to be written as UTF-8, which
PowerShell 7's `>` does.

```powershell
. $env:IDF_PATH\export.ps1
cd firmware\hearth-sink

$id = "s3-n16r8"
$target = "esp32s3"
$dir = "D:\build\hearth-$id"

$defaults = python ..\..\tools\hearth\sink_profile.py defaults $id
$identity = -split (python ..\..\tools\hearth\sink_profile.py identity $id --cmake)

idf.py -B $dir "-DSDKCONFIG=$dir\sdkconfig" "-DIDF_TARGET=$target" "-DSDKCONFIG_DEFAULTS=$defaults" @identity build
idf.py -B $dir size --format json2 > "$dir\size.json"
python ..\..\tools\hearth\sink_profile.py verify $id --build-dir $dir --size-json "$dir\size.json"
```

### After the build

`verify` should print `<profile>: the build holds to its profile`. To check the configuration
without compiling, replace `build` with `reconfigure` and run `verify` without `--size-json`. That
is what the `ESP32 profiles` job does, and it judges everything but the budgets.

Flash the build with `idf.py -B <dir> -p <PORT> flash monitor`. The board pages cover the port, the
reset after a flash, joining a network and pairing: [An ESP32-S3 sink](../../hearth/sink-esp32-s3.md#build-and-flash),
[An ESP32-C6 sink](../../hearth/sink-esp32-c6.md#build-and-flash) and
[An ESP32-P4 sink](../../hearth/sink-esp32-p4.md#build-and-flash). A profile build has no network
built in. A board keeps the network it has stored through a flash, and a new board is given one
as those pages describe.

The plain `idf.py` commands on those pages build from the same fragment lists. The image they make
reports no profile, and `verify` fails it under `identity`.

`python tools/hearth/bench_flash.py build <profile> --tag <tag>` runs the same steps and can be
started from any shell, Git Bash included. It reads the ESP-IDF location and the build root from
`tools/hearth/bench.json`, which describes the project's own bench; the environment variable
`ICLFORGE_BENCH` names another file of the same form for another machine.
[Testing and benchmarking the ESP32 boards](board-bench.md#the-bench-tools) covers that tool and
the rest of the bench.

## Changing a profile

A change to a profile is one pull request that carries the fragment or Kconfig change and the
matching edit to `profiles.json`. That covers a new value in a fragment, a fragment added to or
moved in a list, a new default for a locked key, and a new key.

1. **Make the change and configure each affected profile**, with the commands above and
   `reconfigure`. A change to `sdkconfig.sendspin` or to a Kconfig default can reach every
   profile.
2. **Read what the guard reports.** Each line is a decision to make:

    | The guard says | What changed | What to do |
    |---|---|---|
    | `[locked] CONFIG_X is A, the profile locks B` | The value a board gets | Lock the new value if it is meant; otherwise fix the fragment or the default |
    | `[unplaced] CONFIG_X=A matches a watch pattern ...` | A new key that places or sizes something | Lock it in the profile, or name it in `free` with the reason |
    | `[fragments] the build was given ... and the profile's are ...` | The list or its order | Edit the profile's `fragments`, and the build step if a file was added |
    | `[budget] ... over the profile's ...` | The image grew | Reduce it, or raise the budget in the same pull request and say why |

3. **Update `profiles.json`**, by hand or with
   `sink_profile.py snapshot <profile> --build-dir <dir> --write`, which rewrites that profile's
   `locked` from the configured build. It prints a `note:` for every key a fragment asks for that
   the build resolved differently. Review the diff: a snapshot drops a hand-made lock on a free
   key.
4. **Configure again.** The hash has changed, so the first build directory now fails under
   `identity`. A fresh `identity --cmake` and `reconfigure` should hold.
5. **Regenerate the README's table** when the fragments, the image or the boards changed:
   `python tools/hearth/sink_profile.py check-docs --write`.
6. **Run the tools' tests**: `python -m unittest discover --start-directory tools/hearth --pattern 'test_*.py'`.
   They hold the file to the workflow, the release rules and the baselines.

### Baselines

`firmware/hearth-sink/baselines/` has one file for each profile a bench board runs: `s3-n16r8`,
`s3-n16r8-ac4`, `c6-16mb` and `p4-rev1`. A baseline is a recorded run of the board
suite (`tools/hearth/board_suite.py`): the figures of a fixed list of plays on that profile's
image, and the board's boot line. `c6-4mb` and `p4-rev3` have none, because no bench board runs
them. [Testing and benchmarking the ESP32 boards](board-bench.md#the-play-suite) describes the
suite, its tolerances and how a baseline is recorded.

Each baseline carries the hash of the profile it was recorded under, and an edit to the profile
changes the hash. A test fails the pull request until it does one of two things for every
baseline whose hash no longer matches:

| The edit | What the pull request carries |
|---|---|
| A locked value changed | The baseline recorded again on a board, from an image built under the new hash (`board_suite.py run ... --record`) |
| A key was added at the value it already had, so no figure can have moved | An entry `profiles.<profile>.baseline_accepted` in `firmware/hearth-sink/suite.json` with the profile's new hash and the reason |

An acceptance names one hash. The next edit to the profile changes the hash again and the test
asks again. When a baseline is later re-recorded, its acceptance is removed; the test fails on one
that is left behind.

The suite refuses to run on a board whose `/hardware` reports another profile or another hash, so
a baseline is always recorded from the definition in the checkout.

### Adding a profile

Add the entry with an empty `locked`, configure it, and run `snapshot --write` to fill it; `verify`
says `locks no key: run snapshot` until then. Add its build step to `_build.yml` in the form of
the others, and its id to the list in `tools/hearth/test_sink_profile.py` that holds the workflow
to the file. The `ESP32 profiles` job picks the new profile up from `sink_profile.py list`.

## Where to go next

- [`planning/esp32-platform-profiles.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-platform-profiles.md)
  is the design record, with the board suite, its tolerances and each baseline's figures.
- [The firmware's README](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/README.md#build-profiles)
  has the generated profile table and everything else about `hearth_sink`.
- [Testing and benchmarking the ESP32 boards](board-bench.md) covers what is tested where, the
  play suite, the baselines and the bench tools.
- [Sink firmware](../../hearth/sink-firmware.md) covers installing a release image and updating a
  board over its network.
- [Bare metal](index.md) is the overview of the parts.
