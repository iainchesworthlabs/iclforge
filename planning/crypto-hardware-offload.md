# Crypto hardware offload: audit, what was built, and what the boards measured

**Status, 2026-10-29:** built on branch `feature/crypto-sha256-hardware`, not committed. The PC
side is verified on this machine (Ryzen 9 5950X, MSVC); the ESP side was measured on four boards
(S3b, C6, P4a and the P4X kit, chip v3.2 at 400 MHz) and each was put back as found. The ARMv8
kernel has run only against a model of its instructions.

**The answer to "does hardware crypto buy capacity for decoding and encoding?" is section 6: on the
boards it does not reach the decoder (the AEAD runs on core 0, the decoder on core 1) and frees about
2 % of core 0 at today's coded lanes, 10 to 12 % if the audio were PCM; on the PC it frees about 0.6 %
of a fast encode frame.**

## 1. What crypto the repo has

| # | Routine | Where | When it runs | Path |
|---|---------|-------|--------------|------|
| 1 | SHA-256, HMAC-SHA-256: the EMDF Atmos tag, `HMAC(key, A\|\|B)[:np/8]` | `libs/base/src/crypto/{sha256,hmac_sha256}.cpp`, used by `libs/ac3/src/signing/emdf_atmos_signer.cpp` | Every frame, only when the operator supplies a key (CLI, GUI, Shield demo, Python bindings); `*_any` once per candidate key | Host only: `libs/ac3/minimal.cmake` takes just `cpu_features.cpp` from base, so **no ESP image has it**. Now SHA-NI / ARMv8 SHA2 where the CPU has them, portable C++ otherwise |
| 2 | Signing key load, decode, zeroise | `libs/base/src/crypto/signing_key.cpp` | Once | n/a |
| 3 | Image SHA-256 (streamed), Content-Digest, index hash | `firmware/esp-idf/iclforge/src/firmware.cpp`, `firmware_pull.cpp` | Per update | `psa_hash_*` -> mbedTLS -> the SHA peripheral |
| 4 | Index signature check (RSA-PKCS1/PSS, ECDSA-P256, SHA-256) | `firmware_pull.cpp`, mbedTLS `pk`/`x509` | Once per update check | mbedTLS |
| 5 | Sendspin handshake (Noise, CPace: X25519, SHA-256, HMAC) | `libs/sendspin/src/{noise,cpace,hmac,suite,psk,pairing}.cpp` over `crypto/psa/crypto.cpp` | Per connection | PSA: vcpkg mbedTLS 3.6 on a PC, mbedTLS 4 in IDF |
| 6 | Sendspin transport AEAD, ChaCha20-Poly1305 (default) or AES-256-GCM | `noise.cpp` `CipherState`, `channel.cpp` | Per message on the encrypted channel (how much of that is audio: not read) | PSA |
| 7 | Host Python tooling | `tools/hearth/{sign_index,ota,index_verify,package_firmware}.py` | Build and release time | Not read in detail |

No SHA-512, no signature *generation* on a device, no on-device storage of the Atmos key.

## 2. What was built

**PC.** `Sha256` picks its compression function once per process: the x86-64 SHA extensions
(`hw/x86_64/sha256_kernel.cpp`), the ARMv8 SHA2 instructions (`hw/aarch64/sha256_kernel.cpp`), or the
portable code. The choice of directory is CMake's (`libs/base/CMakeLists.txt`), never an `#ifdef`
(`tools/checks/check_platform_macros.ps1` passes on 1,655 files): one kernel file per architecture
(the only files built with `-msha`/`-march=armv8-a+crypto`) and one probe per compiler or OS
(`hw/x86_64-{msvc,builtin}`, `hw/aarch64-{linux,macos,windows}`), `hw/none` for everything else.
`iclforge/base/detail/sha256_impl.hpp` exports the portable and hardware compressions for the tests
and the bench (three symbols, added to the ABI allowlist). `update()` now hands whole blocks straight
from the caller's buffer and `finish()` writes its padding in one step, which also made the portable
path about 15 % faster.

**Verification.** `libs/base/tests/test_crypto.cpp` holds the hardware compression to the portable
one over 2,000 random states, and the dispatching hash to an independent framing at every length from
0 to 600, split every way, plus FIPS 180-4's million-`a` and RFC 4231. A mutation check (one shuffle
constant in the SHA-NI kernel) fails the hardware case on its first trial and all four whole-hash
cases. Configurations built and tested: x86-64 MSVC Debug and Release, and `ICLFORGE_SIMD=generic`
(the `hw/none` build: the hardware case skips, 1,244 assertions pass).

**Benchmarks.** `tests/performance/crypto_bench.cpp` (`iclforge-cryptobench`, same JSON as the kernel
bench) for the PC. `firmware/crypto-bench/` for the boards: PSA's SHA, HMAC and AEAD against the
repo's portable C++ compiled for the chip, with on-chip equality checks, cycle counts from the CPU's
counter, stack and heap beyond an empty call, and the three experiments in section 5.
`tools/hearth/crypto_bench_report.py` turns the captures into the tables below.

**Not built, on purpose.** A per-chip HAL inside `base::crypto`: the code is not in any ESP image, and
the boards' hashing already goes through PSA with `CONFIG_MBEDTLS_HARDWARE_SHA=y`. No use of the
ESP32 HMAC peripheral: it only takes keys burned into eFuse, and the Atmos key is the operator's at run
time. No PIE staging: SHA runs on the buffers callers already hold.

## 3. Chip capability (from the built sdkconfigs)

| | S3 (rev 0.2) | C6 (rev 0.2) | P4 (rev 1.3 board, 360 MHz) |
|---|---|---|---|
| SHA engine | 1/224/256/384/512, DMA | 1/224/**256 only**, DMA | 1/224/256/384/512 + truncated, DMA |
| ECC | none | yes | yes (P-384; ECDSA verify) |
| AES-GCM in hardware | AES block, GHASH on the CPU | AES block, GHASH on the CPU | yes (`MBEDTLS_HARDWARE_GCM`) |
| SHA DMA buffer | 3,968 B | 3,968 B | 3,968 B |

## 4. Results (cycles from the CPU counter, fastest call; "software" is the repo's portable C++ at -O2)

| | 64 B | 1 KiB | 64 KiB | HMAC, 2,304 B |
|---|---|---|---|---|
| PC, one core, 5950X | 2.9x | 6.1x | 6.5x (334 -> 2,154 MB/s) | 9.6 us -> 1.3 us |
| S3, 240 MHz | 2.0x | 8.9x | 33x (3.0 -> 100 MB/s) | 872 us -> 152 us |
| C6, 160 MHz | 2.4x | 13.5x | 45x (2.0 -> 88 MB/s) | 1,317 us -> 142 us |
| P4 rev 1.3, 360 MHz | 1.2x | 6.2x | 30x (4.4 -> 133 MB/s) | 588 us -> 129 us |

- The hardware path has a **fixed cost of about 25 to 29 us a call on all three chips**, which is
  why small messages gain little (the P4 at 64 B: 25.2 us against 30.8 us). An HMAC through PSA
  costs 120 to 140 us on the boards even at 800 B (140, 128 and 122 us).
- Every on-chip comparison (16 to 18 a chip: PSA against `base::crypto`, bit for bit) passed.
- **AEAD.** On the P4, AES-256-GCM is 2.5x (1 KiB), 3.1x (1,500 B) and 6.7x (4 KiB) faster than
  ChaCha20-Poly1305, which is the Sendspin default (`ICLFORGE_SENDSPIN_SUITE=0`). On the S3 and C6 it
  is only 1.3 to 1.6x at 1 KiB and up, and on the S3 slower at 256 B (0.8x).
- A message in PSRAM hashes at 57 % (P4) or 32 % (S3) of the internal-RAM rate at 64 KiB; one in flash
  (no DMA) at 55 % (P4), 34 % (C6) and 75 % (S3) at 4 KiB.
- Memory beyond an empty call: stack 0.4 to 0.9 KB for every case; heap peak 0 for SHA, about 0.25 KB
  for an HMAC through PSA, 0.4 to 1.1 KB for AES-GCM.

## 5. The ESP32-P4 reference: three claims, checked

The claims came from a Zephyr issue and a Zephyr driver; the firmware here uses ESP-IDF's.

1. **"The crypto clock starts on the 40 MHz crystal; switch it to PLL_F240M."** IDF v6.1 already does
   (`esp_security_init` -> `esp_crypto_clk_init()` sets `CRYPTO_CLK_SRC_SEL` to 2, the 240 MHz PLL;
   `esp_security/src/init.c:68`), and the bench reads the selector back as 2 on P4a and on the P4X
   kit. The effect is real: forcing the crystal makes the same PSA hash 1.3x slower at 64 B, 2.2x at
   2.3 KB and 4.0x at 64 KiB on P4a (1.5x, 2.6x, 4.4x on the kit). Even on the crystal the hardware is
   7.6x faster than the portable C++ at 64 KiB here. Nothing to change.
2. **"Write a block, wait for `SHA_BUSY` to clear, then continue."** IDF's DMA path waits for idle. Its
   non-DMA path, used for input the DMA cannot read (flash-resident data), writes each block into the
   text registers and issues start or continue with no wait between blocks (`sha.c`
   `esp_sha_block_mode_fallback`; `sha_ll_fill_text_block` has no busy check): the pattern the
   reference warns of. **Measured: flash-resident messages of 64, 192, 1,024 and 4,096 B hash
   correctly on the S3, the C6, the P4 (revision 1.3) and the P4X kit (revision 3.2).** The reference
   puts the race on the later silicon, so the kit is the run that matters: there the 192 B and 4,096 B
   flash-resident hashes were each repeated 3,000 times, and the 2,304 B hash from internal RAM and
   from PSRAM too, 12,000 digests in all, none wrong. A race would be intermittent and this ran the
   path long enough that it would very probably have shown. That is "ESP-IDF v6.1's driver is not hit
   by it on a v3.2 chip", not a statement about the silicon; IDF has no revision-specific code in this
   path.
3. **"DMA lets the CPU step away to decode audio."** In IDF the DMA feeds the engine and the caller
   spins: `sha_hal_wait_idle()` is `while (sha_ll_busy())` (`sha.c:315`). **Measured: a task of lower
   priority on the same core keeps 0 % of the CPU during hardware SHA-256 calls (4 KiB and 64 KiB),
   same as software, on all three chips.** What the hardware buys is occupancy: a 64 KiB hash holds
   the core for 0.65 ms (S3), 0.74 ms (C6), 0.49 ms (P4) instead of 21.6, 33.5 and 14.9 ms. AES-GCM is
   interrupt-driven only above 2,000 B (`esp_aes_dma_core.c:62`), and leaves 5 % (S3, C6), 17 % (P4a)
   or 24 % (P4X) of the CPU at 4 KiB, 0 % at 1,500 B. A truly asynchronous SHA would be a driver of its own.

## 6. Does it buy capacity for decoding and encoding?

Short answer: **on the boards, not for the decoder; on the PC, about half a percent of a fast encode
frame.** Hashing is not in any audio path on a board, so the only crypto that costs a board anything
while it plays is the Sendspin AEAD, and two facts decide what it is worth.

**What the encrypted channel carries.** After the Noise handshake every WebSocket binary message is one
AEAD frame, audio chunks included (`libs/sendspin/include/iclforge/sendspin/channel.hpp`). A coded stream
is one burst chunk per access unit, 1,536 samples at 48 kHz, so 31.25 a second. The sizes are from the
Music Assistant programme's recorded runs, not a capture made for this report (every board was idle
during this work, and `GET /status` keeps no byte counter): 3,072 B a burst on the 768 kb/s lane
(7,130 of 7,130 bursts, 21,903,360 B), 1,792 B at 448 kb/s, and 512 B on the C6's 128 kb/s stereo lane.
A PCM stream is the other case: 5.1 at 48 kHz and 16 bits is 576 kB/s, stereo 192 kB/s.

**Where the AEAD runs.** On core 0. The WebSocket handler that calls `PlayerSession::receive` ->
`Channel::open` runs in the HTTP server's task, pinned to `SendspinHostConfig::core`, which the sink sets
to 0 (`firmware/hearth-sink/main/sendspin/player/sendspin.cpp:886`; `firmware/esp-idf/iclforge/src/sendspin_host.cpp:759,959`),
next to lwIP (`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`). The decoder is pinned to core 1
(`ICLFORGE_EXAMPLE_DECODE_CORE`, default 1). So cycles the AEAD stops using go to the network and
control core, not to the decoder. The C6 has one core, but its lane is 128 kb/s.

What the AEAD costs at those sizes (the measured seal times joined by straight lines between sizes; opening
is the same work and was not timed separately), as a share of core 0:

| coded lane, bytes a burst | S3 ChaCha / GCM | C6 | P4a (rev 1.3) | P4X kit (rev 3.2) |
| --- | --- | --- | --- | --- |
| C6 stereo, 128 kb/s, 512 B | 0.8 % / 0.8 % | 1.2 % / 1.0 % | 0.5 % / 0.4 % | 0.5 % / 0.3 % |
| 448 kb/s, 1,792 B | 2.6 % / 1.8 % | 3.9 % / 2.8 % | 1.6 % / 0.5 % | 1.5 % / 0.4 % |
| 768 kb/s, 3,072 B | 4.3 % / 2.8 % | 6.5 % / 4.4 % | 2.7 % / 0.5 % | 2.4 % / 0.4 % |

On the kit, AES-256-GCM instead of ChaCha20-Poly1305 frees 2.0 % of core 0 at the 768 kb/s lane (774 us a
burst falls to 126 us) and 1.1 % at 448 kb/s. The decoder's side of the same burst, from the recorded runs,
is 8 ms worst case on the kit, 18.7 ms on the S3 and 22.5 ms on the C6 against a 32 ms period, on the
other core.

If a board is played **PCM** instead, the arithmetic changes (per-byte cost from the 4 KiB seal; a PCM chunk is
capped by the sink's `max_chunk_bytes`, which on the C6 is 4,096 B):

| PCM stream | chip | ChaCha | AES-GCM | freed |
| --- | --- | --- | --- | --- |
| 5.1, 576 kB/s | P4X kit | 14.3 % | 1.9 % | 12.4 % |
| 5.1, 576 kB/s | P4a | 16.0 % | 2.4 % | 13.6 % |
| 5.1, 576 kB/s | S3 | 25.6 % | 16.0 % | 9.6 % |
| stereo, 192 kB/s | C6 (its one core) | 12.8 % | 8.6 % | 4.2 % |

So hardware AES-GCM is worth having where audio crosses the encrypted channel as PCM, on the P4 above all,
and worth about 1 to 2 % of core 0 where it carries coded bursts. It does not make the decoder faster
on any board, because they are not on the same core. (On the kit AES-GCM at 4 KiB also leaves 24 % of the
CPU to a lower-priority task on its core, for the part of the call above the 2,000 B interrupt threshold;
SHA-256 and ChaCha leave 0.) The suite is the Kconfig int `ICLFORGE_SENDSPIN_SUITE`; all ten profiles in
`firmware/hearth-sink/profiles.json` pin it to 0 (ChaCha), the three P4 ones included. Not changed here.

**PC.** The only crypto on a PC while audio is made is the Atmos HMAC, one per frame, on the encode side.
`iclforge-bench` (one run, this machine) puts a frame at 1.46 ms for E-AC-3 5.1, 1.49 ms for Atmos 4-object
with the QMF fast MDCT, 2.08 ms with the fast MDCT, 13.4 ms for the default Atmos path and 0.58 to 0.73 ms to
decode. The HMAC over a 2,304 B frame was 9.56 us and is 1.32 us:

| encode frame | frame | HMAC before | HMAC after | freed |
| --- | --- | --- | --- | --- |
| E-AC-3 5.1 | 1.455 ms | 0.66 % | 0.09 % | 0.57 % |
| Atmos 4-object, QMF fast MDCT | 1.490 ms | 0.64 % | 0.09 % | 0.55 % |
| Atmos 4-object, default | 13.375 ms | 0.07 % | 0.01 % | 0.06 % |

The decoders do no hashing. The Sendspin AEAD on a PC (a server sealing at most 96 kB/s per coded stream)
was not measured and is far below 0.1 % of a core for any implementation that does hundreds of MB/s. The
SHA-256 speed-up is real (6.5x) and matters for batch signing and `*_any`, which hashes once per
candidate key.

## 7. Not verified

- The ARMv8 kernel and its three probes have never run on an Arm CPU. The kernel's own source passes
  every check the x86 one does against a model of `SHA256H/H2/SU0/SU1` written from the Arm
  pseudocode (a check of structure and indexing, not of the silicon). The Arm CI legs
  (`windows-msvc-arm64`, `linux-gcc-arm64`, `macos-llvm`) will run the parity tests.
- The software baseline is the repo's portable C++, not mbedTLS's software SHA-256 (that needs a second
  build with `CONFIG_MBEDTLS_HARDWARE_SHA=n`).
- The lane sizes and the decoder budgets in section 6 are the Music Assistant programme's recorded
  figures, not measured here; no stream was played during this work. Seeing the bytes a sink really
  receives needs the owner's dev Music Assistant to play to a board (it is audible).
- The AEAD was timed sealing; opening is the same work and is assumed to cost the same.
- The kit's pass is one pass; the other boards' are 3 to 4 passes each (the fastest call of each case is
  reported). The earlier captures hold `task_wdt` warnings ("CPU 0: main", with a backtrace) from the
  bench's tight loop starving the idle task; each pass still ran to `DONE`. The kit's console was not
  captured, so whether it printed any is not known.
- "None of 12,000 digests was wrong" on the kit is evidence about ESP-IDF v6.1's driver on a v3.2 chip,
  not a proof about the silicon.

## 8. Running it on a board, and the traps that cost time

`firmware/crypto-bench/README.md` has the procedure. Traps found on 2026-10-29:

- IDF leaves ChaCha20-Poly1305 off; the product's Sendspin Kconfig turns it on. A bench without
  `CONFIG_MBEDTLS_CHACHA20_C` and `_CHACHAPOLY_C` fails its key import.
- **A board's layout and slot width are runtime settings, not in flash, and any reset (even esptool's
  read) puts them back to 2.0 and 32.** Record `GET /layout` and `GET /slot-width` first and `PUT` them
  back; S3b had been left on 5.1.2 at 16 bits. (The kit's were 9.1.6 and 32 before and after; its
  profile's default.)
- `bench_flash.py flash` waits for an HTTP answer a measurement image never gives and exits 1 after
  120 s; on a P4 esptool also exits 1 after a verified write (the port re-enumerates).
- Python buffers a redirected log until exit: use `python -u` to see "esptool exit".
- `bench.py console` puts a timestamp in front of every line; the report strips it.
- **The P4X kit has no USB on this PC** (its MAC `e753f9` is not among the USB devices, there is no
  COM7), so it is reached by OTA. The Hearth sink's update takes only an image of its own project name
  (`refuse_image`), so the kit's bench image is built under that name with the version
  `crypto-bench-ota`; it serves its pass on TCP port 8080 over the Ethernet port and returns the board
  to its own image by itself. The bootloader's app rollback is what makes that safe: the bench never
  marks itself valid, so any reset boots the previous slot. `ota.py` ends in "rolled back:
  crypto-bench-ota did not last" and exit 2; that is the run having worked.
- **That update overwrites the other OTA slot.** The kit's `ota_1` held `v0.10.0-beta.1-5557-gfdca21ef09`
  (a commit on the local, unpushed `bugfix/s3-wifi-rx-deadlock-and-p4-ac4-fold`; none of 397 build
  directories on D: had an image with its SHA). It now holds the bench image, marked invalid, and the
  kit still runs `v0.10.0-beta.1-5539-ge4a61aceab` from `ota_0`. Rebuild and push that commit if the
  slot is wanted back.
- ESP-IDF works out a component's requirements in a CMake script run without the cache, so a `-D` on the
  idf.py command line cannot decide `PRIV_REQUIRES`: the bench's main component always requires the
  network components, and the `-D` picks only the source file and the project name.
- A v3.x second-stage bootloader is 576 bytes over the window below the partition table at Info level
  (the Hearth sink's overlay says so); the bench sets `CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y` for its build to
  pass the size check, though it never writes the bootloader.

## 9. The measured matrix

(`python tools/hearth/crypto_bench_report.py`, from the captures of 2026-10-29 and
`iclforge-cryptobench --json-out`; `esp32p4` is the revision 1.3 board, `esp32p4x` the revision 3.2 kit.)

### Boards (cycles from the CPU's cycle counter; software = portable C++)

- esp32s3: chip revision 2, ESP-IDF v6.1, 240 MHz
- esp32c6: chip revision 2, ESP-IDF v6.1, 160 MHz
- esp32p4: chip revision 103, ESP-IDF v6.1, 360 MHz
- esp32p4x: chip revision 302, ESP-IDF v6.1, 400 MHz

### Does the hardware path give the portable answer?

| chip | comparisons | PSA against portable, bit for bit |
| --- | --- | --- |
| esp32s3 | 18 | all equal |
| esp32c6 | 16 | all equal |
| esp32p4 | 18 | all equal |
| esp32p4x | 22 | all equal |

### SHA-256: latency, fastest call

| bytes | esp32s3 software | esp32s3 hardware | speedup | esp32c6 software | esp32c6 hardware | speedup | esp32p4 software | esp32p4 hardware | speedup | esp32p4x software | esp32p4x hardware | speedup |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 64 | 13,061 cyc (54.4 us) | 6,656 cyc (27.7 us) | 2.0x | 11,158 cyc (69.7 us) | 4,568 cyc (28.6 us) | 2.4x | 11,084 cyc (30.8 us) | 9,056 cyc (25.2 us) | 1.2x | 11,084 cyc (27.7 us) | 7,056 cyc (17.6 us) | 1.6x |
| 256 | 28,184 cyc (117.4 us) | 7,449 cyc (31.0 us) | 3.8x | 26,824 cyc (167.7 us) | 5,484 cyc (34.3 us) | 4.9x | 26,820 cyc (74.5 us) | 10,704 cyc (29.7 us) | 2.5x | 26,827 cyc (67.1 us) | 8,693 cyc (21.7 us) | 3.1x |
| 1024 | 88,674 cyc (369.5 us) | 9,952 cyc (41.5 us) | 8.9x | 89,482 cyc (559.3 us) | 6,636 cyc (41.5 us) | 13.5x | 89,769 cyc (249.4 us) | 14,513 cyc (40.3 us) | 6.2x | 89,769 cyc (224.4 us) | 11,579 cyc (28.9 us) | 7.8x |
| 2304 | 189,494 cyc (789.6 us) | 12,346 cyc (51.4 us) | 15.3x | 193,912 cyc (1212.0 us) | 8,556 cyc (53.5 us) | 22.7x | 194,660 cyc (540.7 us) | 16,574 cyc (46.0 us) | 11.7x | 194,662 cyc (486.7 us) | 13,959 cyc (34.9 us) | 13.9x |
| 4096 | 330,642 cyc (1377.7 us) | 17,462 cyc (72.8 us) | 18.9x | 340,114 cyc (2125.7 us) | 12,216 cyc (76.3 us) | 27.8x | 341,520 cyc (948.7 us) | 23,192 cyc (64.4 us) | 14.7x | 341,520 cyc (853.8 us) | 19,949 cyc (49.9 us) | 17.1x |
| 16384 | 1,298,519 cyc (5410.5 us) | 45,203 cyc (188.3 us) | 28.7x | 1,342,770 cyc (8392.3 us) | 33,564 cyc (209.8 us) | 40.0x | 1,348,564 cyc (3746.0 us) | 54,008 cyc (150.0 us) | 25.0x | 1,348,561 cyc (3371.4 us) | 50,101 cyc (125.3 us) | 26.9x |
| 65536 | 5,172,894 cyc (21553.7 us) | 156,518 cyc (652.2 us) | 33.0x | 5,356,160 cyc (33476.0 us) | 118,952 cyc (743.5 us) | 45.0x | 5,380,700 cyc (14946.4 us) | 177,226 cyc (492.3 us) | 30.4x | 5,380,055 cyc (13450.1 us) | 170,621 cyc (426.6 us) | 31.5x |

### SHA-256: throughput, MB/s

| bytes | esp32s3 software | esp32s3 hardware | esp32c6 software | esp32c6 hardware | esp32p4 software | esp32p4 hardware | esp32p4x software | esp32p4x hardware |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 64 | 1.18 | 2.31 | 0.92 | 2.24 | 2.08 | 2.54 | 2.31 | 3.63 |
| 256 | 2.18 | 8.25 | 1.53 | 7.47 | 3.44 | 8.61 | 3.82 | 11.78 |
| 1024 | 2.77 | 24.69 | 1.83 | 24.69 | 4.11 | 25.40 | 4.56 | 35.37 |
| 2304 | 2.92 | 44.79 | 1.90 | 43.09 | 4.26 | 50.04 | 4.73 | 66.02 |
| 4096 | 2.97 | 56.30 | 1.93 | 53.65 | 4.32 | 63.58 | 4.80 | 82.13 |
| 16384 | 3.03 | 86.99 | 1.95 | 78.10 | 4.37 | 109.21 | 4.86 | 130.81 |
| 65536 | 3.04 | 100.49 | 1.96 | 88.15 | 4.38 | 133.12 | 4.87 | 153.64 |

### HMAC-SHA-256, 32-byte key: latency, fastest call

| bytes | esp32s3 software | esp32s3 hardware | speedup | esp32c6 software | esp32c6 hardware | speedup | esp32p4 software | esp32p4 hardware | speedup | esp32p4x software | esp32p4x hardware | speedup |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 800 | 88,337 cyc (368.1 us) | 33,639 cyc (140.2 us) | 2.6x | 85,558 cyc (534.7 us) | 20,492 cyc (128.1 us) | 4.2x | 85,755 cyc (238.2 us) | 43,808 cyc (121.7 us) | 2.0x | 85,746 cyc (214.4 us) | 35,766 cyc (89.4 us) | 2.4x |
| 2304 | 209,280 cyc (872.0 us) | 36,519 cyc (152.2 us) | 5.7x | 210,743 cyc (1317.1 us) | 22,776 cyc (142.3 us) | 9.3x | 211,586 cyc (587.7 us) | 46,275 cyc (128.5 us) | 4.6x | 211,575 cyc (528.9 us) | 38,489 cyc (96.2 us) | 5.5x |
| 4608 | 390,756 cyc (1628.2 us) | 42,480 cyc (177.0 us) | 9.2x | 398,627 cyc (2491.4 us) | 27,204 cyc (170.0 us) | 14.7x | 400,398 cyc (1112.2 us) | 53,913 cyc (149.8 us) | 7.4x | 400,397 cyc (1001.0 us) | 45,484 cyc (113.7 us) | 8.8x |

### HMAC-SHA-256, 32-byte key: throughput, MB/s

| bytes | esp32s3 software | esp32s3 hardware | esp32c6 software | esp32c6 hardware | esp32p4 software | esp32p4 hardware | esp32p4x software | esp32p4x hardware |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 800 | 2.17 | 5.71 | 1.50 | 6.25 | 3.36 | 6.57 | 3.73 | 8.95 |
| 2304 | 2.64 | 15.14 | 1.75 | 16.19 | 3.92 | 17.92 | 4.36 | 23.94 |
| 4608 | 2.83 | 26.03 | 1.85 | 27.10 | 4.14 | 30.77 | 4.60 | 40.52 |

### Sendspin AEAD seal: ChaCha20-Poly1305 against AES-256-GCM: latency, fastest call

| bytes | esp32s3 software | esp32s3 hardware | speedup | esp32c6 software | esp32c6 hardware | speedup | esp32p4 software | esp32p4 hardware | speedup | esp32p4x software | esp32p4x hardware | speedup |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 256 | 38,748 cyc (161.4 us) | 45,833 cyc (191.0 us) | 0.8x | 36,721 cyc (229.5 us) | 32,450 cyc (202.8 us) | 1.1x | 37,870 cyc (105.2 us) | 42,946 cyc (119.3 us) | 0.9x | 36,142 cyc (90.4 us) | 35,028 cyc (87.6 us) | 1.0x |
| 1024 | 118,270 cyc (492.8 us) | 92,188 cyc (384.1 us) | 1.3x | 116,797 cyc (730.0 us) | 88,034 cyc (550.2 us) | 1.3x | 112,202 cyc (311.7 us) | 45,630 cyc (126.8 us) | 2.5x | 110,358 cyc (275.9 us) | 38,048 cyc (95.1 us) | 2.9x |
| 1500 | 170,829 cyc (711.8 us) | 124,022 cyc (516.8 us) | 1.4x | 169,643 cyc (1060.3 us) | 123,090 cyc (769.3 us) | 1.4x | 161,358 cyc (448.2 us) | 52,350 cyc (145.4 us) | 3.1x | 159,466 cyc (398.7 us) | 44,102 cyc (110.3 us) | 3.6x |
| 4096 | 436,410 cyc (1818.4 us) | 273,094 cyc (1137.9 us) | 1.6x | 437,101 cyc (2731.9 us) | 294,513 cyc (1840.7 us) | 1.5x | 409,378 cyc (1137.2 us) | 61,471 cyc (170.8 us) | 6.7x | 407,454 cyc (1018.6 us) | 54,378 cyc (135.9 us) | 7.5x |

### Sendspin AEAD seal: ChaCha20-Poly1305 against AES-256-GCM: throughput, MB/s

| bytes | esp32s3 software | esp32s3 hardware | esp32c6 software | esp32c6 hardware | esp32p4 software | esp32p4 hardware | esp32p4x software | esp32p4x hardware |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 256 | 1.59 | 1.34 | 1.12 | 1.26 | 2.43 | 2.15 | 2.83 | 2.92 |
| 1024 | 2.08 | 2.67 | 1.40 | 1.86 | 3.29 | 8.08 | 3.71 | 10.77 |
| 1500 | 2.11 | 2.90 | 1.41 | 1.95 | 3.35 | 10.32 | 3.76 | 13.60 |
| 4096 | 2.25 | 3.60 | 1.50 | 2.23 | 3.60 | 23.99 | 4.02 | 30.13 |

### The Sendspin AEAD at the lanes' burst sizes (31.25 bursts a second)

| lane (bytes a burst) | chip | ChaCha us | AES-GCM us | ChaCha, core share | AES-GCM, core share | freed by GCM |
| --- | --- | --- | --- | --- | --- | --- |
| C6 stereo lane, 128 kb/s (512 B) | esp32s3 | 272 | 255 | 0.8 % | 0.8 % | +0.1 % |
| C6 stereo lane, 128 kb/s (512 B) | esp32c6 | 396 | 319 | 1.2 % | 1.0 % | +0.2 % |
| C6 stereo lane, 128 kb/s (512 B) | esp32p4 | 174 | 122 | 0.5 % | 0.4 % | +0.2 % |
| C6 stereo lane, 128 kb/s (512 B) | esp32p4x | 152 | 90 | 0.5 % | 0.3 % | +0.2 % |
| 448 kb/s lane (1792 B) | esp32s3 | 836 | 587 | 2.6 % | 1.8 % | +0.8 % |
| 448 kb/s lane (1792 B) | esp32c6 | 1,248 | 890 | 3.9 % | 2.8 % | +1.1 % |
| 448 kb/s lane (1792 B) | esp32p4 | 526 | 148 | 1.6 % | 0.5 % | +1.2 % |
| 448 kb/s lane (1792 B) | esp32p4x | 468 | 113 | 1.5 % | 0.4 % | +1.1 % |
| 768 kb/s lane (3072 B) | esp32s3 | 1,382 | 893 | 4.3 % | 2.8 % | +1.5 % |
| 768 kb/s lane (3072 B) | esp32c6 | 2,073 | 1,418 | 6.5 % | 4.4 % | +2.0 % |
| 768 kb/s lane (3072 B) | esp32p4 | 865 | 161 | 2.7 % | 0.5 % | +2.2 % |
| 768 kb/s lane (3072 B) | esp32p4x | 774 | 126 | 2.4 % | 0.4 % | +2.0 % |

### SHA-256 over PSA by where the message lives

| chip | bytes | internal MB/s | psram MB/s | flash MB/s |
| --- | --- | --- | --- | --- |
| esp32s3 | 64 | 2.31 | 2.31 | 2.31 |
| esp32s3 | 256 | 8.25 | 8.24 | - |
| esp32s3 | 1024 | 24.69 | 15.51 | 23.08 |
| esp32s3 | 2304 | 44.79 | 22.40 | - |
| esp32s3 | 4096 | 56.30 | 25.04 | 42.42 |
| esp32s3 | 16384 | 86.99 | 30.09 | - |
| esp32s3 | 65536 | 100.49 | 31.69 | - |
| esp32c6 | 64 | 2.24 | - | 2.25 |
| esp32c6 | 1024 | 24.69 | - | 13.53 |
| esp32c6 | 4096 | 53.65 | - | 18.15 |
| esp32p4 | 64 | 2.54 | 2.53 | 2.55 |
| esp32p4 | 256 | 8.61 | 8.56 | - |
| esp32p4 | 1024 | 25.40 | 21.63 | 21.70 |
| esp32p4 | 2304 | 50.04 | 39.22 | - |
| esp32p4 | 4096 | 63.58 | 46.60 | 34.91 |
| esp32p4 | 16384 | 109.21 | 68.29 | - |
| esp32p4 | 65536 | 133.12 | 76.37 | - |
| esp32p4x | 64 | 3.63 | 3.62 | 3.64 |
| esp32p4x | 256 | 11.78 | 11.77 | - |
| esp32p4x | 1024 | 35.37 | 28.87 | 27.15 |
| esp32p4x | 2304 | 66.02 | 49.26 | - |
| esp32p4x | 4096 | 82.13 | 57.14 | 40.41 |
| esp32p4x | 16384 | 130.81 | 79.26 | - |
| esp32p4x | 65536 | 153.64 | 87.84 | - |

### Crypto clock: crystal against the PLL ESP-IDF selects

| chip | selector at boot | bytes | crystal (cycles) | at boot (cycles) | speedup |
| --- | --- | --- | --- | --- | --- |
| esp32p4 | 2 | 64 | 12,150 | 9,056 | 1.3x |
| esp32p4 | 2 | 2304 | 37,189 | 16,574 | 2.2x |
| esp32p4 | 2 | 65536 | 704,965 | 177,226 | 4.0x |
| esp32p4x | 2 | 64 | 10,650 | 7,056 | 1.5x |
| esp32p4x | 2 | 2304 | 36,820 | 13,959 | 2.6x |
| esp32p4x | 2 | 65536 | 757,096 | 170,621 | 4.4x |

### CPU left for other tasks while the call repeats

| call | bytes | esp32s3 CPU left | esp32c6 CPU left | esp32p4 CPU left | esp32p4x CPU left |
| --- | --- | --- | --- | --- | --- |
| aead.aes256gcm.psa | 1500 | 0 % | 0 % | 0 % | 0 % |
| aead.aes256gcm.psa | 4096 | 5 % | 5 % | 17 % | 24 % |
| aead.chacha20poly1305.psa | 1500 | 0 % | 0 % | 0 % | 0 % |
| aead.chacha20poly1305.psa | 4096 | 0 % | 0 % | 0 % | 0 % |
| sha256.base | 4096 | 0 % | 0 % | 0 % | 0 % |
| sha256.base | 65536 | 0 % | 0 % | 0 % | 0 % |
| sha256.psa | 4096 | 0 % | 0 % | 0 % | 0 % |
| sha256.psa | 65536 | 0 % | 0 % | 0 % | 0 % |

### Memory beyond an empty call, in a task of its own

| case | esp32s3 stack / heap peak (B) | esp32c6 stack / heap peak (B) | esp32p4 stack / heap peak (B) | esp32p4x stack / heap peak (B) |
| --- | --- | --- | --- | --- |
| aead.aes256gcm.psa | 584 / 432 | 604 / 440 | 640 / 1,072 | 644 / 1,072 |
| aead.chacha20poly1305.psa | 820 / 0 | 812 / 16 | 784 / 0 | 788 / 0 |
| hmac_sha256.base | 864 / 0 | 848 / 16 | 772 / 0 | 760 / 0 |
| hmac_sha256.psa | 732 / 252 | 592 / 268 | 624 / 264 | 612 / 264 |
| sha256.base | 504 / 0 | 472 / 16 | 412 / 0 | 408 / 0 |
| sha256.psa | 820 / 0 | 508 / 16 | 792 / 0 | 788 / 0 |

### PC

| case | portable | hardware | portable MB/s | hardware MB/s | speedup |
| --- | --- | --- | --- | --- | --- |
| SHA-256 compression, 64 B | 220 ns | 75 ns | 291 MB/s | 856 MB/s | 2.9x |
| SHA-256 compression, 256 B | 793 ns | 152 ns | 323 MB/s | 1,685 MB/s | 5.2x |
| SHA-256 compression, 1024 B | 3,101 ns | 506 ns | 330 MB/s | 2,023 MB/s | 6.1x |
| SHA-256 compression, 2304 B | 6,959 ns | 1,100 ns | 331 MB/s | 2,094 MB/s | 6.3x |
| SHA-256 compression, 65536 B | 196,295 ns | 30,419 ns | 334 MB/s | 2,154 MB/s | 6.5x |
| SHA-256 compression, 1048576 B | 3,136,800 ns | 488,761 ns | 334 MB/s | 2,145 MB/s | 6.4x |
| HMAC-SHA-256 (dispatching), 800 B | - | 611 ns | - | 1,309 MB/s | - |
| HMAC-SHA-256 (dispatching), 2304 B | - | 1,324 ns | - | 1,740 MB/s | - |
| HMAC-SHA-256 (dispatching), 4608 B | - | 2,392 ns | - | 1,926 MB/s | - |
