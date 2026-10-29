# crypto-bench

SHA-256, HMAC-SHA-256 and the Sendspin transport AEAD, timed on an ESP32 board in CPU cycles: through
the PSA Crypto API the firmware uses (which ESP-IDF routes to the chip's SHA and AES peripherals),
against `iclforge::base::crypto`'s portable C++ as the software reference. It also checks, on the
chip, that the PSA answers equal the portable ones bit for bit, and measures the stack and transient
heap each operation needs beyond an empty call. `main/crypto_bench.cpp` has the method and the
output format; `tools/hearth/crypto_bench_report.py` turns a capture into the comparison tables.

It is a measurement tool, not firmware. It prints to the USB Serial/JTAG console, starts no Wi-Fi and
serves no HTTP, so a board running it is off the network until it is put back.

## Build

```
. $IDF_PATH/export.ps1
idf.py -B D:/esp/build/iclforge/crypto-bench-esp32s3 "-DSDKCONFIG=D:/esp/build/iclforge/crypto-bench-esp32s3/sdkconfig" set-target esp32s3
idf.py -B D:/esp/build/iclforge/crypto-bench-esp32s3 "-DSDKCONFIG=D:/esp/build/iclforge/crypto-bench-esp32s3/sdkconfig" build
```

The same for `esp32c6` and `esp32p4`. Give each target its own build directory and its own
`SDKCONFIG`: they share this source directory, and `sdkconfig.defaults.<target>` is read when a
build's `sdkconfig` is first made. The P4 settings are those of the pre-production revision 1.x
boards (360 MHz); a revision 3 kit takes `firmware/hearth-sink/sdkconfig.p4-rev3`'s lines instead.

## On a Hearth board

The project uses the Hearth sink's partition table, so a build's `app` and `otadata` are at the
offsets the boards already have, and `bench_flash.py flash --app-only` writes those two and leaves
the bootloader, the table and NVS (the board's pairings and stored network) alone. The rules for the
boards are `planning/esp32-board-bench.md`: book the board, and put back what was on it.

1. `bench.py status --probe`, then `bench.py lock take BOARD --session ID --purpose "crypto bench"`
   (and the older lease file `D:\ac3bld\board-<name>.lock`, whose names for the S3s are the other
   way round: planning/esp32-board-bench.md and D:\ac3bld\BOARD-LOCKS.md). `bench.py snapshot BOARD`
   and `GET /layout`, `GET /slot-width`: **the layout and the slot width are runtime settings, not
   in the flash, and any reset - the read in step 2 included - puts them back to 2.0 and 32.**
2. Read `otadata` (0x10000, 0x2000) and the first megabyte of `ota_0` (0x20000, 0x100000) with
   `esptool read-flash`, and keep them: that is everything step 3 overwrites.
3. `bench_flash.py flash BOARD --build-dir <this project's build> --session ID --app-only`. It then
   waits for an HTTP answer this image does not give, and exits 1; the write was verified.
4. `bench.py console BOARD --seconds 70 --out capture.log`. A pass prints every 20 s.
5. Restore: a directory holding those two dumps and a `flasher_args.json` naming them at their
   offsets (`app` for the `ota_0` dump, `otadata`), then `bench_flash.py flash BOARD --build-dir
   <that directory> --session ID --app-only`. The board answers again on the version it ran.
   `PUT /slot-width` and `PUT /layout` with the values from step 1, then read both back.
6. `bench.py lock release BOARD --session ID`, and remove the lease file.

## The P4X kit: no USB, over OTA

The kit (chip v3.2, `bench.json` name `P4X`) is usually on Ethernet and PoE with nothing on its USB
connector, so none of the above reaches it. It takes an OTA update, and its bootloader has app rollback
(`firmware/hearth-sink/partitions.csv`): an image put on by OTA is on trial until it marks itself valid,
and the bench never does, so any reset - a crash, a hang, a power cut - boots the image that was
running. Build it with the report over Ethernet and the revision-3 overlay:

```
idf.py -B D:/esp/build/iclforge/crypto-bench-esp32p4x-ota "-DSDKCONFIG=D:/esp/build/iclforge/crypto-bench-esp32p4x-ota/sdkconfig" \
    "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.p4-rev3" -DCRYPTO_BENCH_REPORT=ethernet set-target esp32p4
idf.py -B D:/esp/build/iclforge/crypto-bench-esp32p4x-ota "-DSDKCONFIG=D:/esp/build/iclforge/crypto-bench-esp32p4x-ota/sdkconfig" build
```

That build carries the project name `iclforge_hearth_sink` (the sink's update takes no other: `refuse_image`
in libs/device/include/iclforge/firmware_image.hpp) and the version `crypto-bench-ota`. It is not the sink.

1. Book the kit (the lease and `bench.py lock take P4X`; `D:\ac3bld\BOARD-LOCKS.md` says who to tell) and
   record `GET /hardware`, `/firmware`, `/layout`, `/slot-width`. **The update overwrites the other slot**:
   read `/firmware` first and see what is in it.
2. `bench_flash.py ota P4X --build-dir <that build> --session ID`. `ota.py` waits for the new image to
   accept itself, which this one never does, so it ends with an error after its timeout; the upload is what
   counts.
3. The bench runs one pass (about two minutes), brings up the Ethernet port and serves the pass on TCP port
   8080 of the address DHCP gives the cable: `curl http://<address>:8080/ > capture.log`. The pass is also
   on the USB console if one is attached.
4. `curl http://<address>:8080/restore` sends the board back to its own image at once. Without it the board
   goes back three minutes after the first fetch, and fifteen minutes after it started whatever happens.
5. Check `GET /hardware` for the version the kit ran before, put `PUT /slot-width` and `PUT /layout` back
   (a reset returns them to the defaults), release both locks.

## Report

```
python tools/hearth/crypto_bench_report.py --board esp32s3=s3.log --board esp32c6=c6.log \
    --board esp32p4=p4.log --board esp32p4x=p4x.log --pc crypto_bench_pc.json
```

The PC half is `iclforge-cryptobench` (tests/performance/crypto_bench.cpp) with `--json-out`.
