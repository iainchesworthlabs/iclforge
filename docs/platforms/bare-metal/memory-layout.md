# ESP32 memory layout

Where the flash and the RAM go on the three ESP32 development kits that run the Hearth sink
firmware (`hearth_sink`): an ESP32-S3-DevKitC-1-N16R8, an ESP32-C6 with 4 MB or 16 MB of flash,
and an ESP32-P4 of chip revision v1.x, or v3.x with the lyric panel. This page is for a developer
who is porting the firmware to a board, tuning one or debugging one. It summarises figures that
the board pages ([ESP32-S3](esp32-s3.md), [ESP32-C6](esp32-c6.md), [ESP32-P4](esp32-p4.md)) and
the sink guides measure and explain, and it links to each measurement.

## Reading the figures

Every image here is built from a **profile**: a locked list of `sdkconfig` fragments, the values
of the keys that decide where memory goes, and the budgets the image is held to.
[`profiles.json`](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/profiles.json)
holds the seven profiles, and
[the firmware's README](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/README.md#build-profiles)
lists them. A value on this page that comes from a profile is a setting. A value with a build and
a date beside it is a measurement.

The terms:

- **Static internal RAM** is what `idf.py size` reports as `DIRAM`: the code ESP-IDF places in
  RAM (interrupt handlers and what runs while the flash cache is off), `.data` (initialised
  variables) and `.bss` (variables that start at zero). It is taken before `app_main` runs.
- **The internal heap** is what is left of internal RAM for `malloc`, task stacks, Wi-Fi's buffers
  and the decoder. "Free" is `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.
- **The largest free block** is the largest single allocation the internal heap can make. The
  heap is in regions and fragments as it is used, and a task's stack is one allocation, so this
  figure decides whether a stack or a ring can be made.
- **PSRAM** is external RAM behind the data cache. With `CONFIG_SPIRAM_USE_MALLOC`, `malloc`
  places a block by its size: under `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` bytes it tries internal
  RAM first, at or over that it tries PSRAM first, and either falls back to the other.
- **A baseline** is the board suite's record for a profile, in
  [`firmware/hearth-sink/baselines/`](https://github.com/iainchesworthlabs/iclforge/tree/main/firmware/hearth-sink/baselines):
  a bench board restarted eight times, then five plays of each stream. Its boot figures are the
  board's `budget:` line, printed once Wi-Fi and the Sendspin player are up, and a range on this
  page is the range over the eight boots.

The baselines the tables cite, with the image each was recorded on (the dates are UTC):

- `s3-n16r8`: board S3a, `v0.10.0-beta.1-5587-g2a14b2b96`, 2026-10-29, with the AC-4 decoder and the 64 KB data cache (until then the
  release image had neither, recorded 2026-10-12 at `v0.10.0-beta.1-4565-gc3878ebaa`, and `s3-n16r8-ac4` was its own profile, S3a,
  `v0.10.0-beta.1-4723-ge3da66cb3`, 2026-10-12: the figures below marked `s3-n16r8-ac4` are that profile's).
- `c6-16mb`: the C6 board, `v0.10.0-beta.1-4738-g357812dae`, 2026-10-12.
- `p4-rev1` (until 2026-10-29 `p4-rev1-ac4`): board P4a, `v0.10.0-beta.1-4723-ge3da66cb3`, 2026-10-12.

`c6-4mb` and `p4-rev3` have no bench board and no baseline. The `p4-rev3` image has not run on a
v3.x chip or on a panel: its figures are a build's, and the screen's cost was measured on a v1.3
board with no panel.

## Side by side

| | ESP32-S3 (N16R8) | ESP32-C6 | ESP32-P4 |
|---|---|---|---|
| Profiles, and the release image of each | `s3-n16r8` (`hearth-sink-esp32s3`) | `c6-4mb` (`hearth-sink-esp32c6`); `c6-16mb` (`hearth-sink-esp32c6-16mb`) | `p4-rev1` (`hearth-sink-esp32p4-rev1`); `p4-rev3` (`hearth-sink-esp32p4-rev3`, with the panel) |
| Cores and decode arithmetic | two Xtensa LX7 at 240 MHz with a single-precision FPU; `float` | one RISC-V at 160 MHz with no FPU; fixed point | two RISC-V at 360 MHz (v1.x) or 400 MHz (v3.x) with a single-precision FPU; `float` |
| Internal RAM | 512 KB | 512 KB, shared with the radio | 768 KB, and 32 KB of low-power SRAM that the profiles keep out of the heap |
| PSRAM | 8 MB, octal, 80 MHz | none | 32 MB, hex, 200 MHz |
| Flash | 16 MB, QIO | 4 MB or 16 MB, QIO | 16 MB, QIO at 80 MHz |
| Partition table and application slots | `partitions.csv`, two of 4 MiB | `partitions_c6.csv`, two of 1.875 MiB (4 MB); `partitions.csv` (16 MB) | `partitions.csv`, two of 4 MiB |
| Bootloader's window | 32,768 bytes from 0x0 | 32,768 bytes from 0x0 | 24,576 bytes from 0x2000 |
| Where the code runs | PSRAM, copied there at start: 2.3 MB of the 8 | flash, through the cache | PSRAM, copied there at start |
| Caches | instruction 32 KB; data 32 KB in 64-byte lines, 64 KB with the AC-4 decoder | 16 KB for code and data together, in 32-byte lines | 16 KB instruction cache and a 128 KB L2 cache in 64-byte lines |
| Wi-Fi | its code in RAM (ESP-IDF's IRAM options on); its buffers try PSRAM first | its code in flash (five IRAM options off) | on the board's ESP32-C6 co-processor; the SDIO link's buffers in PSRAM |
| Static internal RAM, and its budget | 176,854; 185,000 | 111,823; 117,000 | 166,948 for `p4-rev3`; 175,000. No figure for v1.x |
| Internal heap free at boot, Wi-Fi and the Sendspin player up | 85,667 to 102,503; 66,159 to 82,771 with AC-4 | 136,668 to 140,304 | 291,783 to 291,803; 295,671 to 312,299 with AC-4 |
| Largest free block then | 31,744 on every boot | 118,784 to 124,928 | 217,088 to 221,184; 208,896 to 212,992 with AC-4 |
| `malloc` tries internal RAM first under | 16,384 bytes; 512 bytes during an AC-4 play | every size: there is nowhere else | 16,384 bytes |
| Decode task's stack | 32,768 bytes in PSRAM; 40,960 with AC-4 | 32,768 internal for an HTTP play; 24,576 internal for a Sendspin stream | 32,768 internal; 40,960 with AC-4 |
| Sendspin ring | 262,144 bytes in PSRAM | 49,152 bytes, internal | 262,144 bytes in PSRAM |
| What a play is refused under | AC-4: 286,365 bytes of heap. No floor for AC-3 and E-AC-3 | an HTTP play: 160,796 bytes of internal RAM. A Sendspin stream: more than 2 coded channels. AC-4: 286,365 bytes of heap | AC-4: 286,365 bytes of heap. No floor for AC-3 and E-AC-3 |
| Least internal heap free, AC-3 2.0 over Sendspin | 58,483 | 56,104 | 253,939 |
| Least internal heap free, E-AC-3 2.0 over Sendspin | 2,935 | 25,836 | 203,315 |

The heap rows are the baselines' (`s3-n16r8`, `c6-16mb` and `p4-rev1`, and the two AC-4 profiles
where the row says so); the static RAM row is `idf.py size` on builds of 2026-10-12 and
2026-10-13. Each board's section has the sources.

## The flash map

Every board has two application slots, `ota_0` and `ota_1`, and an `otadata` partition that says
which of them boots. The bootloader is below the partition table, which is at 0x8000: from 0x0 on
the S3 and the C6, and from 0x2000 on the P4.

```text
partitions.csv: 16 MB of flash (the S3, the P4, and a C6 built with sdkconfig.flash16mb)

name      type  offset    size                 what
nvs       data  0x009000  0x006000    24 KiB   the board's name, network and pairings
phy_init  data  0x00F000  0x001000     4 KiB
otadata   data  0x010000  0x002000     8 KiB   which application slot boots
ota_0     app   0x020000  0x400000     4 MiB   application slot; a USB flash writes this one
ota_1     app   0x420000  0x400000     4 MiB   application slot
coredump  data  0x820000  0x010000    64 KiB   a panic's core dump (the S3 image writes none)
audio     data  0x830000  0x040000   256 KiB   the sample stream the partition source reads
storage   fat   0x870000  0x040000   256 KiB   the FAT volume of the fatfs source
reserve   data  0x8B0000  0x400000     4 MiB   unused; the table ends at 0xCB0000

partitions_c6.csv: 4 MB of flash (any C6 module)

nvs       data  0x009000  0x006000    24 KiB
phy_init  data  0x00F000  0x001000     4 KiB
otadata   data  0x010000  0x002000     8 KiB
ota_0     app   0x020000  0x1E0000  1.875 MiB
ota_1     app   0x200000  0x1E0000  1.875 MiB
coredump  data  0x3E0000  0x010000    64 KiB   the table ends at 0x3F0000; no audio, no storage
```

The two files are
[`partitions.csv`](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/partitions.csv)
and
[`partitions_c6.csv`](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/partitions_c6.csv);
[the OTA plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-ota.md)
has the reason for each line.

- **An update over the network writes the slot that is not running**, and the new image boots on
  trial. A reset before the image accepts itself boots the other slot again
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
  [Sink firmware](../../hearth/sink-firmware.md#what-the-board-does-during-an-update) has the
  sequence.
- **A USB flash writes** the bootloader, the partition table, an empty `otadata`, the application
  into `ota_0`, and `audio` and `storage`. An empty `otadata` boots `ota_0` with nothing on trial.
- **Nothing writes `nvs`**, which is at 0x9000 in both tables, so a board keeps its name, its
  network and its pairings through a USB flash and through an update.
- **The partition table, the flash size setting and the bootloader change only over USB.** The
  flash mode (DIO or QIO) is the bootloader's, so it changes only over USB too.

## Heap floors, and how a refusal is reported

A decoder that cannot allocate calls `abort()`, which restarts the board and ends its Sendspin
session. The floors refuse a play before that point. Each is the least a play can need, and a
wider stream asks for more.

- **The boot budget line.** After the network is up and the Sendspin player has started, the
  board prints `budget: when=boot net=1 internal_free=... largest=... ask_ring=...
  ask_decode_stack=... ask_fetch_stack=... ask_total=... ask_biggest=... headroom=... floor=...
  verdict=...`. The asks are the ring and the stacks an HTTP play takes from internal RAM in this
  build, and `headroom` is the largest free block less the biggest ask. `verdict` is `ok`,
  `short-block` (no free block holds the biggest ask), `short-total` (not that much free in all)
  or `below-floor` (the HTTP play floor would refuse).
- **The HTTP play floor**, `CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES`: 160,796 in both C6
  profiles and 0 in the others. A `POST /play` that finds less internal RAM free fails at once:
  `GET /status` has `"failed": true`, `"why": "memory"` and the bytes that were free in `error`,
  and the console has `player: a play here needs 160796 bytes of internal RAM free and N are:
  refused (CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES)`. A Sendspin stream is not tested by it.
- **The AC-4 floor**: 286,365 bytes of heap, PSRAM counted, which is the least any AC-4 stream
  has needed (the 2.0 probe fixtures' peak). Under it an HTTP play fails with `"why": "memory"`
  and `player: AC-4 needs at least 286365 bytes of heap and N are free: refused`. A Sendspin
  stream's chunks are refused with the same line under the name `burst player:`. It came with
  [#1233](https://github.com/iainchesworthlabs/iclforge/pull/1233), and in practice a part with
  no PSRAM is the one under it.
- **Coded channels over Sendspin**, `CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS`: 2 on
  the C6. A wider syncframe is refused before a decoder is made for it, the console says
  `burst player: refusing a 6-channel syncframe: more than the 2 this part decodes in this build`
  once a stream, and each refused chunk counts as invalid.
- **A lyric document** is taken only while internal RAM has three times its size and 8,192 bytes
  free (twelve times its size when PSRAM has no room for it). Otherwise the console says
  `sendspin: lyrics not taken: N bytes of internal heap free, M needed` and the stream plays
  without lyrics.
- **An allocation that fails anyway** prints `heap: <function> could not allocate N bytes (caps
  0x...); internal free X, largest Y` before the abort. `tools/checks/check_esp_console.py`
  fails a console capture that has one.

Under QEMU, CI holds the S3's stream set and its Sendspin shape to 20,480 bytes of internal RAM
free at the least, and the decode lane's shape to 12,288 ([ESP32-S3](esp32-s3.md#hearth-sink)).

## ESP32-S3 (N16R8)

The ESP32-S3-DevKitC-1-N16R8: 16 MB of flash and 8 MB of octal PSRAM. Internal RAM is what this
board is short of. Wi-Fi, lwIP, two HTTP servers, mDNS, the Sendspin session and the decoder
share it, and with Wi-Fi up no free block is over 31,744 bytes.

| | Figure | Build and date |
|---|---|---|
| Flash | 16 MB in QIO mode (`sdkconfig.s3-fast`). `partitions.csv`, 4 MiB slots. Budgets: application 1,750,000 bytes (raised from 1,600,000 on 2026-10-25), bootloader 32,768 | `profiles.json`. QIO is worth 8 to 15% of an AC-4 frame and 11 to 19% of an AC-3 or E-AC-3 one: `hearth-eb2c64`, 2026-10-10 ([Playback speed](esp32-s3.md#playback-speed)) |
| Internal RAM | 524,288 bytes. 32,768 at 0x40370000 are on the instruction bus only: the instruction cache at its 16 KB minimum, and 16 KB of IRAM. 491,520 are addressable as data, and the data cache comes off the top of them | [Memory](esp32-s3.md#memory) |
| Caches | Instruction cache 32 KB (`sdkconfig.psram`), 16 KB over ESP-IDF's minimum and 16 KB of internal RAM. Data cache 64 KB in 64-byte lines (`sdkconfig.s3-fast`, `sdkconfig.s3-dcache`), which is 32 KB of the heap (the release image had 32 KB until 2026-10-29) | With the 64 KB cache an AC-4 play started with 62 KB free in place of 95: `hearth-eb2c64`, 2026-10-11 ([Playback speed](esp32-s3.md#playback-speed)) |
| Code and constants | Run from PSRAM (`CONFIG_SPIRAM_FETCH_INSTRUCTIONS`, `CONFIG_SPIRAM_RODATA`): 2.3 MB of the 8 MB. The fragment's three settings cost about 1 KB of internal RAM between them | `sdkconfig.s3-fast`, in the release image since 2026-10-11 |
| Static internal RAM | 176,854 bytes for `s3-n16r8`, budget 185,000. No source splits this image into `.data` and `.bss`. The default shape with no network is 88,563 bytes, 46,232 of it `.bss` | `idf.py size`, 2026-10-12 ([the profiles plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-platform-profiles.md#1-at-build-time-every-pr-no-board)). The default shape: [the README's cost table](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/README.md#what-it-costs), which gives no date |
| Heap with no network | 304,680 bytes free, largest block 241,664, in the footprint probe, which has a 16 KB instruction cache and a 32 KB data cache. IRAM is 16,384 of 16,384 used, so none of it is donated to the heap | QEMU leg, 2026-09-29; a board read 316,196 free on 2026-09-11 ([Memory](esp32-s3.md#memory), [Contiguity](esp32-s3.md#contiguity)) |
| Heap at boot, Wi-Fi and the Sendspin player up | `s3-n16r8`: 75,503 bytes free at the worst of eight boots (2026-10-29, with the decoder and the 64 KB cache; before it 85,667 to 102,503, and the old `s3-n16r8-ac4` 66,159 to 82,771) | Baseline `s3-n16r8` |
| Largest free block, Wi-Fi up | 31,744 bytes on all sixteen boots of the two baselines. A 32,768-byte stack in internal RAM cannot be made: such an image answered every `POST /play` with `could not allocate 32768 bytes` | Baselines; the failure on both boards, `main` `3d6b2aa0a`, 2026-10-12 (`sdkconfig.psram`) |
| What is in PSRAM | The code and constants. Both rings: 32,768 bytes for an HTTP play, 262,144 for Sendspin with its buffers. The decode task's stack for an HTTP play and for a Sendspin stream: 32,768 bytes, 40,960 with AC-4. The decode lane's stack (16,384) and its enhanced-coupling scratch (23 KB), and with AC-4 the second core's worker stack (32,768). Allocations of 16 KB and over. Wi-Fi's and lwIP's buffers, which try PSRAM first. The console's last 16 KiB. A lyric document of up to 2,048 bytes of text | `profiles.json`; [The decode stack goes in PSRAM](esp32-s3.md#the-decode-stack-goes-in-psram): the stack in PSRAM costs 4 to 5% of a 2.0 A-SPX frame and returns 28 KB of internal RAM, `hearth-eb2c64`, 2026-10-10 ([#1233](https://github.com/iainchesworthlabs/iclforge/pull/1233)) |
| The 512-byte rule | For the length of an AC-4 play the limit under which `malloc` tries internal RAM first is `CONFIG_ICLFORGE_AC4_INTERNAL_BELOW`, 512 bytes on this part. The player puts 16,384 back when the play ends. The decoder then keeps 3 to 14 KB in internal RAM and 0.42 to 1.83 MB in PSRAM. A limit of 4,096 bought 1 to 2% of a frame for 23 to 42 KB of internal RAM, and 8,192 or more left the part 0.4 to 5 KB | The split: the AC-4 probe under QEMU, with the decoder as it was before its peaks were cut ([Where its memory goes](esp32-s3.md#where-its-memory-goes)). The sweep: `hearth-eb2c64`, 2026-10-10 ([The 512-byte limit stays](esp32-s3.md#the-512-byte-limit-stays)) |
| What stays internal | The I2S queue, allocated by its capabilities: 12 descriptors of 256 frames, 24 KB at 2.0 in 32-bit slots. Allocations under the limit. Interrupt handlers and the code that runs with the flash cache off | [What stays in internal RAM, and why](esp32-s3.md#what-stays-in-internal-ram-and-why) |
| Core dump | None (`CONFIG_ESP_COREDUMP_ENABLE_TO_NONE`): it would take 4,016 bytes of internal RAM | `idf.py size`, 2026-09-25 (`sdkconfig.psram`) |
| Floors | No HTTP play floor. The AC-4 floor of 286,365 bytes is met by PSRAM. Boot budget: the one internal ask is the fetch task's 8,192-byte stack, so `headroom` is 23,552 and `verdict` is `ok` | Baselines |
| AC-3 5.1 | Decoder peak 56,597 bytes. Over Sendspin the least internal heap free was 21 to 79 KB | Peak: the probe in `float`, on a P4 board, 2026-09-23 ([What the part carries](esp32-p4.md#what-the-part-carries)). Sendspin: S3b on `main` `ce0e099db`, 2026-10-12, and the build before it ([AC-3 and E-AC-3 in the Hearth player](esp32-s3.md#ac-3-and-e-ac-3-in-the-hearth-player)) |
| E-AC-3 5.1 | Decoder peak 102,342 bytes with AHT, spectral extension and coupling; 129,657 with enhanced coupling. Over Sendspin the least free was 0.8 to 14 KB | As the row above |
| E-AC-3 7.1.4 | Decoder peak 167,386 bytes as coded, 173,794 folded to stereo, 195,025 with objects placed onto 7.1.4. Over Sendspin the least free was 23 to 1,100 bytes | Peak: QEMU leg, 2026-09-29, equal to the P4 board's. Sendspin: as above |
| AC-4 2.0 | Decoder peak 286,365 to 418,110 bytes. An HTTP play took 0.40 to 0.43 MB of PSRAM and left 77 to 80 KB of internal RAM at the least; with the 64 KB cache the plays of every layout left 31 to 49 KB. A Sendspin stream on the old `s3-n16r8-ac4` left 83 to 89 KB (on `s3-n16r8` since the fold, 47 to 74 KB at the least, 2026-10-29) | Peak: the probe ([Where its memory goes](esp32-s3.md#where-its-memory-goes)). Plays: `hearth-eb2c64`, 2026-10-10 and 2026-10-11 ([On the board](esp32-s3.md#on-the-board)). Sendspin: S3b, 2026-10-12 ([AC-4 from a server](esp32-s3.md#ac-4-from-a-server)) |
| AC-4 5.1 | Decoder peak 696,375 to 859,616 bytes. An HTTP play to 5.1 took 0.78 to 1.06 MB of PSRAM and left 72 to 78 KB; the decode stack reached 32,560 bytes in A-CPL mode 3. Over Sendspin the image states 2 channels and a server sends PCM | As the row above |
| AC-4 5.1.4 | Decoder peak 1,494,319 bytes. An HTTP play to 5.1.4 took 1.71 to 1.82 MB of PSRAM and left 62 to 75 KB. It fits and takes more than twice real time | As the row above; the time is on [Playback speed](esp32-s3.md#playback-speed) |

The "least free" figures are low because of where allocations go: those under 16 KB try
internal RAM first, so the AC-3 and E-AC-3 decoders and the network fill what is free and then
spill to PSRAM. Sending allocations over 4 KB to PSRAM left 80 KB free, a burst then took 28 ms
where it had taken 20, and the board underran
([In the Sendspin sink](esp32-s3.md#in-the-sendspin-sink), 2026-09-16). What is added to the
player's internal allocations is the risk, and
[the sink guide](../../hearth/sink-esp32-s3.md#memory) says how the console reports it.

## ESP32-C6 (4 MB and 16 MB)

One core, no FPU and no PSRAM: the code runs from flash, and the player's buffers, Wi-Fi's
receive buffers and the decoder come out of the same 512 KB. The two images differ in the
partition table alone. On this board the limit is memory, and the sink plays AC-3 and E-AC-3 of
up to two coded channels ([An ESP32-C6 sink](../../hearth/sink-esp32-c6.md)).

| | Figure | Build and date |
|---|---|---|
| Flash | QIO mode (`sdkconfig.c6`). `c6-4mb`: `partitions_c6.csv`, 1.875 MiB slots, application budget 1,966,080 bytes, which is the slot. `c6-16mb`: `partitions.csv` through `sdkconfig.flash16mb`, 4 MiB slots, application budget 2,000,000 (raised from 1,880,000 on 2026-10-25). Bootloader budget 32,768 | `profiles.json`. The image was 1,665,856 bytes on 2026-09-25, 91% of a 1.75 MiB slot ([the OTA plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-ota.md)). QIO left about 8% of the part idle where DIO left 1%: AC-3 2.0 over Wi-Fi, 2026-09-17 (`sdkconfig.c6`) |
| Cache | 16 KB for code and data together, in 32-byte lines. The fixed-point transform's tables are initialised data (`.data`) for that reason: as constants in flash they cost 15 thousandths of a burst (AC-3 656 to 671, E-AC-3 751 to 766) | The cache: ESP-IDF's ROM header for the part. The timing: the C6 board, `c6-16mb` ([#1511](https://github.com/iainchesworthlabs/iclforge/pull/1511), merged 2026-10-13) |
| Wi-Fi's code | In flash: `CONFIG_ESP_WIFI_IRAM_OPT`, `_EXTRA_IRAM_OPT`, `_RX_IRAM_OPT`, `_SLP_IRAM_OPT` and `CONFIG_ESP_PHY_IRAM_OPT` are off (`sdkconfig.sendspin-c6`). In the probe's Wi-Fi build that took the code in RAM from 93,190 to 44,804 bytes and the heap free at boot from 295,104 to 343,968, and AC-3 5.1 from 0.82 to 1.09 of real time | The probe, 2026-09-15 ([WiFi's code kept in flash](esp32-c6.md#wifis-code-kept-in-flash)) |
| Static internal RAM | 111,823 bytes for both images, budget 117,000: `.bss` 43,664, `.data` 21,655, and the rest code placed in RAM. It was 144,139 (`.bss` 81,736) before 27,776 bytes of the decoder's tables came out, and 116,307 (`.bss` 53,904) after | `idf.py size`, 2026-10-12 and 2026-10-13 ([#1474](https://github.com/iainchesworthlabs/iclforge/pull/1474), [#1511](https://github.com/iainchesworthlabs/iclforge/pull/1511), [the profiles plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-platform-profiles.md#the-c6s-static-ram-budget-was-wrong-pr-14)) |
| Heap with no network | 383,416 bytes free, largest block 352,256, in the footprint probe | The probe on a board, 2026-09-15 ([Memory](esp32-c6.md#memory)) |
| Heap at boot, Wi-Fi and the Sendspin player up | 136,668 to 140,304 bytes free, largest block 118,784 to 124,928 | Baseline `c6-16mb`. Before #1474 the worst boot of eight was 108,792 free and 88,064 largest (`v0.10.0-beta.1-4520-g26274cd77`, 2026-10-12) |
| What a Sendspin stream takes | A 49,152-byte ring, a 24,576-byte decode stack, chunks of at most 4,096 bytes, no delay lines and no lyric document (`sdkconfig.sendspin-c6`). A 64 KB ring left a largest block of 1,824 bytes, and two 1,848-byte Wi-Fi receive buffers could not be allocated | 2026-09-17 to 2026-09-20 (`sdkconfig.sendspin-c6`) |
| What an HTTP play takes | 134,008 bytes: the ring (32,768), the decode stack (32,768), the fetch stack (8,192), the I2S queue (12,672) and the smallest decoder (47,608, AC-3 1/0) | `sdkconfig.sendspin-c6` |
| I2S queue | 12 descriptors of 256 frames in 16-bit slots: 12,288 bytes and 384 of descriptors at 2.0, and 49 KB for eight TDM slots | `sdkconfig.sendspin-c6` |
| Core dump and log | A core dump is kept, for 1,140 bytes of static RAM. `GET /log` keeps the console's last 2 KiB | `idf.py size`, 2026-09-25 (`sdkconfig.defaults`) |
| Opus | While an Opus stream plays, 27,060 bytes of decoder state and about 16.4 KB of the 24 KB decode stack | The Hearth sink's image with Wi-Fi up ([Opus from a server](esp32-c6.md#opus-from-a-server), on that page since 2026-10-11) |
| Floors | An HTTP play needs 160,796 bytes of internal RAM free: the 134,008 above and the 26,788 that a Sendspin E-AC-3 stream left the network at the least. The board has 136,668 to 140,304 at boot, so every HTTP play is refused and the boot budget line says `verdict=below-floor`. A Sendspin stream of more than 2 coded channels is refused. An image built with the AC-4 decoder refuses an AC-4 play under 286,365 bytes | Baseline `c6-16mb`: both HTTP rows end `failed`, `why` `memory`. The floor was 134,008 until #1474 freed enough RAM for a play to start and fail |
| AC-3 2.0 | Decoder peak 49,328 bytes. Over Sendspin the least internal heap free was 56,104 | Peak: the probe, fixed point, 2026-09-15. Sendspin: baseline `c6-16mb` |
| E-AC-3 2.0 | Decoder peak 108,094 bytes on 2026-09-15; a 2.0 stream with coupling has held 24,576 bytes less since [#1494](https://github.com/iainchesworthlabs/iclforge/pull/1494). Over Sendspin the least free was 25,836 | Peak: the probe, fixed point. Sendspin: baseline `c6-16mb` |
| AC-3 5.1 and E-AC-3 5.1 | Decoder peaks 56,685 and 164,066 bytes (109,806 for E-AC-3 since). Both fit the probe with Wi-Fi up. In the sink each stopped within about 12 seconds on a failed allocation, and both are refused now | Peaks: 2026-09-15 ([Memory](esp32-c6.md#memory)). The sink: 2026-09-22; not run again since the decoder got smaller ([An ESP32-C6 sink](../../hearth/sink-esp32-c6.md)) |
| E-AC-3 7.1.4 | Decoder peak 227,662 bytes (174,850 on the CI probe since). It did not fit the probe's Wi-Fi build with ESP-IDF's IRAM options on, and is refused in the sink | 2026-09-15 ([ESP32-C6](esp32-c6.md)) |
| AC-4 | Decoder peak in fixed point: 286,365 to 426,918 bytes at 2.0, 704,311 to 868,424 at 5.1, 1,502,903 at 5.1.4. With the sink up a play started with 117,328 bytes free and is refused. A C6 is sent AC-4 as PCM | Peaks: the Cortex-M3 leg, 2026-10-03. The board: `hearth-db4c40`, 2026-10-10 ([AC-4](esp32-c6.md#ac-4), [#1233](https://github.com/iainchesworthlabs/iclforge/pull/1233)) |

Static RAM is what this board's heap is the remainder of. The 27,776 bytes that
[#1474](https://github.com/iainchesworthlabs/iclforge/pull/1474) took out were function-local
tables of coding tools a stereo stream never uses: enhanced coupling's (10,240 and 5,536),
transient pre-noise's (3,072) and the double MDCT tables that only the object decoder's
direct-form branches named (8,928). Before it, an E-AC-3 stream over Sendspin left 0.9 to 4.5 KB and failed after an AC-3
stream in the same boot.

## ESP32-P4 (v1.x, and v3.x with the panel)

The P4 has the most internal RAM and 32 MB of PSRAM, and no radio: Wi-Fi is the board's ESP32-C6,
over SDIO. Its two chip generations take two images that refuse each other's chips
([An ESP32-P4 sink](../../hearth/sink-esp32-p4.md#two-chips-two-images)). Everything measured
here is from a DFRobot FireBeetle 2, chip revision v1.3.

| | Figure | Build and date |
|---|---|---|
| Chip revision | `p4-rev1`: revisions v1.0 to v1.99, 360 MHz. `p4-rev3`: v3.1 to v3.99, 400 MHz, the lyric panel | `profiles.json`; `p4-rev3` has not run on a board ([The chip revision](esp32-p4.md#the-chip-revision-and-what-it-blocks)) |
| Flash | 16 MB in QIO mode at 80 MHz (`sdkconfig.p4`). `partitions.csv`, 4 MiB slots. `p4-rev3`'s application budget is 2,900,000 bytes (raised from 2,600,000 on 2026-10-25); 473 KB of that image is fonts | QIO took 14 to 25% off an AC-4 frame and 4 to 8% off AC-3 and E-AC-3: 2026-10-02 ([Flash mode](esp32-p4.md#flash-mode)). The `p4-rev3` build was 2,469,920 bytes on 2026-10-12 |
| Bootloader | In the 24,576 bytes from 0x2000 to the partition table. v1.x at the Info log level with QIO: 24,368 bytes, 208 to spare. v3.x is 25,152 at Info, so `p4-rev3` builds it at the Warn level: 22,448 | `sdkconfig.p4`; `sdkconfig.p4-rev3`, a build of 2026-10-12 |
| Internal RAM | 768 KB. The footprint probe, with no network and no PSRAM, had 514,820 bytes free at boot and a largest block of 385,024 | The probe on a board, 2026-09-23 ([Memory](esp32-p4.md#memory)) |
| The low-power SRAM | 32 KB at 0x50108000, reached with no cache: 174 to 195 cycles an access against 6 to 7 for main RAM. `CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=n` keeps it out of the heap, which is 32,640 bytes smaller. With it in, the 29.97 fps converter took 93.8 ms a frame; with it out, 12.0 | 2026-10-01 and 2026-10-02 ([The low-power SRAM](esp32-p4.md#the-low-power-sram), [#1176](https://github.com/iainchesworthlabs/iclforge/pull/1176)). The 8 KB of tightly coupled memory at 0x30100000 is still in the heap and was not measured |
| Caches | A 16 KB instruction cache and a 128 KB L2 cache in 64-byte lines. The firmware is copied into PSRAM at start and runs from there (`CONFIG_SPIRAM_XIP_FROM_PSRAM`): 1.4 MB of code and 0.8 MB of constants in the AC-4 image, 14 KB more image and no internal RAM | 14 to 36% less time a frame on one core: 2026-10-11 ([The firmware in PSRAM and the second core](esp32-p4.md#the-firmware-in-psram-and-the-second-core)) |
| Static internal RAM | 166,948 bytes for `p4-rev3`, budget 175,000. The v1.x profiles have no budget and no source gives their figure or a `.data` and `.bss` split | `idf.py size` on a build of 2026-10-12 ([the profiles plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-platform-profiles.md#profiles)) |
| Heap at boot, Wi-Fi and the Sendspin player up | `p4-rev1`, with the AC-4 decoder since 2026-10-29: 295,671 to 312,299 free, largest 208,896 to 212,992 (without it, as the profile was before: 291,783 to 291,803 free, largest 217,088 to 221,184) | The `p4-rev1` baseline, which is the old `p4-rev1-ac4`'s |
| What is in PSRAM | The firmware. Both rings: 32,768 and 262,144 bytes. Allocations of 16 KB and over. The SDIO link's buffers, about 31 blocks of 1,536 bytes (`CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`). The decode lane's stack (16,384). The console's last 16 KiB. A lyric document of up to 16,384 bytes of text | `profiles.json`, `sdkconfig.p4` |
| What stays internal | The decode task's stack: 32,768 bytes, 40,960 with AC-4, of which the decoder uses 19 to 30 KB. The I2S queue: 12 descriptors of 256 frames, 24 KB at 2.0; a sixteen-slot TDM frame would be 196 KB. With AC-4, the second core's worker stack for an HTTP play: 24,576 bytes. A core dump is kept, for 3,652 bytes of static RAM | Stack use: 2026-10-02 ([What the decoder holds](esp32-p4.md#what-the-decoder-holds)). The worker's stack: `firmware/esp-idf/iclforge/src/player.cpp`. Core dump: `idf.py size`, 2026-09-25 |
| Allocation policy | ESP-IDF's: under 16,384 bytes internal RAM first, and `CONFIG_ICLFORGE_AC4_INTERNAL_BELOW` is 16,384 here. A 512-byte limit left 281 to 298 KB of internal RAM free and took 1.08 to 1.24 times as long a frame | The twenty plays, 2026-10-02 ([Allocation policy](esp32-p4.md#allocation-policy)) |
| The panel (`p4-rev3`) | All in PSRAM: the frame buffer (1,228,800 bytes: 1024 by 600 at 16 bits), two render buffers of 81,920, LVGL's heap and glyph caches (about 100 KB for each of four faces at its largest), and the screen task's 16,384-byte stack | `sdkconfig.panel`, `profiles.json`. The drawing's cost was measured on the v1.3 board with no panel ([Memory and time](../../hearth/sink-esp32-p4.md#memory-and-time)) |
| Floors | No HTTP play floor. The AC-4 floor of 286,365 bytes is met by PSRAM. Over Sendspin the AC-4 image states 6 channels, so a 5.1.4 stream is refused there and sent as PCM. An E-AC-3 stream that declares more than 12 objects is played as its bed | Board 1, 2026-10-12 ([AC-4 from a server](esp32-p4.md#ac-4-from-a-server)) |
| AC-3 5.1 | Decoder peak 56,597 bytes. An AC-3 2.0 stream over Sendspin left 253,939 bytes of internal heap at the least | Peak: the probe, 2026-09-23 ([What the part carries](esp32-p4.md#what-the-part-carries)). Sendspin: baseline `p4-rev1` |
| E-AC-3 5.1 | Decoder peak 102,342 bytes. An E-AC-3 2.0 stream over Sendspin left 203,315 bytes; E-AC-3 5.1 on the AC-4 image left 211 to 218 KB | Peak: as above. Sendspin: baseline `p4-rev1`; board 1, 2026-10-12 |
| E-AC-3 7.1.4 | Decoder peak 167,386 bytes as coded; the stream set's eleven files peak at 80,922 to 256,584. Over Sendspin the internal RAM left was never under 108 KB | Peaks: the probe, 2026-09-23 ([Stream set](esp32-p4.md#stream-set)). Sendspin: board 1, `main` `ce0e099db`, 2026-10-12 ([AC-3 and E-AC-3 in the Hearth player](esp32-p4.md#ac-3-and-e-ac-3-in-the-hearth-player)) |
| AC-4 2.0 | An HTTP play's peak heap was 0.58 to 0.63 MB, internal RAM and PSRAM together, and it left 4 KB of internal RAM at the least. A Sendspin stream left 14 to 28 KB, and 2.9 KB through the frame-rate converter | HTTP: the second FireBeetle 2, 2026-10-02 ([Decode time and memory](esp32-p4.md#decode-time-and-memory)). Sendspin: board 1, 2026-10-12 ([AC-4 from a server](esp32-p4.md#ac-4-from-a-server)) |
| AC-4 5.1 | An HTTP play to 5.1 peaked at 1.06 to 1.36 MB and left 2 to 8 KB of internal RAM. A Sendspin stream left 3.9 to 31 KB | As the row above |
| AC-4 5.1.4 | An HTTP play to 5.1.4 peaked at 2.06 to 2.18 MB and left 1 KB of internal RAM. It is refused over Sendspin | As the row above |

An AC-4 play uses up this board's internal RAM, and the profile leaves it so. Under ESP-IDF's
policy the decoder's many blocks of 8 to 16 KB go to internal RAM until it is full, and its large
blocks go to PSRAM: 0.24 MB at 2.0, 0.7 to 1.0 MB at 5.1 and 1.7 to 1.8 MB at 5.1.4. That is the
faster arrangement here, where PSRAM behind its caches costs the same as main RAM when the data
is cached. A board that is also updated over the network or sent a lyric document during an AC-4
stream has not been run ([AC-4 from a server](esp32-p4.md#ac-4-from-a-server)).

## Rules of thumb

### What to keep out of internal RAM

- **Any single block near the largest free block.** On an S3 with Wi-Fi up that is 31,744 bytes.
  A 32 KB stack or a ring goes to PSRAM by name (`CONFIG_ICLFORGE_EXAMPLE_DECODE_STACK_IN_PSRAM`,
  `..._SENDSPIN_DECODE_STACK_IN_PSRAM`, `..._RING_IN_PSRAM`) and is asked for with PSRAM
  capabilities. `malloc`'s size rule does not apply to a task's stack, which FreeRTOS takes from
  internal RAM unless the task is created with capabilities.
- **Function-local static tables of optional coding tools.** They are `.bss` in every image,
  whether or not a stream uses the tool: 27,776 bytes on the C6
  ([#1474](https://github.com/iainchesworthlabs/iclforge/pull/1474)). Make the table a member of
  the heap object that exists only while the tool runs, or a constant in flash.
- **Constants, where a cache miss is cheap.** On the S3 and the P4 the code and constants run
  from PSRAM. On the C6 a table the decoder reads in every block stays in `.data`, because the
  16 KB cache is shared with the code
  ([#1511](https://github.com/iainchesworthlabs/iclforge/pull/1511)).
- **`thread_local` buffers.** FreeRTOS takes each task's thread-local area from that task's
  stack and sizes it from the whole image. A 32 KB `thread_local` scratch made every task's area
  32 KB, ESP-IDF's IPC task has a 1 KB stack, and the S3 application failed before `app_main`
  ([What the port required from the library](esp32-s3.md#what-the-port-required-from-the-library)).
- **Documents made of many small blocks.** A parsed lyric document is about ten times its text
  at the peak, and every block is under the 16 KB limit. The Sendspin player reads and holds it
  with PSRAM first for that one task (`PsramScope`).
- **Buffers that keep their largest size.** Three Sendspin receive buffers kept the capacity of
  the largest message they had held: an 8,000-byte lyric document left 12.7 KB of an S3's
  internal heap taken for the rest of the connection. A JSON document over 4,096 bytes now
  releases them, and audio chunks keep theirs so that a stream does not allocate for every chunk
  ([#1408](https://github.com/iainchesworthlabs/iclforge/pull/1408): S3a, 2026-10-12).
- **The P4's low-power SRAM.** A project of its own that decodes on a P4 sets
  `CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=n`.
- **A second graphics heap.** LVGL's allocator is PSRAM alone (`CONFIG_LV_USE_CUSTOM_MALLOC`),
  because ESP-IDF's `malloc` would put every block under 16 KB in internal RAM.

Two things belong in internal RAM. DMA buffers are allocated by their capabilities and no limit
moves them. And moving the decoder's working memory to PSRAM costs time: 1.08 to 1.24 times a
frame on the P4, and enough on the S3's E-AC-3 decode to underrun. The S3's 512-byte rule holds
for AC-4 plays alone, where the alternative left Wi-Fi with 0.4 to 5 KB.

### How to measure

Static RAM, from a build:

- `idf.py size --format json2` gives `DIRAM`, and
  `python tools/hearth/sink_profile.py verify <profile> --build-dir <dir> --size-json <file>`
  holds it, the application and the bootloader to the profile's budgets. CI runs both for every
  profile it builds.
- `idf.py size-components` and `idf.py size-files` give the same by archive and by object file.
- For `.bss` by input section, read the linker map in the build directory. An archive's own total
  can differ from the linked image's: the three copies of one table in `libiclforge_ac3_minimal.a`
  are one weak symbol after the link, 39,864 bytes in the archive and 39,752 in the image.
  `tools/checks/footprint_report.py` reads a GNU ld map the same way for the
  [Cortex-M3](cortex-m3.md) probe.
- `idf.py size`'s "remain" is the linker's estimate of the heap. On the S3 probe it said 239,164
  where the allocator reported 304,680.

The heap, from a running board. The firmware has no heap route; it prints these lines, and
`GET /log` returns the console's recent output (the last 16 KiB on a board with PSRAM, 2 KiB on
the C6; `python tools/hearth/ota.py log --host <board> --follow` follows it):

- `budget:` at boot and when a network comes up later: free, the largest block, the asks and the
  verdict ([above](#heap-floors-and-how-a-refusal-is-reported)).
- `sendspin: player up with internal heap N free, largest block M` when the Sendspin player
  starts.
- `heap: internal free N (largest block M), psram free P` when an HTTP play has opened its
  source, before the decoder allocates.
- `sendspin.progress ... heap_free=N heap_largest=M heap_least=L` every 125 chunks of a Sendspin
  stream, and `sendspin.stream= ... heap_free=L` when it ends: the least internal heap free
  while the stream played.
- `stream.decode_stack_free=N` at the end of an HTTP play, and `decode_stack_free` and
  `server_stack_free` in `GET /status`'s `sendspin` object.
- `ac4.heap internal_start=... internal_least=... internal_peak=... psram_start=...
  psram_least=... psram_peak=...` at the end of an AC-4 play.
- `GET /hardware` has `psram_bytes`, `profile` and `profile_hash`, and `GET /firmware` has the
  partition table and both slots.

To hold a board to figures:

- `tools/checks/check_esp_console.py --min-heap-free <bytes> --min-budget-headroom <bytes>` on a
  console capture. `--budget-verdict below-floor` is the C6's.
- `python tools/hearth/board_suite.py run --profile <profile> --restart --boots 8 --repeat 5`
  with `--record` or `--compare <baseline>`, on a booked bench board.
- `tools/checks/run_esp32s3_probe.sh`, and with `--ac4`, for the decoder alone under QEMU: peak
  heap for each fixture and, for AC-4, the internal RAM and PSRAM each took.
- On an S3 image with AC-4, `?below=N` in a play's location sets that play's allocation limit.

### Traps

- **An update over the network overwrites the slot that is not running.** One push leaves the
  image the board had as the one to go back to. A second push overwrites it. After two pushes
  the image the board was found on is in neither slot, and restoring it means pushing that image
  again.
- **A P4 board's chip revision decides the image.** A v1.x image refuses a v3.x chip and the
  other way round. `GET /hardware` and `esptool chip-id` say which a board is, and `ota.py`
  chooses from them ([Sink firmware](../../hearth/sink-firmware.md#which-image)). On v1.x,
  400 MHz asserts in a boot loop.
- **The flash mode is the bootloader's.** An application built for QIO runs at DIO speed under a
  DIO bootloader, and an update over the network keeps the bootloader. A board takes QIO with
  one USB flash that includes its bootloader.
- **The P4's bootloader has 24,576 bytes.** A new ESP-IDF, a log level or a chip revision can
  take it over, and the build then fails.
- **The largest free block decides an allocation.** A C6 with 11,016 bytes free could not
  allocate a 1,848-byte Wi-Fi buffer: no block was over 1,824.
- **QEMU has no Wi-Fi.** The S3's release shape under QEMU boots with 226,679 bytes free and a
  135,168-byte largest block, where a board has about 86,000 and 31,744. A stack that cannot be
  made on a board is made there.
- **A low-water mark depends on what played before.** On an S3 an AC-3 stream after an E-AC-3
  one read 7.2 KB free where the first stream of a boot read 58 to 60 KB. Record a board's
  figures after a restart, in a fixed order.
- **Freeing RAM can defeat a floor.** The C6's HTTP play floor refused every play only while
  the board had too little free to pass it. With 27,776 bytes back a play started and failed,
  and the floor went from 134,008 to 160,796.
- **`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` reserves nothing for long.** ESP-IDF v6.1 lets
  ordinary small allocations take the reserve once the rest of internal RAM is full.

## Where to go next

- [ESP32-S3](esp32-s3.md), [ESP32-C6](esp32-c6.md) and [ESP32-P4](esp32-p4.md): the
  measurements, with how each was made.
- [An ESP32-S3 sink](../../hearth/sink-esp32-s3.md), [An ESP32-C6 sink](../../hearth/sink-esp32-c6.md)
  and [An ESP32-P4 sink](../../hearth/sink-esp32-p4.md): building and flashing each board.
- [Testing and benchmarking the ESP32 boards](board-bench.md): the play suite, how a baseline is
  recorded and compared, and the bench tools.
- [Sink firmware](../../hearth/sink-firmware.md): which image a board takes, and updating over the
  network.
- [The firmware's README](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/README.md):
  every setting and console line.
- [The profiles plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-platform-profiles.md):
  what a profile locks, the guard and the board suite.
