#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <vector>

#include "iclforge/truehd/io/major_sync.hpp"
#include "probe_truehd.hpp"

// The data-rate gate of `forge qc`, `forge probe` and the Forge window's QC reads each unit's own
// input_timing (libs/truehd/ERRATA.md entry 99): a unit n is short when
//   (input_timing[n] - input_timing[n-1]) mod 65536  <  duration[n-1],
// duration being the samples the unit takes to arrive at the declared rate. These cases hold the law
// with units made for the purpose, whose sizes and timings are chosen to sit exactly on it, so a
// reading that carries a backlog forward (the queue this replaced), or that is off by a sample,
// fails here. The streams of Dolby's encoder that the queue failed are 2.5 MB at the least and are
// not in the tree.
//
// ICLFORGE_GUI_FIXTURE_DIR comes from apps/forge/gui/tests/CMakeLists.txt.

namespace {

namespace fs = std::filesystem;
namespace th = iclforge::truehd;
using iclforge::apps::truehd_probe::RateModel;

struct Setup {
    th::MajorSync sync;
    std::uint64_t declared_bps = 0;
    int nominal = 0;
    std::uint32_t rate = 0;
};

Setup setup_from_fixture() {
    std::ifstream in{fs::path{ICLFORGE_GUI_FIXTURE_DIR} / "truehd_st48_s24.thd", std::ios::binary};
    std::vector<std::byte> head(64);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    REQUIRE(in.gcount() == 64);
    const auto sync = th::parse_major_sync(std::span<const std::byte>(head).subspan(4), false);
    REQUIRE(sync.has_value());
    Setup setup{.sync = *sync};
    setup.declared_bps = sync->peak_data_rate_bps();
    setup.rate = sync->sample_rate();
    setup.nominal = th::nominal_samples_per_access_unit(sync->ratebits());
    REQUIRE(setup.declared_bps > 0);
    REQUIRE(setup.nominal == 40);
    return setup;
}

// The samples a unit of `bytes` takes to arrive at the declared rate, as the law has it.
std::int64_t duration_of(const Setup& setup, std::size_t bytes) {
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes) * 8U * setup.rate;
    return static_cast<std::int64_t>((bits + setup.declared_bps - 1U) / setup.declared_bps);
}

// A unit of `bytes` whose bytes 2 and 3 (after the 12-bit length) are `timing`.
std::vector<std::byte> unit_with(std::size_t bytes, std::uint16_t timing) {
    std::vector<std::byte> unit(bytes, std::byte{0});
    unit[2] = static_cast<std::byte>(timing >> 8U);
    unit[3] = static_cast<std::byte>(timing & 0xFFU);
    return unit;
}

// `count` units of the same size, each starting exactly when the one before has arrived (so the law
// holds with no margin), except that unit `short_at` starts `shortfall` samples early. The size is
// chosen so that a unit takes about 1.25 times the 40 samples of its slot at the declared rate: the
// stream runs over the rate for as long as it lasts, which is what a queue at that rate cannot
// carry and the input_timing words (which run ahead of the decode clock) can.
RateModel run(const Setup& setup, std::size_t count, std::uint16_t first_timing,
              std::optional<std::size_t> short_at = std::nullopt, std::int64_t shortfall = 0) {
    const std::size_t bytes = static_cast<std::size_t>(50 * setup.declared_bps / (8ULL * setup.rate));
    const std::int64_t duration = duration_of(setup, bytes);
    RateModel model;
    std::uint16_t timing = first_timing;
    for (std::size_t n = 0; n < count; ++n) {
        model.add(unit_with(bytes, timing), n == 0 ? std::optional<th::MajorSync>{setup.sync} : std::nullopt);
        std::int64_t delta = duration;
        if (short_at && *short_at == n) {
            delta -= shortfall;
        }
        timing = static_cast<std::uint16_t>(timing + delta);
    }
    return model;
}

}  // namespace

TEST_CASE("a stream whose units start exactly when the unit before has arrived is within the rate",
          "[gui][truehd][rate]") {
    const Setup setup = setup_from_fixture();
    // 2000 units of 1.25 slots each: a queue at the declared rate falls 10 samples behind per unit
    // and is 75 ms (3600 samples) late within 360 units; the law has no queue.
    const RateModel model = run(setup, 2000, 0);
    CHECK(model.report().violations == 0);
    CHECK(model.report().worst_overrun_ms == 0.0);
    CHECK(model.report().declared_bps == setup.declared_bps);
    CHECK(model.report().max_unit_rate_bps > setup.declared_bps);
}

TEST_CASE("one unit that starts a sample early is one violation, and the overrun is its size",
          "[gui][truehd][rate]") {
    const Setup setup = setup_from_fixture();
    const RateModel one = run(setup, 500, 0, 250, 1);
    CHECK(one.report().violations == 1);
    CHECK(one.report().worst_overrun_ms > 0.0);
    CHECK(one.report().worst_overrun_ms < 1.0);
    const RateModel ten = run(setup, 500, 0, 250, 10);
    CHECK(ten.report().violations == 1);
    CHECK(ten.report().worst_overrun_ms > one.report().worst_overrun_ms * 9.0);
}

TEST_CASE("input_timing wraps at 65536 without a violation", "[gui][truehd][rate]") {
    const Setup setup = setup_from_fixture();
    const RateModel model = run(setup, 2000, 65500);
    CHECK(model.report().violations == 0);
}
