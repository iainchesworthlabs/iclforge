#pragma once

// What the panel costs, as numbers a person (or a script) can read from the board on port 80: GET /panel-stats. The firmware's panel task
// fills a PanelStats about twice a second; this header is the shape and the JSON text of it, host-tested, so that the route, the developer view
// and the plan's measurement protocol (planning/p4x-meters-view.md, "Kit protocol") all read the same words.
//
// Nothing here is the Meters view's alone except the `meters` object: the panel's own memory and refresh time are what any view costs, and
// the first thing to compare a new view with is the view it replaces.

#include <cstdint>
#include <string>
#include <string_view>

#include "iclforge/meters_governor.hpp"
#include "iclforge/meters_tap.hpp"
#include "iclforge/meters_view.hpp"

namespace iclforge::panelstats {

// A duration that has been measured many times: the last one, the largest, and the mean since the count began.
struct Timing {
    std::uint32_t last_us = 0;
    std::uint32_t max_us = 0;
    std::uint64_t total_us = 0;
    std::uint64_t count = 0;

    void add(std::uint64_t us) noexcept {
        const auto v = static_cast<std::uint32_t>(us > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : us);
        last_us = v;
        max_us = v > max_us ? v : max_us;
        total_us += v;
        ++count;
    }
    [[nodiscard]] double mean_us() const noexcept { return count == 0 ? 0.0 : static_cast<double>(total_us) / static_cast<double>(count); }
};

// The Meters view since it was last opened.
struct MetersStats {
    bool showing = false;
    iclforge::meters::Representation representation = iclforge::meters::Representation::kPair;
    int outputs = 0;
    int width = 0;
    int height = 0;
    std::uint32_t opens = 0;       // times it has been opened since the board started
    std::uint64_t open_ms = 0;     // how long this opening has lasted
    int rate_hz = 20;              // the rate in force
    std::int64_t mean_cost_us = 0;  // the governor's window: what an update costs on average (analysis + widgets + render)
    iclforge::meters::GovernorStats governor;
    Timing analysis;  // the tap read, the transform, the averaging and the ballistics
    Timing widgets;   // setting the bars, ticks and labels that moved (LVGL invalidates; nothing is drawn yet)
    Timing render;    // LVGL's refresh while the view shows: from the start of a refresh to its end, the flush to the panel included
    iclforge::meters::TapStats tap;
};

// The panel as a whole.
struct PanelStats {
    int width = 0;
    int height = 0;
    std::uint64_t refreshes = 0;
    Timing refresh;  // every LVGL refresh, whichever view
    // Bytes. The "min" figures are the lowest the heap has been since the board started (the headroom), the others are now. LVGL draws in
    // the PSRAM on the firmware (lv_malloc), so the PSRAM figures are its heap.
    std::uint32_t task_stack_free_min = 0;
    std::uint32_t psram_free = 0;
    std::uint32_t psram_free_min = 0;
    std::uint32_t psram_largest = 0;
    std::uint32_t internal_free = 0;
    std::uint32_t internal_free_min = 0;
    std::uint32_t internal_largest = 0;
    bool has_meters = false;  // this build has the Meters view
    MetersStats meters;
};

namespace detail {

inline void key(std::string& out, std::string_view name) {
    out += '"';
    out += name;
    out += "\":";
}

inline void number(std::string& out, std::string_view name, std::uint64_t value) {
    key(out, name);
    out += std::to_string(value);
}

inline void signed_number(std::string& out, std::string_view name, std::int64_t value) {
    key(out, name);
    out += std::to_string(value);
}

// A non-negative mean with one decimal place, as the console prints one.
inline void fixed1(std::string& out, std::string_view name, double value) {
    key(out, name);
    const auto tenths = static_cast<std::uint64_t>(value <= 0.0 ? 0.0 : value * 10.0 + 0.5);
    out += std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

inline void timing(std::string& out, std::string_view name, const Timing& t) {
    key(out, name);
    out += '{';
    number(out, "last", t.last_us);
    out += ',';
    fixed1(out, "mean", t.mean_us());
    out += ',';
    number(out, "max", t.max_us);
    out += ',';
    number(out, "n", t.count);
    out += '}';
}

}  // namespace detail

[[nodiscard]] inline std::string_view representation_name(iclforge::meters::Representation r) {
    switch (r) {
        case iclforge::meters::Representation::kPair:
            return "pair";
        case iclforge::meters::Representation::kSum:
            return "sum";
        case iclforge::meters::Representation::kGrid:
            return "grid";
    }
    return "pair";
}

[[nodiscard]] inline std::string meters_json(const MetersStats& m) {
    using namespace detail;
    std::string out = "{";
    key(out, "showing");
    out += m.showing ? "true" : "false";
    out += ",\"representation\":\"";
    out += representation_name(m.representation);
    out += "\",";
    number(out, "outputs", static_cast<std::uint64_t>(m.outputs));
    out += ',';
    number(out, "width", static_cast<std::uint64_t>(m.width));
    out += ',';
    number(out, "height", static_cast<std::uint64_t>(m.height));
    out += ',';
    number(out, "opens", m.opens);
    out += ',';
    number(out, "open_ms", m.open_ms);
    out += ',';
    number(out, "rate_hz", static_cast<std::uint64_t>(m.rate_hz));
    out += ',';
    signed_number(out, "mean_cost_us", m.mean_cost_us);
    out += ',';
    number(out, "updates", m.governor.updates);
    out += ',';
    number(out, "late", m.governor.late);
    out += ',';
    number(out, "dropped", m.governor.dropped);
    out += ',';
    number(out, "falls", m.governor.falls);
    out += ',';
    number(out, "rises", m.governor.rises);
    out += ',';
    timing(out, "analysis_us", m.analysis);
    out += ',';
    timing(out, "widgets_us", m.widgets);
    out += ',';
    timing(out, "render_us", m.render);
    out += ",\"tap\":{";
    number(out, "blocks", m.tap.blocks);
    out += ',';
    fixed1(out, "us_per_burst", m.tap.us_per_burst());
    out += ',';
    number(out, "us_max_block", m.tap.us_max_block);
    out += ',';
    fixed1(out, "us_pass_per_burst", m.tap.part_per_burst(m.tap.us_pass));
    out += ',';
    fixed1(out, "us_pack_per_burst", m.tap.part_per_burst(m.tap.us_pack));
    out += ',';
    fixed1(out, "us_per_clock_read", m.tap.us_per_clock_read());
    out += ',';
    number(out, "reads", m.tap.reads);
    out += ',';
    number(out, "torn", m.tap.torn);
    out += ',';
    number(out, "short_reads", m.tap.short_reads);
    out += "}}";
    return out;
}

[[nodiscard]] inline std::string panel_stats_json(const PanelStats& p) {
    using namespace detail;
    std::string out = "{\"panel\":{";
    number(out, "width", static_cast<std::uint64_t>(p.width));
    out += ',';
    number(out, "height", static_cast<std::uint64_t>(p.height));
    out += ',';
    number(out, "refreshes", p.refreshes);
    out += ',';
    timing(out, "refresh_us", p.refresh);
    out += ',';
    number(out, "task_stack_free_min", p.task_stack_free_min);
    out += ',';
    number(out, "psram_free", p.psram_free);
    out += ',';
    number(out, "psram_free_min", p.psram_free_min);
    out += ',';
    number(out, "psram_largest", p.psram_largest);
    out += ',';
    number(out, "internal_free", p.internal_free);
    out += ',';
    number(out, "internal_free_min", p.internal_free_min);
    out += ',';
    number(out, "internal_largest", p.internal_largest);
    out += '}';
    if (p.has_meters) {
        out += ",\"meters\":";
        out += meters_json(p.meters);
    }
    out += "}\n";
    return out;
}

}  // namespace iclforge::panelstats
