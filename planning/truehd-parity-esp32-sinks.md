# TrueHD and DD+ parity: the ESP32 sinks

!!! note "Status as of 2026-11-01: the decoder is measured on the C6 and the P4; the S3 is owed; no sink plays TrueHD"
    A probe of the library's decoder (`firmware/truehd-probe`) ran on an ESP32-C6 and an ESP32-P4: all eight
    fixtures decode bit-exact to FFmpeg's PCM and the figures are in
    [Measured on the C6 and the P4](#measured-on-the-c6-and-the-p4-2026-11-01). The S3 was not measured
    (its USB port stopped answering after a debug session; see the section). The host-ratio estimates
    below it stand for the S3 only. No ESP-IDF component, `hearth_sink` or `libs/device` source names
    TrueHD yet; the sender side (Sendspin carriage, the sink library, Hearth) is built and tested on the
    host ([truehd-parity-hearth-player.md](truehd-parity-hearth-player.md)).

!!! note "Status as of 2026-10-29: audit and plan only; nothing built, nothing run on a board"
    TrueHD does not run on any ESP32 sink, and no source file of the ESP-IDF component, `hearth_sink`,
    `libs/sendspin`, `libs/sink` or `libs/device` names it. This page sets the DD+ budget of each tier
    beside the TrueHD decoder as measured on a host, translates the host figures into the three
    chips by a stated method (every translated figure is an estimate), says which streams each tier
    could carry, and plans the work. Round 2 (implementation) waits for the owner's answers in
    [Open questions](#open-questions).

DD+ here is E-AC-3 with AC-3 and the JOC layer riding with it, as in the other pages. The tier study is
[esp32-sink-tiers.md](esp32-sink-tiers.md); the arithmetic tiers are
[arithmetic-tiers.md](arithmetic-tiers.md); the TrueHD plan is [truehd.md](truehd.md).

## Basis

**Commit audited:** `523cf5dc` (`integ`, 2026-10-29). `libs/truehd` and `libs/base` are identical at the
later `integ` head `7a095779`. The decoder under test was compiled from this worktree's sources; only
`forge probe` and `forge truehd-encode` came from the shared forge binary.

**Run:**

- `forge probe` on the fixtures and on the DD+ stream-set files, for rates (the shared build's binary).
- A decoder-only timing harness written for this audit (scratch, not in the tree) over `iclforge::truehd`:
  per-access-unit time with the thread CPU clock, many passes, `nice -n 19`. Built from the decode-side
  sources at `-Os`, `-O2` and `-O3`, GCC 13, x86-64, with `-fno-exceptions -fno-rtti`.
- A counting `operator new` and a painted stack around the same decoder, for heap and stack.
- `-fstack-usage`, and `size` on the decode-side objects.
- The minimum-footprint DD+ probe (`firmware/baremetal/probe.cpp`) built for the host in both arithmetic
  tiers (`float` and `fixed`) in a scratch build directory, six runs each, to get a host time for the fixtures the
  board tables report.
- `forge truehd-encode` to make eight more streams (7.1, 5.1 and 5.1 at 96 kHz from the repository's own
  programme material, and four full-scale noise streams), because the committed fixtures stop at 5.1 and
  at 0.25 s.

**Only read:** the board tables and sink baselines (`docs/platforms/bare-metal/esp32-{c6,s3,p4}.md`,
`firmware/hearth-sink/baselines/*.json`, `profiles.json`, the `sdkconfig.*` overlays), the Sendspin role
(`hearth-sendspin-extension.md`), the MAT packer's constants, and the component and root `CMakeLists.txt`.

**Not available here:** an ESP-IDF, RISC-V, Xtensa or `arm-none-eabi` toolchain, QEMU for either, and any
board. Nothing below was cross-compiled or run on a chip. No toolchain was installed.

**How noisy the host was.** The machine has four cores and the load average was 6 to 15 throughout, other
agents' builds included. The harness uses the thread CPU clock, which removes time-slicing but not cache and
SMT interference. Across five invocations of one stream the median pass differed by 1.7% to 54%, typically
3% to 7%, so the **minimum** over invocations is the figure used and the median is shown beside it. A
minimum taken while the machine was busy is too high, which makes the translated estimates pessimistic if
anything.

## The DD+ budget, per tier

All figures are from the pages named; none is re-measured here except where the Basis says so.

| | ESP32-C6 | ESP32-S3 | ESP32-P4 |
|---|---|---|---|
| Core | 1 core, RV32IMAC, 160 MHz, no FPU | 2 cores, Xtensa LX7, 240 MHz, single-precision FPU | 2 cores, RV32IMAFC, 360 MHz on revision v1.x silicon (400 on v3.x), FPU |
| DD+ arithmetic | `fixed` (Q7.24) | `float` | `float` |
| RAM | 512 KB SRAM, no PSRAM possible on this die | 512 KB SRAM, 8 MB PSRAM | 768 KB L2MEM, 32 MB PSRAM |
| Free internal heap in the sink | 136,668 B at boot; 25,836 to 56,104 B during a 2.0 play | 110,195 B at boot; 79,131 to 103,023 B during a 2.0 play; 23 to 1,100 B at the least over a 7.1.4 Sendspin play | 291,783 B at boot; 203,315 to 253,939 B during a 2.0 play |
| Flash (application slot) | 1,966,080 B on a 4 MB module, image 1.87 MB in October; 16 MB module: `app_bytes_max` 2,000,000 | 4 MiB slot, `app_bytes_max` 1,750,000 | 4 MiB slot; `p4-rev3` `app_bytes_max` 2,900,000 |
| DD+ in the probe, no network | AC-3 2.0 0.20x, E-AC-3 2.0 0.32x, 5.1 0.64x to 0.75x, 7.1 0.87x | 0.07x (AC-3 mono) to 0.92x (7.1.4 folded) | 0.027x to 0.448x, `714-ecpl` 0.70x |
| DD+ in the sink over Sendspin | AC-3 2.0 0.66, E-AC-3 2.0 0.76 of a 32 ms burst | AC-3 2.0 0.22, E-AC-3 2.0 0.18; 7.1.4 0.78 to 0.95 with the second core | AC-3 2.0 0.085, E-AC-3 2.0 0.067; 7.1.4 0.33 to 0.43 |
| Decode task stack | 24,576 B, 13.4 KB used by AC-3 2.0 | 32,768 B in PSRAM (40,960 with AC-4) | 32,768 B (40,960 with AC-4) |
| Ring and chunk | 49,152 B ring, 4,096 B chunk | 262,144 B ring (PSRAM), 25,600 B chunk | 262,144 B ring (PSRAM), 25,600 B chunk |
| TCP receive window | lwIP default, 5,760 B (the overlay sets none) | not re-read | 23,040 B (`sdkconfig.p4`) |
| Stated limits | `max_coded_channels` 2, `max_objects` 0 | no channel limit, `max_objects` 0 | no channel limit, `max_objects` 12 |

Sources: [ESP32-C6](../docs/platforms/bare-metal/esp32-c6.md), [ESP32-S3](../docs/platforms/bare-metal/esp32-s3.md),
[ESP32-P4](../docs/platforms/bare-metal/esp32-p4.md), `firmware/hearth-sink/baselines/c6-16mb.json`, `s3-n16r8.json` and `p4-rev1.json`,
`firmware/hearth-sink/profiles.json`, `firmware/hearth-sink/sdkconfig.sendspin-c6`, `firmware/hearth-sink/sdkconfig.p4`.
The sink costs more than the probe, by an amount that depends on the tier more than the stream. From the probe with
no network to the sink over Sendspin: on the C6 AC-3 2.0 goes from 0.20 to 0.66 and E-AC-3 2.0 from 0.32 to 0.76
(about 12 ms of a 32 ms burst for Wi-Fi, flash contention, render and the I2S path); on the S3 from 0.11 to 0.22
and from 0.17 to 0.18; on the P4 from 0.051 to 0.085 and from 0.071 to 0.067.

### Which DD+ forms each tier decodes

| Form | C6 | S3 | P4 |
|---|---|---|---|
| 2.0 | Yes; the sink's ceiling | Yes | Yes |
| 5.1 | Probe yes (0.82x to 0.96x with Wi-Fi); the sink refuses it, no room beside Wi-Fi | Yes (0.38 to 0.42 over Sendspin) | Yes (0.15 to 0.18) |
| 7.1 | Probe 0.87x, 1.09x with Wi-Fi: no | Yes | Yes |
| 7.1.4, no Annex E tool | No (1.88x) | Yes (0.78 none, 0.82 walk over Sendspin, second core) | Yes (0.33) |
| 7.1.4 with AHT, SPX, CPL, all | No | Yes: `714-all` 0.95 over Sendspin, `714-aht` 0.73 over HTTP | Yes (0.43) |
| 7.1.4 with enhanced coupling | No (2.09x at 5.1) | No: 1.25 over HTTP, 1.33 over Sendspin with 168 late bursts | Yes: 0.62 to 0.73 in the player, 0.70 in the probe |
| JOC objects | No (5.58x) | Bed only in the profile; 2 to 3 objects 0.95 to 0.98 with underruns, four or more past real time | Up to 12 objects (0.85); a stream past that plays as its bed |
| Output wider than 2 channels | Standard I2S 2 slots, TDM 128-bit frame | Standard I2S and TDM, two lines | Not on revision v1.x silicon: TDM does not open above 2 channels |

## The TrueHD decoder, measured on the host

### Decode time

Microseconds and milliseconds below are for the decoder alone (`Decoder::decode` on whole access units,
strict mode, a new decoder per pass), built at `-Os` as the ESP-IDF component builds. A TrueHD access unit
is 40 samples at 48 and 44.1 kHz, 80 at 96 and 160 at 192, so 0.833 ms (0.907 ms at 44.1 kHz) at every
rate and 1,200 units a second. "RTF" is decode time over audio time on this host.

Fixtures in `testdata/truehd/` (FFmpeg's encoder; the 24-bit ones carry random low bits, as
`tools/generators/gen_truehd_ffmpeg.py` describes), 0.25 s each:

| Stream | Ch | kHz | Bits | Average kbit/s | Peak over 32 ms kbit/s | Largest unit B | Mean us per unit | p99 us | ms per second of audio, min / median | RTF (min) | Median spread over 5 runs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|
| `truehd_mono48_s24` | 1 | 48 | 24 | 739 | 756 | 152 | 2.13 | 5.8 | 2.30 / 2.58 | 0.0023 | 44% |
| `truehd_st48_s16` | 2 | 48 | 16 | 629 | 653 | 174 | 3.07 | 9.7 | 3.30 / 3.84 | 0.0033 | 34% |
| `truehd_st48_s24` | 2 | 48 | 24 | 1,414 | 1,447 | 254 | 3.52 | 9.0 | 3.80 / 4.33 | 0.0038 | 7% |
| `truehd_st44_s24` | 2 | 44.1 | 24 | 1,327 | 1,357 | 256 | 3.46 | 7.0 | 3.40 / 3.90 | 0.0034 | 4% |
| `truehd_st96_s24` | 2 | 96 | 24 | 1,967 | 2,043 | 338 | 5.88 | 12.0 | 5.92 / 7.14 | 0.0059 | 27% |
| `truehd_st192_s24` | 2 | 192 | 24 | 2,736 | 2,926 | 478 | 10.35 | 23.2 | 10.70 / 12.73 | 0.0107 | 3% |
| `truehd_m51_s16` | 6 | 48 | 16 | 1,891 | 1,989 | 428 | 6.59 | 18.4 | 7.33 / 7.99 | 0.0073 | 6% |
| `truehd_m51_s24` | 6 | 48 | 24 | 4,227 | 4,324 | 680 | 7.96 | 20.4 | 8.68 / 9.96 | 0.0087 | 5% |
| `mlp_st48_s24` | 2 | 48 | 24 | 1,413 | 1,442 | 254 | 3.35 | 6.9 | 3.49 / 4.12 | 0.0035 | 5% |
| `mlp_st96_s24` | 2 | 96 | 24 | 1,970 | 2,052 | 338 | 5.90 | 12.3 | 6.05 / 7.13 | 0.0061 | 2% |
| `mlp_st44_s16` | 2 | 44.1 | 16 | 626 | 654 | 174 | 2.99 | 6.3 | 2.85 / 3.47 | 0.0028 | 6% |
| `mlp_m51_s24` | 6 | 48 | 24 | 4,178 | 4,275 | 654 | 7.44 | 18.9 | 8.20 / 9.09 | 0.0082 | 2% |

Streams made for this audit with `forge truehd-encode`, 2 to 2.5 s each (music is `reference_51.wav`,
widened to 7.1 by two delayed copies of the surrounds and put in the upper 16 bits of 24 with random low
bits, as the fixture generator does; noise is full-scale uniform 24-bit, the least compressible input):

| Stream | Ch | kHz | Average kbit/s | Peak over 32 ms kbit/s | Largest unit B | Mean us per unit | p99 us | ms per second of audio, min / median | RTF (min) | Median spread |
|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|
| music 5.1 | 6 | 48 | 3,893 | 3,959 | 626 | 7.27 | 18.3 | 7.93 / 8.82 | 0.0079 | 3% |
| music 7.1 | 8 | 48 | 5,656 | 5,749 | 856 | 10.16 | 25.7 | 11.71 / 12.42 | 0.0117 | 3% |
| music 5.1 | 6 | 96 | 6,809 | 6,884 | 1,012 | 14.68 | 33.2 | 16.83 / 17.83 | 0.0168 | 6% |
| music 7.1 | 8 | 96 | 9,609 | 9,961 | 1,398 | 20.49 | 43.9 | 23.49 / 25.02 | 0.0235 | 5% |
| noise 2.0 | 2 | 48 | 2,404 | 2,413 | 298 | 2.34 | 5.0 | 2.66 / 2.84 | 0.0027 | 54% |
| noise 7.1 | 8 | 48 | 9,435 | 9,452 | 1,076 | 8.06 | 20.9 | 9.16 / 9.93 | 0.0092 | 8% |
| noise 5.1 | 6 | 96 | 13,983 | 13,996 | 1,526 | 10.87 | 23.1 | 12.25 / 13.38 | 0.0123 | 5% |
| noise 2.0 | 2 | 192 | 9,316 | 9,325 | 1,018 | 7.95 | 16.6 | 9.19 / 10.21 | 0.0092 | 45% |

What the numbers say. Cost follows channels and sample rate (the work per sample is filters, matrices and
residual decoding, a few operations per channel per sample) far more than bit rate: full-scale noise 7.1
at 9.4 Mbit/s costs less than the music 7.1 at 5.7 Mbit/s, which is consistent
with noise being coded as raw residuals with little prediction (not checked). The slowest unit of a run takes 68 to 230 us, 10 to 60 times the mean. The harness does not say which unit
it is; the first unit of a pass (it constructs state) and a unit with a restart header (it copies and resets
the state; see the stack finding below) are the candidates, and a busy host adds its own.
At `-O3`, which is how the shared library is built, the same streams are about 10% faster (5.1 fixture 7.79
against 8.68 ms per second). The decoder does no float or double arithmetic anywhere in the decode-side sources.

A callgrind run on the 5.1 music stream at `-Os` counts about 73,000 x86-64 instructions per access unit (87
million per second of audio). Of the decoder's instructions 67% are in `Decoder::decode` itself (the
filter, matrix and output loops), 14% in `BitReader::read`, 7% in `BitReader::window`, 4% in the substream
CRC, 3% in the parity byte and 3% in `advance_bits`. Every filter and matrix term is a 32 by 32 to 64-bit
multiply-accumulate (`std::int64_t` accumulators), which a 32-bit core builds from two multiplies and carry
handling.

### Presentations are cheaper

A stream with downmix substreams (substream 0 is the 2-channel presentation, 1 adds 6, 2 adds 8, TH spec
§2.2) can be decoded with `Presentation::k2ch` or `k6ch`, which read fewer substreams. Same host, `-Os`,
median of one 30-pass run per cell (so not comparable with the minima above):

| Stream | `best` ms/s | `k6ch` ms/s | `k2ch` ms/s | `k2ch` over `best` | Substream 0 as a share of the coded bytes |
|---|---:|---:|---:|---:|---:|
| `truehd_m51_s24` | 10.21 | 11.63 | 4.05 | 0.40 | 123.5 of 430.6 B, 29% |
| music 5.1, 48 kHz | 9.07 | 8.99 | 3.90 | 0.43 | 108.5 of 397.2 B, 27% |
| music 7.1, 48 kHz | 12.20 | 10.65 | 3.90 | 0.32 | 108.9 of 578.9 B, 19% |
| music 7.1, 96 kHz | 25.43 | 18.16 | 7.09 | 0.28 | 209.7 of 990.8 B, 21% |

The 2-channel presentation of a 7.1 stream is 1.0 to 2.0 Mbit/s of the coded stream, and it costs 0.28
to 0.43 of the full decode. The sender can drop the other substreams' bytes (the library writes unit
headers and directories, `write_access_unit_prefix`); no tool does it today.

### Memory and code

| | Value | How |
|---|---|---|
| Heap of one decoder | **156,024 B for every stream configuration**, 5 allocations, the largest 102,728 B (the four substreams' state, each sized for 16 channels); a unit allocates nothing after the first | counting `operator new`, six streams from mono to the hand-built 16-channel one: identical |
| Stack | **32,712 B** painted on x86-64 at `-Os`; static frames `Decoder::decode` 25,056 B and `read_block_header` 7,424 B | painted stack; `-fstack-usage` |
| Code, decode-side objects | 37,874 B text at `-Os`, 51,034 at `-O2`, 92,279 at `-O3`; no data or bss | `size` on the seven decode-side objects; includes the syntax writers that a decode-only link would drop |
| Tables | The three Huffman decode tables, 512 entries each, built at compile time | `libs/truehd/src/core/huffman.hpp` |

The heap is the sum of `Impl` (four substreams of 16 channels, each channel with two 168-entry histories
and two filter specs) and three buffers sized for 255 samples of 16 or 18 columns. The stack frame has the size of the copy of all sixteen channels' filter state that `begin_restart` makes
(`const auto previous = state.channels`, about 24 KB) beside the temporary `SubstreamState{}`; that is
read from the source and the sizes agree, and was not confirmed by changing the code. The frame is
allocated by every call, not only at a restart. For comparison DD+ peaks at 47,608 B (AC-3 mono) to 234,070 B (7.1.4 folded) on the C6
and uses 13 to 21 KB of stack. These are x86-64 figures; on a 32-bit core pointers are smaller and the
arrays are the same `int32` arrays, so the heap and stack should be within a few hundred bytes of these
(estimate).

## Measured on the C6 and the P4 (2026-11-01)

`firmware/truehd-probe` compiles the library's decoder (`core`, `decoder`, `io/access_unit`,
`io/major_sync`; nothing else of the repository) into a small ESP-IDF v6.1 image, embeds eight fixtures from
`testdata/truehd`, and for each one decodes it once while taking the SHA-256 of the 32-bit samples and
comparing it with the manifest's `pcm_sha256` (FFmpeg's strict decode), then times four passes in CPU cycles and
keeps the least. The decoder's sources are `-O2`, the rest `-Os`. No network, no I2S, no PSRAM: the decoder
and its state are in internal RAM. Each board's `ota_0` and `otadata` were backed up first and written back
afterwards; the read-back of both is byte-identical to the backup and the board answered `/status` after.
Full output: `firmware/truehd-probe/results/esp32c6.txt` and `esp32p4.txt`.

| Fixture | C6 160 MHz: mean us/unit | real-time factor | slowest unit | P4 360 MHz: mean us/unit | real-time factor | slowest unit |
|---|---:|---:|---:|---:|---:|---:|
| 1.0, 48 kHz, 24-bit | 164 | 0.197 | 705 us | 67 | 0.080 | 315 us |
| 2.0, 48 kHz, 16-bit | 251 | 0.301 | 840 us | 101 | 0.122 | 372 us |
| 2.0, 48 kHz, 24-bit | 285 | 0.342 | 958 us | 111 | 0.133 | 376 us |
| 2.0, 44.1 kHz, 24-bit | 296 | 0.327 | 992 us | 115 | 0.127 | 379 us |
| 2.0, 96 kHz, 24-bit | 495 | 0.595 | 1,222 us | 192 | 0.230 | 464 us |
| 2.0, 192 kHz, 24-bit | 890 | **1.068** | 1,670 us | 348 | 0.417 | 631 us |
| 5.1, 48 kHz, 16-bit | 546 | 0.656 | 1,759 us | 220 | 0.264 | 730 us |
| 5.1, 48 kHz, 24-bit | 670 | 0.804 | 1,898 us | 266 | 0.319 | 763 us |

All sixteen decodes match the manifest. A unit is 40, 80 or 160 samples (0.83 ms at 48 kHz), so the slowest
unit of the C6's 5.1 is 2.3 units of audio and a sink needs a few units of buffer ahead of its output whatever the
mean.

| | C6 | P4 | Host estimate on this page |
|---|---:|---:|---:|
| Heap of one decoder | 157,660 B | 158,020 B | 156,024 B |
| Stack the decoding task reached | **60,356 B** | **60,368 B** | 32,712 B |
| Internal RAM free at start of the probe | 347,812 B (largest block 311,296) | 489,076 B (largest 258,048) | |
| Probe image | 693 KB | 690 KB | |

- **The heap estimate held (within 1 %); the stack estimate did not.** The decoder needs about 60 KB of stack on
  both RISC-V chips, 1.85 times the x86-64 figure, so the 40,960 B the plan below assumes for the S3 and the P4 is
  too small. On the C6 a 48 KB task stack faulted (`Stack protection fault`, the stack pointer 10.7 KB past the
  bound) before the first unit finished. The probe gives the task 128 KB.
- **`Decoder::reset()` needs the footprint twice.** It builds the new state before the old is released
  (`impl_ = std::make_unique<Impl>(config)`): 316 KB at the peak, and on the C6, with 190 KB free after the first
  decoder, a `std::bad_alloc` (the probe's timed passes hit it; it now releases the decoder first). The sink
  restarts a decoder at every seek and format change, so a decoder that is released first, or reset in place,
  is part of Phase 1.
- **The estimates were right for 2.0 and 5.1 at 48 kHz and a little high for the P4.** C6: 0.342 (estimated 0.33
  to 0.44) and 0.804 (0.75 to 1.00); P4: 0.133 (0.14 to 0.32) and 0.319 (0.33 to 0.74). The C6 cannot decode 2.0 at
  192 kHz in real time (1.068), and its 96 kHz 2.0 takes 0.6 of the core before Wi-Fi, the network stack and the
  I2S path, which the DD+ numbers say add a half again.
- **The C6 stays out**, as question 1 recommended: 158 KB of decoder state plus 60 KB of stack is 218 KB, against
  a C6 sink that has 25 to 56 KB free in a 2.0 play.
- **What the probe does not show.** The state is in internal RAM here; in the sink the allocations over a size
  go to PSRAM on the S3 and the P4 and cost more (the S3 page records 20 ms becoming 28 ms for a burst), the
  P4's rev v1.x silicon opens only two output channels, and no Wi-Fi or Sendspin traffic ran. The P4 therefore
  has CPU to spare for every stream tested, and the sink's overhead is still to be measured (Phase 3).
- **The S3 is not measured.** With a 48 KB stack the decoder hung at the first unit and the interrupt watchdog
  reset the board without a message (no stack-overflow check is on by default there; the C6's fault names the
  cause, and a stack of 60 KB on both chips says the S3 did the same, but that is inferred, not seen). A GDB
  session on its USB-Serial-JTAG port then left the port refusing writes, which only a replug of the cable
  clears. The image to run with the 128 KB stack is built; the board is owed (question 21 of the
  Hearth page's list asks for it).

## Translating the host figures to the tiers

### The method, and what the repository does

The repository's documents translate in three ways, none of which fits a decoder with no Cortex-M3 leg:
the Cortex-M3 `-icount` instruction count as a proxy ("instructions are not cycles",
[cortex-m3.md](../docs/platforms/bare-metal/cortex-m3.md)), the nearest board standing in for a part with
none (the C6 for the C3), and clock scaling (the P4 against the S3 at 1.67 times,
[esp32-c3.md](../docs/platforms/bare-metal/esp32-c3.md)). One document gives a host ratio, "about fifty
times slower than a desktop core" for the S3 encoder ([esp32-player.md](esp32-player.md)). The first needs
`arm-none-eabi`, which is not installed here.

So this page follows the closest practice and builds the ratio from DD+, which has both a host time and a
board time for the same fixtures in the same arithmetic tier: **the board's microseconds per frame divided
by the host's, for the same fixture**, host built from the same minimum-footprint profile as the probe.
Host DD+ figures are the minimum of six runs of the probe built here.

| Tier | Board over host, DD+ rows | Rows used | Source of the board figure |
|---|---|---|---|
| C6, `fixed` | **86 to 115** (median 95) | AC-3 mono, 2/0, 5.1, E-AC-3 2/0, 5.1, Atmos bed, 7.1, enhanced coupling | C6 page, no network, two runs within 3 us |
| S3, `float` | 97 to 167 (median 125) | the thirteen rows with a host figure | S3 page, third pass table |
| P4, `float` | 50 to 85 (median 63) | the fourteen rows | P4 page |

The fold, line-mode and object rows (132 to 322 on the C6) are left out: they run float or other code that
TrueHD has no counterpart for. The S3 and P4 ratios are for float DD+ against a host that vectorises, and
TrueHD is integer and does not, so they overstate. For the bounds used below, the **lower** bound for each
chip assumes its cycles per operation equal the C6's and scales by clock (S3 160/240, P4 160/360, so
57 and 38), and the **upper** bound is the float-tier maximum (167 and 85). The S3's range is the widest
because nothing in the repository says whether its core's 32 by 32 to 64-bit multiply is a single
instruction (`MULSH` or `MUL32H`) or a library call; the DD+ fixed tier is not built for it. The cost of
a library call there would exceed the upper bound, so the S3 row can be too low.

Limits of this construction. DD+ fixed spends its time in transforms; TrueHD spends it in 64-bit
multiply-accumulates and bit reading, both dearer on a 32-bit core. The host IPC differs between the two
workloads. The ratios were measured on a loaded host. All of it is replaced by the first number a probe
gives on a chip (Phase 0 to 3 below).

### Real time, estimated

CPU of one core, no network, no render, as a fraction of audio time (host minimum times the ratio range).
**A**: the upper estimate is 0.5 or less, which leaves room for the network, render and sink (by the
baselines about 0.4 of a core on the C6, up to 0.1 on the S3 and 0.04 on the P4 for 2.0). **C**: the lower estimate is over 1.0. **B**: between; only a board
decides. All figures are estimates.

| Stream | C6 (ratio 86 to 115) | S3 (57 to 167) | P4 (38 to 85) |
|---|---|---|---|
| 2.0, 48 kHz, 24-bit (`truehd_st48_s24`) | 0.33 to 0.44 **A** | 0.22 to 0.63 B | 0.14 to 0.32 **A** |
| 2.0, 96 kHz | 0.51 to 0.68 B | 0.34 to 0.99 B | 0.22 to 0.50 B |
| 2.0, 192 kHz | 0.92 to 1.23 B | 0.61 to 1.79 B | 0.41 to 0.91 B |
| 5.1, 48 kHz (`truehd_m51_s24`) | 0.75 to 1.00 B | 0.49 to 1.45 B | 0.33 to 0.74 B |
| 7.1, 48 kHz, music | 1.01 to 1.35 **C** | 0.67 to 1.96 B | 0.44 to 1.00 B |
| 5.1, 96 kHz, music | 1.45 to 1.94 **C** | 0.96 to 2.81 B | 0.64 to 1.43 B |
| 7.1, 96 kHz, music | 2.02 to 2.70 **C** | 1.34 to 3.92 **C** | 0.89 to 2.00 B |
| 7.1, 48 kHz, noise (9.4 Mbit/s) | 0.79 to 1.05 B | 0.52 to 1.53 B | 0.35 to 0.78 B |
| 5.1, 96 kHz, noise (14 Mbit/s) | 1.05 to 1.41 **C** | 0.70 to 2.05 B | 0.47 to 1.04 B |
| Substream 0 only (2.0) of the 7.1 music stream | 0.34 to 0.45 **A** | 0.22 to 0.65 B | 0.15 to 0.33 **A** |

On the C6 the DD+ decode grows 16% to 53% with Wi-Fi up (the smallest streams most) and the sink adds
about 12 ms a burst, so a TrueHD stream at 0.4 in this table is at about 0.8 to 1.0 in the sink (estimate,
by the same additions). The slowest unit of a run, 68 to 230 us on the host, would be 6 to 26 ms on the C6
and 3 to 20 ms on the P4 (estimates); a chunk groups tens of units, so a spike falls inside one chunk's decode.

### Link and buffer

The Sendspin ring holds bytes, so its length in time falls with the rate. `buffer_capacity` is in bytes
and a server may fill the ring to three quarters.

| Stream rate | C6 ring (49,152 B) | S3 and P4 ring (262,144 B) |
|---|---:|---:|
| AC-3 192 kbit/s (DD+ stream set) | 2.0 s | 10.9 s |
| E-AC-3 640 kbit/s (DD+ 7.1.4 maximum in the set) | 0.61 s | 3.3 s |
| TrueHD 2.0, 1.4 Mbit/s | 0.28 s | 1.5 s |
| TrueHD 5.1, 4.2 Mbit/s | 93 ms | 0.50 s |
| TrueHD 7.1, 5.7 Mbit/s | 70 ms | 0.37 s |
| TrueHD 7.1 noise, 9.4 Mbit/s | 42 ms | 0.22 s |
| TrueHD at the 18 Mbit/s the encoder keeps by default | 22 ms | 0.12 s |

The evidence on what a link needs is the repository's own. On the C6 a 32 KB ring (1.5 s of 192 kbit/s
AC-3) saw 43 to 131 chunks come back late in one or two clusters per 5 to 10 minutes, each after about a
second of Wi-Fi gap; 48 KB cut that to 41 over ten minutes and 64 KB to none, at the cost of failing
Wi-Fi buffer allocations (`sdkconfig.sendspin-c6`). Stereo PCM at 1.5 Mbit/s with a 192 ms ring gave 10 to
65 underruns in 15 s ([ESP32-C6, PCM from a server](../docs/platforms/bare-metal/esp32-c6.md#pcm-from-a-server)).
A ring of 0.1 s therefore cannot ride out the gaps the C6 sees. The highest rate the repository has
carried to a board is 1.5 Mbit/s TCP in the C6 probe (802.11n) and 24-bit stereo PCM at 2.3 Mbit/s on the
P4 boards. Nothing records a Sendspin rate of 4 to 18 Mbit/s on any tier.

The TCP window bounds the rate to window over round-trip time (an estimate from the settings above and the
C6 page's ping figures of 8 to 18 ms mean and 35 to 90 ms worst): the C6's 5,760 B gives 2.6 Mbit/s at
18 ms and 0.5 Mbit/s at 90 ms; the P4's 23,040 B gives 10.2 Mbit/s at 18 ms, 3.7 at 50 ms and 2.0 at 90 ms.
Chunk size is the other limit: a chunk is at most 4,096 B on the C6 and 25,600 B on the S3 and P4, against
a MAT burst of 61,440 B (see the matrix) and a 20 ms group of TrueHD at 18 Mbit/s of 45,000 B.

Sample rates: the Sendspin role's `sample_rates` is `[48000]` on the boards. No document read here covers
I2S or DAC clocking at 44.1, 88.2, 96, 176.4 or 192 kHz, so on today's sinks only 48 kHz streams could play.

## Which streams each tier could carry

Three limits apply and the tightest wins: CPU (above), memory (the decoder as written needs 156,024 B in
a 102,728 B block and 32.7 KB of stack) and the link.

| Tier | Could carry, with margin | Could not | Unknown until a board runs it |
|---|---|---|---|
| **C6** | Nothing as the sink is built. CPU alone would allow 2.0 at 48 kHz and the 2-channel substream of a larger stream (0.33 to 0.45 in the probe, about 0.8 to 1.0 in the sink) | **Everything.** Heap: 25,836 to 56,104 B free in a 2.0 play, and a largest block of 96,256 B at the start of an AC-4 play on the C6 (2026-10-10), against 156,024 B and 102,728 B. Stack: 24,576 B against 32.7 KB. Ring: 0.28 s of 2.0 and 93 ms of 5.1, with 1.5 s already too short in the C6's own tests. CPU: 7.1, any 96 kHz 5.1 and 7.1, over 1.0 | A decoder rewritten to about 60 KB and 16 KB of stack, with a ring over 100 KB, which the C6 has no room for (a 64 KB ring already failed on the heap) |
| **S3** | Nothing is shown with margin. 2.0 at 48 kHz is the closest (0.22 to 0.63 CPU; 1.5 s of ring) | 7.1 at 96 kHz (1.34 and over); every stream past 48 kHz for lack of a sample-rate path | 5.1 and 7.1 at 48 kHz, 96 kHz stereo: CPU 0.5 to 2.0; the state (156 KB) lives in PSRAM behind a 32 or 64 KB data cache, and sending the S3 player's allocations over 4 KB to PSRAM made a burst take 28 ms where it had taken 20 ([ESP32-S3, in the Sendspin sink](../docs/platforms/bare-metal/esp32-s3.md#in-the-sendspin-sink)); the multiply question above; the stack must go from 32,768 to 40,960 B (in PSRAM) |
| **P4** | 2.0 at 48 kHz (0.14 to 0.32); the 2-channel substream of anything | Nothing excluded on CPU for 48 kHz up to 7.1 (0.44 to 1.00 at the edge); rev v1.x silicon opens no output wider than 2 channels, so 5.1 and 7.1 fold to 2.0 there | 5.1 and 7.1 at 48 kHz (0.33 to 1.00), 96 kHz stereo (0.22 to 0.50), 192 kHz stereo (0.41 to 0.91), 96 kHz 5.1 (0.64 to 1.43); hosted Wi-Fi at 4 to 10 Mbit/s |

### Options for what a tier cannot carry

Each is a choice of the sender (Hearth's server, `forge`) or of the sink's stated limits: the sink states what it
carries and the sender picks, as it does with `max_coded_channels` and `max_objects`.

1. **Send the core presentation.** The stream already holds a 2-channel and a 6-channel presentation in
   substreams 0 and 1 when the encoder wrote downmixes (`truehd-downmix=standard`); a sender that drops the
   later substreams sends 19% to 29% of the bytes and the sink decodes 0.28 to 0.43 of the work. It stays
   lossless for that presentation. Not built: a substream-dropping tool, a field in the support object, and
   the sender's choice. A stream without downmix substreams (FFmpeg's stereo, mono, and 7.1 with `truehd-downmix=none`)
   has nothing to drop.
2. **Decode on the sender and send PCM.** Hearth does this today for AC-3 family streams a board cannot
   decode ([ESP32-C6, PCM from a server](../docs/platforms/bare-metal/esp32-c6.md#pcm-from-a-server)), as
   `player@v1` stereo. PCM is 1.5 Mbit/s at 16 bit and 2.3 at 24 bit for 2.0 and 6.9 for 5.1, and the C6 drops out on
   it. Multichannel PCM has no channel order or layout in `player@v1` (open question Q7 of
   [hearth-sendspin-extension.md](hearth-sendspin-extension.md#open-questions)), so this is stereo.
3. **Send Opus or FLAC of the decode.** The C6 has an Opus path for a link too poor for PCM, lossy. It gives
   up the point of TrueHD.
4. **Refuse by name.** The support object already refuses a syncframe wider than `max_coded_channels` before
   a decoder opens, and the console says why. A TrueHD limit set (channels, rate, depth, peak coded rate) would
   do the same.
5. **Transcode to E-AC-3.** `forge transcode` reads AC-3, E-AC-3 and AC-4 only, so it does not read TrueHD
   today. The result is lossy and loses the lossless property and any Atmos; listed because it is the
   "DD+-to-DD path" the other codecs use.
6. **A lower-rate substream.** There is one, in the sense of option 1: the fixtures' 2-channel substream. TrueHD
   has no lower-bit-rate variant of the same channels.

## The matrix

One row for each capability DD+ has in this area. Gap: none, partial, missing, or not applicable. Size is the
ROADMAP's: S an afternoon, M a day or two, L a focused week, XL several PRs.

| # | Capability | DD+ status and files | TrueHD status and files | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| 1 | Decoder built into the ESP-IDF component | `libs/ac3/minimal.cmake`, `firmware/esp-idf/iclforge/CMakeLists.txt`; manifest lists `esp32s3`, `esp32c3`, `esp32c6`, `esp32p4` | None. The root `CMakeLists.txt` refuses `ICLFORGE_BUILD_TRUEHD=ON` with the minimum-footprint profile (it is in the conflict list); the component has no TrueHD switch | missing | M | Q1 | Ran the configure of the minimal profile with defaults: "The minimum-footprint profile needs ICLFORGE_BUILD_TRUEHD=OFF"; a grep for TrueHD over `firmware`, `libs/sendspin`, `libs/sink` and `libs/device` finds no source file |
| 2 | Arithmetic tiers (`double`, `float`, `fixed`) | Three tiers, [arithmetic-tiers.md](arithmetic-tiers.md) | One integer implementation; `grep -w 'float\|double'` over the decode-side sources finds none | not applicable: lossless integer decode has no tier choice. On a 32-bit core its cost is the 64-bit multiply-accumulate | n/a | none | Ran the grep |
| 3 | Host and emulated probe: instructions, peak heap, stack, pinned PCM hashes | `firmware/baremetal/probe.cpp`, `tools/checks/run_baremetal_probe.sh` (host, Cortex-M3 `-icount`, C3 under `qemu-riscv32`), `testdata/fixed-probe-pcm-hashes.json` | None. Heap and stack measured on the host by this audit's scratch harness only | missing | M | a CI image with `arm-none-eabi` and QEMU (not here) | Built and ran the DD+ probe on the host in both tiers; counting allocator and painted stack on the TrueHD decoder |
| 4 | Board timing table, no network | `docs/platforms/bare-metal/esp32-c6.md`, `-s3.md`, `-p4.md` | None; no board has run TrueHD | missing | M per chip | boards, `tools/hearth/bench.json` booking | Read only |
| 5 | Memory budget per stream configuration | Peak heap 47,608 to 234,070 B by fixture; stack 13 to 21 KB; allocations per frame 1 to 27 | 156,024 B for every configuration, 5 allocations, largest 102,728 B; 0 per unit; stack 32,712 B (static frames 25,056 and 7,424 B) | partial: measured, but exceeds the C6's free heap and stack and the S3 and P4 decode stacks of 32,768 B | S for a footprint change in `libs/truehd/src/decoder/decoder.cpp`; sized by the stream's channels, no restart copy | Q4 | Counting allocator over nine streams; painted stack; `-fstack-usage` |
| 6 | Sendspin data type and the sink's support object | `DataType::{kAc3, kEac3, kAc4}` in `libs/sendspin/include/iclforge/sendspin/iclforge_player.hpp`; `data_types`, `max_coded_channels`, `max_frame_rate`, `max_objects` in [hearth-sendspin-extension.md](hearth-sendspin-extension.md) | No data type, no limit keys | missing | L (server, sink and ESP, with the Hearth audit) | Q2, Q3 | Read |
| 7 | Burst packing for the link | `ac3::iec61937::wrap_frame`, `Eac3BurstPacker`; a burst is 1,536 samples, at most 24,568 B | MAT, data type 22, in `libs/containers/src/iec61937/mat.cpp`: 61,440 B per 24 units (20 ms), padded to that size whatever the data (24.6 Mbit/s of carrier for any stream) | partial: exists for HDMI; over every sink's chunk limit (4,096 B, 25,600 B) and wasteful as a network carrier | M | Q2 | Read the constants in `mat.hpp`; limits from `profiles.json` |
| 8 | Incremental framing from a byte stream | `AccessUnitAccumulator` (`iclforge::ac3::io`), allocation-free, used by the sink's sources | `ElementaryReader` reads a whole span (`libs/truehd/include/iclforge/truehd/io/elementary.hpp`); each unit's length is in its first two bytes (`access_unit_length_bytes`) | partial | S | none | Read |
| 9 | Decode to render to sink in `hearth_sink` | Planar `float` blocks to `libs/render` and `libs/device/include/iclforge/interleave.hpp`; level meter | `Decoder` returns interleaved `int32` (24-bit values) with a speaker list; no adaptor | missing | M | row 1 | Read `decoder.hpp` and the C6 page's interleave section |
| 10 | Capability advertisement in `/hardware` and the support object | `firmware/hearth-sink/main/board_caps.cpp`; `max_coded_channels`, `max_objects` per profile | None; a TrueHD limit set (channels, rate, depth, coded rate, substreams) is not defined | missing | S after row 6 | row 6 | Read |
| 11 | Stream set, play baselines and CI shapes | `firmware/hearth-sink/www/streams.json`, `suite.json`, `baselines/*.json`; QEMU shapes in `.github/workflows/_build.yml` | None; no TrueHD file in `firmware/hearth-sink/www/` | missing | L | rows 1, 3, a stream set | Read; `ls firmware/hearth-sink/www` |
| 12 | Flash budget in the profile | `profiles.json` `app_bytes_max`; C6 4 MB slot 1,966,080 B with the image at 1.87 MB | Decode-side objects 37,874 B at `-Os` on x86-64; RV32 and Xtensa unmeasured. The C6 4 MB slot has about 96 KB spare | partial | S | row 1 | `size` on the objects; C6 figure from the `sdkconfig.c6` comment |
| 13 | Loss and late-chunk behaviour | A late chunk is dropped and the decoder reset; DD+ resumes at the next syncframe, 32 ms; `concealment` setting | `Decoder::lost_data()` makes every substream wait for its next restart header. The encoder's default interval is 128 units, 107 ms at 48 kHz, and a stream can set up to 4,096; no concealment | partial | M | Q7 | Read `decoder.hpp`, `decoder.cpp`, `forge help truehd-encode` |
| 14 | Second-core decode lane | `decode-lane` executor for per-channel transforms and objects ([ESP32-S3](../docs/platforms/bare-metal/esp32-s3.md)) | Single-threaded. Channels within a block filter independently, but a substream reads the one before | missing, optional | L | row 4's numbers | Read |
| 15 | Objects (JOC, OAMD) | Reconstruct and place; P4 12 objects, S3 bed only | The Atmos substream is not built (on hold on the evolution payload, [truehd.md](truehd.md)) | missing | XL | owner's evolution layout | Read |
| 16 | Presentation or downmix to the sink's channel count | §7.8 output stage in the decoder and `libs/render` | `Presentation::k2ch`, `k6ch`, `k8ch`; 2.0 costs 0.28 to 0.43 of a full decode and its substream is 19% to 29% of the bytes. No tool drops substreams; no downmix of a 7.1 stream without downmix substreams | partial | S for a sender-side substream drop | Q3 | Harness runs with `Presentation` set; per-substream sizes from `parse_access_unit` |
| 17 | Documentation: per-chip pages and the sink guides | `docs/platforms/bare-metal/esp32-*.md`, `docs/hearth/sink-esp32-*.md` list the DD+ forms and figures | Nothing | missing | S | rows 4 and 6 | Read |

By gap: 10 missing (rows 1, 3, 4, 6, 9, 10, 11, 14, 15, 17), 6 partial (5, 7, 8, 12, 13, 16), 1 not applicable (2).
By size, not counting row 2: S 6 (5, 8, 10, 12, 16, 17), M 6 (1, 3, 4, 7, 9, 13), L 3 (6, 11, 14), XL 1 (15).

## What TrueHD has that DD+ never needed, in this area

- **A rate 10 to 100 times higher and variable.** The DD+ stream set runs 192 to 640 kbit/s; the TrueHD
  fixtures 0.6 to 4.2 Mbit/s, the streams above up to 14 Mbit/s, and the encoder's default ceiling is
  18 Mbit/s. Rings, chunk limits and `buffer_capacity` in bytes were sized for DD+. A link limit and a
  buffer measured in time are new.
- **Access units 38 times shorter.** 0.833 ms against 32 ms. A chunk has to group tens of units; sending one
  unit per chunk is 1,200 chunks a second.
- **MAT.** The HDMI carrier is a fixed 61,440 B per 20 ms whatever the content. Fine for HDMI, not a
  network format.
- **A restart interval as the recovery time.** DD+ resumes in a syncframe. TrueHD resumes at the next
  restart header, up to 107 ms at the encoder's default, and a join waits for one.
- **A lossless check.** The check of each restart interval is compared at the next restart header; a sink
  could report it as a health counter ("this play was bit-exact"). DD+ has CRCs only.
- **24-bit samples and 44.1 to 192 kHz.** Every sink states `[48000]`, and the slot widths are 16 and 32 bit
  (`interleave_16in16`, `interleave_24in32`). A 24-bit PCM output is already built; a 96 or 192 kHz clock is not.
- **Cost by channels and rate, not by tools.** DD+ cost depends on Annex E tools (enhanced coupling is 2 to
  3 times plain). TrueHD cost scales with channel count times sample rate; there are no optional tools in
  the decoded streams that change it by more than the filter order.
- **Seek granularity** is the restart interval, which belongs to the player and container audits.

## A proposed definition of "first class" for the ESP32 sinks

For the owner to approve or change.

1. The ESP-IDF component builds the TrueHD decoder, decode only, for each chip it lists, behind a Kconfig
   switch that is off by default, with its size gated as AC-4's is.
2. Each tier states, in `GET /hardware` and the Sendspin support object, the TrueHD streams it plays
   (channels, sample rate, bit depth, peak coded rate), and refuses a stream outside that set by name before a
   decoder opens, as `max_coded_channels` does.
3. A probe on the host, on the Cortex-M3 leg and under QEMU on RISC-V, with PCM hashes pinned to the
   manifest's SHA-256 values and ceilings for instructions, heap and stack, in CI.
4. A board timing table for the C6, S3 and P4 in the format of the DD+ tables, for the fixtures and for a
   stream set that includes a 7.1 stream, a 96 kHz stream and a noise stream, with Wi-Fi up.
5. A sink play over Sendspin (or the decision to have none) with zero underruns over ten minutes for every
   stream the tier states, the criterion already used for the P4 and S3.
6. A decoder footprint that fits the tier's free heap and decode stack at the shipped profile, stated as a
   measured figure per tier.
7. A defined behaviour after a lost or late chunk, tested.
8. The sender knows each sink's limits and chooses from: the stream, its core presentation, PCM, refusal.
9. The per-chip pages and sink guides list what TrueHD each tier carries, with figures.

## Plan

Phases are in dependency order. "No decision" means the owner need not answer anything first.

**Phase 0: measurement on the host and the emulated legs. No decision. Rows 3, 5 (measure), 12.** Add a TrueHD
probe beside the AC-4 probe in `firmware/baremetal` (a separate build, because the minimum-footprint profile
excludes TrueHD) that decodes the fixtures, prints heap, stack and a PCM hash, and runs under `-icount` on
the Cortex-M3 leg and under `qemu-riscv32`. Check the PCM against the SHA-256 values in `testdata/truehd/manifest.json` on the host leg and pin the probe's hashes. Size M. Risks:
the toolchain legs live in CI, not here; the profile's guard needs a TrueHD-aware exception. Done when: the
probe's hashes equal the manifest on the x86 host, the M3 leg and the C3 leg, and the M3 instruction count
exists, which replaces this page's host-ratio estimates with the repository's own proxy.

**Phase 1: the decoder's footprint. No decision beyond Q4 (internal change, behaviour-preserving). Row 5.**
Size the substream state by the stream's channel count and substream count, drop the restart copy and the
temporary, size the three buffers by the stream's samples per unit. Size S to M. Risk: the 16-channel
substream needs the large case; the state is reused across restarts, so the change must keep the history
carry-over of ERRATA entry 13. Done when: all of `iclforge-truehd-tests`, the hand-built streams and the
FFmpeg interop legs give the same PCM, the stack is under 16 KB and the heap for 2.0 and 5.1 is under
about 60 KB (Phase 0's probe measures both).

**Phase 2: the component and a file or HTTP play. Waits on Q1. Rows 1, 8, 9, 12.** A `CONFIG_ICLFORGE_TRUEHD`
switch, the profile guard, a streaming framer, the adaptor from the decoder's `int32` frames to the render and
sink path, and a `.thd` source in `hearth_sink` for the partition, FAT and HTTP sources (no Sendspin yet).
Size L. Risk: the S3's and P4's decode stack goes to 40,960 B; the P4 rev v1.x opens only 2 channels. Done
when: the capture-sink shapes in QEMU play a fixture with levels equal to the host's, and the pack check
builds the archive for every listed target.

**Phase 3: the board table. Waits on Q1 and board time (Q9). Rows 4, 5, 11.** Run the stream set on the
S3b, P4a and, if Q1 includes it, the C6 under the bench rules, with Wi-Fi up and again with a null sink.
Size M. Done when: each tier has a table in its platform page and a "carries" statement that replaces this
page's estimates; the S3's multiply cost and PSRAM placement are answered.

**Phase 4: Sendspin carriage and capability advertisement. Waits on Q2, Q3, Q5, Q6. Rows 6, 7, 10, 11, 13.**
A TrueHD data type in the role (an added value, not a new version, per the extension document), the
chunk grouping, the support-object limits, the settings for loss, `hearth-testserver` support, a play
baseline row per stream. Size L to XL, shared with the Hearth audit. Done when: a ten-minute group play at
each stated limit shows no underruns, and an over-limit stream is refused by name.

**Phase 5: the sender's fallbacks. Waits on Q3. Rows 6, 16.** A tool and an engine step that drop
substreams to the presentation the sink states, PCM for stereo, refusal otherwise. Size M. Done when: a 7.1
stream with downmix substreams plays on a sink limited to 2.0 as its stereo substream, bit-exact against
the host's decode of that substream.

**Phase 6: CI and documentation. No decision once the phases above land. Rows 11, 17.** The QEMU shapes, the
stream-set manifest, the per-chip pages and the sink guides. Size M.

**Phase 7: objects. Blocked on the evolution layout. Row 15.** XL; see [truehd.md](truehd.md).

Phases 0 and 1 need no decision and can start at once. Phase 2 onwards wait on one or more answers.

## Open questions

1. **Which tiers are in scope for TrueHD?** The P4 and S3 only, or the C6 too? Recommendation: P4 and S3. The
   C6 fails on heap, stack, ring and (above 2.0) CPU, and the tier page already calls 2.0 its durable ceiling
   ([esp32-sink-tiers.md](esp32-sink-tiers.md)); state the C6 as refusing TrueHD by name, and revisit only if
   Phase 1 gets the 2.0 decoder under about 60 KB and a board shows the ring problem solved.
2. **What carries TrueHD over Sendspin?** Raw access units grouped into chunks by duration and size, as E-AC-3
   groups syncframes into a burst, or MAT bursts as the existing packer writes them? MAT is 61,440 B for 20 ms
   regardless of content (24.6 Mbit/s) and is over both chunk limits. Recommendation: raw units grouped to at
   most about 20 ms and the sink's `max_chunk_bytes`, a new data type, with MAT kept for HDMI and S/PDIF.
3. **What does Hearth do when a sink cannot take a stream?** Options: send the core presentation, send
   PCM, send Opus, transcode to E-AC-3, or refuse. Recommendation: the highest presentation the sink states, then
   stereo PCM, then refuse; no transcode, which is lossy, drops the lossless point and is not built.
4. **May `libs/truehd`'s decoder be restructured for footprint?** Behaviour-preserving, pinned by the existing
   tests. Recommendation: yes. It is the largest single obstacle (a 25 KB stack frame, 156 KB of fixed state).
5. **Which sample rates does a sink play?** Today `[48000]`. Recommendation: 48 kHz first (44.1 kHz and the
   96 and 192 kHz families refused by name), and decide later whether 96 kHz needs a clock and DAC study; a
   sample-rate converter in the sink would reintroduce a lossy step.
6. **What is "real time with margin" for a stated limit?** Recommendation: the repository's own, zero underruns
   over at least ten minutes in a group play (exit criterion 2 of the tier study), plus a sink burst under 0.7
   of its duration, with the numbers in the support object measured, not estimated.
7. **How should a sink behave after a lost or late chunk?** The decoder is silent until the next restart header
   (107 ms at the encoder's default; a stream's own interval may be longer). Recommendation: accept that and
   have a server that can start chunks on restart boundaries do so; no concealment in the sink for now.
8. **Is the board time available?** Phase 3 needs S3b, P4a and, per Q1, the C6, under the bench rules.
   Recommendation: book them once Phase 2 builds; until then the figures in this page are estimates and say so.
9. **May the minimum-footprint profile admit TrueHD?** Its configure guard lists `ICLFORGE_BUILD_TRUEHD` among
   the options that conflict with it. Recommendation: a separate decode-only option for the component, the way
   the AC-4 decoder is a `CONFIG_` switch, not an exception to the guard for the whole profile.
