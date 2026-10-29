# TrueHD and DD+ parity: performance and platforms

!!! note "Status as of 2026-10-29: the matrix, the host measurements, the arithmetic analysis and one AVX2 kernel are built; the small parts are not"
    The owner wants Dolby TrueHD to have what Dolby Digital Plus (DD+, E-AC-3; AC-3 and the JOC layer ride with it) has
    for performance and for platform-specific code: timing, instruction counts, memory and size measurements, the
    arithmetic tiers, SIMD, the minimum-footprint profile and the ESP32 parts. This page lists every such artifact DD+
    has beside its TrueHD counterpart (section 2), says what DD+ is measured on, where and against what (section 3),
    and, in the sections that follow it, the TrueHD measurements, the arithmetic analysis and the kernels that were built.
    The library, `forge`, `crucible` and `hearth` have pages of their own (`truehd-parity-library.md`,
    `-forge.md`, `-crucible.md` and `-hearth.md`), written on separate branches.

Names used below. **DD+** is E-AC-3 and its AC-3 core. **TH-ERRATA n** is open question n of
[libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md). **Qn** is a question in the last section of this page. The TrueHD plan is
[truehd.md](truehd.md); the DD+ arithmetic plan is [arithmetic-tiers.md](arithmetic-tiers.md).

## 1. Basis

Commit `523cf5dc` (branch `integ`), audited in a worktree of branch `task-perf`. The machine is an x86-64 Xeon at 2.1 GHz
with AVX2 and AVX-512 (F, DQ, CD, BW, VL, IFMA), four cores shared with other agents, GCC 13.3, `valgrind` 3.22. There is no
QEMU, no `arm-none-eabi` or RISC-V cross compiler and no board, so every Cortex-M3, ESP32 and arm64 leg below is read, not
run. Clang 18 is installed and can be asked for another `--target` without running the result.

| Tag | What |
|---|---|
| `grep` | A named `grep` over the tree; the result is quoted. |
| `bench` | `iclforge-bench`, `iclforge-membench` or `iclforge-kernelbench` from the existing build of this commit, run here. |
| `cg` | `valgrind --tool=callgrind` on a scratch harness that links `libs/truehd` at `-O2 -g` (section 4 says how the split is taken). |
| read only | I opened the file and did not run anything. |

## 2. The matrix

One row per performance or platform artifact DD+ has. Gap: **none**, **partial**, **missing**, or **n/a** with the reason.
Size is the ROADMAP's scale: S an afternoon, M a day or two, L a focused week, XL several PRs. "Blocked by" is a question
(Qn), another row or nothing. A row marked **built** was closed by a later commit of this branch; the section named says how.

### 2.1 Host measurement

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| P1 | Hard real-time gate: every workload within twice its real-time budget, in the pull-request gate | `tests/performance/test_performance.cpp`: 10 cases (AC-3, E-AC-3, Atmos, AC-4; encode and decode), label `Performance`, `kSlackFactor` 2.0 | Four more cases, the same four workloads, the whole 2.5 s fixture each (3,000 access units) | none | - | - | `iclforge-perf "the TrueHD*"`: 4 test cases, 12,010 assertions, all pass |
| P2 | Whole-frame trend: ms per frame, p95 and max per frame, fed real programme material | `tests/performance/bench_encoder.cpp` (`iclforge-bench`): 14 workloads, 200 frames each, three runs, the fastest kept | Four workloads (stereo and 5.1, encode and decode; a frame is an access unit) and, with `--truehd-fixtures`, a decode row for each of the 12 committed streams and an encode row for each TrueHD one | none | - | - | Section 4; `iclforge-bench --only truehd_51_decode` | |
| P3 | Per-kernel trend: ns per call of each hot kernel, fed real audio | `tests/performance/kernel_bench.cpp` (`iclforge-kernelbench`): 34 kernels | 19 kernels: the decoder's CRC and parity, residual reader (Huffman, raw), filters (FIR 8, FIR 4 with IIR 4, IIR 8), matrices (pair, six), noise, output and lossless fold; the encoder's autocorrelation, Levinson, filter quantiser, residual, code lengths, coding choice, bit estimate and bit packing | none | - | - | Section 4; the decoder kernels are checked to give the source back before they are timed | |
| P4 | Memory trend: allocations and bytes per frame, live growth, peak transient, peak RSS | `tests/performance/bench_memory.cpp` (`iclforge-membench`), `platform/{linux,windows}/mem_probe.cpp` | The same four names; the encode rows count three restart intervals | none | - | - | Section 4 | |
| P5 | Workloads reach the history: one record per leg and name, trailing-mean tiers, same-CPU baseline | `tools/ci/append_performance_history.py`, `append_kernel_history.py`, `append_memory_history.py`; unit tests beside them | The scripts key on the workload name and need no change; there is no TrueHD input | none | - | - | Run against the new rows in a scratch history: `append_performance_history.py` (two commits, no flag), `append_kernel_history.py`, `append_memory_history.py` (new series warn once, as designed) |
| P6 | Pre-merge comparison against the merge base, speed and heap, with the hard tier and the approval labels | `tools/ci/compare_performance.py`, `compare_memory.py`; jobs `performance-compare`, `performance-gate`, `Memory gate` in `.github/workflows/_compare.yml` | The scripts build `iclforge-bench`, `iclforge-kernelbench` and `iclforge-membench` and compare by name, so a TrueHD row is compared once the binaries carry it | none | - | - | `compare_performance.py` on a base without and a head with the TrueHD rows: each is listed `new`, none fails |
| P7 | The card, badge and hero tables that name each workload's codec, and refuse a name they do not list (`UnknownWorkload`) | `tools/ci/write_measurement_badges.py`, the script of `docs/performance-quality.md`, `docs/javascripts/hero-stats.js`; `tools/ci/test_write_measurement_badges.py` holds the three equal | Lists no TrueHD name. A TrueHD row in the history makes the badge step of `persist-performance-trend` fail | none | - | - | The four names are in all three tables; TrueHD is added to the speed, allocation and growth lines once the series holds one. `test_write_measurement_badges.py`: 54 tests, OK |
| P8 | The trend page's account of what is measured | `docs/performance-trend.md`, "What is measured, and on what" | No TrueHD text | none | - | - | A paragraph on the workloads in `docs/performance-trend.md`; the table prints three significant digits below 1 ms |
| P9 | Second ISA: the whole-frame bench on arm64 (Neoverse-N2) every merge | `ci.yml` job `performance-trend-arm64` runs `iclforge-bench` only | Runs TrueHD rows once P2 lands; no kernel or memory series on arm64 for DD+ either | partial | S | P2 | read only |
| P10 | The machine each record was measured on | `.github/scripts/capture-bench-environment.sh` writes `environment.json` beside the bench output | Generic | none | - | - | read only |
| P11 | Layering allowance for the bench targets | `tools/checks/projects.json`: project `tests` may use `ac3`, `ac4`, `dsp` | `truehd` is not allowed, so `tests/performance` cannot link it | none | - | - | `tests` may use `base` and `truehd`; `truehd` exposes three headers (`check_layering.py`: 0 failures) |

### 2.2 Instruction counts, size and memory on the small parts

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| P12 | Instructions per frame on a Cortex-M3, deterministic: QEMU `-icount shift=0`, ceilings per fixture | `tools/checks/run_baremetal_probe.sh --icount` (decode `float` and `fixed`, `--encoder`, `--ac4`); tables in `docs/performance-trend.md` "Instructions per frame" | None | missing | L | Q1 | `grep -c truehd tools/checks/run_baremetal_probe.sh`: 0; QEMU and `arm-none-eabi` are absent here |
| P13 | Instructions per frame on the host, counted | None for DD+ (the host probe prints microseconds; the counted figure is the Cortex-M3's) | `tools/checks/truehd_instructions.py` (callgrind, per access unit and per second of audio) and `testdata/truehd-instruction-ceilings.json`; not run in CI, which has no valgrind | built, host only | - | - | Section 4 |
| P14 | Image size: `.text`, `.data`, `.bss`, table ROM, per-object attribution | `tools/checks/footprint_report.py`, `ICLFORGE_MAX_IMAGE_BYTES`, `build-footprint` job, `docs/performance-trend.md` "Static footprint" | None | missing | M | P17, Q1 | read only; the library has no minimum-footprint branch |
| P15 | Peak heap, retained bytes, allocations per frame, ceilings per fixture | `firmware/baremetal/probe.cpp`, `ac4_probe.cpp`; `testdata/ac4-probe-ceilings.json`, `tools/checks/check_probe_ceilings.py` | `iclforge-membench` gives the heap on the host (P4); no probe, no ceilings | partial | M | P17 | `iclforge-membench` is the host-side counter (section 4); no probe, no ceilings file |
| P16 | Stack a decode uses, read by painting | `firmware/baremetal/ac4_probe.cpp` | None | missing | S | P17 | read only |
| P17 | The probe itself: fixtures built into an image, key=value output, three shapes (Cortex-M3, ESP-IDF, host) | `firmware/baremetal/{probe,encode_probe,ac4_probe}.cpp`, `main.cpp`, `platform/{baremetal,host,esp32s3,esp32c3,esp32c6,esp32p4}`, `ICLFORGE_MINIMAL_AC4` | None. `tools/checks/projects.json` does not let `baremetal` use `truehd` | missing | L | Q1 | `grep -rli truehd firmware`: no file |
| P18 | PCM pinned across legs: x86-64, Cortex-M3, RV32IMC, board all on one hash | `testdata/fixed-probe-pcm-hashes.json`, `ac4-probe-pcm-hashes.json`, `tools/checks/check_probe_hashes.py` | The fixtures' SHA-256 is pinned in `testdata/truehd/manifest.json` and checked on the host by `test_ffmpeg_fixtures.cpp`; no probe leg | partial | S | P17 | `grep pcm_sha256 testdata/truehd/manifest.json` |
| P19 | Stage timers and zone markers: where a frame's time goes on a part with no profiler | `ICLFORGE_ZONE_SCOPED_N` in `libs/ac3/src` (E-AC-3 encoder alone has 20 or more), `firmware/baremetal/stage_timers.cpp`, `libs/base/variants/profiling-stage_timers` | Five zones in `Decoder::decode_block` (residuals, filters, noise, matrices, output); none in the encoder | partial | S | - | `grep -rn ICLFORGE_ZONE libs/truehd`: 5 in `decoder.cpp`. They expand to nothing without Tracy or the stage-timer directory |
| P20 | Minimum-footprint profile: a decode-only or encode-only static archive, no exceptions, none of the host-only code | `libs/ac3/minimal.cmake`, `libs/ac4/minimal.cmake`, `cmake/MinimalDecoder.cmake`, options `ICLFORGE_MINIMAL_DECODER`, `_ENCODER`, `_AC4`, 12 presets in `CMakePresets.json` | `libs/truehd` builds as an ordinary library only; it links `base` for the bit reader, which the profile builds as headers | partial | M | Q1 | `grep -n MINIMAL libs/truehd/CMakeLists.txt`: no match |
| P21 | `-fno-exceptions` build of the codec | `libs/ac3`, `libs/ac4` (the probes assert it) | The 9 sources of `libs/truehd/src` compile with `-fno-exceptions -fno-rtti -Wall -Wextra -Wsign-conversion` and no diagnostic | none | - | - | `g++ -std=c++23 -O2 -fno-exceptions -fno-rtti -fsyntax-only` on each: exit 0, no output |

### 2.3 Arithmetic tiers

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| P22 | Decoder tiers: `double` reference, `float` for a single-precision FPU, `Fixed32` for parts with none, each gated | `ICLFORGE_DECODE_SCALAR`, `libs/ac3/variants/decode-scalar-*`, `libs/dsp/variants`, `check_decode_scalar_snr.py` (121 dB and up, fixed against double), `check_probe_hashes.py` | **n/a**: the decoder is exact integer arithmetic and has one tier; the gate is bit equality with the source | n/a | - | - | read only |
| P23 | Encoder tiers: `float` analysis front end, quality held to 0.5 dB of `double` | `ICLFORGE_ENCODE_SCALAR`, `libs/ac3/variants/encode-scalar-*`, `check_encode_scalar_quality.py` | The analysis is `double` and integer with no switch. Measured in section 5: a `float` analysis changes the bytes of 18 of 18 inputs and makes the stream 0.0 to 78 % larger, every one still lossless | partial | M | Q3 | Section 5 |
| P24 | The scalar building blocks shared across codecs | `libs/base/internal/iclforge/base/arithmetic/{fixed32,mant_exp,scalar_math}.hpp` | Not used; a 64-bit multiply-accumulate on integers is a different primitive | n/a | - | - | read only |
| P25 | The lossless gate as the tier gate | n/a (DD+ is lossy; SNR floors stand in) | Every variant decodes to the source sample for sample (`libs/truehd/tests/decoder/test_ffmpeg_fixtures.cpp`, `tools/checks/verify_truehd_encode_interop.py`) | none | - | - | read only |
| P26 | An effort axis for the encoder: named levels with measured cost and quality | `delta_allocation` (`reduced`), planned `minimal`; `arithmetic-tiers.md` "The effort axis" | `EncoderConfig::max_fir_order`, `matrix_search`, `restart_interval_units` exist; no named levels, no measured cost or size | partial | M | Q4 | `grep -n max_fir_order libs/truehd/include/iclforge/truehd/encoder/encoder.hpp`; the cost of each knob is not measured |

### 2.4 SIMD and platform-specific kernels

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| P27 | The SIMD seam: one `simd.hpp` per architecture directory, chosen by CMake, no conditional in a kernel | `libs/base/variants/arch-{generic,x86_64,aarch64}` (`f64x2`, `f32x4`, a small `i32x4`), `ICLFORGE_SIMD` | Not used, and not needed by the AVX2 kernel, which has a tier of its own as ac3's MDCT kernels do. The seam has no integer multiply or 64-bit lane, so an integer kernel in the seam's SSE2 and NEON tiers would need new operations | missing | M | Q2 | `grep -n 'struct\|operator' libs/base/variants/arch-x86_64/iclforge/base/detail/simd.hpp` |
| P28 | AVX2 tier selected at run time, kernel bodies in their own translation unit, plain signatures, a stub for builds without it | `libs/ac3/src/internal/avx2/`, `has_avx2()` in `libs/base`, option `ICLFORGE_AVX2`, `iclforge_ac3_simd_avx2` | `libs/truehd/src/internal/avx2/` (the encoder's bit count) and `none/`, `iclforge_truehd_simd_avx2`; the same option and probe | partial | L | Q2, Q6 | Section 6: one kernel of the three that qualify |
| P29 | NEON | The `arch-aarch64` seam; exercised by the arm64 leg | Not written. The scalar kernels build on arm64 as they are; a NEON body needs the seam change of P27 and could be compiled here but not run | missing | M | Q2 | read only |
| P30 | WASM SIMD | None: Emscripten lands on `arch-generic`; `docs/platforms/wasm.md` says "no WASM SIMD" | None | none | - | - | `grep -n 'no WASM SIMD' docs/platforms/wasm.md` |
| P31 | AVX-512 | None: `arch-x86_64/simd.hpp` says wider than SSE2 needs run-time dispatch; the AVX2 tier is the only one | None | none | - | - | read only |
| P32 | Scalar-against-vector tests: bit-exact on real data and on random inputs, a mutation-proof | `libs/ac3/tests/core/test_simd_kernels.cpp`, `libs/ac3/tests/core/avx2/{present,absent}` | `libs/truehd/tests/encoder/test_coded_bits_simd.cpp`: 7.1 million comparisons on all eight fixtures and 40,000 random blocks; five mutations each fail it. The tests compile the tier's translation unit or its stub into their binary and skip on a CPU without AVX2, in place of ac3's present/absent header pair | built, one kernel | - | - | Section 6 |
| P33 | The ESP32-S3's PIE and other part-specific kernels | None in the tree for DD+ either: the S3 runs the `float` tier on the generic seam | None | none | - | - | `grep -rn 'PIE\b' libs firmware`: three comments (`libs/ac3/minimal.cmake:226`, `libs/render/include/iclforge/render/float_biquad.hpp:14`, `firmware/esp-idf/iclforge/idf_component.yml:52`), no code |

### 2.5 Platforms

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| P34 | ESP-IDF component with a Kconfig switch per codec | `firmware/esp-idf/iclforge` (`CONFIG_ICLFORGE_AC4`), `idf_component.yml`, `esp-component.yml` | No TrueHD option | missing | L | Q1 | `grep -rn -i truehd firmware/esp-idf`: no match |
| P35 | ESP32-S3 under QEMU in CI: correctness, image size, heap floor | `tools/checks/run_esp32s3_probe.sh`, job in `_build.yml` | None | missing | M | Q1, P17 | no QEMU here |
| P36 | ESP32-C3 under `qemu-riscv32`: the fixed tier on RV32IMC | `tools/checks/run_esp32c3_probe.sh` | None | missing | M | Q1, P17 | no QEMU here |
| P37 | Timed on a board: ESP32-C6, ESP32-P4, ESP32-S3 | `docs/platforms/bare-metal/esp32-{c6,p4,s3}.md`, `tools/hearth/board_suite.py`, `firmware/hearth-sink/baselines/` | None | missing | L | Q1 | no board here |
| P38 | Cortex-M3 under QEMU in CI | `build-footprint` and the ESP lanes of `_build.yml`, `docs/platforms/bare-metal/cortex-m3.md` | None | missing | M | P17 | no QEMU here |
| P39 | Image size and profile guards for the Hearth sink | `.github/workflows/_esp_size.yml` (4 MB C6 slot), `_profiles.yml` (each board's locked configuration) | n/a: they hold `firmware/hearth-sink` images, which have no TrueHD | n/a | - | the Hearth page | read only |
| P40 | Per-platform pages with measured numbers | `docs/platforms/{linux,windows,macos,raspberry-pi,android,wasm}.md`, `bare-metal/*.md`, `memory-layout.md`, `board-bench.md` | One TrueHD mention in `linux.md` (an HDMI sink) | missing | M | P2 | `grep -il truehd docs/platforms/*.md docs/platforms/bare-metal/*.md`: `linux.md` |
| P41 | The generated support tables (`docs-snippets/generated/platform-*.md`) | `tools/checks/generate_support_matrices.py` from its data | No TrueHD row | missing | S | the library page | `grep -il truehd docs-snippets/generated/*.md`: no file. Generated: not edited here |
| P42 | The WASM module's real-time figure | `docs/platforms/wasm.md`: AC-3 385x, E-AC-3 120x, Atmos 82x under Node | `apps/demos/wasm` does not link `truehd` | missing | M | the library page | `grep -rn -i truehd apps/demos/wasm`: no match |
| P43 | Byte-reproducible encoder output across platforms | `testdata/bitstream-hashes.json`, `tools/checks/check_cross_platform_hash.py` | No golden bytes: the encoder is held to FFmpeg's decode, not to a hash | partial | S | Q3 | `grep -rn sha256 libs/truehd/tests/encoder`: none of encoder output |

## 3. What DD+ is measured on

This is what the matrix rows rest on, in one place.

**On every merge to `main`, nightly and on request** (`ci.yml`, job `persist-performance-trend`, then `performance-trend-arm64`):
`iclforge-bench` three times, the fastest kept, with the CPU model recorded; `iclforge-kernelbench` and `iclforge-membench`
once. The x86-64 leg is `linux-gcc` on `ubuntu-latest`; the arm64 leg runs `iclforge-bench` alone. Records go to the
`quality-history` branch as `performance-main.jsonl`, `kernels-main.jsonl` and `memory-main.jsonl`, one record per (leg, name,
commit). A run is compared with the trailing ten records of its own CPU model: 20 % slower warns, 100 % slower fails the job after
the push (speed and memory), and the kernel series never fails. The README badges and the cards of `docs/performance-quality.md`
read the same files. **Before a merge** (`_compare.yml`) the entry's head and its base are built and measured, three
interleaved runs for speed and one for the heap; a hard regression fails `performance-gate` or `Memory gate` unless the
pull request carries `perf-regression-approved` or `memory-regression-approved`. **In the pull-request gate** `iclforge-perf`
holds ten workloads to twice their real-time budget.

**On the small parts**: instructions per frame on a soft-float Cortex-M3 under QEMU (`run_baremetal_probe.sh --icount`),
decode in `float` and `fixed`, encode, and AC-4, each fixture with a ceiling in the script; image size (`.text`, `.bss`),
peak heap, retained bytes, allocations per frame and stack, each with a ceiling; PCM hashes pinned across the host, the
Cortex-M3 leg and RV32IMC; the ESP32-S3 and ESP32-C3 probes under QEMU. Real time on a board (ESP32-S3, C6, P4) is measured by
hand with `tools/hearth/board_suite.py` and kept as baselines under `firmware/hearth-sink/baselines/`; CI publishes none of it.

**What DD+ is not measured on**: Windows, macOS, WASM and Android have no series (their pages carry figures from single runs);
there is no kernel or memory series on arm64; no tier of the repository uses AVX-512, WASM SIMD or the S3's PIE.

## 4. TrueHD in the measurement machinery

**What was added.** Four workloads, stereo and 5.1 at 24 bits and 48 kHz, encode and decode, named `truehd_stereo_encode`,
`truehd_51_encode`, `truehd_stereo_decode` and `truehd_51_decode`, in `iclforge-perf`, `iclforge-bench` and `iclforge-membench`;
19 kernels in `iclforge-kernelbench`; `iclforge-bench --truehd-fixtures`, which adds a decode row for each of the 12 streams in
`testdata/truehd` and an encode row for each of the eight TrueHD ones; and `tools/checks/truehd_instructions.py`, which counts
instructions under valgrind. A frame in these rows is an access unit (40 samples, 0.83 ms at 48 kHz). The input is
`reference_51.wav`, in the file's own channel order, which is TrueHD's 5.1 assignment; it is a 16-bit file, so the 24-bit rows put its
samples in the high 16 bits and a fixed pseudo-random byte beneath, which stands for a master's noise floor and is incompressible. The
history, comparison and badge scripts needed only the names (the badge tables needed them in three places, see P7). The decoder's
inner loops were moved into `libs/truehd/src/decoder/kernels.hpp` so each can be timed and checked on its own; the PCM of all 24
committed streams is the same before and after.

**Whole workloads**, x86-64, GCC 13 `-O3`, the fastest of five runs on a machine with other work on it (the load average was 5 at the
time, and 12 to 15 earlier; a wall-clock figure here moves by a factor of two or three with it, and the instruction counts do not):

| Workload | Microseconds an access unit | Times real time | Instructions an access unit | Million instructions a second of audio |
|---|---:|---:|---:|---:|
| `truehd_stereo_decode` | 2.06 | 404 | 24,074 | 28.9 |
| `truehd_51_decode` | 6.01 | 139 | 59,974 | 72.0 |
| `truehd_stereo_encode` | 19.6 | 42 | 265,778 | 318.9 |
| `truehd_51_encode` | 53.4 | 15.6 | 732,361 | 878.8 |

The DD+ rows measured in the same session: `ac3_51_decode` 291 times real time, `eac3_51_decode` 352, `ac4_51_decode` 110, `plain_51_fast_mdct`
63, `eac3_51_auto` 52, `ac4_51_encode` 9.8. TrueHD decodes at the speed of AC-4 and encodes at the speed of E-AC-3 and AC-4, with 15 to 40
times the data rate to move. The committed streams (`iclforge-bench --truehd-fixtures`; 300 access units each, so an encode
here is two and a bit restart intervals):

| Stream | Decode: us/unit, times real time | Encode: us/unit, times real time |
|---|---|---|
| `truehd_mono48_s24` | 1.45, 574 | 8.5, 98 |
| `truehd_st48_s16` | 2.45, 340 | 20.1, 41 |
| `truehd_st48_s24` | 2.78, 300 | 21.0, 40 |
| `truehd_st44_s24` | 3.88, 234 | 22.0, 41 |
| `truehd_st96_s24` | 4.80, 174 | 38.8, 21.5 |
| `truehd_st192_s24` | 10.3, 81 | 100.6, 8.3 |
| `truehd_m51_s16` | 5.81, 144 | 55.0, 15.1 |
| `truehd_m51_s24` | 6.94, 120 | 57.5, 14.5 |
| `mlp_st44_s16` / `mlp_st48_s24` / `mlp_st96_s24` / `mlp_m51_s24` | 3.40, 267 / 4.40, 189 / 6.14, 136 / 8.09, 103 | not written (the encoder writes TrueHD only) |

The slowest decode is stereo at 192 kHz (81 times real time), because an access unit there holds 160 samples; per second of audio the cost
follows channels times sample rate. A 7.1 stream at 96 kHz is 8/6 times 2, or 2.7 times, the work of 5.1 at 48 kHz.

**Where a decode goes**, in instructions (callgrind, `-O2 -g`, inside `Decoder::decode`, attributed by source line to the kernels):

| Kernel | 5.1, 16-bit source | 5.1, 24-bit | Stereo, 24-bit |
|---|---:|---:|---:|
| Prediction filters (FIR 8 and IIR, `inverse_filter`) | 30.8 % | 28.2 % | 30.1 % |
| Bit reader and Huffman (`read_residuals` and `BitReader`) | 27.7 % | 33.4 % | 30.0 % |
| Output: channel assignment, shift, PCM write (`assign_output`) | 13.1 % | 12.0 % | 8.2 % |
| `memset` and `memcpy` (clearing and copying the rows) | 8.1 % | 7.5 % | 15.7 % |
| CRC-8 and parity of the segments | 6.6 % | 9.5 % | 6.6 % |
| Matrices (`apply_matrix`) | 4.8 % | 2.2 % | 0.0 % |
| Noise generator | 2.8 % | 2.6 % | 3.3 % |
| Headers, restart, the rest | 5.6 % | 4.5 % | 6.1 % |

callgrind counts every iteration of a `rep`-prefixed instruction, so the 8 to 16 % for `memset` and `memcpy` is an overcount on
hardware; the rows buffers they clear are 10 columns wide for any channel count (`kRowStride`), which is wasteful, and a candidate for a
change that is not SIMD. The filters are a recurrence: each output is in the next sample's sum. Of the 3.9 million predictions in the 18-input run of section 5,
45 % have a sum above 2^31.

**Where an encode goes** (5.1, 16-bit source, one call with the lot; the same shares hold for a call a unit once the buffer fix below is in):

| Kernel | Share of instructions |
|---|---:|
| `coded_bits`, the bit count of a block under one coding (`choose_coding` tries about 30) | 41.7 % |
| FIR search: `fir_prediction` 13.8 %, `compute_residual` 9.4 % (nine orders a channel, a whole interval each) | 23.2 % |
| `autocorrelation` (double) | 12.1 % |
| `memset` and `memmove` | 3.4 % |
| `choose_coding` itself, `estimated_bits` | 4.6 % |
| Bit packing: `BitWriter::put`, `put_residual` | 3.6 % |
| `process_interval`, `try_pair` (the pair matrix search) | 4.5 % |
| `std::cos` (the window) and `std::log2` | 1.6 % |

**Memory** (`iclforge-membench`; a frame is an access unit):

| Workload | Setup | Steady allocations an access unit | Steady bytes an access unit | Live growth | Peak transient |
|---|---:|---:|---:|---:|---:|
| `truehd_stereo_decode` | 156,040 B in 5 | 0 | 0 | 0 | 0 |
| `truehd_51_decode` | 156,040 B in 5 | 0 | 0 | 0 | 0 |
| `truehd_stereo_encode` | 41,504 B in 6 | 15.8 | 25,672 | 0 | 1,885,168 B |
| `truehd_51_encode` | 123,472 B in 8 | 30.5 | 54,929 | 0 | 3,508,624 B |

The decoder takes 156 KB of state in five allocations whatever the stream is (it is sized for the sixteen-channel substream: four
substreams of sixteen channels' histories), then allocates nothing: DD+'s decoders allocate 3 to 35 times a frame. The encoder's peak is
what one restart interval takes to analyse, 1.9 MB for stereo and 3.5 MB for 5.1, which no part with less than 4 MB of RAM can give; its
steady allocations are the interval's planes and vectors, once an interval.

**Found on the way, and fixed.** `Encoder::encode` called `reserve(size + n)` on its pending input on every call. libstdc++ reserves
exactly what it is asked for, so a caller that gives less than a restart interval a call copied the whole of the pending PCM on each
call. `reference_51.wav` at 24 bits, fed an access unit at a time, took 2,734 ms; it takes 356 ms with the buffer sized once (7.7 times
faster, and the same 1,215,734 bytes). The pending input was also kept after the intervals that had consumed it until it passed 1 MiB, so
the memory bench read a live growth of 122,880 and 368,640 bytes; it reads 0. These are `e05a9fd8` and `9a2c5c12`. It is the shape of
issue 544 for E-AC-3, found here by the same bench.

**Not run.** Everything with a Cortex-M3, an ESP32, an arm64 machine or Windows in it. The arm64 leg of CI will run the four whole-frame
rows on its first merge and nothing else of TrueHD's; the x86 leg will run all three benches. No baseline is recorded: a baseline is the
first merge to `main` after this branch, by the `Publish performance trend` job. `truehd_instructions.py` needs valgrind, which the CI
images do not have, so its ceilings are held by whoever runs it.

## 5. Arithmetic

**The decoder is exact integer arithmetic and has one tier.** There is no rounding anywhere in it: the specification gives the filters
and matrices as integer sums with a shift, and a decoder that gives other bits than the stream's is wrong, not approximate. `double` could
hold the sums (53 bits against the 42 they reach) and `float` could not (24), but nothing is gained by running them on an FPU. DD+'s
three tiers exist because a lossy decode has a quality that arithmetic trades; a lossless one has a pass mark. The tiers collapse to one,
and the variants of that one are *implementations*, not scalars: scalar, a wider vector unit, a different multiply. The gate that holds
every variant is stronger than DD+'s SNR floors: **every variant of a decoder kernel must give the same samples as the scalar for every
input, and the decode of every stream must equal its source** (the FFmpeg fixtures' SHA-256, the encoder's round trip, the lossless check
in every restart header). No variant needs an SNR threshold, a pinned hash set per platform or a decision about how far is far enough.

**The widths each decoder kernel needs.** Measured on the 12 FFmpeg fixtures and 12 hand-built streams (588,000 filtered samples) and on
the encoder's output for 18 inputs, real audio and synthetic, mono to 7.1 (3.9 million samples), with the kernels instrumented:

| Kernel | Operands | Largest sum seen | Accumulator needed | Samples whose sum does not fit 32 bits |
|---|---|---|---|---|
| FIR and IIR prediction (`inverse_filter`) | history 24 bits (2^22.2 seen), coefficient 16 bits (2^15.0), up to 8 taps, shift 0 to 15 | 2^34.7 in the streams of other encoders, 2^37.2 in this encoder's | 40 bits seen; 43 by the format (24 + 16 + 3) | 16 % and 45 % |
| Matrices (`apply_matrix`) | row 24 bits (2^22.1), coefficient scaled to 2^-14 (2^14.0), up to 8 terms | 2^35.3 and 2^36.1 | 40 bits | 61 % and 39 % |
| Output shift and lossless fold | 32-bit words, shifts by the stream's amount | 32 bits | 32 bits | none |
| CRC-8, parity, bit reader, Huffman | bytes and bit fields up to 32 bits; a Huffman symbol times a power of two up to 2^24 | 32 bits | 32 bits (the code uses 64 for `value`) | none |
| Noise generator | a 32-bit LFSR state | 32 bits | 32 bits | none |

So the two kernels that carry the time need a **32-bit by 32-bit multiply with a 64-bit accumulate**, or a 40-bit accumulator at the
least, and a 32-bit accumulator is wrong for 16 to 61 % of the samples. What that means for each part, from the instructions the
compilers emit for an eight-tap sum (clang 18, `-O2`, loop unrolled and not vectorised; **compiled, not run**):

| Part | 64-bit multiply-accumulate | Instructions a tap | Consequence |
|---|---|---:|---|
| x86-64 | `imul` on 64-bit registers | 3.9 | the measured baseline |
| AArch64 | `smaddl` | 2.0 | about half of x86's |
| Cortex-M3 and M4 | `smlal` (32 by 32 into 64) | 3.0 | as x86; no soft-float to pay, the decoder has none |
| Cortex-M0 and M0+ | none: `__aeabi_lmul` per tap | 13.0 and a library call | the multiply is a library call per tap |
| ESP32-C3 and C6 (RV32IMC) | `mul` and `mulh` (two instructions), then a carry chain | 7.5 | about twice x86's per tap |
| ESP32-P4 (RV32IMAFC, PIE) | as RV32IMC; PIE not used | 7.5 | as the C6, at 360 MHz |
| ESP32-S3 (LX7) | `mull` and `mulsh` of the 32-bit multiplier option; PIE's multiply-accumulate is 8 and 16 bits wide | not compiled: no Xtensa target in clang here | like RV32IMC; I could not check PIE's accumulator width against a manual, and say so |

The static counts say a tap costs 1.9 times x86's on RV32IMC; the measured x86-64 decode of 28.9 million instructions a second of stereo
audio and 72 million of 5.1 would then be near 55 and 135 million on an ESP32-C3 or C6 at best (one instruction a cycle, nothing for the
flash cache or WiFi), against 160 million cycles available. Stereo is plausible on those parts, 5.1 is at the edge, and 7.1 at 96 kHz (about
2.7 times 5.1) is not; that is the argument for Q5. These are estimates from instruction counts and not measurements, and a board would
replace them (Q1).

**The encoder's analysis** is the only inexact part: `autocorrelation`, `levinson`, `quantise_fir` and `estimated_bits` are `double`,
and `std::cos` (the window) and `std::log2` come from the C library. Everything the decoder reads is integer, so the choice of
arithmetic here changes the *encoded bytes* and never their correctness: every variant below decoded to its source, sample for sample, on
every input. Eighteen inputs (reference_51.wav at 24 and 16 bits in stereo and 5.1, the eight TrueHD fixtures' own PCM at their rates,
six synthetic signals of 1 to 8 channels):

| Variant | Bytes identical to the `double` encoder | Stream size against it | Decodes to the source |
|---|---|---|---|
| `double`, `std::cos` replaced by a correctly rounded cosine (stands for another C library's last bit) | 18 of 18 | identical | 18 of 18 |
| `float` internals in the four analysis functions | 0 of 18 | -0.02 % to +78 %: +3 % to +47 % on the eight fixtures' material, +10 % to +78 % on `reference_51.wav`, none on the six synthetic signals | 18 of 18 |

A `float` analysis is not a tier: the autocorrelation of 24-bit samples over 5,120 terms needs about 53 bits, and the predictors it gives
cost up to 78 % more stream on real audio (103,496 bytes became 184,088 for the 16-bit stereo reference). An exact integer
autocorrelation (an `int64` or `__int128` sum; the products reach 2^48 and the sum 2^61) would remove the order dependence that stops a
vectorised sum being bit-identical to the scalar one, and would fix the encoder's bytes across platforms, at the cost of changing them once:
the Levinson recursion and the quantiser stay `double`, which the project's flags (`-ffp-contract=off`) already make the same on every
IEEE machine. What is left platform-dependent is the C library's `cos` and `log2`: the 18 inputs show no sensitivity to the last bit of
`cos`, and nothing in the repository pins the encoder's bytes across platforms, so the question of whether to is the owner's (Q3).

## 6. SIMD and platform-specific kernels

**The threshold.** A kernel is built for a vector unit when it is an exact integer computation (so a vector body can be bit-identical to
the scalar, which is the only kind the repository's seam accepts) and takes at least 20 % of the instructions of a workload of section 4.
Twenty per cent because a kernel sped up four-fold removes 15 % of the whole at that share, which is the least that repays a second body, a
stub, a test and a CMake target to keep. Four kernels clear it:

| Kernel | Share | Exact? | State |
|---|---:|---|---|
| Encoder: `coded_bits` | 41.7 % of a 5.1 encode | integer | **built** (AVX2) |
| Encoder: the filter search's residual (`fir_prediction`, `compute_residual`) | 23.2 % | integer, no recurrence | not built |
| Decoder: the prediction filters (`inverse_filter`) | 28 to 31 % of a decode | integer, a recurrence | not built |
| Decoder: bit reader and Huffman (`read_residuals`) | 28 to 33 % | integer, sequential | not built: not a SIMD kernel |

`autocorrelation` (12.1 %) is under the line and is `double`, so a vector body would change the encoded bytes; it is the kernel Q3 is about.

**Built: the encoder's bit count, AVX2.** `libs/truehd/src/internal/avx2/coded_bits_avx2.cpp` is the only translation unit built with
`-mavx2`, with plain signatures, a `none/` stub, a call site in `encoder/coding.cpp` behind `has_avx2()`, and the scalar body kept as
`coded_bits_reference`: the layout of `libs/ac3/src/internal/avx2`. It needs no change to the seam. Eight residuals a step in 32-bit
lanes, a gather for the code lengths, and the scalar's own 64-bit arithmetic for a tail of fewer than eight and for a block with a
residual outside +-2^30 (which the encoder's own residuals never are). It is held to the scalar by `test_coded_bits_simd.cpp`: the
residuals the encoder's analysis finds in all eight TrueHD fixtures at four block sizes and three filter orders, with every codebook and
the widths and offsets around the chosen ones, and 40,000 random blocks of 1 to 200 residuals at every magnitude to 2^30, every codebook,
width and offset, with the edges of the lanes planted; 7.1 million comparisons, feasible and infeasible both. Five mutations of the vector
body (the shift, the table index, the sum, the length limit, the raw bound) each fail it. What it bought, x86-64, the fastest of three runs,
against `ICLFORGE_SIMD_TIER=sse2`, which takes the tier away:

| | Scalar | AVX2 | |
|---|---:|---:|---|
| `choose_coding`, a block of 40 | 6.4 us | 2.8 us | 2.3 times |
| `truehd_51_encode`, an access unit | 71.4 us | 54.3 us | 1.31 times |
| `truehd_stereo_encode`, an access unit | 25.7 us | 21.4 us | 1.20 times |
| `truehd_coded_bits`, one feasible 40-residual candidate | 84 ns | 87 ns | none |

The last row is the kernel alone on one candidate that fits: the vector body pays for its set-up and the scalar one for nothing, and they tie.
The gain is in the candidates that do not fit and in the raw codings, which the scalar counts to the end or leaves late. The streams are
byte for byte the same under both tiers (`reference_51.wav` 497,026 bytes and `reference_stereo.wav` 189,940, by `cmp`).

**Not built, and why.**

- *The encoder's filter search (23 %).* It has no recurrence and an 8-tap 64-bit dot product vectorises across samples with
  `vpmuldq`; the work is a second body and its tests on the pattern above. Left for the next round; nothing blocks it.
- *The decoder's prediction filters (28 to 31 %).* Each output is in the next sample's sum, so the filter is latency-bound along a
  channel: a vector across the eight taps would wait on the same chain, and the scalar loop already issues the eight multiplies together.
  What can be vectorised is *across channels*, each with its own chain, which wants the block's samples channel-major where
  `decode_block` keeps a row of 10 columns per sample time (and clears and copies it every block, the `memset` and `memcpy` of 8 to 16 %
  in section 4). That is a change to the decoder's data layout, pinned by 24 streams and 88 test cases, and it is a question before it is
  code (Q6). A scalar version that interleaves the channels would let an out-of-order core overlap their chains with no vector unit at all,
  and is the first thing to try.
- *NEON, WASM SIMD and the ESP32 parts.* Nothing is written. The repository's seam has no integer multiply or 64-bit lane for the first
  (Q2); WASM has no SIMD tier for DD+ either; the S3's PIE multiply-accumulate is 8 and 16 bits wide as far as I could tell, narrower than the
  24-bit samples, and I had no manual to check. The scalar kernels build for aarch64, thumbv7m and riscv32 (section 5 compiled the
  sums); no vector body for them was compiled, so there is nothing to label "compiled, not run".
- *AVX-512.* This machine has it, the repository's pattern has no AVX-512 tier for DD+, and the AVX2 body is the repository's pattern.

**Found and left alone.** The decoder clears and copies `kRowStride` (10) columns of every sample time for every channel count and every
block, which is 8 to 16 % of a decode's instructions on x86-64 and more on a part with a slow `memset`; and its state is 156 KB for any
stream. Both are data-layout changes to the decoder and belong with Q6.

## 7. Questions for the owner

Each has a recommendation; none is settled here. Sections that follow the matrix add to these as the measurements come in.

1. **Which platforms must TrueHD reach?** DD+ is built for every hosted platform, WASM, the Cortex-M3 leg and four ESP32
   parts. TrueHD at 7.1, 24 bit and 96 kHz is a different load from E-AC-3 5.1: a coded bit rate of several Mbit/s against
   0.4 Mbit/s, and a 64-bit multiply-accumulate in every filter and matrix. *Recommendation:* hosted platforms and WASM first
   (P2 to P6, P9, P42); the Cortex-M3 probe (P12, P17) next, because it is the only deterministic counter; then the ESP32-P4 and
   S3 for stereo and 5.1 at 48 kHz; the ESP32-C3 and C6 only for a core-only or stereo decode (Q5), if at all.
2. **May the SIMD seam grow integer operations?** The seam (`libs/base/variants/arch-*/iclforge/base/detail/simd.hpp`) has
   `f64x2`, `f32x4` and an `i32x4` with a load, a broadcast, a store, a subtract and a left shift. A TrueHD kernel needs a
   32 by 32 to 64 bit multiply and a 64-bit add. *Recommendation:* yes, as a separate type (`i64x2` with `mul_widen`), added
   to all three directories in one change, but only when a kernel needs it on a part the seam serves; until then TrueHD
   takes an AVX2 tier of its own on the pattern of `libs/ac3/src/internal/avx2`, which needs no seam change.
3. **Should the TrueHD encoder's output be byte-reproducible across platforms?** The decoder is exact. The encoder's
   analysis uses `double` and `std::cos` and `std::log2`, so two platforms may choose different filters for the same input
   and write different, equally lossless, streams. *Recommendation:* decide against the owner's wish for golden hashes: if
   they are wanted, move the analysis to exact integer arithmetic and pin hashes in `testdata`; if not, keep `double` and
   hold the encoder to the lossless gate alone, as now.
4. **Should the encoder's effort axis match E-AC-3's?** `max_fir_order`, `matrix_search` and `restart_interval_units`
   already trade size for time. *Recommendation:* name three levels from the measured costs once the owner says which
   target needs the fast one.
5. **Is a core-only or reduced-rate decode acceptable on small parts?** TH spec §2.2 lets a decoder read substream 0 (stereo)
   or substreams 0 and 1 (5.1) of a stream that carries more. `Presentation::k2ch` and `k6ch` already do this.
   *Recommendation:* yes; it is the only way the ESP32-C3 and C6 decode a 7.1 stream at all.
6. **May the decoder's block buffer change shape?** It keeps a row of 10 columns per sample time whatever the channel count, clears it
   and copies it every block (8 to 16 % of a decode's instructions), and any vector filter across channels wants the samples
   channel-major. *Recommendation:* yes, as one change to `decode_block` before any decode-side SIMD, pinned by the 24 streams and the
   test cases that exist; the scalar channel-interleaved version first, because it may need no vector unit.
7. **Should CI run `tools/checks/truehd_instructions.py`?** It is the only deterministic cost figure TrueHD has without QEMU, and it
   needs valgrind, which the CI images do not carry. *Recommendation:* in the nightly run only, with the ceilings in
   `testdata/truehd-instruction-ceilings.json`, on the image that already runs the benches; or not at all until the Cortex-M3 leg exists.
