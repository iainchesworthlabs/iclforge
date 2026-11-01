# truehd-probe

The TrueHD decoder of `libs/truehd` on an ESP32 chip, measured. It decodes eight fixtures of
`testdata/truehd` from flash, checks each decode against the PCM hash FFmpeg's strict decode gives
(`testdata/truehd/manifest.json`, `pcm_sha256`: the 24-bit sample shifted left by 8 as 32-bit little-endian,
channels interleaved), and prints, as `key=value` lines on the console, what each cost: microseconds and CPU
cycles an access unit (the mean and the slowest of the least of four passes), the real-time factor, the heap
the decoder holds and the stack the task reached. The last line is `result=pass` or `result=fail`.

No network, no I2S, no PSRAM: the numbers are the decoder's alone, as `firmware/baremetal`'s probe gives them
for the AC-3 decoder. The decoder's sources are compiled `-O2` (`TRUEHD_PROBE_OPT=Os` for the size build).
Results from the boards are in `results/`; the analysis is in
`planning/truehd-parity-esp32-sinks.md`.

## Build

```
. D:\esp\esp-idf\export.ps1            # with IDF_TOOLS_PATH=D:\esp\tools
$b = 'D:\esp\build\iclforge\truehd-probe-esp32c6'
idf.py -B $b -DSDKCONFIG="$b\sdkconfig" -DIDF_TARGET=esp32c6 "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.c6" build
```

`sdkconfig.s3` and `sdkconfig.p4` are the other two chips. Always pass `-DSDKCONFIG`: without it `idf.py`
writes `sdkconfig` into this directory and the next chip's build refuses it. The stack of the decoding task
is `CONFIG_TRUEHD_PROBE_TASK_STACK_BYTES`, 128 KB by default: the decoder reaches 60 KB on the RISC-V chips and a
48 KB stack faults on the C6 (and hangs the S3 without a message).

Two diagnostic overlays, added after the defaults: `sdkconfig.bisect` prints each step of the first unit, and
`sdkconfig.trace` compiles the decoder with `-finstrument-functions`, turns C++ exceptions on and, when one is
thrown, prints the last forty functions entered (resolve them with the toolchain's `addr2line` on the `.elf`).

## Run on a board, and put the board back

The image is written to `ota_0` of the sink partition table with `otadata` erased, so the chip boots it and
nothing else on the board is touched: the bootloader, the NVS (name, network, pairings) and `ota_1` stay. Take
the board's lease first (`python tools/hearth/bench.py lock take <board>` and `D:\ac3bld\board-<name>.lock`,
`D:\ac3bld\BOARD-LOCKS.md`), check the MAC against `tools/hearth/bench.json`, and **back up what is replaced**,
all of `ota_0` (4 MB, the stale slot is full of an old image) and `otadata`:

```
python -m esptool --port COMx read-flash 0x10000 0x2000 backup\otadata.bin
python -m esptool --port COMx read-flash 0x20000 0x400000 backup\ota0_full.bin
python -m esptool --port COMx erase-region 0x10000 0x2000
python -m esptool --port COMx write-flash 0x20000 <build>\iclforge_truehd_probe.bin
python capture.py COMx --seconds 400 --out result.txt
```

`capture.py` reads the console until it prints `result=`, holding DTR and RTS low so that opening the port does not
reset the chip. Put the board back, then read both regions again and compare their hashes with the backup:

```
python -m esptool --port COMx write-flash 0x10000 backup\otadata.bin 0x20000 backup\ota0_full.bin
```

then `python tools/hearth/bench.py status --probe` shows the board answering `/status`. Release the lease.

Do not leave a GDB session open on a board's USB-Serial-JTAG port, and do not kill one: on the S3 it left the port
refusing every write until the cable was replugged.
