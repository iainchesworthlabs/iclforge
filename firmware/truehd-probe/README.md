# truehd-probe

The TrueHD decoder of `libs/truehd` on an ESP32 chip, measured. It decodes eight fixtures of
`testdata/truehd` from flash, checks each decode against the PCM hash FFmpeg's strict decode gives
(`testdata/truehd/manifest.json`, `pcm_sha256`: the 24-bit sample shifted left by 8 as 32-bit little-endian,
channels interleaved), and prints, as `key=value` lines on the console, what each cost: microseconds and CPU
cycles an access unit (the mean and the slowest of the least of four passes), the real-time factor, the heap
the decoder holds and the stack the task reached. The last line is `result=pass` or `result=fail`.

No network, no I2S, and no PSRAM unless an overlay turns it on (`sdkconfig.s3mem`, below): the numbers are the
decoder's alone, as `firmware/baremetal`'s probe gives them for the AC-3 decoder. The decoder's sources are
compiled `-O2` (`TRUEHD_PROBE_OPT=Os` for the size build). Results from the boards are in `results/`.

## Build

```
. D:\esp\esp-idf\export.ps1            # with IDF_TOOLS_PATH=D:\esp\tools
$b = 'D:\esp\build\iclforge\truehd-probe-esp32c6'
idf.py -B $b -DSDKCONFIG="$b\sdkconfig" -DIDF_TARGET=esp32c6 "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.c6" build
```

`sdkconfig.s3` and `sdkconfig.p4` are the other two chips. Always pass `-DSDKCONFIG`: without it `idf.py`
writes `sdkconfig` into this directory and the next chip's build refuses it. The stack of the decoding task
is `CONFIG_TRUEHD_PROBE_TASK_STACK_BYTES`, 128 KB by default: the decoder reached 60 KB on the three chips
(`stack_used_bytes` 60,356 to 60,944 in the earlier S3 and C6 results files, 60,368 in the P4's) and a 48 KB stack
faults on the C6 (and hangs the S3 without a message). With the unit-cost change (draft PR #1815) the S3 and C6 results here show stack_used_bytes of 9,272 to 9,280 and
8,804, and the S3 heap_internal_decoder_bytes is 188,740 where the earlier results file had 158,020. The probe's stack is
still 128 KB.

### The S3 with the sink's memory settings

```
$b = 'D:\ac3bld\perf-c5\probe-s3-mem'
idf.py -B $b -DSDKCONFIG="$b\sdkconfig" -DIDF_TARGET=esp32s3 "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.s3;sdkconfig.s3mem" build
```

`sdkconfig.s3mem` is an overlay after `sdkconfig.s3`: a 32 KB instruction cache, a 64 KB data cache with 64-byte
lines, and the code and constants executed from PSRAM, which are the Hearth sink's S3 settings
(`firmware/hearth-sink/sdkconfig.s3-fast`, `sdkconfig.s3-dcache` and the cache line of `sdkconfig.psram`) without
`CONFIG_ESPTOOLPY_FLASHMODE_QIO`, which is the bootloader's and the probe does not replace the bootloader. PSRAM is
mapped and kept out of `malloc` (`CONFIG_SPIRAM_USE_CAPS_ALLOC`), so the decoder's heap stays internal, as in the
results without the overlay. The file says what each line is for and what it costs in internal RAM. It takes 55,112
bytes of the internal heap free at start (276,820 to 221,708, line 4 of the two S3 results files).

What it measured on one ESP32-S3 (hearth-47b39c, 240 MHz, DIO flash), one run of each image, the decoder of draft
PR #1815, PCM matching the reference hash in every fixture (`results/esp32s3.txt` without the overlay,
`results/esp32s3-mem.txt` with it); the decode time over the audio's time, below 1 keeps up:

| | 5.1, 24 bit | 5.1, 16 bit | 2.0 at 192 kHz | slowest unit, 5.1 24 bit | least internal heap free |
|---|---:|---:|---:|---:|---:|
| `sdkconfig.s3` alone | 1.095 | 0.970 | 1.706 | 2,598.74 us | 88,064 bytes |
| with `sdkconfig.s3mem` | 0.931 | 0.823 | 1.573 | 1,105.94 us | 32,952 bytes |

A 5.1 unit at 48 kHz is 40 samples, 833 us of audio, so the slowest unit with the overlay (1,106 us) is longer than
the unit it decodes while the mean (776 us) is shorter: a player needs the ring to ride out the slow ones. The
probe has no Wi-Fi and no I2S; Wi-Fi's buffers come out of the same internal heap, and this is the decoder
alone. What a play of 5.1 through the sink does with these settings has not been measured.

On main's decoder (`57fef9af0`) the larger caches alone took 5.1 at 24 bit from 1.499 to 1.094, and the caches with the
PSRAM lines aborted in the first decode and rebooted in a loop; those captures are not kept here, and why was not
investigated.

To read the console after a flash: on that S3, `esptool --after watchdog-reset` left the probe's console silent
for the whole capture, and `--after hard-reset` printed at once. Use `hard-reset`.

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
