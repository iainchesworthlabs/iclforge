#include <string>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/panel_stats.hpp"

namespace ps = iclforge::panelstats;
namespace mt = iclforge::meters;

TEST_CASE("a Timing keeps the last, the largest and the mean", "[panelstats]") {
    ps::Timing t;
    CHECK(t.mean_us() == 0.0);
    t.add(1000);
    t.add(3000);
    t.add(2000);
    CHECK(t.last_us == 2000);
    CHECK(t.max_us == 3000);
    CHECK(t.count == 3);
    CHECK(t.mean_us() == 2000.0);
    t.add(0x1'0000'0000ULL);  // saturates rather than wrapping
    CHECK(t.last_us == 0xFFFFFFFFU);
}

TEST_CASE("the panel's JSON names every figure, and has no meters object on a build without the view", "[panelstats]") {
    ps::PanelStats p;
    p.width = 1024;
    p.height = 600;
    p.refreshes = 12;
    p.refresh.add(4000);
    p.refresh.add(5000);
    p.task_stack_free_min = 9000;
    p.psram_free = 1000000;
    p.psram_free_min = 900000;
    p.psram_largest = 800000;
    p.internal_largest = 70000;
    const std::string json = ps::panel_stats_json(p);
    CHECK(json.starts_with("{\"panel\":{\"width\":1024,\"height\":600,\"refreshes\":12,"));
    CHECK(json.find("\"refresh_us\":{\"last\":5000,\"mean\":4500.0,\"max\":5000,\"n\":2}") != std::string::npos);
    CHECK(json.find("\"task_stack_free_min\":9000") != std::string::npos);
    CHECK(json.find("\"psram_free\":1000000,\"psram_free_min\":900000,\"psram_largest\":800000") != std::string::npos);
    CHECK(json.find("\"internal_largest\":70000}") != std::string::npos);
    CHECK(json.find("meters") == std::string::npos);
    CHECK(json.ends_with("}\n"));
}

TEST_CASE("the meters object carries the rate, the representation, the timings and the tap's cost", "[panelstats]") {
    ps::PanelStats p;
    p.has_meters = true;
    p.meters.showing = true;
    p.meters.representation = mt::Representation::kGrid;
    p.meters.outputs = 12;
    p.meters.width = 1024;
    p.meters.height = 600;
    p.meters.opens = 3;
    p.meters.open_ms = 61000;
    p.meters.rate_hz = 10;
    p.meters.mean_cost_us = 41234;
    p.meters.governor.updates = 700;
    p.meters.governor.late = 5;
    p.meters.governor.dropped = 2;
    p.meters.governor.falls = 1;
    p.meters.analysis.add(9000);
    p.meters.widgets.add(2500);
    p.meters.render.add(30000);
    p.meters.tap.blocks = 6;
    p.meters.tap.frames = 1536;  // one burst
    p.meters.tap.us_total = 20;
    p.meters.tap.us_max_block = 9;
    p.meters.tap.sampled_blocks = 4;  // four blocks timed in parts: 40 us in the pass, 10 in the pack, 2 us of clock reads
    p.meters.tap.us_pass = 40;
    p.meters.tap.us_pack = 10;
    p.meters.tap.us_clock = 2;
    const std::string json = ps::panel_stats_json(p);
    CHECK(json.find(",\"meters\":{\"showing\":true,\"representation\":\"grid\",\"outputs\":12,\"width\":1024,\"height\":600,\"opens\":3,\"open_ms\":61000,"
                    "\"rate_hz\":10,\"mean_cost_us\":41234,\"updates\":700,\"late\":5,\"dropped\":2,\"falls\":1,\"rises\":0,") != std::string::npos);
    CHECK(json.find("\"analysis_us\":{\"last\":9000,\"mean\":9000.0,\"max\":9000,\"n\":1}") != std::string::npos);
    CHECK(json.find("\"widgets_us\":{\"last\":2500,") != std::string::npos);
    CHECK(json.find("\"render_us\":{\"last\":30000,") != std::string::npos);
    CHECK(json.find("\"tap\":{\"blocks\":6,\"us_per_burst\":20.0,\"us_max_block\":9,\"us_pass_per_burst\":60.0,\"us_pack_per_burst\":15.0,"
                    "\"us_per_clock_read\":0.5,\"reads\":0,\"torn\":0,\"short_reads\":0}}") != std::string::npos);
}

TEST_CASE("the representation names are the screen's", "[panelstats]") {
    CHECK(ps::representation_name(mt::Representation::kPair) == "pair");
    CHECK(ps::representation_name(mt::Representation::kSum) == "sum");
    CHECK(ps::representation_name(mt::Representation::kGrid) == "grid");
}
