// The TrueHD decoder on a chip (see ../CMakeLists.txt). For each fixture embedded in the image:
//
//   - one decode with the SHA-256 of the 32-bit little-endian samples (the 24-bit value shifted left
//     by 8, channels interleaved) taken and compared with testdata/truehd/manifest.json's pcm_sha256,
//     which is what FFmpeg's strict decode gives: a mismatch is a decode that is wrong on this chip;
//   - kPasses timed decodes after it, the first of which fills the caches: the least of their totals
//     is the figure, as firmware/baremetal's probe takes the least of three. A unit is timed in CPU
//     cycles, and the slowest and the mean of the least pass are reported;
//   - the heap the decoder held (internal RAM free before and after the first unit, which allocates
//     all of it) and the stack the decoding task reached.
//
// The output is key=value lines on the console, one fixture to a group, and "result=pass" or
// "result=fail" last.
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <span>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "sdkconfig.h"

namespace th = iclforge::truehd;

namespace {

struct Fixture {
    const char* name;
    const unsigned char* begin;
    const unsigned char* end;
    int sample_rate;
    int channels;
    int bits;
    int units;
    int frames;
    const char* pcm_sha256;
};

#include "fixtures.inc"

constexpr int kPasses = 4;  // the first fills the caches; the least of the rest is the figure
constexpr std::uint32_t kTaskStackBytes = CONFIG_TRUEHD_PROBE_TASK_STACK_BYTES;

// SHA-256 (FIPS 180-4), small and standing alone so that the probe needs no crypto library.
class Sha256 {
public:
    Sha256() {
        state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    }
    void update(const std::uint8_t* data, std::size_t size) {
        total_ += size;
        while (size > 0) {
            const std::size_t take = std::min<std::size_t>(size, 64 - used_);
            std::memcpy(block_.data() + used_, data, take);
            used_ += take;
            data += take;
            size -= take;
            if (used_ == 64) {
                compress();
                used_ = 0;
            }
        }
    }
    void final_hex(char (&out)[65]) {
        const std::uint64_t bits = total_ * 8;
        const std::uint8_t one = 0x80;
        update(&one, 1);
        const std::uint8_t zero = 0;
        while (used_ != 56) {
            update(&zero, 1);
        }
        std::array<std::uint8_t, 8> length{};
        for (int i = 0; i < 8; ++i) {
            length[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        }
        update(length.data(), 8);
        for (std::size_t i = 0; i < 8; ++i) {
            std::snprintf(out + i * 8, 9, "%08" PRIx32, state_[i]);
        }
    }

private:
    static constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress() {
        static constexpr std::array<std::uint32_t, 64> k{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{block_[i * 4]} << 24) | (std::uint32_t{block_[i * 4 + 1]} << 16) |
                   (std::uint32_t{block_[i * 4 + 2]} << 8) | std::uint32_t{block_[i * 4 + 3]};
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto h = state_;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(h[4], 6) ^ rotr(h[4], 11) ^ rotr(h[4], 25);
            const std::uint32_t ch = (h[4] & h[5]) ^ (~h[4] & h[6]);
            const std::uint32_t t1 = h[7] + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(h[0], 2) ^ rotr(h[0], 13) ^ rotr(h[0], 22);
            const std::uint32_t maj = (h[0] & h[1]) ^ (h[0] & h[2]) ^ (h[1] & h[2]);
            const std::uint32_t t2 = s0 + maj;
            h = {t1 + t2, h[0], h[1], h[2], h[3] + t1, h[4], h[5], h[6]};
        }
        for (std::size_t i = 0; i < 8; ++i) {
            state_[i] += h[i];
        }
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

struct Units {
    std::array<std::span<const std::byte>, 512> unit;
    int count = 0;
};

bool split(const Fixture& f, Units& units) {
    const auto* bytes = reinterpret_cast<const std::byte*>(f.begin);
    const std::size_t size = static_cast<std::size_t>(f.end - f.begin);
    std::size_t at = 0;
    while (at + 2 <= size && units.count < static_cast<int>(units.unit.size())) {
        const std::size_t length = th::access_unit_length_bytes(std::span<const std::byte>(bytes + at, size - at));
        if (length < 4 || at + length > size) {
            break;
        }
        units.unit[static_cast<std::size_t>(units.count++)] = std::span<const std::byte>(bytes + at, length);
        at += length;
    }
    return units.count == f.units;
}

std::size_t internal_free() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
std::size_t spiram_free() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

// A decoder started over. Decoder::reset() builds the new state before the old is released, so it needs the
// footprint twice (157 KB on the chips: more than a C6 has free); this releases the old first.
void restart(std::unique_ptr<th::Decoder>& decoder) {
    decoder.reset();
    decoder = std::make_unique<th::Decoder>(th::DecoderConfig{});
}

bool run_fixture(const Fixture& f) {
    std::printf("truehd_probe.%s.stage=split\n", f.name);
    Units units;
    if (!split(f, units)) {
        std::printf("truehd_probe.%s.error=split found %d units, the manifest says %d\n", f.name, units.count, f.units);
        return false;
    }
    std::printf("truehd_probe.%s.stage=construct units=%d\n", f.name, units.count);
    const std::size_t internal_before = internal_free();
    const std::size_t spiram_before = spiram_free();
    auto decoder = std::make_unique<th::Decoder>(th::DecoderConfig{});
    std::printf("truehd_probe.%s.constructed_internal_free=%u largest=%u\n", f.name,
                static_cast<unsigned>(internal_free()),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    if (CONFIG_TRUEHD_PROBE_BISECT != 0) {
        // Where the decoder stops, if it does: each step of the first unit, one line each.
        std::printf("truehd_probe.%s.bisect=major_sync\n", f.name);
        const auto sync = th::parse_major_sync(units.unit[0].subspan(4));
        std::printf("truehd_probe.%s.bisect=major_sync %s\n", f.name, sync ? "ok" : "error");
        std::printf("truehd_probe.%s.bisect=access_unit\n", f.name);
        const auto au = th::parse_access_unit(units.unit[0], 0);
        std::printf("truehd_probe.%s.bisect=access_unit %s substreams=%d\n", f.name, au ? "ok" : "error",
                    au ? au->num_substreams : -1);
        std::printf("truehd_probe.%s.bisect=decode_first\n", f.name);
        const auto first = decoder->decode(units.unit[0]);
        std::printf("truehd_probe.%s.bisect=decode_first %s\n", f.name, first ? "ok" : "error");
        restart(decoder);
    }
    std::printf("truehd_probe.%s.stage=decode\n", f.name);
    // The verifying decode.
    Sha256 sha;
    std::uint64_t frames = 0;
    for (int i = 0; i < units.count; ++i) {
        const auto decoded = decoder->decode(units.unit[static_cast<std::size_t>(i)]);
        if (!decoded) {
            std::printf("truehd_probe.%s.error=unit %d: %s\n", f.name, i, th::to_string(decoded.error().kind).data());
            return false;
        }
        if (!*decoded) {
            continue;
        }
        const th::DecodedAccessUnit& pcm = **decoded;
        frames += static_cast<std::uint64_t>(pcm.samples);
        for (const std::int32_t sample : pcm.pcm) {
            const std::uint32_t shifted = static_cast<std::uint32_t>(sample) << 8;
            const std::uint8_t le[4] = {static_cast<std::uint8_t>(shifted), static_cast<std::uint8_t>(shifted >> 8),
                                        static_cast<std::uint8_t>(shifted >> 16),
                                        static_cast<std::uint8_t>(shifted >> 24)};
            sha.update(le, 4);
        }
    }
    char hex[65] = {};
    sha.final_hex(hex);
    const bool match = std::strcmp(hex, f.pcm_sha256) == 0 && frames == static_cast<std::uint64_t>(f.frames);
    const std::size_t internal_after = internal_free();
    const std::size_t spiram_after = spiram_free();

    // The timed decodes.
    const std::uint32_t mhz = static_cast<std::uint32_t>(esp_rom_get_cpu_ticks_per_us());
    std::uint64_t best_total = UINT64_MAX;
    std::uint32_t best_max = 0;
    for (int pass = 0; pass < kPasses; ++pass) {
        restart(decoder);
        std::uint64_t total = 0;
        std::uint32_t slowest = 0;
        for (int i = 0; i < units.count; ++i) {
            const std::uint32_t t0 = esp_cpu_get_cycle_count();
            const auto decoded = decoder->decode(units.unit[static_cast<std::size_t>(i)]);
            const std::uint32_t cycles = esp_cpu_get_cycle_count() - t0;
            (void)decoded;
            total += cycles;
            slowest = std::max(slowest, cycles);
        }
        if (pass > 0 && total < best_total) {
            best_total = total;
            best_max = slowest;
        }
    }
    const double unit_samples = static_cast<double>(f.frames) / f.units;
    const double unit_us_audio = unit_samples * 1e6 / f.sample_rate;
    const double mean_cycles = static_cast<double>(best_total) / f.units;
    const double mean_us = mean_cycles / mhz;
    std::printf("truehd_probe.%s.format=%d ch %d Hz %d bit, %d units of %.0f samples\n", f.name, f.channels,
                f.sample_rate, f.bits, f.units, unit_samples);
    std::printf("truehd_probe.%s.pcm=%s\n", f.name, match ? "match" : "MISMATCH");
    if (!match) {
        std::printf("truehd_probe.%s.pcm_sha256=%s\ntruehd_probe.%s.frames=%" PRIu64 "\n", f.name, hex, f.name, frames);
    }
    std::printf("truehd_probe.%s.cycles_per_unit_mean=%.0f\n", f.name, mean_cycles);
    std::printf("truehd_probe.%s.us_per_unit_mean=%.2f\n", f.name, mean_us);
    std::printf("truehd_probe.%s.us_per_unit_max=%.2f\n", f.name, static_cast<double>(best_max) / mhz);
    std::printf("truehd_probe.%s.ms_per_second_of_audio=%.1f\n", f.name, mean_us / unit_us_audio * 1000.0);
    std::printf("truehd_probe.%s.realtime_factor=%.3f\n", f.name, mean_us / unit_us_audio);
    std::printf("truehd_probe.%s.heap_internal_decoder_bytes=%u\n", f.name,
                static_cast<unsigned>(internal_before - internal_after));
    std::printf("truehd_probe.%s.heap_spiram_decoder_bytes=%u\n", f.name,
                static_cast<unsigned>(spiram_before > spiram_after ? spiram_before - spiram_after : 0));
    return match;
}

#if CONFIG_TRUEHD_PROBE_TRACE
// The decoder's sources are compiled with -finstrument-functions: every function entered leaves its address
// and its frame address in a ring; dump_ring() prints the last ones, newest first, for addr2line on the .elf.
constexpr int kRing = 40;
volatile std::uintptr_t g_ring_fn[kRing];
volatile std::uintptr_t g_ring_sp[kRing];
volatile std::uint32_t g_entered = 0;
volatile bool g_frozen = false;  // set when an exception is allocated: the ring then holds the throw

void __attribute__((no_instrument_function)) dump_ring() {
    const std::uint32_t now = g_entered;
    std::printf("truehd_probe.trace.entered=%u\n", static_cast<unsigned>(now));
    for (int i = 0; i < kRing; ++i) {
        const std::uint32_t slot = (now + kRing - 1 - static_cast<std::uint32_t>(i)) % kRing;
        std::printf("truehd_probe.trace.back%d=fn 0x%08x sp 0x%08x\n", i, static_cast<unsigned>(g_ring_fn[slot]),
                    static_cast<unsigned>(g_ring_sp[slot]));
    }
}
#endif

void probe_task(void*) {
    // The console is a USB device that a host opens after the chip has reset: a few seconds' grace
    // keeps the first lines from being written to nobody. The probe then runs once and idles.
    vTaskDelay(pdMS_TO_TICKS(CONFIG_TRUEHD_PROBE_START_DELAY_MS));
    std::printf("truehd_probe.chip=%s\n", CONFIG_IDF_TARGET);
    std::printf("truehd_probe.cpu_mhz=%u\n", static_cast<unsigned>(esp_rom_get_cpu_ticks_per_us()));
    std::printf("truehd_probe.task_stack_bytes=%u\n", static_cast<unsigned>(kTaskStackBytes));
    std::printf("truehd_probe.heap_internal_free_at_start=%u\n", static_cast<unsigned>(internal_free()));
    std::printf("truehd_probe.heap_internal_largest_block_at_start=%u\n",
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    std::printf("truehd_probe.heap_spiram_free_at_start=%u\n", static_cast<unsigned>(spiram_free()));
    bool pass = true;
    for (const Fixture& fixture : kFixtures) {
#if CONFIG_TRUEHD_PROBE_TRACE && CONFIG_COMPILER_CXX_EXCEPTIONS
        try {
            pass = run_fixture(fixture) && pass;
        } catch (const std::exception& e) {
            std::printf("truehd_probe.%s.exception=%s\n", fixture.name, e.what());
            dump_ring();
            pass = false;
        }
#else
        pass = run_fixture(fixture) && pass;
#endif
    }
    const UBaseType_t free_words = uxTaskGetStackHighWaterMark(nullptr);
    std::printf("truehd_probe.stack_used_bytes=%u\n",
                static_cast<unsigned>(kTaskStackBytes - free_words * sizeof(StackType_t)));
    std::printf("truehd_probe.heap_internal_minimum_free=%u\n",
                static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    std::printf("result=%s\n", pass ? "pass" : "fail");
    vTaskDelete(nullptr);
}

}  // namespace

#if CONFIG_TRUEHD_PROBE_TRACE
// The decoder's sources are compiled with -finstrument-functions: every function entered leaves its address
// and the stack pointer in a ring, and a task on the other core prints the ring when nothing has been
// entered for a second (a loop, or a hang): addr2line on the .elf names the functions.
extern "C" void __attribute__((no_instrument_function, noinline, used)) __cyg_profile_func_enter(void* fn, void*) {
    if (g_frozen) {
        return;
    }
    const std::uint32_t slot = g_entered % kRing;
    g_ring_fn[slot] = reinterpret_cast<std::uintptr_t>(fn);
    g_ring_sp[slot] = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    g_entered = g_entered + 1;
}
extern "C" void __attribute__((no_instrument_function, noinline, used)) __cyg_profile_func_exit(void*, void*) {}
extern "C" void* __real___cxa_allocate_exception(std::size_t);
extern "C" void* __attribute__((no_instrument_function)) __wrap___cxa_allocate_exception(std::size_t size) {
    g_frozen = true;
    return __real___cxa_allocate_exception(size);
}

namespace {
void __attribute__((no_instrument_function)) trace_task(void*) {
    std::uint32_t last = 0xffffffffU;
    int still = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const std::uint32_t now = g_entered;
        still = now == last ? still + 1 : 0;
        last = now;
        if (still == 2) {
            dump_ring();
        }
    }
}
}  // namespace
#endif

extern "C" void app_main() {
#if CONFIG_TRUEHD_PROBE_TRACE
    xTaskCreatePinnedToCore(trace_task, "trace", 4096, nullptr, 6, nullptr, portNUM_PROCESSORS - 1);
    xTaskCreatePinnedToCore(probe_task, "truehd_probe", kTaskStackBytes, nullptr, 5, nullptr, 0);
#else
    xTaskCreate(probe_task, "truehd_probe", kTaskStackBytes, nullptr, 5, nullptr);
#endif
}
