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

namespace {

// `count` small units (duration well under a slot) a slot apart, the first with a restart header
// that puts the lead at `lead`; the lead then stays there, since each step is a slot.
RateModel run_led(const Setup& setup, std::size_t count, std::int64_t lead,
                  std::uint16_t first_timing = 1000, std::int64_t step = 40) {
    RateModel model;
    std::uint16_t timing = first_timing;
    for (std::size_t n = 0; n < count; ++n) {
        const std::optional<std::uint16_t> out =
            n == 0 ? std::optional<std::uint16_t>(static_cast<std::uint16_t>(timing + lead))
                   : std::nullopt;
        model.add_unit(8, timing, out, n == 0 ? std::optional<th::MajorSync>{setup.sync} : std::nullopt);
        timing = static_cast<std::uint16_t>(timing + step);
    }
    return model;
}

}  // namespace

TEST_CASE("a start less than a quarter of a slot after the last is out of the law", "[gui][truehd][rate]") {
    const Setup setup = setup_from_fixture();
    // 8-byte units take 1 sample to arrive; the floor is 40 / 4 = 10 samples between starts.
    CHECK(run_led(setup, 100, 40, 1000, 10).report().violations == 0);
    const RateModel nine = run_led(setup, 100, 40, 1000, 9);
    // Each step also moves the lead by a slot less the step, so the lead grows by 31 per unit: only
    // the step itself is under test, so count the violations of a stream that keeps its lead.
    CHECK(nine.report().violations > 0);
    RateModel exact;
    RateModel one_short;
    std::uint16_t timing = 0;
    for (std::size_t n = 0; n < 50; ++n) {
        exact.add_unit(8, timing, n == 0 ? std::optional<std::uint16_t>(40) : std::nullopt,
                       n == 0 ? std::optional<th::MajorSync>{setup.sync} : std::nullopt);
        timing = static_cast<std::uint16_t>(timing + (n % 2 == 0 ? 10 : 70));  // steps of 10 and 70
    }
    CHECK(exact.report().violations == 0);
    timing = 0;
    for (std::size_t n = 0; n < 50; ++n) {
        one_short.add_unit(8, timing, n == 0 ? std::optional<std::uint16_t>(40) : std::nullopt,
                           n == 0 ? std::optional<th::MajorSync>{setup.sync} : std::nullopt);
        timing = static_cast<std::uint16_t>(timing + (n % 2 == 0 ? 9 : 71));  // steps of 9 and 71
    }
    CHECK(one_short.report().violations == 25);
}

TEST_CASE("the lead is held between a slot and 75 ms", "[gui][truehd][rate]") {
    const Setup setup = setup_from_fixture();
    const std::int64_t ceiling = static_cast<std::int64_t>(setup.rate) * 75 / 1000;  // 3600 at 48 kHz
    CHECK(run_led(setup, 20, 40).report().violations == 0);
    CHECK(run_led(setup, 20, ceiling).report().violations == 0);
    CHECK(run_led(setup, 20, 39).report().violations == 20);
    CHECK(run_led(setup, 20, ceiling + 1).report().violations == 20);
    // A lead is the difference of two 16 bit words: input 65530, output 10 is 16 samples.
    CHECK(run_led(setup, 20, 16, 65530).report().violations == 20);
    CHECK(run_led(setup, 20, 100, 65530).report().violations == 0);
    // The overrun is the miss in milliseconds.
    CHECK(run_led(setup, 1, ceiling + 48).report().worst_overrun_ms > 0.99);
    CHECK(run_led(setup, 1, ceiling + 48).report().worst_overrun_ms < 1.01);
}
