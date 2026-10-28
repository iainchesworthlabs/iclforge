# AC-4 in the Hearth sink's release images

**Status, 2026-10-28.** A measurement of what one shipping image per board would carry if it decoded AC-3, E-AC-3 and AC-4
together, and of what stops it where it cannot. Twelve images were built on `main` at `956572141` (nothing flashed, no board
touched, nothing pushed); sizes and static RAM are from those builds, boot heap and play figures are the boards' recorded
baselines and the platform pages' board runs, and every figure that is an estimate says so. It is the follow-up to
[decision 42](ac4.md#decisions-of-2026-10-10) and to the profile split of 2026-10-12
([`esp32-platform-profiles.md`](esp32-platform-profiles.md#profiles)).

## Answer

`sdkconfig.ac4` is a superset: a stream that opens with an AC-4 sync word plays as an AC-3 or E-AC-3 one does, and the AC-3 and
E-AC-3 rows of both `-ac4` baselines pass as the release rows do. So the question is only what the extra 1.0 to 1.25 MB of
image, 2 KB of static internal RAM, 8 to 16 KB of internal stack and the decoder's heap do to each board.

| Board | One image with AC-3, E-AC-3 and AC-4 today? | What stops it | Cheapest change |
|---|---|---|---|
| P4 rev 1.3 (`p4-rev1`) | **Yes** | nothing technical: 2.73 MB of a 4 MiB slot, 295 KB of heap at boot, AC-4 5.1 takes 59% of a frame | fold the `ac4` fragment into `p4-rev1` (the 8 locked keys `p4-rev1-ac4` already differs by) and re-record the baseline on P4a |
| P4 rev 3.x (`p4-rev3`, the P4X kit) | **Yes, with the owner's call on margins** | the app budget (2,900,000, image 3,768,832); 425 KB (10.1%) of slot left; 10 to 15 KB of internal heap in an AC-4 5.1 play; the glass adds 10% to that play | fold in; raise `app_bytes_max` to about 3,900,000; record `p4-rev3` AC-4 rows on the kit; a 6 MiB slot (USB only) if the image is to keep growing |
| S3 (`s3-n16r8`) | **No** | internal RAM policy: E-AC-3 needs the 4 KB limit and the AC-4 profile cannot take it; an unresolved suite hang that only the AC-4 profile has; the 64 KB data cache takes 32 KB of heap E-AC-3 does not have; flash is not the problem | board work first: the JTAG post-mortem of the hang, then a per-play allocation limit; until then AC-4 reaches the S3 as PCM from the server |
| C6 (`c6-16mb`, `c6-4mb`) | **No, out** | RAM: the decoder's floor is 286,365 bytes of heap and the part has 117 KB beside Wi-Fi; the 4 MB part also has no slot for it (1.21 MB over) | none that fits; decision 42 stands |

## What was built

Each profile with its fragments as `sink_profile.py defaults` gives them, and again with `;sdkconfig.ac4` after them (for the
two AC-4 profiles, the profile itself). `idf.py size` gave the memory types; the slot is `ota_0` in the profile's partition
table (`partitions.csv`, 4 MiB slots, for the S3, the P4 and the 16 MB C6; `partitions_c6.csv`, 1.875 MiB, for the 4 MB C6).
Build directories are `D:\esp\build\iclforge\ac4rel-*`. Two traps for whoever repeats it: builds of different targets from
one checkout race on `firmware/hearth-sink/managed_components` (the component manager prunes the other target's components and
fails with `PermissionError`), so build one target per checkout or one at a time; and `bench_flash.py build --overlay` takes
only the overlays `profiles.json` declares, not `ac4`, so the AC-4 builds were run with `idf.py` and the defaults string.

The P4X figures are 40,000 bytes below the ones that started this page (3,810,752 and 2,797,600) because this tree is `main`
and that one carried other work; the difference between the two builds is the same, 1.01 MB.

| Build | App bytes | `ota_0` slot | Slot left | Budget (`app_bytes_max`) | Static internal RAM (DIRAM) |
|---|---:|---:|---:|---:|---:|
| `s3-n16r8` | 1,635,168 | 4,194,304 | 2,559,136 | 1,750,000 | 163,846 (budget 185,000) |
| `s3-n16r8` + `sdkconfig.ac4` | 2,621,328 | 4,194,304 | 1,572,976 | 1,750,000: **871,328 over** | 165,814 |
| `s3-n16r8-ac4` (the profile) | 2,621,520 | 4,194,304 | 1,572,784 | none | 165,814 |
| `p4-rev1` | 1,720,064 | 4,194,304 | 2,474,240 | none | 148,206 |
| `p4-rev1-ac4` (the profile) | 2,732,064 | 4,194,304 | 1,462,240 | none | 150,262 |
| `p4-rev3` | 2,757,920 | 4,194,304 | 1,436,384 | 2,900,000 | 161,770 (budget 175,000) |
| `p4-rev3` + `sdkconfig.ac4` | 3,768,832 | 4,194,304 | 425,472 | 2,900,000: **868,832 over** | 163,818 |
| `p4-rev3` + AC-4 + stage timers | 3,771,888 | 4,194,304 | 422,416 | | 167,322 |
| `p4-rev3` + AC-4, hot `-O2`/`-O3` units off | 3,554,416 | 4,194,304 | 639,888 | | 163,850 |
| `c6-16mb` | 1,927,904 | 4,194,304 | 2,266,400 | 2,000,000 | 112,887 (budget 117,000) |
| `c6-16mb` + `sdkconfig.ac4` | 3,179,696 | 4,194,304 | 1,014,608 | 2,000,000: **1,179,696 over** | 114,951 |
| `c6-4mb` | 1,927,904 | 1,966,080 | 38,176 | 1,966,080 | 112,887 |
| `c6-4mb` + `sdkconfig.ac4` | 3,179,696 | 1,966,080 | **-1,213,616** | | the link refused it |

The decoder is the same size on every board: `libiclforge_ac4_static.a` is 954,748 bytes in the S3 image (411,567 of code and
the rest read-only data), and on the P4 the new symbols are 435,296 bytes of code, 540,157 of read-only data and 2,047 of
`.bss`. Static internal RAM goes up 1,968 to 2,064 bytes on every board; that is all AC-4 asks of it. What it asks of the heap
is dynamic, and is in each board's section.

## ESP32-P4, silicon v1.x (`p4-rev1`)

| | `p4-rev1` | `p4-rev1-ac4` |
|---|---|---|
| App | 1,720,064 | 2,732,064 (65% of the slot; 1,462,240 left) |
| Budget | none | none |
| Static internal RAM | 148,206 | 150,262 |
| Free internal heap at boot (P4a, recorded 2026-10-12) | 291,783, largest 217,088 | 295,707, largest 208,896 |
| Free heap in a Sendspin play (the baseline's `play_heap_free`) | AC-3 253,939, E-AC-3 203,315 | AC-3 259,719, E-AC-3 207,527, AC-4 2.0 17,951, AC-4 5.1 27,687 |
| Decoder time in a Sendspin burst | AC-3 2.7 ms, E-AC-3 2.1 ms | AC-3 2.7 ms, E-AC-3 2.1 ms, AC-4 2.0 8.1 ms (19% of the 42.7 ms frame), AC-4 5.1 25.2 ms (59%) |

**Verdict: yes today.** The two profiles differ in these 8 locked keys and nothing else: `ICLFORGE_AC4`,
`ICLFORGE_AC4_INTERNAL_BELOW` 16384, `AC4_CORE`, `AC4_PARALLEL`, `AC4_PCM_HASH`, `DECODE_STACK_BYTES` and
`SENDSPIN_DECODE_STACK_BYTES` 32,768 to 40,960 (internal RAM on the P4, so up to 16 KB of the 295 KB), and
`SENDSPIN_AC4` 1. AC-3 and E-AC-3 rows are unchanged between the baselines. The sink lists 6 coded channels, so 5.1.4 AC-4
(1.0 to 1.2 of real time on this part, [the P4 page](../docs/platforms/bare-metal/esp32-p4.md#what-it-decodes-in-real-time))
stays with the server as PCM. TDM above two channels cannot open on this silicon, so the 5.1 rows play folded to 2.0 here.
The only reason it is not already one image is the owner's decision of 2026-10-12 to keep AC-4 as its own profile because it
changes locked keys.

**The change:** `p4-rev1` takes the `ac4` fragment and the eight values above; `p4-rev1-ac4` and its baseline go; the AC-4 rows
(`ac4-20-music`, `ac4-20-acpl`, `ac4-20-companding`, `ac4-51-music`, and the two Sendspin ones) join `p4-rev1`'s play list;
the baseline is re-recorded on P4a, because a changed profile hash refuses the old one. CI already builds both shapes
(`_build.yml`), so it loses a build and gains none. **Not prepared on this branch:** it edits `profiles.json`, which is the
owner's, and the re-record needs a board.

## ESP32-P4X, silicon v3.x (`p4-rev3`)

| | `p4-rev3` | `p4-rev3` + AC-4 |
|---|---|---|
| App | 2,757,920 | 3,768,832 (89.9% of the slot; **425,472 left**) |
| Budget | 2,900,000 | 868,832 over |
| Static internal RAM | 161,770 (budget 175,000) | 163,818 (11,182 under); 167,322 with stage timers |
| Free internal heap at boot (kit, recorded 2026-10-26) | 104,523, largest 69,632 | about 102,500 (estimate: the static difference) |
| Least free in a play | AC-3 and E-AC-3 with the panel: 48.9 to 65.7 KB | AC-4 5.1: 9.7 to 10.4 KB on 2026-10-20, 11 to 15 KB after the TDM queue fix (#1607); the decoder takes what is left by design |
| Decoder time in a Sendspin burst, kit at 400 MHz | AC-3 5.1 ms, E-AC-3 4.5 ms (layout 9.1.6) | AC-4 2.0 12.7 ms (30%), AC-4 5.1 29.3 to 29.6 ms at layout 5.1 on the glass (69%), 31.7 ms at 9.1.6 (74%); the glass adds 10% |

**Verdict: yes, but with three things the owner should decide**, all from [the kit's own runs](karaoke-p4x-panel.md#ac-4-51-at-400-mhz)
and not from my builds:

- **Flash.** The image fits an OTA slot with 10.1% to spare; the 2,900,000 budget is a tripwire the owner set on 2026-10-25
  and has to move to about 3,900,000 (leaving 294 KB to the slot). The slot is then the only limit, and 473 KB of the image is
  fonts. A 6 MiB slot pair fits the 16 MB part (`reserve`, 4 MiB, is unused: the table would end at `0xCB0000` of
  `0x1000000`), but a partition table changes over USB only, so each kit needs one cable flash.
- **Internal RAM.** 10 to 15 KB at the least of an AC-4 5.1 play, against 27.7 KB on the v1.3 board. The AC-3 and E-AC-3 plays
  keep their headroom, minus the decode stacks' extra 8 KB each (internal on this part).
- **Time.** AC-4 5.1 on the glass fails the screen plan's "decode moves by less than 5%" pass (10%); 5 of 18 plays at layout 5.1
  glitched, which a later session traced to the stream's first chunk arriving 300 to 600 ms late, not to the glass. AC-4 5.1.4
  is not real time here either and the sink states 6 channels, so the server decodes it.

**The change:** fold `ac4` in as for `p4-rev1`; raise `app_bytes_max`; record the `p4-rev3` AC-4 rows (the kit's scratch run
of 2026-10-20 is the figure to hold them to). The kit was in use by another session while this was done, so none of that was run.

## ESP32-S3 (`s3-n16r8`)

| | `s3-n16r8` (release) | `s3-n16r8` + `sdkconfig.ac4` | `s3-n16r8-ac4` (as it is) |
|---|---|---|---|
| App | 1,635,168 | 2,621,328 (62.5% of the slot) | 2,621,520 |
| Budget | 1,750,000 | 871,328 over | none |
| Static internal RAM | 163,846 (budget 185,000) | 165,814 | 165,814 |
| Data cache / allocation limit | 32 KB / 4,096 (`psram-4k`) | 32 KB / 4,096 | 64 KB / 16,384 |
| AC-4 listed to a Sendspin server | n/a | **no**: `SENDSPIN_AC4` defaults to 0 without the 64 KB cache | yes, 2 channels, 25 fps |
| Free internal heap at boot (S3a, recorded) | 110,195 (2026-10-25) | about 108,000 (estimate: the static difference) | 66,159 (2026-10-12) |
| Free heap in an E-AC-3 2.0 Sendspin play (`play_heap_free`) | 79,131 | | **2,523**, 2 of 5 plays glitched |
| AC-4 burst, 2.0 (share of the frame) | n/a | | music 22.0 ms (52%), A-CPL 29.4 ms (69%); 2 and 3 of 5 plays glitched |

**Verdict: no, and it is not flash.** The image has 1.57 MB of slot left and 2.5 MB of the 8 MB PSRAM holds its code and
constants. What stops it:

1. **E-AC-3 and AC-4 want opposite allocation limits.** [`sdkconfig.psram-4k`](../firmware/hearth-sink/sdkconfig.psram-4k) is
   in the release profile because at 16 KB an E-AC-3 stream took about 65 KB of internal heap and left 247 to 543 bytes at its
   tightest, where a failed allocation aborts the board; at 4 KB it leaves 70 KB or more. The AC-4 profile cannot take it
   ("its board suite hung at the first play after eight restarts, twice in two attempts") and keeps 16 KB, so today's AC-4
   image has the E-AC-3 behaviour the release profile was changed to get away from, with a 64 KB data cache on top, which is
   32 KB less heap again. Decision 43 measured 1,479 bytes free at the least in an E-AC-3 7.1.4 play on that cache, before the 4 KB limit existed.
2. **A hang only the AC-4 profile has.** Three suite runs of `s3-n16r8-ac4` locked up (the network dead, the CPUs alive, twice
   in E-AC-3 Sendspin plays) and `s3-n16r8` ran clean twice on the same server
   (memory note `s3-ac4-profile-hang-not-acpl-specific-2026-10-26`; finding 28 in the owner's `ma-atmos-e2e` folder). It is
   unresolved, and folding AC-4 in would carry it to every S3 user.
3. **Real time is narrow.** With the 64 KB cache the S3 keeps up at 2.0 up to 25 frames a second and no wider
   ([decision 42](ac4.md#decisions-of-2026-10-10)); its recorded AC-4 plays glitched in 2 and 3 of 5. Without the cache, 2.0
   A-SPX is 0.61 of a frame on the HTTP path against 0.50 with it (the table in `sdkconfig.s3-dcache`), which is why the
   Kconfig default stops listing AC-4 there.
4. **The budget** moves from 1,750,000 to about 2,700,000. Not a blocker.

**The change, in order, all of it needing the S3 and an owner window:** (a) the JTAG post-mortem of the next hang
(OpenOCD on the board's USB-Serial-JTAG, `thread apply all bt`); (b) the player already sets `SPIRAM_MALLOC_ALWAYSINTERNAL` for
an AC-4 play and puts it back after (`ICLFORGE_AC4_INTERNAL_BELOW`, 512 on the S3), so the idle value can be 4,096 with AC-4
plays at 512; that was not measured with the cache at 64 KB and not on a build where the hang is understood; (c) choose the
cache: 32 KB keeps E-AC-3's heap and halves the AC-4 margin, 64 KB the other way. Until then the S3's image decodes AC-3 and
E-AC-3 on the board and AC-4 reaches it as PCM from Hearth, which is how decision 42 already has it.

## ESP32-C6 (`c6-16mb`, `c6-4mb`)

**Out, on RAM first and on flash for the 4 MB part.**

- **Flash.** The AC-4 image is 3,179,696 bytes. The 16 MB part's 4 MiB slots hold it (1,014,608 left; the 2,000,000 budget
  would have to move). The 4 MB part's 1.875 MiB slots do not: the build stops 1,213,616 bytes over, which is also where the
  PR size gate ([#1690](https://github.com/iainchesworthlabs/iclforge/pull/1690)) would stop it.
- **RAM.** The decoder's smallest peak is 286,365 bytes of heap at 2.0 (the fixed-point tier on RISC-V, from the probes,
  [the C6 page](../docs/platforms/bare-metal/esp32-c6.md#ac-4)). The C6 has 136,668 bytes free at boot with Wi-Fi up (the
  baseline), 117 KB when a play starts, and no PSRAM. The board ran it on 2026-10-10 and aborted on a 27,264-byte allocation
  with 7,952 free; the player now refuses with `why: "memory"`. Static internal RAM is not the issue: 114,951 against a
  117,000 budget. The two decode stacks, internal on this part, would grow by 24,576 bytes (32,768 to 40,960 and 24,576 to 40,960)
  on top of that.
- **Time (an estimate).** On the S3 and the P4, AC-4 2.0 in SIMPLE mode is four to six times the cost per second of audio of
  AC-3 2.0 (P4 0.24 against 0.06, S3 0.50 against 0.12). The C6 already spends 0.66 to 0.76 of real time on AC-3 and E-AC-3 2.0.
  Nothing at that ratio keeps up, but the question does not arise before the RAM one.

What this rests on: my two builds, the C6 page's board run and probe peaks, and the baseline's boot figures. No new C6
measurement was made.

## What the decoder costs, and the levers

| Lever | Measured |
|---|---|
| A bigger app slot | The 16 MB boards' `reserve` partition (4 MiB) is unused. Two 6 MiB slots end at `0xCB0000`. Over USB only. Needed only by `p4-rev3`, and only to keep growing. |
| A smaller decoder | The new read-only data on the P4 is 540 KB: the 1001/960 converter table (`kHalfTable<1001, 960>`, 188,376 bytes, for 23.976 and 29.97 fps), `kPow43` (32,768), nine inverse-transform tables for 384 to 2,048 samples (128,508 together) and the syntax tables. Dropping a frame length or a rate is a decoder change, and buys slot space no board but the P4X is short of. |
| Dropping stage timers | Already off in every release build. Turning them on costs 3,056 bytes of image and 3,504 of static internal RAM: not a lever. |
| Optimisation level | `-Os` already. The hot units at `-O2`/`-O3` (`ICLFORGE_MINIMAL_HOT_O2`) are worth 214,416 bytes (5.7%) of the P4X AC-4 image if turned off, at a decode time not measured (the P4 page has `-O3` on the transforms at 2.7 ms of a 5.1 frame) and for AC-3 and E-AC-3 too. Not recommended for the one board that has no time to spare. |
| Tables to PSRAM | Already so on the P4 (`SPIRAM_XIP_FROM_PSRAM`) and the S3 (`SPIRAM_FETCH_INSTRUCTIONS`, `SPIRAM_RODATA`): the image is flash and PSRAM, internal RAM sees 2 KB of `.bss`. There is nothing left to move; the C6 has no PSRAM. |
| The decode stacks | 40,960 each (two tasks), in internal RAM on the P4 and C6, in PSRAM on the S3. The decoder used 20 to 25 KB of them in the plays recorded, 28.6 KB in A-CPL mode 3. 36 KB would keep a 7 KB margin and give back 8 KB on a P4, and `*STACK*` keys are watched. |

## CI

- **PR size gate (#1690)** holds the 4 MB C6 image to its slot and nothing else; no other budget is checked until the push to
  `main` (`ci.yml`, `verify --size-json`). A `p4-rev3` image at 90% of its slot is a case for adding it to the gate's list.
- **The profile guard** (`_profiles.yml`) configures every profile and holds its locked keys: folding is a `profiles.json`
  change with a new profile hash and a board run, as the file's `note` says.
- **Builds:** `_build.yml` already builds `s3-n16r8-ac4` and `p4-rev1-ac4` (not run). Folding removes those two and makes the
  release builds 1 MB larger.
- **QEMU** runs only the S3 release shape (`s3-n16r8` with `qemu-net`); there is none for the P4 or the C6, and no AC-4 QEMU
  leg. An S3 release image with the decoder would run its boot gate with 2 KB less heap and the decoder idle.
- **The board suite** (`board_suite.py`) refuses a baseline recorded under another hash: every folded profile needs its run.

## What could not be determined without a board

- Boot heap and least-free of every AC-4 variant that has no baseline (`p4-rev3` + AC-4 beyond the kit's scratch run, the S3
  overlay, the C6): the boot figures above are the static difference added to a recorded one.
- Whether an S3 with the 4,096 limit idle and 512 in an AC-4 play still hangs, and whether the 64 KB cache and E-AC-3 7.1.4 fit.
- AC-4 2.0 through Sendspin on an S3 with the 32 KB cache (A-SPX 0.61 on the HTTP path is the nearest figure).
- Any C6 time.

## Reproducing

```
python tools/hearth/sink_profile.py defaults p4-rev3
idf.py -C firmware/hearth-sink -B D:/esp/build/iclforge/<tag> -DSDKCONFIG=D:/esp/build/iclforge/<tag>/sdkconfig \
  -DIDF_TARGET=esp32p4 "-DSDKCONFIG_DEFAULTS=<that string>;sdkconfig.ac4" \
  -DICLFORGE_PROFILE=p4-rev3 -DICLFORGE_PROFILE_HASH=<identity> build
idf.py -C firmware/hearth-sink -B D:/esp/build/iclforge/<tag> size --format json2
```
