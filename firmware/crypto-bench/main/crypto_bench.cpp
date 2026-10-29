// The crypto bench: what SHA-256, HMAC-SHA-256 and the Sendspin transport AEAD cost on this board.
//
//   * "psa" rows go through the PSA Crypto API, which is how the firmware reaches mbedTLS and, below
//     it, ESP-IDF's drivers for the chip's SHA and AES peripherals (firmware.cpp, firmware_pull.cpp,
//     libs/sendspin/src/crypto/psa/crypto.cpp);
//   * "base" rows are iclforge::base::crypto's portable C++ - the software reference, compiled for
//     this CPU through the hw/none/ member of the SHA-256 seam.
//
// Cycles come from the CPU's own cycle counter (esp_cpu_get_cycle_count: Xtensa CCOUNT, RISC-V
// mcycle), minus the cost of reading it. Each case runs for about 150 ms and reports the FASTEST
// call, which is the least disturbed by the tick and the idle task, and the mean beside it. Every
// call flips one byte of its message, so nothing is repeated exactly. MB/s is bytes per cycle times
// the CPU clock in MHz.
//
// Output is lines of comma-separated fields, repeated every 20 s so a reader that attaches late still
// gets a whole pass:
//   INFO,<chip>,<revision>,<idf>,<cpu_mhz>,<internal_free>,<psram_free>,<crypto_clock>,<flash_dma>
//   CHECK,<what>,<bytes>,<ok>          the PSA result equals the portable one, bit for bit
//   BENCH,<chip>,<cpu_mhz>,<case>,<memory>,<bytes>,<min_cycles>,<mean_cycles>,<calls>,<mb_per_s>
//   FOOT,<chip>,<case>,<stack_bytes>,<heap_peak_bytes>   beyond an empty call, in a task of its own
//   LEFT,<chip>,<case>,<bytes>,<percent>   the CPU a lower-priority task on the same core keeps
//   DONE
//
// Three things a reference for the ESP32-P4 says to account for are measured here, not assumed:
//   * the crypto clock: INFO carries the selector's raw value, and on the P4 the "@xtal" rows run the
//     same call with it on the 40 MHz crystal instead of the PLL ESP-IDF selects at startup;
//   * blocks issued back to back: the "flash" rows hash a message in flash, which the SHA DMA cannot
//     read, so ESP-IDF writes it block by block through the peripheral's registers - the path that
//     issues the next block with no wait between - and CHECK compares the digest with the portable one;
//   * the CPU the call leaves free: LEFT runs a spinner at a lower priority on the same core while
//     the call repeats, and reports what share of its idle rate the spinner kept.
// tools/hearth/crypto_bench_report.py reads them.

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>

#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#include "crypto_clock.hpp"
#include "iclforge/base/crypto/hmac_sha256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "net_report.hpp"

namespace {

namespace base_crypto = iclforge::base::crypto;

constexpr std::uint32_t kCpuMhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
constexpr const char* kChip = CONFIG_IDF_TARGET;
constexpr std::size_t kMaxMessage = 65536;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kNonceBytes = 12;
constexpr std::uint32_t kBudgetUs = 150000;

// A message that lives in flash. The SHA peripheral's DMA cannot read flash, so ESP-IDF hashes this
// one a block at a time through the peripheral's registers (mbedtls/port/sha/core/sha.c).
constexpr std::size_t kFlashBytes = 4096;
constexpr std::array<std::uint8_t, kFlashBytes> make_flash_message() {
    std::array<std::uint8_t, kFlashBytes> m{};
    std::uint32_t x = 0x2545f491U;
    for (std::uint8_t& b : m) {
        x = x * 1664525U + 1013904223U;
        b = static_cast<std::uint8_t>(x >> 24);
    }
    return m;
}
constexpr std::array<std::uint8_t, kFlashBytes> kFlashMessage = make_flash_message();

std::uint32_t g_overhead = 0;
volatile std::uint32_t g_sink = 0;

struct Stats {
    std::uint32_t min_cycles = 0;
    double mean_cycles = 0.0;
    std::uint32_t calls = 0;
};

// Runs `fn` for kBudgetUs (and at least eight calls) and keeps the fastest and the mean.
template <typename Fn>
Stats measure(Fn&& fn) {
    for (int i = 0; i < 3; ++i) {
        fn();  // caches, the key lookup, the first-use paths
    }
    Stats s;
    std::uint64_t total = 0;
    std::uint32_t best = UINT32_MAX;
    const std::int64_t end = esp_timer_get_time() + kBudgetUs;
    do {
        const std::uint32_t t0 = esp_cpu_get_cycle_count();
        fn();
        const std::uint32_t cycles = esp_cpu_get_cycle_count() - t0;
        total += cycles;
        best = std::min(best, cycles);
        ++s.calls;
    } while (esp_timer_get_time() < end || s.calls < 8);
    s.min_cycles = best;
    s.mean_cycles = static_cast<double>(total) / s.calls;
    return s;
}

// Everything the bench prints is kept as well as printed: a board with no USB hands the text back over
// the network instead (net_report.hpp). A pass is about a tenth of this.
std::array<char, 48 * 1024> g_log{};
std::size_t g_log_size = 0;

__attribute__((format(printf, 1, 2))) void emit(const char* format, ...) {
    std::array<char, 256> line{};
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line.data(), line.size(), format, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    const std::size_t length = std::min(static_cast<std::size_t>(n), line.size() - 1);
    std::fwrite(line.data(), 1, length, stdout);
    if (g_log_size + length <= g_log.size()) {
        std::memcpy(g_log.data() + g_log_size, line.data(), length);
        g_log_size += length;
    }
}

void report(const char* name, const char* memory, std::size_t bytes, const Stats& s) {
    const std::uint32_t cycles = s.min_cycles > g_overhead ? s.min_cycles - g_overhead : 1;
    const double mean = s.mean_cycles > g_overhead ? s.mean_cycles - g_overhead : 1.0;
    const double mb_per_s = static_cast<double>(bytes) * kCpuMhz / cycles;
    emit("BENCH,%s,%lu,%s,%s,%lu,%lu,%.0f,%lu,%.2f\n", kChip, static_cast<unsigned long>(kCpuMhz), name,
         memory, static_cast<unsigned long>(bytes), static_cast<unsigned long>(cycles), mean,
         static_cast<unsigned long>(s.calls), mb_per_s);
}

void check(const char* what, std::size_t bytes, bool ok) {
    emit("CHECK,%s,%lu,%d\n", what, static_cast<unsigned long>(bytes), ok ? 1 : 0);
}

// --- the operations ---------------------------------------------------------

bool psa_sha256(const std::uint8_t* msg, std::size_t n, std::uint8_t* out) {
    std::size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, msg, n, out, 32, &length) == PSA_SUCCESS && length == 32;
}

bool psa_hmac(psa_key_id_t key, const std::uint8_t* msg, std::size_t n, std::uint8_t* out) {
    std::size_t length = 0;
    return psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, n, out, 32, &length) ==
               PSA_SUCCESS &&
           length == 32;
}

bool psa_seal(psa_key_id_t key, psa_algorithm_t alg, const std::uint8_t* nonce, const std::uint8_t* in,
              std::size_t n, std::uint8_t* out) {
    std::size_t length = 0;
    return psa_aead_encrypt(key, alg, nonce, kNonceBytes, nullptr, 0, in, n, out, n + kTagBytes,
                            &length) == PSA_SUCCESS &&
           length == n + kTagBytes;
}

psa_key_id_t import_key(const char* name, psa_key_type_t type, psa_key_usage_t usage,
                        psa_algorithm_t alg, const std::uint8_t* key, std::size_t bytes) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, type);
    psa_set_key_bits(&attributes, bytes * 8);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, alg);
    psa_key_id_t id = PSA_KEY_ID_NULL;
    const psa_status_t status = psa_import_key(&attributes, key, bytes, &id);
    if (status != PSA_SUCCESS) {
        emit("FAIL,import_key,%s,%ld\n", name, static_cast<long>(status));
        id = PSA_KEY_ID_NULL;
    }
    psa_reset_key_attributes(&attributes);
    return id;
}

// --- stack and heap beyond an empty call --------------------------------------

constexpr std::uint32_t kProbeStackBytes = 12288;

struct FootprintJob {
    std::function<void()> fn;
    TaskHandle_t parent = nullptr;
    std::uint32_t stack_used = 0;
    std::uint32_t heap_peak = 0;
};

void footprint_task(void* arg) {
    auto* job = static_cast<FootprintJob*>(arg);
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    const std::uint32_t before = static_cast<std::uint32_t>(info.total_free_bytes);
    heap_caps_monitor_local_minimum_free_size_start();
    job->fn();
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    heap_caps_monitor_local_minimum_free_size_stop();
    const auto lowest = static_cast<std::uint32_t>(info.minimum_free_bytes);
    job->heap_peak = before > lowest ? before - lowest : 0;
    job->stack_used = kProbeStackBytes - static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
                                             static_cast<std::uint32_t>(sizeof(StackType_t));
    xTaskNotifyGive(job->parent);
    vTaskDelete(nullptr);
}

FootprintJob footprint(std::function<void()> fn) {
    FootprintJob job;
    job.fn = std::move(fn);
    job.parent = xTaskGetCurrentTaskHandle();
    xTaskCreate(footprint_task, "footprint", kProbeStackBytes, &job, 5, nullptr);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return job;
}

void report_footprint(const char* name, const FootprintJob& job, const FootprintJob& empty) {
    const std::uint32_t stack = job.stack_used > empty.stack_used ? job.stack_used - empty.stack_used : 0;
    emit("FOOT,%s,%s,%lu,%lu\n", kChip, name, static_cast<unsigned long>(stack),
         static_cast<unsigned long>(job.heap_peak));
}

// --- the CPU a call leaves free ------------------------------------------------

// A task that only counts, run at a priority below the bench's own on the bench's core. While the
// bench task is on the CPU the counter does not move; while it waits - sleeping, or blocked on a
// peripheral's interrupt - the counter runs. So the counter's rate during a stretch of calls, as a
// share of its rate during an equally long sleep, is the share of the CPU the calls left to others.
struct Spinner {
    volatile std::uint32_t count = 0;
    volatile bool stop = false;
    volatile bool done = false;
};
Spinner g_spinner;

constexpr UBaseType_t kBenchPriority = 6;
constexpr UBaseType_t kSpinnerPriority = 5;
constexpr std::int64_t kWindowUs = 200000;

void spinner_task(void*) {
    while (!g_spinner.stop) {
        g_spinner.count = g_spinner.count + 1;
    }
    g_spinner.done = true;
    vTaskDelete(nullptr);
}

// Counts per millisecond of the spinner while `work` runs.
template <typename Work>
double spinner_rate(Work&& work) {
    g_spinner.count = 0;
    g_spinner.stop = false;
    g_spinner.done = false;
    const UBaseType_t original = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, kBenchPriority);
    xTaskCreatePinnedToCore(spinner_task, "spinner", 2048, nullptr, kSpinnerPriority, nullptr,
                            xPortGetCoreID());
    const std::int64_t begin = esp_timer_get_time();
    work();
    const std::int64_t end = esp_timer_get_time();
    const std::uint32_t counted = g_spinner.count;
    g_spinner.stop = true;
    while (!g_spinner.done) {
        vTaskDelay(1);
    }
    vTaskPrioritySet(nullptr, original);
    return static_cast<double>(counted) / (static_cast<double>(end - begin) / 1000.0);
}

// The share of the CPU, in percent, that calls of `op` repeated for kWindowUs leave to the spinner.
template <typename Op>
void report_cpu_left(const char* name, std::size_t bytes, Op&& op) {
    for (int i = 0; i < 3; ++i) {
        op();
    }
    const double idle = spinner_rate([] { vTaskDelay(pdMS_TO_TICKS(kWindowUs / 1000)); });
    const double loaded = spinner_rate([&] {
        const std::int64_t stop_at = esp_timer_get_time() + kWindowUs;
        do {
            op();
        } while (esp_timer_get_time() < stop_at);
    });
    emit("LEFT,%s,%s,%lu,%.1f\n", kChip, name, static_cast<unsigned long>(bytes),
         idle > 0.0 ? 100.0 * loaded / idle : 0.0);
}

// --- the bench ----------------------------------------------------------------

struct Buffers {
    std::uint8_t* internal = nullptr;  // messages in internal RAM
    std::uint8_t* external = nullptr;  // the same size in PSRAM, null on a chip without
    std::uint8_t* out = nullptr;       // digests and ciphertext
};

std::uint8_t* allocate(std::size_t bytes, std::uint32_t caps) {
    auto* p = static_cast<std::uint8_t*>(heap_caps_aligned_alloc(32, bytes, caps));
    if (p != nullptr) {
        std::uint32_t x = 0x9e3779b9U;
        for (std::size_t i = 0; i < bytes; ++i) {
            x = x * 1664525U + 1013904223U;
            p[i] = static_cast<std::uint8_t>(x >> 24);
        }
    }
    return p;
}

void run_pass(const Buffers& b, psa_key_id_t hmac_key, psa_key_id_t chacha_key, psa_key_id_t aes_key) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    emit("INFO,%s,%u,%s,%lu,%lu,%lu,%d,%d\n", kChip, static_cast<unsigned>(chip.revision),
         esp_get_idf_version(), static_cast<unsigned long>(kCpuMhz),
         static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
         bench::crypto_clock_selector(), esp_ptr_dma_capable(kFlashMessage.data()) ? 1 : 0);

    const std::array<std::uint8_t, 32> key = [] {
        std::array<std::uint8_t, 32> k{};
        for (std::size_t i = 0; i < k.size(); ++i) {
            k[i] = static_cast<std::uint8_t>(0x40 + i);
        }
        return k;
    }();
    const std::array<std::uint8_t, kNonceBytes> nonce{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    // The PSA result is held to the portable one first: a fast wrong answer is not a result.
    for (const std::size_t n : {std::size_t{64}, std::size_t{2304}, std::size_t{65536}}) {
        std::array<std::uint8_t, 32> psa{};
        const auto ref = base_crypto::sha256(std::as_bytes(std::span(b.internal, n)));
        check("sha256", n, psa_sha256(b.internal, n, psa.data()) &&
                               std::memcmp(psa.data(), ref.data(), 32) == 0);
    }
    for (const std::size_t n : {std::size_t{800}, std::size_t{2304}, std::size_t{4608}}) {
        std::array<std::uint8_t, 32> psa{};
        const auto ref = base_crypto::hmac_sha256(std::as_bytes(std::span(key)),
                                                  std::as_bytes(std::span(b.internal, n)));
        check("hmac_sha256", n, psa_hmac(hmac_key, b.internal, n, psa.data()) &&
                                     std::memcmp(psa.data(), ref.data(), 32) == 0);
    }
    // The AEAD calls succeed (a refused call is fast, and would otherwise read as a result).
    for (const std::size_t n : {std::size_t{256}, std::size_t{1500}, std::size_t{4096}}) {
        check("aead.chacha20poly1305", n,
              psa_seal(chacha_key, PSA_ALG_CHACHA20_POLY1305, nonce.data(), b.internal, n, b.out));
        check("aead.aes256gcm", n, psa_seal(aes_key, PSA_ALG_GCM, nonce.data(), b.internal, n, b.out));
    }
    // A message in PSRAM goes by DMA through the cache; one in flash cannot, and goes block by block
    // through the registers with no wait between blocks (the reference's "SHA_BUSY race" path).
    if (b.external != nullptr) {
        for (const std::size_t n : {std::size_t{2304}, std::size_t{65536}}) {
            std::array<std::uint8_t, 32> psa{};
            const auto ref = base_crypto::sha256(std::as_bytes(std::span(b.external, n)));
            check("sha256.psram", n, psa_sha256(b.external, n, psa.data()) &&
                                          std::memcmp(psa.data(), ref.data(), 32) == 0);
        }
    }
    for (const std::size_t n : {std::size_t{64}, std::size_t{192}, std::size_t{1024}, kFlashBytes}) {
        std::array<std::uint8_t, 32> psa{};
        const auto ref = base_crypto::sha256(std::as_bytes(std::span(kFlashMessage.data(), n)));
        check("sha256.flash", n, psa_sha256(kFlashMessage.data(), n, psa.data()) &&
                                      std::memcmp(psa.data(), ref.data(), 32) == 0);
    }
    // A race, if there is one, is intermittent: one digest per length proves little. Each path is
    // repeated and a single wrong digest fails the line (the P4X reference's back-to-back-blocks claim).
    constexpr int kStress = 3000;
    const auto stress = [&](const char* what, const std::uint8_t* data, std::size_t n) {
        std::array<std::uint8_t, 32> psa{};
        const auto ref = base_crypto::sha256(std::as_bytes(std::span(data, n)));
        bool ok = true;
        for (int i = 0; i < kStress && ok; ++i) {
            ok = psa_sha256(data, n, psa.data()) && std::memcmp(psa.data(), ref.data(), 32) == 0;
        }
        check(what, n, ok);
    };
    stress("sha256.flash.x3000", kFlashMessage.data(), 192);
    stress("sha256.flash.x3000", kFlashMessage.data(), kFlashBytes);
    stress("sha256.internal.x3000", b.internal, 2304);
    if (b.external != nullptr) {
        stress("sha256.psram.x3000", b.external, 2304);
    }

    // SHA-256: PSA against the portable reference, and PSA over a message held in PSRAM.
    for (const std::size_t n : {std::size_t{64}, std::size_t{256}, std::size_t{1024}, std::size_t{2304},
                                std::size_t{4096}, std::size_t{16384}, std::size_t{65536}}) {
        std::size_t cursor = 0;
        report("sha256.psa", "internal", n, measure([&] {
                   b.internal[cursor++ % n] ^= 1;
                   g_sink = g_sink + (psa_sha256(b.internal, n, b.out) ? b.out[0] : 0);
               }));
        report("sha256.base", "internal", n, measure([&] {
                   b.internal[cursor++ % n] ^= 1;
                   const auto digest = base_crypto::sha256(std::as_bytes(std::span(b.internal, n)));
                   g_sink = g_sink + std::to_integer<std::uint32_t>(digest[0]);
               }));
        if (b.external != nullptr) {
            report("sha256.psa", "psram", n, measure([&] {
                       b.external[cursor++ % n] ^= 1;
                       g_sink = g_sink + (psa_sha256(b.external, n, b.out) ? b.out[0] : 0);
                   }));
        }
    }

    // The same hash over a message in flash (the block-by-block path), and, where the chip lets the
    // crypto clock be chosen, over a message in RAM with the clock on the crystal instead of the PLL.
    for (const std::size_t n : {std::size_t{64}, std::size_t{1024}, kFlashBytes}) {
        report("sha256.psa", "flash", n, measure([&] {
                   g_sink = g_sink + (psa_sha256(kFlashMessage.data(), n, b.out) ? b.out[0] : 0);
               }));
    }
    if (bench::crypto_clock_selector() >= 0) {
        for (const std::size_t n : {std::size_t{64}, std::size_t{2304}, std::size_t{65536}}) {
            std::size_t cursor = 0;
            bench::crypto_clock_use_crystal(true);
            report("sha256.psa@xtal", "internal", n, measure([&] {
                       b.internal[cursor++ % n] ^= 1;
                       g_sink = g_sink + (psa_sha256(b.internal, n, b.out) ? b.out[0] : 0);
                   }));
            bench::crypto_clock_use_crystal(false);
        }
    }

    // HMAC-SHA-256 over a frame-sized message with a 32-byte key: the EMDF signer's per-frame cost.
    for (const std::size_t n : {std::size_t{800}, std::size_t{2304}, std::size_t{4608}}) {
        std::size_t cursor = 0;
        report("hmac_sha256.psa", "internal", n, measure([&] {
                   b.internal[cursor++ % n] ^= 1;
                   g_sink = g_sink + (psa_hmac(hmac_key, b.internal, n, b.out) ? b.out[0] : 0);
               }));
        report("hmac_sha256.base", "internal", n, measure([&] {
                   b.internal[cursor++ % n] ^= 1;
                   const auto tag = base_crypto::hmac_sha256(std::as_bytes(std::span(key)),
                                                             std::as_bytes(std::span(b.internal, n)));
                   g_sink = g_sink + std::to_integer<std::uint32_t>(tag[0]);
               }));
    }

    // The Sendspin transport AEAD, one message at a time as the channel seals them.
    for (const std::size_t n : {std::size_t{256}, std::size_t{1024}, std::size_t{1500}, std::size_t{4096}}) {
        std::size_t cursor = 0;
        report("aead.chacha20poly1305.psa", "internal", n, measure([&] {
                   b.internal[cursor++ % n] ^= 1;
                   g_sink = g_sink + (psa_seal(chacha_key, PSA_ALG_CHACHA20_POLY1305, nonce.data(),
                                               b.internal, n, b.out) ? b.out[0] : 0);
               }));
        report("aead.aes256gcm.psa", "internal", n, measure([&] {
                   b.internal[cursor++ % n] ^= 1;
                   g_sink = g_sink + (psa_seal(aes_key, PSA_ALG_GCM, nonce.data(), b.internal, n,
                                               b.out) ? b.out[0] : 0);
               }));
    }

    // The CPU each call leaves to a task of lower priority on the same core.
    for (const std::size_t n : {std::size_t{4096}, std::size_t{65536}}) {
        report_cpu_left("sha256.psa", n, [&] { psa_sha256(b.internal, n, b.out); });
        report_cpu_left("sha256.base", n, [&] {
            const auto d = base_crypto::sha256(std::as_bytes(std::span(b.internal, n)));
            g_sink = g_sink + std::to_integer<std::uint32_t>(d[0]);
        });
    }
    for (const std::size_t n : {std::size_t{1500}, std::size_t{4096}}) {
        report_cpu_left("aead.chacha20poly1305.psa", n, [&] {
            psa_seal(chacha_key, PSA_ALG_CHACHA20_POLY1305, nonce.data(), b.internal, n, b.out);
        });
        report_cpu_left("aead.aes256gcm.psa", n, [&] {
            psa_seal(aes_key, PSA_ALG_GCM, nonce.data(), b.internal, n, b.out);
        });
    }

    // Stack and transient heap beyond an empty call, each in a task of its own.
    const FootprintJob empty = footprint([] {});
    report_footprint("empty", empty, empty);
    report_footprint("sha256.psa", footprint([&] { psa_sha256(b.internal, 4096, b.out); }), empty);
    report_footprint("sha256.base", footprint([&] {
                         const auto d = base_crypto::sha256(std::as_bytes(std::span(b.internal, 4096)));
                         g_sink = g_sink + std::to_integer<std::uint32_t>(d[0]);
                     }), empty);
    report_footprint("hmac_sha256.psa", footprint([&] { psa_hmac(hmac_key, b.internal, 2304, b.out); }), empty);
    report_footprint("hmac_sha256.base", footprint([&] {
                         const auto t = base_crypto::hmac_sha256(std::as_bytes(std::span(key)),
                                                                 std::as_bytes(std::span(b.internal, 2304)));
                         g_sink = g_sink + std::to_integer<std::uint32_t>(t[0]);
                     }), empty);
    report_footprint("aead.chacha20poly1305.psa", footprint([&] {
                         psa_seal(chacha_key, PSA_ALG_CHACHA20_POLY1305, nonce.data(), b.internal, 1500, b.out);
                     }), empty);
    report_footprint("aead.aes256gcm.psa", footprint([&] {
                         psa_seal(aes_key, PSA_ALG_GCM, nonce.data(), b.internal, 1500, b.out);
                     }), empty);

    emit("DONE\n");
}

}  // namespace

extern "C" void app_main(void) {
    // On a build that reports over the network this arms the way back to the image the board came
    // from, whatever happens next; elsewhere it does nothing.
    bench::arm_return_to_previous_image();

    if (psa_crypto_init() != PSA_SUCCESS) {
        emit("FAIL,psa_crypto_init\n");
        bench::report_over_network(g_log.data(), g_log_size);
        return;
    }

    Buffers b;
    b.internal = allocate(kMaxMessage, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    b.external = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= kMaxMessage
                     ? allocate(kMaxMessage, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                     : nullptr;
    b.out = allocate(kMaxMessage + kTagBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (b.internal == nullptr || b.out == nullptr) {
        emit("FAIL,allocate\n");
        bench::report_over_network(g_log.data(), g_log_size);
        return;
    }

    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(0x40 + i);
    }
    const psa_key_id_t hmac_key =
        import_key("hmac", PSA_KEY_TYPE_HMAC, PSA_KEY_USAGE_SIGN_MESSAGE,
                   PSA_ALG_HMAC(PSA_ALG_SHA_256), key.data(), key.size());
    const psa_key_id_t chacha_key =
        import_key("chacha20", PSA_KEY_TYPE_CHACHA20, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT,
                   PSA_ALG_CHACHA20_POLY1305, key.data(), key.size());
    const psa_key_id_t aes_key =
        import_key("aes", PSA_KEY_TYPE_AES, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT,
                   PSA_ALG_GCM, key.data(), key.size());
    if (hmac_key == PSA_KEY_ID_NULL || chacha_key == PSA_KEY_ID_NULL || aes_key == PSA_KEY_ID_NULL) {
        bench::report_over_network(g_log.data(), g_log_size);  // import_key said which, and why
        return;
    }

    // What reading the cycle counter itself costs, taken off every case.
    g_overhead = UINT32_MAX;
    for (int i = 0; i < 2000; ++i) {
        const std::uint32_t t0 = esp_cpu_get_cycle_count();
        const std::uint32_t t1 = esp_cpu_get_cycle_count();
        g_overhead = std::min(g_overhead, t1 - t0);
    }

    for (;;) {
        g_log_size = 0;
        run_pass(b, hmac_key, chacha_key, aes_key);
        // Returns at once on a build with no network; on one with, hands the pass over and does not
        // return (the board goes back to its own image).
        bench::report_over_network(g_log.data(), g_log_size);
        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}
