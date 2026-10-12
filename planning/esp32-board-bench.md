# ESP32 board bench: booking a board, using it, handing it back

**Status, 2026-10-12:** built, as PR 8 of [esp32-platform-profiles.md](esp32-platform-profiles.md).
The five boards on the owner's desk are a table in the repository
([`tools/hearth/bench.json`](../tools/hearth/bench.json)), the booking is one module that the board
suite and the flash tool share, and the scripts that built, flashed and read boards from
`D:\ac3bld\esp-reflash` are in `tools/hearth/` and read the build from the profile instead of a
hand-kept list of fragments.

## Why this page

Five boards are shared by every session that touches firmware, and they cannot be shared by
looking: a board that is being flashed, played or restarted by one session fails in ways that look
like a defect in another's work. The rule that keeps that from happening lived in the owner's
instruction to each session and in the head of whoever had last done it right. This page is the
rule, the order of a session's work, the hazards each with where it was learned, and where the
tools are.

## The boards

| Board | Chip | Address | Port | Lock file (in `D:/ac3bld`) | Note |
|---|---|---|---|---|---|
| S3a | ESP32-S3 N16R8 | 192.168.1.156 | COM15 | `s3-board-156.lock` | |
| S3b | ESP32-S3 N16R8 | 192.168.1.45 | COM16 | `s3-board-45.lock` | |
| C6 | ESP32-C6 16 MB | 192.168.1.117 | COM9 | `c6-board.lock` | `/log` holds 2 KB: a restart a moment before still has the boot line in it |
| P4a | ESP32-P4 rev v1.x | 192.168.1.99 | COM10 | `p4-board.lock` | a restart can leave it with no Wi-Fi |
| P4b | ESP32-P4 rev v1.x | 192.168.1.76 | COM13 | `p4-board2.lock` | no OTA: USB flash, app and otadata only |

`bench.json` is the source and a test holds this table to it. A board is addressed by name
(`S3b`), so that its lock, its address and its port come from one row and cannot be mixed: a lock
that is not the board's is the mistake the rule exists for. `ICLFORGE_BENCH` names another file of
the same shape for another machine.

## The rule

The owner's, 2026-10-11. Before touching **any** board:

1. Read all five lock files: `python tools/hearth/bench.py status`.
2. `ListAgents`: is a peer building or testing near the board?
3. No `idf.py`, `esptool`, `ota.py` or serial capture of that board is running (nothing here can
   see a process; look).
4. The board's `GET /status` is `stopped` (`bench.py status --probe` reads it; `flash` there means
   someone is updating it).
5. Take your own lock, atomically, one line:
   `bench.py lock take S3b --session <id> --purpose "..." --minutes 30`, which writes
   `session=<id> (<what>) | purpose=... | start=...Z | expected_end=...Z` with `O_EXCL`, so two
   sessions cannot both take it.
6. Message the peers whose work the board is under.
7. When done, **restore** the board to the image it had and **release** the lock
   (`bench.py lock release S3b --session <id>`).

A lock that is past its `expected_end` is shown as `OVERDUE` and is a reason to **ask that
session**, not to take the board over; `release` refuses a lock that is not the session's.
The tools that write a board (`bench_flash.py flash` and `ota`) refuse one the session has not
booked, and `board_suite.py run` takes the lock itself and gives it back.

## A session's order of work

```
bench.py status --probe                        # 1-4 above, as far as a script can see
bench.py lock take S3b --session <id> --purpose "baseline"
bench_flash.py build s3-n16r8 --tag <tag> --wifi
bench_flash.py ota S3b --build-dir D:/esp/build/iclforge/<tag>-s3-n16r8 --session <id>
bench_flash.py verify S3b --build-dir ... --profile s3-n16r8
board_suite.py run --profile s3-n16r8 --board S3b --session <id> --restart --compare ...
bench_flash.py ota S3b --build-dir <the image it ran> --session <id>     # restore
bench.py lock release S3b --session <id>
```

`--wifi` appends the network fragment last and adds the `wifi-credentials` overlay, so the guard
allows the image to differ from the profile in those keys and no others. A build made this way
carries the profile's id and hash, so the board suite accepts it; a scratch overlay does not
(`<id>+scratch`).

## Network values

The SSID and password are in one fragment (`wifi_fragment` in `bench.json`) and in every build
directory made with it. They are never printed, uploaded, put in a log or a PR. The tools take
them out of what they show:

- `bench.py log S3b` is `GET /log` with the values, the access point's address (`bssid`, `ifx`)
  and any line with a Sendspin pairing token or key taken out. **`GET /log` carries the SSID in
  its `connected with` line**, and in `network: wifi station on '...'`: a script that prints the
  log raw leaks it, as a session found out on 2026-10-12 by printing it.
- `bench.py snapshot` and `bench_flash.py` redact `/hardware`, `/status`, esptool and idf.py output
  by key and by value; `bench.py scrub --check|--mask FILE` counts or masks them in files, in the
  raw, JSON-escaped, percent-escaped and hex forms, and prints a count and never a value.

A line that contains the word `psk` is dropped whole, which also loses `WPA2-PSK` from the
association line: that is the old scripts' rule and the safe side of it.

## Hazards

Each with when it was learned and from what; the first is a report that was relayed to this page,
not something it ran.

- **A P4's restart, or an OTA's restart of it, can leave it with no Wi-Fi.** Session 32ab95's
  report of 2026-10-12: the fault is on the C6 side (`sd_host_wait_for_event 0x107`, no network),
  and a system reset in place of `esp_restart`'s CPU reset cures and prevents it (a `system_reset`
  pair of files beside `hearth_sink.cpp`, in that session's worktree and not merged: 24 of 24
  forced OTA pushes and 8 of 8 heals clean against 8 faults in 16 on the same protocol before).
  Until it is merged and on a board, a dead P4 is brought back with
  `esptool --before default-reset --after watchdog-reset read-mac`, with no USB flash; the image
  that is **running** when an update arrives does the restart, so a board still on an older image
  can fault once more. The table marks both P4s (`restart_kills_c6_link`), and
  `board_suite.py run --restart` refuses them unless told `--reset-by esptool` (a watchdog
  reset through the serial port) or `--accept-restart-fault`.
  Seen on P4a on 2026-10-12, for the report's sake: an OTA from 3556 to the identity image
  restarted cleanly, twenty-nine resets (twelve in the p4-rev1 session: one `bench_flash.py
  reset`, eight in the record run and three in the compare run; seventeen in the p4-rev1-ac4
  one) all came back on Wi-Fi with the C6 identified, the first timed at 16 s, and the
  rollback back to 3556, which the identity image restarted, left it with no answer for 90 s. The board's own record says
  an earlier 3556 trial was rolled back for restarting "before it had proved itself", so a
  dead-link trial that is then reset can be sent to the other slot: `flash --otadata-only`
  (the build's `otadata` alone, then a watchdog reset) boots the first slot as valid with
  nothing on trial, and is how P4a was put back on the 3556 it was found on, in both sessions,
  with no fault (a restore by rollback is a restart by the image that is running).
  **It boots the first slot**: it is the way back only while the image to restore is in
  `ota_0`. Another session's push to P4a on 2026-10-12 moved 3556 to `ota_1`, and an
  `--otadata-only` there would have booted their image; read `ota.py status` first. An S3's
  restart is safe, so a rollback or an OTA of the build is how an S3 goes back.
- **esptool exits 1 after a watchdog reset that worked**: the USB-Serial/JTAG port of a P4
  re-enumerates, pySerial complains, and the write had been verified and the board reset.
  `flash` reads that as a verified write (`Hash of data verified`, `Hard resetting`, and no
  fatal error) and anything else with exit 1 as a failure.
- **`GET /log` answers 4 KB at a time** (`kLogReplyBytes`) out of a 16 KB ring: a client
  follows `X-Log-Next`. A read that takes the first reply as the whole log sees the first 4 KB
  of the ring, which on a P4 ends before the boot `budget:` line (its boot is noisy with
  esp_hosted lines) and, as a mark, sits in the middle of the ring. `board_suite.py` reads to
  the end since PR 10.
- **P4b's RTS and hard resets land in the ROM's download mode** (the old script's note), so it is
  started with `--after watchdog-reset` (`after_flash` in its row) and put back by `flash
  --app-only`, which writes the `app` and `otadata` sections of `flasher_args.json` and nothing
  else: not the bootloader, the table, NVS, or the audio sample and storage image, which a
  build's `flash_files` also lists.
- **A board that has played has rotated its boot line out of `GET /log`** (S3), and the C6's log
  is 2 KB. A baseline's boot figures need `board_suite.py run --restart`, which the C6's 2 KB
  log still holds the line after (27 boots), and `--boots N` where the figures vary from boot to
  boot, as the C6's largest block does (88,064 to 96,256). `bench.py console C6 --reset` is the
  serial console's, for a boot that crashed.
- **`GET /hardware` is built once**, when the control surface starts: a notice added at boot cannot
  be there.
- **The boot line's free internal RAM is read twice over**: the Sendspin player's 16 KiB start
  task stack goes either side of the line, so S3b read 85.9 to 86.2 KB on eight boots and 102.5 KB
  on the ninth; `--boots N` records the worst of N
  ([the board suite](esp32-platform-profiles.md#3-the-board-suite)).
- **ESP-IDF's export refuses a Git Bash shell** ("MSys/Mingw is not supported"), and under
  Windows PowerShell 5.1 on the bench machine its execution policy stops the script from loading
  at all. The tools run it with the MSYS variables removed, under `pwsh` where it is installed,
  and say 127 when `idf.py` did not
  come out of the export: PowerShell's own `exit` of an unset `$LASTEXITCODE` is 0, and a build
  that never ran was first reported built.
- **`idf.py size` configures again.** It re-runs CMake on the build directory; run it on one that
  can be rebuilt.

## The tools

| Command | Does | Writes a board |
|---|---|---|
| `bench.py boards`, `status [--probe]` | the table; every lock, overdue or not, and each board's state | no |
| `bench.py lock take\|show\|release` | one board's lock | no |
| `bench.py log`, `snapshot`, `console`, `scrub` | what a board says, masked; the serial console (needs `pyserial`) | no |
| `bench_flash.py build PROFILE --tag T [--wifi]` | the profile's build, sized and held to it, as CI does it | no |
| `bench_flash.py flash BOARD --build-dir D --session ID [--app-only \| --otadata-only] [--erase]` | esptool, after the lock and a MAC check | yes |
| `bench_flash.py verify BOARD --build-dir D [--profile P]` | what the board says against the build | no |
| `bench_flash.py ota BOARD --build-dir D --session ID` | `ota.py push`, with the board's address and lock | yes |
| `bench_flash.py reset BOARD --session ID` | a system reset by esptool's watchdog reset after the MAC check, then waits for the board | resets it |
| `board_suite.py run --board B ...` | the play list, held to the baseline; `--restart --reset-by esptool --boots N` boots a P4 by watchdog reset; a row with `via sendspin` is played by `hearth-testserver` ([below](#sendspin-rows)) | plays |

`flash` reads the MAC of the chip on the port first and writes **nothing** unless its last three
octets are the table's (`mac_tail`): a port that moved to another board when a cable was changed
is the case it is for. `--erase` also wipes the board's pairing records and stored network, and
says so.

## Sendspin rows

A row of `firmware/hearth-sink/suite.json` with `"via": "sendspin"` is played by the repository's
`hearth-testserver`, not by `POST /play`: the C6 refuses an HTTP play on purpose (#1395), so
Sendspin is its audio path, and a baseline that does not play it holds nothing of what the part
is for. The harness is `tools/hearth/suite_sendspin.py`; the board's own `sendspin.stream=` line,
printed when the stream ends, is read the way a `/play`'s `result=` line is.

**The server** is built from the tree under test into the bench's directory, once per change to
`libs/sendspin` or `apps/hearth/testserver` (about twenty minutes), and `board_suite.py run`
finds it by `--testserver`, then `ICLFORGE_TESTSERVER`, then `<locks>/suite-testserver/bin/`. A
run that has a Sendspin row and no server stops before it books the board:

```powershell
$env:VCPKG_ROOT = 'D:/vcpkg'
cmake --preset config-windows-msvc -B D:/ac3bld/suite-testserver -DICLFORGE_BUILD_GUI=OFF `
    -DICLFORGE_BUILD_TESTS=OFF -DICLFORGE_BUILD_CLI=OFF -DICLFORGE_BUILD_EXAMPLES=OFF
cmake --build D:/ac3bld/suite-testserver --target hearth-testserver
```

**What the harness does to the board**, which a row's author should know before adding one:

- **One identity.** The server's key and pairing records are in a state directory
  (`<locks>/suite-sendspin-state`, or `--sendspin-state`). A fresh directory is a new server to the
  board, which is a new pairing record each run: eight of them once pushed every other server off
  a C6. Keep the directory.
- **The pairing code is relayed from `GET /status`** (`sendspin.pairing_code`) into the file the
  server reads after it asks, for the first 25 seconds only, and a code the board kept from a
  pairing that failed before the run is not used.
- **`--code-log` is given only to a board that does not hold the suite's record.** The server asks
  a board to pair at the dial whenever it has one, and a board that holds the record then shows a
  code nobody enters: the second row of a run waited 99 seconds and failed that way. The
  harness reads `GET /pairing` and decides; a stored record the board refuses is forgotten and
  the row played again by a code, with a note.
- **The suite's record is forgotten at the end**, by its `server_id` (`POST /pairing`, `forget
  <id>`), never a bare `forget`, which drops the owner's Hearth. A record that was there before
  the run and is not the suite's name is left. `--keep-pairing` keeps it.
- **The board is not polled while it plays.** The C6's E-AC-3 play had 2.6 to 3.2 KB of internal
  heap at its least (19 to 31 KB since the decoder's feature tables left static RAM, PR 16), and
  its HTTP server answers from that heap: with the server polling `/status` every second and a
  relay every 0.4 s, the same play rebooted the board once in four. The server is started without
  `--status`, the totals come from the log afterwards, and the decode stack from one
  `GET /status` after the stream.
- **A C6 that has more free RAM refuses fewer plays.** `CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES`
  is a check on free RAM, and when PR 16 gave the C6 28 KB back the 134,008 it had been sized to
  stopped refusing: the suite's `*-refused` rows started an HTTP play, AC-3 2.0 ran beside Wi-Fi
  buffer allocations failing at 6 to 14 KB free, and the harness died on a `/status` that timed
  out with the play still running. A change that gives a board memory back is a change to that
  figure too. The floor is 160,796 now (planning/esp32-platform-profiles.md, "The C6's static RAM
  budget"); `bench.py status --probe` showing `/status playing` on a board nobody booked is how a
  play left behind looks.
- **Music Assistant is displaced** while a stream plays: it holds a board by unpaired access (PSK
  `sentinel`), and the server pairs at the dial and wins. It has to dial the board again.

## What was in `D:\ac3bld\esp-reflash`, and where it went

| Was | Is |
|---|---|
| `build_one.ps1`, `deploy_build_chip.ps1` | `bench_flash.py build`, fragments from `sink_profile.py defaults` |
| `flash_board.ps1`, `deploy_usb_flash_p4_board2.ps1` | `bench_flash.py flash`, `--app-only` for the P4b |
| `verify_board.ps1` | `bench_flash.py verify`; **not carried**: the mDNS lookup (`dns-sd`) and the OTA round trip, which `ota.py` does on its own |
| `capture.py`, `peek_console.py` | `bench.py console` |
| `scrub.py`, `snap_board.py` | `bench.py scrub`, `snapshot` |
| its own copy of `ota.py` | [`ota.py`](../tools/hearth/ota.py), which is the newer |
| `strings_check.py`, `usb-diag/` | left: the first was the rename's one-off check, the second a USB-hub investigation |

The scripts in `D:\ac3bld` are not removed; other sessions' own helpers are there too.

## Changing the bench

A board added, moved or re-addressed is an edit to `bench.json` and to the table above in one PR;
`test_bench.py` holds the lock names to the owner's rule, the addresses, ports, locks and MAC tails
to be unique, every board a profile names to be on the bench with the profile's chip, and this
page's table to the file.
