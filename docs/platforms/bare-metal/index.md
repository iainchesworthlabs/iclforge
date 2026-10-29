# Bare metal

Five targets, one profile: `iclforge::ac3_minimal`, the minimum-footprint build of the codec — one
static library, no exceptions, no RTTI, decode-only or encode-only, and none of the direct-form
transform tables. What differs between the targets is the part and, on parts with no
floating-point unit, the arithmetic tier the decoder runs in
([the plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/arithmetic-tiers.md)).

AC-4 is built from its own libraries (`libs/ac4`), the decoder only:
no bare-metal build has the AC-4 encoder. The [Cortex-M3](cortex-m3.md#status) leg probes it, and the
[ESP32-P4](esp32-p4.md#ac-4) decodes it behind `CONFIG_ICLFORGE_AC4`, to 5.1 in real time. The
[ESP32-S3](esp32-s3.md#ac-4) decodes it with its state in PSRAM, under QEMU in CI and on a board,
where 2.0 keeps up (phase D14c). The [ESP32-C6](esp32-c6.md#ac-4) and the ESP32-C3 build it in the
fixed-point tier (phase D14d of
[`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#d14-ac-4-on-the-esp32s));
a decode does not fit a C6 beside WiFi, and on a board the play is refused.
A Hearth sink built with the decoder takes AC-4 in a Sendspin group on the P4 and the S3
([Hearth](../../hearth/index.md#esp32-sinks)); the published sink images have the decoder off.

TrueHD and MLP (`libs/truehd`) are in none of these builds: the component, the probes and the sinks do not
compile the library, and nobody has measured what a decoder for its rates (up to 192 kHz) and channel counts (up to 8, or 16
in the Atmos substream) would need from these parts. The variant table has the row, and the
[roadmap](https://github.com/iainchesworthlabs/iclforge/blob/main/ROADMAP.md#truehd-and-e-ac-3-as-equals-proposed) the work.

The variant table keeps codec support, Hearth support, distribution and evidence separate.

--8<-- "docs-snippets/generated/platform-bare-metal.md"

Whether a part is viable at all comes down to floating point, not RAM — the comparison across the
wider ESP32 family, why the ESP32-P4 was declined as an S3 *replacement*, and the complementary
C6 / S3 / P4 sink modules on one dual-ES9080 PCB, are on
[ESP32-C3 → Why this part](esp32-c3.md#why-this-part-and-not-another-esp32-variant) and
[`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-sink-tiers.md).

## Which page

- **Building the library itself for a part with no operating system?** Start at
  [Cortex-M3](cortex-m3.md) — it's the reference leg CI measures the profile on, and the page
  that explains what the profile gives up.
- **Have an ESP32-S3 board?** [ESP32-S3](esp32-s3.md) is real time on real hardware, with two
  example players (`i2s_player`, `hearth_sink`) that drive I2S, and the Sendspin sink. AC-3 and
  E-AC-3 only.
- **Have an ESP32-C3 (or another part with no FPU)?** [ESP32-C3](esp32-c3.md) covers the
  fixed-point tier and what has and hasn't been measured on it. AC-3 and E-AC-3 only.
- **Have an ESP32-C6?** [ESP32-C6](esp32-c6.md) has the fixed-point and float tiers timed on a
  board, with and without WiFi and a stream arriving, and which streams fit. AC-3 and E-AC-3
  only.
- **Have an ESP32-P4?** [ESP32-P4](esp32-p4.md) is the "best" tier of the shared sink family. Its
  probe is real time on every fixture with no network, and `hearth_sink` plays a paired Sendspin
  stream over Wi-Fi through the board's onboard ESP32-C6. If the board is pre-production silicon
  like the one this was measured on, the page also has a chip-revision trap worth reading before
  flashing anything. It decodes AC-4 behind a switch, from an HTTP source or a Sendspin group:
  2.0 and 5.1 in real time, 5.1.4 slower ([AC-4](esp32-p4.md#ac-4)).
- **Porting, tuning or debugging memory on an ESP32 board?**
  [ESP32 memory layout](memory-layout.md) has the flash map, the internal RAM split, what lives in
  PSRAM, the heap floors and what each codec costs on the S3, the C6 and the P4, side by side.
- **Building with ESPHome instead of raw ESP-IDF?** [ESPHome](esphome.md) is the external
  component. It decodes AC-3 only, and it is not a `media_player` or `speaker` source.
- **Building `hearth_sink` for a dev kit, adding a Kconfig key or changing a board's
  configuration?** [ESP32 build profiles](build-profiles.md) has the locked configuration of each
  board, the guard CI holds a build to, the commands to build a board from its profile, and what a
  change to one has to carry.
- **Changing the ESP32 firmware, or a decoder it links?**
  [Testing and benchmarking the boards](board-bench.md) says what is tested on the host, under
  QEMU and in CI, what needs a board, and how a board's plays are held to a recorded baseline.

## Where to go next

- [Platforms](../index.md) is one level up — the full routing table across every target, not only
  the bare-metal ones.
- [Capabilities](../../library/capabilities.md) — the complete codec surface; the table above
  records where minimum-footprint targets narrow it.
