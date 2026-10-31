# Firmware over the network for Hearth sinks

**Status, 2026-09-30:** O1 to O5, O8 and O9 are built and merged, with the fixes that followed
them. O1 merged as #1019 and #1026 on 2026-09-25. #1034, #1035, #1038 and #1056 followed on
2026-09-25 and #1036, #1039 to #1042, #1048 and #1053 on 2026-09-26 (UTC). All four boards are
on the two-slot layout and passed O2's checks on 2026-09-25. O6, the P4's co-processor
firmware, is a study: nothing that updates the co-processor exists. O7, signed images, is not
built, by the user's decision. The exits of O8 and O9 wait for a release that carries sink
firmware, and none has been cut since v0.10.0-beta.1 on 2026-09-01 ([Phases](#phases)). O10, updates
pulled from GitHub releases with O7's signing brought into it, is designed (2026-10-25) and waits for the
owner's decisions ([Updates from GitHub releases](#updates-from-github-releases-o10)). The owner took
some of them on 2026-10-25 and changed the signing: the board verifies a signed index when it pulls, and a
pushed image needs no signature.

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

**2026-10-25: this design is not the one that will be built.** [O10](#updates-from-github-releases-o10)
replaces it, on the owner's direction. ESP-IDF's signed apps check every image written through `esp_ota_*`,
take their trust from the image that is running and have no setting to relax, and an image that has the
check and no signature block can take no update. That would end `PUT /firmware` of an unsigned development
build, which the owner will not give up ("I don't want to lock myself out"). O10 signs the update index
instead, checks it only when a board pulls, holds the anchor in the build where a person can replace it, and
leaves a push alone.

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
- `build-esp32c3` builds one more P4 image, the release profile with the stage timers on, to hold that the
  zone markers compile. It is not packaged. Since 2026-10-29 the P4 (v1.x and v3.x) and S3 release images carry the AC-4 decoder
  (`sdkconfig.ac4`, phase D14b of [`ac4.md`](ac4.md); [`ac4-in-release-images.md`](ac4-in-release-images.md)), so they
  decode AC-3, E-AC-3 and AC-4; the C6's still decodes AC-3 and E-AC-3 only.
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

**Status, 2026-10-25: designed, nothing built.** The owner decided most of it the same day:
[decisions 23 to 30, 33, 34 and 35](#decisions) are taken (24, 25 and 28 as the owner reshaped them), and
36's leaf lifetime and 37; 31, 32 and the rest of 36 wait. This section takes up the two things the plan put
off: **pull** ([decision 3](#decisions): "pull can come later, for updates from a release server") and
**signing**, which it makes a *process* around a certificate the owner already has, in place of the
mechanism O7 designed ([Signing, later](#signing-later)). O8 and O9 are the published images and the guide,
both built, so this is O10. It was written after the rename, so Kconfig names are `ICLFORGE_*` and the
repository is `iainchesworthlabs/iclforge`; the rest of this file keeps the names of its time.

The owner, 2026-10-25: "There is probably a check-for-firmware option in the preferences ... We have to
extend our existing firmware thing to go look at GitHub for the latest release, and if there's a later
release, upload it from GitHub and make sure it's signed." It has to work for every board variant, the
boards with no screen included (through the device web page and the REST routes). Then, on the design:
"I don't want to lock myself out. I don't want to make this really difficult to do dev. I want to be able to
upload dev firmware from my local development onto my dev kits. But if it's being pulled from an image on
GitHub, you probably want it signed." "We make it quite complicated if we mandate signing, and I'm not sure
we do that at the moment." The signature may come from the owner's own code-signing certificate and local
CA, the firmware carries that CA as its trust anchor, and a person must be able to replace the anchor with
their own and to upload their own unsigned firmware: "We've got to have a whole process around this."

### Principles

1. **Never lock a person out.** A board can always take an image pushed to it from the machine in front of
   it, signed or not, and USB always recovers it. No eFuse is burned, and nothing here is a one-way switch.
   A policy that could shut a person out is a setting, and the way back from it is at the board.
2. **Development stays easy.** `PUT /firmware` and `ota.py push` of an unsigned build go on working exactly
   as built, on every board, with no key on the desk and no step added.
3. **What a board fetches for itself must verify.** An image that arrives by the board's own pull, from a
   place that is not in the room, is installed only after the board has checked a signature that chains to
   a trust anchor it holds. No flag in the data it fetched can turn that off.
4. **The anchor is the owner's to replace.** The firmware ships an anchor, a person can replace it with
   their own CA or key, and a third party can run a release channel of their own with no help from this
   project.
5. **No revocation list, no clock we cannot trust.** Keys are rolled over by an update and by floors that a
   signed index raises, and a board with a wrong clock fails closed and says so.

**Who this protects, said plainly.** A machine on the board's network can push an unsigned image today,
by the owner's choice (the network is the boundary, as [Integrity](#integrity) says), and that stays. So
signing here defends the *pull*: GitHub's side, the CDN and the path between them and the board. It does
not defend a board against its own LAN. The setting that would ([policy `all`](#later-hardening)) is built
later, if the owner wants it, and is off.

**What does not change.** The A/B slots, flash mode, the trial, rollback, the integrity chain and
`PUT /firmware` stay as built. A pull is a second *source* for the same upload core: from the first byte of
the image on, a board that pulls does what a board that is pushed to does. What is new is where the bytes
come from (an HTTPS client), how the board learns there are new ones (a signed index), and what it demands
of them (that the index verifies).

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
| No board speaks TLS today | A search of `firmware/`: no certificate bundle, no `esp_tls`, no `esp_https_ota`. `esp_http_client` is used for the plain-HTTP stream source only (`main/source/http/byte_source.cpp`); mbedTLS is linked for SHA-256 | TLS is new code, flash and RAM on every board. Its X.509 and RSA/ECDSA code is the same code a certificate check on the signature needs |
| A board keeps the time with SNTP | `main/clock/sntp/wall_clock.cpp`: `wall_clock_read()` gives no time until a server has answered, and none before 2026. QEMU builds `clock/none` | A certificate has dates (the CDN's leaf starts 2026-09-30), so a board with no time cannot check one. A QEMU build cannot take a TLS test |
| The P4's TCP/IP stack is on the P4 | `sdkconfig.p4`: lwIP is configured on the P4; the C6 co-processor carries 802.11 frames over SDIO (`esp_wifi_remote`, `esp_hosted`) | TLS ends on the P4, which has PSRAM. The co-processor sees ciphertext and gets no TLS code and no new RAM cost |
| The host tool already reads GitHub | `ota.py --release` reads `releases?per_page=30` with the standard library | It works from a laptop. It is not a model for a board |

So "TLS memory on the C6" (decision 3's wording) is true of the C6 board, which has its own radio
and 512 KB with no PSRAM, and is not true of the P4's C6, whose image stays as it is
([The P4's co-processor](#the-p4s-co-processor-o6)).

### What the board reads, and where it is

The board never reads GitHub's API. For its channel it reads two small files, the **update index** and the
**signature** over it: `hearth-update-<channel>.json` and `hearth-update-<channel>.sig.json`. They are assets of one
long-lived release, tagged `firmware-channels` and flagged Pre-release so that it can never be "latest" and
never confuses `ota.py --release latest` or `installer_site.py`, which pick a release by the manifest it
carries (a check in slice O10c). The board's URL for the index is
`https://github.com/iainchesworthlabs/iclforge/releases/download/firmware-channels/hearth-update-beta.json`.
The images stay where O8 puts them, as assets of the release's own tag. Two channels exist and both are
published, `stable` (the newest release that is not a Pre-release) and `beta` (the newest of any kind).

```
{
  "format": 1,
  "channel": "beta",
  "sequence": 1793000000,                  unix seconds when CI signed it; a board keeps the highest
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

```
{                                           hearth-update-beta.sig.json
  "format": 1,
  "alg": "ecdsa-p256-sha256",              or "rsa-pkcs1-sha256", "rsa-pss-sha256"
  "sha256": "<64 hex of the index file, for a person comparing by eye>",
  "chain": ["<base64 DER of the leaf>", "<base64 DER of the sub-CA>"],   the root is never sent
  "signature": "<base64 of the signature over the index file's exact bytes>"
}
```

The index is about 330 bytes an image and 2 KB in all; the signature file 1.5 to 4 KB, depending on the key type
and the chain. The board reads at most 4 KiB of index and 8 KiB of signature.

- **Files, not URLs.** The board builds `<releases base>/<tag>/<file>` from its own Kconfig
  (`ICLFORGE_FIRMWARE_UPDATE_BASE_URL`), so nothing in the index can send it to another host, and a third
  party's firmware points at its own repository ([decision 32](#decisions)). The release notes link is
  `<repository>/releases/tag/<tag>`, built the same way. A `file` outside `[A-Za-z0-9._+-]` is refused.
- **The board picks its image by facts, not by name.** The same facts `refuse_image` checks
  (`firmware_image.hpp`): project, chip, flash size and a revision inside the range, then PSRAM. A new board
  variant appears in the index by being in a release; no board's firmware changes to be offered it, and a
  board with no entry says "this release has no image for this board". The image's own head is checked
  again after the first 288 bytes arrive, before anything is erased, exactly as for a push.
- **The index is signed, and so is each image through it.** The signature covers the index file's exact bytes,
  and the index carries each image's size and SHA-256, so an image that arrives is installed only if it is the
  one a verified index named. The images themselves carry no signature of their own (a push needs none).
- **Order of publication.** The images are published and checked first, then the index, then its signature,
  and a board that sees an index without its signature, or one that does not match it, treats the pair as
  absent. `gh release upload --clobber` replaces an asset by deleting it and uploading it again, so for a
  moment either file may answer `404`; the board says "could not reach the update list" and a retry works.

### Trust and signing

**Two paths in, two rules.**

| | Arrives how | Signature | Version rules |
|---|---|---|---|
| **Pushed** | A client sends the image to the board: `PUT /firmware`, `ota.py push`, the page, ac3hearth | **None required, always accepted** (unless the owner later turns on policy `all`, [Later hardening](#later-hardening)) | None |
| **Pulled** | The board fetches it: `PUT /firmware/from-release` | **Required**: the index verifies, as below; the image's SHA-256 is the index's | Strictly newer than the running version; `secure_version` and `sequence` no lower; see [What an update may do](#what-an-update-may-do) |
| Rolled back | `PUT /firmware/rollback` | Not checked: the image already ran here | None |

**What the board checks on a pull, in order. Any failure installs nothing and says which.**

1. Both files arrive over TLS that validated ([TLS on the board](#tls-on-the-board)).
2. The signature file's SHA-256 field equals the index file's SHA-256 (a mismatch means the pair was caught
   half-replaced).
3. The chain (at most three certificates: leaf, optionally an intermediate, ending at the anchor) verifies
   with mbedTLS against the **trust anchors in the build**, with its default profile (SHA-256 or better,
   RSA 2048 or better, P-256 or better). The anchor's own dates are not looked at; the others' are, against
   the board's clock when it is set ([Expiry and the clock](#expiry-and-the-clock)).
4. The leaf is for code signing (extended key usage `id-kp-codeSigning`, key usage `digitalSignature`) and
   its subject common name is the **name pinned for this channel** in the build
   (`ICLFORGE_FIRMWARE_LEAF_STABLE`, `ICLFORGE_FIRMWARE_LEAF_BETA`), unless the pin is empty. The pin is what
   keeps another certificate from the same CA (one made for Windows binaries, one made for another project)
   from signing firmware, and a beta leaf from signing the stable channel.
5. The signature is a valid signature over the index file's bytes by the leaf's key.
6. Then the index itself: `format` is understood, `channel` is the one this board follows, `sequence` is no lower
   than the highest this board has accepted, the board has an entry, the entry's version is newer, and its
   `secure_version` is no lower.

Only then does the install begin, and the image has to match the entry's size and SHA-256 as it streams.

**The four key options, against these principles.**

| | What signs, what the board holds | Fits | Does not fit |
|---|---|---|---|
| **A. A dedicated key** | A raw key pair made for firmware. The board holds the public key, or a self-signed certificate of it | The least code: no chain, no clock. Fine for a third party with no CA | No expiry, no chain, so a rollover is an update that carries two keys; the owner's CA is not used |
| **B. A leaf from the owner's local CA, the CA in the board** *(recommended; taken by the owner)* | Two new code-signing leaves, `stable` and `beta`, from the local EJBCA. The board holds a firmware sub-CA (or the CA) as its anchor | Uses the CA the owner has. A leaf can be reissued with no firmware change; beta and stable separate; expiry and a chain are real; a self-signed certificate as the anchor degenerates to option A with the same code, so a third party needs no CA | Needs X.509 on the board (already there for TLS) and a CA ceremony for the owner. Two leaf keys to keep, as secrets of two GitHub environments |
| **C. The Windows code-signing certificate, reused** | The leaf already in the organisation secrets `WIN_TESTSIGN_PFX_*`, signed with in CI the way `sign-windows` does | Nothing new to make, and CI already has the pattern | One key signs Windows binaries and firmware, so a leak of either is both; the secrets are organisation-wide, readable by the other repositories they are shared with; one leaf for both channels; its renewal follows the Windows leaf's (365 days at most). If the anchor is the CA's root, **every** certificate that CA ever made for code signing becomes a firmware key unless the leaf name is pinned. The design accepts it for stable if the owner chooses, by pinning its subject name |
| **D. Keyless Sigstore** | GitHub's workflow identity gets a ten-minute certificate from Fulcio and the signature goes in the Rekor log | No key to keep. The release already makes these attestations for people (`gh attestation verify`) | A board would need Fulcio's roots, Rekor's keys and a TUF-distributed trust root that rotates; the certificate is valid for minutes, so the board must verify a log timestamp and an identity extension; a JSON-and-protobuf bundle to parse. Far more code and RAM than A, B or C, and a dependency on a public service being up. Right for people, wrong for a board |

**Recommend B.** One mechanism covers everything from a lone developer's self-signed certificate to the
owner's CA, with the code A needs anyway. **C is B with an existing leaf**: nothing in the design stops the
owner using it for stable to begin with, and it costs nothing to change later because a leaf is not in the
firmware, only its pinned name is.

**What the signature protects, and what it does not.**

| Threat | Result |
|---|---|
| A stolen or swapped release asset or index (a hijacked GitHub account, the CDN) | **Refused**: the index does not verify, or an image does not match it |
| TLS broken on the way | **Refused** the same way |
| A page or a machine on the board's LAN pushing an image | **Not stopped**, by the owner's choice: pushes are unsigned. The `Host` check ([decision 13](#decisions)) still keeps a web page from doing it |
| An old signed index replayed | **Refused** on a board that has seen a newer one (`sequence`), and an older version is never offered |
| A board kept on an old index (a freeze) | **Not stopped**: an attacker who can keep serving the last good index can keep a board where it is. An `expires` field would bound it ([Later hardening](#later-hardening)) |
| A leaked beta key | Beta boards can be given a hostile image; **stable boards cannot**, since they accept only the stable leaf |
| A leaked stable key or CA key | Boards that pull from the stable channel, or trust that CA, can be; the answer is a rollover |
| Hostile code built in CI and then signed | **Not stopped.** A signature says who approved an index, not what is in it |
| Someone with a USB cable, or with the board | **Not stopped**, by design |

### The process

The steps a person takes, in the order they come up. Nothing in a session touches key material: the
owner makes the keys, and sessions only write the tools and the documents.

#### Making the keys

The owner, once, with [the EJBCA guide](hearth-firmware-signing-ejbca.md):

1. A **firmware sub-CA** under the local root, "Iain Chesworth Labs Firmware CA" (RSA 3072, path length 0, key
   usage `keyCertSign` and `cRLSign`), so that nothing else the root ever issued, the Windows signing
   certificate included, can sign firmware. If the root's key is not in EJBCA the guide's external route makes
   the same sub-CA; if even that is more than is wanted today, the root as the anchor with the leaf names
   pinned does the same job and the board is unchanged.
2. **Two profiles** copied from the Windows pair (`code signer`, `local code signing`) and narrowed to this one
   sub-CA, RSA 3072 only, two years, `CA:FALSE`, key usage `digitalSignature` only, extended key usage
   `codeSigning` only (basic constraints and key usage critical, the extended key usage as the Windows profile has
   it: the board and the tool check that code signing is present, never its criticality), with no CRL, AIA, OCSP or
   alternative name (the board checks none).
3. **Two leaves**, `Hearth Firmware Stable` and `Hearth Firmware Beta` (the names are the pins), each from a key
   and a certificate request made on the owner's machine, so a private key never reaches EJBCA. RSA 3072 with
   SHA-256 is what the existing profile issues, a board already carries RSA for TLS, and ECDSA P-256 would save
   about 2 KB per signature file, so it is chosen only if a profile that allows it already exists.
4. The public certificate of the sub-CA goes into the repository as `firmware/keys/trust.pem` (a build setting,
   `ICLFORGE_FIRMWARE_TRUST_ANCHORS`). Each leaf's key and certificate go into a PFX that is saved as the two
   secrets of its GitHub environment ([CI signing](#ci-signing)); the owner keeps the key files and their
   passphrases offline in two places, which is the only recovery for a lost leaf key (a new leaf is issued, and no
   board changes).

Nothing in a session touches key material: sessions write the tools and the documents, and check the public
certificates and the screenshots the owner sends back.

#### Publishing a release

CI does it ([CI signing](#ci-signing)). A release run builds and packages the images (O8) and publishes the
release. Then the `firmware-channels` workflow, which starts when the Release run has finished, builds each
channel's index from the release's manifest
(`package_firmware.py index`), signs it in that channel's GitHub environment (`sign_index.py sign`), uploads the
pair to the `firmware-channels` release with the images already up and the index last, and downloads it back
and verifies it as a board would. **The stable job waits for the owner's approval**; the beta job does not. A
`workflow_dispatch` of the same workflow signs any release again, which is how a leaf renewal reaches the
channels. A person with a leaf key on their machine can do the same by hand with the
same two commands, which is how a third party without CI publishes ([A third party's own channel](#a-third-partys-own-channel)).

#### Replacing the anchor

Since a pushed image is never refused, a person can always replace what a board
trusts by pushing a build that trusts something else. Three surfaces, in the order they are built:

| Surface | What it does | When |
|---|---|---|
| **A build and a push** | The anchor, the leaf names and the base URL are build settings. A person builds with their own, or takes a third party's image, and pushes it (`ota.py push`, the page, or USB). Nothing to add to the board | First slice |
| **At the board** | A setting in NVS that overrides the build's anchor. It is changed only while the board is in a *presence window*: opened by a button on its screen ("Allow changing the update key for 5 minutes", which also shows the certificate's fingerprint for a yes or no), or, on a board with no screen, in the first two minutes after a power-on reset (a software restart, which anyone on the LAN can ask for, does not open it). One more action puts the build's anchor back | Later; wanted when policy `all` exists, since then a push can no longer replace it |
| **USB** | Erase NVS, or flash a build with other anchors: the board's own and everything it kept is gone, and the factory trust is back | Always |

There is no unauthenticated route that changes the anchor: the board has no accounts, and the presence
window is the authentication.

#### Recovery

What can go wrong, and what a person does. None needs a cable unless it says so.

| What happened | What the board does | What the person does |
|---|---|---|
| The anchor in a build is wrong | Pulls fail with "the update list is not trusted"; pushes work | Push a corrected build |
| A leaf key is lost | Pulls stay as they were; nothing new can be signed | Issue a new leaf from the sub-CA, same name, replace the two secrets of its environment, and run the signing workflow again for the current release. No firmware change |
| A leaf key leaks | An attacker holding it and the transport can feed boards | Issue a new leaf; publish an index that raises the leaf floor ([Rollover](#rollover-floors-and-revocation)) |
| The CA key is lost | Existing boards keep trusting a CA that can sign no more | Push (or USB) builds with a new anchor, each board once. A pull cannot do it |
| The CA key leaks | As above, for every channel it signed | The same, at once |
| A board's NVS override (later) is wrong | Pulls fail | Reset it at the screen, or erase NVS over USB |
| Policy `all` (later) is on and the signing key is lost | Pushes of unsigned builds refused | Switch it off at the screen; or erase NVS over USB |
| Everything | | USB, which no setting can disable: the ROM's download mode is not touched |

#### Rollover, floors and revocation

There is no revocation list. What stands in for one:

- **`sequence`** is in every index and a board keeps the highest it has accepted for its channel. An older
  index is refused. It is one `uint32` per channel in NVS (`fw_seq_s` and `fw_seq_b`), so that following the
  other channel for a while does not move the floor of this one.
- **A leaf rollover** is a new leaf with the same name, made before the old one expires, and signs the next
  index. Nothing in any board changes.
- **An anchor rollover** ships an image whose anchor file holds both the old and the new certificate (it
  verifies under the old one), then a later image drops the old.
- **The leaf floor** ([Later hardening](#later-hardening)): an index may carry `min_leaf_issued`, a date; a
  board that has seen one refuses any leaf whose `notBefore` is earlier. A leaked leaf is then dead on every
  board that has fetched one index signed by the new leaf, with no CRL and no clock: the board compares two
  dates that are both in signed data. (`notBefore` and not the certificate serial, because a CA's serials need
  not rise.)

#### Expiry and the clock

TLS to GitHub already needs a board whose clock is right (the CDN's leaf starts on
2026-09-30), so a pull is not attempted without one. For the chain: the leaf's and the intermediate's dates are
checked against that clock, and the anchor's own are not (it is trusted by being in the build, and a root made
for 20 years should not be a reason to refuse a pull). A leaf that has expired fails with "the update
certificate has expired, or this board's date is wrong", the cue to sign with a fresh leaf. A clock that is
wrong in the future makes a good leaf look expired and the board fails closed, which the message names. A
clock set wrong *to the past* cannot help an attacker, because `sequence` has already moved. A QEMU test
build that has no clock passes `now` as nothing and the dates are not judged, which is the only place that
happens.

#### A third party's own channel

Someone who runs their own releases: makes a CA, or only a self-signed
certificate, which is then both the anchor and the leaf; issues a leaf for each channel; builds with their
anchor file, leaf names and base URL; runs `package_firmware.py index` and `sign_index` over their images; and
installs their first build by USB, the browser installer or a push. Their boards pull from them and trust
nothing of this project's, and a person with stock images who wants to follow them pushes their build once.
The component ships no anchor of the owner's in a place an integrator cannot replace.

#### Chain size and RAM

The signature file is 1.5 to 4 KB (an ECDSA leaf and intermediate in base64 are about
1.4 KB, an RSA-2048 pair about 3.2 KB, and the signature 0.1 to 0.35 KB). The board reads the two small files
and closes the TLS session before it verifies, so the check does not sit on top of the session: a buffer of up
to 12 KiB, and the parsed chain, a few KB, freed after. These are estimates, to be measured in slice O10b. The
cost to the image is X.509 chain verification and the signature algorithms, which TLS to GitHub already pulls in
(an ECDSA chain from `github.com`, an RSA one from the CDN).

### CI signing

**What exists (read from the workflows and docs on 2026-10-25).** iclforge signs no Windows binary today: the
documents say "no code-signing certificate yet", and nothing in `.github` signs a Windows file. The pattern the
owner means is the shared repository `icl-common-infra`'s `sign-windows` action, which arinc-data-loader's
release workflow uses:

- a repository variable `SIGN_WINDOWS` that is `false` (the default; nothing signed), `test` or `production`
  (reserved, fails until a service is chosen);
- in `test` mode a leaf PFX in **organisation** secrets `WIN_TESTSIGN_PFX_BASE64` and `WIN_TESTSIGN_PFX_PASSWORD`
  (visible to selected repositories), written to a temporary file on a Windows runner, `signtool sign /fd SHA256`
  with an RFC 3161 timestamp, the file removed, the signature checked afterwards;
- the leaf is `CA:FALSE`, key usage `digitalSignature`, extended key usage `codeSigning`, RSA 3072, SHA-256, at
  most 365 days; the CA key is offline and never in CI, only the leaf reaches it.

**This corrects what an earlier version of this section said**, that the Windows certificate "cannot be in CI". A
PFX in a secret is exactly how it is. What the firmware keeps from the pattern: the mode variable that makes a
merged workflow harmless until it is switched on, the leaf PFX in a secret and nothing else, and the check after
signing. What it does not reuse: the Windows leaf itself (its secrets are organisation-wide, so any repository
with access could read a firmware key, and one key would do two jobs and both channels), and `signtool`, which is
Authenticode and has nothing to do with an index file.

**The workflow.** A new `firmware-channels.yml` that starts on `workflow_run` (the `Release` workflow has
completed) and on `workflow_dispatch` (`version_tag` and `dry_run`, default true, so it can be tried against any
release that exists). `release.yml` is not edited. A `workflow_run` workflow always runs the copy of the file on
`main`, however the release run started (a tag push, or `release.yml`'s own `workflow_dispatch`, which pushes its
tag only at the end), so `github.ref` is `refs/heads/main` in every run of it:

| Job | Runs on | Needs the secret | Does |
|---|---|---|---|
| `index` | `ubuntu-latest`, `contents: read` | no | Runs only when the Release run succeeded. Finds the release: the `v*` tag that points at the Release run's commit (none, as after a dry run of Release: it stops with a notice; two: it stops with an error naming them). Downloads the release's `hearth-sink-manifest.json`; `package_firmware.py index` writes the unsigned `beta` index, and the `stable` one when the release is not a Pre-release; refuses an index older than the published one (a re-run of an old tag cannot move a channel back). Writes a summary to the run's page: the tag, the versions, the `sequence` and the SHA-256 of each index |
| `sign-beta` | `ubuntu-latest`, `contents: read`, **`environment: firmware-beta`** | `FIRMWARE_SIGN_PFX_BASE64`, `FIRMWARE_SIGN_PFX_PASSWORD` | `sign_index.py sign` reads the PFX from the environment into memory (no file), signs, checks the leaf is a code-signing leaf with the pinned name and at least 60 days left (a warning under 60, a failure when expired), and verifies the result against the committed `firmware/keys/trust.pem` |
| `sign-stable` | the same, **`environment: firmware-stable`** | the same names, the stable leaf's values | the same, for the `stable` index; skipped for a Pre-release |
| `publish` | `ubuntu-latest`, `contents: write` | no | Makes the `firmware-channels` Pre-release if it is missing; uploads each pair with `--clobber`; downloads the pair back from the public URL with no token and runs `sign_index.py verify`; checks that every file the index names is downloadable. With `dry_run` it stops before the upload |

- **`dry_run` signs with a throwaway CA made in the job**, so every dry run exercises the whole pipeline and no
  environment secret is read; the pair is verified against that throwaway anchor, and nothing is published.
- **A switch.** The repository variable `PUBLISH_FIRMWARE_CHANNELS` (`false` by default) gates the real jobs, as
  `SIGN_WINDOWS` does. This matters: a workflow job that names an environment that does not exist *creates it*
  with no reviewer, so merging the workflow before the owner has set the environments up would run the stable
  job unguarded. With the variable `false` nothing runs; the owner sets it to `true` last. (The owner set it on
  2026-10-25, before the workflow exists: the first Release after the workflow reaches `main` signs for real, beta
  unattended and stable after an approval. A `dry_run` dispatch needs no variable, so it can be tried first.)
- **It cannot hurt the release.** It starts after the Release run has finished and is a run of its own, so it
  cannot delay or fail the release, and the stable job waiting for an approval holds nothing up.
- **The signing jobs stay small.** Python and `cryptography`, installed from a hash-locked requirements file,
  and `actions/checkout` and the artifact actions pinned by SHA as in every workflow here; no other third-party
  action sees the key. The tools run from `main`, not from the tag, so an old tag can be signed again with
  today's tool, and nothing from the tag runs: its manifest and images are read as data.
- **Why a workflow of its own started by `workflow_run`:** (1) `release.yml` is not edited at all, so there is
  nothing to conflict with #1621 and no job the release has to wait for; (2) the ref is always `main`, so the
  environments can allow `main` only and what can read a key is a workflow file that has been merged; (3) it
  works for both ways a release starts. A `workflow_call` job in `release.yml` (the pattern of
  `manifest-bump.yml`) would have run with the release's own ref, a tag or, in a dispatch-started release,
  `main`, and so would have needed tags allowed.

**What the owner creates** (a session never runs `gh secret set`; these are in the GitHub web pages):

| Where | What | Value |
|---|---|---|
| Settings, Environments | `firmware-beta` | Deployment branches and tags: **`main` only**. **No reviewer: it signs on its own.** Secrets `FIRMWARE_SIGN_PFX_BASE64` and `FIRMWARE_SIGN_PFX_PASSWORD` for the **beta** leaf |
| Settings, Environments | `firmware-stable` | Deployment branches and tags: **`main` only**. **Required reviewers: the owner, so every signing run waits for their approval.** *Prevent self-review* off (the owner starts the release, and is also the one who approves its signing); *Allow administrators to bypass configured protection rules* off, if the page offers it, so that nobody skips the approval. The same two secret names, for the **stable** leaf |
| Settings, Secrets and variables, Actions, Variables | `PUBLISH_FIRMWARE_CHANNELS` | `true`, **last**, once the two environments exist |
| Not these | `WIN_TESTSIGN_PFX_*` | Never reused for firmware |

**Decided by the owner, 2026-10-25: a stable signing run needs the owner's approval every time; a beta run signs
on its own.** The beta leaf signs only what beta boards read (the owner's development kits, which take unsigned
pushes anyway), so a run that signs it is not worth a click. The stable leaf is the one a person who is not
the owner would rely on, and the approval shows the owner which release is about to be signed: the gate "a
human at every release" asked for, at the cost of one approval at each stable release (the job waits; the
release is already out).

- **Both environments allow `main` only, and there is no tag ruleset.** A run on `main` uses the workflow file as it
  was merged, through the repository's own review; a tag can point at any commit that anyone with write access
  pushes, with any workflow file in it, and an environment's secrets are readable by every ref it allows. So with
  `main` only, what can read a key is what has been merged. (The first version of this section, PR #1652, limited
  stable to `v*` tags and asked for a tag ruleset. The keys session found on 2026-10-25 that `release.yml` makes
  that unworkable: its `workflow_dispatch` path pushes the release tag itself with `GITHUB_TOKEN`, which a ruleset
  that restricts creating `v*` tags blocks, and a job it calls runs with `github.ref` = `main`, which a tags-only
  environment refuses.)
- **What the owner checks when they approve:** the `index` job has already written its summary (the release
  tag, the versions, the `sequence`, the SHA-256 of the index) to the run's page, before the stable job waits.
  They read it, then approve the pending deployment in the run, and only for a release they cut. An approval
  nobody gives expires after 30 days and the run fails; signing again is a dispatch.
- **Both settings above matter.** With *Prevent self-review* on, the owner, who started the release, may not be
  allowed to approve its signing run and would lock themselves out; with administrators allowed to bypass,
  the approval is optional for anyone with that role.

**What would conflict with PR #1621 (the release prep), read on 2026-10-25.** #1621 changes `release.yml` in the
`resolve-version` job (an added `REPO` variable and a `render_release_notes.py --check` call near line 97 to 150),
`_static.yml`, `CHANGELOG.md`, `firmware/esp-idf/iclforge/idf_component.yml` and `tools/release`. The firmware
workflow is a new file started by `workflow_run`, so `release.yml` is not edited, and its tool files are under
`tools/hearth`: no textual overlap, so no conflict while they merge in either order. The one real interaction is
`CHANGELOG.md`: #1621 turns `[Unreleased]` into the 0.11.0-beta.1 section, and its own notes say to merge it last,
since anything added to `[Unreleased]` before then becomes part of that release's notes (already 541k characters
against a 125k limit). So the firmware slices keep their CHANGELOG lines out of `[Unreleased]` until #1621 has
landed, or the owner decides they belong in 0.11.0-beta.1. Also: the channel index is only published for a
release that carries sink firmware, which the first one to do so is whichever the owner cuts after the signing
slice lands.

### What an update may do

| Source | Version |
|---|---|
| Pull (`PUT /firmware/from-release`) | Strictly newer than the running version, `secure_version` at least the running image's, and the index's `sequence` at least the highest the board has accepted |
| Push (`PUT /firmware`) | None: a person developing may push anything |
| `PUT /firmware/rollback` | None |

- **`secure_version`** is the 32-bit field ESP-IDF keeps in `esp_app_desc_t`
  (`CONFIG_BOOTLOADER_APP_SECURE_VERSION`). It is a floor our code enforces on a pull, not ESP-IDF's
  anti-rollback, which burns eFuses and which `check_esp_efuse_free.py` forbids. A release that fixes a
  security flaw raises it by hand. The index carries it for each image, and since the index is signed and
  names the image's SHA-256, so does the image. The default stays 0.
- **Comparing versions.** `v0.11.0-beta.1` is newer than `v0.10.0-beta.1-2039-gf5aded471` (a build 2,039
  commits past a tag sorts after that tag and before the next one); `-beta.N` sorts before the same number
  without it. A version the parser does not understand is never "newer": it fails closed, and the reply
  says so. The rule lives in `libs/device`, with a table of cases in its test. The version a board stores is
  31 characters, so the comparison is on as many.
- **Channels.** A board has a `channel` setting in NVS. Its default is Kconfig's, `beta` (the owner's
  decision: every release is a Pre-release today), flipped to `stable` by the first stable release if the
  owner wants. Changing channel never installs anything, and never goes back to an older version.
- **A version that failed here is not offered again.** An update that rolled back, or failed its checks,
  stores its version in NVS (`fw_skip`); `GET /firmware/available` says "v0.11.0-beta.1 is available, but it
  went back on this board on 2026-11-06" and the install button asks for a second confirmation. A newer
  release clears it.

### TLS on the board

- **Trust.** `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_NONE` and `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`
  with a path: a bundle of USERTrust ECC, ISRG Root X1 and one spare, chosen in slice O10b from what the
  three hosts present that week. A few KB in all. The full default bundle is tens of KB on every board for
  hosts that are never contacted. Validation is on and has no fallback: a connection the board cannot
  verify is not made. This bundle is GitHub's, and is **separate from the firmware trust anchors**: a CA
  that vouches for a host never vouches for a firmware index, and the reverse. A root rotation GitHub makes
  before a board's bundle covers it leaves that board unable to pull until it is pushed to (`ota.py push
  --release`, from a laptop, does not depend on the board's bundle).
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
- **Flash.** TLS, the HTTP client's redirects, the bundle and the chain check add code to every build that
  pulls. The size is measured in O10b, not guessed; a typical ESP-IDF HTTPS client is of the order of 100 KB.

### Which builds can pull

Pull is a build option, `ICLFORGE_FIRMWARE_PULL`, and the board reports whether it has it. Three
reasons a board cannot, each shown with its reason, not hidden:

- **The 4 MB C6** (`hearth-sink-esp32c6`): its slot is `0x1E0000` (1,966,080 bytes) and its profile's
  budget is the same number ([Flash layout](#flash-layout), 2026-10-21). TLS does not fit. It takes pushes
  and not pulls: "This board's firmware has no room for a secure connection" ([decision 33](#decisions)).
- **A build with no trust anchor** (an empty `ICLFORGE_FIRMWARE_TRUST_ANCHORS`): "This firmware carries no
  update key". A pull is *never* made without an anchor to check it against.
- **A QEMU build** (`clock/none`, no TLS test): reports no clock.

### The flow on the board

```
 screen or page                  board                                        GitHub
 ──────────────                  ─────                                        ──────
 Check for updates ───────────►  POST /firmware/check  (202)
                                   needs: network, time, an anchor, memory, not on trial,
                                          not in flash mode
                                   read hearth-update-<channel>.json + .sig.json  ───►  index, signature (302, CDN)
                                   verify the pair (steps 1 to 6 of Trust and signing)
 poll GET /firmware/available ◄─   up_to_date | available (version, size, date) | failed (why)
 Install v0.11.0-beta.1 … ─ confirm
                      ─────────►  PUT /firmware/from-release  "v0.11.0-beta.1"
                                   enter flash mode: playback, Sendspin, sink stopped
                                   GET <base>/<tag>/<file> ──────────────────►  image (302, CDN)
                                   first 288 bytes: refuse_image, version = the index's,
                                       newer, secure_version      (nothing erased until these pass)
                                   erase-as-you-go + write; running SHA-256 = the verified index's
                                   esp_ota_end: image checks; read-back hash
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
| Failed | The reason, in words ("The player does not know the date yet", "Could not reach GitHub", "The update list is not trusted: nothing was changed", "The update certificate has expired, or this player's date is wrong", "This version has no image for this board", "Stop what is playing and try again", "The download did not match its checksum: nothing was changed") | Try again |
| Not available | A greyed row with its reason, for good: "This board has no network", "This board has one app slot; an update over the network needs a USB flash first", "This firmware carries no update key", "This firmware has no room for a secure connection" | none |
| Not now | A greyed row with its reason, for the moment: "The player has no network address yet", "The player does not know the date yet", "Time sync is off", "This version is still being tried; wait 4 minutes, or go back to the previous one", "An update is already running", "Stop what is playing and try again" | none |

The words come from one place, `firmware_update.hpp` in `libs/device` (below), so the screen, the page,
`ota.py` and ac3hearth cannot drift; the panel code only draws them. An install's progress is
`GET /firmware`'s `upload` object with `stage: "downloading"`, so `ota.py status`, the page and ac3hearth's
Firmware tab show it with no change.

### Routes

| Route | What it does | Replies |
|---|---|---|
| `GET /firmware/available` | The update flow: `supported` and `why` (what is true of this board for good, so a greyed row has its reason), `blocked` (what stops a check or an install now, or null), `state` (`idle`, `checking`, `up_to_date`, `available`, `installing`, `failed`), `channel`, `running`, `checked` (seconds, or null), `latest` (version, released, size, notes link, image name, or null), `trust` (the number of anchors, whether they are the build's or the board's own, and each one's SHA-256 fingerprint, short), `signed_by` (the leaf's name from the last verified index, or null), `error` (code and text, or null), `skipped` (a version that went back, or null). Every key is always present | `200`, JSON |
| `POST /firmware/check` | Starts a check and returns; the result is on `GET /firmware/available` | `202`; `409` flash mode, on trial, an update running; `503` no memory beside what is playing |
| `PUT /firmware/from-release` | Body: the version, as text, which must equal what the last check offered. Enters flash mode, downloads, checks, writes, restarts | `202` then a restart; `403` the `Host` is not the board's; `409` nothing offered (check again), on trial or already running; `412` no anchor, no clock or no pull in this build; `503` no memory |
| `PUT /firmware/channel` | Body: `stable` or `beta`; stored | `200`; `400` |

The error codes in `error.code` are `no_network`, `no_clock`, `tls`, `unreachable`, `bad_index`,
`bad_sig_file`, `hash_mismatch` (the signature file is for another index), `untrusted`, `leaf_usage`,
`weak_key`, `bad_signature`, `leaf_name` (steps 2 to 5 of the check, one code each), `cert_expired`,
`cert_not_yet_valid`, `replayed` (a `sequence` below the board's), `no_image`, `no_memory`, `bad_image`,
`bad_hash`, `unknown_version`, `older_security`, `write_failed`, `no_slot`, `on_trial`, `busy`,
`nothing_offered`, `offer_stale` and `not_supported`. Each has a sentence in `firmware_update.hpp`.

The board's routes are for a board whose network is its own (see [Routes](#routes)). `PUT` is used for the
install, as for `PUT /firmware`, so that a page on another site cannot send it without a preflight, and
it carries the same `Host` check; `POST /firmware/check` needs none, since it changes nothing but a few HTTPS
requests. A build without pull answers the first route with `supported: false` and the others
with `404`, as it does for any route it lacks. The routes the later hardening adds (`PUT` and `DELETE
/firmware/trust`, `PUT /firmware/policy`) are not in the first slices.

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
  no `caps.update.pull`; `caps.update.trust_anchors == 0`. **Greyed for now** is `inert(backend)`, the text of
  `UpdateStatus::blocked`, which `firmware_update.hpp` works out from the board's facts at that moment (an
  address held, the clock set, time sync on, not on trial, not in flash mode, no update running, room for a
  session); `update-install` is also inert with "Check for updates first" until a check has offered a version.
  `GET /firmware/available`'s `supported`/`why` and `blocked` are the same two things for the page.

  **What this asks of the contract** (for prefs-core, through the coordinator), nothing else:
  1. `BoardCaps::update`, an `UpdateCaps` of two build facts, filled once at boot in `board_caps.cpp`:
     `bool pull` (the image has the HTTPS client, the bundle and the routes: `ICLFORGE_FIRMWARE_PULL`) and
     `int trust_anchors` (the certificates in the build's anchor file; 0 for a build with none, which cannot
     verify a pull). The variants in `board_variants.hpp` get them: `pull` and one anchor on the shipping boards,
     none on the frame buffer bench build, and a new `c6-4mb` variant with `pull` false, so the greyed
     reason is tested. The other facts it needs are already there: `network`, `wall_clock`, `firmware_slots`.
  2. `Backend` gains `update_status()` (an `UpdateStatus` from `firmware_update.hpp`, so O10a lands before
     O10d), `start_update_check()`, `start_update_install()` and the channel's getter and setter.
  3. A `confirm` text on a `kAction` `Pref`, shown before `set` runs: the install's ("The player stops
     playing, downloads it (about two minutes), and restarts..."), and the same field serves the restart and
     the resets another session is building.
  4. While a check or an install runs the screen resolves the Updates group once a second, so that
     `update-status` follows the progress; no editor of its own is needed.

  The panel reaches the pull through a seam like `panel.hpp`, with a null implementation in a build with
  no screen (`panel/none` links no LVGL). The trust rows ("Update key: built in", "Use the built-in key",
  "Allow changing the update key") are [Later hardening](#later-hardening) and are registry entries too.
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

What the build and the release must emit. Workflow edits are allowed now, each in a PR of its own and told to the
coordinator first; they are listed under [CI signing](#ci-signing), and `wheels.yml` (which publishes to PyPI on a
tag push) and the release prep (PR #1621) are not touched by them.

1. `package_firmware.py index` (a new command): writes `hearth-update-<channel>.json` from the merged
   `hearth-sink-manifest.json` of a release, for the channel it is given. The images need nothing new: they
   carry no signature, so there is no `sdkconfig.sign`, no 4 KiB, no change to `check_esp_efuse_free.py` and
   no ESP-IDF setting to prove.
2. `sign_index.py` (a new tool beside `ota.py`): `sign` signs an index with a leaf from a PFX in the environment
   or a key file and writes the signature file; `verify` checks a pair against an anchor file the way a board
   does, and is what a person uses to look at a channel before they trust it; `selftest` makes a throwaway CA and
   two leaves, for tests and for the workflow's dry run. It needs `cryptography`, installed from a hash-locked
   requirements file; the rest is the standard library. A PowerShell form that signs through a key in the Windows
   certificate store (`SignData`, so the key never leaves it) is a manual fallback only: CI cannot use a
   non-exportable key. Each is tested against an independent verifier.
3. `check_firmware_package.py`: the index agrees with the manifest (sizes, SHA-256, no image missing for
   a profile that publishes one), and every file name in it is one the release has.
4. The `firmware-channels.yml` workflow, started by `workflow_run` ([CI signing](#ci-signing)); `release.yml`
   is not edited.
5. Docs: `docs/hearth/sink-firmware.md` (updating from the screen and the page), `docs/releasing.md` (the key
   ceremony, the environments, rollover, the recovery table), CHANGELOG (after #1621 has landed).

Nightly and pull-request runs build and package images as today and publish no index.

### Slices

**The smallest first slice that gives value** is O10a, O10b and O10c together, merged one after the other: CI
signs an index, a board checks GitHub, verifies it and installs the image it names, and nothing about pushing
changes. What it leaves out is everything that is machinery rather than value: an anchor stored on the board,
a policy setting, a leaf floor. Each slice is a PR of its own, and none touches a board until the
coordinator gives a window.

| Slice | What | Tested by | Needs a board window |
|---|---|---|---|
| **O10a, the index, the signature and a host-tested library** | `firmware_update.hpp` in `libs/device` (version compare, index and signature parse, image selection, the policy table, the state's JSON and words; no ESP-IDF); `package_firmware.py index`; `sign_index.py` (`sign`, `verify`, `selftest`) and a PowerShell fallback for a Windows-store key; `check_firmware_package.py`; `ota.py` untouched | Host: Catch2 cases for every rule and a version table, with a verifier result passed in. Python: a pair signed by `sign_index.py` with a throwaway CA and leaves made in the test (`selftest`) is checked by an independent verifier, for RSA 3072 and for ECDSA | No |
| **O10b, the board's pull** | `firmware_pull.cpp` in the component beside `firmware.cpp` (the upload core takes chunks from either source), the chain and signature check with mbedTLS against the build's anchors, the four routes, Kconfig (`ICLFORGE_FIRMWARE_PULL`, `_UPDATE_BASE_URL`, `_UPDATE_CHANNEL`, `_TRUST_ANCHORS`, `_LEAF_STABLE`, `_LEAF_BETA`), the certificate bundle, the NVS keys, the four-place route word list, `stub.js` | Host: the policy and JSON from O10a. **QEMU:** a loopback HTTP server serves an index, its signature and an image, signed by the fixture CA; the base URL accepts `http://` only in `sdkconfig.ci-ota`, and the signature rules are not relaxed; a wrong anchor, a wrong leaf name, a tampered index, an old `sequence`, an expired leaf and a wrong image hash are each refused; a **push of an unsigned image still works**. TLS is not tested here (no clock) | Yes: the TLS handshake with the three real hosts, RAM and flash measured, on the S3, the C6 (16 MB) and the P4 or the kit; a real pull from a throwaway channel release signed by the owner's leaf |
| **O10c, the workflow and the process** | `firmware-channels.yml`, started by `workflow_run` (`release.yml` is not edited; [CI signing](#ci-signing)), in a PR of its own; `docs/releasing.md`: the key ceremony, the environments, rollover, recovery. The owner's EJBCA work, the environments and the public anchor in `firmware/keys/` | The linters the workflows already pass (actionlint, zizmor); a `dry_run` dispatch, which signs with a throwaway CA and publishes nothing; `ota.py --release latest` and `installer_site.py` ignore the channel release | No (the owner makes the keys, the environments and the variable) |
| **O10d, Preferences and the page** | The four registry entries, `BoardCaps::update` and its variants, the `Backend` methods; the page's section; `ota.py update`; docs | The preview tool at several sizes with the null display; the Playwright suite with the stub | Yes: the kit's screen, once (including what the screen does while flash mode writes) |

#### O10b as built

What the board's code does that the table above did not say, or says another way:

- **The sequence floor is per channel** (`fw_seq_s`, `fw_seq_b`), and the channel is `fw_chan`.
- **The skipped-version note is derived**, from the last update `Firmware` reports (a version that went back
  after its trial), not stored: there is no NVS key for it.
- **A pull that fails after it began leaves the board in flash mode**, as a push that fails does: a restart
  leaves it (`PUT /firmware/mode normal`). A check never enters flash mode, and one that cannot run says why in
  `blocked`.
- **The trust anchors belong to the application, not the component.** `hearth_sink` embeds a PEM file as
  `trust_anchors.pem`, from `ICLFORGE_EXAMPLE_TRUST_ANCHORS` or, for CI's throwaway CA, the CMake
  variable `ICLFORGE_TRUST_ANCHORS_FILE`, and names its start symbol undefined at link time: the
  code refers to the symbols weakly, so that a build without them links, and a weak reference does not take
  an object out of an archive.
- **The QEMU test** is `tools/checks/run_pull_qemu.py`, on a build with `sdkconfig.pull` and
  `sdkconfig.ci-pull` (`ICLFORGE_FIRMWARE_PULL_TEST_HTTP`: an `http://` address and no date
  judged; never for a board). It covers the refusals, the offer, a replay, a damaged and a wrong image, the
  install and its trial, an up-to-date check, the unsigned push, both channels with an RSA, an ECDSA and a
  PSS signature, and the `Host` check. An expired leaf and TLS itself are not tested there (QEMU's board
  has no clock and the server is plain HTTP): they wait for a board. A non-self-signed sub-CA works as the
  anchor, as the plan said it should.
- **The PowerShell form that signs through the Windows store was not built**: CI cannot use a non-exportable
  key, and the owner's keys are made on their own machine ([the process](#the-process)).
- **Measured under QEMU** (an ESP32-S3, no PSRAM): the image grows by 31,968 bytes; the install task had
  8,496 bytes of its 12 KiB stack spare; the least free internal heap while pulling was 148,396 bytes. None of
  it is a board's number: the C6, the memory beside a playing stream and the time an RSA-3072 chain takes
  are owed to a board window.

### Later hardening

Each is its own slice, built only if the owner wants it, in this order of value:

- **The leaf floor and `expires`**: `min_leaf_issued` and an optional `expires` in the index, raised and checked
  from signed data; the answer to a leaked leaf and to a freeze.
- **The anchor at the board**: the NVS override, the presence window (a button on the screen, or a power-on
  reset), "Use the built-in key", and their registry entries and routes (`PUT` and `DELETE /firmware/trust`).
- **Policy `all`**: an opt-in setting under which a push must also carry a signature (a detached signature over
  the image's SHA-256 by a leaf that chains to the anchor, which `ota.py push --sign` makes). Off by default;
  switched on and off only at the board or over USB, never by a route alone, so it can never lock a person
  out; it is the setting that makes the anchor replacement at the board necessary.
- **`ota.py --release` verifies the index** the way a board does, and can use Sigstore's attestation for the
  images on the host, where its size does not matter.
- **An opt-in daily check** that only changes the screen to "available" ([decision 30](#decisions)).

**Exit of the first slice:** a board on the desk checks, installs a release whose index CI signed with the
owner's leaf from GitHub, passes its trial, and reports "Updated to ...", while the same board still takes an unsigned build by
`PUT /firmware`, and refuses an index signed by another CA. The exit waits for the owner's CA certificate
and a signed channel release (O10c).

**Open, to prove rather than assume:**

- that mbedTLS takes an intermediate, and a self-signed leaf, as a trust anchor, and accepts a chain whose
  anchor's dates it is told to ignore (a verify callback), on the build's mbedTLS (O10b, QEMU);
- that `mbedtls_x509_crt_check_extended_key_usage` and the key-usage check are in the build and judge a
  certificate the way the CA's template made it (O10b);
- that a signature made by .NET's `SignData` on a store key (PKCS#1 v1.5 or PSS; ECDSA's raw `r||s`) verifies
  under mbedTLS and the independent verifier (O10a);
- that `esp_http_client` follows the 901-byte redirect with a 2 KiB header buffer and a custom bundle (O10b);
- whether ISRG Root X1 alone validates the CDN's chain, which ends in "Root YR" cross-signed by X1 (O10b);
- the session's real peak on the C6 beside a stopped stream, the chain check's RAM and the code size on each
  chip (O10b);
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
  a board checks for a newer release, shows it, and installs it, from a signed index. Four slices, each its
  own PR: O10a the index, the signature and a host-tested library; O10b the board's pull and its routes;
  O10c the process (the keys, the first signed channel release); O10d the Preferences entry and the page.
  Later hardening is listed, not scheduled. **Status: designed 2026-10-25;** the owner took decisions 23 to 30,
  33, 34, 35 and 37; 31, 32 and most of 36 wait. **Exit:** a board on the desk checks,
  installs a release signed by the owner's leaf from GitHub, passes its trial, and still takes an unsigned
  build sent by `PUT /firmware`.

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
pins `esp_hosted`. Decisions 23 to 37 are those of O10 (2026-10-25): the owner took 23 to 30, 33, 34, 35, 36's lifetime and 37, and the rest are proposals.

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

Taken by the owner on 2026-10-25: 23 (b), 26, 27, 29, 30 and 33 as recommended (26 for a pull only), 24 (a) and
25 (a) with the owner's own CA (EJBCA), 28 (a), 34 (a) and 35 (a) (CI signs), 36's lifetime and 37 (a) (the owner
approves every stable signing run; beta signs on its own). 31, 32 and the rest of 36 are proposals.

23. **Where a board learns what the newest release is.** (a) GitHub's REST API; (b) **an update index, one
    small JSON file for each channel with a signature file beside it, published as assets of a long-lived
    `firmware-channels` Pre-release**; (c) the same index on GitHub Pages; (d) no pull: a machine on the LAN
    (ac3hearth, `ota.py`, the page) reads GitHub and pushes. **Recommend (b).** (a) is 60 calls an hour for
    the whole household, `.../releases/latest` answers `404` while every release is a Pre-release, and one
    release's listing is 263 KB. (c) has one host, no redirect and CORS, but the index changes only when
    the docs build runs (a failed docs build delays an update), and a Pages address does not follow a
    repository rename where release addresses do. (d) needs no TLS on any board and is the way for a board
    that cannot hold it, but it is not the board looking at GitHub, and it needs a machine awake. Cost of
    (b): a mutable release whose assets are replaced, a redirect to a second host (two TLS sessions, two
    roots). **Taken 2026-10-25: (b).**
24. **How a board proves a pulled image is the owner's.** (a) **a signed index: a detached signature over the
    index file, with a certificate chain that ends at a trust anchor in the build, checked by the board on
    the pull path only; the images carry no signature and a push needs none**; (b) ESP-IDF's signed apps
    (RSA-3072, the running image's keys as the anchor, the O7 design); (c) a raw dedicated key; (d) keyless
    Sigstore; (e) none, TLS only. **Recommend (a).** It is the one that meets the owner's words: it
    enforces on a pull and on nothing else, its anchor is in the build and can be replaced, and an unsigned
    push never stops working. (b) enforces on every path through `esp_ota_*`, takes its trust from the image
    that is running, has no setting to relax, and an image with the check and no signature block can take no
    update at all, so it makes development hard and can lock a person out. (c) is (a) with the least code
    and no chain, and (a) degenerates to it when the anchor is a self-signed certificate. (d) is too large
    for a board. (e) leaves every board's code in the hands of whoever holds the GitHub account. Cost of
    (a): X.509 chain verification and a signature algorithm on the board (the code TLS needs anyway), two
    small files fetched beside the index, and a signing tool; it does not defend a board against its own
    LAN. **Replaces the earlier (a), which the owner's principles rule out. Taken 2026-10-25: (a).**
25. **Whose certificate signs.** (a) **two new code-signing leaves, `stable` and `beta`, from the owner's local
    CA (EJBCA), a firmware sub-CA under it as the anchor in the board, the leaf names pinned in the build**; (b) the Windows code-signing certificate reused, as the stable leaf or both; (c) a raw
    dedicated key or a self-signed certificate; (d) keyless Sigstore. **Recommend (a).** Beta and stable have
    keys that cannot stand in for each other; a leaf is replaced with no firmware change; the Windows key
    signs Windows binaries only. (b) costs nothing now and is allowed by the design (pin its subject name),
    at the price of one key doing two jobs, an organisation-wide secret, and one leaf for both channels. The owner supplies:
    the CA's public certificate, whether it can make an intermediate, the key type it offers, and whether a
    leaf's key can be exported. Cost of (a): a ceremony at the CA ([the EJBCA guide](hearth-firmware-signing-ejbca.md)),
    two keys to keep, a rollover about every two years. See [Trust and signing](#trust-and-signing).
    **Taken 2026-10-25: (a).**
26. **What an update may do to the version.** (a) **a pull installs only a strictly newer version, with
    `secure_version` and the index's `sequence` no lower than the board's; a push and a rollback are
    unchecked**; (b) any signed image. **Recommend (a).** A signature says who approved an index, not that
    it is the newest, and a replayed old release with a known flaw is the downgrade attack. Cost: a release
    that fixes a security flaw has `secure_version` raised by hand; a version the parser does not understand
    is never offered; a board that has seen a newer index refuses an older one until it is pushed to.
    **Taken 2026-10-25: (a).**
27. **Channels.** (a) **two channels, `stable` and `beta`, both published; a per-board setting with Kconfig's
    default `beta` until the owner flips it**; (b) stable only; (c) beta only. **Recommend (a).** Every
    release so far is a Pre-release, so (b) would say "up to date" for ever. Cost: one more row in
    Preferences. **Taken 2026-10-25: (a).**
28. **A board and an unsigned `PUT /firmware`.** (a) **always accepted**, on every board, whatever it
    trusts; signature enforcement for pushes is an opt-in setting built later, switched on or off only at
    the board or over USB; (b) refused once the board has a key (the earlier recommendation). **Recommend
    (a),** the owner's: "I want to be able to upload dev firmware from my local development onto my dev
    kits." Cost: the signature protects the pull and not the LAN, which [Principles](#principles) says
    plainly. **Taken 2026-10-25: (a); (b) is withdrawn.**
29. **What a check and an install do to what is playing.** (a) **a check never stops anything and is
    refused where memory does not allow it; an install stops playback, after a confirm that says so**; (b)
    both stop playback; (c) an install waits until the stream ends. **Recommend (a).** Cost: on the C6 a
    check also needs the stream stopped and says so; (c) can wait for ever on a player that plays all day.
    **Taken 2026-10-25: (a).**
30. **Automatic checks.** (a) **none to begin with: a person presses Check; later, an opt-in daily check that
    only changes the screen to "available" and never installs**; (b) a daily check on by default; (c) an
    install by itself. **Recommend (a).** Cost: a board nobody checks stays old. (c) restarts a player in
    the middle of an evening. **Taken 2026-10-25: (a).**
31. **Trust for the connection.** (a) **a small custom bundle (USERTrust ECC, ISRG Root X1 and one spare),
    validation on, the date required, no fallback**; (b) ESP-IDF's full default bundle; (c) no validation,
    because the index is signed. **Recommend (a).** Cost: if GitHub changes a host's root before the bundle
    covers it, boards cannot pull until pushed to; (b) is tens of KB on every board for hosts it never
    contacts; (c) leaves the index and the image readable by anyone on the path, with the signed index as
    the only guard, and gives up the privacy of what a person runs.
32. **Whose repository and anchor.** (a) **the base URL, the anchor file and the leaf names are an
    integrator's build settings; the component ships none of the owner's in a place that cannot be
    replaced, and the example's defaults name this repository**; (b) compiled into the component.
    **Recommend (a).** The product is a generic base ([decision 12](#decisions)). Cost: an integrator sets
    three things and runs their own signing.
33. **The 4 MB C6.** (a) **push only, no pull**; (b) trim the build until TLS fits its slot (1,966,080
    bytes, the profile's budget to the byte); (c) give its slots back the room `audio` and `storage` took
    on 2026-10-21. **Recommend (a).** Its profile has no board on the desk, and (b) and (c) are work for a
    board nobody runs. Cost: `hearth-sink-esp32c6` is updated from a laptop or the page.
    **Taken 2026-10-25: (a).**
34. **How a person replaces the trust anchor.** (a) **first, by building (or taking) an image with the anchor
    they want and pushing it, which needs nothing on the board; later, an override in NVS set in a presence
    window (a button on the screen, or a power-on reset on a board with no screen) with a reset to the
    build's anchor, and USB always**; (b) the NVS override from the first slice; (c) never. **Recommend
    (a).** A push is never refused, so the first way is complete; the override matters only once a policy
    can refuse a push. Cost of (a): until the override exists a person who cannot build must be given an
    image. Cost of (b): a surface that can be set wrong (pulls fail until it is reset) and a presence
    design to get right before it earns anything. **Taken 2026-10-25: (a).**
35. **Who signs, and when.** (a) **CI, after the release run, each channel's leaf as a secret of a GitHub
    environment of its own, in a workflow of its own that starts when the Release run has finished
    (`workflow_run`, so it always runs from `main`; first drafted as a job `release.yml` calls, the pattern
    of `manifest-bump.yml`), behind a repository variable that keeps it off until the owner has set the
    environments up**; (b) the owner, by hand, on their machine; (c) CI for beta and the owner for stable.
    **Recommend (a),** the owner's: "just let CI do the signing. We already do that for Windows signing."
    The Windows pattern is a PFX in a secret and `signtool`; this is the same shape with a different leaf and
    no `signtool`. Cost: a key in GitHub's secret store (two, one for each channel, apart from the Windows
    one), so who can run a workflow on an allowed ref is who can read it (environments that allow `main` only
    and a reviewer on stable narrow that); (b) is a click and a laptop at every release. **Taken 2026-10-25: (a).**
36. **Revocation, expiry and the clock.** (a) **no revocation list: `sequence` now; a leaf floor
    (`min_leaf_issued`) and an optional `expires`, from signed data, later; the anchor's dates ignored, the
    leaf's and the intermediate's judged against the clock when it is set, failing closed with a message;
    leaves last two years and CI warns 60 days before one ends**; (b) a CRL or OCSP; (c) dates ignored
    altogether. **Recommend (a).** A board with no clock cannot pull in any case (TLS needs it), and a floor is
    two dates in signed data. Cost: a leaked leaf stays good on a board that has not fetched a newer index;
    (b) is machinery a board should not carry; (c) lets an expired leaf, or one made before a CA was rebuilt,
    sign for ever. **Lifetime taken 2026-10-25: one to two years is fine; the rest is a proposal.**
37. **Who approves a signing run.** (a) **a required reviewer on `firmware-stable` (the owner), none on
    `firmware-beta`**; (b) none on either; (c) a reviewer on both. **Recommend (a); taken 2026-10-25: (a),
    with both environments limited to `main`** ([CI signing](#ci-signing)). The beta leaf signs only
    what beta boards read, which are development kits that take unsigned pushes anyway; the stable leaf is the
    one someone who is not the owner relies on, and the click shows the owner which release is about to be
    signed. Cost: one approval at each stable release, with the release already out meanwhile. (b) signs
    unattended, and a hostile workflow on an allowed ref could read the stable key without a human seeing it.
