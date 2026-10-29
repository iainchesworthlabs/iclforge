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
| `s3-n16r8` | `hw`, `psram`, `psram-4k`, `sendspin`, `ac4`, `s3-fast`, `s3-dcache` | `hearth-sink-esp32s3` | S3a `.156`, S3b `.45` (16 MB flash, 8 MB octal PSRAM) |
| `c6-16mb` | `hw`, `sendspin`, `c6`, `sendspin-c6`, `flash16mb` | `hearth-sink-esp32c6-16mb` | C6 `.117` |
| `c6-4mb` | `hw`, `sendspin`, `c6`, `sendspin-c6` | `hearth-sink-esp32c6` | none on the bench: any C6 module |
| `p4-rev1` | `hw`, `p4`, `sendspin`, `ac4` | `hearth-sink-esp32p4-rev1` | P4a `.99`, P4b `.76` |
| `p4-rev3` | `hw`, `p4`, `p4-rev3`, `sendspin`, `ac4`, `panel`, `panel-dsi` | `hearth-sink-esp32p4-rev3` | none on the bench: the owner's ESP32-P4X-Function-EV-Board has not arrived |

**AC-4 was its own profile on the S3 and the P4 (the owner, 2026-10-12); since 2026-10-29 it is in every release profile but the C6's.** The owner
decided on 2026-10-12 that AC-4 is a profile of its own on both boards, because it changes locked keys: `sdkconfig.ac4`
sets the decode stack to 40,960, `sdkconfig.s3-dcache` takes 32 KB of heap for a 64 KB data cache, and the images had different
heap budgets, speeds and baselines. **On 2026-10-29 the profiles were folded** (the owner: "if we need to fold the
profiles together, that's fine. The budget can be grown"): `p4-rev1` is what `p4-rev1-ac4` was (the same fragments and the same
locked values, so the same hash, `f797fb39d0f9`, and the baseline P4a recorded on 2026-10-12 stands for it), with an app budget
of 3,400,000 bytes against the 4 MiB slot. `p4-rev3` took the decoder after the kit's first run of the AC-4 image on 2026-10-29 failed
(the largest internal block, 37,888 bytes after the I2S queue, was under the 40,960-byte decode stack and the 24,576-byte lane
stack): the player now places a stack in PSRAM when no internal block holds it, `p4-rev3` locks the HTTP play's decode stack
in PSRAM, and its budget is 3,950,000 against an image of 3,811,472. `s3-n16r8` took the decoder and the 64 KB data cache and
kept its 4 KB allocation limit (budget 2,750,000, image 2,621,600), with `psram-4k` after `psram` and `LWIP_TCP_OOSEQ_MAX_PBUFS` bounded in
`sdkconfig.psram`. [`ac4-in-release-images.md`](ac4-in-release-images.md) has the measurements.

A C6 has no AC-4 profile: the decoder's floor (286,365 bytes) never fits beside Wi-Fi
([`esp32-sink-compatibility-matrix.md`](esp32-sink-compatibility-matrix.md)).

**A P4 of revision 3.x is a profile of its own, `p4-rev3`** (PR 13, once #1400 was on main). Its
image is `hearth-sink-esp32p4-rev3` (chips 3.01 to 3.99, `panel` and `panel-dsi` fragments over
`p4-rev3`, 400 MHz, the bootloader at warn level to fit). It first ran on a board on 2026-10-20,
the P4X kit (`P4X` in `bench.json`), and has its guard and its budgets, a play list of three rows
and a baseline (`baselines/p4-rev3.json`): the two 2.0 streams as the other profiles play them,
and a 5.1 stream on the layout `9.1.6`, whose row names the `layout` to set for the play and the
`slots` the sink must open for (16, the full TDM frame, which a chip below v3.0 cannot open). What
it is:

- **133 locked keys**, snapshotted from a real configured build (2026-10-12): the two revision
  bounds (`ESP32P4_REV_MIN_FULL` 301 and `ESP32P4_REV_MAX_FULL` 399, so it refuses a v1.x chip,
  as `p4-rev1` refuses a v3.x one), 400 MHz, the DSI panel and no null display, LVGL's heap in PSRAM
  (`LV_USE_CUSTOM_MALLOC`), and the I2S queue. **The screen task's stack
  (`ICLFORGE_EXAMPLE_PANEL_STACK_BYTES`, 16,384, in PSRAM) is locked by hand**: it is `free` for
  every profile with no screen, where it is inert, and `snapshot` skips a free key, while
  `verify` reads a locked key first. A test holds it locked here and in no other profile.
- **Budgets**, about 5% over what the build measured, as the S3's and C6's are: the app
  2,900,000 bytes (2,469,920 built when set at 2,600,000; CI's #1400 build 2,466,032; raised to 2,900,000 on 2026-10-25 because main alone then built to about 2,604,700 bytes and the Preferences screens, layout chooser, speakers styles and late-night mode add roughly 60 KB more: about 2,663 KB with all visible work, under the 4 MiB slot), static internal RAM 175,000
  (DIRAM 166,948), the bootloader 24,576 (22,448).
- **CI reads it**: the `_build.yml` step for this image takes its fragments from
  `sink_profile.py defaults p4-rev3`, is held by `verify` and packaged with `--profile p4-rev3`,
  as the boards' are. The release rules' list and the profiles are one list now:
  `IMAGES_WITHOUT_A_PROFILE` in the test is empty. The panel's null-display build stays an
  `other_stacks` entry: a development build, not a board's image.
- **The hash is `6e5a68ebbb57`.** The real build holds to it.

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
| `panel-capture` (the screen's read-back and touch server on 8081, which has no authentication) | `ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT` | everything else; a release profile locks it at 0 and the packager refuses an image with it on |
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

### Where each check runs

Read from the workflows on 2026-10-13, because the question "how is the canon enforced while other
sessions change the firmware" has a shorter answer than the guard's size suggests:

| where | what it holds | when | fails a PR? |
|---|---|---|---|
| PR Gate, `Static` (`_static.yml`) | `sink_profile.py check-docs`; `test_sink_profile.py` (the file against `_build.yml`'s fragment lists, `check_firmware_package.py`'s image rules, the keys every profile must lock); `test_board_suite.py` (a baseline holds exactly its profile's rows, and is under the profile's hash or accepted by the PR against the current one); `sink_profile.py check-change --base` (a Kconfig key the change adds that the guard watches, that a profile has in its configuration and neither locks nor frees; a lock on a key no Kconfig declares) | every PR and queue entry | **yes**, but these are checks that the *files agree*, not that a configured build is the profile |
| PR Gate, `ESP32 profiles` (`_profiles.yml`) | every profile **configured** in the IDF image (`idf.py reconfigure`) and held by `verify`: the fragments, the locked values, the watched keys nobody placed (no compile: the budgets stay with the builds below) | a pull request or queue entry that changes `firmware/hearth-sink`, `firmware/esp-idf` or the profile tools (the planner's `esp`), and every push to `main` | **yes**: `CI Status` fails closed on it |
| `ci.yml` → `_build.yml`: `build-esp32s3`, `hearth-esp32s3`, `build-esp32c3`, `package-esp32-firmware` | the build **is** the profile: `sink_profile.py defaults`/`identity` configure it, `verify` holds it (asked against given, locked values, watched keys nobody placed, the app, bootloader and static RAM budgets from `idf.py size`), `package_firmware.py --profile` refuses an image that is not one | push to `main` only | **no**: red on `main`. Until #1499 they were skipped there whenever a minimum-footprint leg above them in the same job failed, which is how the C6's budget stood wrong; they run after a footprint failure now |
| `bench_flash.py build <id>` | the same guard, on a bench build | by hand | no |
| a booked board | the boot `budget:` line, the identity in `/hardware`, `board_suite.py` (it refuses an image built under another hash) and `compare` | by hand | no |

`ESP32_PROFILE_GUARD=warn` (a repository variable, unset) makes the build steps warn instead of
fail. What this means for another session's valid change: it is never blocked, and the guard
makes it *decide* when a configured build sees something new: a locked key whose effective value
changed (`[locked] KEY is X, the profile says Y`) is locked at its new value in `profiles.json`
and, by the owner's rule, re-recorded; a new key matching a watch pattern (`[unplaced]`) is locked
or named `free`; a key added at an inert value is locked and its baselines kept. Where the
decision is forced depends on what changed. **A key the change adds, or a lock it orphans, at the
pull request** (`check-change`, which reads the Kconfig at the head and the base and evaluates a
key's conditions against each profile's locked values and target; what it cannot decide it leaves
to the configured build). **A baseline the edit moves, at the pull request** (the test above).
**A changed default of a key a profile locks, or a fragment's value, at the pull request too**,
by the `ESP32 profiles` job, which configures every profile in the IDF image: a PR that edits
a Kconfig default or a fragment fails there with the guard's own `[locked]` or `[unplaced]`,
and so does a merge-queue entry, which is where two PRs that are each valid alone first meet.
What it does not hold is a budget (the app, bootloader and static RAM need a build): those are
the post-merge jobs', which now run after a footprint failure.

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
| Static internal RAM | 176,854 (budget 185,000) | 111,823 (budget 117,000; [corrected in PR 14, then lowered twice](#the-c6s-static-ram-budget-was-wrong-pr-14)) | `idf.py size` |
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
`why: memory`, from `CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES` (160,796 in the C6 fragment: a
sum of the sizes the fragment gives a play and the least decoder's peak, 134,008, and the 26,788
bytes the network needs free beside a decoder, the least a Sendspin E-AC-3 stream left), and the
Sendspin session stays up. The C6's suite plays over Sendspin, which is the product's path on that part, with the
repository's test server on the bench ([Sendspin rows](#sendspin-rows-pr-14)). Whether the HTTP
play should work on a C6 at all is the owner's call: it needs the Sendspin player's 48 KB ring
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

Tests (`test_board_suite.py`, 94) run it against a stand-in board that answers with what the real
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

A row may name a `layout`, set with `PUT /layout` for its play and put back after it, and the `slots`
the sink must open for: `p4-rev3`'s 5.1 stream on `9.1.6` is the one, and fails when the sink opens
for fewer than sixteen.

**A row declares what it needs, and the board says what it carries (2026-10-25).** A row that names a
`layout` needs the board to carry it, and the suite does not guess that from the profile: it reads
`GET /layout-options`, the firmware's one verdict over its capabilities (the same one `PUT /layout`, the
Preferences list and the device page ask: [`p4x-layout-options.md`](p4x-layout-options.md)), once per run,
and prints what the board carries, carries with a limit and does not. A layout the board carries is played
as before; one it carries with a limit is played and the limit is noted. One it cannot carry is what the
row's `if_unavailable` says: `refuse` (the default) sends `PUT /layout` and holds the board to answering
409 with the reason the listing gives for it, which is the row's pass, since a board told a layout it
cannot carry is meant to say no (state `refused-layout`, the reason in `why`); `skip` leaves the row out
and says why (state `skipped`). A board that takes a layout it lists as unavailable, or refuses it in other
words than it lists, is a finding. A firmware with no `GET /layout-options` plays the row as written. The
old rule that only a v3.x profile could have a layout row is gone: a P4 below v3.0 reports two slots
(`sink_slots()`) and says so, and the fold is tested where it always was, by a 5.1 stream on the board's own
2.0. No row was added to another profile in this change: a new row is a new baseline, and a baseline is
recorded on a board.

`--fixtures-url` plays the streams from a server that is already running
and serves the repository's paths, where the suite would otherwise start one over the whole
repository on the LAN.

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
the cross-check, recorded below under "Cross-checked on S3a"), the identity image `v0.10.0-beta.1-4175-g79d2e7618-`, profile
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

**Recorded (PR 12, `s3-n16r8-ac4`):** S3a (`.156`, the proposed recording board), the image
`v0.10.0-beta.1-4460-g12c4fdaaf` built with the `measure` overlay, profile hash
`6124ef3f591f`, five plays a row after eight restarts. AC-3 2.0 at 120 permille and E-AC-3 2.0 at
84 (with the AC-4 build's 64 KB data cache, a few percent under the release shape's 123 and 87);
`ac4-20-music` at 510 permille of the audio, `ac4-20-acpl` at 645 and `ac4-20-companding` at 608,
so all three decode inside real time on an S3, and their PCM hashes are **the host's pins, equal**
(`f860e51f602ae753`, `e8b8cfa716dd0e1c`, `5b93c61c57566b0c`), as the P4's are. The boot line is
the worst of eight: `internal_free=52075 largest=31744 headroom=23552 verdict=ok`, which spread
28 bytes, and is 33 KB under the release shape's 85,755 (the 64 KB data cache takes 32 KB of heap).
The S3 is steady where the C6's and the P4's boots are not, so the profile has no tolerance of its
own.

**Cross-checked on S3b**, the second board: the same image against S3a's baseline, worst of three
boots and three plays a row, holds with no finding: AC-3 120 and E-AC-3 82 permille, the three
AC-4 rows at 510, 646 and 598 with the same three hashes, boot free RAM 51,927 against 52,075. The
two S3 boards agree to within the noise of the rows.

**A locked default moved under the profile (PR 12).** The first S3 build after `main` was merged in
failed the guard on two locked keys, for both S3 profiles and for nothing else: the Sendspin
lyric budget (`ICLFORGE_EXAMPLE_SENDSPIN_LYRICS_MAX_BYTES`, 16,384 to 2,048 on the S3, because a
lyric document costs about ten times its text of internal RAM at the peak) and the objects a
board states (`ICLFORGE_EXAMPLE_SENDSPIN_MAX_OBJECTS`, 2 to 0). Both are other sessions' commits to
the Kconfig, which is what the guard is for. The profiles follow them in one commit at the bottom
of the stack, so the profile hashes change (`s3-n16r8` `c4b8d369cf36` to `0fb46709d4bb`,
`s3-n16r8-ac4` `9d72f4f00ba8` to `6124ef3f591f`) and **a baseline recorded under the old hash is
re-recorded**: `s3-n16r8`'s was, on S3b, the worst of eight boots now (AC-3 123 and E-AC-3 87
permille against 124 and 88; boot free RAM 85,755 against 86,223). Nothing material moved: the new
image held to the old baseline before it was replaced. The C6 and P4 profiles were not touched:
their locked values still equal the Kconfig's defaults (and the P4 builds of PR 10 and 11 held).
`compare` does not hold a run to a baseline's hash, so a stale one is a number that is still right
and a hash that is not: it says which hash the baseline was recorded under when the run's
differs (a one-line note), and the PR that changes a locked default carries the re-record.

**Cross-checked on S3a (`s3-n16r8`, after PR 12).** The plain image
`v0.10.0-beta.1-4460-g12c4fdaaf` (hash `0fb46709d4bb`) on S3a, against the S3b baseline above, by
the baseline's own method (five plays a row, the worst of eight restarts): **holds with no
finding.** AC-3 2.0 at 124 permille against 123 (S3a's five plays 122 to 125, S3b's 122 to 124) and
E-AC-3 2.0 at 87 against 87 (87 to 88 against 86 to 88); the decode stack's free bytes are identical
(18,256 and 18,064); boot free RAM at its worst 85,811 against 85,755. **Both boards show the
same second mode** in boot free RAM, 102,655 on two of S3a's eight boots and 102,503 on two of
S3b's, so it is the firmware's (the Sendspin start task's stack freed before the line is printed),
not a board's. With the AC-4 image's cross-check in PR 12, both S3 profiles have been run on both
boards, and the two agree to within the rows' own noise. S3a was put back on its 4065 and released.

**Re-recorded (the two AC-4 profiles, the 40 KB decode stack).** #1446 gave the burst player's
decode task 40,960 bytes where the decoder is, and the two AC-4 profiles locked 32,768
(`ICLFORGE_EXAMPLE_SENDSPIN_DECODE_STACK_BYTES`). The lock commit moves the value, which is a
changed locked value, so both baselines are re-recorded here (the two keys #1446 added at an
inert value are not: those baselines were kept and carry the hash note). Same image on both,
`v0.10.0-beta.1-4517-g84efde0e7` built with the `measure` overlay, five plays a row after eight
restarts, then a compare run (two plays, three boots) that holds with no finding:

| | hash | boot `internal_free` | `largest` | `headroom` |
| --- | --- | --- | --- | --- |
| `p4-rev1-ac4`, P4a | `6cd3fe4b015f` to `85e6b4f66269` | 289,271 to 281,063 | 217,088 to 208,896 | 176,128 to 167,936 |
| `s3-n16r8-ac4`, S3a | `6124ef3f591f` to `d7a1513e42b6` | 52,075 to 51,699 | 31,744 to 31,744 | 23,552 to 23,552 |

The P4's three boot figures each fall by exactly 8,192 bytes, which is the stack: on the P4 the
Sendspin decode task's stack is internal SRAM (`..._DECODE_STACK_IN_PSRAM` defaults to 1 only on
an S3 with PSRAM), so the board pays for it at boot. On the S3 it is in PSRAM, so the internal
figures do not move (376 bytes of free RAM, inside the S3's bimodal spread, and the largest block
and headroom equal). The rows hardly moved either way: on the P4 AC-3 and E-AC-3 2.0 at 61 and 40
permille (61 and 41), `ac4-20-music` 239 (241), `ac4-20-acpl` 289 (281), the folded 5.1 row 603
(617) and `ac4-20-companding` 286 (282); on the S3 120 and 84 (120 and 84), 517 (510), 644 (645)
and 605 (608). **Every PCM hash is equal to the one before, and to the host's pin where there is
one**, and `decode_stack_free` did not move on a row of the P4 (the plays run on the `/play`
decode task, not the burst player's stack). Both boards were put back on the images they had, and
their locks released.

### Sendspin rows (PR 14)

**Built.** A row of `suite.json` with `"via": "sendspin"` is played by the repository's
`hearth-testserver` instead of `POST /play`, and the board's own `sendspin.stream=` line is read
the way a `/play`'s `result=` line is (`tools/hearth/suite_sendspin.py`; the rules for the
harness, what it does to a board's pairing table and why it does not poll, are in
[the bench page](esp32-board-bench.md#sendspin-rows)). It exists because the C6 refuses an HTTP
play on purpose, so until now nothing held the audio it is sold for.

**The figures** of such a row are the `/play` rows' where there is one (`frames`,
`decoder_permille`, which is the board's mean time to decode a burst over the burst's 32 ms,
`decode_stack_free`, `units`) and these: `plays` and `glitched_plays` (below), `play_heap_free` (the
least internal heap while the stream played, which the board resets at each stream's start), the
worst of `late`, `dropped`, `invalid` and `resyncs`, and `ring_high`. A play that stops short, a
board that goes away, a reboot, or a stream of another kind is a finding. A stream that **plays
through and runs the board dry** is a *glitch*: the C6 does it on about one 30 s AC-3 stream in
seven (two of the thirteen plays made before the recording on 2026-10-12: three underruns and
three resyncs in one, one of each in another), so a row holds how many of its plays did, and `compare` allows one more than the
baseline had (`glitched_plays` in the tolerance), which five plays a row make a real margin. Zero
cannot be the figure, and a recorded zero from a lucky five would fail the next honest run.

**Recorded, `c6-16mb`, first** (the C6, `.117`, the image `v0.10.0-beta.1-4520-g26274cd77-` built
from this PR's tree, profile hash `ebc59bd7d7f8`, five plays a row after eight restarts,
2026-10-12): `ac3-2.0-sendspin` plays 938 bursts at 661 permille of a burst (the C6's decoder is
about 660 against the S3's 120 and the P4's 61), 0 of 5 plays glitched, `decode_stack_free`
11,120 bytes, and the least internal heap during a stream 27,968 bytes (it read 17,124 to 52,560
over the plays made). The boot line is the worst of eight: `internal_free=108792 largest=88064`.

**Recorded again with the E-AC-3 row** (PR 16, the image `v0.10.0-beta.1-4569-g9643eba39-`, profile
hash `cc71ee4908ff`, five plays a row after eight restarts, the decoder's feature tables out of
static RAM and the play floor at 160,796, 2026-10-12):

| | Before (4520) | After (4569) |
|---|---|---|
| static internal RAM (DIRAM) | 144,139 | 116,307 |
| boot, worst of eight: free, largest block | 108,792, 88,064 | 134,196, 116,736 |
| `ac3-2.0-sendspin`: permille, glitched of 5, least heap | 661, 0, 27,968 | 658, 0, 53,536 |
| `eac3-2.0-sendspin`: permille, glitched of 5, least heap | not in the list | 743, 1, 6,836 |
| E-AC-3 least heap, other plays after AC-3 streams in the same boot | 916 to 4,496, and the row failed | 26,788 (least of 3); 30,920, 18,936, 26,144, 24,868, 23,288 and 27,732 (single plays) |

The E-AC-3 stream plays all 938 bursts every time, after AC-3 streams in the same boot, which is
where it failed to allocate its Wi-Fi receive buffers. One E-AC-3 play in five to six ran the board
dry (an underrun and a resynchronisation; the decoder is at 743 permille of a burst against the
AC-3's 658, so it has less to give a late chunk), which is what a row's `glitched_plays` is for.
The recorded 6,836 is the least of five and was not seen again in nine single plays, which read
18,936 and up; the row's tolerance on it is 3,072 bytes, so a play that took the room back to
where it was (916 to 4,496) fails.

**After PR 1494** (the decoder holds the spectra of the streams a frame codes, not of seven: 79,069 -> 54,083 bytes live for this stream on the host's fixed tier, and 24,576 less on the board; the image `v0.10.0-beta.1-4738-g357812dae`, profile hash `0021e64331a6`, same board and sequence, 2026-10-13):

| | before | after |
|---|---|---|
| `eac3-2.0-sendspin`: permille, glitched of 5, least heap (the least of five) | 743, 1, 6,836 | 756, 0, 25,836 |
| single E-AC-3 plays after AC-3 streams, least heap | 30,920, 18,936, 26,144, 24,868, 23,288, 27,732 | 53,772, 52,968, 55,532, 28,748, 44,712, 48,836, 57,648 |
| E-AC-3 plays that ran dry, over the plays made | 3 of 14 | 0 of 15 |
| `ac3-2.0-sendspin`: permille, glitched of 5, least heap | 658, 0, 53,536 | 660, 0, 56,104 |

The median of the single plays moved by 27 KB and the dips, which are the network's (Wi-Fi's
receive buffers and lwIP take what is free in a burst), moved with it. The record's own figure is
the least of five and is the noisier one; the tolerance on it (3,072) is the same.

**What the E-AC-3 stream's plays were before** (kept for what it measured, 2026-10-12):

The same board played the E-AC-3 stream through Sendspin with almost no internal heap spare,
and not reliably:

| | plays | outcome | least internal heap |
|---|---|---|---|
| 4065 image (a plain build), the board not polled | 3 | clean, 938 bursts each | 2,616, 2,856, 3,192 |
| 4065 image, the server polling `/status` every second | 2 | one clean, one **rebooted the board** | 2,652 (the clean one) |
| `c6-16mb` at this tree, the first stream after a boot | 3 | clean, 938 bursts each | 916 (the least of three) |
| `c6-16mb` at this tree, after three AC-3 streams in the same boot | 3 | the row **failed**: one play stopped at 170 of 938 bursts and another at 401; 51 late chunks and 8 underruns across the three | 4,292 (the least of three), 4,496 (one of them) |

The failing plays print `heap_caps_malloc could not allocate 1848 bytes (caps 0x1800); internal
free 11016, largest 1824` over and over: the Wi-Fi receive buffer (1,848 bytes) cannot be had
from a heap that has 11 KB free and no block over 1,824 bytes. It is fragmentation as much as
total free, the failure the C3 session found on 2026-09-17. It was why the row was not in the
list: a baseline recorded from a lucky first-stream-after-boot play would have called a board
that fails after an AC-3 stream "holding". The row is in the list since PR 16, after the AC-3 one,
as a user plays them.

### The C6's static RAM budget was wrong (PR 14)

`internal_static_bytes_max` was 108,000 for both C6 profiles, from a figure of 103,823 that PR 3
took as the C6's. It is not a figure any C6 `hearth_sink` image has had: from the IDF's own
size tool on the maps of builds back to 2026-10-01 the image's `DIRAM` was 147,123, 144,091,
137,067, 144,115, 144,139 (and `libiclforge_ac3_minimal.a` alone holds 39,864 bytes of `.bss`, in
function-local static tables). Nothing flagged it because CI's C6 steps have not run: in the two
`main` runs read on 2026-10-12 the minimum-footprint leg above them in the same job failed and
the C6 steps were skipped. The first `bench_flash.py build c6-16mb` of this tree stopped at the
guard with 144,139 against 108,000, which is how it was found. **The budget is now 151,000** (the
measured 144,139 and 4.8%, as the S3's 185,000 is its 176,854 and 4.6%). A budget is not in the
profile hash, so no baseline moves.
Static RAM is also what the C6's heap is the remainder of: the same tables were the
reason the E-AC-3 stream above had so little left.

**Then lowered to 122,000, with the tables out.** The 39,752 bytes of `.bss` of
`libiclforge_ac3_minimal.a` in the linked image (not the archive's 39,864: the three
translation units' copies of `fixed_imdct_tables()` are one weak symbol in the image, and that
table, 5,728 bytes, is every stream's) were tables of tools a plain stereo stream never uses.
Taken out, each where its tool is: the ECPL double tables (`xcos3_table`, `ecpl_y`: 10,240) are no
longer linked, their narrowed copies being evaluated from the same expressions where the scratch
makes them; the 512-point DFT's twiddles (5,536) and the transient pre-noise cross-fades
(3,072) are members of the heap objects that exist only for a stream that uses those tools; and
JOC's MDCT-domain direct-form branches, which a profile with no direct form cannot take and whose
stubs they would have called, no longer name the double transforms, so `mdct.cpp`'s double
tables (8,928) are not linked. DIRAM of both C6 images 144,139 -> **116,307**
(`.dram0.bss` 81,736 -> 53,904), budget 116,307 and 4.9% = 122,000. What is left of the
library's 11,976 bytes is `fixed_imdct_tables` (5,728), JOC's float MDCT tables (4,496, which
only an object stream in the MDCT domain reads) and 1,752 of small tables.

**Then 4,484 bytes more, and 117,000.** What was left of the library's tables was
`fixed_imdct_tables` (5,728, every stream's), JOC's float MDCT tables (4,496) and 1,752 of small
ones. The float tables are constants now (flash rodata) in a build whose decoder is not float, since
only an object stream in the MDCT domain reads them and a C6 never does; the fixed pair's tables
are initialised data (`constinit`, `.data`) and not built by the first block that decodes (800
software-double cosines on a part with no FPU). They are **not** constants in flash on purpose, and
the measurement is the reason: on the C6 the flash cache is 16 KB, shared by the code and the data
with 32-byte lines, and the same tables as read-only constants cost 15 permille of a burst
(AC-3 656 -> 671, E-AC-3 751 -> 766, bracketed by two runs of the baseline: 656/751 and 657/752),
against 3 to 5 (noise: 661, 755) as initialised data. DIRAM of both C6 images 116,307 -> **111,823**
(`.dram0.bss` 53,912 -> 43,664, `.dram0.data` 15,927 -> 21,655, 4,492 net), budget 111,823 and 4.6%
= 117,000. Timing on the board with the change: AC-3 658, E-AC-3 755 permille. The S3's and the
P4's float tables are the decoder's own scalar there, so they stay built on first use in RAM (the
S3 image's `fast_mdct_tables<512, float>` is still `.bss`).

**The play floor went up with it.** The HTTP play's refusal (134,008, above) was a sum of what a
play takes, and it refused every play because the part had 117 KB free with the Sendspin player
up. With 136,648 to 140,404 free at boot (the worst and the best of eight) the same floor let an
AC-3 2.0 play start and run its 938 units beside Wi-Fi buffer allocations failing at 6 to 14 KB
free, which is the failure the refusal is for: the suite's `*-refused` rows played, and the
harness died on a `/status` that timed out. The floor is now 160,796: the 134,008 and the 26,788
bytes the network and the session were left at their least beside an E-AC-3 stream, so a play
over HTTP is refused unless it leaves them their room. Whether HTTP plays should work on the C6
is still the owner's call; this keeps the answer the profile gave.

### Sendspin rows for the S3 and the P4 (PR 15)

**Built.** The four profiles with a board gain `ac3-2.0-sendspin` and `eac3-2.0-sendspin`, played
by the test server as the C6's are ([PR 14](#sendspin-rows-pr-14); the harness's rules are on
[the bench page](esp32-board-bench.md#sendspin-rows)), and their baselines are re-recorded whole,
five plays a row after eight restarts, on the image of each profile (the two AC-4 profiles on their
4517 images, whose profile hashes are the current ones). The `/play` rows came back where they
were within the tolerance, so those baselines were holding; the new figures:

| profile | board | AC-3 over Sendspin (of a 32 ms burst) | E-AC-3 over Sendspin |
|---|---|---|---|
| `s3-n16r8` | S3a | 204 permille, 1 of 5 glitched, heap 58,483 | 167 permille, 2 of 5 glitched, heap 2,935 |
| `s3-n16r8-ac4` | S3a | 200 permille, 0 of 5 glitched, heap 23,991 | 164 permille, 2 of 5 glitched, heap 2,539 |
| `p4-rev1` | P4a | 85 permille, 0 of 5 glitched, heap 253,939 | 67 permille, 0 of 5 glitched, heap 203,315 |
| `p4-rev1-ac4` | P4a | 85 permille, 1 of 5 glitched, heap 246,487 | 67 permille, 0 of 5 glitched, heap 192,879 |

**What a Sendspin row found, and what it did not.**

- **The bursts that were "missing" were late.** An E-AC-3 stream that played 932 bursts and had
  6 arrive late is the server's own `did not play every burst: 6 late`, and the first judge called it
  a stream that lost six. A stream's bursts are *accounted for* when played, late or dropped
  (`suite_sendspin.accounted()`, the server's own test); a late one is a glitch and is counted as
  one, and a burst that is none of them is a finding.
- **The S3's E-AC-3 stream starts late.** Two of five plays in each S3 profile lost 11 to 17 bursts
  to lateness at the start of the stream (0.35 to 0.55 s of audio), and one of the two E-AC-3
  plays in the probe on S3a's 4065 image lost two. The S3's decoder is not the cause (164 to 167
  permille of a burst) and the loss is at the start of the stream, which suggests the start
  alignment; that is not investigated. The P4 is better on the profile images (none of ten
  E-AC-3 plays on 4565 and 4517 lost one; one AC-3 play of five in `p4-rev1-ac4` lost 7) than on
  the 3556 image in the probes, where three of five E-AC-3 plays lost 1 to 6. The S3 rows are
  held as they are, with the baseline's glitched plays and two more allowed, and the loss is a
  finding for whoever takes the S3's Sendspin start.
- **The S3's internal heap is thin during an E-AC-3 stream.** The least internal heap during it was
  2.5 to 2.9 KB in the recordings (6.6 KB in the probes), and an AC-3 stream after an E-AC-3 one
  read 7.2 KB where the first stream of a boot read 58 to 60 KB (24 KB with the AC-4 decoder in
  the image): the same shape as the C6's ([PR 14](#sendspin-rows-pr-14)). The E-AC-3 row has a
  `play_heap_free_bytes` tolerance of 2,048 so that a play that takes it is the regression. The
  P4's heap is 190 to 250 KB and has none.
- **One hang, not explained.** On the first attempt S3a stopped answering (HTTP and console
  silent) just after its eighth restart, before the first play, with the same image the rerun then
  recorded on without trouble. An esptool reset recovered it. Nothing else was using it; S3b was
  booked by another session for a network-stall experiment around then, which may or may not be
  related. It is noted and not reproduced.

### AC-4 over Sendspin, and the S3's AC-4 baseline re-recorded (PR 16)

**Which baseline was owed a re-record.** #1459 (merged after my S3a and P4a images were built) made
`s3-n16r8-ac4` list `ac4` to a server, at two channels and 25 frames a second
(`ICLFORGE_EXAMPLE_SENDSPIN_AC4` 0 to 1, and a third key, `..._AC4_MAX_FRAME_RATE`, 25), and added
that key to every other profile at 0. Under [the owner's rule](#changing-a-profile) the first is a
**changed locked value**, so `s3-n16r8-ac4` carries a re-record, which it had not had: its baseline
was under `d7a1513e42b6`, the definition is `5654f90ee458`. The key at 0 on the others is an
**inert addition**, so their baselines are kept and say so (`s3-n16r8` `9ed6f4eb5c70` to
`156f7310e8d0`, `p4-rev1` `af3ac10bdf67` to `60ce70a0bf92`, `c6-16mb` `cc71ee4908ff` to
`0021e64331a6`; the P4 AC-4 profile's re-record below is for its new rows).

**Recorded**, `s3-n16r8-ac4` on S3a and `p4-rev1-ac4` on P4a, the images built from `main` at
`e3da66cb3` with the `measure` overlay, five plays a row after eight restarts:

| | boot free / largest block, before to now | `/play` rows, before to now (permille) |
|---|---|---|
| `s3-n16r8-ac4` (S3a) | 51,919 / 31,744 to 66,159 / 31,744 | AC-3 120 to 120, E-AC-3 86 to 82, AC-4 music 517 to 505, A-CPL 642 to 641, companding 601 to 606 |
| `p4-rev1-ac4` (P4a) | 281,191 / 208,896 to 295,707 / 208,896 | AC-3 61 to 61, E-AC-3 40 to 40, AC-4 music 241 to 240, A-CPL 283 to 287, 5.1 604 to 619, companding 278 to 281 |

Every PCM hash is equal to the one before and to the host's pin. The S3's boot free RAM rose by
14,240 bytes between the two images, which
includes #1474's move of the AC-3 decoder's tables out of static RAM (27,776 bytes on the C6); it is
not isolated.

**AC-4 over Sendspin rows.** The AC-4 seeds are three frames, which is not a stream, so the server
plays one over and over for `sendspin_seconds` (20) and the row holds that **every burst the server
sent is accounted for** (its report's `programme.bursts`, 471 or 472 for these). All four seeds are
23.4375 frames a second (`frame_rate_index` 13, 2,048 samples), a burst period of 42,667 µs, which
is the `period_us` the permille is taken over and is inside the S3's 25 frames a second.

| row | S3a, `s3-n16r8-ac4` | P4a, `p4-rev1-ac4` |
|---|---|---|
| AC-3 | 200 permille, 0 of 5 plays glitched, least heap 36,843 | 85 permille, 0 of 5 plays glitched, least heap 259,719 |
| E-AC-3 | 162 permille, 2 of 5 plays glitched, least heap 2,523 | 67 permille, 1 of 5 plays glitched, least heap 207,527 |
| AC-4 2.0 music | 516 permille, 2 of 5 plays glitched, least heap 4,787 | 189 permille, 1 of 5 plays glitched, least heap 17,951 |
| AC-4 2.0 A-CPL | 689 permille, 3 of 5 plays glitched, least heap 64,947 | not in the list |
| AC-4 5.1 (folded to 2.0 on the P4's two-slot sink) | not in the list (the S3 lists two channels) | 590 permille, 1 of 5 plays glitched, least heap 27,687 |

- **The S3 plays AC-4 over Sendspin inside real time, with glitches.** A-CPL, the heaviest 2.0
  stream, takes 689 permille of a burst and glitched 3 plays of five (late chunks and a few
  underruns); the music stream 516 permille and 2 of five. The decoder leaves 0.31 to 0.48 of
  every burst spare, so it looks like the same lateness the S3's E-AC-3 row has
  ([PR 15](#sendspin-rows-for-the-s3-and-the-p4-pr-15)), but where in a stream the late chunks fall
  is not something the board's totals say and it was not measured.
- **Allowances.** A row's `glitched_plays` allowance is now a number by row id: the S3 profiles allow
  the E-AC-3 row two more than its baseline and every other row the default one, so that a row
  that glitched 3 of 5 cannot only fail at 5 of 5. The two AC-4 music and E-AC-3 rows carry a
  2,048-byte `play_heap_free_bytes` allowance on the S3 (4.8 KB and 2.5 KB at their least).

## Changing a profile

A change to a profile, a fragment it names, or a locked key's Kconfig default is one PR that:

1. edits `profiles.json` (the locked value, or the fragment order) and the fragment;
2. regenerates the doc tables (`sink_profile.py table`, checked in CI);
3. attaches `board-run.json` from a board booked for the run, and updates
   `baselines/<profile>.json` if a figure moved;
4. says in the description which keys moved, why, and what the heap table did;
5. decides about every baseline the edit moves, in the same PR: it is re-recorded (a locked value
   that changed), or it is **accepted** in `suite.json` (`profiles.<id>.baseline_accepted`: the
   profile's new hash and why, for a key added at the value it already had).
   `test_board_suite.py` holds it at PR time, and an acceptance names the hash it was made
   against, so the next edit of the profile asks again.

**When a baseline is re-recorded, and when it is only noted (the owner's rule, 2026-10-12).** A
baseline carries the hash of the profile it was recorded under, and the hash is the fragments and
the locked values, so it moves whenever the profile is edited. The two kinds of edit are not the
same:

- **A locked value that changes** (the S3's lyric budget and object count in PR 12) is a change of
  what the image is, where a figure can have moved: the baseline is re-recorded in the same PR.
- **A key that is added at the value it already had in effect** (a new Sendspin switch left off,
  locked so that the guard knows it) re-hashes the profile and moves no figure: **the baselines are
  kept**, and `board_suite.py` says on every compare against one that it was recorded under the
  earlier hash and that the figures are compared as recorded. The PR says which keys it locked.

A kept baseline's hash is then not the file's, and the PR that moved it says so in
`baseline_accepted` (above): a baseline under another hash with no acceptance for the profile's
current hash fails the PR, so the decision is made where the edit is made and not found later (the
S3 AC-4 baseline sat under its old hash for seven hours after #1459 because nothing asked). The
suite still refuses an *image* built under another hash, so a board is only ever run on the
definition the file has.

**A PR that adds a key the guard watches fails at the pull request until a profile locks or frees
it** (`sink_profile.py check-change`). **A PR that changes a locked key's Kconfig default, or a
fragment's value, without the profile file fails at the pull request**, in the `ESP32 profiles`
job ([where each check runs](#where-each-check-runs)). A scratch build for a measurement is `sink_profile.py build <id> --overlay <file>`: it
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
| 7 | Baselines: `s3-n16r8`, `c6-16mb`, `p4-rev1`, then the AC-4 profiles, one PR each with its board run attached | yes | **`s3-n16r8` built** (S3b; see [the ratchet](#3-the-board-suite)) and **`c6-16mb` built** (the C6, the refusal rows and the worst of eight boots) and **`p4-rev1` built** (P4a, by watchdog-reset boots) and **`p4-rev1-ac4` built** (P4a, with the `measure` image and its new fragment, the 5.1 row held to its own hash) and **`s3-n16r8-ac4` built** (S3a, cross-checked on S3b; `s3-n16r8` re-recorded under its new hash); **both AC-4 baselines re-recorded** under the hashes the 40 KB Sendspin decode stack gave them (P4a, S3a). `c6-4mb` has no board: its guard and budgets only, the suite's rows run on `c6-16mb`, which differs in flash and table alone |
| 8 | Board scripts out of `D:\ac3bld` into `tools/hearth/` (build, OTA, the lock), and the process page | no | **Built**: [the board bench](esp32-board-bench.md): `bench.json` (the five boards, their locks, MAC tails), `bench.py` (status, lock, log, snapshot, console, scrub; the lock code the suite now shares), `bench_flash.py` (build from a profile, flash after a MAC check, verify, OTA with the lock). The build and the redaction were run for real; flash, `--app-only` and OTA through the tool were not, because they write a board |

| 9 | The v3.x profile, `p4-rev3`, once #1400 was on main | no | **Built**: the profile, its budgets, CI reading it, the test's exception gone; the P4X kit ran the image on 2026-10-20 and its list and baseline were recorded then |
| 10 | Sendspin rows: the suite plays a stream the way the C6's product path does, with the test server | yes | **Built** (the harness, tests, the C6's AC-3 row and its baseline); the E-AC-3 row joined it in PR 16, once the decoder's feature tables were out of static RAM, see [Sendspin rows](#sendspin-rows-pr-14) |
| 11 | Sendspin rows for the S3 and the P4 profiles, and their baselines | yes | **Built** (S3a and P4a, the four baselines re-recorded); the S3's E-AC-3 stream starts late in two plays of five, see [PR 15](#sendspin-rows-for-the-s3-and-the-p4-pr-15) |
| 12 | AC-4 over Sendspin rows for the two AC-4 profiles, and the S3 AC-4 baseline re-recorded under its #1459 hash | yes | **Built** (S3a and P4a); see [PR 16](#ac-4-over-sendspin-and-the-s3s-ac-4-baseline-re-recorded-pr-16) |

PR 3 follows 2. `p4-rev3` waited for #1400's fragments to be on main (row 9).

## Decisions for the owner

Taken on 2026-10-12: AC-4 on the S3 and on the P4 are their own profiles; PR 1 is S3-only and the
C6 refuses cleanly (option A); the fix's shape is the Kconfig default and the fragment both.

Still open:

1. **The recording board for each profile.** Proposed: S3a for both S3 profiles with S3b as the
   cross-check, C6 `.117` for `c6-16mb`, P4a `.99` for both P4 profiles (P4b needs a USB flash
   after any OTA). Done so: S3a for `s3-n16r8-ac4` with the S3b cross-check, S3b for
   `s3-n16r8` (cross-checked on S3a), the C6 and P4a for theirs.
2. **The tolerance:** now measured, not proposed: the larger of 5% and 5 permille over the
   median, for the decoder's time per frame; 0 underruns; the stack's margin within 512 bytes of
   the baseline's (see the ratchet above). Say if looser or tighter.
3. **QEMU is not the heap authority.** The twin runs the release shape but not Wi-Fi, so a heap
   number from it is a smoke floor. The board suite owns the budget. Confirm that is acceptable.
4. **An HTTP play on the C6.** Left refused. The alternative is giving the Sendspin player's ring
   and stack back for the length of a play, which is a change to the player, not to a profile.
5. ~~**E-AC-3 on the C6 over Sendspin.**~~ Done in PR 16: the heap was fixed (27,776 bytes of the decoder library's feature tables out of static RAM), the row is in the list and recorded ([Sendspin rows](#sendspin-rows-pr-14)); the C6 lists E-AC-3 to a server, as before.

## What this does not claim

- That the S3 release shape keeps up. Its first recorded number is a suite run away; the
  `s3-fast` table is for another shape.
- That QEMU can run the release shape's PSRAM execution. It is a spike (PR 5).
- That the boot line is sufficient. It is an early warning for the failure this page started with,
  and the play list is what holds the rest.
- That the v3.x image works. No board of that revision has run it: `p4-rev3` holds the shape of
  its build (placement, sizes, caches, the screen's keys), not that LVGL draws or the panel lights.
