# AC-4 in the Hearth sink's release images

**Status, 2026-10-29 (written in two parts the same day).** A measurement of what one shipping image per board would carry if it
decoded AC-3, E-AC-3 and AC-4 together, and of what stops it where it cannot, and then, on the owner's go-ahead ("if we need to fold
the profiles together, that's fine. The budget can be grown"), the folds themselves: **the S3, the P4 v1.x and the P4X images now
decode all three, and the C6 stays AC-3 and E-AC-3 only.** Twelve images were built on `main` at `956572141`; sizes and static RAM
are from those builds. Boot heap and play figures are the boards' recorded baselines, the platform pages' board runs and, for the
S3 and the P4X after the fold, the boards' runs of 2026-10-29 (S3a recorded, S3b held to it, the kit recorded); every figure that
is an estimate says so. The S3 hang's fix and the P4 v1.x fold are
[#1697](https://github.com/iainchesworthlabs/iclforge/pull/1697); the S3 and P4X folds are its follow-up. It is the follow-up to
[decision 42](ac4.md#decisions-of-2026-10-10) and to the profile split of 2026-10-12
([`esp32-platform-profiles.md`](esp32-platform-profiles.md#profiles)). Boards: the S3s at 192.168.1.156 and .45 and the P4X kit,
each leased and put back as found.

## Answer

`sdkconfig.ac4` is a superset: a stream that opens with an AC-4 sync word plays as an AC-3 or E-AC-3 one does, and the AC-3 and
E-AC-3 rows of both `-ac4` baselines pass as the release rows do. So the question is only what the extra 1.0 to 1.25 MB of
image, 2 KB of static internal RAM, 8 to 16 KB of internal stack and the decoder's heap do to each board.

| Board | One image with AC-3, E-AC-3 and AC-4 today? | What stops it | Cheapest change |
|---|---|---|---|
| P4 rev 1.3 (`p4-rev1`) | **Yes, folded** (#1697) | nothing technical: 2.73 MB of a 4 MiB slot, 295 KB of heap at boot, AC-4 5.1 takes 59% of a frame | done: the `ac4` fragment is in `p4-rev1`, the same hash as the old AC-4 profile, so P4a's baseline stands |
| P4 rev 3.x (`p4-rev3`, the P4X kit) | **Yes, folded** (follow-up PR) | the kit's first run of the AC-4 image failed: after the I2S queue the largest internal block was 37,888 bytes, under the 40,960-byte decode stack (AC-3 and E-AC-3 plays could not start) and the 24,576-byte lane stack (AC-4 5.1 ran on one core at 1.3 to 1.4 of real time), and the board's `GET /log` lost the tail of every long line, so the suite could not read the AC-4 rows | done: a stack goes to PSRAM when no internal block holds it, the HTTP play's decode stack is locked in PSRAM on `p4-rev3`, `log_write` reports the console's failed write whole; 3,811,472 bytes against a budget of 3,950,000, AC-4 5.1 at 0.68 of a frame, AC-3, E-AC-3 and TDM as fast as before |
| S3 (`s3-n16r8`) | **Yes, folded** (follow-up PR) | a Wi-Fi receive-buffer deadlock that is S3-only (`LWIP_TCP_OOSEQ_MAX_PBUFS` is 0 on the S3 and 4 on the P4 and C6) and was **not the AC-4 decoder** (the release profile showed it too); then the E-AC-3 heap policy, which one image can have | done: the bound (#1697), then `ac4` and `s3-dcache` in `s3-n16r8` with `psram-4k` kept, budget 2,750,000, S3a recorded and S3b held to it; 2,621,600 bytes, 75.5 KB of heap at boot, E-AC-3 leaves 44 KB |
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

**Done on this branch (the owner, 2026-10-29: "if we need to fold the profiles together, that's fine. The budget can be
grown"):** `p4-rev1` takes the `ac4` fragment and the eight values above, `p4-rev1-ac4` is gone, its play list is
`p4-rev1`'s and its baseline is `p4-rev1`'s. The folded profile has the same fragments and the same locked values as the old
AC-4 profile, so the **same hash (`f797fb39d0f9`)**: P4a's baseline of 2026-10-12 is for exactly this definition and needs no
re-record; only the name and the budget moved (`app_bytes_max` 3,400,000, 668,000 over the image and 794,000 under the slot).
CI's two P4 steps build the one profile, once as it is and once with the stage timers on. The tests, `check-docs` and the
docs' tables follow.

## ESP32-P4X, silicon v3.x (`p4-rev3`)

| | `p4-rev3` | `p4-rev3` + AC-4 |
|---|---|---|
| App | 2,757,920 | 3,768,832 (89.9% of the slot; **425,472 left**) |
| Budget | 2,900,000 | 868,832 over |
| Static internal RAM | 161,770 (budget 175,000) | 163,818 (11,182 under); 167,322 with stage timers |
| Free internal heap at boot (kit, recorded 2026-10-26) | 104,523, largest 69,632 | about 102,500 (estimate: the static difference) |
| Least free in a play | AC-3 and E-AC-3 with the panel: 48.9 to 65.7 KB | AC-4 5.1: 9.7 to 10.4 KB on 2026-10-20, 11 to 15 KB after the TDM queue fix (#1607); the decoder takes what is left by design |
| Decoder time in a Sendspin burst, kit at 400 MHz | AC-3 5.1 ms, E-AC-3 4.5 ms (layout 9.1.6) | AC-4 2.0 12.7 ms (30%), AC-4 5.1 29.3 to 29.6 ms at layout 5.1 on the glass (69%), 31.7 ms at 9.1.6 (74%); the glass adds 10% |

**Verdict, first reading (from [the kit's own runs of 2026-10-20](karaoke-p4x-panel.md#ac-4-51-at-400-mhz) and my builds): yes, with three things the owner should decide. Revised below by the kit's first run of 2026-10-29 (not as the image was built), and again by the fixes that followed it (yes).**

- **Flash.** The image fits an OTA slot with 10.1% to spare; the 2,900,000 budget is a tripwire the owner set on 2026-10-25
  and has to move to about 3,900,000 (leaving 294 KB to the slot). The slot is then the only limit, and 473 KB of the image is
  fonts. A 6 MiB slot pair fits the 16 MB part (`reserve`, 4 MiB, is unused: the table would end at `0xCB0000` of
  `0x1000000`), but a partition table changes over USB only, so each kit needs one cable flash.
- **Internal RAM.** 10 to 15 KB at the least of an AC-4 5.1 play, against 27.7 KB on the v1.3 board. The AC-3 and E-AC-3 plays
  keep their headroom, minus the decode stacks' extra 8 KB each (internal on this part).
- **Time.** AC-4 5.1 on the glass fails the screen plan's "decode moves by less than 5%" pass (10%); 5 of 18 plays at layout 5.1
  glitched, which a later session traced to the stream's first chunk arriving 300 to 600 ms late, not to the glass. AC-4 5.1.4
  is not real time here either and the sink states 6 channels, so the server decodes it.

**The kit ran it (2026-10-29, leased, put back as found), and the verdict above has to change.** The folded profile was built
(3,768,832 bytes, held to its profile and to a 3,900,000 budget by `verify`), pushed to the kit by OTA and run through the
board suite with the `measure` overlay (`--record` refused the run, as it must). What the board said:

| On the kit, `p4-rev3` + AC-4 | |
|---|---|
| Free internal heap at boot (worst of 3) | 85,859, largest block 57,344 (the baseline of 2026-10-26: 104,523 and 69,632; this tree has 80 more commits and AC-4's 2 KB of `.bss`) |
| At an HTTP play's start | 78,439 free, largest 58,368 |
| After the 40,960-byte decode stack | 28,687 free, **largest 7,424**: `heap_caps_malloc could not allocate 24576 bytes (caps 0x804)`, the second lane's stack, and `player: no second lane for AC-4, one core` |
| AC-4 2.0 rows | pass, 0.45 to 0.51 of a frame, one failed allocation each (the lane) |
| AC-4 5.1 row (to 9.1.6) | **1.26 to 1.43 of real time**, 2 sink underruns: the 0.65 to 0.74 the kit measured on 2026-10-20 needed the second lane |
| AC-3 2.0, E-AC-3 2.0 and the 5.1 TDM row | `state=failed, frames=0`, one failed allocation each (not read to the allocation: the log ring held the later plays) |
| The internal heap's least in an AC-4 play | 8,423 to 8,491 bytes |

**What the log said (the same day, after the first run was read again).** The log ring was read by following `X-Log-Next` (a
`GET /log` answers 4,096 bytes at a time) and the failed allocations named themselves: at an AC-3 play's start the board had 79,687 bytes
of internal heap free, largest block 55,296; after the I2S sink's queue, 57,519 free and the largest block 37,888, and
`heap_caps_malloc could not allocate 40960 bytes (caps 0x804); player: could not start the decode task`. That is the AC-4 image's
decode stack (40,960 bytes, internal on this part) against a block 3 KB too small, and it failed every AC-3 and E-AC-3 play, not only
the AC-4 ones; the lane stack (24,576) met a 7,424-byte block in the AC-4 plays. Nothing was wrong with the decoder.

**A second finding, in the log itself.** With the AC-4 plays passing their allocations, the suite still could not read them ("no PCM
hash on the AC-4 line", "the verdict line says result=None"): the ring held `...decoder_us=89608 decoder` where the line goes on for
another 150 bytes, cut at exactly 128 characters, and the boot's `sink:` line cut the same way. ESP-IDF's USB-Serial-JTAG console
fails every write with `EIO` while no host is on the port (`usb_serial_jtag_vfs.c`, `usb_serial_jtag_write`), the kit's COM7 was
no longer there, and `log_write` returned the console's failure to stdio, which drops the rest of what it had buffered after a failed
write. The ring itself had been given every chunk it saw; stdio stopped giving it the rest. `log_write` now reports the write whole
(the console stays best effort). Any board of this firmware with no cable on its USB console had the fault.

**What was changed on this branch, and what the kit said after each:**

1. `player.cpp`: the HTTP play's decode task takes its stack in PSRAM when the largest internal block, read just before, is under the
   stack and a kilobyte; where it fits the stack stays internal, as the baselines were measured. `lane_executor.hpp` does the same for the
   decoder's second lane. AC-3, E-AC-3 and the TDM row then passed.
2. `log.cpp`: `log_write` returns the whole write. The AC-4 rows then read, and their PCM hashes equal the host's pins
   (`f860e51f602ae753`, `e8b8cfa716dd0e1c`, `5b93c61c57566b0c`, and `6d36d7d2e8a070e9` for the 5.1 row).
3. One AC-4 5.1 play in about ten still failed (`could not start the fetch task`, 25,499 free and a largest block of 6,656): the numbers fit
   the check in 1 having found a big enough block that time (70,247 free and a largest block of 51,200 at the play's start, 25,499 free
   after the sink and a 40,960-byte stack), leaving the stack internal and the fetch task's 8,192-byte stack no block. So
   `sdkconfig.p4-rev3` locks `DECODE_STACK_IN_PSRAM=1` (the S3's setting; the kit's internal RAM is what the screen, the TDM queue and the
   networks leave it), and 24 AC-4 5.1 plays in a row then all finished with no failed allocation. In PSRAM the stack costs the
   decoders nothing measurable on the kit: AC-3 2.0 56 permille of a frame (the baseline of 2026-10-26 had 55), E-AC-3 42 (42), the 5.1
   TDM row 143 (142).
4. The AC-4 internal-allocation limit (`below=`) was swept on the kit at 16,384, 8,192, 4,096 and 1,024 (6 plays each): the peak of
   internal use, 61.7 to 65.8 KB, and the least free, 7.7 to 8.9 KB, did not move, so the limit is not what the decoder takes from
   this part's internal RAM and it stays at 16,384.

| The kit, `p4-rev3` as folded (recorded 2026-10-29, five plays a row, three boots) | |
|---|---|
| Free internal heap at boot (worst of 3) | 86,967, largest block 56,320 (the baseline of 2026-10-26: 104,523 and 69,632) |
| AC-3 2.0, E-AC-3 2.0, AC-3 5.1 onto 9.1.6 (16 slots) | 56, 42 and 143 permille of a frame; no underrun |
| AC-4 2.0 A-SPX, A-CPL, companding | 261, 317 and 320 permille |
| AC-4 5.1 onto 9.1.6 | 676 permille (673 to 694; the kit's run of 2026-10-20 had 650 to 740) |
| Internal heap's least in an AC-4 5.1 play | 8.3 to 9.7 KB (24 plays); the decoder takes what is left by design |
| Image | 3,811,472 bytes: 382,832 under the slot, 138,528 under the budget of 3,950,000 |

**Not measured:** AC-4 through Sendspin on the kit (the suite's server would have to pair with the owner's kit, which the suite's
rows for it have never done: its Sendspin player takes its 40,960-byte decode stack at boot, internal, and its lane stack in PSRAM
already, and the P4a runs the same path in its baseline); the Meters view and AC-4 together; a soak. The slot is not "plenty" on
this board: 383 KB under it, about three features' worth, so a 6 MiB slot pair is the structural answer if the image keeps growing.

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

**Verdict: the one image is possible, and neither thing that stopped it is the decoder.** Flash is not a limit (1.57 MB of
slot left, 2.5 MB of the 8 MB PSRAM for code and constants). What stood in the way, as first read and as found:

1. **"A hang only the AC-4 profile has."** It was not. The suite's lock-ups are a Wi-Fi receive-buffer deadlock, reproduced on
   the board and read out by instrumentation (next section), that exists because the S3, alone of the four targets, has no bound on
   the TCP out-of-order queue. The AC-4 profile showed it in 7 of 8 suite runs and the plain release profile in none of 4,
   and the release profile with the diagnostic in the image showed it once, so the profile that carries the decoder reaches it
   more often; why is not established. One config line bounds the queue, and the configuration below ran five full suites
   (three with the diagnostic, two without) without a stall where the same configuration without that line had hung twice.
2. **E-AC-3 and AC-4 want opposite allocation limits.** True, and a configuration choice, not a hang: see
   [the policy](#the-allocation-policy-for-one-s3-image), which keeps the release profile's 4 KB limit, lets an AC-4 play
   take 512 as it does now, and costs 32 KB of heap to the 64 KB data cache.
3. **Real time is narrow.** With the 64 KB cache the S3 keeps up at 2.0 up to 25 frames a second and no wider
   ([decision 42](ac4.md#decisions-of-2026-10-10)); the recorded AC-4 plays glitched in 1 to 3 of 5, as they did before
   this work. Without the cache, 2.0 A-SPX is 0.61 of a frame on the HTTP path against 0.50 with it (the table in
   `sdkconfig.s3-dcache`), which is why the Kconfig default stops listing AC-4 on a 32 KB cache.
4. **The budget** moves from 1,750,000 to about 2,750,000 (the image is 2,621,328). Not a blocker.

**Done (2026-10-29).** `s3-n16r8` has the fragments `hw`, `psram`, `psram-4k`, `sendspin`, `ac4`, `s3-fast`, `s3-dcache`, the AC-4 profile's locked
values with the allocation limit at 4,096, a budget of 2,750,000 and the AC-4 profile's play list; `s3-n16r8-ac4` and CI's separate AC-4
build are gone, and the release build is the AC-4 build (2,621,600 bytes). S3a recorded the baseline (the suite's recording
run: eight boots, five plays a row, every row ok, the three AC-4 PCM hashes the host's); S3b, a different board, then ran it against
that baseline with every row ok and the figures within a few percent (boot 75,731 against 75,503; the least heap of the Sendspin
rows 63.6, 44.2, 47.5 and 74.1 KB against 64.3, 44.1, 47.4 and 74.1). E-AC-3 JOC 5.1.4, 7.1.4 and 9.1.6 streams
(`testdata/object-fixture/dee_joc_*.ec3`) played three times each on S3b over HTTP with no failed allocation and no underrun, 60 KB of
internal heap free at the end of the lap from 79.8 KB at the start. S3a then ran the suite against that baseline 11 more times with the first folded image (three boots and two plays a row each; about 1 h 36 min of suite with the recording run): no stall, no timeout, no failed allocation, and ten runs held to the baseline. One (run 9) flagged `ac4-20-music-sendspin`, 2 of 2 plays glitched against the baseline's 0 of 5 (one underrun, one resynchronisation, one late chunk), in a run that overlapped S3b's rollback to its as-found image on the same network. The image built from the merged branch (the stack fallbacks and the log fix, 2,621,840 bytes) then ran the suite three more times on S3a with nothing else running: every row ok, no regression, boot 75,551 to 75,731 bytes free.

## The S3 hang, read on the board (2026-10-28)

Board: the S3 at 192.168.1.156 (`hearth-eb2c64`, lock `s3b`, bench `S3a`), leased and booked for the evening and put back as found
at the end. Every image is an OTA of a build of this tree with the scratch diagnostic described next; none of it is in the
repository. USB JTAG could not be used: the two S3s' `USB JTAG/serial debug unit (Interface 2)` have WinUSB bound with an empty
`DeviceInterfaceGUIDs`, so OpenOCD's libusb cannot open them (the P4a and the C6 have
`{80869AD8-C37F-476A-A6B4-AE241C30A473}` and open). Installing the GUID is a driver change on this PC and was not done.

**The diagnostic.** A task, with its stack in PSRAM, printing once a second: internal heap (free, largest, least), DMA-capable
free, whether lwIP's thread answers a callback posted to it, whether Wi-Fi reports an association and its RSSI, and lwIP's link
receive counter. When the receive counter has not moved for 10 s it prints the task list, the Wi-Fi driver's own statistics
(`esp_wifi_statis_dump`), lwIP's statistics, the heap summary and, from lwIP's thread, every TCP and UDP PCB with its
out-of-order, unacked and mailbox state. The task's source, its configuration fragments, the soak script and the captures of
every stall are in `D:/ac3bld/s28b-evidence` on the owner's machine (not in the repository).

**What the stalls look like.** One suite run restarts the board eight times, plays five AC-3, five E-AC-3, five AC-4 2.0 SIMPLE
and five A-CPL streams over Sendspin, then the same over HTTP, and takes 13 to 17 minutes. A stall came 10 to 16 minutes in, at
the start of a stream, a second or so after the I2S sink opened (every stall, including finding 28's):

| In the stall | Reading |
|---|---|
| PC to the board | ARP unanswered (`Destination host unreachable` from the PC itself); HTTP and the suite's `GET /log` time out |
| The board's own view | its health task kept printing every second for 130 s and more; lwIP's thread answered every callback; Wi-Fi stayed associated and its RSSI kept changing |
| CPUs and tasks | all alive; every network task (`wifi`, `tcpip`, both `httpd`, `sendspin-play`) Blocked; no watchdog, no panic |
| Heap | **not exhausted**: 59 to 82 KB of internal heap free; the board's failed-allocation callback printed nothing |
| Wi-Fi driver buffer table | type 7, the dynamic receive buffers: 32 of 32 out, 3,982 allocation failures (`fail_fc`), 193,129 allocated and 193,097 freed; before a stall the table shows 0 to 4 out |
| lwIP | link receive counter frozen (4,899 for 131 s); `TCP drop` 1,685; no memory error |
| PCBs | one TCP connection, the Sendspin server's port, ESTABLISHED, **`ooseq` set**, `unacked` set, window 5,760; every datagram mailbox empty |

So the receive path is full: the Wi-Fi driver has no receive buffer to accept a frame with, and the buffers are held above it. The
same exhaustion happened earlier in the same runs and **recovered** (32 of 32 out at one reading, 0 at the next): during an E-AC-3
stream with 3.8 KB of internal heap free, for the whole stream, the receive counter moved once a second.

**The cause that fits.** ESP-IDF's own Kconfig help for `LWIP_TCP_OOSEQ_MAX_PBUFS` describes it: segments that arrive out of order
are queued, each holding its Wi-Fi receive buffer, and "it is possible that all RX buffers for MAC layer are used by OOSEQ", after
which the retransmission that would fill the gap has no buffer to arrive in. Its default is 0, no bound, when Wi-Fi's buffers go
to PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). The resolved sdkconfigs of the targets:

| Target | `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | `LWIP_TCP_OOSEQ_MAX_PBUFS` | Dynamic RX buffers |
|---|---|---|---|
| S3 | y | **0** (no bound) | 32 |
| P4 (v1.x and v3.x) | n | 4 | 32 |
| C6 | n | 4 | 32 |

**The runs.** All on this board on 2026-10-28, the full suite each (13 minutes without AC-4, 17 with).

| Image | Result |
|---|---|
| the control image of finding 28 (AC-4 profile, 16 KB limit, 64 KB cache), no diagnostic | one run clean; the next stalled 10 minutes after boot; in finding 28, 3 of 3 stalled |
| the same with heap poisoning and a diagnostic that took 17 KB of heap | stalled (E-AC-3 stream, 3.8 KB free) |
| the same with the lighter diagnostic and the buffer table | stalled at 16 minutes, at an AC-4 stream with 59 KB free: the table above, 131 s without recovery |
| the same with the PCB dump | stalled at 11 minutes: the PCB row above |
| the release profile (no AC-4), diagnostic in | stalled at 11 minutes (an AC-3 stream, 100 KB free, 32 of 32 out) |
| the plain release profile (no AC-4), no diagnostic | clean, twice (and twice in finding 28) |
| **the recommended configuration below, `LWIP_TCP_OOSEQ_MAX_PBUFS=4`, diagnostic in** | **clean** (three full runs, 51 minutes), receive buffers out at most 4; the same configuration without the line stalled twice (finding 28's `psram4k-rec` images) |
| **the same, no diagnostic, with the `measure` overlay** (the image the board suite records) | **clean** (two full runs; then, as the folded profile on S3a, 14 more (11 on the first folded image, 3 on the merged branch's) more, below): every row ok, the three AC-4 `POST /play` rows' PCM hashes equal the host's pins (`f860e51f602ae753`, `e8b8cfa716dd0e1c`, `5b93c61c57566b0c`) |

**Established:** the stall is a Wi-Fi receive-buffer exhaustion with lwIP, Wi-Fi and the CPUs alive and the heap not short; the
unbounded out-of-order queue is how this part is configured and how the P4 and C6 are not; bounding it removed the stall in the
full runs of the configuration that stalled before. **Not established:** which segment loss starts it (the board counted none);
why the profile with the decoder reaches it more often than the release profile, which is not immune; that these runs are enough
(at the rate seen, 7 in 8, two clean runs would be a 2% chance and three a third of a percent, but a rate is not a proof, and the
fix should be soaked for hours before it ships); that the diagnostic does not disturb (the control stalled without it).

## The allocation policy for one S3 image

Measured on the board, the whole suite. "Least" is the stream's least internal free heap (the baseline's `play_heap_free`); a
burst is the player's mean time to decode and render one. Row C is the image with no diagnostic; the diagnostic's own cost
is about 4 KB of heap and, in the A-CPL stream, glitched plays (4 or 5 of 5 with it, 2 of 5 without).

| | Cache | Base limit | AC-4 play | Boot heap | AC-3 least, burst | E-AC-3 least, burst | AC-4 2.0 least, burst (SIMPLE; A-CPL) |
|---|---|---|---|---|---|---|---|
| A. release as built | 32 KB | 4,096 | none | 110,123 | 102,400, 6.9 ms | 78,500, 5.8 ms | none |
| B. the AC-4 profile as built | 64 KB | 16,384 | 512 | 60,283 | 35,000, 6.4 ms | **5,350 to 6,600**, 5.1 ms | 62,900, 21.8 ms; 29.6 ms |
| **C. recommended: B with the base limit at 4,096, and the receive-buffer fix** | 64 KB | 4,096 | 512 | 75,643 (largest 34,816) | 62,200, 6.5 ms | **43,800**, 5.3 ms | 47,700 (the first, with the E-AC-3 decoder still held) and then 72,000, 22.0 ms; 74,700, 29.7 ms |
| D. 32 KB cache, 4,096, AC-4 at 512 | 32 KB | 4,096 | 512 | about 108,000 (estimate) | about 102,000 | about 78,000 | A-SPX 0.61 of a frame against 0.50: about 26 ms and 35 ms of a 42.7 ms frame (estimate) |
| E. B, with E-AC-3 streams also switching the limit to 4,096 (as AC-4 plays switch to 512) | 64 KB | 16,384 idle | 512 | 60,283 | 35,000 | about 40,000 (estimate) | as B |

What each costs:

- **C against B (today's AC-4 profile):** +15 KB of boot heap, +27 KB in an AC-3 stream, **+37 KB in an E-AC-3 stream (from 5 KB
  to 44 KB, the difference between an allocation that can fail under Wi-Fi and one that cannot)**, +10 KB in the AC-4 plays
  after the first; a burst 1.5% longer for AC-3, 4% for E-AC-3, none for AC-4. No code: the release profile's `psram-4k` stays.
- **C against A (the release profile):** -34 KB of boot heap (the cache, 32 KB, and 2 KB of static RAM), -35 to -38 KB in an E-AC-3
  stream; E-AC-3's burst is 9% shorter because of the cache. This is the price of AC-4 at 0.40 to 0.50 of a frame.
- **D** keeps the release profile's heap and slows AC-4 by a fifth: A-CPL at 2.0 would take about 0.8 of a frame, where the
  baseline already glitches 3 of 5 plays at 0.69. Not measured over Sendspin.
- **E** gives C's E-AC-3 headroom with less idle heap and 15 lines of player code. No gain over C.
- **An explicit allocator for the decoders' tables and scratch** (PSRAM by capability for AC-4, or `PsramScope` for the E-AC-3
  decoder) is the most precise and the biggest change; C gets the same result from one configuration key.

**Recommendation: C**, with the receive-buffer fix. In `profiles.json` that is `s3-n16r8` taking the `ac4` and `s3-dcache`
fragments after `s3-fast` and keeping `psram-4k`; the locked values that move are the cache (6 keys), the AC-4 keys (8, as for
`p4-rev1`, plus `SENDSPIN_AC4_MAX_FRAME_RATE` 25), the two decode stacks (40,960, in PSRAM on this part) and the budget; and
`s3-n16r8-ac4` goes. It was recommended before the board run that re-records the baselines; that run then measured what was owed:
E-AC-3 JOC 5.1.4, 7.1.4 and 9.1.6 streams with the 64 KB cache and the 4 KB limit leave 60 KB free at the end of the lap (the
estimate was about 38 KB), AC-4 over Sendspin ran on the second S3 as on the first, and the soak is the fifteen suite runs under the
S3 verdict above (hours of it are still owed).

**Done:** the one line, `CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=4` in `sdkconfig.psram` with its reason (#1697), and the fold of the S3
profile as recommended (the follow-up): `s3-n16r8` is policy C. The 4 KB limit's old comment in `sdkconfig.psram-4k` ("its board suite
hung at the first play") was the out-of-order queue, not the limit, and says so now.

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
- **Builds:** `_build.yml` built `s3-n16r8-ac4` and `p4-rev1-ac4` (not run); both steps are gone and the release builds are 1 MB
  larger. The PR tier does not run the S3 QEMU leg, so the first run of the S3 release shape with the decoder and the 64 KB cache
  under QEMU is the nightly's.
- **QEMU** runs only the S3 release shape (`s3-n16r8` with `qemu-net`); there is none for the P4 or the C6, and no AC-4 QEMU
  leg. An S3 release image with the decoder would run its boot gate with 2 KB less heap and the decoder idle.
- **The board suite** (`board_suite.py`) refuses a baseline recorded under another hash: every folded profile needs its run.

## What could not be determined

- AC-4 through Sendspin on the P4X kit, the Meters view and AC-4 together, and a soak on the kit.
- On the S3: a soak of more than the 15 suite runs above (the stall needs hours to rule out, not minutes); whether the
  diagnostic or the bound on the out-of-order queue is what made the A-CPL stream glitch more in the diagnostic runs (the folded
  image's A-CPL over Sendspin glitches 4 of 5 plays in the recording run, where the AC-4 profile's baseline had 2 and 3 of 5: the stream is
  0.69 of a frame on this part and always has been marginal); which segment loss starts the receive-buffer stall; why the profile
  with the decoder reached it more often than the release profile.
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
