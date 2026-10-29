// SHA-256 and HMAC-SHA-256 micro-benchmarks: iclforge::base::crypto's portable compression function
// against the CPU's SHA instructions (x86-64 SHA-NI, ARMv8 SHA2), and the whole keyed hash the EMDF
// Atmos signer computes per frame (libs/ac3/src/signing/emdf_atmos_signer.cpp).
//
// Not a Catch2 binary, same reasoning as iclforge-kernelbench: nothing here asserts anything. It
// prints a table and, with --json-out, writes one {name, iters, ns_per_call} record per case in
// kernel_bench.cpp's format, so tools/ci/append_kernel_history.py can take it as a trend signal.
// Correctness is libs/base/tests/test_crypto.cpp's job: the hardware kernel is held bit-for-bit to
// the portable one there, and this file only times them.
//
// Every iteration flips one byte of the message, so no compiler can prove two calls identical and
// hoist or fold one away, and each digest or state is folded into a sink that is printed.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/printf.h>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/hmac_sha256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/detail/sha256_impl.hpp"

namespace {

namespace crypto = iclforge::base::crypto;
namespace internal = iclforge::internal::crypto;

std::uint64_t g_sink = 0;

struct Result {
    std::string name;
    std::size_t bytes = 0;
    std::uint64_t iters = 0;
    double ns_per_call = 0.0;
};

// Best of five 100 ms rounds: the minimum is the least disturbed by the scheduler, and a case is
// measured in whole rounds rather than a hand-picked iteration count because the cases differ in
// cost by four orders of magnitude (one 64-byte block against a megabyte).
template <typename Fn>
Result time_case(std::string name, std::size_t bytes, Fn&& fn) {
    for (int i = 0; i < 3; ++i) {
        fn();
    }
    double best = 1.0e30;
    std::uint64_t iters_at_best = 0;
    for (int round = 0; round < 5; ++round) {
        std::uint64_t iters = 0;
        const auto start = std::chrono::steady_clock::now();
        double elapsed_ms = 0.0;
        do {
            fn();
            ++iters;
            elapsed_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
        } while (elapsed_ms < 100.0 || iters < 16);
        const double per_call = elapsed_ms * 1.0e6 / static_cast<double>(iters);
        if (per_call < best) {
            best = per_call;
            iters_at_best = iters;
        }
    }
    return {.name = std::move(name), .bytes = bytes, .iters = iters_at_best, .ns_per_call = best};
}

std::vector<std::uint8_t> message_of(std::size_t size) {
    std::vector<std::uint8_t> data(size);
    std::uint32_t x = 0x9e3779b9U;
    for (std::uint8_t& b : data) {
        x = x * 1664525U + 1013904223U;
        b = static_cast<std::uint8_t>(x >> 24);
    }
    return data;
}

void write_json(const std::vector<Result>& results, const std::string& path) {
    std::ofstream out(path);
    out << "{\n  \"kernels\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Result& r = results[i];
        out << "    {\"name\": \"" << r.name << "\", \"iters\": " << r.iters
            << ", \"ns_per_call\": " << r.ns_per_call << "}"
            << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string json_out;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        }
    }

    const bool hardware = internal::sha256_hardware_supported();
    fmt::printf("SHA-256 hardware kernel in this build and on this CPU: %s\n\n",
                hardware ? "yes" : "no (the 'hardware' rows run the portable code)");

    std::vector<Result> results;

    // The compression function alone, over whole blocks: what a long message costs per byte.
    for (const std::size_t size : {std::size_t{64}, std::size_t{256}, std::size_t{1024},
                                   std::size_t{2304}, std::size_t{65536}, std::size_t{1} << 20}) {
        std::vector<std::uint8_t> data = message_of(size);
        std::array<std::uint32_t, 8> state{};
        std::size_t cursor = 0;
        const std::string label = std::to_string(size) + "B";
        results.push_back(time_case("sha256_compress_portable_" + label, size, [&] {
            data[cursor++ % size] ^= 1;
            internal::sha256_compress_portable(state.data(), data.data(), size / 64);
            g_sink += state[0];
        }));
        results.push_back(time_case("sha256_compress_hardware_" + label, size, [&] {
            data[cursor++ % size] ^= 1;
            internal::sha256_compress_hardware(state.data(), data.data(), size / 64);
            g_sink += state[0];
        }));
    }

    // A whole hash with its padding, the way callers get it (dispatching to whichever is fastest).
    for (const std::size_t size : {std::size_t{64}, std::size_t{1024}, std::size_t{65536}}) {
        std::vector<std::uint8_t> data = message_of(size);
        std::size_t cursor = 0;
        results.push_back(time_case("sha256_oneshot_" + std::to_string(size) + "B", size, [&] {
            data[cursor++ % size] ^= 1;
            const auto digest = crypto::sha256(std::as_bytes(std::span(data)));
            g_sink += std::to_integer<std::uint8_t>(digest[0]);
        }));
    }

    // The signer's per-frame cost: HMAC(key, A||B) over a frame-sized message, per key tried.
    const std::vector<std::uint8_t> key = message_of(32);
    for (const std::size_t size : {std::size_t{800}, std::size_t{2304}, std::size_t{4608}}) {
        std::vector<std::uint8_t> data = message_of(size);
        std::size_t cursor = 0;
        results.push_back(time_case("hmac_sha256_" + std::to_string(size) + "B", size, [&] {
            data[cursor++ % size] ^= 1;
            const auto tag = crypto::hmac_sha256(std::as_bytes(std::span(key)),
                                                 std::as_bytes(std::span(data)));
            g_sink += std::to_integer<std::uint8_t>(tag[0]);
        }));
    }

    fmt::printf("%-36s %12s %12s\n", "case", "ns/call", "MB/s");
    for (const Result& r : results) {
        fmt::printf("%-36s %12.1f %12.1f\n", r.name.c_str(), r.ns_per_call,
                    static_cast<double>(r.bytes) / r.ns_per_call * 1.0e3);
    }
    // Not printed for its value - just to anchor g_sink as observably used.
    fmt::printf("(checksum %llu)\n", static_cast<unsigned long long>(g_sink));

    if (!json_out.empty()) {
        write_json(results, json_out);
        fmt::printf("wrote %s\n", json_out.c_str());
    }
    return 0;
}
