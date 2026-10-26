#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/meters_governor.hpp"

namespace mt = iclforge::meters;

namespace {

// Runs `n` updates of the same cost and lateness, each one step after the last at the step the governor gave; returns the time after them.
std::int64_t run(mt::RateGovernor& g, std::int64_t t, int n, std::int64_t cost_us, std::int64_t late_us = 0, std::int64_t missed = 0) {
    for (int i = 0; i < n; ++i) {
        t += g.record(t, cost_us, late_us, missed);
    }
    return t;
}

}  // namespace

TEST_CASE("the governor starts at twenty a second and stays there while the updates are cheap and on time", "[meters][governor]") {
    mt::RateGovernor g;
    CHECK(g.rate_hz() == 20);
    CHECK(g.step_us() == mt::kFastStepUs);
    run(g, 0, 400, 12'000);
    CHECK_FALSE(g.slow());
    CHECK(g.stats().falls == 0);
    CHECK(g.stats().updates == 400);
    CHECK(g.mean_cost_us() == 12'000.0);
}

TEST_CASE("an average cost over 70 percent of a step makes it fall to ten a second, after a full window", "[meters][governor]") {
    mt::RateGovernor g;
    std::int64_t t = run(g, 0, 19, 40'000);
    CHECK_FALSE(g.slow());  // nineteen updates is not a window
    t = run(g, t, 1, 40'000);
    CHECK(g.slow());
    CHECK(g.step_us() == mt::kSlowStepUs);
    CHECK(g.rate_hz() == 10);
    CHECK(g.stats().falls == 1);
    // 34 ms is 68 percent: under the limit
    mt::RateGovernor h;
    run(h, 0, 200, 34'000);
    CHECK_FALSE(h.slow());
}

TEST_CASE("one slow update in a window of cheap ones is not a reason to fall", "[meters][governor]") {
    mt::RateGovernor g;
    std::int64_t t = run(g, 0, 19, 10'000);
    t += g.record(t, 120'000, 0, 0);  // the average is 15.5 ms
    t = run(g, t, 40, 10'000);
    CHECK_FALSE(g.slow());
}

TEST_CASE("late updates make it fall even when each is cheap; three of twenty do not", "[meters][governor]") {
    mt::RateGovernor g;
    std::int64_t t = 0;
    for (int i = 0; i < 20; ++i) {
        t += g.record(t, 8'000, i < 3 ? 30'000 : 0, 0);
    }
    CHECK_FALSE(g.slow());
    CHECK(g.stats().late == 3);
    mt::RateGovernor h;
    t = 0;
    for (int i = 0; i < 20; ++i) {
        t += h.record(t, 8'000, i < 4 ? 30'000 : 0, 0);
    }
    CHECK(h.slow());
    CHECK(h.stats().late == 4);
    // 15 ms exactly is not late
    mt::RateGovernor e;
    run(e, 0, 200, 8'000, 15'000);
    CHECK_FALSE(e.slow());
    CHECK(e.stats().late == 0);
}

TEST_CASE("a missed step counts against the window, and is counted as dropped", "[meters][governor]") {
    mt::RateGovernor g;
    std::int64_t t = 0;
    for (int i = 0; i < 20; ++i) {
        t += g.record(t, 8'000, 0, i % 5 == 0 ? 2 : 0);  // 4 updates came after two missed steps each
    }
    CHECK(g.slow());
    CHECK(g.stats().dropped == 8);
}

TEST_CASE("it rises again only after the dwell, with the load under 45 percent and nothing late", "[meters][governor]") {
    mt::RateGovernor g;
    std::int64_t t = run(g, 0, 20, 40'000);
    REQUIRE(g.slow());
    // 9 s at ten a second, cheap: not yet
    t = run(g, t, 90, 10'000);
    CHECK(g.slow());
    // past 10 s
    t = run(g, t, 12, 10'000);
    CHECK_FALSE(g.slow());
    CHECK(g.stats().rises == 1);
}

TEST_CASE("it does not rise while the load is 45 percent or more, or while an update is late", "[meters][governor]") {
    mt::RateGovernor g;
    std::int64_t t = run(g, 0, 20, 40'000);
    REQUIRE(g.slow());
    t = run(g, t, 300, 23'000);  // 46 percent of a fast step
    CHECK(g.slow());
    t = run(g, t, 100, 10'000);  // the window turns over: cheap now
    CHECK_FALSE(g.slow());
    mt::RateGovernor h;
    t = run(h, 0, 20, 40'000);
    REQUIRE(h.slow());
    for (int i = 0; i < 400; ++i) {
        t += h.record(t, 10'000, i % 15 == 0 ? 30'000 : 0, 0);  // a late update in every window
    }
    CHECK(h.slow());
}

TEST_CASE("a fall soon after a rise doubles the dwell, up to its limit; a minute at twenty a second resets it", "[meters][governor]") {
    mt::RateGovernor g;
    std::int64_t t = run(g, 0, 20, 40'000);
    REQUIRE(g.slow());
    CHECK(g.dwell_us() == 10'000'000);
    t = run(g, t, 130, 10'000);
    REQUIRE_FALSE(g.slow());
    // it falls again in the next second
    t = run(g, t, 25, 40'000);
    REQUIRE(g.slow());
    CHECK(g.dwell_us() == 20'000'000);
    // 15 s of cheap ten-a-second updates is not enough now, 22 s is
    t = run(g, t, 150, 10'000);
    CHECK(g.slow());
    t = run(g, t, 80, 10'000);
    REQUIRE_FALSE(g.slow());
    // and a third time
    t = run(g, t, 25, 40'000);
    REQUIRE(g.slow());
    CHECK(g.dwell_us() == 40'000'000);
    // after a rise, a minute of 12 ms updates settles it
    t = run(g, t, 500, 10'000);
    REQUIRE_FALSE(g.slow());
    t = run(g, t, 20 * 70, 12'000);
    CHECK_FALSE(g.slow());
    CHECK(g.dwell_us() == 10'000'000);
}

TEST_CASE("the dwell stops doubling at its limit", "[meters][governor]") {
    mt::GovernorLimits lim;
    lim.dwell_us = 1'000'000;
    lim.dwell_max_us = 3'000'000;
    mt::RateGovernor g(lim);
    std::int64_t t = 0;
    for (int round = 0; round < 6; ++round) {
        t = run(g, t, 25, 40'000);   // falls
        t = run(g, t, 100, 10'000);  // rises after the dwell
    }
    CHECK(g.dwell_us() == 3'000'000);
}

TEST_CASE("restart_window forgets the window and keeps the rate", "[meters][governor]") {
    mt::RateGovernor g;
    std::int64_t t = run(g, 0, 20, 40'000);
    REQUIRE(g.slow());
    g.restart_window();
    CHECK(g.slow());
    CHECK(g.mean_cost_us() == 0.0);
    run(g, t, 3, 40'000);
    CHECK(g.slow());
}
