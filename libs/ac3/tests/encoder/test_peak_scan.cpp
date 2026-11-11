#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include "iclforge/ac3/core/exponents.hpp"
#include "iclforge/ac3/encoder/bandwidth.hpp"

// The peak scan (bandwidth.hpp, PeakScan) against the loop it replaced.
//
// accumulate_peak_exponents converts every coefficient of every block to an
// exponent and keeps the smallest per bin. The scan keeps the largest |c| per
// bin and converts once. These cases hold the second to the first, bin for
// bin, over frames of 6 blocks by 256 bins, for double and for float: random
// magnitudes spread over the whole exponent range, every ordered pair of edge
// values (each exponent's smallest and largest magnitude and the neighbours of
// each, the rounding thresholds, the clamp and what lies past it, denormals,
// both zeros), and NaN.

namespace {

constexpr std::size_t kScanBlocks = 6;
constexpr std::size_t kScanBins = 256;

// The reference is the loop as it was, written out here so the test does not
// depend on the production function it holds the replacement to. It starts at
// kMaxExponent, as the callers did.
template <typename T>
std::array<std::uint8_t, kScanBins> reference_exponents(const std::vector<T>& frame) {
    std::array<std::uint8_t, kScanBins> minimum{};
    minimum.fill(static_cast<std::uint8_t>(iclforge::ac3::kMaxExponent));
    for (std::size_t block = 0; block < kScanBlocks; ++block) {
        for (std::size_t bin = 0; bin < kScanBins; ++bin) {
            const auto exponent = static_cast<std::uint8_t>(iclforge::ac3::exponent_from_fixed(
                iclforge::ac3::to_fixed25(frame[(block * kScanBins) + bin])));
            minimum[bin] = std::min(minimum[bin], exponent);
        }
    }
    return minimum;
}

template <typename T>
std::array<std::uint8_t, kScanBins> scanned_exponents(const std::vector<T>& frame) {
    iclforge::ac3::encoder::PeakScan<T> scan;
    for (std::size_t block = 0; block < kScanBlocks; ++block) {
        iclforge::ac3::encoder::accumulate_peak_scan(
            scan, std::span<const T>{frame.data() + (block * kScanBins), kScanBins});
    }
    std::array<std::uint8_t, kScanBins> exponents{};
    iclforge::ac3::encoder::peak_scan_exponents(scan, exponents);
    return exponents;
}

// The shipped per-block function, run the way the encoders used to run it.
template <typename T>
std::array<std::uint8_t, kScanBins> shipped_exponents(const std::vector<T>& frame) {
    std::array<std::uint8_t, kScanBins> minimum{};
    minimum.fill(static_cast<std::uint8_t>(iclforge::ac3::kMaxExponent));
    for (std::size_t block = 0; block < kScanBlocks; ++block) {
        iclforge::ac3::encoder::accumulate_peak_exponents(
            std::span<const T>{frame.data() + (block * kScanBins), kScanBins}, minimum);
    }
    return minimum;
}

// Every value worth testing, in T. The fixed-point magnitudes M that
// to_fixed25 can produce are 0..2^24; exponent_from_fixed changes at each
// power of two, so the smallest and largest magnitude of every exponent are
// 2^k and 2^(k+1)-1. Rounding changes at (M - 0.5) / 2^24 exactly, so those
// and the representable neighbours either side are in too, with both signs.
template <typename T>
std::vector<T> edge_values() {
    std::vector<double> base;
    const auto add_fixed = [&](double fixed) {
        base.push_back(fixed / 16777216.0);
        base.push_back((fixed - 0.5) / 16777216.0);
        base.push_back((fixed + 0.5) / 16777216.0);
    };
    add_fixed(0.0);
    add_fixed(1.0);
    for (int k = 0; k <= 25; ++k) {
        const double power = std::ldexp(1.0, k);
        add_fixed(power - 1.0);
        add_fixed(power);
        add_fixed(power + 1.0);
    }
    // At and past the clamp, the extremes of each type, and denormals.
    for (const double v : {0.9999999, 1.0, 1.0000001, 1.5, 2.0, 1e3, 1e10, 1e30, 1e38, 3.5e38,
                           1e100, 1e300, std::numeric_limits<double>::max(),
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::denorm_min(),
                           std::numeric_limits<double>::min(), 1e-40, 1e-44, 1e-45, 1e-46,
                           1e-300, 0.0}) {
        base.push_back(v);
    }
    std::vector<T> out;
    const auto add = [&](T v) {
        out.push_back(v);
        out.push_back(-v);
    };
    for (const double v : base) {
        const auto narrowed = static_cast<T>(v);
        add(narrowed);
        add(std::nextafter(narrowed, std::numeric_limits<T>::infinity()));
        add(std::nextafter(narrowed, T{0}));
    }
    add(std::numeric_limits<T>::denorm_min());
    add(std::numeric_limits<T>::min());
    add(std::numeric_limits<T>::max());
    add(std::numeric_limits<T>::infinity());
    out.push_back(T{-0.0});
    return out;
}

template <typename T>
void check_scan_on_random_frames() {
    std::mt19937_64 rng{0x5eedb00c1234ULL};
    std::uniform_real_distribution<double> unit{0.5, 1.0};
    std::uniform_int_distribution<int> shift{0, 34};
    std::uniform_int_distribution<int> kind{0, 99};
    std::uniform_int_distribution<int> deep{35, 150};
    std::uniform_int_distribution<int> up{1, 40};
    std::size_t differing = 0;
    for (int frame_index = 0; frame_index < 300; ++frame_index) {
        std::vector<T> frame(kScanBlocks * kScanBins);
        // Every third frame is pushed down, so the block holding a bin's peak
        // varies from bin to bin.
        const int quiet_extra = frame_index % 3 == 0 ? 12 : 0;
        for (T& c : frame) {
            const int roll = kind(rng);
            double magnitude = 0.0;
            if (roll < 2) {
                magnitude = 0.0;
            } else if (roll < 5) {
                magnitude = std::ldexp(unit(rng), up(rng));  // at and past the clamp
            } else if (roll < 8) {
                magnitude = std::ldexp(unit(rng), -deep(rng));  // denormal in float
            } else {
                magnitude = std::ldexp(unit(rng), -(shift(rng) + quiet_extra));
            }
            c = static_cast<T>((rng() & 1U) != 0 ? -magnitude : magnitude);
        }
        const auto expected = reference_exponents(frame);
        if (expected != scanned_exponents(frame) || expected != shipped_exponents(frame)) {
            ++differing;
        }
    }
    CHECK(differing == 0);
}

template <typename T>
void check_scan_on_edge_value_pairs() {
    const std::vector<T> edges = edge_values<T>();
    REQUIRE(edges.size() > 500);

    // The two properties the scan rests on, directly: the exponent depends on
    // |c| alone, and never rises as |c| grows.
    const auto exponent_of = [](T c) {
        return iclforge::ac3::exponent_from_fixed(iclforge::ac3::to_fixed25(c));
    };
    std::size_t sign_differs = 0;
    std::size_t order_broken = 0;
    for (const T a : edges) {
        if (exponent_of(a) != exponent_of(-a)) {
            ++sign_differs;
        }
        for (const T b : edges) {
            if (std::abs(a) <= std::abs(b) && exponent_of(b) > exponent_of(a)) {
                ++order_broken;
            }
        }
    }
    CHECK(sign_differs == 0);
    CHECK(order_broken == 0);

    // Every ordered pair of edge values in two different blocks of the same
    // bin, the rest zero, 256 pairs a frame.
    std::size_t differing = 0;
    std::size_t pair_index = 0;
    std::vector<T> frame(kScanBlocks * kScanBins);
    for (std::size_t i = 0; i < edges.size(); ++i) {
        for (std::size_t j = 0; j < edges.size(); ++j) {
            const std::size_t bin = pair_index % kScanBins;
            const std::size_t block_a = (pair_index / kScanBins) % kScanBlocks;
            const std::size_t block_b = (block_a + 1 + (i % (kScanBlocks - 1))) % kScanBlocks;
            frame[(block_a * kScanBins) + bin] = edges[i];
            frame[(block_b * kScanBins) + bin] = edges[j];
            ++pair_index;
            if (pair_index % kScanBins == 0) {
                const auto expected = reference_exponents(frame);
                if (expected != scanned_exponents(frame) || expected != shipped_exponents(frame)) {
                    ++differing;
                }
                std::fill(frame.begin(), frame.end(), T{0});
            }
        }
    }
    if (pair_index % kScanBins != 0) {  // the last, partly filled frame
        const auto expected = reference_exponents(frame);
        if (expected != scanned_exponents(frame) || expected != shipped_exponents(frame)) {
            ++differing;
        }
    }
    CHECK(differing == 0);

    // Each edge value alone in a frame of zeros, and amid another value.
    for (const T value : edges) {
        for (const T filler : {T{0}, edges[7], edges[edges.size() / 2]}) {
            std::fill(frame.begin(), frame.end(), filler);
            for (std::size_t bin = 0; bin < kScanBins; ++bin) {
                frame[((bin % kScanBlocks) * kScanBins) + bin] = value;
            }
            REQUIRE(reference_exponents(frame) == scanned_exponents(frame));
        }
    }
}

template <typename T>
void check_scan_with_nan() {
    // A NaN has no exponent the language defines (to_fixed25 casts it to an
    // integer), so the scan must do what the loop did on this machine rather
    // than anything of its own: never let a NaN into the magnitude, and lower
    // the floor with the same two calls. The expected side makes those very
    // calls.
    const T nan = std::numeric_limits<T>::quiet_NaN();
    std::mt19937_64 rng{0xdeadbeefULL};
    std::uniform_real_distribution<double> unit{0.5, 1.0};
    std::uniform_int_distribution<int> shift{0, 30};
    std::uniform_int_distribution<std::size_t> pick{0, (kScanBlocks * kScanBins) - 1};
    for (int frame_index = 0; frame_index < 40; ++frame_index) {
        std::vector<T> frame(kScanBlocks * kScanBins);
        for (T& c : frame) {
            c = static_cast<T>(std::ldexp(unit(rng), -shift(rng)));
        }
        for (int n = 0; n < 1 + frame_index; ++n) {
            frame[pick(rng)] = nan;
        }
        frame[pick(rng)] = -nan;
        CHECK(reference_exponents(frame) == scanned_exponents(frame));
    }
    // A frame that is nothing but NaN.
    const std::vector<T> all_nan(kScanBlocks * kScanBins, nan);
    CHECK(reference_exponents(all_nan) == scanned_exponents(all_nan));
}

}  // namespace

TEST_CASE("The peak scan matches the per-coefficient exponent loop on random double frames") {
    check_scan_on_random_frames<double>();
}

TEST_CASE("The peak scan matches the per-coefficient exponent loop on random float frames") {
    check_scan_on_random_frames<float>();
}

TEST_CASE("The peak scan matches the per-coefficient exponent loop on every pair of double edge "
          "values") {
    check_scan_on_edge_value_pairs<double>();
}

TEST_CASE("The peak scan matches the per-coefficient exponent loop on every pair of float edge "
          "values") {
    check_scan_on_edge_value_pairs<float>();
}

TEST_CASE("The peak scan treats a double NaN as the per-coefficient loop did") {
    check_scan_with_nan<double>();
}

TEST_CASE("The peak scan treats a float NaN as the per-coefficient loop did") {
    check_scan_with_nan<float>();
}

TEST_CASE("The peak scan writes only the bins it is asked for and leaves the rest alone") {
    iclforge::ac3::encoder::PeakScan<float> scan;
    std::array<float, 40> block{};
    block.fill(0.25f);
    iclforge::ac3::encoder::accumulate_peak_scan(scan, block);
    std::array<std::uint8_t, 100> exponents{};
    exponents.fill(99);
    iclforge::ac3::encoder::peak_scan_exponents(scan, std::span<std::uint8_t>{exponents}.first(60));
    CHECK(exponents[0] == 1);
    CHECK(exponents[39] == 1);
    CHECK(exponents[40] == iclforge::ac3::kMaxExponent);
    CHECK(exponents[59] == iclforge::ac3::kMaxExponent);
    CHECK(exponents[60] == 99);
}
