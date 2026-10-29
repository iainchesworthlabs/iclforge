# TrueHD and DD+ parity: performance and platforms

!!! note "Status as of 2026-10-29: an audit; nothing in it is built yet"
    The owner wants Dolby TrueHD to have what Dolby Digital Plus (DD+, E-AC-3; AC-3 and the JOC layer ride with it) has
    for performance and for platform-specific code: timing, instruction counts, memory and size measurements, the
    arithmetic tiers, SIMD, the minimum-footprint profile and the ESP32 parts. This page lists every such artifact DD+
    has beside its TrueHD counterpart (section 2), says what DD+ is measured on, where and against what (section 3),
    and, in the sections that follow it, the TrueHD measurements, the arithmetic analysis and the kernels that were built.
    The sibling pages for the library, `forge`, `crucible` and `hearth` are
    [truehd-parity-library.md](truehd-parity-library.md), [truehd-parity-forge.md](truehd-parity-forge.md),
    [truehd-parity-crucible.md](truehd-parity-crucible.md) and [truehd-parity-hearth.md](truehd-parity-hearth.md).

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
| P1 | Hard real-time gate: every workload within twice its real-time budget, in the pull-request gate | `tests/performance/test_performance.cpp`: 10 cases (AC-3, E-AC-3, Atmos, AC-4; encode and decode), label `Performance`, `kSlackFactor` 2.0 | None | missing | S | - | `ctest -N -L Performance`: 10 tests, none TrueHD; `grep -rli truehd tests/performance`: no file |
| P2 | Whole-frame trend: ms per frame, p95 and max per frame, fed real programme material | `tests/performance/bench_encoder.cpp` (`iclforge-bench`): 14 workloads, 200 frames each, three runs, the fastest kept | None | missing | M | - | `grep`: no TrueHD name in `bench_encoder.cpp` |
| P3 | Per-kernel trend: ns per call of each hot kernel, fed real audio | `tests/performance/kernel_bench.cpp` (`iclforge-kernelbench`): 34 kernels | None | missing | M | - | `grep -c 'time_kernel(' kernel_bench.cpp`: 34 |
| P4 | Memory trend: allocations and bytes per frame, live growth, peak transient, peak RSS | `tests/performance/bench_memory.cpp` (`iclforge-membench`), `platform/{linux,windows}/mem_probe.cpp` | None | missing | M | - | `grep`: no TrueHD name in `bench_memory.cpp` |
| P5 | Workloads reach the history: one record per leg and name, trailing-mean tiers, same-CPU baseline | `tools/ci/append_performance_history.py`, `append_kernel_history.py`, `append_memory_history.py`; unit tests beside them | The scripts key on the workload name and need no change; there is no TrueHD input | partial | S | P2, P3, P4 | read only |
| P6 | Pre-merge comparison against the merge base, speed and heap, with the hard tier and the approval labels | `tools/ci/compare_performance.py`, `compare_memory.py`; jobs `performance-compare`, `performance-gate`, `Memory gate` in `.github/workflows/_compare.yml` | The scripts build `iclforge-bench`, `iclforge-kernelbench` and `iclforge-membench` and compare by name, so a TrueHD row is compared once the binaries carry it | partial | S | P2, P3, P4 | read only |
| P7 | The card, badge and hero tables that name each workload's codec, and refuse a name they do not list (`UnknownWorkload`) | `tools/ci/write_measurement_badges.py`, the script of `docs/performance-quality.md`, `docs/javascripts/hero-stats.js`; `tools/ci/test_write_measurement_badges.py` holds the three equal | Lists no TrueHD name. A TrueHD row in the history makes the badge step of `persist-performance-trend` fail | missing | S | P2 | `test_write_measurement_badges.py` lines 338 and 916 use `truehd_51_encode` as the example of an unknown name |
| P8 | The trend page's account of what is measured | `docs/performance-trend.md`, "What is measured, and on what" | No TrueHD text | missing | S | P2 | `grep -il truehd docs/performance-trend.md docs/performance-quality.md`: no file |
| P9 | Second ISA: the whole-frame bench on arm64 (Neoverse-N2) every merge | `ci.yml` job `performance-trend-arm64` runs `iclforge-bench` only | Runs TrueHD rows once P2 lands; no kernel or memory series on arm64 for DD+ either | partial | S | P2 | read only |
| P10 | The machine each record was measured on | `.github/scripts/capture-bench-environment.sh` writes `environment.json` beside the bench output | Generic | none | - | - | read only |
| P11 | Layering allowance for the bench targets | `tools/checks/projects.json`: project `tests` may use `ac3`, `ac4`, `dsp` | `truehd` is not allowed, so `tests/performance` cannot link it | missing | S | - | `python3 -c` on `projects.json`: `tests.may_use` is `[ac3, ac4, dsp]` |

### 2.2 Instruction counts, size and memory on the small parts

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| P12 | Instructions per frame on a Cortex-M3, deterministic: QEMU `-icount shift=0`, ceilings per fixture | `tools/checks/run_baremetal_probe.sh --icount` (decode `float` and `fixed`, `--encoder`, `--ac4`); tables in `docs/performance-trend.md` "Instructions per frame" | None | missing | L | Q1 | `grep -c truehd tools/checks/run_baremetal_probe.sh`: 0; QEMU and `arm-none-eabi` are absent here |
| P13 | Instructions per frame on the host, counted | None for DD+ (the host probe prints microseconds; the counted figure is the Cortex-M3's) | None | missing | S | - | `valgrind` 3.22 is installed here; no script uses it |
| P14 | Image size: `.text`, `.data`, `.bss`, table ROM, per-object attribution | `tools/checks/footprint_report.py`, `ICLFORGE_MAX_IMAGE_BYTES`, `build-footprint` job, `docs/performance-trend.md` "Static footprint" | None | missing | M | P17, Q1 | read only; the library has no minimum-footprint branch |
| P15 | Peak heap, retained bytes, allocations per frame, ceilings per fixture | `firmware/baremetal/probe.cpp`, `ac4_probe.cpp`; `testdata/ac4-probe-ceilings.json`, `tools/checks/check_probe_ceilings.py` | `iclforge-membench` gives the heap on the host (P4); no probe, no ceilings | partial | M | P17 | `iclforge-membench` is the host-side counter |
| P16 | Stack a decode uses, read by painting | `firmware/baremetal/ac4_probe.cpp` | None | missing | S | P17 | read only |
| P17 | The probe itself: fixtures built into an image, key=value output, three shapes (Cortex-M3, ESP-IDF, host) | `firmware/baremetal/{probe,encode_probe,ac4_probe}.cpp`, `main.cpp`, `platform/{baremetal,host,esp32s3,esp32c3,esp32c6,esp32p4}`, `ICLFORGE_MINIMAL_AC4` | None. `tools/checks/projects.json` does not let `baremetal` use `truehd` | missing | L | Q1 | `grep -rli truehd firmware`: no file |
| P18 | PCM pinned across legs: x86-64, Cortex-M3, RV32IMC, board all on one hash | `testdata/fixed-probe-pcm-hashes.json`, `ac4-probe-pcm-hashes.json`, `tools/checks/check_probe_hashes.py` | The fixtures' SHA-256 is pinned in `testdata/truehd/manifest.json` and checked on the host by `test_ffmpeg_fixtures.cpp`; no probe leg | partial | S | P17 | `grep pcm_sha256 testdata/truehd/manifest.json` |
| P19 | Stage timers and zone markers: where a frame's time goes on a part with no profiler | `ICLFORGE_ZONE_SCOPED_N` in `libs/ac3/src` (E-AC-3 encoder alone has 20 or more), `firmware/baremetal/stage_timers.cpp`, `libs/base/variants/profiling-stage_timers` | No marker in `libs/truehd` | missing | S | - | `grep -rn ICLFORGE_ZONE libs/truehd`: no match |
| P20 | Minimum-footprint profile: a decode-only or encode-only static archive, no exceptions, none of the host-only code | `libs/ac3/minimal.cmake`, `libs/ac4/minimal.cmake`, `cmake/MinimalDecoder.cmake`, options `ICLFORGE_MINIMAL_DECODER`, `_ENCODER`, `_AC4`, 12 presets in `CMakePresets.json` | `libs/truehd` builds as an ordinary library only; it links `base` for the bit reader, which the profile builds as headers | partial | M | Q1 | `grep -n MINIMAL libs/truehd/CMakeLists.txt`: no match |
| P21 | `-fno-exceptions` build of the codec | `libs/ac3`, `libs/ac4` (the probes assert it) | The 9 sources of `libs/truehd/src` compile with `-fno-exceptions -fno-rtti -Wall -Wextra -Wsign-conversion` and no diagnostic | none | - | - | `g++ -std=c++23 -O2 -fno-exceptions -fno-rtti -fsyntax-only` on each: exit 0, no output |

### 2.3 Arithmetic tiers

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| P22 | Decoder tiers: `double` reference, `float` for a single-precision FPU, `Fixed32` for parts with none, each gated | `ICLFORGE_DECODE_SCALAR`, `libs/ac3/variants/decode-scalar-*`, `libs/dsp/variants`, `check_decode_scalar_snr.py` (121 dB and up, fixed against double), `check_probe_hashes.py` | **n/a**: the decoder is exact integer arithmetic and has one tier; the gate is bit equality with the source | n/a | - | - | read only |
| P23 | Encoder tiers: `float` analysis front end, quality held to 0.5 dB of `double` | `ICLFORGE_ENCODE_SCALAR`, `libs/ac3/variants/encode-scalar-*`, `check_encode_scalar_quality.py` | The analysis is `double` and integer with no switch; a variant would change bytes and not correctness | partial | M | Q3 | `grep -n 'double' libs/truehd/src/encoder/coding.cpp`: `autocorrelation`, `levinson`, `quantise_fir`, `estimated_bits` |
| P24 | The scalar building blocks shared across codecs | `libs/base/internal/iclforge/base/arithmetic/{fixed32,mant_exp,scalar_math}.hpp` | Not used; a 64-bit multiply-accumulate on integers is a different primitive | n/a | - | - | read only |
| P25 | The lossless gate as the tier gate | n/a (DD+ is lossy; SNR floors stand in) | Every variant decodes to the source sample for sample (`libs/truehd/tests/decoder/test_ffmpeg_fixtures.cpp`, `tools/checks/verify_truehd_encode_interop.py`) | none | - | - | read only |
| P26 | An effort axis for the encoder: named levels with measured cost and quality | `delta_allocation` (`reduced`), planned `minimal`; `arithmetic-tiers.md` "The effort axis" | `EncoderConfig::max_fir_order`, `matrix_search`, `restart_interval_units` exist; no named levels, no measured cost or size | partial | M | Q4 | `grep -n max_fir_order libs/truehd/include/iclforge/truehd/encoder/encoder.hpp` |

### 2.4 SIMD and platform-specific kernels

| # | Capability | DD+ | TrueHD | Gap | Size | Blocked by | Evidence |
|---|---|---|---|---|---|---|---|
| P27 | The SIMD seam: one `simd.hpp` per architecture directory, chosen by CMake, no conditional in a kernel | `libs/base/variants/arch-{generic,x86_64,aarch64}` (`f64x2`, `f32x4`, a small `i32x4`), `ICLFORGE_SIMD` | Not used. The seam has no integer multiply or 64-bit lane, so an integer kernel in the seam's SSE2 and NEON tiers would need new operations | missing | M | Q2 | `grep -n 'struct\|operator' libs/base/variants/arch-x86_64/iclforge/base/detail/simd.hpp` |
| P28 | AVX2 tier selected at run time, kernel bodies in their own translation unit, plain signatures, a stub for builds without it | `libs/ac3/src/internal/avx2/`, `has_avx2()` in `libs/base`, option `ICLFORGE_AVX2`, `iclforge_ac3_simd_avx2` | None | missing | L | Q2 | `grep -rli avx2 libs/truehd`: no file |
| P29 | NEON | The `arch-aarch64` seam; exercised by the arm64 leg | Not written (needs the seam change of P27) | missing | M | Q2 | read only |
| P30 | WASM SIMD | None: Emscripten lands on `arch-generic`; `docs/platforms/wasm.md` says "no WASM SIMD" | None | none | - | - | `grep -n 'no WASM SIMD' docs/platforms/wasm.md` |
| P31 | AVX-512 | None: `arch-x86_64/simd.hpp` says wider than SSE2 needs run-time dispatch; the AVX2 tier is the only one | None | none | - | - | read only |
| P32 | Scalar-against-vector tests: bit-exact on real data and on random inputs, a mutation-proof | `libs/ac3/tests/core/test_simd_kernels.cpp`, `libs/ac3/tests/core/avx2/{present,absent}` | None | missing | M | - | `ls libs/truehd/tests`: no SIMD test |
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

## 4. Questions for the owner

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
