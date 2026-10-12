# ESP32 platform profiles: one locked build per board, and a suite that plays audio on it

**Status, 2026-10-12, later the same day:** the design was merged (#1369) and the owner approved the
first steps. Built: the S3 fix (#1387, verified on S3a and S3b), the C6's clean refusal (#1395,
verified on the C6), the profiles file with its reader and guard (`firmware/hearth-sink/profiles.json`,
`tools/hearth/sink_profile.py`), and CI reading it: every board build in `_build.yml` takes its
fragments from the file and is held to it, packaging checks the profile, and the docs' fragment
lists are held to it. Not built: everything from the boot budget line on
([Order of work](#order-of-work)). Two things this page said before the
boards ran are corrected where they stood: the C6 is not a config value away
([the C6](#the-c6-is-not-a-config-value-away)), and `realtime_permille` is not a speed figure on a
paced sink ([the board suite](#3-the-board-suite)).

## Why

On 2026-10-12, main `3d6b2aa0a` (`v0.10.0-beta.1-4065`), an HTTP `POST /play` failed on both
ESP32-S3 boards and on the ESP32-C6, and Sendspin, which starts at boot, played fine:

```
heap: heap_caps_malloc could not allocate 32768 bytes (caps 0x804); internal free ..., largest 31744
player: could not start the decode task
```

(31,744 on the S3s, 31,232 on the C6.) The cause is a Kconfig default, not a regression in a
fragment and not heap luck:

- `ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM`, the HTTP and partition player's decode task, defaults
  to 1 only `if ICLFORGE_AC4 && IDF_TARGET_ESP32S3 && SPIRAM`
  ([`Kconfig.projbuild`](../firmware/hearth-sink/main/Kconfig.projbuild), the option after
  `ICLFORGE_EXAMPLE_DECODE_STACK_BYTES`). The S3 release shape has no AC-4, so its 32,768-byte
  stack stays in internal RAM, where no block over 31,744 bytes is left once Wi-Fi is up. The
  Sendspin twin of the option (`..._SENDSPIN_DECODE_STACK_IN_PSRAM`, default 1 on S3 with PSRAM)
  was fixed on its own in #1340, which is why Sendspin plays and `/play` does not.
- The C6 has no PSRAM. Its 32 KiB ring goes to internal RAM first and leaves 31,232 bytes, and
  `sdkconfig.sendspin-c6` sizes only the Sendspin decode stack (24,576), not this one;
  `sdkconfig.ci-http` already uses 24,576 for it.

CI was green throughout, because it tests other shapes:

- The HTTP play runs under QEMU only with `sdkconfig.defaults;sdkconfig.ci-http` (a 24,576-byte
  stack, no PSRAM, capture sink): step "Fetch and decode an E-AC-3 stream over QEMU's Ethernet" in
  [`_build.yml`](../.github/workflows/_build.yml).
- The S3, C6 and P4 release shapes are "built and not run" (the steps that call
  `package_firmware.py package`).

The same hazard class is D14c in [`ac4.md`](ac4.md) (no internal block over 31,744 bytes with
Wi-Fi up, so the AC-4 decode stack went to PSRAM), and the Sendspin option's help text in the
Kconfig records an image that started its player "fitted by a few bytes, and did not after a few
more" (the lyrics' document, `karaoke-phase2.md`, W7d).

### It is not a flip-flop, and not only a missing test

A replay of every fragment key's history since 2026-08-06 (the clone is shallow) found exactly 1
of 90 file/key histories that went A, then B, then A: the I2S DMA descriptors, 12 to 4 to 12 on
2026-09-11. The committed fragments are stable. What looks like ping-pong is three other things:

1. **The image shape is spelled in many places, and they disagree.** At least nine distinct
   fragment lists name a board's build (table below), across about twenty files, plus per-session
   scratch overlays and scripts outside the repository. On 2026-10-10 to 12 sessions measured and
   deployed different shapes of the same board; one S3's slots held builds of three sessions in
   two days. The S3 release shape itself changed on 2026-10-11 (decision 43 in
   [`ac4.md`](ac4.md): it now takes `sdkconfig.s3-fast`).
2. **Internal RAM erodes as features land** (Sendspin, Opus, lyrics). The C6's largest internal
   block after Wi-Fi was 32,256 bytes on the 2026-10-10 image and 31,232 on 4065. Nothing records
   that number, so nothing fails when it crosses a line a stack needs.
3. **Nothing runs the shipped shape.** The speed table in
   [`sdkconfig.s3-fast`](../firmware/hearth-sink/sdkconfig.s3-fast) was measured on `hearth-eb2c64`
   with a null sink and AC-4 (`ac4` + `s3-fast` + `s3-dcache`, second lane): 2.0 A-SPX 1.09 to 0.61
   of real time, 5.1 SIMPLE 2.13 to 1.17. That is not the S3 release shape, and there is no
   recorded number for the release shape, because its plays failed.

## What exists today

The board-facing stacks, as written in the tree:

| Stack after `sdkconfig.defaults` | Where it is spelled | What it is |
|---|---|---|
| `hw;psram;sendspin;s3-fast` | `_build.yml` (S3 release step), `README.md`, `docs/hearth/sink-esp32-s3.md`, `sdkconfig.sendspin` / `s3-fast` headers | S3 release image, since 2026-10-11 |
| `hw;psram;sendspin` | `planning/esp32-ota.md` (O8 table), the 2026-10-10 board deploys | S3 before decision 43 |
| `hw;psram;sendspin;ac4;s3-fast;s3-dcache` | prose in `README.md`, `s3-fast` and `s3-dcache` headers | S3 with AC-4 (no CI step builds it) |
| `hw;psram` | `README.md`, `planning/esp32-stream-set.md`, `esp32-device-ui.md`, `esp32-714-realtime.md`, `docs/platforms/bare-metal/esp32-s3.md` | S3 network shapes before Sendspin |
| `hw` | `README.md`, `esp32-714-realtime.md` | S3 without PSRAM |
| `hw;sendspin;c6;sendspin-c6` | `_build.yml`, `README.md`, `docs/hearth/sink-esp32-c6.md`, the fragments' own headers | C6, 4 MB table |
| `hw;sendspin;c6;sendspin-c6;flash16mb` | `_build.yml`, `sdkconfig.flash16mb` | C6, 16 MB table (the board on COM9) |
| `hw;p4;sendspin` | `_build.yml`, `README.md` | P4 revision 1.x, the published image |
| `hw;p4;sendspin;ac4` | `_build.yml` (build only, stage timers on), `README.md`, `sdkconfig.ac4`, `docs/platforms/bare-metal/esp32-p4.md` | P4 with AC-4: what the boards run |

Eleven more stacks are CI shapes (`sdkconfig.ci*`: capture sink, QEMU Ethernet, TDM widths, lane,
OTA). They are tests of code, not of a board, and stay as they are (see [Scope](#scope)).

Three things read "what is an image" and each keeps its own copy: `_build.yml` (the fragment lists
and names), `tools/hearth/package_firmware.py` (chip table, what to read from the build) and
`tools/ci/check_firmware_package.py` (`IMAGES`: chip, flash size, PSRAM, revision range per
published name).

## The proposal in one paragraph

A **profile** is the one named, locked build configuration of one board: its fragments in order,
the values of the keys that decide whether it runs (placement, sizes, caches, flash and PSRAM
mode), the budgets its image and its internal RAM are held to, and the plays it has to pass. One
machine-readable file holds every profile. CI, packaging, the docs tables and the board tools read
it and nothing else, and a guard fails the build when a fragment, an overlay or a Kconfig default
moves a locked key. A suite plays a fixed list of streams on each profile, on the profile's own
image, and records a baseline that later runs may not fall below.

## Profiles

Proposed ids, from the board and not from the codec:

| Profile | Fragments after `sdkconfig.defaults`, in order | Release image | Boards |
|---|---|---|---|
| `s3-n16r8` | `hw`, `psram`, `sendspin`, `s3-fast` | `hearth-sink-esp32s3` | S3a `.156`, S3b `.45` (16 MB flash, 8 MB octal PSRAM) |
| `s3-n16r8-ac4` | `hw`, `psram`, `sendspin`, `ac4`, `s3-fast`, `s3-dcache` | none | the same boards |
| `c6-16mb` | `hw`, `sendspin`, `c6`, `sendspin-c6`, `flash16mb` | `hearth-sink-esp32c6-16mb` | C6 `.117` |
| `c6-4mb` | `hw`, `sendspin`, `c6`, `sendspin-c6` | `hearth-sink-esp32c6` | none on the bench: any C6 module |
| `p4-rev1` | `hw`, `p4`, `sendspin` | `hearth-sink-esp32p4-rev1` | P4a `.99`, P4b `.76` |
| `p4-rev1-ac4` | `hw`, `p4`, `sendspin`, `ac4` | none | the same boards |

**AC-4 on the S3 is its own profile, not an overlay on `s3-n16r8`, and so is AC-4 on the P4
(`p4-rev1-ac4`).** Both were decided by the owner on 2026-10-12. The reason it holds up: AC-4 changes
locked keys, not just adds a feature. `sdkconfig.ac4` sets the decode stack to 40,960,
`sdkconfig.s3-dcache` takes 32 KB of heap for a 64 KB data cache, and the two images have different
heap budgets, different speeds and different baselines. An overlay that moves six locked keys is a
profile with a longer name. The published P4 image stays AC-4 off (`esp32-ota.md`, O8); the boards
run the AC-4 profile.

A C6 has no AC-4 profile: the decoder's floor (286,365 bytes) never fits beside Wi-Fi
([`esp32-sink-compatibility-matrix.md`](esp32-sink-compatibility-matrix.md)).

**A P4 of revision 3.x is a profile of its own, `p4-rev3`, which this file does not have yet.**
#1400 adds its image (`hearth-sink-esp32p4-rev3`, chips 3.01 to 3.99, `panel` and `panel-dsi`
fragments over `p4-rev3`, 400 MHz, the bootloader at warn level to fit). It has never run on a
board, so when it is added it has its guard and its budgets and no play list.

## What a profile locks

A key is **locked** when changing it can make a play that worked fail, or make it slower, without
anything failing at build time. The classes, with the keys they hold today:

| Class | Keys (today's names) | Why it is locked |
|---|---|---|
| Silicon, flash, partition table | `IDF_TARGET`, `ESPTOOLPY_FLASHSIZE_*`, `ESPTOOLPY_FLASHMODE_*`, `PARTITION_TABLE_CUSTOM_FILENAME`, `ESP32P4_REV_*`, `ESP_DEFAULT_CPU_FREQ_MHZ_*`, `BOOTLOADER_APP_ROLLBACK_ENABLE` | A published image's table cannot change over the network; QIO is the bootloader's |
| PSRAM and heap policy | `SPIRAM`, `SPIRAM_MODE_*`, `SPIRAM_SPEED_*`, `SPIRAM_USE_MALLOC`, `SPIRAM_MALLOC_ALWAYSINTERNAL`, `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `SPIRAM_RODATA`, `SPIRAM_FETCH_INSTRUCTIONS`, `SPIRAM_XIP_FROM_PSRAM`, `ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`, `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`, `ESP_COREDUMP_ENABLE_TO_*` | Decides what is in internal RAM at all |
| Caches | `ESP32S3_INSTRUCTION_CACHE_*`, `ESP32S3_DATA_CACHE_*`, `ESP32S3_DATA_CACHE_LINE_*` | `s3-dcache` is 32 KB of heap |
| IRAM options | `ESP_WIFI_IRAM_OPT`, `ESP_WIFI_EXTRA_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT`, `ESP_WIFI_SLP_IRAM_OPT`, `ESP_PHY_IRAM_OPT`, `PM_SLEEP_CLK_ICG_ENABLE` | 45 KB of C6 heap |
| Every task stack: size and placement | `ESP_MAIN_TASK_STACK_SIZE`, `ICLFORGE_EXAMPLE_DECODE_STACK_BYTES`, `..._DECODE_STACK_IN_PSRAM`, `..._SENDSPIN_DECODE_STACK_BYTES`, `..._SENDSPIN_DECODE_STACK_IN_PSRAM`, `..._SENDSPIN_SERVER_STACK_BYTES`, `..._DECODE_LANE`, `..._AC4_PARALLEL`, `..._DECODE_CORE`, `..._FETCH_CORE`, the IDF task-stack sizes | The failure above |
| Rings, queues, buffers | `..._RING_BYTES`, `..._RING_IN_PSRAM`, `..._SENDSPIN_RING_BYTES`, `..._SENDSPIN_MAX_CHUNK_BYTES`, `..._SENDSPIN_LYRICS_MAX_BYTES`, `..._SENDSPIN_MAX_DELAY_MS`, `..._SENDSPIN_MAX_CODED_CHANNELS`, `..._I2S_DMA_DESCRIPTORS`, `..._I2S_DMA_FRAMES`, `..._I2S_SLOT_BITS`, `..._HOLD_FIRST_UNIT`, `LWIP_MAX_SOCKETS` | The 12 to 4 to 12 flip; a lyric document is about ten times its text in heap |
| Features present | `ICLFORGE_AC4`, `ICLFORGE_SENDSPIN`, `ICLFORGE_SENDSPIN_OPUS`, the source, sink and network choices | A profile is a feature set |

Free keys are everything else: the name, port, layout text, log sizes, report cadence.

**The locked set is found by pattern, not remembered.** A completeness check lists every key in
`Kconfig.projbuild` and in the generated `sdkconfig` whose name matches `*STACK*`, `*IN_PSRAM*`,
`*RING*`, `*DMA*`, `*CACHE*`, `SPIRAM*` or `*IRAM_OPT*`, and fails if one is neither locked in a
profile nor on an explicit "free, because" list. A new stack option then cannot arrive without
someone saying where it lives on every board.

### Who may touch what

| Kind of overlay | May set | May not set |
|---|---|---|
| A fragment of the profile itself | any key it declares, once, in the profile's order | a locked key a later fragment resets without saying so |
| `wifi-credentials` (the board's network) | `ICLFORGE_EXAMPLE_WIFI_*` | anything else; the packager already refuses an image with these |
| `measure` (stage timers, `ICLFORGE_EXAMPLE_AC4_PCM_HASH`, report cadence) | observation keys | placement, sizes, caches, features |
| `qemu-net` (the release shape on QEMU's Ethernet; [below](#2-a-qemu-twin-of-the-release-shape)) | network, sink and source choice, PSRAM mode and `-m` | stacks, rings, caches, features |
| `ci-*` shapes (today's `sdkconfig.ci*`) | anything: they are tests of code, and are not profiles | they must not be named as a board's image |
| A scratch overlay (someone's measurement) | whatever it declares | it cannot produce a baseline, and its image reports `+scratch` in its profile id |

A key an overlay sets that is locked in the profile must be named in the overlay's
`may_touch` list in the profile file. The guard below enforces this in both directions.

## One source of truth

`firmware/hearth-sink/profiles.json` holds every profile. JSON because the CI container's Python is
standard library only (as `package_firmware.py` says), the CI steps are shell and the board tools
are PowerShell, and all three read JSON with no dependency. Comments go in `note` fields.

```
profiles.<id>
  target, chip, board                       esp32s3 | esp32c6 | esp32p4, "N16R8", "16mb", "rev1"
  fragments                                 ordered, after sdkconfig.defaults
  locked                                    { "CONFIG_...": "value" }  the values the final sdkconfig must have
  free_because                              { "CONFIG_...": "why this key is not locked" }
  image                                     { name, flash_size, psram, rev_min, rev_max, slot_bytes_max }  (null: not published)
  budget                                    { app_bytes_max, bootloader_bytes_max, internal_static_bytes_max }
  overlays.<name>.may_touch                 keys that overlay is allowed to move
  baseline                                  the file under baselines/ and the board it was recorded on
```

`sink_profile.py`, in `tools/hearth/` (standard library; tests beside it, as `package_firmware.py` has
`test_package_firmware.py`) is the only reader:

| Command | Does | Used by |
|---|---|---|
| `defaults <id> [--overlay ...]` | prints the `SDKCONFIG_DEFAULTS` string, with the order and paths | `_build.yml`, board scripts, README |
| `verify <id> <build-dir> [--overlay ...]` | the guards below, on a finished configure or build | `_build.yml` after each profile build |
| `image <name>` | the rule `check_firmware_package.py` holds a published image to | `check_firmware_package.py`, `package_firmware.py` |
| `table <doc>` | the markdown table a doc embeds between marker comments | the docs check (fails on a stale table) |
| `list` | ids, fragments, boards | humans, scripts |

What goes away: the fragment lists typed into `_build.yml`, the `IMAGES` dictionary, and the stack
tables in `README.md` and `docs/hearth/sink-esp32-*.md`, each replaced by a generated block or a
command. The scattered older stacks in prose (`hw;psram`, `hw`) are rewritten or marked as the
probe or example shapes they are.

## The guard

Run after the configure step of any profile build (a build is not needed; `idf.py reconfigure`
writes the `sdkconfig`):

1. **Fragment order.** The fragments of the build are the profile's, in its order, plus declared
   overlays and nothing else. This catches a stack typed differently in a script.
2. **Locked values.** Every locked key in the final `sdkconfig` equals the profile's value, unless
   a declared overlay lists it in `may_touch`. This is the check that fails on the 2026-10-12 bug:
   the profile says `DECODE_STACK_IN_PSRAM=1`, a default that depends on `ICLFORGE_AC4` makes it
   0, and the build stops. It also catches a Kconfig `depends on` that drops a value a fragment
   asked for, which ESP-IDF's kconfgen reports as a warning and carries on from.
3. **Asked against given.** Every key a fragment sets and an overlay does not touch has that value
   in the final `sdkconfig`; each difference is a finding.
4. **Completeness.** The pattern check above.
5. **Budgets.** The app image and the bootloader are under the profile's limits (the P4's
   bootloader is 24,368 bytes in a 24,576-byte window, so the number is already tight), and the
   **static internal RAM** (`.data`, `.bss`, IRAM; from `idf.py size`) is under the profile's
   limit. That last one is what turns "RAM erodes as features land" into a build failure: each
   byte of static use is a byte less in the largest block at runtime, on any board, with no board
   run.

## Boot-time budget line

**Built (PR 4).** The failure showed at the first play, after the board had been "fine" since
boot. The board already prints `heap: internal free N (largest block M)` at a play's start and
`sendspin: player up with internal heap ...`. One more line, after the network is up and the
Sendspin player has started at boot, and again when a network comes up later
(`report_budget` in `firmware/hearth-sink/main/hearth_sink.cpp`):

```
budget: when=boot net=1 internal_free=86231 largest=31744 ask_ring=0 ask_decode_stack=0
ask_fetch_stack=8192 ask_total=8192 ask_biggest=8192 headroom=23552 floor=0 verdict=ok
```

(One line on the console; this is the S3b's, 2026-10-12, the `s3-n16r8` image. The C6's, from its
serial console on the `c6-16mb` image: `internal_free=113188 largest=98304 ask_ring=32768
ask_decode_stack=32768 ask_fetch_stack=8192 ask_total=73728 ask_biggest=32768 headroom=65536
floor=134008 verdict=below-floor`, which is the C6 profile saying that it refuses an HTTP play.) The asks are the HTTP play's ring and stacks that this build puts in
internal RAM, from the same Kconfig the play reads: a ring or a stack in PSRAM asks for 0. The
headroom is the largest internal block less the biggest single ask. `verdict` is `ok`,
`short-block` (no block that size), `short-total` (not that much in all) or `below-floor` (the
build's `PLAY_MIN_FREE_BYTES` would refuse a play). An S3 image with its 32,768-byte decode
stack internal, as 4065's was, has a 31,744-byte largest block against a 32,768-byte ask, so the
code gives `short-block` at boot where the failure used to be met at the first play; that shape has
not been run with this line on a board.

It is a floor and not a fit: the decoder, the I2S queue and the heap's fragmentation come after
it, so the suite also holds a board to a headroom of its own
(`check_esp_console.py --min-budget-headroom`, which fails a verdict that is not `ok` or a margin
under the figure). The line goes to the console and so to `GET /log` (2 KB on a C6, where a
crash clears it: read the serial console there). Whether a `short-*` should also reach the page is
still open: `GET /hardware`'s JSON is built once, when the control surface starts, before the
Sendspin player does.

**`GET /hardware` reports the profile id and a hash of its locked values** (`profile`,
`profile_hash`). Both are compile-time constants: the build is configured with
`sink_profile.py identity <profile> --cmake` (`-DICLFORGE_PROFILE=` and
`-DICLFORGE_PROFILE_HASH=`), `verify` fails a build that does not carry its own profile's id and
current hash, and the board suite refuses a board that runs another profile, an older definition of
this one, or an image built with neither. A session no longer has to guess from a version string
what a slot holds.
## The suite

Per profile, three layers. A new feature is a new row in the profile's play list and a new figure
in its budget, in the same PR.

### 1. At build time (every PR, no board)

The [guard](#the-guard), with the budgets, run by `_build.yml` on each profile it builds. The
profile's `budget` is also where the heap table lives:

| | S3 `s3-n16r8` | C6 `c6-16mb` | Source |
|---|---|---|---|
| Largest internal block, Wi-Fi up, Sendspin up | 31,744 | 31,232 | 2026-10-12 deploy, 4065 |
| Static internal RAM | recorded by the first guard run | same | `idf.py size` |
| Free low-water over the play list | recorded by the first suite run | same | board suite |

The numbers above are today's, from a failing build: the first PR fixes the build and then the
baseline PRs replace them with numbers from a passing one.

### 2. A QEMU twin of the release shape

**Built (PR 5).** CI ran the HTTP play only on a shape that is not a board's. The twin is the
release profile's own fragments (`s3-n16r8`) with one declared overlay, `qemu-net`
(`sdkconfig.qemu-net`), which changes only what QEMU has no hardware for: Ethernet in place of
Wi-Fi, the capture sink in place of I2S, QUAD PSRAM in place of octal (QEMU started with
`-m 8M`), and the console on UART0 (QEMU's `-serial`; `sdkconfig.hw` puts it on USB-Serial-JTAG,
which QEMU does not model, so the console was silent until this was added). A test holds the
overlay to those: it may not name a key with a stack, a ring, a cache, a DMA queue, AC-4, the
Sendspin player or the lane in it.

The step "Play the S3 board's release shape over QEMU's Ethernet" in the ESP lane (`_build.yml`):

1. configures the twin with the profile's identity and holds it to the profile with
   `verify --overlay qemu-net` (without the overlay named, `verify` names the extra fragment
   and the console, network and PSRAM-mode keys, and nothing else, which is the check that the
   overlay is only them);
2. boots it and waits for the boot `budget:` line;
3. reads `GET /hardware`: the profile and its hash are `sink_profile.py identity`'s;
4. plays over `POST /play`, as a board gets it: an AC-3 2/0, an E-AC-3 2/0 and the 5.1 with
   objects, each to `result=pass` with every unit and no resynchronised byte, the demo at the
   host's Lo/Ro levels (56,673 and 47,346, the same figures the HTTP step holds);
5. holds the console to `sink_profile.py expect-console s3-n16r8`: the lines that say the HTTP
   play's ring and decode stack, the Sendspin ring and its decode stack are in PSRAM, at the sizes
   the profile locks, and that the decode lane started on cores 1 and 0;
6. runs `check_esp_console.py --min-budget-headroom 4096`: one boot, no panic, no failed
   allocation, and a boot budget with a margin.

What the spike found, run locally under QEMU's esp_develop 9.2.2 on Windows:

- **PSRAM execution runs.** With quad PSRAM at `-m 8M` and `SPIRAM_FETCH_INSTRUCTIONS` and
  `SPIRAM_RODATA` on, as the release shape has them, the image boots, brings the network up,
  starts the Sendspin player and the control surface, and plays. `GET /hardware` says
  `psram_bytes: 8388608`. Nothing needed declaring as "not exercised under QEMU".
- **It does not reproduce the 2026-10-12 failure's symptom.** QEMU has no Wi-Fi, so its internal
  RAM is far larger than a board's with the network up: 226,679 bytes free and a 135,168-byte
  largest block at boot against the S3b's 86,231 and 31,744. A 32,768-byte stack in internal RAM
  would be made without trouble, and the play would pass. What the twin catches is the **cause**:
  the stack's placement. A build whose HTTP stack is internal fails `verify` (the key) and fails the
  console expectation (the line `player: decode stack 32768 bytes in PSRAM` is not printed), as
  4065's S3b console did not print it. Heap stays the board's: the twin's budget line is a smoke
  floor, and the board suite owns the budget.
- **The stream set's level checker is for the 7.1.4 shapes.** `check_stream_set.py` holds a play
  to the twelve slots of the layout the CI shapes render to; the release shape plays 2.0, so the
  step checks the verdicts and the demo's two levels itself, as the HTTP step does.
- **Cost:** one more S3 build in the ESP lane, and about two minutes of QEMU for the three plays.

The C6 and the P4 have no emulator, so for them layer 1 and layer 3 are the whole suite.
### The C6 is not a config value away

The brief's config-only candidate for the C6 (the decode stack at 24,576, as `sdkconfig.ci-http`
has it) was tried on a board on 2026-10-12 with the serial console captured, and it is not a fix. A
C6 with the Sendspin player up has 109 to 117 KB of internal RAM free when an HTTP play opens its
source, and the play takes a ring, two stacks, the I2S queue and a decoder out of it:

| Build | What an HTTP play did |
|---|---|
| ring 32,768, stack 32,768 (main) | `could not allocate 32768 bytes`, `could not start the decode task` |
| ring 32,768, stack 24,576 | the task started; the decoder failed a 6,144-byte allocation with 5,636 free and `abort()`ed: the board rebooted |
| ring 8,192, stack 24,576 | AC-3 2.0 played beside 129 failed Wi-Fi buffer allocations and an underrun; E-AC-3 2.0 failed a 7,168-byte allocation with 12,476 free and `abort()`ed |

So the C6 profile does not claim HTTP plays. #1395 makes the board refuse one up front, with
`why: memory`, from `CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES` (134,008 in the C6 fragment, a
sum of the sizes the fragment gives a play and the least decoder's peak), and the Sendspin session
stays up. The C6's suite plays over Sendspin, which is the product's path on that part, and its
play list needs a Sendspin server on the bench (the repository's test server is one). Whether the
HTTP play should work on a C6 at all is the owner's call: it needs the Sendspin player's 48 KB ring
and 24 KB stack given back for the length of a play.

### 3. The board suite

**Built (PR 6).** `board_suite.py`, in `tools/hearth/` (standard library), reads the play list
from `firmware/hearth-sink/suite.json` and writes baselines under
`firmware/hearth-sink/baselines/`. It replaces the scripts that lived in `D:\ac3bld` and the
session scratchpads:

```
board_suite.py run --profile s3-n16r8 --board S3b --session <id> [--repeat 3] [--restart [--boots 8]] \
                   [--record | --compare baselines/s3-n16r8.json]
board_suite.py plan --profile s3-n16r8
board_suite.py compare --run board-run.json --baseline baselines/s3-n16r8.json
board_suite.py lock take|release|show --file <lock> --session <id>
```

`--board` names a row of [`bench.json`](../tools/hearth/bench.json), which gives `--host` and the
lock, and refuses an address or a lock that is not that board's; `--restart` is refused on a P4
(see [the board bench](esp32-board-bench.md)). Without `--board`, `--host` and `--lock` are
given by hand as before.

It does, in this order, and stops at the first refusal:

1. **Booking.** Refuses with no `--lock` and `--session`. Takes the lock file atomically
   (create-new, one line `session=... | purpose=... | start=... | expected_end=...`) and refuses if
   one exists, unless it is this session's own (a run on a board the session booked to put an
   image on it uses that lock and leaves it for its owner). It releases only a lock it took. It
   does not read or keep anything the board says about its network. The rest of the booking rule
   (the other four locks, `ListAgents`, running `idf.py`/`esptool`/`ota.py`) is the operator's, and
   the suite prints it first as a checklist, because a script cannot see another session.
2. **Identity.** `GET /hardware`: the profile id and the hash of the file's current definition are
   the board's (`sink_profile.py identity`); the board is `state: stopped`.
3. **Plays.** Serves the fixture streams from this machine (its own LAN address, a server that is
   shut down at the end) and for each row: `POST /play` of a location of its own, `/status` to
   `finished` or `failed`, then `/log?from=` until the play's `result=` line, because the state
   flips before the end-of-play lines print.
4. **Asserts,** per row: finished and not failed; every unit and no resynchronised byte; sink
   underruns 0; no failed allocation; no reboot (the log is shorter than where it was); and for
   AC-4 the `pcm_hash=` of the `ac4.<label>=` line equal to the host's pin in
   [`testdata/ac4-probe-pcm-hashes.json`](../testdata/ac4-probe-pcm-hashes.json). A row that
   expects the C6's refusal expects state `failed`, why `memory`, and no reboot.
5. **Figures,** per row: the decoder's time per frame, the decode stack left and the sink queue's
   least headroom. `--repeat N` plays a row N times and holds the median time and the worst of the
   rest.
6. **Writes `board-run.json`** (the figures, the image's version and profile hash, the chip, the
   date). `--record` writes `baselines/<profile>.json` from a clean run; `--compare` holds a run to
   a baseline with the tolerance in `suite.json` and prints an improvement as "re-baseline".

Tests (`test_board_suite.py`, 89) run it against a stand-in board that answers with what the real
boards printed on 2026-10-12, and hold the file to the tree (every stream is in the repository,
every AC-4 row names a pin the host holds, every profile has a list).

**Run on S3b (2026-10-12, the `s3-n16r8` image, 4175):** AC-3 2.0 and E-AC-3 2.0, 938 frames each,
no underruns, the decode stack 18,256 and 18,064 bytes free, the queue's least headroom 5 ms, and
the decoder at **123 and 88 permille of a frame** where `realtime_permille` read 981 for both.

Not in PR 6, and said so: the boot `budget:` line was not read into the run (S3's `/log` may have
rotated it out and the C6's is 2 KB). PR 7 reads it: `--restart` restarts the board so that the boot
line is the log's first, the suite parses it, and `--record` refuses a baseline when the log holds
none; the C6's 2 KB log still holds the line a moment after a restart (27 boots, PR 9), so
its serial console is not needed for it. The 5.1 and
wider rows (`demo.ec3` with objects) wait for the baselines; the AC-4 rows need an image built with
`CONFIG_ICLFORGE_EXAMPLE_AC4_PCM_HASH=1` (the `measure` overlay) and say so when the hash is not on
the line; and a Sendspin play list for the C6 needs a server on the bench.

The play list, in `suite.json`: AC-3 2.0 and E-AC-3 2.0 (30 s of music each) on every profile with
an HTTP play, the C6's two rows as the refusal they are, and on the AC-4 profiles the pinned
streams the host and the boards' PCM agree on (`ac4_20_music`, `ac4_20_acpl`,
`ac4_20_companding`, and `ac4_51_music` on the P4). The streams are the repository's; the suite
does not generate its own.
**The speed figure is not `realtime_permille`.** On an I2S sink every stereo play on an S3 read
981 to 992 permille in 2026-10-12's runs, whatever it decoded, because the figure includes the
wait for the DAC (the player's own comment says so for a paced sink). The suite's figure is the
decoder's: `us_per_frame` less `sink_us_per_frame` and `render_us_per_frame`, which `GET /status`
carries, over the 32 ms an AC-3 or E-AC-3 frame lasts; for AC-4 the `decoder_permille` of the
`ac4.<label>=` line, which already leaves them out. A paced sink also hides a short queue: the
S3's plays held `sink.min_headroom_ms` at 5 ms of a 64 ms queue (0 on the one play that
underran, with `sink.dry_ms` at 88), and that figure, with `sink.dry_ms`, is the other number a
baseline records.

**Tolerance, and the ratchet.** The decoder's time per frame may be at most the larger of 5% and
5 permille of a frame above the baseline's median. The 20 permille this page proposed was a guess;
PR 7 measured the spread, ten plays a row on S3b (the `s3-n16r8` image, 2026-10-12): AC-3 2.0
122 to 124 permille, E-AC-3 2.0 87 to 90. With the spread that narrow, 20 permille would have let
a 23% slowdown of the E-AC-3 decoder through; 5 holds it to under 6%. The
other figures are held one way only, each against the baseline less its margin: the decode stack's
free bytes 512 (the spread was 180), the queue's least headroom 2 ms (it read 5 every play), and at
boot the largest internal block 1,024 and the free internal RAM 2,048. The values are in
`suite.json`, with this paragraph's numbers beside them. A figure better than the baseline by more
than the tolerance is reported, and the PR that made the speed-up changes the baseline in the same
commit, so the next regression is measured from the new floor. A baseline only changes in a PR
that edits the profile or the code and carries a board run.

**The boot line's free RAM is read twice over, and the baseline sits at the common value.** The
Sendspin player starts on a 16 KiB task stack of its own (`sendspin-keys`) that goes when the work
is done, and `report_budget("boot")` runs just after: it read 85,923 to 86,235 bytes free on eight
of nine boots on S3b and 102,539 on the ninth, the idle task having freed that stack first. The
largest block (31,744) and the headroom (23,552) did not move. Because the ratchet is one-way, the
high reading can only look like an improvement; a baseline recorded on it would fail every ordinary
boot with a figure 16 KB under it, so `s3-n16r8`'s is the common value, and a re-record that lands
on the high one is run again. Making the line wait for the idle task is a firmware change and not
made here.

**Recorded (PR 7, `s3-n16r8`):** S3b, not S3a (S3a was booked by another session; the S3a run is
the cross-check and is not done), the identity image `v0.10.0-beta.1-4175-g79d2e7618-`, profile
hash `c4b8d369cf36`, five plays a row after a restart: AC-3 2.0 124 permille and E-AC-3 2.0 88, 938
frames each, no underruns, 0 ms dry, queue headroom 5 ms, decode stack 18,256 and 18,068 bytes
free; boot line `internal_free=86223 largest=31744 headroom=23552 verdict=ok`. A second run of
two plays a row after another restart held to it with no finding.

**A board's boot figures are not one number (PR 9).** The S3's free RAM has two modes, above;
the C6's largest internal block at boot was 96,256 bytes on 21 of 27 boots and 94,208 on four,
92,160 on one and 88,064 on one, with free RAM at 108,988 on one boot and 112,204 to 113,128
on the rest. A baseline taken from one boot would fail one boot in five of the next runs, so
`board_suite.py run --restart --boots N` restarts N times and holds **the worst boot** (the
smallest largest block, then the least free RAM), keeps every sample in the record
(`boot_samples`), and a run that starts higher than the baseline is not a regression. A profile
whose figures jitter by more than the S3's carries its own tolerance in `suite.json`, with the
spread it came from: the C6's largest block is allowed 12,288 bytes (three 4 KB steps, where
8,192 was seen) and keeps the suite's 2,048 for free RAM, which is held below the worst boot.

**Recorded (PR 9, `c6-16mb`):** the C6 (`.117`), the
identity image `v0.10.0-beta.1-4175-g79d2e7618-`, profile hash `c256f286a410`, five refused
plays a row after eight restarts. Both rows are state `failed`, `why: memory`, no frames and no
reboot; the boot line is the worst of the eight: `internal_free=108988 largest=94208
headroom=61440 floor=134008 verdict=below-floor`. A run of two refusals a row after the worst
of three more boots (112,172 free, 96,256 largest) held to it with no finding. Not in it: a
Sendspin play, which needs a server on the bench, and the C6's `4mb` twin, which has no board.

**The log is read to its end (PR 10).** `GET /log` answers at most 4 KB (`kLogReplyBytes`)
out of a 16 KB ring, and a client follows `X-Log-Next` for the rest. The suite's whole-log read
took the first reply only: on the P4 the boot `budget:` line lay past the first 4 KB and was cut
off mid-word, and a mark taken from a log longer than that sat in the middle of the ring, where
an old play's `result=` line could end the wait for the new one. The S3 and C6 baselines (PR 7
and 9) were taken after `--restart`, from logs under 4 KB, so their reads were whole; the fix is
in `Board.log`, with a stand-in that caps its replies as the board does.

**Recorded (PR 10, `p4-rev1`):** P4a (`.99`), the identity image
`v0.10.0-beta.1-4367-g957c80e16`, profile hash `1da8144216fb`, five plays a row after eight
restarts by esptool's watchdog reset (`--reset-by esptool`, because a P4's `POST /restart` can
lose its C6 link): AC-3 2.0 at **61 permille** of a frame and E-AC-3 2.0 at **40**, 938 frames
each, no underruns, 0 ms dry, queue headroom 5 ms, decode stack 18,480 and 18,400 bytes free.
The boot line is the worst of the eight: `internal_free=291875 largest=217088
headroom=184320 verdict=ok`. Its largest block steps by 4 KB (221,184 on eight of ten boots read
one by one, 217,088 on two), so `p4-rev1` carries its own tolerance of 8,192 bytes there, as
`c6-16mb` does. A run of two plays a row after the worst of three more boots held to it with no
finding. About half the S3's decoder time per frame, at 400 MHz. Not in it: `p4-rev1-ac4`, which
needs the `measure` image for the PCM hash, and the P4b, which another session's build holds.

**Recorded (PR 11, `p4-rev1-ac4`):** P4a again, the image `v0.10.0-beta.1-4413-g43d19b5c1`
built with the `measure` overlay, profile hash `6cd3fe4b015f` (the overlay does not move it),
five plays a row after eight watchdog-reset boots. AC-3 2.0 at 61 permille and E-AC-3 2.0 at
41, as the release shape, with the AC-4 build's 40 KB decode stack (26,292 and 26,212 bytes
free). The three 2.0 AC-4 rows carry the host's pins, equal on the board: `ac4-20-music` 241
permille of the audio, `ac4-20-acpl` 281 and `ac4-20-companding` 282. The boot line is the worst
of eight: `internal_free=289271 largest=217088 headroom=176128 verdict=ok`.

**The 5.1 row has no host oracle on this sink.** The image's I2S sink has two outputs, and asked
for a 5.1 layout it fails (`could not start I2S in TDM mode`: the APLL's maximum frequency,
`sample rate is too large`), so the decoder folds the stream to Lo/Ro and hands over stereo, whose
hash is not the 5.1 probe's. The row has `pin_layout: 5.1` in `suite.json`: a play on another
layout is not held to the pin and is recorded with its layout and its own hash
(`7162259f3e596916`, 617 permille), which `compare` holds to the baseline's. D14e's 5.1 numbers
were taken on a null sink; whether the float output of the 6-output render equals the host's is
not tested by this profile on this board.

**Why the profile has its own tolerance.** The first compare run, after the record, failed two
findings: free RAM at boot 2,940 bytes under the recorded worst, and `ac4-20-acpl` at 297 permille
against 281 (allowed 295). The rows are three or four frames, so a play is noisy by about 6% of
itself (`ac4-20-music` read 220 to 253 in five plays), against the S3's 122 to 124 on a 30 s AC-3;
and free RAM at boot has a second mode, 286,331 against 289,3xx to 289,9xx, on two of 17 boots.
`p4-rev1-ac4` therefore carries `relative` 12% (twice the noise, and still calling a fifth more
decoder time a regression), 4,096 bytes of free RAM and 8,192 of largest block. The saved compare
run holds to it, and a run 20% slower on `ac4-20-acpl` and 8 KB lower on RAM still fails.

## Changing a profile

A change to a profile, a fragment it names, or a locked key's Kconfig default is one PR that:

1. edits `profiles.json` (the locked value, or the fragment order) and the fragment;
2. regenerates the doc tables (`sink_profile.py table`, checked in CI);
3. attaches `board-run.json` from a board booked for the run, and updates
   `baselines/<profile>.json` if a figure moved;
4. says in the description which keys moved, why, and what the heap table did.

CI fails a PR that touches a fragment or the Kconfig defaults of a locked key and not the profile
file. A scratch build for a measurement is `sink_profile.py build <id> --overlay <file>`: it
reports `<id>+scratch`, is refused by the suite's `--record`, and its overlay is committed beside
the measurement if the figure is quoted anywhere.

## Scope

In: `firmware/hearth-sink` and its fragments, the release images, the docs that state a shape, the
board tooling that builds or flashes one.

Out, deliberately: the bare-metal probes (`firmware/baremetal/platform/*/sdkconfig.*`) and the
`i2s_player` example, which measure footprint and are not a product shape; the `ci-*` fragments
(tests of code, not of a board); the ESP32-P4's co-processor firmware (`esp32-ota.md`, O6); and
Wi-Fi values, which are credentials and stay out of the profile.

## Order of work

Each is a small PR. The suite and the baselines need board time, and a board is booked through its
lock file in `D:/ac3bld` before anything is run on it, and restored and released after
([the rule](esp32-board-bench.md#the-rule)).

| # | PR | Needs a board | Notes |
|---|---|---|---|
| 0 | This page, and a row in `planning/README.md` | no | Docs PR; the owner confirms or changes the decisions below |
| 1 | **The fix**, as profile values: `DECODE_STACK_IN_PSRAM` defaults to 1 on S3 with PSRAM (the AC-4 condition goes) and `sdkconfig.psram` says so in words | yes | **Built, #1387**: A/B/A on S3b (4065 fails, the fix plays AC-3 and E-AC-3 2.0, 938 frames, stack in PSRAM) and the same on S3a. The C6's half did not work as config; see [the C6](#the-c6-is-not-a-config-value-away) |
| 1b | The C6 refuses a play it cannot hold: `CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES` | yes | **Built, #1395**: refused in under half a second with `why: memory`, no reboot, Sendspin stayed connected |
| 2 | `profiles.json` and `sink_profile.py`, with tests and the guard; no consumer yet | no | **Built**: six profiles; its tests hold the file to `_build.yml`'s fragment lists and to `check_firmware_package.py`'s rules |
| 3 | `_build.yml`, `package_firmware.py` and the docs read it; the guard on every profile build | no | **Built**: each board build takes `sink_profile.py defaults <id>` and is held by `verify` (with `idf.py size` for the static-RAM budget); `package_firmware.py package --profile` refuses a build that is not the profile; `check-docs` holds every fragment list the tree spells to a profile or to `other_stacks`, and the README's profile table is generated; `s3-n16r8-ac4` gets a CI build for the first time. Left: `check_firmware_package.py`'s `IMAGES` still carries its own copy, held to the file by a test, until #1400 lands its row. The repository variable `ESP32_PROFILE_GUARD=warn` turns a finding into an annotation |
| 4 | The boot `budget:` line, `profile` in `GET /hardware`, `check_esp_console.py` reads the line | yes, one boot per profile | **Built**: see [the budget line](#boot-time-budget-line); read on S3b and the C6 (the P4s were booked by another session) |
| 5 | The QEMU twin of `s3-n16r8` | no | **Built**: see [the twin](#2-a-qemu-twin-of-the-release-shape). The spike found PSRAM execution runs under QEMU and the twin catches the stack's placement, not the heap's symptom |
| 6 | `board_suite.py` and its tests (a stub board, as `ota.py` has) | yes, to prove it on one | **Built**: see [the suite](#3-the-board-suite); run on S3b |
| 7 | Baselines: `s3-n16r8`, `c6-16mb`, `p4-rev1`, then the AC-4 profiles, one PR each with its board run attached | yes | **`s3-n16r8` built** (S3b; see [the ratchet](#3-the-board-suite)) and **`c6-16mb` built** (the C6, the refusal rows and the worst of eight boots) and **`p4-rev1` built** (P4a, by watchdog-reset boots) and **`p4-rev1-ac4` built** (P4a, with the `measure` image and its new fragment, the 5.1 row held to its own hash); `s3-n16r8-ac4` needs its board and the same image. `c6-4mb` has no board: its guard and budgets only, the suite's rows run on `c6-16mb`, which differs in flash and table alone |
| 8 | Board scripts out of `D:\ac3bld` into `tools/hearth/` (build, OTA, the lock), and the process page | no | **Built**: [the board bench](esp32-board-bench.md): `bench.json` (the five boards, their locks, MAC tails), `bench.py` (status, lock, log, snapshot, console, scrub; the lock code the suite now shares), `bench_flash.py` (build from a profile, flash after a MAC check, verify, OTA with the lock). The build and the redaction were run for real; flash, `--app-only` and OTA through the tool were not, because they write a board |

PR 3 follows 2. PR 2 adds `p4-rev3` once #1400 is on main, since its fragments are not.

## Decisions for the owner

Taken on 2026-10-12: AC-4 on the S3 and on the P4 are their own profiles; PR 1 is S3-only and the
C6 refuses cleanly (option A); the fix's shape is the Kconfig default and the fragment both.

Still open:

1. **The recording board for each profile.** Proposed: S3a for both S3 profiles with S3b as the
   cross-check, C6 `.117` for `c6-16mb`, P4a `.99` for both P4 profiles (P4b needs a USB flash
   after any OTA).
2. **The tolerance:** now measured, not proposed: the larger of 5% and 5 permille over the
   median, for the decoder's time per frame; 0 underruns; the stack's margin within 512 bytes of
   the baseline's (see the ratchet above). Say if looser or tighter.
3. **QEMU is not the heap authority.** The twin runs the release shape but not Wi-Fi, so a heap
   number from it is a smoke floor. The board suite owns the budget. Confirm that is acceptable.
4. **An HTTP play on the C6.** Left refused. The alternative is giving the Sendspin player's ring
   and stack back for the length of a play, which is a change to the player, not to a profile.

## What this does not claim

- That the S3 release shape keeps up. Its first recorded number is a suite run away; the
  `s3-fast` table is for another shape.
- That QEMU can run the release shape's PSRAM execution. It is a spike (PR 5).
- That the boot line is sufficient. It is an early warning for the failure this page started with,
  and the play list is what holds the rest.
