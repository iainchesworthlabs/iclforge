# ICL Forge

ICL Forge is a clean-room C++23 implementation of AC-3, E-AC-3, and Dolby Atmos decoding and
encoding, and of AC-4 decoding and encoding. It also holds a TrueHD and MLP codec, written from a
specification the project's owner supplied. The repository contains the codec library and three
applications built on it. AC-4 is in the library, in `forge`, in the Forge GUI, in the Hearth
desktop player, and in the C, Python, Rust, and WebAssembly bindings. Crucible does not use it.
An ESP32 network sink decodes it in a Sendspin group when its firmware is built with the decoder;
the published sink images are built without it. TrueHD is in the library, the C API and `forge`
(decode, inspect, wrap in MP4, Matroska, MPEG-TS and IEC 61937, and encode TrueHD); it is not in
the Forge GUI, Hearth, Crucible, the Python, Rust and WebAssembly bindings or any firmware.

## Choose what you need

| Goal | Start here |
|---|---|
| Learn what AC-3, E-AC-3, Atmos, AC-4, and TrueHD mean | [Concepts](concepts/index.md) |
| Install or use the CLI and GUI | [Forge](forge/index.md) |
| Capture desktop applications and position them in an Atmos scene | [Crucible](crucible/index.md) |
| Play audio through the desktop player or an ESP32 network sink | [Hearth](hearth/index.md) |
| Play, time, sing or make a karaoke stream | [Karaoke](karaoke/index.md) |
| Decode or encode AC-4 | [AC-4 in the library](library/ac4.md) |
| Decode or encode TrueHD or MLP | [TrueHD and MLP in the library](library/truehd.md) |
| Link the codec from C++, C, Python, Rust, or WebAssembly | [Library](library/index.md) |
| Compare products and features by platform and architecture | [Platforms](platforms/index.md) |
| Find the new name of something from a pre-release (`ac3cli`, `ac3forge`, ...) | [Renamed](renamed.md) |

The [Quick start](quickstart.md) covers installation, source builds, and the ESP32-S3 sink.

!!! note "Status"
    Releases are 0.x betas and the API is not stable. The
    [changelog](https://github.com/iainchesworthlabs/iclforge/blob/main/CHANGELOG.md) records
    shipped changes. Platform pages distinguish CI, emulation, and real-hardware results.

## Performance and quality

The *Measured on main* strip above loads the newest figures from the append-only
measurement history that CI keeps on the `quality-history` branch, and the line under it names the
commit they were measured on and says how old it is. Each tile has one line per codec
(AC-3, E-AC-3, and AC-4), and a line is the worst case among that codec's own workloads, with the
workload named. TrueHD has no series yet. The lines of two codecs come from different workloads and do not compare with each
other. A line whose series was last recorded on an older commit than the newest says which.

[Performance and quality](performance-quality.md) explains the measures and links to the full
performance, decoder-accuracy, listening-quality, object-quality, and memory histories.

## Project information

- [Capabilities](library/capabilities.md) — supported formats, coding tools, layouts, and limits.
- [Development status](library/development-status.md) — done / partial / not-started across every codec surface.
- [Application coverage](library/application-coverage.md) — which library features each
  application exposes.
- [Validation](verification.md) — how output is checked and where independent checking ends.
- [Building from source](building.md) — toolchains, presets, options, and platform details.
- [Renamed](renamed.md) — the old name and the new one of every program, library, package and setting.
- [Contributing](contributing.md) — repository structure and contribution requirements.

!!! warning "Standards and trademarks"
    "Dolby", "Dolby Digital", "Dolby TrueHD", and "Dolby Atmos" are trademarks of Dolby Laboratories.
    This project implements ATSC A/52:2018, ETSI TS 102 366, ETSI TS 103 420, and ETSI TS 103 190,
    and TrueHD from a specification its owner supplied. It is not
    affiliated with, endorsed by, or certified by Dolby Laboratories. Patent requirements
    depend on how and where the formats are used.
