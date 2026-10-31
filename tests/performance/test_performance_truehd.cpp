// The TrueHD guards of iclforge-perf (test_performance.hpp says why they are a file of their own).
// test_performance_truehd_absent.cpp takes this file's place when ICLFORGE_BUILD_TRUEHD is off.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "test_performance.hpp"
#include "truehd_bench.hpp"

namespace {

using perf::gate::fixture;
using perf::gate::kSlackFactor;

// TrueHD's frame is an access unit, 40 samples at 48 kHz, so its budget is 0.83 ms against A/52's
// 32. The whole 2.5 s fixture is run: 3 000 units.
void check_truehd_faster_than_real_time(const std::string& what, double elapsed_seconds,
                                        std::size_t units, std::uint32_t sample_rate,
                                        int samples_per_unit) {
    const double budget = static_cast<double>(units) *
                          perf::truehd_bench::budget_ms(sample_rate, samples_per_unit) / 1000.0;
    INFO(what << ": " << units << " access units in " << elapsed_seconds << "s ("
              << (1.0e6 * elapsed_seconds / static_cast<double>(units))
              << "us/unit); real-time budget is " << budget << "s");
    CHECK(elapsed_seconds < kSlackFactor * budget);
}

perf::truehd_bench::Source truehd_source(const perf::truehd_bench::Layout& layout) {
    return perf::truehd_bench::make_source(fixture(), layout.channels, 24, layout.sample_rate);
}

const perf::truehd_bench::Layout& layout_named(std::string_view name) {
    for (const auto& layout : perf::truehd_bench::kLayouts) {
        if (name == layout.name) {
            return layout;
        }
    }
    FAIL("no TrueHD layout named " << name);
    return perf::truehd_bench::kLayouts.front();
}

void check_truehd_encode(const std::string& what, const perf::truehd_bench::Layout& layout) {
    const auto source = truehd_source(layout);
    auto encoder = iclforge::truehd::Encoder::create(perf::truehd_bench::config_for(source));
    REQUIRE(encoder.has_value());
    const int samples = encoder->samples_per_access_unit();
    const auto units = source.frames() / static_cast<std::size_t>(samples);

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t unit = 0; unit < units; ++unit) {
        REQUIRE(encoder->encode(perf::truehd_bench::unit_pcm(source, samples, unit)).has_value());
    }
    REQUIRE(encoder->flush().has_value());
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    check_truehd_faster_than_real_time(what, elapsed.count(), units, source.sample_rate, samples);
}

void check_truehd_decode(const std::string& what, const perf::truehd_bench::Layout& layout) {
    const auto source = truehd_source(layout);
    const auto stream =
        perf::truehd_bench::encode_stream(source, perf::truehd_bench::config_for(source));
    REQUIRE_FALSE(stream.units.empty());
    iclforge::truehd::Decoder decoder;

    const auto start = std::chrono::steady_clock::now();
    for (const auto& unit : stream.units) {
        REQUIRE(decoder.decode(unit).has_value());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    check_truehd_faster_than_real_time(what, elapsed.count(), stream.units.size(),
                                       stream.sample_rate, stream.samples_per_unit);
}

}  // namespace

TEST_CASE("the TrueHD stereo encoder stays faster than real time") {
    check_truehd_encode("TrueHD stereo encode", layout_named("stereo"));
}

TEST_CASE("the TrueHD 5.1 encoder stays faster than real time") {
    check_truehd_encode("TrueHD 5.1 encode", layout_named("51"));
}

TEST_CASE("the TrueHD 7.1 encoder stays faster than real time") {
    check_truehd_encode("TrueHD 7.1 encode", layout_named("71"));
}

TEST_CASE("the TrueHD 96 kHz 5.1 encoder stays faster than real time") {
    check_truehd_encode("TrueHD 96 kHz 5.1 encode", layout_named("51_96k"));
}

TEST_CASE("the TrueHD 192 kHz stereo encoder stays faster than real time") {
    check_truehd_encode("TrueHD 192 kHz stereo encode", layout_named("stereo_192k"));
}

TEST_CASE("the TrueHD stereo decoder stays faster than real time") {
    check_truehd_decode("TrueHD stereo decode", layout_named("stereo"));
}

TEST_CASE("the TrueHD 5.1 decoder stays faster than real time") {
    check_truehd_decode("TrueHD 5.1 decode", layout_named("51"));
}

TEST_CASE("the TrueHD 7.1 decoder stays faster than real time") {
    check_truehd_decode("TrueHD 7.1 decode", layout_named("71"));
}

TEST_CASE("the TrueHD 96 kHz 5.1 decoder stays faster than real time") {
    check_truehd_decode("TrueHD 96 kHz 5.1 decode", layout_named("51_96k"));
}

TEST_CASE("the TrueHD 192 kHz stereo decoder stays faster than real time") {
    check_truehd_decode("TrueHD 192 kHz stereo decode", layout_named("stereo_192k"));
}
