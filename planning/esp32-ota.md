# Firmware over the network for Hearth sinks

**Status, 2026-09-30:** O1 to O5, O8 and O9 are built and merged, with the fixes that followed
them. O1 merged as #1019 and #1026 on 2026-09-25. #1034, #1035, #1038 and #1056 followed on
2026-09-25 and #1036, #1039 to #1042, #1048 and #1053 on 2026-09-26 (UTC). All four boards are
on the two-slot layout and passed O2's checks on 2026-09-25. O6, the P4's co-processor
firmware, is a study: nothing that updates the co-processor exists. O7, signed images, is not
built, by the user's decision. The exits of O8 and O9 wait for a release that carries sink
firmware, and none has been cut since v0.10.0-beta.1 on 2026-09-01 ([Phases](#phases)). O10, updates
pulled from GitHub releases with O7's signing brought into it, is designed (2026-10-25) and waits for the
owner's decisions ([Updates from GitHub releases](#updates-from-github-releases-o10)).

This plan was written on 2026-09-24, when every `hearth_sink` layout on `main` (`b49a966c`) was a
single `factory` app and nothing in the tree called `esp_ota_*`. The ESP-IDF facts below were read
from the v6.1 tree at `D:\esp\esp-idf`, which the board builds use. The board facts come from the
builds and flashes of 2026-09-24. [Decisions](#decisions) lists what is recommended and what each
choice costs. The user took decisions 2, 5 and 9 on 2026-09-24:

- Images are not signed while the boards are in development, so anyone on the network can flash
  a board, as anyone with a USB cable can. Every image is checked for damage from the build to the
  flash and at every boot ([Integrity](#integrity)).
- The board decides when a new image is accepted.
- The P4's co-processor firmware waits for a phase of its own.

On 2026-09-25 the user added two final phases. CI builds the firmware for every board and
publishes it, as it does the desktop packages ([Published images](#published-images), O8). A
guide then tells a user how to use those images ([The user guide](#the-user-guide), O9).

Before this plan a `hearth_sink` board was updated over its USB connector:

- a build on the PC;
- `esptool write-flash @flash_args` on the board's COM port;
- sometimes a BOOT or RESET press, or `--after watchdog-reset` on the board that needs it;
- and nothing at all while the cable or the port is being used for something else.

USB is still how a board moves to the two-slot layout, gets a new partition table or bootloader,
and is recovered when it will not boot. The plan added updates over the network the board is
already on, usually Wi-Fi, or Ethernet under QEMU. It also added a remote restart, and a way back
to the previous firmware. Five rules keep it safe:

- The new image goes into the slot that is not running.
- The board keeps running its current image until the new one has been checked in full: for
  damage, and for being an image for this board.
- A new image stays on trial until it has shown it can do the job, and any reset during the trial
  brings the previous image back.
- The bootloader checks an image before every boot, and boots the other slot if the image has
  been damaged.
- An update never writes the bootloader, the partition table or eFuses, so USB stays the way to
  recover a board. It is still needed when all else fails.

It covers every board the project runs on: the ESP32-S3, the ESP32-C6 and the ESP32-P4.

## What existed on 2026-09-24

This section and its table are the tree before O1. Since O1 (#1019) every board uses a two-slot
table: `partitions.csv` for the S3, the P4 and a C6 with 16 MB of flash, and `partitions_c6.csv`
for a C6 with 4 MB. `partitions_p4.csv` no longer exists. The C6 on COM9 has 16 MB of flash and
runs the 16 MB table. The control surface has the firmware routes and `GET /log` now, and
`GET /firmware` reports what a board runs.

| | ESP32-S3 | ESP32-C6 | ESP32-P4 |
|---|---|---|---|
| Boards | two DevKitC-1 N16R8: COM15 `hearth-eb2c64`, COM16 `hearth-47b39c` | one, QFN40 rev v0.2, COM9 | one DFRobot FireBeetle 2, rev v1.3, COM10 (down on 2026-09-24 for want of a data cable, back on 2026-09-25 through the console USB-C) |
| Flash on the board | 16 MB | 16 MB (the 2026-09-15 bring-up note; check with `esptool flash-id` before migrating) | 16 MB |
| Flash size the build assumes | 16 MB (`sdkconfig.defaults`) | 4 MB (`sdkconfig.c6`, for any C6 module) | 16 MB |
| Partition table | `partitions.csv`: `factory` 1.5 MiB | `partitions_c6.csv`: `factory` 2 MiB | `partitions_p4.csv`: `factory` 4 MiB |
| Sendspin image, 2026-09-24 | 1,419,104 bytes (90% of its partition) | 1,580,816 bytes (75%) | 1,463,536 bytes (35%) |
| Bootloader | 21,168 of 32,768 bytes (at `0x0`) | 23,152 of 32,768 bytes (at `0x0`) | **23,296 of 24,576 bytes** (at `0x2000`) |
| QEMU machine | yes; CI runs `hearth_sink` over the emulated Ethernet (`net/openeth/`) | no | no |
| Network | Wi-Fi | Wi-Fi, with Wi-Fi's code in flash (`sdkconfig.sendspin-c6`) | Wi-Fi through the onboard ESP32-C6 over SDIO (`esp_hosted`) |

All three tables share one shape: `nvs` at `0x9000` (24 KiB), `phy_init` at `0xF000`, the app at
`0x10000`, then `audio` and `storage` (256 KiB each, for the partition and FAT sources). NVS holds
what a board is:

- its name, network, slot width and wiring (namespace `hearth_sink`, `main/settings.cpp`);
- its Sendspin identity and pairing records (namespace `sendspin`,
  `firmware/esp-idf/iclforge/src/sendspin_store.cpp`).

A USB flash leaves NVS alone, because `flash_args` has no region there. The migration below keeps
it that way.

The control surface ([control.hpp](../firmware/esp-idf/iclforge/include/iclforge/control.hpp)) is
`esp_http_server` on port 80:

- three sockets, least-recently-used purge, a 6,144-byte task stack;
- no authentication. Whoever can reach the port can drive the board, as
  [the device UI plan](esp32-device-ui.md#security) records. The network is the boundary.

`GET /hardware` already reports the running firmware's project, version and ESP-IDF version.

## The shape

```
 PC                                   board (running ota_0)
 ──                                   ─────────────────────
 idf.py build
 tools/hearth/ota.py push ──────────► GET /hardware, GET /firmware    (pre-flight: chip, revision,
                                                                       layout, not on trial)
                      ──────────────► PUT /firmware  (the app image, with its SHA-256)
                                        enter flash mode: playback, Sendspin, sink stopped
                                        write ota_1; check the image and the SHA-256 of what
                                        was sent against what is now in flash
                                        boot ota_1 next, reply 200, restart
                                      bootloader: ota_1 is NEW → PENDING_VERIFY, boot it
                                      ota_1 on trial: network address + HTTP server
                                        + Sendspin player, 30 s without a break
 poll GET /firmware ────────────────► accepted (valid)          or     any reset / 5 min → ota_0
 report: updated / rolled back (why) / did not come back (what to try)
```

Six pieces:

1. **An A/B flash layout.** Two app slots (`ota_0` and `ota_1`) and `otadata`. Each board gets it
   once, over USB, with NVS kept.
2. **A bootloader with rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). A new image boots on
   trial, and a reset before it is accepted boots the previous one.
3. **Integrity checks at every step.** A SHA-256 goes from the build to the flash with each
   image, and the bootloader checks the image before every boot. No eFuses are burned. Signed
   images would come later ([Signing, later](#signing-later)); that is O7, and it is not built.
4. **Flash mode, and the routes around it.** Flash mode is the state the board is in while it
   takes an update. The routes are `GET /firmware`, `PUT /firmware`, `PUT /firmware/mode`,
   `PUT /firmware/rollback` and `POST /restart`.
5. **A host tool.** `tools/hearth/ota.py` pushes a build to one board or to all of them, waits for
   each one to accept the new image, and reports what happened. `idf.py ota` wraps it. The web
   page (O3) and `ac3hearth` (O5) came later and use the same routes.
6. **Published images and a guide.** CI builds each board's firmware and publishes it with the
   desktop packages, and a user guide covers installing and updating from those images (O8 and
   O9, the last phases).

All six are built except the signing in piece 3.

## Flash layout

**Status, 2026-09-30: built** in O1a (#1019). `partitions.csv` and `partitions_c6.csv` in the
example have the offsets and sizes below, `partitions_p4.csv` is gone, `sdkconfig.flash16mb`
exists, and `tools/checks/check_esp_efuse_free.py` runs in CI.

`nvs` and `phy_init` stay where they are. `otadata` goes where the app started, and the first
slot at `0x20000`.

**16 MB boards: every board on the desk.** `partitions.csv` becomes:

| Name | Type | SubType | Offset | Size | |
|---|---|---|---|---|---|
| `nvs` | data | nvs | `0x9000` | `0x6000` | unchanged |
| `phy_init` | data | phy | `0xF000` | `0x1000` | unchanged |
| `otadata` | data | ota | `0x10000` | `0x2000` | new |
| `ota_0` | app | ota_0 | `0x20000` | `0x400000` | 4 MiB |
| `ota_1` | app | ota_1 | `0x420000` | `0x400000` | 4 MiB |
| `coredump` | data | coredump | `0x820000` | `0x10000` | new; used from O4 |
| `audio` | data | `0x40` | `0x830000` | `0x40000` | moved, same size |
| `storage` | data | fat | `0x870000` | `0x40000` | moved, same size |
| `reserve` | data | `0x41` | `0x8B0000` | `0x400000` | new; empty ([decision 8](#decisions)) |

That ends at 12.7 MiB. A 4 MiB slot is 2.6 to 3 times the size of each chip's image on
2026-09-24. The P4 already had a 4 MiB app partition for this reason. The P4 and the C6 on COM9
then use this table, so `partitions_p4.csv` goes. A 16 MB C6 selects it with a new overlay, `sdkconfig.flash16mb`
(`CONFIG_ESPTOOLPY_FLASHSIZE_16MB` and this table), placed after `sdkconfig.c6` in the list
([decision 7](#decisions)).

**4 MB C6 modules.** `partitions_c6.csv` becomes:

| Name | Offset | Size | |
|---|---|---|---|
| `nvs`, `phy_init`, `otadata` | as above | | |
| `ota_0` | `0x20000` | `0x1C0000` | 1.75 MiB |
| `ota_1` | `0x1E0000` | `0x1C0000` | 1.75 MiB |
| `coredump` | `0x3A0000` | `0x10000` | |
| `audio` | `0x3B0000` | `0x10000` | 64 KiB, from 256; the sample is 10,752 bytes |
| `storage` | `0x3C0000` | `0x40000` | ends at `0x400000` |

The C6 image was 86% of a 1.75 MiB slot on 2026-09-24, and 91% on 2026-09-25 with O4's core dump
code in it ([Diagnostics](#diagnostics-without-a-cable-o4)). When it outgrows that, 4 MB C6
modules need something else: a smaller image, or no `audio`/`storage` partitions in the C6's
Sendspin build. The limit is written here so that it does not arrive as a surprise.

**Taken 2026-10-21: no `audio` and no `storage` in `partitions_c6.csv`.** The image reached 99.9% of
the slot on 2026-10-20 and the next three changes to code every board carries put it 38,784 bytes
over. The slots are now `0x1E0000` (1.875 MiB) at `0x20000` and `0x200000`, `coredump` follows at
`0x3E0000`, and the 64 KiB after it is unused (an application partition starts on a 64 KiB
boundary, so the slots cannot take it). The project's CMake flashes `audio` and `storage` only on a
table that has them. A board moves to this table over USB, as it did to the one before.

**Moving a board to the new layout** is one USB flash of the usual `write-flash @flash_args`. The
build's `flash_args` then writes:

- the bootloader, now with rollback;
- the new partition table;
- `ota_data_initial.bin` (erased, so the bootloader boots `ota_0`);
- the app into `ota_0`;
- `audio` and `storage` at their new offsets.

It still writes nothing at `0x9000`, so the board keeps its name, its network and its pairings.
The app finds `audio` and `storage` by label, not by offset. The first image written this way has
to be able to take the next update over the network, so no board migrates until O1 is merged in
full ([Phases](#phases)).

**A network built into the image.** A board joins the network stored in its NVS, or failing
that, the one compiled into its image from `CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID`
(`net/wifi/network.cpp`). Nothing copies the built-in one into NVS. The board builds on the desk
compile one in from a local fragment, so a board may never have had its network stored. A
published image has no network built in (O8). A board whose network comes only from its image
would boot a published image with no network, fail its trial and roll back. That is safe, but it
would be confusing. So from O1 an image stores its built-in network in NVS the first time it
boots and finds none stored. `GET /firmware` reports where the network came from, and `ota.py`
refuses to push an image with no network to a board whose only network is built in. Built in O1b
(#1026): the console prints "stored the network this image was built with", and `network` in
`GET /firmware` is `stored`, `built-in`, `wired` or `none`.

**The P4's bootloader.** It sits at `0x2000`, below the partition table at `0x8000`: a
24,576-byte window. The "ESP32P4 firmware flash" session measured it on 2026-09-24 with
bootloader-only builds from `b49a966c` and the board's overlays:

| P4 bootloader | Bytes | Spare |
|---|---|---|
| As flashed on 2026-09-24: log level Info, no rollback | 23,296 | 1,280 |
| Rollback, Info | 23,424 | 1,152 |
| Rollback, Warning | 20,896 | 3,680 |
| Rollback, Error | 20,608 | 3,968 |
| Rollback, Warning, signed apps (O7) | 20,992 | 3,584 |

Rollback costs 128 bytes, so it fits and the partition table stays where it is. O1 keeps the Info
level, because O2's board tests read the bootloader's lines about which slot it chose and why.
A board's bootloader changes only with a USB flash, so this margin matters only at build time. If
a later ESP-IDF grows the bootloader past the window, the build fails and says so, and
`CONFIG_BOOTLOADER_LOG_LEVEL_WARN` is the one-line fix. The S3 and C6 bootloaders start at `0x0`
in a 32,768-byte window, with 11,600 and 9,616 bytes spare. As built, `sdkconfig.p4` records the
rollback figure (23,424 bytes at Info, 1,152 spare, 20,896 at Warning), and CI builds the P4's
`hearth_sink` in the `build-esp32c3` job of `_build.yml`, so that a bootloader grown past the
window fails the build. The table's other figures are the 2026-09-24 measurement's, from
bootloader-only builds, and were not taken again.

Anything else that might one day need a partition has to be in this table before the boards
migrate, because a table change is a USB flash. That is why `coredump` and `reserve` are there
already.

## Flash mode

**Status, 2026-09-30: built** in O1b (#1026), with the changes of #1053 (the upload's task made
after the teardown, a 15 s limit on the example's hook).

The user's direction: flashing is a mode of its own, in which nothing else runs. The board enters
flash mode on `PUT /firmware/mode` with body `flash`, or on the first `PUT /firmware`. Entering
it does this, in order:

1. The command goes through the queue app_main already reads, since app_main's task owns the
   player (`main/hearth_sink.cpp`). That task ends any play started with `POST /play`.
2. The Sendspin player sends `client/goodbye` with reason `restart` to every server connected (as
   `sendspin_host.cpp` already does when the board's configuration changes). It then stops, which
   frees its ring, its decode task and its WebSocket buffers. Clock sync stops with it.
3. The sink closes, and its I2S channels stop. Nothing goes to the DACs until the restart.
4. mDNS withdraws `_sendspin._tcp`, so servers stop dialling, and keeps the host name, so tools
   can still find the board.
5. `/status` reports `"state": "flash"`. That is a new value of an existing key, so the page's
   key-order contract is unaffected.

Three things keep running: the network (the Wi-Fi station, the SDIO link to the P4's
co-processor, or Ethernet), the HTTP server and mDNS's name.

Routes that would start playback or change a setting answer `409`, with a reply that says the
board is in flash mode:

- `POST /play`, `POST /volume` and `POST /pairing`;
- `PUT /layout`, `/slot-width`, `/wiring`, `/name` and `/network`.

Every `GET` still answers. (The list is `refused_in_flash_mode` in `control.cpp`. `POST /stop` is
not in it.)

**Every way out is a restart.**

- An image written and checked restarts the board into that image.
- `PUT /firmware/mode` with body `normal` restarts it into the running image.
- So do ten minutes in flash mode with no upload in progress.
- A refused image leaves the board in flash mode, so a corrected one can be sent. The ten minutes
  start again from the refusal.

A restart is the only way out because it rebuilds everything in the one order boot already uses
and tests. A resume would have to bring the player, the Sendspin host and the sink back in an
order nothing else runs. The cost is the seconds a board takes to rejoin its network after an
update that was cancelled or refused.

**Memory.** The upload needs a task whose stack is in internal RAM (8 KiB to start with, measured
in O1). The flash cache is off while the task erases and writes, and PSRAM is reached through that
cache, so a stack there would be out of reach. It also needs a 4 KiB receive buffer. The teardown frees far more than that on every chip: the C6's Sendspin ring alone
is 48 KiB and its decode stack 24 KiB. So flash mode changes no memory setting. The C6's internal
low-water mark during an upload confirms it: 114,308 bytes free at the lowest, measured once O4
printed it ([Diagnostics](#diagnostics-without-a-cable-o4)).

The S3 board is tighter. Playing JOC over Sendspin has left it 723 bytes of internal RAM (the
example's README), so the upload's task could not have been made before the teardown. Flash mode
therefore comes first. A board that still cannot make the task answers `503` with the largest
block it has, and records it. On the S3 board the largest internal block is 31,744 bytes as flash
mode starts, and 89,415 bytes is the least free during an upload (the table under
[Diagnostics](#diagnostics-without-a-cable-o4)), so the task fits.

**Flash writes and the cache.** An erase or a write disables the flash cache, in windows of up to
one 64 KiB block erase. Nothing time-critical is left running by then. On the C6, Wi-Fi's own code
runs from flash (its IRAM options are off), so Wi-Fi also pauses in those windows. TCP resends
whatever those windows delay.

## An update, on the board

**Status, 2026-09-30: built** in O1b (#1026), with the interrupted-upload record, erase-as-you-go
and the ten-minute limit from #1053. The steps below are what `firmware.cpp` does.

1. `PUT /firmware` arrives on the HTTP server's task. It is refused at once, before any of the
   body is read, when:
   - an update is already running;
   - the running image is on trial;
   - there is no second slot (a board still on the old layout);
   - there is no `Content-Length`, or the length is more than the slot holds;
   - the `Content-Type` is not `application/octet-stream`.
2. The server's task enters flash mode, as `PUT /firmware/mode flash` does, and waits for
   app_main to confirm the teardown.
3. The request goes to a firmware task (`httpd_req_async_handler_begin`, in ESP-IDF v6.1). The
   server stays free to answer `GET /firmware` while the upload runs. The task is made before
   the request is handed over, so a board that cannot make it answers `503` and records why,
   where a failure after the handover could only drop the connection ([Memory](#flash-mode)).
4. The task reads the first 288 bytes: the image header, the first segment's header and
   `esp_app_desc_t`. Nothing is erased until they pass these checks:
   - the magic numbers;
   - the chip ID is this chip's;
   - this chip's revision is within the image's minimum and maximum;
   - the project name is the running image's (`ac3forge_hearth_sink`);
   - the flash size in the header is the running image's.

   ESP-IDF checks the chip ID and revision again at the end
   (`bootloader_common_check_chip_validity`, called from `esp_image_verify`). Checking here
   refuses a wrong image before 1.5 MB are written, and the reply says which check failed. That
   matters on the P4: an image built without `sdkconfig.p4`'s revision settings needs v3.1 or
   newer, and this board is v1.3.
5. An `uploading` marker goes into NVS, naming the version. Whatever records the upload's outcome
   clears it in the same commit, so one found at boot is an upload a reset cut short: it becomes
   `last_update` `interrupted`, with the reset's cause.
6. `esp_ota_begin` erases the slot's first 64 KiB. Then 4 KiB reads go into `esp_ota_write`, each
   later 64 KiB block erased just before the writes reach it, and each read also goes into a
   running SHA-256 of the body. `GET /firmware` reports the progress. A stalled connection gives
   up after 30 s, and any upload after ten minutes.

   Erasing the whole image's worth first, as O1 did, left the upload reading nothing for the
   seconds that took: the client stalled, and on the P4 the link to the radio ran only in the
   gaps between block erases. Erasing as the writes go spreads the same work over the upload.
7. Three checks follow, the first and third reading the image back from flash
   ([Integrity](#integrity)):
   - `esp_ota_end` runs `esp_image_verify`: the header's checksum, the SHA-256 the build
     appended to the image, the chip ID, the revision range and the segment layout;
   - the SHA-256 of the body has to equal the request's `Content-Digest`, when there is one (the
     tool always sends one);
   - the SHA-256 of the bytes read back from the slot has to equal the SHA-256 of the body.

   Only then does `esp_ota_set_boot_partition` make the new slot the next boot.
8. The board replies `200` with the version written, waits 500 ms for the reply to leave, and
   calls `esp_restart()`.

Any failure calls `esp_ota_abort`, replies with the reason, records it for `GET /firmware`, and
leaves the board in flash mode. The slot is left with a partial image that nothing boots, because
`otadata` was never changed. The image that was in that slot has gone too, so after a failed
upload the running image carries on with nothing to roll back to. `PUT /firmware/rollback`
answers `409` until an update succeeds.

## The trial

**Status, 2026-09-30: built** in O1b (#1026). The timer that reads it came in #1034 and the
acceptance from `esp_timer`'s task in #1053.

The rollback bootloader marks a newly written slot `PENDING_VERIFY` the first time it boots it. If
the board resets while the slot is still in that state, the bootloader marks it `ABORTED` and
boots the other slot.

On the new image:

- It runs as normal: servers can connect and play. `GET /firmware` reports `"trial"` and the time
  left.
- **It is accepted** (`esp_ota_mark_app_valid_cancel_rollback`) once three things have held for
  30 s without a break:
  - the board holds a network address;
  - the HTTP server is running;
  - on a Sendspin build, the Sendspin player has started.
- **It is rolled back** (`esp_ota_mark_app_invalid_rollback_and_reboot`) if it is not accepted
  within 5 minutes. Before that it stores which condition never held.
- A panic, a watchdog reset, a brownout or a power cut before acceptance also rolls back, through
  the bootloader. A board that hangs during its trial can therefore be unplugged and plugged back
  in, and it comes back on the previous image.
- The trial is read once a second by an `esp_timer`, not in app_main's loop, so a stuck loop
  still rolls back. It has no task of its own. A task kept for the whole trial took 6 KiB of
  internal RAM as the board started, and on the S3 board that left the Sendspin player without
  the 32 KiB block it starts with, so no update could pass its trial there (found by O2).
  - Accepting writes otadata and NVS from `esp_timer`'s own task. That left 2,192 of the task's
    3,584 bytes of stack unused on the S3 board, 2,680 on the C6 and 2,660 on the P4. A task
    made for it could fail on a board
    whose internal RAM a stream had taken by then, such as one a server resumed as the board came
    back, and the guard would then roll back a good image.
  - Giving up makes a short-lived task, since it tells servers the board is going. If it cannot
    make one, it restarts, which rolls back without the reason.
  - A second `esp_timer`, 30 s past the deadline, restarts the board if the trial has not acted,
    and a restart while on trial is itself a rollback.
  - A rollback asked for while the acceptance is being written is refused with a `409`, rather
    than racing it: the board would go back, and its record would say the new image was
    accepted.
- The task watchdog is left as the builds set it: it reports and does not panic
  (`CONFIG_ESP_TASK_WDT_PANIC` is off in every board build). A decode that keeps the idle task
  from running for 5 s makes it fire. That is a problem of load, not a broken image, and a trial
  that panicked on it would roll back a good image because of what a server happened to play.
- While the image is on trial:
  - `PUT /firmware` is refused. ESP-IDF refuses too: `esp_ota_begin` returns
    `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`, because the other slot holds the image to fall back to.
  - `POST /restart` is refused, since a restart now is a rollback, and the reply says so.
  - `PUT /firmware/rollback` rolls back straight away.

The hold time and the deadline are Kconfig values (`AC3FORGE_FIRMWARE_TRIAL_HOLD_S` and
`_DEADLINE_S`), which the QEMU tests shorten.

**After a rollback**, the previous image reports it in `GET /firmware`'s `last_update`: the
version, `"rolled back"`, and why. The reason is what the failed image stored before it gave up,
or else the reset reason the previous image reads on its first boot back (`esp_reset_reason()`:
panic, task watchdog, brownout, power-on).

**What the trial cannot catch:**

- A fault that appears only after acceptance, such as a crash 20 minutes into a play. The
  previous image stays in the other slot until the next update, so `PUT /firmware/rollback` (or
  `ota.py rollback`) brings it back, as long as the network and the HTTP server still work. A
  later option is a crash-loop guard, which would roll back by itself after repeated panics
  shortly after boot. It is not built: it must never swap back and forth between two images that
  both fail.
- An image that runs well but cannot take the next update. Roll back to the previous image, which
  can; failing that, USB.
- Settings that a newer image writes in a form the older one cannot read. See
  [Settings survive a rollback](#settings-survive-a-rollback).

## Settings survive a rollback

Both images share one NVS partition. The Sendspin store keeps its pairing records as one blob and
reads it only if its length is exactly what it expects (`sendspin_store.cpp`). If a newer image
changed that blob's layout, a rollback would silently lose every pairing.

The rule from O1 on: a new image never changes the meaning or the layout of a key it did not add.
A new layout goes under a new key, and the old key stays readable. O1 adds a host test that loads
blobs written by the previous layout of each store; for the pairing records it is in
`libs/device/tests/test_pairing_records.cpp`.

## Integrity

The user's requirement: anyone on the network may flash a board, as anyone with a USB cable can,
but a damaged image must never run, whether it was damaged on disk, on the way to the board, on
the way into flash or while it sat in flash.

**Status, 2026-09-30: built** in O1b (#1026), with the background check of the other slot reworked
in the same PR's later commits (it runs from `esp_timer` in slices, after the trial, and stops
before an update writes).

Every ESP-IDF app image carries a SHA-256 of itself, which the build appends: `hash_appended` is 1
in the S3, C6 and P4 images of 2026-09-24. The checks build on that:

| Where the damage happens | What catches it | Where |
|---|---|---|
| On disk: a truncated or changed build output | `ota.py` checks the file's own appended SHA-256 before it sends anything | the tool |
| On the network | The image's own SHA-256 (next row). Also, the tool sends a SHA-256 of the whole file (`Content-Digest: sha-256=:…:`, RFC 9530) and the board compares it with its hash of the body as it arrived | `PUT /firmware`, before the new slot can boot |
| On the way into flash | `esp_image_verify`, which `esp_ota_end` runs, reads the image back from flash and checks its checksum, its appended SHA-256, the chip ID, the revision range and the segment layout. The board then hashes the bytes read back from the slot, and they must equal the SHA-256 of the body | `PUT /firmware` |
| In flash, later | The bootloader checks the image's SHA-256 before every boot. If the slot it was going to boot fails, it tries the other slot (`bootloader_utility_load_boot_image`) | every boot |
| The fallback image, before it is needed | Once no trial is left to decide, the board reads both slots through in the background, a little at a time from its timer task, recomputes each image's SHA-256 and compares it with the one appended, as the bootloader does, and `GET /firmware` reports whether each is intact. It stops before an update writes anything. `esp_partition_get_sha256` would not do: it returns the appended digest without checking it | `GET /firmware`, `ota.py status` |

Two build settings would switch the boot-time check off: `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON`
and `_ALWAYS`. Both default to off. A new check, `tools/checks/check_esp_efuse_free.py`, fails CI
if any `sdkconfig` fragment under `firmware/esp-idf/` or `firmware/baremetal/` turns either on (the
script-lint job's step "ESP-IDF settings that would stop USB recovery"). The same check refuses
the options that burn eFuses: hardware secure boot, flash encryption, anti-rollback, and a
disabled or secure ROM download mode. Each of those would take away some way of recovering a
board over USB.

`GET /firmware` reports each slot's SHA-256 as the board has it. After an update, `ota.py`
compares the running slot's digest with the file it sent. That proves the board runs exactly that
file, which a version string cannot do when two builds of one commit share it.

A `curl -T` upload carries no `Content-Digest`. The image's own SHA-256, checked from flash,
still catches any damage to the image itself. It does not cover bytes after the image's end, such
as padding or, later, a signature block. The file's digest does, which is why the tool always
sends it.

**What this does not stop.** Anyone who can reach the board can install any image built for its
chip, as they could with a USB cable. They can also restart it, or put it in flash mode (which
ends in a restart), as they can stop it today.

### Signing, later

**Status, 2026-09-30: not built** (O7). No sdkconfig fragment sets
`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, the tree holds no signing key and no key-path guard,
and the CI job has no throwaway key. The user put signing off until the boards leave development.
What follows is the design, and its ESP-IDF facts were read from the v6.1 tree on 2026-09-24.

**2026-10-25:** [O10](#updates-from-github-releases-o10) takes this design up together with a pull from
GitHub. It changes four things: the release key sits in a CI environment that stops at the owner's
approval; a development key lets the owner's boards take local builds; `secure_version` gives a downgrade
floor; and the `Host` check on the firmware `PUT`s stays.

When the boards move out of development, signed images are a phase of their own (O7). Everything
it needs is in ESP-IDF v6.1:

- `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` with the RSA-3072 scheme, which all three chips
  support (`SOC_SECURE_BOOT_V2_RSA`). No eFuses are burned.
- With hardware secure boot off, `esp_ota_end` takes its trusted keys from the signature blocks
  of the running image (`secure_boot_signatures_app.c`, "Take trusted digest key(s) from running
  app").
- The bootloader checks no signatures in this mode: `SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT` is
  only for the ESP32's V1 scheme.

**It can be switched on over the network, with no USB flash:**

1. A board running an image built without signing checks only integrity, so it accepts the first
   signed image as it would any other.
2. From then on, the running image's key is the one every update must be signed with.
3. An image can carry three signature blocks, so keys can be changed over the network too: sign
   the changeover image with both keys.

O7 proves the first step under QEMU before any board takes it.

**What it costs:**

- a private key kept outside every worktree, with an offline copy;
- one USB flash per board to replace the key if it is lost;
- a key-path guard in the example's CMakeLists;
- a throwaway key in CI's QEMU job.

It also stops what the table above allows, because a board would then refuse any image not
signed with its key. That includes one sent by a hostile web page ([Routes](#routes)).

## Routes

**Status, 2026-09-30: built.** `control.cpp` registers each route below, and
`libs/device/tests/test_firmware_status.cpp` pins `GET /firmware`'s body.

| Route | What it does | Replies |
|---|---|---|
| `GET /firmware` | Mode; each slot's version, ELF SHA-256, image SHA-256, state, and whether its image is intact; the trial's progress; the last update and how it ended (`on trial`, `accepted`, `rolled back`, `rollback requested`, `refused`, `failed` or `interrupted`); an upload's progress; the last crash's core dump, when one is kept (O4); slot size, flash size and the partition table as the board has it; the bootloader's version; whether the board's network is stored or built into its image; why the board last started (`reset_reason`) and how long ago (`uptime_ms`) | `200`, JSON |
| `PUT /firmware` | Body: an app image (`ac3forge_hearth_sink.bin`, not the merged image), with an optional `Content-Digest`. Enters flash mode, writes the other slot, checks it, restarts into it | `200` then a restart; `400` not an app image, the wrong chip, a revision this chip does not meet, cut short, more than ten minutes arriving, or damaged (a SHA-256 does not match, and the reply says which); `503` no internal RAM left for the upload's task (the reply says how much there is); `403` the `Host` is not one of the board's own names ([decision 13](#decisions)); `409` on trial, or an update already running; `411` no length; `413` larger than the slot; `415` a `Content-Type` other than `application/octet-stream` (none at all is fine: `curl -T` sends none) |
| `PUT /firmware/mode` | Body: `flash` enters flash mode; `normal` leaves it with a restart into the running image, and outside flash mode does nothing | `200`; `409` on trial |
| `PUT /firmware/rollback` | Makes the other slot's image, if it is valid, the next to boot, and restarts into it, on trial as an update's image is. On trial, gives up the trial instead | `200`; `409` nothing valid to roll back to |
| `POST /restart` | Restarts into the running image | `200`; `409` on trial, where a restart would roll back |
| `GET /firmware/coredump` | The core dump the last crash left, as it lies in flash (O4) | `200`, `application/octet-stream`; `404` none, or a build that keeps none |
| `DELETE /firmware/coredump` | Erases it | `200`; `403` the `Host` is not the board's; `409` an update is under way |
| `GET /log` | The console's recent output, oldest first; `?from=N` for what was written since byte N, with `X-Log-From` and `X-Log-Next` saying where the text starts and where to ask next (O4) | `200`, text; `404` a build that keeps no log |

`curl -T build/ac3forge_hearth_sink.bin http://hearth-eb2c64.local/firmware` is a whole update,
since `curl -T` sends a PUT.

Three routes are PUT for the reason [the device UI plan](esp32-device-ui.md#security) gives: a
page on another site cannot send a PUT without a preflight, and the board answers none (Control
sends no CORS headers). A POST with a text body needs no preflight. So `PUT /firmware/rollback`
reads oddly as a verb, but it cannot be sent cross-site. `POST /restart` is a POST like
`POST /stop`: sent cross-site it restarts the board, and nothing persistent changes.

A page on the internet can still get past that by DNS rebinding: it points its own host name at
the board's address, so its PUTs count as same-origin. While images are unsigned, nothing else
would stop such a page installing an image, so the three firmware PUTs check the `Host` header
([decision 13](#decisions)). They accept:

- an IP address;
- the board's own mDNS name, bare or with `.local`.

A rebinding page's requests carry its own host name and are refused with `403`. Browsers
increasingly block these requests themselves (Chrome asks before a public site reaches the local
network), and the check covers the rest. The cost: the page's **Update firmware…** works only
when the page was opened by IP address or `.local` name, not by a name a router hands out. Every
other route is unchanged.

These routes are for the board's own network. A board must never be reachable from the internet;
to update from outside the house, use a VPN into the network.

## The host tool

**Status, 2026-09-30: built** in O1d (#1026), with `coredump` and `log` from O4 (#1036) and
`--release` and `--run` from O8 (#1040). `python tools/hearth/ota.py --help` prints the usage
below; its exit status is 0 when every board was updated or already ran the image, 1 for a
refusal, a failure or a usage error, 2 when a board rolled back and 3 when a board did not come
back. `tools/hearth/test_ota.py` tests it against a stand-in board, and CI runs that suite.

`tools/hearth/ota.py`, Python 3 standard library only. `zeroconf` is optional and used only by
`--all`.

```
ota.py push   (--host H [--host H ...] | --all) [--yes] [--force] [--timeout SECONDS]
              [--download-dir DIR] (--build-dir DIR | --release TAG | --run RUN_ID | IMAGE)
ota.py status [--host H ... | --all]
ota.py restart  --host H
ota.py rollback --host H
ota.py cancel   --host H          # leave flash mode: restart into the running image
ota.py coredump --host H [--out FILE] [--elf ELF] [--erase]
ota.py log      --host H [--follow]
```

**`push`**, for each board in turn:

1. **Read the image.** From a build directory, `project_description.json` gives the app binary,
   the target and the version, and `partition_table/partition-table.bin` the layout. From the
   image itself: the chip ID, the revision range, the flash size, and `esp_app_desc_t` (project,
   version, ELF SHA-256). The tool checks the image's own appended SHA-256, which refuses a
   damaged file before anything is sent, and takes a SHA-256 of the whole file.
2. **Pre-flight**, from `GET /hardware`, `GET /firmware` and `GET /status` (whether the board is
   playing). It refuses when:
   - the target or chip differs;
   - the chip's revision is outside the image's range;
   - the image is another project's;
   - the layout differs from the build's (an update cannot change it: "this needs one USB
     flash");
   - the image does not fit the slot;
   - the flash size differs;
   - the running image is on trial, or an update is already under way;
   - the board has one app slot, so it is still on the old layout;
   - the board's only network is built into its image, and the new image has none
     ([Flash layout](#flash-layout));
   - the board already runs this image (skipped unless `--force`).

   A board that is playing is asked about first on a terminal, and refused without one; `--yes`
   answers for it. `GET /hardware` and `GET /firmware` are each asked up to three times, so one
   slow name lookup does not refuse the push.
3. **Upload.** `PUT /firmware` with the file's `Content-Digest`, and a progress line.
   - **An upload that breaks off** is followed by the board's own account of it, once it
     answers with no upload running:
     - from flash mode, "gave it up", with its reason ("the upload stopped after N of M bytes");
     - from normal mode, "restarted during it", with its `interrupted` record, or with its reset
       reason when a firmware keeps no record, going by an uptime shorter than the upload.

     The image is then sent once more when that can help: the board still runs what it ran, and
     nothing was accepted.
   - **A push that ends refused or broken** leaves no board waiting in flash mode. The tool tells it
     to leave, rather than leave it silent, unadvertised, until its ten-minute timeout.
4. **Wait.** Poll `GET /firmware` at the address the board had, for up to 6 minutes, until:
   - the new image is running and accepted, and the running slot's SHA-256 equals the file's:
     **updated**;
   - the old image is running, with `last_update` saying why: **rolled back**;
   - nothing answers: **did not come back**. The tool then says what to try. A board that hangs
     during its trial comes back on the previous image if its power is cycled. After that, USB:
     the same `write-flash @flash_args` as today.

`--all` finds boards by their `_sendspin._tcp` records. It updates one board at a time and stops
at the first board that rolls back or does not come back, before touching the others
([decision 11](#decisions)). A build without the Sendspin player advertises no service, so its
boards have to be named with `--host`.

**`idf.py ota`.** An `idf_ext.py` in the example adds an `ota` action (ESP-IDF v6.1 loads a
project's `idf_ext.py`). `idf.py -C <example> -B <build dir> ... build ota --host hearth-eb2c64.local`
builds and then pushes, as `build flash` does with a port. It takes `--host` (required, repeatable),
`--yes` and `--force`, and no `--all`. Built in O1d (#1026); it did not load until #1038 added the
`"version"` key that ESP-IDF v6.1 asks of an extension.

## The web page (O3)

**Status, 2026-09-30: built and merged** as #1035.

The page's firmware tiles already show `/hardware`'s version. A Firmware section adds:

- the running slot, the other slot's version and state, and the trial's countdown;
- how the last update ended;
- **Update firmware…**, a file input whose upload shows the bytes sent;
- **Restart**, and **Roll back to** the other slot's version, each behind a dialog like the one
  for forgetting servers.

After an upload the page polls `GET /hardware` until the board answers with the new version, then
reloads. Pages are already served `Cache-Control: no-cache`, so the page that reloads is the new
image's. The device-UI suite gains the routes in `stub.js` and in `contract.spec.js`'s route
check. The page budget (45,056 bytes, 42,846 used) is derived again, as each redesign did.

**Built 2026-09-25**, as [the device page's plan](esp32-device-ui.md#firmware) describes, with
three changes to the sketch:

- The page reads `GET /firmware` rather than `GET /hardware` for what the board runs. When the
  board answers again running another image than the page was loaded with, the page loads
  again, whoever made the update.
- It reads the image's head before sending, and keeps back a file the board would refuse on it
  alone. An upload enters flash mode before the board reads a byte, so a wrong file would stop
  what plays for nothing.
- It sends no `Content-Digest`. A page on plain HTTP has no `crypto.subtle`, and the board
  checks the image's own SHA-256 and reads back what it wrote. The device page's decision 29
  has the reasoning.

The budget is 57,344 bytes, against 55,454 used.

## Diagnostics without a cable (O4)

**Status, 2026-09-30: built and merged** as #1036.

A board updated over its network is usually a board with no cable on it. When one panics, or
does something odd, its console is where the cause is written, and nobody is reading it. O4 keeps
two things the console would have shown, where the network can reach them.

**The last crash.** A panic writes a core dump to the `coredump` partition, which O1 put in both
tables for this ([decision 8](#decisions)). The board keeps it through the restart that follows,
and through a rollback: the image that goes back reads the dump the failed image wrote.

- `GET /firmware` says whether there is one: its size, the task that crashed and where, and
  which image wrote it, by the ELF SHA-256 the dump carries. That image is usually one of the
  two slots.
- `GET /firmware/coredump` sends the dump as it lies in the partition.
- `DELETE /firmware/coredump` erases it, so the next crash is not mistaken for this one.
- `ota.py coredump --host H` saves it to a file. With `--elf`, the ELF of the image that wrote
  it, it runs ESP-IDF's `esp_coredump info_corefile` on it: every task's backtrace, which
  `idf.py coredump-info` would show at the desk.

**Recent console lines.** A ring of the console's last few kilobytes, in RAM:

- `GET /log` sends it as text, oldest first. `GET /log?from=N` sends only what was written since
  byte N, and each reply says where the next read starts, in `X-Log-Next`. So a client can
  follow the console the way a terminal would.
- `ota.py log --host H` prints it, and `--follow` keeps printing what is new.

**How the console is kept.** ESP-IDF has no public way to add an output to its console, and
picolibc, its C library in v6.1, has one `stdout` for every task. So the component reopens
`stdout` and `stderr` on a device of its own, `/dev/ac3log`, whose writes go on to
`/dev/console` as before and into the ring as well ([decision 17](#decisions)). That covers a
`printf` from any task, and the `ESP_LOGx` that reach `stdout`. It does not cover:

- what was printed before `log_start`, first thing in `app_main`;
- what `esp_rom_printf` writes, which includes a panic's registers and backtrace. The core dump is
  the record of a crash.

**What the network does not get.** The console prints the Sendspin pairing token, which pairs a
server with no code. It is printed for whoever holds the board, and the page and `/status` never
carry it. `GET /log` is for anyone on the network, so the token's line is printed inside a
`ConsoleOnly` scope, which keeps that task's writes out of the ring ([decision 18](#decisions)).
Improv's packets are kept out the same way: they are binary, not lines.

A core dump holds what was on each task's stack when the board crashed. That can include key
material a task was working with. The network is already the boundary for everything else this
API does ([Routes](#routes)), and it is for this too. `DELETE /firmware/coredump` takes the same
`Host` check as the firmware PUTs.

**What each board keeps** ([decision 16](#decisions)):

| Build | Core dump | Its static internal SRAM | Console ring |
|---|---|---|---|
| S3 board (`sdkconfig.psram`) | off | 4,016 bytes | 16 KiB, in PSRAM |
| C6 | on | 1,140 bytes | 2 KiB, internal |
| P4 | on | 3,652 bytes, of 418 KiB left | 16 KiB, in PSRAM |
| CI's update test (`sdkconfig.ci-ota`) | on | 2,128 bytes | 2 KiB, internal |
| CI's 7.1.4 stream set (`sdkconfig.ci-http714`) | off | 2,128 bytes | none |

Each cost is `idf.py size`'s, against the same build with `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE`
(2026-09-25). A build whose task stacks may be in PSRAM, the S3 board and the P4, costs more:
ESP-IDF gives the dump a stack of its own in internal SRAM (`ESP_COREDUMP_USE_STACK_SIZE`, 1,792
bytes at the least). A JOC stream has left the S3 board 43 bytes of internal SRAM (hearth_sink's
README). A crash there still says it panicked, in `GET /firmware`'s last update, and the ring
keeps what the console said before it.

The core dump's code adds 16 to 17 KB to an image. The C6's on the 4 MB table, the tightest, was
1,665,856 bytes on 2026-09-25, which left 169,152 (9%) of its 1.75 MiB slot.

**The upload's least free heap.** O2 asks for the C6's internal heap low-water mark during an
upload. The upload now measures it from the moment flash mode has stopped the player to the
upload's end, and prints it (`firmware: the upload's least free internal heap was N bytes`). The
line comes just before the restart into the new image, so it reaches the USB console. After an
update that went through, the ring in RAM is gone with the restart. Measured on 2026-09-25, each
board taking a whole image from O4's image:

| Board | Internal heap free as flash mode starts | Least free during the upload |
|---|---|---|
| C6 (COM9) | 135,920 bytes, largest block 86,016 | 114,308 bytes |
| S3 board (COM15) | 94,335 bytes, largest block 31,744 | 89,415 bytes |

Neither comes near running out during an upload. The S3 board is the tighter of the two, and
still keeps 89 KB free.

## ac3hearth (O5)

**Status, 2026-09-30: built and merged** as #1039, with fixes from #1053 (the sink is kept while
an update runs and while the tab shows how it ended, and one broken upload is sent again).

The desktop app finds sinks by mDNS, and has a settings page for each paired Hearth sink with
Speakers and Decoder tabs. O5 adds a third tab, **Firmware** ([decision 19](#decisions)):

- **What the sink runs.** The image in each slot, with its version, its state and whether the
  board's own check found it intact. Whether it is this app's own build
  ([decision 20](#decisions)). Also a trial, an update another client is sending, flash mode, how
  the last update ended, and the last crash (O4).
- **Update from a file…** The app reads the image and checks it before anything is sent (below). A
  dialog then names the image and the version it replaces, and says the sink stops playing while
  it takes it. Roll back and Restart ask first too.
- **The update's progress.** The bytes sent, then the board's own stages through the restart and
  the trial. Then how it ended, in the tool's words: updated, rolled back and why, refused and
  why, or not come back and what to try.
- **Without a cable.** Buttons that open the sink's recent console output and its core dump in the
  browser.

**How it works** ([decision 21](#decisions)). `ac3::hearth::SinkFirmware`
(`apps/hearth/engine/src/sink_firmware.hpp`) is `ota.py push` in C++, on a thread of its own for each
sink:

- It talks to the board's web server on port 80, at the address mDNS gave for the sink. Nothing
  goes through Sendspin, so the tab follows the sink through the restart that takes it off
  Sendspin.
- It reads GET /firmware into `ac3forge::FirmwareStatus`, the struct the board renders it from, and
  holds an image to the board's own rules in `firmware_image.hpp`. Both headers are in
  `firmware/esp-idf/iclforge/include` and have no ESP-IDF in them. So the app reads what the board writes,
  and refuses what the board would refuse.
- Its checks before an upload are `ota.py`'s for a bare image:
  - the file is an application image, and its checksum and appended SHA-256 check out;
  - it is for this board's chip, revision, project and flash size, and it fits the slot;
  - the board is not on trial or taking another update;
  - the board has two slots, and its network is not only built into the image it runs.
- The upload carries the file's SHA-256 as its `Content-Digest`. The wait after it reads GET
  /firmware every 1.5 s, for up to six minutes, as `ota.py` does.
- The board is asked about its firmware once a second, and only while the tab is open.
- When the window closes during an upload, the app does not wait for the board's answer. On
  Windows nothing wakes a socket's wait from another thread, so the request's thread is let go to
  finish on its own timeout.

The display strings are formatted in the engine (`sink_firmware_view.hpp`), as the Network page's
others are, so they are tested without Qt.

**Tested.**

- `ac3tests` `[sink-firmware]` checks the parser against the board's own `render_firmware_status`,
  and the file checks against synthetic images. It also covers each refusal, each step of the
  wait, and the tab's rows.
- The same suite runs the client against a stand-in board on loopback. It covers an accepted
  update, a rollback, a refusal before sending and one after, a board that never decides, the
  answers to Roll back and Restart, and closing during an upload's answer.
- A hidden case in the same suite sends an image to a real board. On 2026-09-25 it took the C6 on
  COM9 from o4-c6-b to o4-c6-a: the image was accepted after its 30 s trial, and its SHA-256 on
  the board matched the file's.
- The Qt Quick suites open the tab on a paired test sink and with no sink selected.

Shipping sink images inside the app's release packages, once they are signed (O7), would need a
release key held by CI, which means a secret only the user can set. That is its own decision,
taken when it comes up.

## Published images

**Status, 2026-09-30: built and merged** as #1040. CI packages the four images on every run of
the ESP lane and uploads `esp32-firmware`. No release has carried them yet: the last release,
v0.10.0-beta.1, is from before O8.

CI builds the desktop packages for each platform, and `release.yml` publishes them. If boards are
to be updated over the network, their firmware should come the same way, so that updating a board
does not need an ESP-IDF install. That is O8.

**What CI built on 2026-09-24.** In `_build.yml`, the S3's Sendspin board configuration and the
C6's were compiled, then thrown away: the S3's with `rm -rf build`, and the C6's in
`$RUNNER_TEMP`. Neither was uploaded. CI built the P4's bare-metal probe, not `hearth_sink` for
the P4; O1a (#1019) added that build. Every image CI runs under QEMU is a CI shape
(`sdkconfig.ci-*`), not a board's.

**What O8 builds.** One image for each board configuration the guides describe, from the same
overlay lists the board recipes use:

| Image | Overlays after `sdkconfig.defaults` | Table | For |
|---|---|---|---|
| `hearth-sink-esp32s3` | `hw`, `psram`, `sendspin` | 16 MB | an S3 with 16 MB of flash and 8 MB of octal PSRAM, such as the DevKitC-1 N16R8 |
| `hearth-sink-esp32c6` | `hw`, `sendspin`, `c6`, `sendspin-c6` | 4 MB | any C6 module |
| `hearth-sink-esp32c6-16mb` | the same, then `flash16mb` | 16 MB | a C6 with 16 MB of flash, such as the board on COM9 |
| `hearth-sink-esp32p4-rev1` | `hw`, `p4`, `sendspin` | 16 MB | a P4 of silicon revision v1.x, such as the FireBeetle 2 on COM10 |
| `hearth-sink-esp32p4-rev3` | `hw`, `p4`, `p4-rev3`, `sendspin`, `panel`, `panel-dsi` | 16 MB | a P4 of silicon revision v3.x with the 7-inch panel of Espressif's ESP32-P4X-Function-EV-Board ([planning/karaoke-p4x-panel.md](karaoke-p4x-panel.md)) |

A P4 of revision v3.x needs a build without `sdkconfig.p4`'s revision settings, which
`sdkconfig.p4-rev3` takes over (v3.1 to v3.99, 400 MHz). It is the one image that carries a
screen, and the two P4 images refuse each other's chips at boot (a v1.x chip's bootloader sees
revisions 1.00 to 1.99, a v3.x image's sees 3.01 to 3.99), so a wrong flash cannot happen over the
network: `ota.py` chooses by the revision the board reports. Built by CI and **not run on a v3.x
board yet** ([decision 14](#decisions)).

No published image has a network built in: a board gets its network from Improv or from the page,
and keeps it in NVS. CI refuses to publish an image whose `sdkconfig` sets
`CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID` or `_PASSWORD`.

**What each image publishes:**

- `<image>-<version>.bin`: the app image, for updates over the network.
- `<image>-<version>-factory.bin`: bootloader, partition table, empty `otadata`, app, `audio` and
  `storage` merged into one file, written at `0x0`. It is for a new board: it also overwrites NVS.
- `<image>-<version>-parts.zip`: the same pieces as separate files, with a `flash_args` of
  relative paths. This is how a board already in use moves to this layout with its NVS kept
  ([Flash layout](#flash-layout)). The build directory's own `flash_args` names `audio`'s source
  by absolute path, so it cannot be shipped as it is.
- `<image>-<version>-elf.zip`: the ELF, so a backtrace or a core dump (O4) from a published image
  can be read.
- `hearth-sink-manifest.json`: every image of the release, with its chip, table, revision range
  and SHA-256, which `ota.py` reads to choose an image for each board.

`<version>` is the release's tag. In CI's other runs it is `git describe` of the commit, which on
their shallow checkout is the commit's hash. The build steps run that `describe` themselves:
ESP-IDF's own fails in the build container, whose user does not own the checkout, and ESP-IDF
then names the image "1". Found on 2026-09-25, when an image from a PR's run reported "1" on the
C6. CI now refuses to publish an image named "1".

**When.**

- **On every CI run** that builds the ESP lane: the images are uploaded as a workflow artifact
  kept for 14 days. `ota.py push --run <run id>` downloads one with `gh run download`, so a PR's
  firmware can go onto a board with no local build.
- **At a release:** `_build.yml` uploads them as `packages-esp32-firmware`, which the
  `github-release` job already collects (`pattern: packages-*`). The images then get what every
  release asset gets: `SHA512SUMS`, the GPG signature when the key is provisioned, the SBOM and
  a build provenance attestation (`gh attestation verify`). The "Verify every documented package
  was built" step gains a line for each image, and `docs/releasing.md`'s "What gets published"
  lists them.

A check beside the others (`tools/ci/check_firmware_package.py`, after
`check_hearth_package.py`) opens each image before it is uploaded. It checks:

- the chip ID and revision range are the right ones for its name;
- `hash_appended` is set and the image's SHA-256 matches;
- the image fits the smallest slot of its table;
- the `parts.zip` flashes the same bytes as the factory image;
- no network is built in.

**Choosing the image for a board.** `ota.py push --release <tag|latest>` reads the release's
manifest and each board's `/hardware` and `/firmware`: chip, revision, flash size, PSRAM and
partition table. It downloads the image that matches and checks its SHA-256 against the
manifest's and against `SHA512SUMS`. A board that no image fits is named and skipped.
`--release latest --all` updates every board on the network to the newest release, one board at
a time.

**Signing, when O7 comes.** A published image has to be signed with the key the boards trust. That
means a release key held by CI, a secret only the user can set, or signing on the maintainer's
machine before upload. O7 decides which.

**Built** (O8, 2026-09-25), as above, with these specifics:

- `tools/hearth/package_firmware.py package` writes one image's files from its build directory.
  It lays the factory image out itself: each region at its offset, with `0xFF` between them as
  erased flash has. For the C6's 16 MB build that came to byte for byte what `esptool merge-bin
  @flash_args` writes, all 9,109,504 bytes. `check_firmware_package.py` then lays the parts out
  again with its own code, so each checks the other. The zips' members carry a fixed date, so one build packages to the
  same bytes every time.
- Each image's facts go into a fragment (`<image>.json`), which `package_firmware.py manifest`
  merges into `hearth-sink-manifest.json`. The facts are its chip, revision range, flash size,
  PSRAM, the partition table it ships and each part's offset. The last of those is what the
  browser installer's manifest needs (O9).
- `build-esp32s3` packages the S3 board's image and `build-esp32c3` the other three; the 16 MB C6
  is one more build there. `package-esp32-firmware` merges and checks them, and uploads
  `esp32-firmware` (14 days) and, on a release, `packages-esp32-firmware`.
- `build-esp32c3` builds one more P4 image, with the AC-4 decoder (`sdkconfig.ac4`, phase D14b of
  [`ac4.md`](ac4.md)) and the stage timers on, to hold that it compiles. It is not packaged: the
  release images have the AC-4 decoder off, so they decode AC-3 and E-AC-3 only.
- `release.yml` lists the four factory images and the manifest in its completeness check. It adds
  `*.bin` and the manifest to the files it checksums, signs and attests, whose lists name
  extensions and had no `.bin`.
- `ota.py push --release` reads the release through the GitHub API with the standard library, so
  it needs neither the GitHub CLI nor a token for a public repository. It downloads the manifest
  and `SHA512SUMS` first, and then only the image each board takes. `--run` uses `gh run
  download`, since workflow artifacts need a token.

Checked on 2026-09-25 against #1040's own CI run (36130182488):
- **Packaging.** `package-esp32-firmware` published the four images.
- **Onto a board.** `ota.py push --run` chose the 16 MB C6's image for the C6 on COM9 and sent it
  over Wi-Fi. The board checked it and accepted it after its trial, in 67 s.
- **What it found.** The image called itself "1", which led to the version fix above.

## The user guide

**Status, 2026-09-30: built and merged** as #1041. The exit, someone taking a blank board to one
that plays and then updating it to a newer release with only the guide, has not been run: it
waits for a release that carries sink firmware.

O9 writes `docs/hearth/sink-firmware.md`, for someone who has a board and a release, and no
ESP-IDF install:

1. **Which image.** The table above, and how to tell a board apart: `esptool chip-id` and
   `esptool flash-id` over USB, or `/hardware` on a board already on the network.
2. **Checking a download.** `SHA512SUMS`, `gh attestation verify`, and the GPG signature when
   the release has one.
3. **A new board.** The browser installer or `esptool write-flash 0x0 <image>-factory.bin`
   ([decision 15](#decisions)), then joining a network over Improv, as the S3 guide already
   describes.
4. **A board running an older build.** The browser installer with erasing left off, or the
   `parts.zip` and `write-flash @flash_args`. Either keeps the board's name, network and
   pairings.
5. **Updating over the network.** `ota.py push --release latest --all`, the page's **Update
   firmware…**, or `curl -T`.
6. **What the board does** during an update: flash mode, the trial and a rollback, and what
   `ota.py` and the page show for each.
7. **Going back.** **Roll back** on the page, `ota.py rollback`, or an older release pushed as
   any other.
8. **When a board does not come back.** Cycle the power during a trial; failing that, USB, with
   the browser installer or the release's `parts.zip`. If the board does not answer on USB, hold
   BOOT while pressing RESET to put it in the ROM's download mode, which is always there.

O9 also brings the rest of the documentation into line:

- `docs/hearth/sink-esp32-s3.md`'s "Build and flash" offers the published image first and
  building it second;
- `docs/hearth/index.md` links the guide, and `mkdocs.yml` lists it;
- `docs/releasing.md` covers the firmware in its release checklist.

**The browser installer** ([decision 15](#decisions)) is a page on the documentation site built
on ESP Web Tools. Its supported chips include the ESP32-S3, ESP32-C6 and ESP32-P4.

- **How it flashes.** Over Web Serial on the board's USB serial connector, talking to the chip's
  ROM download mode, as `esptool` does. It resets a board into download mode by itself where the
  board allows that. A board that does not can be put into download mode by hand, by holding
  BOOT while pressing RESET, and the installer then talks to the ROM directly. Such a board is
  one whose firmware no longer answers on USB, or one that does not reset cleanly, like COM16.
  The ROM cannot be overwritten, so the same page recovers a board that will not boot.
- **After flashing.** A board put into download mode by hand may need one press of RESET to start
  the new firmware. The page then offers Improv, which the boards already answer, to give the board
  its network. A first install needs a browser and a USB cable, and nothing else.
- **Its manifest lists the pieces, not the merged factory image**: bootloader, partition table,
  `otadata`, app, `audio` and `storage`, each at its offset. It sets
  `new_install_prompt_erase: true`, so the person installing chooses whether to erase. A new board
  is erased; a board already in use is left unerased, so its NVS survives and the same page moves
  it to the new layout.
- **Browsers:** Chrome, Edge and Firefox, which ESP Web Tools lists as having Web Serial; not
  Safari, and nothing on iOS.
- **Hosting.** The page cannot fetch GitHub release assets, which carry no CORS headers, so
  `docs.yml` copies the latest release's images and manifest into the site when it deploys.
  Tried in a browser on 2026-09-25: a page on another site can read GitHub's release list from
  its API, but not a release's files, through their download links or through the API.
- **On the P4,** the installer has to be on the USB-C connector that carries the console. The
  board's other connector is a separate USB peripheral.

**Built** (O9, 2026-09-25): `docs/hearth/sink-firmware.md` follows the eight steps above, and
`docs/hearth/sink-installer.md` is the installer. The specifics:

- **ESP Web Tools 10.4.0 is served by the site itself.** `docs.yml`'s deploy job packs it from npm,
  checks the tarball against the integrity npm published for that version, and unpacks it into
  the page's assets, with its licence. A reader's browser then loads nothing from a third party on
  the page that writes their board's flash.
- **The firmware.** `tools/hearth/installer_site.py` fills the assets in the same job. It reads
  the newest release that publishes sink firmware with `ota.py`'s own reading of a release,
  checks each image's `parts.zip` against the manifest and `SHA512SUMS`, unpacks it, and writes
  ESP Web Tools' manifest for each image.
- **One manifest for each image.** ESP Web Tools chooses a build by chip family alone, so the two
  C6 images need a button each.
- **Every chip, every time.** The page lists the ESP32-S3, the ESP32-C6 and the ESP32-P4, each
  with a button for each of its images. A chip the release has no image for says so, rather
  than going missing.
- **Which release.** The page names the release its images came from, with its date and a link.
  It also asks GitHub's API which is the newest release with firmware. When that is newer than
  the site's, it says so, and points to that release's page and to the guide's `esptool` steps.
  A page the site has not been republished for since a release is then still correct about
  what it offers.
- **Keys typed into the installer's dialog.** Material for MkDocs takes bare keys as shortcuts
  unless a text field has the focus: `s`, `f` and `/` open its search, and `n`, `p`, `.` and `,`
  turn the page. It cannot see a field inside ESP Web Tools' nested shadow roots. On 2026-09-25 a
  network name typed into Improv's form lost a letter to the search box. The page now stops keys
  that come from the dialog at the body, after the field has had them.
- **Before a release.** `installer_site.py --run <id>` or `--dir <path>` fills the assets from a
  CI run's `esp32-firmware` artifact, or from a directory of the same files, to try the page
  with real images before any release publishes them.
- **Before any release publishes firmware**, the script writes an index with no images and the
  page says so. Any other failure, such as the API not answering, fails the deploy rather than
  publish an empty installer.
- **After a release.** A release created with a workflow's token starts no other workflow by its
  own event, so `release.yml`'s `github-release` dispatches `docs.yml` as its last step.
- The rest of the documentation is brought into line: the Hearth overview, the S3 guide's
  "Build and flash", `docs/releasing.md` and the site's navigation.

Checked on 2026-09-25: the site builds with `mkdocs build --strict`. The four images were built
the way CI builds them and passed `check_firmware_package.py`. With the assets filled from them by
`installer_site.py --dir`, the page served locally lists the S3, the C6 (4 MB and 16 MB) and the
P4, each with its install button. Then with stand-in indexes:
- **An older release without the S3:** the page names and links that release, and says the S3
  has no image. Asked for a release file every release carries, the real GitHub API showed
  v0.10.0-beta.1 as newer, and the page said so.
- **No images:** every chip says it has none yet.
The exit waits for a release that publishes sink firmware, and a blank board.

## What stays USB-only, and how a board is recovered

**USB only:**

- the migration to the new layout;
- any later change to the partition table, the flash size setting or the bootloader, which
  includes an ESP-IDF upgrade that changes the bootloader;
- the P4's co-processor firmware, until O6 ([decision 9](#decisions)).

Signing does not need USB: [Signing, later](#signing-later) switches it on over the network.

ESP-IDF v6.1 can update the bootloader and the partition table over the network, through a
staging partition and a final copy (`esp_ota_set_final_partition`). This plan does not use that
([decision 6](#decisions)): a power cut during the copy leaves a board with nothing to boot.

**Recovery, from least to most effort:**

1. The trial rolls back by itself.
2. A power cycle during a hung trial rolls back.
3. `PUT /firmware/rollback` for a fault that appears after acceptance.
4. USB: `write-flash @flash_args`, as today, or from O9 the browser installer. Both talk to the
   ROM download mode, which is in mask ROM, and this plan burns no eFuse that could lock it, so a
   board can always be recovered this way. A board that does not reset into it by itself is put
   there by holding BOOT while pressing RESET. On the P4 this is the console USB-C, not the OTG
   port.

## Per chip

**ESP32-S3.**

- The 16 MB table.
- The only chip with a QEMU machine, so CI's end-to-end OTA tests run here, over the emulated
  Ethernet.
- COM16 needs `--after watchdog-reset` for its migration flash: `esptool`'s default hard reset
  leaves that board in download mode (the `i2s_player` README records it).

**ESP32-C6.**

- The 16 MB table on COM9, through `sdkconfig.flash16mb`; the 4 MB table for other modules.
- No PSRAM, and Wi-Fi's code in flash. Flash mode's teardown matters most here, and O2 measures
  it: the internal heap's low-water mark during an upload, and whether Wi-Fi keeps its association
  through the erase windows. It did through every upload, and the low-water mark was 114,308
  bytes.
- No QEMU machine, so board-only.

**ESP32-P4.**

- The 16 MB table.
- The tightest bootloader of the three: 1,152 bytes spare with rollback at log level Info
  ([Flash layout](#flash-layout)).
- Rev v1.3: images must come from `sdkconfig.p4` (`ESP32P4_SELECTS_REV_LESS_V3`,
  `REV_MIN_100`). A default P4 image needs v3.1 and is refused before anything is written. O2
  pushes one on purpose to prove it. The other direction holds too: this board's images accept
  revisions v1.0 to v1.99 (`min_rev_full` 100, `max_rev_full` 199 in the image of 2026-09-24), so
  a production v3.x P4 refuses them.
- Wi-Fi through the co-processor, which stays up in flash mode.
- The co-processor's own firmware (boot log: "Version mismatch: Host [2.12.0] > Co-proc [0.0.0]")
  is a separate flash target: [The P4's co-processor](#the-p4s-co-processor-o6) has O6's study.
- **A restart once left the P4 unable to reach its co-processor** until its power was cycled (O2,
  2026-09-25). Every update ends in a restart; the next section says what is known. The soak of
  2026-09-26 ran the P4 through dozens of restarts without a repeat
  ([Robustness](#robustness-2026-09-26)).
- One upload to the P4 broke off 64 KiB in, about 10 s after the board had restarted: the board
  restarted again, with nothing recorded, and the same push passed a few minutes later. Its
  console was not attached, so the cause is not known.
- No QEMU machine. Its USB is the console USB-C (COM10), which it was migrated over on
  2026-09-25.

## The P4's co-processor (O6)

**Status, 2026-09-30: studied, and not built** (#1042 is the study). Nothing here was tried on a
board, and nothing that updates the co-processor exists: no route, no staging in `reserve`, no
`esp_hosted` update call. The example's manifest still asks for `espressif/esp_hosted` "~2" and
`espressif/esp_wifi_remote` ">=0.10,<2.0" (`main/idf_component.yml`), so decision 22 is not
taken. The steps under "What O6 would do next" wait for someone at the desk with a USB-UART
adapter.

The P4 has no radio of its own. Its Wi-Fi goes over SDIO to the FireBeetle 2's onboard ESP32-C6,
which runs Espressif's `esp_hosted` co-processor firmware. That firmware is separate from
`hearth_sink`, and no update of the P4 changes it. O6 is a study first
([decision 9](#decisions)). This section comes from the `esp_hosted` sources the P4 builds with,
the issues on Espressif's `esp-hosted-mcu` repository (the numbers below) and DFRobot's
schematic of the board, read on 2026-09-25. None of it was tried on a board.

**What `esp_hosted` offers.**

- The build asks for `espressif/esp_hosted` "~2", which resolved to 2.12.13, with
  `esp_wifi_remote` 1.6.5. The example's lock file is not committed, so "~2" moves to the newest
  2.x whenever the dependencies are resolved again ([decision 22](#decisions)).
- The host updates the co-processor with four calls over the existing SDIO link:
  `esp_hosted_slave_ota_begin`, `_write` and `_end`, and `_activate` from 2.6 on. The
  co-processor writes the slot it is not running, and switches to it only after `esp_ota_end` has
  checked the image. Espressif's example, `host_performs_slave_ota`, takes the image from HTTPS, a
  LittleFS file or a raw partition.
- The C6 needs two OTA slots for this; `esp_hosted`'s own C6 table has two of 1,920 KiB.
  DFRobot's table on this board is not known.

**The co-processor on this board.** The host prints version 0.0.0 when the co-processor sends
none, which `esp_hosted` has sent since 2.1.6, in July 2025. So this C6 runs firmware from before
then. Factory images on other P4 boards (0.0.0 and 1.4.x) have been updated from the host (#143,
#244, #113). Whether this one can be is not proven; the C6's own console, or
`esp_hosted_get_coprocessor_fwversion()` once the link is up, would say.

**What can go wrong.**

- **An update cut short, or an image that does not check out:** safe. The co-processor keeps its
  old image.
- **An image that checks out and then crashes, or never brings SDIO up:** not safe.
  - `esp_hosted`'s co-processor has no app rollback, and its bootloader skips only an image that
    fails to load. A C6 left boot-looping this way after an update over SDIO has been reported
    (esphome/esphome#16692).
  - There is no USB path to the C6 on this board. Its UART and IO9 (BOOT) go only to test pads on
    the back (DFRobot's schematic V1.0, page 7). The P4 reaches it only over SDIO and its enable
    pin (GPIO54), and the C6 is on the P4's own 3.3 V, so the P4 cannot cycle its power either.
  - Recovering such a C6 takes a 3.3 V USB-UART adapter on those pads, with the P4 held in reset.
  - One wire from a spare P4 GPIO to the IO9 pad would let the P4 put the C6 into its download mode
    and reflash it over SDIO with esp-serial-flasher, which marks that mode experimental. This is
    untested.

**The restart hang, which comes first.** The restart that ended O2's rollback check, on
2026-09-25, left the P4 unable to reach the C6. The restarts of the three pushes before it that
day had come back normally.

- The SDIO card initialised, but the C6 never sent the event that completes the link:
  `sd_host_wait_for_event returned 0x107`, then "Not able to connect with ESP-Hosted slave
  device".
- The P4 resets the C6 through GPIO54 each time it starts, and that did not bring it back.
  `esp_hosted` restarted the P4 about every 15 s ("Restarting host"). The release it had been
  rolled back to was on trial, so the bootloader went back to the update in `ota_1`, as the trial
  is meant to (its reason: "it restarted before it had proved itself").
- A power cycle brought it back. About ten restarts that evening then came back normally,
  through every one of O2's checks, so the hang does not follow every restart.
- It matches an open upstream issue, #240 (since 2026-08-31): a P4 v1.3 with a C6 on GPIO54, after
  a large update over Wi-Fi and a restart. Espressif have not reproduced it, and have asked for the
  C6's console.
- The version mismatch (host 2.12 against a co-processor older than 2.1.6) has not been shown to
  cause it: #240 had matched versions, and the hang comes before any call the mismatch concerns.
- Every P4 update ends in a restart, so until the cause is known, any of them can end this way.
- Settings worth trying, at the desk:
  - a longer `CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS` (1,500 ms now);
  - `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY`, in place of a reset at every start;
  - `CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE` off, with an `esp_wifi_init()` failure that
    retries rather than restarting the P4.

**Staging.** A C6 co-processor image is about 1.2 MB: ESPHome's prebuilt 2.12.13 SDIO slave is
1,240,368 bytes. The P4's `reserve` partition (4 MiB at `0x8B0000`, [Flash layout](#flash-layout))
holds that three times over, and the example can read an image from a partition by its label.
So the image would be uploaded to `reserve` first and checked there, as the P4's own images are,
and the C6 fed from it with the network idle, as Espressif advise (#71). Streamed straight from
the network, the upload would pass through the chip being rewritten.

**What O6 would do next, in order:**

1. **With someone at the desk:**
   - the P4's console on COM10;
   - a 3.3 V USB-UART adapter on the C6's pads;
   - the C6's console captured at a cold boot and through a P4 restart;
   - the C6's whole flash backed up, with the P4 held in reset.
2. The restart hang's cause, from that console, before anything else.
3. `esp_hosted` pinned to an exact version, and the co-processor image built at that version for
   the C6: SDIO, with the flash size and mode of the factory image, and checked to fit its slot.
4. **The first update at the desk, with the UART attached:**
   - from `reserve`, with begin, write and end, and activate only on a co-processor of 2.6 or later;
   - the link brought back up;
   - the co-processor reporting the pinned version and joining Wi-Fi.
5. Only then a route of its own. It refuses unless `reserve` holds a checked C6 image (SHA-256,
   header, chip, size within the slot) and the board is in flash mode, and it never runs by itself.
6. Optionally, the IO9 wire, and the P4 recovering the C6 over SDIO.

**Still open:**

- the factory C6 image's version, partition table and flash mode;
- the restart hang's cause, and why one restart hangs when others do not;
- whether the factory bootloader boots a co-processor built with ESP-IDF 6.1;
- whether esp-serial-flasher's SDIO mode works with a v1.3 P4;
- which `esp_hosted` patch the running P4 image has.

## Updates from GitHub releases (O10)

**Status, 2026-10-25: designed, nothing built.** The owner decides [decisions 23 to 33](#decisions)
before any slice starts. This section takes up the two things the plan put off: **pull**
([decision 3](#decisions): "pull can come later, for updates from a release server") and **signing**
([O7](#signing-later), [decision 2](#decisions)). O8 and O9 are the published images and the guide,
both built, so this is O10. It was written after the rename, so Kconfig names are `ICLFORGE_*` and the
repository is `iainchesworthlabs/iclforge`; the rest of this file keeps the names of its time.

The owner, 2026-10-25: "There is probably a check-for-firmware option in the preferences ... We have to
extend our existing firmware thing to go look at GitHub for the latest release, and if there's a later
release, upload it from GitHub and make sure it's signed. We'll need signed firmware. That's probably a
bit of a longer bow." And: this is a generic product base, so it has to work for every board variant,
including the boards with no screen (through the device web page and the REST routes).

**What does not change.** The A/B slots, flash mode, the trial, rollback, the integrity chain and
`PUT /firmware` stay as built. A pull is a second *source* for the same upload core: from the first byte
of the image on, a board that pulls does what a board that is pushed to does. What is new is where the
bytes come from (an HTTPS client), how the board learns that there are new ones (an index file), and
what it demands of them (a signature).

### What was checked on 2026-10-25

| Fact | How it was checked | What follows |
|---|---|---|
| The repository is public | `gh repo view iainchesworthlabs/iclforge`: `PUBLIC` | A board can read releases with no token, and none has to be stored on it |
| Every release is a Pre-release | `gh api repos/.../releases`: 10 releases, v0.2.0-beta.1 to v0.10.0-beta.1 (2026-09-01), all `prerelease: true` | `.../releases/latest` and `.../releases/latest/download/<file>` both answer `404` (tried unauthenticated). "Latest" cannot be how a board finds the newest release until a stable one exists |
| The API allows 60 calls an hour | `X-RateLimit-Limit: 60` on an unauthenticated call; 13 were already spent by others on this address at the first call | The six boards and every laptop in the house share one address |
| The API's answer is too big for a board | `releases?per_page=1` is 263,050 bytes (v0.10.0-beta.1 has 127 assets) | A board cannot hold it, and it would need a streaming parser to find five file names |
| A download is a redirect to another host | `.../releases/download/<tag>/<file>` answers `302` with a 901-byte `Location` (a signed query string) to `release-assets.githubusercontent.com`; the target answers `200` with `Content-Length` and `Accept-Ranges: bytes`. The `302` itself is `Cache-Control: no-cache` | The board makes two TLS connections, to two hosts. Its HTTP client's header buffer must hold more than 1 KB; `esp_http_client`'s default is 512 bytes, which cannot |
| Two certificate authorities, and leaves that rotate | `openssl s_client`. `github.com`: Sectigo E36, under Sectigo E46, under USERTrust ECC (an ECDSA leaf, valid to 2026-11-29). `release-assets.githubusercontent.com`, `raw.githubusercontent.com` and `*.github.io` all presented one Let's Encrypt certificate: an RSA-2048 leaf from "YR1", under "ISRG Root YR", which is cross-signed by ISRG Root X1; issued 2026-09-30, valid 90 days | Trust roots, never leaves. Today a board needs USERTrust ECC and ISRG Root X1. GitHub changes a host's authority without notice, so the bundle carries a spare |
| GitHub Pages is on | `gh api repos/.../pages`: `gh-pages` branch, `https://iainchesworthlabs.github.io/iclforge/` | An alternative home for the index ([decision 23](#decisions)) |
| No board speaks TLS today | A search of `firmware/`: no certificate bundle, no `esp_tls`, no `esp_https_ota`. `esp_http_client` is used for the plain-HTTP stream source only (`main/source/http/byte_source.cpp`); mbedTLS is linked for SHA-256 | TLS is new code, flash and RAM on every board |
| A board keeps the time with SNTP | `main/clock/sntp/wall_clock.cpp`: `wall_clock_read()` gives no time until a server has answered, and none before 2026. QEMU builds `clock/none` | A certificate has dates (the CDN's leaf starts 2026-09-30), so a board with no time cannot check one. A QEMU build cannot take a TLS test |
| The P4's TCP/IP stack is on the P4 | `sdkconfig.p4`: lwIP is configured on the P4; the C6 co-processor carries 802.11 frames over SDIO (`esp_wifi_remote`, `esp_hosted`) | TLS ends on the P4, which has PSRAM. The co-processor sees ciphertext and gets no TLS code and no new RAM cost |
| The host tool already reads GitHub | `ota.py --release` reads `releases?per_page=30` with the standard library | It works from a laptop. It is not a model for a board |

So "TLS memory on the C6" (decision 3's wording) is true of the C6 board, which has its own radio
and 512 KB with no PSRAM, and is not true of the P4's C6, whose image stays as it is
([The P4's co-processor](#the-p4s-co-processor-o6)).

### What the board reads, and where it is

The board never reads GitHub's API. It reads an **update index**: one small file for each channel,
`hearth-update-stable.json` and `hearth-update-beta.json`, written by CI from the release's
`hearth-sink-manifest.json` (so the facts of each image have one source, `package_firmware.py`). The
indexes are assets of one long-lived release, tagged `firmware-channels` and flagged Pre-release so that it
can never be "latest" and never confuses `ota.py --release latest` or `installer_site.py`, which pick a
release by the manifest it carries (a check for slice O10c). The board's URL for it is
`https://github.com/iainchesworthlabs/iclforge/releases/download/firmware-channels/hearth-update-beta.json`.
The images stay where O8 puts them, as assets of the release's own tag.

```
{
  "format": 1,
  "channel": "beta",
  "sequence": 2026110501,                  rises with every index CI writes
  "version": "v0.11.0-beta.1",             the release's tag, which is also the images' version
  "released": "2026-11-05",
  "images": [
    { "name": "hearth-sink-esp32s3",
      "file": "hearth-sink-esp32s3-v0.11.0-beta.1.bin",
      "size": 1583104, "sha256": "<64 hex>",
      "project": "<esp_app_desc_t project>", "chip_id": 9,
      "min_rev_full": 0, "max_rev_full": 99, "flash_size": "16MB", "psram": true,
      "secure_version": 0 },
    ...  one entry for each image the release publishes: five today
  ]
}
```

About 330 bytes an image and 2 KB in all; the board reads at most 4 KiB.

- **Files, not URLs.** The board builds `<releases base>/<tag>/<file>` from its own Kconfig
  (`ICLFORGE_FIRMWARE_UPDATE_BASE_URL`), so nothing in the index can send it to another host, and an
  integrator's firmware points at its own repository and signs with its own key
  ([decision 32](#decisions)). The release notes link is `<repository>/releases/tag/<tag>`, built the same
  way. A `file` outside `[A-Za-z0-9._+-]` is refused.
- **The board picks its image by facts, not by name.** The same facts `refuse_image` checks
  (`firmware_image.hpp`): project, chip, flash size and a revision inside the range, then PSRAM. A new board
  variant appears in the index by being in a release; no board's firmware changes to be offered it, and a
  board with no entry says "this release has no image for this board". The image's own head is checked
  again after the first 288 bytes arrive, before anything is erased, exactly as for a push.
- **The index is not trusted for anything that matters.** A swapped index can make a board download a file
  that then fails its size, its SHA-256 or its signature, and change nothing. What it cannot do is install
  code, because the code's signature is checked against a key the index does not control. It can point
  at an older signed release, which the version rules below refuse, and it can hide a new release,
  which no design stops while the transport is someone else's.
- **Order of publication.** CI publishes the images, checks that they download, and only then replaces
  the index, so a board never reads an index that names a file that is not there. `gh release upload
  --clobber` replaces an asset by deleting it and uploading it again, so for a moment the index may answer
  `404`; the board says "could not reach the update list" and a retry works.

### Signing

**What each scheme is.**

- **(a) Signed apps, ESP-IDF's own** (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, RSA-3072; the O7
  design). A signature sector is appended to the app image. Every image written through `esp_ota_*`
  (`esp_ota_end`) is checked against the keys in the signature blocks of the *running* image
  (`secure_boot_signatures_app.c`: "Take trusted digest key(s) from running app"). No eFuses, no change to
  the bootloader. RSA is the scheme all three chips share: ESP32-S3 has only RSA for Secure Boot V2
  (`SOC_SECURE_BOOT_V2_RSA`); the C6 and the P4 also have ECDSA.
- **(b) A detached signature** (ECDSA P-256 over the index, which holds each image's SHA-256), checked by
  code of ours before `esp_ota_set_boot_partition`, against a public key compiled into the firmware.
  Ed25519 would mean a vendored library, because the mbedTLS this tree builds with does not implement
  it (its own `tf-psa-crypto` notes say PureEdDSA is "not yet implemented"); P-256 needs none.

**What each protects, and what it does not.**

| | (a) signed apps | (b) detached signature |
|---|---|---|
| A web page or a machine on the LAN `PUT`s its own image | **Refused**, however the image arrived: the check is in the write path, so push and pull share it, and so do the page, `ota.py` and ac3hearth | Only on the pull. `PUT /firmware` would need a signature of its own (a header, and a change to every client), or it stays open |
| A release asset or the index swapped (a stolen GitHub account, the CDN) | **Refused** unless signed with a trusted key | **Refused** the same way |
| TLS broken on the way | **Refused** the same way | **Refused** the same way |
| An old signed image replayed | **Not stopped by the signature.** Stopped by the version rules below, which read the signed image's own `secure_version` | The same: needs the rules |
| The signing key stolen | The thief's images are accepted until a board runs an image whose key ring leaves the key out ([Keys](#keys)) | The same |
| Hostile code built in CI and then signed | **Not stopped.** A signature says who approved, not what is in it. The approval gate and the build provenance (`gh attestation verify`) are the checks | The same |
| Someone with a USB cable | **Not stopped**, by design: no eFuses are burned, and the cable is how a board is recovered | The same |

**Recommend (a).** One mechanism covers every way an image reaches a board, which is the reason O7 chose
it. It adds no cryptographic code of ours to audit, and an integrator signs with a key of theirs and
ships no key constant of ours. Its costs:

- **4 KiB for each image** (one signature sector, which holds up to three blocks; slice O10a confirms the
  figure against `esp_secure_boot_sig_block_size()`). The 4 MB C6 table has none to spare
  ([Which builds can pull](#which-builds-can-pull), [decision 33](#decisions)).
- **The running image is the trust anchor.** A board whose running image has the check compiled in and
  *no* signature block finds no trusted key: `esp_secure_boot_get_signature_blocks_for_running_app()`
  returns `ESP_ERR_NOT_FOUND` ("No signatures were found for the running app"), and that reads as an image
  that can take no update at all, push included. O10a proves it under QEMU. If it holds, every image built
  with the check is signed, development images with the development key, and the local recipe does it for
  you.
- **The first signed image is the window.** A board still running an image without the check accepts any
  image; a hostile machine on the LAN could push one signed with its own key and strand the owner's
  updates (a USB flash puts it right). The owner closes the window by pushing the first verifying image to
  every board in one sitting.
- **A rejected image is found out late.** The signature sector is the end of the file, so the board
  has the whole of an unsigned image before it can refuse it, with playback stopped. The tools avoid that:
  `GET /firmware` gains a `signing` object (below), and `ota.py` and the page refuse an unsigned file
  before they send it.

**(b)** needs a verifier of ours (host-testable, which is its attraction), a second tool path, and still
leaves the push open unless it is extended; its one real advantage is that dev builds need no signing
because nothing in the write path depends on the running image's blocks. **(c) both** pays for both.
**(d) no signature, TLS and the index's SHA-256 only** leaves every board's code in the hands of whoever
holds the GitHub account, and the owner asked for signed firmware.

**What the board says about itself.** `GET /firmware` gains `signing`: `null` on an image with no check
(a development build, which accepts what it accepts today), else
`{"required": true, "keys": ["<SHA-256 of each trusted public key, hex>"]}`. The tools compare it with
the file's signature sector and say "this board accepts only images signed with 3f2a...; this file is
signed with 91bc..." instead of sending 1.5 MB to be refused. Host-tested in `firmware_status.hpp`'s tests
like the rest of the body.

### Keys

Two keys, two jobs. Both RSA-3072, made with `espsecure generate-signing-key --version 2 --scheme
rsa3072`.

| Key | Where the private half lives | Trusted by |
|---|---|---|
| **Release** | A repository secret in a protected environment (below), and two offline copies. Signs every published image | Every released image |
| **Development** | The owner's machine. Signs local builds | A board the owner has made a development board; never a released image on its own |

- **A released image carries one block, the release key's.** A board trusts the keys whose blocks are in the
  image it runs, so the other two blocks stay free: for the development key, and for a rotation. (A block
  holds a signature of *that* image by its key, so a key can be in an image's ring only if its private half
  signs that image. A second key held offline "for recovery" would have to sign every release by hand;
  that is [decision 25](#decisions)'s alternative (c), not the recommendation.)
- **Rotation.** The owner makes a new key K2 and sets it as a second secret. The next release is signed
  with K1 and K2, so that boards still trusting only K1 take it; the release after is signed with K2 alone.
  A board trusts the keys of the image it runs, so once it has taken a K2-only image it no longer trusts K1.
  A board that never takes it still does. There is no revocation list on a board; the only revocation is an
  update.
- **A development board.** Signature blocks are independent of one another, so anyone can add one to an
  image that has a free slot, and a released image has two. The owner makes a board a development board by
  pushing a *released* image with the development key's block added (`ota.py push --dev-key`), after which the
  board trusts release and development. A development build is signed with the development key alone, so a
  board running one trusts only that key; going back to a release is a push of the release with the
  development block added again. A board nobody made a development board takes only released images, and
  refuses a development build, which is right. Slice O10a writes the three commands down in
  `docs/releasing.md` and proves the round trip under QEMU.
- **CI that is not a release signs with nothing.** Pull-request and nightly images are unsigned (a board
  that trusts a key refuses them, which is right). The QEMU test builds with a **throwaway key made in the
  job**, the design O7 had.
- **The release key in CI.** The owner creates the repository secret `FIRMWARE_SIGNING_KEY` (never created
  by a session) in an **environment** named `firmware-signing` that has the owner as a required reviewer
  and allows only `v*` tags, so a release stops at a button the owner presses, and nothing else in the
  repository can read the key. The signing job is a job of its own, between packaging and upload, with no
  third-party action in it and the key written to a temporary file that is removed. This is the contrast
  with the `pypi` environment, which has no reviewer
  ([release prep, PR #1621](https://github.com/iainchesworthlabs/iclforge/pull/1621)).
- **Never in a worktree.** The example's CMake refuses a signing key inside the source tree or any
  worktree (the guard O7 listed), and a check in the script-lint job refuses a private key anywhere in the
  repository. Public keys and their digests are committed (`firmware/keys/`) so that CI, `ota.py` and the
  guide can name them.
- **If the release key is lost,** with its two offline copies gone as well, no board can take another
  network update from that key's line, and each needs one USB flash, the browser installer or `esptool`, with
  a build that carries a new key. That is the price of the offline copies being the only recovery.
  **If it leaks,** an attacker can sign images that every board trusting it will take, and the owner
  starts the race by rotating at once; boards that are off, or off the network, stay exposed until they take
  the rotation. Someone with the key and the LAN can also push to a board directly, before any rotation. This
  is the cost of having no eFuses.

### What an update may do

| Source | Signature | Version |
|---|---|---|
| Pull (`PUT /firmware/from-release`) | Required, and the running image must carry a key | Strictly newer than the running version, `secure_version` at least the running image's, and the index's `sequence` at least the highest the board has accepted |
| Push (`PUT /firmware`) | Required when the running image carries a key | `secure_version` at least the running image's. An older release pushed on purpose is allowed, and the tool says so |
| `PUT /firmware/rollback` | Not checked: the image already ran here | None |

- **`secure_version`** is the 32-bit field ESP-IDF keeps in `esp_app_desc_t`
  (`CONFIG_BOOTLOADER_APP_SECURE_VERSION`). It is a floor that our code enforces, not ESP-IDF's anti-rollback,
  which burns eFuses and which `check_esp_efuse_free.py` forbids. A release that fixes a security flaw raises
  it by hand; the image carries it inside the signed bytes, so it cannot be edited. The default stays 0.
- **Comparing versions.** `v0.11.0-beta.1` is newer than `v0.10.0-beta.1-2039-gf5aded471` (a build 2,039
  commits past a tag sorts after that tag and before the next one); `-beta.N` sorts before the same number
  without it. A version the parser does not understand is never "newer": it fails closed, and the reply
  says so. The rule lives in `libs/device`, with a table of cases in its test. The version a board stores is
  31 characters, so the comparison is on as many.
- **`sequence`** stops a replay of an old index after the board has seen a newer one. It is kept in NVS
  (`fw_seq`).
- **Channels.** `stable` is the newest release that is not a Pre-release; `beta` is the newest of any kind.
  A board has a `channel` setting in NVS. Its default is Kconfig's, `beta` while every release is a
  Pre-release (an `stable` board would say "up to date" forever), flipped to `stable` by the release that
  is the first stable one. Changing channel never installs anything, and never goes back to an older version.
- **A version that failed here is not offered again.** An update that rolled back, or failed its checks,
  stores its version in NVS (`fw_skip`); `GET /firmware/available` says "v0.11.0-beta.1 is available, but it
  went back on this board on 2026-11-06" and the install button asks for a second confirmation. A newer
  release clears it.
- **A signed board and an unsigned `PUT /firmware`.** Refused: `esp_ota_end` fails and the reply says the
  signature did not check out. The `Host` check on the firmware `PUT`s
  ([decision 13](#decisions)) stays. The plan said signing would make it unnecessary, and it still keeps a web
  page from putting the board in flash mode and stopping what plays.

### TLS on the board

- **Trust.** `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_NONE` and `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`
  with a path: a bundle of USERTrust ECC, ISRG Root X1 and one spare, chosen in slice O10b from what the
  three hosts present that week. A few KB in all. The full default bundle is tens of KB on every board for
  hosts that are never contacted. Validation is on and has no fallback: a connection the board cannot
  verify is not made. Because the *code* is verified by the signature, a failure to validate is a failure of
  availability, not of security; the cost is that a root rotation GitHub makes before a board has a bundle
  that covers it leaves the board unable to pull until it is pushed to (`ota.py push --release`, from a
  laptop, does not depend on the board's bundle).
- **The clock.** The handshake needs the date. A board with no time, or with time sync off
  (`PUT /time-sync off`), answers "needs the time" and does not try. The check does not guess a date.
- **Redirects.** `esp_http_client` follows `302`s, at most three, to `https` only, with a header buffer of
  at least 2 KiB. The first connection is closed before the second opens, so there is never more than one
  session in memory.
- **RAM.** Estimates, to be measured on each chip in slice O10b; the sums below use the figures this plan
  measured for the upload (the table in [Diagnostics](#diagnostics-without-a-cable-o4)).
  - ESP-IDF's default record buffers are 16 KiB in and 4 KiB out; its help text says the asymmetric
    setting saves 12 KB a connection over a symmetric 16/16. A client session with a certificate chain to
    parse commonly peaks at 40 to 60 KB. `CONFIG_MBEDTLS_DYNAMIC_BUFFER` takes some of that back.
  - **S3 board and P4:** the session goes to PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`). Internal RAM
    pays only for the download task's stack, which has to be internal because it writes flash: about 12 KiB,
    against the upload task's 8. The S3 has a 31,744-byte largest block as flash mode starts.
  - **C6, no PSRAM:** the session is internal. It had 114,308 bytes free at its lowest during an upload, in
    flash mode. A session and a stack of about 70 KB fit there, if the estimate holds. They would not fit
    beside a playing stream, so on the C6 a check as well as an install needs the stream stopped.
  - **A check never enters flash mode.** It runs when the board can hold a session beside what it is
    doing, and otherwise answers `503` "stop what is playing to check" (the C6 while it plays; an S3 while a
    JOC stream plays). An install always enters flash mode, after a confirm.
- **Flash.** TLS, the HTTP client's redirects and the bundle add code to every build that pulls. The size
  is measured in O10b, not guessed; a typical ESP-IDF HTTPS client is of the order of 100 KB.

### Which builds can pull

Pull is a build option, `ICLFORGE_FIRMWARE_PULL`, and the board reports whether it has it. Three
reasons a board cannot, each shown with its reason, not hidden:

- **The 4 MB C6** (`hearth-sink-esp32c6`): its slot is `0x1E0000` (1,966,080 bytes) and its profile's
  budget is the same number ([Flash layout](#flash-layout), 2026-10-21). TLS and 4 KiB of signature do not
  fit. It takes signed pushes and not pulls: "This board's firmware has no room for a secure connection".
- **A build whose image carries no signing key** (a development build): "This firmware carries no update
  key". A pull is *never* made without a key to check it against.
- **A QEMU build** (`clock/none`, no TLS test): reports no clock.

### The flow on the board

```
 screen or page                  board                                        GitHub
 ──────────────                  ─────                                        ──────
 Check for updates ───────────►  POST /firmware/check  (202)
                                   needs: network, time, a key, memory, not on trial,
                                          not in flash mode                      
                                   read hearth-update-<channel>.json  ────────►  index (302, CDN)
                                   pick this board's entry; compare version, sequence
 poll GET /firmware/available ◄─   up_to_date | available (version, size, date) | failed (why)
 Install v0.11.0-beta.1 … ─ confirm
                      ─────────►  PUT /firmware/from-release  "v0.11.0-beta.1"
                                   enter flash mode: playback, Sendspin, sink stopped
                                   GET <base>/<tag>/<file> ──────────────────►  image (302, CDN)
                                   first 288 bytes: refuse_image, version = the index's,
                                       newer, secure_version      (nothing erased until these pass)
                                   erase-as-you-go + write; running SHA-256 = the index's
                                   esp_ota_end: image checks + signature; read-back hash
                                   boot ota_N next, restart
 poll GET /firmware           ◄─   trial: 30 s hold within 5 min  (as for a push)
 "Updated to …" / "Went back to …, because …"   from last_update, which the trial already writes
```

What the board does to what is playing: the install stops it. A play started with `POST /play` ends; a
Sendspin server gets `client/goodbye` with reason `restart`; the sink closes; the board's mDNS service is
withdrawn. After the restart the server reconnects by itself within about a minute; the song does not resume. A push
to the C6 took 67 s start to accepted on 2026-09-25, so "about two minutes" is the figure the dialog gives.

**The states a person sees** (the screen and the page show the same set, from `GET /firmware/available`):

| State | Words | Buttons |
|---|---|---|
| Idle | "Version v0.10.0-beta.1-2039-gf5aded471. Channel: beta" | Check for updates |
| Checking | "Checking for updates..." | none |
| Up to date | "You have the newest version. Checked 14:02" | Check again |
| Available | "Version v0.11.0-beta.1 is available (1.6 MB, 5 Nov). Release notes: github.com/iainchesworthlabs/iclforge/releases/tag/v0.11.0-beta.1" | Install v0.11.0-beta.1 |
| Confirm | "Install v0.11.0-beta.1? The player stops playing, downloads it (about two minutes), and restarts. If the new version does not start properly, the player goes back to this one." | Install, Cancel |
| Installing | "Downloading 62%", then "Checking the download", then "Restarting" | none |
| Done | "Updated to v0.11.0-beta.1" or "Went back to v0.10.0-beta.1: the player did not find a network in time" | OK |
| Failed | The reason, in words ("The player does not know the date yet", "Could not reach GitHub", "This version has no image for this board", "Stop what is playing and try again", "The download did not match its checksum: nothing was changed") | Try again |
| Not available | A greyed row with its reason, for good: "This board has no network", "This board has one app slot; an update over the network needs a USB flash first", "This firmware carries no update key (a development build)", "This firmware has no room for a secure connection" | none |
| Not now | A greyed row with its reason, for the moment: "The player has no network address yet", "The player does not know the date yet", "Time sync is off", "This version is still being tried; wait 4 minutes, or go back to the previous one", "An update is already running", "Stop what is playing and try again" | none |

The words come from one place, `firmware_update.hpp` in `libs/device` (below), so the screen, the page,
`ota.py` and ac3hearth cannot drift; the panel code only draws them. An install's progress is
`GET /firmware`'s `upload` object with `stage: "downloading"`, so `ota.py status`, the page and ac3hearth's
Firmware tab show it with no change.

### Routes

| Route | What it does | Replies |
|---|---|---|
| `GET /firmware/available` | The update flow: `supported` and `why` (what is true of this board for good, so a greyed row has its reason), `blocked` (what stops a check or an install now, or null), `state` (`idle`, `checking`, `up_to_date`, `available`, `installing`, `failed`), `channel`, `running`, `checked` (seconds, or null), `latest` (version, released, size, notes link, image name, or null), `error` (code and text, or null), `skipped` (a version that went back, or null). Every key is always present | `200`, JSON |
| `POST /firmware/check` | Starts a check and returns; the result is on `GET /firmware/available` | `202`; `409` flash mode, on trial, an update running; `503` no memory beside what is playing |
| `PUT /firmware/from-release` | Body: the version, as text, which must equal what the last check offered. Enters flash mode, downloads, checks, writes, restarts | `202` then a restart; `403` the `Host` is not the board's; `409` nothing offered (check again), on trial or already running; `412` no key, no clock or no pull in this build; `503` no memory |
| `PUT /firmware/channel` | Body: `stable` or `beta`; stored | `200`; `400` |

The board's routes are for a board whose network is its own (see [Routes](#routes)). `PUT` is used for the
install, as for `PUT /firmware`, so that a page on another site cannot send it without a preflight, and
it carries the same `Host` check; `POST /firmware/check` needs none, since it changes nothing but one
HTTPS request. A build without pull answers the first route with `supported: false` and the others
with `404`, as it does for any route it lacks.

**The route word list is in four places** and a new route has to be in all of them or the
device-UI contract test fails: `control.cpp`'s `GET /api` text (near line 489) and its `400` text, and the
two copies in `apps/demos/wasm/tests/device-ui/stub.js` (near lines 60 and 191). The stub also needs the new
replies (`/firmware/available` for each state).

### The screen and the page

- **Preferences (the screen).** Added **after prefs-core's PR merges**; nothing in `lyric_screen.cpp`
  before. Its contract is `board_caps.hpp`, `board_variants.hpp` and `preferences.hpp` (branch
  `feature/hearth-preferences-core`, 6c47ca278): an entry is a `Pref` in `kRegistry`, whose availability is a
  function of `BoardCaps` and whose `inert` is a function of `Backend`, and a value is a word. Four entries in
  group Updates, and no bespoke button:

  | id | Kind, apply, route | Shows or does |
  |---|---|---|
  | `update-status` | `kText`, live | Read-only: the sentence `firmware_update.hpp` renders for the state: "Version v0.10.0-beta.1-2039-gf5aded471. You have the newest version. Checked 14:02", "Version v0.11.0-beta.1 is available (1.6 MB, 5 Nov)", "Downloading 62%", or the failure in words |
  | `update-channel` | `kChoice` (stable, beta), live, `/firmware/channel` | The channel |
  | `update-check` | `kAction`, live, `/firmware/check` | Check for updates |
  | `update-install` | `kAction`, restart, `/firmware/from-release` | Install the offered version; its label takes the version, and it needs a confirm |

  **Greyed for good** is `availability(caps)`, each a sentence about the board: no `caps.network.any()`;
  no `caps.wall_clock` ("keeps no time, and a secure connection needs the date"); no `caps.firmware_slots`;
  no `caps.update.pull`; `caps.update.trusted_keys == 0`. **Greyed for now** is `inert(backend)`, the text of
  `UpdateStatus::blocked`, which `firmware_update.hpp` works out from the board's facts at that moment (an
  address held, the clock set, time sync on, not on trial, not in flash mode, no update running, room for a
  session); `update-install` is also inert with "Check for updates first" until a check has offered a version.
  `GET /firmware/available`'s `supported`/`why` and `blocked` are the same two things for the page.

  **What this asks of the contract** (for prefs-core, through the coordinator), nothing else:
  1. `BoardCaps::update`, an `UpdateCaps` of two build facts, filled once at boot in `board_caps.cpp`:
     `bool pull` (the image has the HTTPS client, the bundle and the routes: `ICLFORGE_FIRMWARE_PULL`) and
     `int trusted_keys` (the signature blocks of the running image; 0 for a development build). The
     variants in `board_variants.hpp` get them: `pull` and one key on the shipping boards, none on the frame
     buffer bench build, and a new `c6-4mb` variant with `pull` false, so the greyed reason is tested. The
     other facts it needs are already there: `network`, `wall_clock`, `firmware_slots`.
  2. `Backend` gains `update_status()` (an `UpdateStatus` from `firmware_update.hpp`, so O10a lands before
     O10d), `start_update_check()`, `start_update_install()` and the channel's getter and setter.
  3. A `confirm` text on a `kAction` `Pref`, shown before `set` runs: the install's ("The player stops
     playing, downloads it (about two minutes), and restarts..."), and the same field serves the restart and
     the resets another session is building.
  4. While a check or an install runs the screen resolves the Updates group once a second, so that
     `update-status` follows the progress; no editor of its own is needed.

  The panel reaches the pull through a seam like `panel.hpp`, with a null implementation in a build with
  no screen (`panel/none` links no LVGL).
- **The device page.** A section under Firmware, beside Update firmware: the version, **Check for updates**,
  the result line, an **Install** button that opens a dialog like Restart's, the release-notes link
  (`target="_blank" rel="noopener"`), the channel choice, and the same progress. The page budget
  ([The web page (O3)](#the-web-page-o3): 57,344 bytes, 55,454 used) has 1,890 bytes left; the section
  needs more, so the budget is derived again, as each redesign did. The Playwright suite gets a stub
  reply for each state.
- **Boards with no screen** lose nothing: the page and the routes are the whole interface, and `ota.py` can
  call the same routes (`ota.py update --host H` is the check and install on one board).
- **ac3hearth** can show the same state on its Firmware tab later; it is not in these slices.

### Packaging and CI

What the build and the release must emit, in the order they run. **None of the workflow edits below is made
without telling the coordinator first**: `release.yml` and the release itself are in the owner's hands
(PR #1621), `wheels.yml` publishes to PyPI on a tag push, and a signing job in a release path is the owner's
to approve.

1. `sdkconfig.sign`, an overlay: `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y`,
   `CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y`, `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n` (the build never
   sees a key; the image is signed afterwards), and the pull options. Each profile that signs locks them in
   `profiles.json` (a profile change carries a board run, by that file's own rule).
   `tools/checks/check_esp_efuse_free.py` must go on passing: none of these burn an eFuse, and a test pins
   that the check tells `SECURE_SIGNED_APPS_NO_SECURE_BOOT` from `SECURE_BOOT`.
2. A new `sign_firmware.py` beside `ota.py`: a wrapper around `espsecure sign-data --version 2` (and
   `--append-signatures`) with `sign`, `verify` and `keys` (list a file's key digests), tested with
   throwaway keys.
3. `package_firmware.py`: `package --app-image <signed.bin>` swaps the signed app into the `.bin`, the
   factory image and `parts.zip` (a signed app in one and not the others would be a trap); the fragment
   gains `signed`, `signers` (key digests) and `secure_version`; a new `index` command writes
   `hearth-update-<channel>.json` from the merged manifest.
4. `check_firmware_package.py`: every `.bin` verifies against the committed public keys and carries the
   release block; the factory image and the zip hold the same signed app; the index agrees
   with the manifest (sizes, SHA-256, no image missing for a profile that publishes one); the signed image
   fits the smallest slot.
5. A `sign-esp32-firmware` job in the release path, in the `firmware-signing` environment, between
   `package-esp32-firmware` and the upload. `packages-esp32-firmware` is uploaded **only** signed: the
   release collects `packages-*`, so an unsigned copy uploaded under that name would be published.
6. A step in the release job that publishes the images, checks that they download, and then replaces the
   index assets of the `firmware-channels` release.
7. Docs: `docs/hearth/sink-firmware.md` (updating from the screen and the page), `docs/releasing.md` (the
   key ceremony, the secret and environment the owner sets, rotation, and what to do if a key is lost),
   CHANGELOG.

Nightly and pull-request runs build and package unsigned images as today, and publish no index.

### Slices

Each is a PR of its own, in this order. Nothing here touches a board until the coordinator gives a window.

| Slice | What | Tested by | Needs a board window |
|---|---|---|---|
| **O10a, signing, packaging and a host-tested library** | `firmware_update.hpp` in `libs/device` (version compare, index parse, image selection, the policy table, the state's JSON and words; no ESP-IDF); `firmware_image.hpp` reads `secure_version` and finds the signature sector; `firmware_status.hpp` gains `signing`; `sdkconfig.sign`; `sign_firmware.py`, `package_firmware.py` and `check_firmware_package.py` as above; `ota.py` refuses an unsigned file for a board that wants signatures | Host: Catch2 cases for every rule and a version table; Python suites for the tools. **QEMU** (S3): new steps in `run_ota_qemu.py` with throwaway keys: an unsigned image is accepted by an unsigned build and then the first signed one is taken; a wrong-key image, an unsigned image, and an image with one of two keys are handled as the rules say; a key rotation; what a signed-in-code, unsigned-in-fact image does (the `ESP_ERR_NOT_FOUND` case). C6 and P4 images build | Yes, last: the first signed image on one board of each chip (S3, C6, P4 rev1, the P4X kit), with the owner's go-ahead, to prove "no USB flash" |
| **O10b, the board's pull** | `firmware_pull.cpp` in the component next to `firmware.cpp` (the upload core takes chunks from either source), the four routes, Kconfig (`ICLFORGE_FIRMWARE_PULL`, `_UPDATE_BASE_URL`, `_UPDATE_CHANNEL`), the certificate bundle, the NVS keys, the four-place route word list, `stub.js` | Host: the policy and JSON from O10a. **QEMU:** a loopback HTTP server serves an index and a signed image; the base URL accepts `http://` only in `sdkconfig.ci-ota`, and the signature rules are not relaxed; TLS is not tested here (no clock) | Yes: the TLS handshake with the three real hosts, RAM and flash measured, on the S3, the C6 (16 MB) and the P4 or the kit; a real pull from a throwaway release in the repository |
| **O10c, the release pipeline** | Items 5 to 7 above, on a throwaway tag first | `release.yml` dry run; a signed image from the job verifies with the committed keys | No (the owner's environment and secret) |
| **O10d, Preferences and the page** | The four registry entries, `BoardCaps::update` and its variants, the `Backend` methods; the page's section; `ota.py update`; docs | The preview tool at several sizes with the null display; the Playwright suite with the stub | Yes: the kit's screen, once (including what the screen does while flash mode writes) |

**Exit of the whole:** a board on the desk checks, installs a signed release from GitHub, passes its
trial and reports "Updated to ...", and the same board refuses an unsigned image by `PUT /firmware`.
The exit waits for a signed release, so for the owner's key and a release run (O10c).

**Open, to prove rather than assume:**

- what `esp_ota_end` does in a running image that has the check but no signature block (O10a, QEMU);
- that `esp_http_client` follows the 901-byte redirect with a 2 KiB header buffer and a custom bundle (O10b);
- whether ISRG Root X1 alone validates the CDN's chain, which ends in "Root YR" cross-signed by X1 (O10b);
- the session's real peak on the C6 beside a stopped stream, and the code size on each chip (O10b);
- that the kit's screen survives flash mode's cache stalls well enough to show progress (O10d);
- that `ota.py --release latest` and `installer_site.py` ignore the `firmware-channels` release (O10c).

The P4's co-processor firmware is not part of this: a release updates the P4's application, and the C6
beside it stays as it is ([O6](#the-p4s-co-processor-o6)).

## Tests

**On the host.** The logic that needs no ESP-IDF goes in `libs/device/include/iclforge/firmware_image.hpp`,
the way `hardware_info.hpp` is kept free of it:

- parsing an image header;
- every pre-write refusal and its reply text;
- parsing `Content-Digest`, and the reply when a SHA-256 does not match;
- the `Host` check: IP addresses and the board's own names pass, anything else is refused;
- the trial's decision from what has held and for how long, including a network that drops and
  comes back, which restarts the 30 s;
- `GET /firmware`'s JSON.

`libs/device/tests/test_firmware_image.cpp` checks all of it from synthetic headers, including a P4 image
that needs v3.1 on a v1.3 chip, and `test_firmware_trial.cpp` and `test_firmware_status.cpp` cover
the trial and the status body. The settings rule has its test of old blobs.
`tools/hearth/test_ota.py` runs the tool against a stand-in board built on `http.server`: the
pre-flight refusals, a damaged file refused before sending, an update that is accepted, one that
rolls back, one that does not come back, and `--all` stopping at the first failure. Status,
2026-09-30: all built; the tool's suite runs in the "Oracle unit tests" step.

**Under QEMU** (S3, in the ESP32 job). Built as the step "Update the Sendspin player over the
network under QEMU" in `_build.yml`, which runs `tools/checks/run_ota_qemu.py`. The job builds
image A (`sdkconfig.ci-ota`, which shortens the trial to a 5 s hold and a 40 s deadline) and
image B from the same tree with different versions, and two more from the same directory with
`AC3FORGE_FIRMWARE_TEST` set. As built, the script's ten steps are:

1. the board boots A from `ota_0`, valid, with the other slot empty, and `GET /log` has the
   boot's lines without the Sendspin pairing token;
2. `ota.py` pushes B: B restarts, is accepted, and A is the image to go back to;
3. refusals, each leaving B running: a byte of the image changed (`400`), a `Content-Digest`
   that does not match (`400`), another chip's image (`400`, before anything is written), an
   upload cut short (`400`), a `Host` that is not the board's (`403`), and a play in flash mode
   (`409`);
4. `ota.py` pushes A back, and it is accepted;
5. an image built with `AC3FORGE_FIRMWARE_TEST=unhealthy`, which never reports healthy, rolls
   back at the deadline, and `last_update` says why;
6. one built with `AC3FORGE_FIRMWARE_TEST=panic`, which panics as its trial starts, is rolled
   back by the bootloader, and the panic's core dump is reported, fetched whole, read with
   `esp_coredump` where the ESP-IDF environment is there, and erased;
7. `PUT /firmware/rollback` with nothing to go back to answers `409`; then B is pushed, and
   `PUT /firmware/rollback` takes the board back to A;
8. `POST /restart` restarts into the image that ran;
9. the running slot's image damaged in the flash file between two boots: the bootloader boots
   the other slot;
10. `PUT /firmware/mode` `flash` and then `normal`: the board restarts.

The design's list had the same checks in another order and without the log, the core dump and
the flash-mode exit. `run_ota_qemu.py` starts QEMU again, twice, if it dies before the board
prints anything (#1056).

`AC3FORGE_FIRMWARE_TEST` is a CMake variable, not a Kconfig option, and so is `PROJECT_VER` for
image B. Each variant is then a rebuild of A's build directory that recompiles a file or two, so
the step costs one full build.

QEMU writes to the flash image it was given, and a new QEMU started on that file boots what was
written. It does not discard the code it has already translated from flash when the flash is
written, though: an image written over one that ran in the same QEMU runs the old image's code
after an `esp_restart()`, while reporting its own version. The test therefore starts a new QEMU on
the same flash file for each image an update writes, as a power cycle would. Rollbacks, panics
and `POST /restart` stay in one QEMU, which keeps the reset reason a panic leaves.

**On boards** (O2): each exit in [Phases](#phases), on each chip.

## Phases

Status, 2026-09-30, at a glance: O1 to O5, O8 and O9 built and merged; O2 passed on all four
boards; O6 studied only; O7 not built. The robustness fixes of 2026-09-26 (#1053) are merged.

Nothing reaches a board until O1 has merged in full, because each board gets one USB flash and
that image has to be able to take the next update.

- **O1, in the repository only.** **Status: built and merged**, (a) as #1019 and (b) to (d) as
  #1026, both on 2026-09-25. Split into PRs that merge before any board migrates:
  - (a) the layouts, the rollback bootloader, `sdkconfig.flash16mb` and
    `check_esp_efuse_free.py`, with CI's QEMU shapes passing on the new table and the README's
    offsets updated;
  - (b) `ac3forge::Firmware` beside `Control` in the component, flash mode through the example's
    hooks, the integrity checks, the `Host` check, the trial, a built-in network stored in NVS at
    first boot, and the host tests;
  - (c) the QEMU end-to-end tests;
  - (d) `ota.py`, its tests and `idf.py ota`.

  **Exit:** CI green, the QEMU job included; no board touched.
- **O2, boards.** **Status: done, on 2026-09-25.** One USB migration flash each: S3 COM15 and
  COM16, C6 COM9, and P4 COM10. **Exit, on each chip:**
  - name, network and pairings survive the migration (for example "1 pairing record(s)" at boot,
    and the name in `/status`);
  - a build pushed over Wi-Fi is accepted, and a Sendspin server reconnects by itself;
  - a damaged image is refused, and the board keeps running what it had;
  - an image that panics at boot comes back on the previous version by itself;
  - a power cut during a trial comes back on the previous version;
  - `PUT /firmware/rollback` works;
  - an update of a playing board stops the play and says goodbye to its server;
  - the time each step takes;
  - the C6's internal heap low-water mark during an upload (114,308 bytes, read with O4's line);
  - on the P4, a v3.1 image is refused before anything is written.

  Passed on all four boards on 2026-09-25. The power cut was made by hand on the C6: an image
  built with a 600 s hold (`CONFIG_AC3FORGE_FIRMWARE_TRIAL_HOLD_S`), so that there was time to
  pull the plug, lost its power 15 s into its trial. The board was back 8 s later on the image
  before it, with the reason "the power went off before it had proved itself". The P4's run
  needed its power cycled part-way ([The P4's co-processor](#the-p4s-co-processor-o6)).
- **O3.** The page's Firmware section ([built](#the-web-page-o3)). **Status: built and merged**
  as #1035.
- **O4, diagnostics without a cable.** Core dumps to the `coredump` partition, fetched with
  `GET /firmware/coredump` and read with `idf.py coredump-info`. Also a ring of recent console
  lines at `GET /log`, since flashing without a cable also means reading the console without one.
  [Built](#diagnostics-without-a-cable-o4). **Status: built and merged** as #1036.
- **O5.** The firmware panel in `ac3hearth`. [Built](#ac3hearth-o5). **Status: built and merged**
  as #1039.
- **O6.** The P4's co-processor firmware, as a study first ([decision 9](#decisions)).
  [Studied](#the-p4s-co-processor-o6); what comes next needs someone at the desk with a USB-UART
  adapter on the C6's pads. **Status: studied only**, in #1042; nothing after the study is built.
- **O7, when the boards leave development.** Signed images, switched on over the network
  ([Signing, later](#signing-later)). **Status: not built.**

The last two phases come once the ones above are done. O7 has no fixed place: it happens when the
boards leave development, and if that is before O8, O8's images are published signed.

- **O8, published images.** Everything in [Published images](#published-images):
  - the four images built on every run of the ESP lane and kept for 14 days;
  - the same images published with each release;
  - `check_firmware_package.py`;
  - `ota.py push --run` and `--release`.

  [Built](#published-images); its exits wait for a release (or `release.yml`'s dry run) and for a
  blank board. **Status: built and merged** as #1040; no release has published the images yet.

  **Exit:**
  - `ota.py push --release` puts a release's images on each board on the desk over the network,
    each board getting the image that fits it;
  - a factory image installs a blank board;
  - a dry run of `release.yml` finds every image it is documented to publish.
- **O9, the user guide.** [The user guide](#the-user-guide), the browser installer if decision 15
  takes it, and the other pages brought into line. **Exit:** someone with only the guide takes a
  blank board to one that plays, then updates it over the network to a newer release.
  [Built](#the-user-guide); its exit waits for a release that publishes sink firmware.
  **Status: built and merged** as #1041.
- **O10, updates from GitHub releases.** [Updates from GitHub releases](#updates-from-github-releases-o10):
  a board checks for a newer release, shows it, and installs it, signed. Four slices, each its own PR:
  O10a signing, packaging and a host-tested library; O10b the board's pull and its routes; O10c the release
  pipeline; O10d the Preferences entry and the page. **Status: designed 2026-10-25;** the owner's decisions
  23 to 33 come first. **Exit:** a board on the desk checks, installs a signed release from GitHub, passes
  its trial, and refuses an unsigned image sent by `PUT /firmware`.

## Robustness (2026-09-26)

**Status, 2026-09-30: built and merged** as #1053, with #1048 (a sink given its network over
Improv restarts to start its player) and #1056 (QEMU started again when it dies before the board
prints). The fixes named below are in the tree; the one item under "Still open" is still open.

**A soak on the boards.** Each board was updated over and over through the night. Each cycle
was a push with `ota.py`, as a user makes one, then one fault injected by hand:
- a push straight after a restart (the P4's broken upload of O2 was one);
- uploads cut off after the header, early, at random and at 99%;
- one at 16 KiB/s, and one that stalls for 20 s halfway;
- a second `ota.py push` during an upload;
- one sent while three other clients poll `GET /firmware` on kept-alive connections;
- `PUT /firmware/rollback`;
- a damaged image, and another chip's image.

After every step the soak read `GET /firmware`, kept any core dump and saved `GET /log`; the
S3s' and the C6's consoles were recorded throughout. The images were the stack as it goes to
`main` with #1034's trial fix, before the fixes below.

- **Result:** every kind of step went as designed on every board but one: an upload sent while
  three other clients poll, which the board's HTTP server sometimes reset (below). None of the
  faults left a board stuck, and none needed a power cycle. The P4 came through dozens of
  restarts without its co-processor hang.
- **A second upload during one** is refused by `ota.py` before it sends anything ("an update is
  already under way"), from `GET /firmware`. A client that skips that check, and a raw second
  `PUT`, get their connection dropped rather than a `409`: the board's HTTP server cannot answer
  before the body, and the client is still sending it.
- **An upload sent while three other clients poll** was sometimes reset before the board read
  any of it: `ConnectionResetError` after 69,632 bytes, nothing in the board's log, and the board
  as it was. The P4 lost 2 of its 3 tries at that step, an S3 1 of 4, and the C6 none of 3. It is
  a race in ESP-IDF v6.1's HTTP server:
  - The control server keeps three sockets. For a fourth connection it queues a close of the
    least recently used, and takes the new connection once that close has run.
  - A server that goes round again before the close has run queues a second close of the same
    slot. The first frees the slot, the new connection takes it, and the second closes the new
    connection.
  - `httpd_sess_close()` has a check meant to skip such a close. It looks for a session that has
    never been used, and v6.1 starts every new session at the server's current count, so it
    never skips.
  - An upload is the connection most likely to lose: its own burst of data keeps lwIP busy, which
    holds up the queued close. Once the upload has begun, the server no longer purges its socket.

  On the S3 board, 300 connections made as that step makes them, with a 64 KiB body the board
  refuses with `415`, lost 16 to a reset. With `CONFIG_HTTPD_QUEUE_WORK_BLOCKING`, which makes the
  server close the least recently used connection at once with nothing queued, 300 lost none, and
  the Sendspin player still started and took Music Assistant's connection. The example now sets
  it (`sdkconfig.defaults`). The cost is that `httpd_queue_work()` waits for room in a full queue
  rather than failing. `ota.py` and ac3hearth also send an upload that breaks off once more.

**A review of the code.** It ran the same night, and found the cases these fixes answer:
- a reset during an upload leaving no trace (the P4's, most likely `esp_hosted` restarting it),
  now the `interrupted` record;
- the whole slot erased before the second read, now as the writes go;
- no overall deadline, so a trickling client could keep a board busy until its power was cut,
  now ten minutes;
- the upload's task made before the teardown, now after it, answering `503` when it cannot be
  made;
- the example's flash-mode hook able to wait forever, now 15 s;
- the tools leaving a board in flash mode after a failed push, now told to leave, and a broken
  upload sent again;
- the trial's acceptance needing a new 4 KiB task, which a heavy stream started during the S3's
  hold could leave no room for, now done from `esp_timer`'s task;
- a rollback asked for at the very end of a trial's hold racing its acceptance, now refused
  while the acceptance is written;
- ac3hearth's Firmware tab losing its sink when flash mode withdrew the board's mDNS record and
  ended its Sendspin connection, so the progress and the outcome went unseen. The sink is now
  kept while the update runs and while the tab shows how it ended.

Tested on the S3 board: a reset over USB 5.1 s into an upload produced the `interrupted` record,
`ota.py` said so, sent the image again, and it was accepted.

**Still open**, from the same review: the NVS record is three writes, which a reset can tear.
That needs power lost during those writes, and it can only leave a report that mixes two
updates: NVS decides nothing about which image boots.

**Tested and not reproduced:** polls during an S3's boot splitting the internal RAM its player's
32 KiB block needs. Three updates to the S3 board were each pushed while 4 or 8 clients polled
`GET /firmware`, `/status` and `/hardware` on new connections every 50 to 100 ms, from before the
restart until after the trial: 1,000 to 2,400 requests each. Every one started the player with
the heap of a quiet boot (largest free block 31,744) and was accepted. That build keeps lwIP's
buffers in PSRAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`).

## What cannot be verified

- **Wi-Fi under CI.** QEMU has no Wi-Fi, so CI's uploads go over the emulated Ethernet. Wi-Fi
  uploads are tested on boards only.
- **The C6 and the P4 in CI.** Neither has a QEMU machine, so CI builds them and cannot run them.
- **Power lost at an exact instant**, such as during the write of `otadata`. The protection there
  is ESP-IDF's: `otadata` keeps two sectors, each with a sequence number and a CRC. Power can be
  pulled by hand during an upload and during a trial, but not at a chosen microsecond.
- **Every way an image can fail after it is accepted.** The trial covers 30 s of the board being
  healthy. A fault that takes longer to show is found in use, and rolled back by hand.
- **Boards other than the ones on the desk.** A published image is checked on one board of its
  kind. Another module with the same chip, flash and PSRAM should run it; one that differs in any
  of them is what the guide's "Which image" section and `ota.py`'s choice exist to catch.
- **The browser installer in every browser.** ESP Web Tools lists Chrome, Edge and Firefox as
  having Web Serial; O9 tries it in each. There is no Web Serial in Safari or on iOS, and the
  guide's `esptool` commands are the way in there.

## Decisions

Outcomes, 2026-09-30. Decisions 1, 3 to 8 and 10 to 21 were built as recommended, the recommended
(a) each time; decision 6 is a decision not to build something, and it stands. Decision 2 was
taken as (c) while the boards are in development, and its (a) is O7, which is not built.
Decision 9 was taken as (a), and the study is all of it that exists. Decision 22 is open: nothing
pins `esp_hosted`. Decisions 23 to 33 are the proposals of O10 (2026-10-25); none is taken.

1. **Update scheme.** (a) **two slots and a rollback bootloader**; (b) a small factory recovery
   app plus one update slot; (c) one slot and a staging area. **Recommend (a).** It is the only
   one of the three that always leaves a whole, working image on the board. (b) needs a second
   application to write and keep working, and (c) copies over the only image. Cost: two full
   slots of flash, easy on 16 MB and 1.75 MiB each on a 4 MB C6.
2. **What an update must prove about itself.** (a) **signed with the user's key: RSA-3072, no
   eFuses, the running image's key as the anchor**; (b) a password for the upload route, set when
   the board is provisioned; (c) nothing, the network is the boundary as it is for the rest of
   the API. **Recommend (a).** It is the only one that stops someone else on the network running
   their own code on the board. A password crosses the network in the clear and sits in every
   tool and page that uses it. Cost: a private key to keep safe and back up, since losing it
   means a USB flash per board; every board build needs the key; and images built by CI cannot
   be pushed to the boards on the desk. **Taken 2026-09-24: (c) while the boards are in
   development, with every image checked for damage end to end ([Integrity](#integrity)).** The
   user's words: "anyone on the wifi can flash these boards (since they support usb flashing too)
   but we should have a mechanism to validate the firmware is not corrupted either in flash or on
   transfer to flash". (a) becomes O7, switched on over the network when the boards leave
   development.
3. **Push or pull.** (a) **push: the tool, the page or curl sends the image with a PUT**; (b)
   pull: the board fetches a URL (`esp_https_ota`); (c) both. **Recommend (a).** It needs no
   server, no certificates and no TLS memory on the C6. Pull can come later for a fleet, or for
   updates from a release server. Cost: every update starts from a machine on the same network.
4. **How flash mode ends.** (a) **always with a restart**; (b) a resume in place when an update
   is cancelled or refused. **Recommend (a),** for the reasons in [Flash mode](#flash-mode).
   Cost: a board rejoins its network after a cancelled or refused update.
5. **When a new image is accepted.** (a) **the board decides: an address, the HTTP server and
   the Sendspin player, held for 30 s, within 5 minutes**; (b) the tool must confirm it
   (`PUT /firmware/accept`) within the deadline; (c) at once. **Recommend (a).** Unlike (b), it
   does not roll back a good image because a laptop went to sleep or the page was closed; unlike
   (c), it catches an image that crashes at boot or cannot rejoin the network. Cost: a Wi-Fi
   outage during those 5 minutes rolls back a good image, which then has to be pushed again.
   **Taken 2026-09-24: (a).**
6. **The bootloader and the partition table over the network.** (a) **never**; (b) with
   ESP-IDF's staging copy. **Recommend (a).** Neither has a second copy, so a power cut during
   the copy leaves nothing that boots. Cost: those changes need one USB flash per board.
7. **Which table the 16 MB C6 uses.** (a) **the 16 MB table through `sdkconfig.flash16mb`**; (b)
   the 4 MB table on every C6. **Recommend (a).** Slots of 4 MiB, against a 1.75 MiB slot that was
   86% full on 2026-09-24. Cost: one more overlay line in the C6's board recipe.
8. **Partitions reserved before migrating.** (a) **`coredump` (64 KiB) on both tables, and a
   4 MiB `reserve` on the 16 MB table**; (b) only what O1 uses. **Recommend (a).** The table is
   USB-only after the migration. Cost: flash nothing uses yet, on boards with room to spare.
9. **The P4's co-processor firmware.** (a) **a later phase of its own (O6)**; (b) part of this
   work. **Recommend (a).** If its update fails, the P4 has no network at all, and on this board
   the co-processor may have no USB path to recover it. That risk needs its own study of
   `esp_hosted`'s example. Cost: the version mismatch in the P4's boot log stays for now.
   **Taken 2026-09-24: (a).** If O6 finds it needs a partition to stage the co-processor's image
   in, `reserve` is sized for that, so O6 needs no USB flash of its own.
10. **The tool.** (a) **Python under `tools/hearth/`, plus `idf.py ota`**; (b) a command in
    `ac3cli`; (c) only the desktop app. **Recommend (a).** Board builds already run in the
    ESP-IDF Python environment, and `idf.py ota` fits the recipe in use. Cost: a second client
    of the routes to keep in step, beside the page.
11. **Several boards.** (a) **one at a time, stopping at the first rollback or silence**; (b) in
    parallel. **Recommend (a).** The first board is the test of the image for the rest. Cost: a
    minute or so per board, most of it the trial.
12. **Where the code lives.** (a) **the component (`firmware.cpp` beside `control.cpp`), with the
    example supplying flash mode's teardown and the trial's health checks as hooks**; (b) the
    example only. **Recommend (a).** Control's routes are the component's
    ([device UI plan, decision 5](esp32-device-ui.md#decisions)), and an integrator's firmware
    gets the same updates. Cost: hooks in `ControlHandlers` for what only the owner knows.
13. **The `Host` header on the firmware PUTs, while images are unsigned.** (a) **accept only an IP
    address or the board's own mDNS name, bare or with `.local`**; (b) accept any `Host`, as every
    other route does. **Recommend (a).** Without signing, it is what stops a web page on the
    internet from flashing a board through the viewer's browser by DNS rebinding
    ([Routes](#routes)). Cost: the page's firmware upload works only when the page was opened by
    IP address or `.local` name. O7 makes the check unnecessary, and it can then go.
14. **Which images CI publishes.** (a) **the four in [Published images](#published-images): the
    S3, the C6 at 4 MB and at 16 MB, and the P4 of revision v1.x**; (b) those four and a P4 image
    for revision v3.x as well. **Recommend (a).** Each of the four runs on a board on the desk and
    is checked there in O8. A v3.x image would be published before anything had run it. Cost: a
    v3.x P4 has no published image until someone has such a board.
15. **How a new board gets its first image.** (a) **a browser installer on the documentation
    site (ESP Web Tools), with the `esptool` commands beside it**; (b) the `esptool` commands
    only. **Recommend (a).** A first install then needs only a browser and a cable, and it ends
    in Improv, which gives the board its network in the same few minutes. The same page moves a
    board in use to the new layout without erasing it, and recovers one held in download mode.
    Cost: a third-party script on one page of the site; a browser with Web Serial (not Safari or
    iOS); and `docs.yml` copying each release's images into the site, because a page cannot fetch
    release assets directly.
16. **Which boards keep a core dump** ([Diagnostics](#diagnostics-without-a-cable-o4)). (a)
    **every build, except the S3 board and the widest CI shape, where internal SRAM is what runs
    out**; (b) every build; (c) none, and a crash read over USB. **Recommend (a).** The C6 and
    the P4 have internal SRAM to spare, and the S3 board has none: 4,016 bytes, against a JOC
    stream that left 43. Cost: a crash on the S3 board says it panicked and no more; reading
    one takes a USB cable and ESP-IDF's monitor.
17. **How the console reaches the ring.** (a) **`stdout` and `stderr` reopened on a device of
    the component's own, which writes on to `/dev/console`**; (b) `esp_log_set_vprintf`; (c)
    ESP-IDF's ROM output channel (`esp_rom_install_channel_putc`). **Recommend (a).** The
    example's lines, the ones that say what an update or a trial is doing, are `printf`, which
    (b) never sees; (c) runs from interrupts and with the cache off, where the ring's lock
    cannot be taken, and it sees only ROM output. Cost: a device registered with ESP-IDF's VFS,
    one more step on every console write, and nothing kept from before `app_main`.
18. **The pairing token and the log.** (a) **a scope, `ConsoleOnly`, around the few writes
    that are the console's alone**; (b) filter the ring for known secrets; (c) no ring on a
    Sendspin board. **Recommend (a).** The code that prints the token knows it is one; a
    filter would have to recognise every secret that might ever be printed. Cost: a new line
    that ought to stay off the network has to be written inside the scope, and a line some
    other task prints is kept.
19. **Where the firmware goes in ac3hearth** ([ac3hearth](#ac3hearth-o5)). (a) **a third tab on a
    paired Hearth sink's settings page, beside Speakers and Decoder**; (b) a card in that page's
    right column; (c) a dialog opened from the "Only on the sink" panel. **Recommend (a).** An
    update needs room for its progress and its outcome, and the right column is a third of the
    page. Cost: the app updates only a sink it has paired. An unpaired board is updated from its
    own page or with `ota.py`.
20. **"The build the app knows of"** ([ac3hearth](#ac3hearth-o5)). (a) **this app's own version,
    `git describe` of the tree it was built from, compared with the version the board reports**;
    (b) nothing until O8 publishes images the app could list. **Recommend (a).** The board's
    version is ESP-IDF's `git describe` of the same tree, so one build of both reads the same.
    Cost: builds of one commit whose describe differs, say one with uncommitted changes, do not
    match; and the board keeps 31 characters, so the comparison is on as many.
21. **How ac3hearth reaches the board** ([ac3hearth](#ac3hearth-o5)). (a) **the board's HTTP
    routes, from a thread for each sink**; (b) a new command in Sendspin's `_ac3forge_player@v1`
    role. **Recommend (a).** The routes exist, the board's page and `ota.py` use them and CI
    tests them, and an update takes the sink off Sendspin while it runs. Cost: a second
    connection to the sink, and the same boundary as the page: anyone on the network can
    update a board while images are unsigned.
22. **Which `esp_hosted` the P4 builds with** ([The P4's co-processor](#the-p4s-co-processor-o6)).
    (a) **`esp_hosted` and `esp_wifi_remote` each at one exact version in the example's
    manifest, with the co-processor's image built from the same `esp_hosted`**; (b) "~2" and
    ">=0.10,<2.0", as now. **Recommend (a).** The lock file is not committed, so two builds of
    one commit, a day apart, can carry different releases of both. `esp_hosted` warns when the
    host and the co-processor differ, and a new release on either side should be a choice, not
    what a clean build happened to fetch. Cost: each new release is taken by hand, and once O6
    ships, a new `esp_hosted` means a co-processor update as well.

The next eleven are the proposals of [O10](#updates-from-github-releases-o10), 2026-10-25. None is taken.

23. **Where a board learns what the newest release is.** (a) GitHub's REST API; (b) **an update index, one
    small JSON file for each channel, published as an asset of a long-lived `firmware-channels`
    release**; (c) the same index on GitHub Pages; (d) no pull: a machine on the LAN (ac3hearth,
    `ota.py`, the page) reads GitHub and pushes. **Recommend (b).** (a) is 60 calls an hour for the whole
    household, `.../releases/latest` answers `404` while every release is a Pre-release, and one
    release's listing is 263 KB. (c) has one host, no redirect and CORS, but the index changes only when
    the docs build runs (a failed docs build delays an update), and a Pages address does not follow a
    repository rename where release addresses do. (d) needs no TLS on any board and is the way for a board
    that cannot hold it, but it is not the board looking at GitHub, and it needs a machine awake. Cost of
    (b): a mutable release whose assets the release job replaces, a redirect to a second host (two TLS
    sessions, two roots), and an edit to the release workflow, which is the owner's.
24. **How a board proves an image is the owner's.** (a) **signed apps: RSA-3072, ESP-IDF's own, the keys of
    the running image as the anchor (O7)**; (b) a detached ECDSA P-256 signature over the index, checked
    by code of ours; (c) both; (d) none, TLS and the index's SHA-256 only. **Recommend (a).** One mechanism
    covers push and pull, the page and every tool, with no cryptography of ours to audit. Cost: 4 KiB on
    every image; an image with the check and no signature block takes no update, so every image is signed
    (development ones with the development key); the first signed image is a window in which a hostile
    machine could strand the updates; an unsigned image is refused only after all of it has arrived, which
    the tools avoid by asking the board what it trusts first.
25. **Keys.** (a) **one release key, a secret in a CI environment with a required reviewer and two offline
    copies, plus a development key on the owner's desk for development boards**; (b) the same release key in
    CI with no reviewer; (c) a release key in CI and a second, recovery key held offline whose block the owner
    adds by hand to every release; (d) one key on the owner's machine, signing by hand at each release,
    nothing secret in GitHub. **Recommend (a).** The release stops at a button only the owner presses, and
    nothing else in the repository can read the key. Cost: the owner's click at each release; two offline
    copies to keep, and if every copy is lost a USB flash for every board; a rotation that takes two
    releases. (b) lets any workflow run on the tag sign. (c) makes a lost key a network update, but the
    owner's machine and a manual stage then sit in the middle of every release, which is (d)'s cost with
    (a)'s secret as well. (d) leaves the unsigned images waiting in the job for the owner to sign them.
26. **What an update may do to the version.** (a) **a pull installs only a strictly newer version, with
    `secure_version` and the index's `sequence` no lower than the board's; a push needs only
    `secure_version` no lower; a rollback is unchecked**; (b) any signed image. **Recommend (a).** A
    signature says who approved an image, not that it is the newest, and a replayed old release with a
    known flaw is the downgrade attack. Cost: a release that fixes a security flaw has `secure_version`
    raised by hand; a version the parser does not understand is never offered; a board that has seen a
    newer index refuses an older one until it is pushed to.
27. **Channels.** (a) **a setting, `stable` or `beta`, with Kconfig's default `beta` until a stable release
    exists**; (b) stable only; (c) beta only. **Recommend (a).** Every release so far is a Pre-release, so
    (b) would say "up to date" for ever. Cost: one more row in Preferences, and a flip of the default in the
    first stable release.
28. **A signed board and an unsigned `PUT /firmware`.** (a) **refused**; (b) accepted from the LAN as today,
    with a switch. **Recommend (a),** since a hostile page or machine on the LAN is the case signing is for.
    Cost: a board the owner develops on is made a development board first, and a development build is
    refused by a board nobody made one.
29. **What a check and an install do to what is playing.** (a) **a check never stops anything and is
    refused where memory does not allow it; an install stops playback, after a confirm that says so**; (b)
    both stop playback; (c) an install waits until the stream ends. **Recommend (a).** Cost: on the C6 a
    check also needs the stream stopped and says so; (c) can wait for ever on a player that plays all day.
30. **Automatic checks.** (a) **none to begin with: a person presses Check; later, an opt-in daily check that
    only changes the screen to "available" and never installs**; (b) a daily check on by default; (c) an
    install by itself. **Recommend (a).** Cost: a board nobody checks stays old. (c) restarts a player in
    the middle of an evening.
31. **Trust for the connection.** (a) **a small custom bundle (USERTrust ECC, ISRG Root X1 and one spare),
    validation on, the date required, no fallback**; (b) ESP-IDF's full default bundle; (c) no validation,
    because the code is signed. **Recommend (a).** Cost: if GitHub changes a host's root before the bundle
    covers it, boards cannot pull until pushed to; (b) is tens of KB on every board for hosts it never
    contacts; (c) leaves the index and the image readable and replaceable by anyone on the path, with the
    signature as the only guard.
32. **Whose repository and key.** (a) **the base URL, the channel names and the trusted keys are an
    integrator's build settings; the component ships none of the owner's, and the example's defaults name
    this repository**; (b) compiled into the component. **Recommend (a).** The product is a generic base
    ([decision 12](#decisions)). Cost: an integrator sets three things and runs their own signing job.
33. **The 4 MB C6.** (a) **signed push only, no pull**; (b) trim the build until TLS fits its slot (1,966,080
    bytes, the profile's budget to the byte); (c) give its slots back the room `audio` and `storage` took
    on 2026-10-21. **Recommend (a).** Its profile has no board on the desk, and (b) and (c) are work for a
    board nobody runs. Cost: `hearth-sink-esp32c6` is updated from a laptop or the page.
