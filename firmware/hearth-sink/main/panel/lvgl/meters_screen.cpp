// The Meters view. See meters_screen.hpp, planning/p4x-meters-view.md and the prototype in preview/meters_options/ that drew it first.
//
// Every bar is a rectangle that is resized when its pixel height moved and left alone otherwise, so that a step redraws the bars that changed and
// the ticks that moved (the speakers view's rule: LVGL redraws the rectangles that are invalid, and with the invalid-area table raised to 256 in the
// panel's build, which a spectrum of sixty changes a step needs, nothing else). A spectrum bar is three rectangles in a column, green, amber and
// red, so that the zone colours are the level's; a meter's bar is the same. The labels that say a number have a buffer of their own and are
// set only when the number changed.
//
// The rules (which representation, which output is in which master, what a bar covers, the ranges and the ballistics) are libs/device's
// meters_view.hpp, and the analysis libs/dsp's spectrum.hpp, both held to their numbers on the host.

#include "meters_screen.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <span>
#include <string>
#include <string_view>

#include "iclforge/dsp/spectrum.hpp"
#include "iclforge/meters_draw.hpp"
#include "iclforge/meters_governor.hpp"
#include "iclforge/meters_tap.hpp"
#include "iclforge/meters_view.hpp"
#include "iclforge/panel_stats.hpp"

#include "src/misc/cache/instance/lv_image_cache.h"

#include "fonts.hpp"
#include "lv_alloc.hpp"

namespace player::panel {

namespace {

namespace mv = iclforge::meters;
namespace dsp = iclforge::dsp::spectrum;
namespace ps = iclforge::panelstats;
namespace dr = iclforge::meters::draw;

constexpr std::uint32_t kGreen = 0x4FC28A;
constexpr std::uint32_t kAmber = 0xF0B24A;
constexpr std::uint32_t kRed = 0xFF5C40;
constexpr std::uint32_t kTrough = 0x201D1C;
constexpr std::uint32_t kTickColour = 0xECE7E5;
// The stage's own colour, for the lines of the scale that cross the bars.
constexpr std::uint32_t kStageBg = 0x151313;
// A frame that comes this much before an update is due is that update's: the loop's own beat jitters by a tick or two.
constexpr std::int64_t kEarlyUs = 8'000;
// The two kinds of chart's colours: a spectrum has no troughs and a line of the scale across its bars; a meter has its trough.
constexpr dr::Colours kSpectrumColours = dr::make_colours(kStageBg, kStageBg, kGreen, kAmber, kRed, kTickColour, kStageBg, 120);
constexpr dr::Colours kMeterColours = dr::make_colours(kStageBg, kTrough, kGreen, kAmber, kRed, kTickColour, kStageBg, 120);
// The numbers (each meter's peak) are written four times a second: a label's text changing is an area of its own for LVGL to draw and copy, and
// nobody reads twenty numbers a second.
constexpr std::int64_t kValueEveryUs = 250'000;

// A microsecond clock for timing the view's own work: the same one the tap's blocks are timed with on the firmware (esp_timer), a steady clock
// on the preview's host. Only differences of it are used.
std::uint64_t micros() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

float unit(float db, float floor_db, float top_db) { return std::clamp((db - floor_db) / (top_db - floor_db), 0.0F, 1.0F); }

int pixels(float db, int total) { return static_cast<int>(std::lround(unit(db, mv::kFloorDb, mv::kTopDb) * static_cast<float>(total))); }

// What a stage of this width allows (the three size classes of meters_view.hpp).
struct Dim {
    int w = 0;
    int h = 0;
    int m = 0;        // the margin
    int f_small = 0;  // the scale and the band labels
    int f_label = 0;  // a name, a value
    int scale_w = 0;  // the dB scale's column
    bool wide = false;
};

Dim dim_of(int w, int h) {
    Dim d;
    d.w = w;
    d.h = h;
    d.wide = w >= mv::kLargeWidth;
    d.m = w >= mv::kLargeWidth ? 20 : w >= mv::kMediumWidth ? 10 : 6;
    d.f_small = w >= mv::kLargeWidth ? 16 : w >= mv::kMediumWidth ? 12 : 11;
    d.f_label = w >= mv::kLargeWidth ? 20 : w >= mv::kMediumWidth ? 14 : 12;
    d.scale_w = w >= mv::kLargeWidth ? 44 : w >= mv::kMediumWidth ? 28 : 22;
    return d;
}

// The faces: Archivo Regular at the sizes the view draws in, made once and kept (a tiny_ttf face has a cache of glyphs).
const lv_font_t* face(int px) {
    struct Entry {
        int px = 0;
        lv_font_t* font = nullptr;
    };
    static std::array<Entry, 6> faces{};
    for (const Entry& e : faces) {
        if (e.font != nullptr && e.px == px) {
            return e.font;
        }
    }
    const FontFile file = archivo_regular();
    lv_font_t* const made = lv_tiny_ttf_create_data(file.data, file.size, px);
    for (Entry& e : faces) {
        if (e.font == nullptr) {
            e = {px, made};
            break;
        }
    }
    return made;
}

lv_obj_t* plain(lv_obj_t* parent) {
    lv_obj_t* const object = lv_obj_create(parent);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_outline_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    return object;
}

lv_obj_t* rect(lv_obj_t* parent, int x, int y, int w, int h, std::uint32_t rgb, int opa = 255) {
    lv_obj_t* const object = plain(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, std::max(w, 1), std::max(h, 1));
    lv_obj_set_style_bg_color(object, color(rgb), 0);
    lv_obj_set_style_bg_opa(object, static_cast<lv_opa_t>(std::clamp(opa, 0, 255)), 0);
    return object;
}

enum class Align { kLeft, kCentre, kRight };

lv_obj_t* label(lv_obj_t* parent, const char* text, int px, std::uint32_t rgb, int x, int y, int w, Align align = Align::kLeft) {
    lv_obj_t* const l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, face(px), 0);
    lv_obj_set_style_text_color(l, color(rgb), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_align(l, align == Align::kLeft ? LV_TEXT_ALIGN_LEFT : align == Align::kCentre ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_RIGHT, 0);
    return l;
}

// A number in a label of its own: the text is kept here and the label is told only when it changed.
struct Value {
    lv_obj_t* object = nullptr;
    std::array<char, 16> text{};

    void set(float db, int decimals) {
        std::array<char, 16> next{};
        if (db <= -99.0F) {
            std::snprintf(next.data(), next.size(), "-inf");
        } else {
            std::snprintf(next.data(), next.size(), decimals == 0 ? "%.0f" : "%.1f", static_cast<double>(db));
        }
        if (object != nullptr && next != text) {
            text = next;
            lv_label_set_text_static(object, text.data());
        }
    }
};

// A chart: a buffer in LVGL's heap that LVGL shows as an image and the view writes by the rows that changed (iclforge/meters_draw.hpp), so that an
// update is one area for LVGL to draw and copy, not one for each part of each bar. The first drawing, an object for each of them, took 130 to 440 ms an
// update on the kit (planning/p4x-meters-view.md, "Kit results").
struct Chart {
    lv_obj_t* object = nullptr;
    lv_draw_buf_t* buffer = nullptr;
    std::uint16_t* px = nullptr;
    std::size_t stride = 0;  // pixels a row
    int w = 0;
    int h = 0;
};

bool make_chart(Chart& c, lv_obj_t* parent, int x, int y, int w, int h) {
    c = Chart{};
    c.buffer = lv_draw_buf_create(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), LV_COLOR_FORMAT_RGB565, 0);
    if (c.buffer == nullptr) {
        return false;
    }
    c.object = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(c.object, c.buffer);
    lv_obj_remove_flag(c.object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(c.object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(c.object, x, y);
    lv_obj_update_layout(c.object);  // so that its coordinates on the display are known when the first rows are shown
    c.px = static_cast<std::uint16_t*>(static_cast<void*>(c.buffer->data));
    c.stride = c.buffer->header.stride / sizeof(std::uint16_t);
    c.w = w;
    c.h = h;
    return true;
}

// Tells LVGL that rows [first, last) of the chart changed: one area (in the display's coordinates, which is what lv_obj_invalidate_area() takes).
void show_rows(const Chart& c, dr::Rows rows) {
    if (c.object == nullptr || rows.empty()) {
        return;
    }
    lv_area_t at;
    lv_obj_get_coords(c.object, &at);
    const lv_area_t area{at.x1, at.y1 + rows.first, at.x1 + c.w - 1, at.y1 + rows.last - 1};
    lv_obj_invalidate_area(c.object, &area);
}

// The same for a cell of the chart.
void show_cell(const Chart& c, const dr::Cell& cell) {
    if (c.object == nullptr || cell.empty()) {
        return;
    }
    lv_area_t at;
    lv_obj_get_coords(c.object, &at);
    const lv_area_t area{at.x1 + cell.x0, at.y1 + cell.y0, at.x1 + cell.x1 - 1, at.y1 + cell.y1 - 1};
    lv_obj_invalidate_area(c.object, &area);
}

// Draws what a row of bars changed and tells LVGL: a rectangle for each run of neighbouring bars that is cheaper together than apart
// (meters_draw.hpp, plan_cells), not one of the plot's whole width for the rows of the bar that moved most. The kit measured the render at
// about 135 ns a pixel and 1 ms an area: 21 ms for 124,000 pixels in the 5.1 room, 47 ms with a JOC stream decoding.
void draw_cells(const Chart& chart, const dr::VChart& geometry, std::span<const dr::VBar> bars, std::span<const std::uint8_t> lines, const dr::Colours& colours,
                bool troughs, std::span<const dr::Change> changes) {
    std::array<dr::Cell, dr::kMaxChanges> cells{};
    const std::size_t n = dr::plan_cells(changes, cells);
    for (std::size_t i = 0; i < n; ++i) {
        dr::render_vbars(chart.px, chart.stride, geometry, bars, lines, colours, troughs, cells[i]);
        show_cell(chart, cells[i]);
    }
}

// Gives a chart's buffer back. The object is LVGL's: the stage's clearing has deleted it, and one that is still there goes now.
void drop_chart(Chart& c) {
    if (c.object != nullptr && lv_obj_is_valid(c.object)) {
        lv_obj_delete(c.object);
    }
    if (c.buffer != nullptr) {
        lv_image_cache_drop(c.buffer);
        lv_draw_buf_destroy(c.buffer);
    }
    c = Chart{};
}

// A level meter's bar: its ballistics and the number it shows.
struct Bar {
    dsp::Ballistic bal;
    Value* value = nullptr;
    int decimals = 1;
};

// Moves a vertical meter's bar to a level and notes the columns and rows of its chart that changed.
void move_vbar(Bar& bar, dr::VBar& drawn, float db, float seconds, int total, dr::Change& change) {
    bar.bal.update(db, seconds, mv::kMeterFallDbPerS, mv::kMeterHoldS, mv::kMeterPeakFallDbPerS);
    const int top = total - pixels(bar.bal.level, total);
    const int peak = pixels(bar.bal.peak, total);
    const int tick = peak > 1 ? total - peak : -1;
    change = {drawn.x, drawn.x + drawn.width, dr::rows_touched(drawn.top, drawn.tick, top, tick, total)};
    drawn.top = top;
    drawn.tick = tick;
}

// The same for a horizontal one (the pair's): whether it moved.
bool move_hbar(Bar& bar, dr::HBar& drawn, float db, float seconds, int total) {
    bar.bal.update(db, seconds, mv::kMeterFallDbPerS, mv::kMeterHoldS, mv::kMeterPeakFallDbPerS);
    const int lit = pixels(bar.bal.level, total);
    const int peak = pixels(bar.bal.peak, total);
    const int tick = peak > 1 ? peak - 2 : -1;
    const bool moved = lit != drawn.lit || tick != drawn.tick;
    drawn.lit = lit;
    drawn.tick = tick;
    return moved;
}

// What the analysis needs and the view keeps while it shows: 63 KB of tables and scratch (the transform's work buffers among them: the panel's stack has no room for them) and the 16 KB the ring is read into, in LVGL's heap
// (PSRAM on the firmware), given back when the stage is cleared.
struct Heavy {
    dsp::Analyzer analyzer;
    std::array<std::int16_t, dsp::kHistory> history{};
};

struct State {
    bool built = false;
    Dim d;
    mv::Representation rep = mv::Representation::kPair;
    mv::Fold fold;
    int per_bar = 1;
    bool downmix = false;
    // The spectrum's plot, as a chart: each bar's ballistics, what it last drew and the lines of the scale across it.
    int sx = 0;
    int sy = 0;
    int sw = 0;
    int sh = 0;
    Chart spectrum;
    dr::VChart spectrum_geometry;
    LvVector<dsp::Ballistic> bal;
    LvVector<dr::VBar> bars;
    LvVector<std::uint8_t> lines;
    // The channels, as a chart: two horizontal meters for the pair, or a bar for each master or output.
    Chart channels;
    dr::VChart channels_geometry;
    dr::HChart pair_geometry;
    LvVector<Bar> meters;
    LvVector<dr::VBar> meter_bars;
    LvVector<dr::HBar> pair_bars;
    LvVector<Value> values;
    std::int64_t values_due_us = 0;  // when the numbers are next written (0: now)
    LvVector<std::uint8_t> outputs;  // a grid's: the output each bar is
    Value peak_text;
    float peak_held = mv::kSilentDb;
    // The analysis.
    Heavy* heavy = nullptr;
    dsp::PowerAverage average;
    std::array<float, dsp::kBands> bands{};
    std::array<float, dsp::kBands> averaged{};
    bool have_spectrum = false;
    std::uint64_t last_us = 0;
    std::int64_t due_us = 0;  // when the next update is due (0: the next frame's)
    std::uint32_t analyses = 0;  // updates that read the ring: every other one runs the low half of the analysis at twenty a second
    // The time LVGL took to refresh since the last update: an update that moved no pixel asks for no refresh, and the one before it is not charged again.
    std::uint64_t render_since_us = 0;
    // What the view costs (iclforge/panel_stats.hpp), and the rate it holds (iclforge/meters_governor.hpp).
    mv::RateGovernor governor;
    ps::MetersStats stats;
    mv::TapStats tap_base;  // the tap's counters when the view opened: what it reports is since then
    std::uint64_t opened_us = 0;
};

State g;
std::uint32_t g_opens = 0;
// The audio comes first: while the decoder is busy the panel holds the view at ten a second (now_meters_hold_slow()). Kept here, not in
// State, which every opening makes again.
bool g_hold_slow = false;
// The last opening's figures, kept when the view is closed so that a measurement can read them after the screen has gone to another view.
ps::MetersStats g_last;

// What the stats say now: the view's own figures, the governor's, and the tap's since the view opened.
ps::MetersStats snapshot() {
    ps::MetersStats s = g.stats;
    s.showing = g.built;
    s.opens = g_opens;
    s.open_ms = g.built ? (micros() - g.opened_us) / 1000 : 0;
    s.rate_hz = g.governor.rate_hz();
    s.mean_cost_us = static_cast<std::int64_t>(g.governor.mean_cost_us());
    s.governor = g.governor.stats();
    const mv::TapStats now = meters_tap().stats();
    s.tap = now;
    s.tap.blocks -= g.tap_base.blocks;
    s.tap.frames -= g.tap_base.frames;
    s.tap.us_total -= g.tap_base.us_total;
    s.tap.windows -= g.tap_base.windows;
    s.tap.reads -= g.tap_base.reads;
    s.tap.torn -= g.tap_base.torn;
    s.tap.short_reads -= g.tap_base.short_reads;
    s.tap.sampled_blocks -= g.tap_base.sampled_blocks;
    s.tap.us_pass -= g.tap_base.us_pass;
    s.tap.us_pack -= g.tap_base.us_pack;
    s.tap.us_clock -= g.tap_base.us_clock;
    return s;
}

// --- building -------------------------------------------------------------------------------------------------------

struct Block {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// The title row of a tall stage, the spectrum with its scale and labels above a block of `bottom_h` pixels, which is returned.
Block build_spectrum(lv_obj_t* parent, const Dim& d, std::string_view layout, std::string_view status, int bottom_h) {
    const bool tall = d.h >= 300;
    const int rows = d.wide ? 2 : 1;
    const int labels_h = rows * (d.f_small + 3) + 3;
    const int top = d.m / 2 + 2;
    const int left_x = d.m + d.scale_w + 4;
    const int plot_w = d.w - left_x - d.m;
    int plot_y = top;
    if (tall) {
        const std::string title = "Main mix  " + std::string(layout);
        label(parent, title.c_str(), d.f_label, 0x8B8583, left_x, top, plot_w - 300);
        if (!status.empty()) {
            const std::string text(status);
            label(parent, text.c_str(), d.f_label - 2, 0x8B8583, left_x + plot_w - 460, top + 2, 460, Align::kRight);
        }
        plot_y = top + d.f_label + 8 + 8;
    }
    const Block bottom{d.m, d.h - d.m / 2 - bottom_h, d.w - 2 * d.m, bottom_h};
    g.sx = left_x;
    g.sy = plot_y;
    g.sw = plot_w;
    g.sh = std::max(20, bottom.y - 6 - labels_h - plot_y);
    g.per_bar = mv::bands_per_bar(d.w);
    const int bars = mv::spectrum_bars(g.per_bar);
    g.bal.assign(static_cast<std::size_t>(bars), {});
    g.bars.assign(static_cast<std::size_t>(bars), {});
    const float pitch = static_cast<float>(plot_w) / static_cast<float>(bars);
    const int bar_w = std::max(2, static_cast<int>(std::lround(pitch * (g.per_bar == 1 ? 0.78F : 0.8F))));
    const auto bar_x = [&](int j) { return left_x + static_cast<int>(std::lround(static_cast<float>(j) * pitch + (pitch - static_cast<float>(bar_w)) / 2.0F)); };
    // The plot's floor, so the bars have something to stand on.
    rect(parent, left_x, g.sy + g.sh, plot_w, 1, 0x4B4645);
    for (int j = 0; j < bars; ++j) {
        g.bars[static_cast<std::size_t>(j)] = {bar_x(j) - left_x, bar_w, g.sh, -1};
    }
    g.spectrum_geometry = {plot_w, g.sh, g.sh - pixels(mv::kSpectrumAmberDb, g.sh), g.sh - pixels(mv::kSpectrumRedDb, g.sh)};
    g.lines.assign(static_cast<std::size_t>(g.sh), 0);
    // The lines of the scale cross the bars, so that a bar is read against them.
    const int step = g.sh >= 120 ? 10 : 20;
    for (int db = 0; db >= static_cast<int>(mv::kFloorDb); db -= step) {
        const int ly = g.sy + static_cast<int>(std::lround((1.0F - unit(static_cast<float>(db), mv::kFloorDb, mv::kTopDb)) * static_cast<float>(g.sh)));
        if (db > static_cast<int>(mv::kFloorDb)) {
            g.lines[static_cast<std::size_t>(std::min(ly - g.sy, g.sh - 1))] = 1;
        }
        std::array<char, 12> text{};
        std::snprintf(text.data(), text.size(), "%d", db);
        label(parent, text.data(), d.f_small, 0x8B8583, left_x - d.scale_w - 2, ly - d.f_small / 2 - 2, d.scale_w, Align::kRight);
    }
    if (make_chart(g.spectrum, parent, left_x, g.sy, plot_w, g.sh)) {
        dr::render_vbars(g.spectrum.px, g.spectrum.stride, g.spectrum_geometry, g.bars, g.lines, kSpectrumColours, false, 0, g.sh);
    }
    // The names of the bands: every fourth on a row or two.
    const int stride = g.per_bar == 1 ? 4 : g.per_bar == 2 ? 3 : 1;
    const int every = rows == 2 ? stride / 2 : stride;
    for (int j = 0; j < bars; j += every) {
        const int row = rows == 2 ? ((j / every) % 2) : 0;
        const int band = g.per_bar == 1 ? j : mv::first_band_of_bar(j, g.per_bar) + g.per_bar / 2;
        const int cx = left_x + static_cast<int>(std::lround((static_cast<float>(j) + 0.5F) * pitch));
        const std::string_view name = mv::kBandLabels[static_cast<std::size_t>(band)];
        label(parent, name.data(), d.f_small, 0x8B8583, cx - 30, g.sy + g.sh + 3 + row * (d.f_small + 3), 60, Align::kCentre);
    }
    return bottom;
}

// A row of vertical bars (the six masters, or every output) in `block`: their names under them, their numbers above them on a tall stage.
void build_bars(lv_obj_t* parent, const Dim& d, const Block& block, const std::vector<int>& centres, int pitch,
                const LvVector<std::string>& names, const std::vector<std::pair<std::string, std::pair<int, int>>>& captions) {
    const bool tall = d.h >= 300;
    const int caption_h = captions.empty() || !tall ? 0 : d.f_small + 8;
    const bool values = tall && centres.size() <= 16;
    const int value_h = values ? d.f_small + 4 : 0;
    const int name_h = d.f_small + 6;
    const int bar_top = block.y + value_h;
    const int bar_h = std::max(8, block.h - caption_h - value_h - name_h);
    const int bar_w = std::clamp(static_cast<int>(static_cast<float>(pitch) * 0.55F), 6, d.wide ? 64 : 44);
    g.meters.assign(centres.size(), {});
    g.meter_bars.assign(centres.size(), {});
    g.values.assign(centres.size(), {});
    const int chart_x = centres.front() - bar_w / 2;
    const int chart_w = centres.back() + (bar_w - bar_w / 2) - chart_x;
    for (std::size_t k = 0; k < centres.size(); ++k) {
        if (values) {
            g.values[k].object = label(parent, "", d.f_small, 0xECE7E5, centres[k] - pitch / 2, block.y, pitch, Align::kCentre);
        }
        g.meters[k].decimals = values && centres.size() <= 12 ? 1 : 0;
        g.meters[k].value = values ? &g.values[k] : nullptr;
        g.meter_bars[k] = {centres[k] - bar_w / 2 - chart_x, bar_w, bar_h, -1};
        label(parent, names[k].c_str(), d.f_small + 1, 0x8B8583, centres[k] - pitch / 2, bar_top + bar_h + 3, pitch, Align::kCentre);
    }
    g.channels_geometry = {chart_w, bar_h, bar_h - pixels(mv::kMeterAmberDb, bar_h), bar_h - pixels(mv::kMeterRedDb, bar_h)};
    if (make_chart(g.channels, parent, chart_x, bar_top, chart_w, bar_h)) {
        dr::render_vbars(g.channels.px, g.channels.stride, g.channels_geometry, g.meter_bars, {}, kMeterColours, true, 0, bar_h);
    }
    // The scale of the bars, at the left.
    for (const float db : {0.0F, -18.0F, -40.0F}) {
        const int ly = bar_top + static_cast<int>(std::lround((1.0F - unit(db, mv::kFloorDb, mv::kTopDb)) * static_cast<float>(bar_h)));
        std::array<char, 8> text{};
        std::snprintf(text.data(), text.size(), "%.0f", static_cast<double>(db));
        label(parent, text.data(), d.f_small, 0x4B4645, d.m, ly - d.f_small / 2 - 2, d.scale_w + 2, Align::kRight);
    }
    for (const auto& caption : captions) {
        if (!tall) {
            break;
        }
        const int cx = caption.second.first;
        const int cw = caption.second.second;
        rect(parent, cx + 2, block.y + block.h - caption_h + 2, cw - 4, 1, 0x4B4645);
        label(parent, caption.first.c_str(), d.f_small, 0x8B8583, cx, block.y + block.h - caption_h + 5, cw, Align::kCentre);
    }
}

std::vector<int> even_centres(int x, int w, int n) {
    std::vector<int> c;
    for (int k = 0; k < n; ++k) {
        c.push_back(x + static_cast<int>(std::lround((static_cast<float>(k) + 0.5F) * static_cast<float>(w) / static_cast<float>(n))));
    }
    return c;
}

// The room's layout as the heading says it ("Dolby Digital Plus 5.1 to 7.1.4": what is after the last " to "), and what the status line says of the stream.
std::string layout_of(const NowPlaying& now) {
    const std::string_view heading(now.heading.data(), now.heading.size());
    const std::size_t at = heading.rfind(" to ");
    return std::string(at == std::string_view::npos ? std::string_view() : heading.substr(at + 4));
}

std::string status_of(const NowPlaying& now, const std::string& layout) {
    std::string text = layout;
    text += "  48.0 kHz";
    if (now.kbps > 0) {
        text += "  " + std::to_string(now.kbps) + " kb/s";
    }
    return text;
}

}  // namespace

bool now_build_meters(lv_obj_t* parent, int width, int height, const NowPlaying& now, const NowStyle& /*style*/) {
    now_forget_meters();
    std::array<mv::Spot, mv::kMaxOutputs> spots{};
    const std::size_t count = std::min(now.spots.size(), spots.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& s = now.spots[i];
        spots[i] = {.azimuth_deg = static_cast<float>(s.azimuth_deg), .elevation_deg = static_cast<float>(s.elevation_deg), .lfe = s.lfe, .connected = s.connected};
    }
    const mv::Fold fold = mv::Fold::of(std::span<const mv::Spot>(spots.data(), count));
    if (fold.connected.count == 0) {
        return false;
    }
    void* const memory = lv_malloc(sizeof(Heavy));
    if (memory == nullptr) {
        return false;
    }
    g = State{};
    g.governor.set_hold_slow(g_hold_slow);
    g.heavy = new (memory) Heavy();
    g.d = dim_of(width, height);
    g.fold = fold;
    g.rep = mv::representation_for(width, static_cast<int>(fold.connected.count));
    const Dim& d = g.d;
    const bool tall = d.h >= 300;
    const std::string layout = layout_of(now);
    const std::string status = status_of(now, layout);

    if (g.rep == mv::Representation::kPair) {
        const int meter_h = d.wide ? 18 : d.h >= 200 ? 12 : 8;
        const int gap = d.wide ? 8 : 4;
        const int status_h = tall ? d.f_label + 6 : d.h >= 200 ? d.f_small + 4 : 0;
        const int scale_gap = tall ? 14 : 0;
        const int bottom_h = scale_gap + 2 * meter_h + gap + (status_h > 0 ? status_h + gap : 0);
        const Block bottom = build_spectrum(parent, d, layout, std::string_view(), bottom_h);
        g.downmix = fold.connected.count > 2;
        const int left_x = g.sx;
        const int plot_w = g.sw;
        const int value_w = d.wide ? 64 : d.h >= 200 ? 44 : 34;
        const int my = bottom.y + scale_gap;
        g.values.assign(2, {});
        g.meters.assign(2, {});
        g.pair_bars.assign(2, {});
        const int chart_w = plot_w - value_w - 8;
        const std::array<int, 2> rows = {my, my + meter_h + gap};
        const std::array<const char*, 2> names = {"L", "R"};
        for (std::size_t i = 0; i < 2; ++i) {
            g.values[i].object = label(parent, "", d.f_label, kTickColour, left_x + plot_w - value_w, rows[i] + (meter_h - d.f_label) / 2 - 2, value_w, Align::kRight);
            label(parent, names[i], d.f_small + 2, 0x8B8583, d.m, rows[i] + (meter_h - d.f_small) / 2 - 3, d.f_small + 6);
            g.meters[i].value = &g.values[i];
            g.pair_bars[i] = {rows[i] - my, meter_h, 0, -1};
        }
        g.pair_geometry = {chart_w, 2 * meter_h + gap, pixels(mv::kMeterAmberDb, chart_w), pixels(mv::kMeterRedDb, chart_w)};
        if (make_chart(g.channels, parent, left_x, my, chart_w, 2 * meter_h + gap)) {
            dr::render_hbars(g.channels.px, g.channels.stride, g.pair_geometry, g.pair_bars, kMeterColours, 0, g.channels.h);
        }
        if (tall) {
            for (const float db : {-60.0F, -40.0F, -20.0F, -10.0F, 0.0F}) {
                const int tx = left_x + static_cast<int>(std::lround(unit(db, mv::kFloorDb, mv::kTopDb) * static_cast<float>(plot_w - value_w - 8)));
                std::array<char, 8> text{};
                std::snprintf(text.data(), text.size(), "%.0f", static_cast<double>(db));
                label(parent, text.data(), d.f_small, 0x4B4645, tx - 20, bottom.y, 40, Align::kCentre);
            }
        }
        if (status_h > 0) {
            const int sy = d.h - status_h - d.m / 2;
            rect(parent, d.m, sy - 3, d.w - 2 * d.m, 1, 0x4B4645);
            const std::string line = status + (g.downmix ? "  stereo downmix" : "");
            label(parent, line.c_str(), tall ? d.f_label - 2 : d.f_small, 0x8B8583, d.m, sy, d.w - 2 * d.m - 120);
            g.peak_text.object = label(parent, "", tall ? d.f_label - 2 : d.f_small, kTickColour, d.w - d.m - 110, sy, 110, Align::kRight);
        }
    } else {
        const bool grid = g.rep == mv::Representation::kGrid;
        const int bottom_h = tall ? (grid ? 196 : 172) : (d.h >= 200 ? 96 : 60);
        const Block bottom = build_spectrum(parent, d, layout, tall ? std::string_view(status) : std::string_view(), bottom_h);
        LvVector<std::string> names;
        std::vector<int> centres;
        std::vector<std::pair<std::string, std::pair<int, int>>> captions;
        int pitch = 0;
        if (!grid) {
            for (const std::string_view name : mv::kMasterNames) {
                names.emplace_back(name);
            }
            centres = even_centres(g.sx, g.sw, static_cast<int>(mv::kMasters));
            pitch = g.sw / static_cast<int>(mv::kMasters);
        } else {
            // Every output, in space: the groups left to right, a gap between them.
            const int gap = d.wide ? 22 : 8;
            int groups = 0;
            int total = 0;
            for (const mv::Members& m : fold.group) {
                groups += m.count > 0 ? 1 : 0;
                total += m.count;
            }
            pitch = (g.sw - gap * (groups - 1)) / std::max(total, 1);
            int cx = g.sx;
            for (std::size_t grp = 0; grp < mv::kGroups; ++grp) {
                const mv::Members& m = fold.group[grp];
                if (m.count == 0) {
                    continue;
                }
                const int start = cx;
                for (const std::uint8_t o : m.span()) {
                    g.outputs.push_back(o);
                    names.emplace_back(now.spots[o].name.c_str());
                    centres.push_back(cx + pitch / 2);
                    cx += pitch;
                }
                captions.push_back({std::string(mv::kGroupNames[grp]), {start, cx - start}});
                cx += gap;
            }
        }
        build_bars(parent, d, bottom, centres, pitch, names, captions);
    }
    if (g.spectrum.object == nullptr || g.channels.object == nullptr) {
        // No memory for a chart: the view is not shown, and the screen draws what it draws without it.
        now_forget_meters();
        return false;
    }
    g.built = true;
    mv::MeterTap& tap = meters_tap();
    ++g_opens;
    g.opened_us = micros();
    g.tap_base = tap.stats();
    tap.reset_max();
    g.stats.representation = g.rep;
    g.stats.outputs = static_cast<int>(fold.connected.count);
    g.stats.width = width;
    g.stats.height = height;
    tap.set_wanted(true);
    return true;
}

void now_forget_meters() {
    if (g.heavy != nullptr) {
        g.heavy->~Heavy();
        lv_free(g.heavy);
        g.heavy = nullptr;
    }
    if (g.built) {
        meters_tap().set_wanted(false);
        g_last = snapshot();
        g_last.showing = false;
    }
    drop_chart(g.spectrum);
    drop_chart(g.channels);
    g = State{};
}

ps::MetersStats now_meters_stats() { return g.built ? snapshot() : g_last; }

void now_meters_hold_slow(bool hold) {
    g_hold_slow = hold;
    g.governor.set_hold_slow(hold);
}

void now_meters_refresh(std::uint32_t took_us) {
    if (g.built) {
        g.stats.render.add(took_us);
        g.render_since_us += took_us;
    }
}

namespace {

void update_spectrum(float seconds) {
    std::array<dr::Change, dr::kMaxChanges> changes{};
    const std::size_t bars = std::min(g.bars.size(), changes.size());
    for (std::size_t j = 0; j < bars; ++j) {
        const float level = mv::bar_db(g.averaged, static_cast<int>(j), g.per_bar);
        g.bal[j].update(level, seconds, mv::kSpectrumFallDbPerS, mv::kSpectrumHoldS, mv::kSpectrumPeakFallDbPerS);
        const int top = g.sh - pixels(g.bal[j].level, g.sh);
        const int peak = pixels(g.bal[j].peak, g.sh);
        const int tick = peak > 1 ? std::max(g.sh - peak - 2, 0) : -1;
        changes[j] = {g.bars[j].x, g.bars[j].x + g.bars[j].width, dr::rows_touched(g.bars[j].top, g.bars[j].tick, top, tick, g.sh)};
        g.bars[j].top = top;
        g.bars[j].tick = tick;
    }
    draw_cells(g.spectrum, g.spectrum_geometry, g.bars, g.lines, kSpectrumColours, false, std::span<const dr::Change>(changes.data(), bars));
}

// `numbers`: the numbers under or beside the bars are written this time.
void update_channels(const mv::FastLevels& fast, float seconds, bool numbers) {
    const std::span<const float> peaks(fast.peak_db.data(), fast.outputs);
    dr::Rows touched;  // the pair's: the whole of its chart's width
    std::array<dr::Change, dr::kMaxChanges> changes{};
    switch (g.rep) {
        case mv::Representation::kPair: {
            float l = mv::kSilentDb;
            float r = mv::kSilentDb;
            if (g.downmix) {
                mv::downmix_db(peaks, g.fold, l, r);
            } else if (g.fold.connected.count >= 2) {
                const std::size_t a = g.fold.connected.output[0];
                const std::size_t b = g.fold.connected.output[1];
                l = a < peaks.size() ? peaks[a] : mv::kSilentDb;
                r = b < peaks.size() ? peaks[b] : mv::kSilentDb;
            }
            const bool moved_l = move_hbar(g.meters[0], g.pair_bars[0], l, seconds, g.pair_geometry.width);
            const bool moved_r = move_hbar(g.meters[1], g.pair_bars[1], r, seconds, g.pair_geometry.width);
            if (moved_l || moved_r) {
                touched = {0, g.channels.h};
                dr::render_hbars(g.channels.px, g.channels.stride, g.pair_geometry, g.pair_bars, kMeterColours, 0, g.channels.h);
            }
            g.peak_held = std::max(g.meters[0].bal.peak, g.meters[1].bal.peak);
            break;
        }
        case mv::Representation::kSum:
            for (std::size_t m = 0; m < mv::kMasters; ++m) {
                move_vbar(g.meters[m], g.meter_bars[m], mv::power_sum_db(peaks, g.fold.master[m].span()), seconds, g.channels_geometry.height, changes[m]);
            }
            break;
        case mv::Representation::kGrid:
            for (std::size_t k = 0; k < g.meters.size(); ++k) {
                const std::size_t o = g.outputs[k];
                move_vbar(g.meters[k], g.meter_bars[k], o < peaks.size() ? peaks[o] : mv::kSilentDb, seconds, g.channels_geometry.height, changes[k]);
            }
            break;
    }
    if (g.rep != mv::Representation::kPair) {
        const std::size_t n = std::min(g.meter_bars.size(), changes.size());
        draw_cells(g.channels, g.channels_geometry, g.meter_bars, {}, kMeterColours, true, std::span<const dr::Change>(changes.data(), n));
    } else {
        show_rows(g.channels, touched);
    }
    if (!numbers) {
        return;
    }
    for (Bar& bar : g.meters) {
        if (bar.value != nullptr) {
            bar.value->set(bar.bal.peak, bar.decimals);
        }
    }
    if (g.peak_text.object != nullptr) {
        std::array<char, 16> text{};
        std::snprintf(text.data(), text.size(), "peak %.1f dB", static_cast<double>(g.peak_held));
        if (text != g.peak_text.text) {
            g.peak_text.text = text;
            lv_label_set_text_static(g.peak_text.object, g.peak_text.text.data());
        }
    }
}

}  // namespace

bool now_update_meters(std::uint64_t now_us, bool dimmed) {
    if (!g.built) {
        return false;
    }
    mv::MeterTap& tap = meters_tap();
    // While dimmed the player is not asked for its output, and nothing is drawn.
    tap.set_wanted(!dimmed);
    if (dimmed) {
        g.last_us = 0;
        g.due_us = 0;
        g.render_since_us = 0;
        g.governor.restart_window();
        return false;
    }
    // The step keeps its own beat rather than the frames': a loop of 30 frames a second serves a 50 ms step at 67 ms, 33 ms, 67 ms ... and
    // twenty a second is what comes of it; counting 50 ms from the last update would give fifteen.
    const auto now = static_cast<std::int64_t>(now_us);
    const bool fast = !g.governor.slow();
    if (g.due_us != 0 && now + kEarlyUs < g.due_us) {
        return fast;
    }
    const std::int64_t step = g.governor.step_us();
    const std::int64_t behind = g.due_us == 0 ? 0 : std::max<std::int64_t>(now - g.due_us, 0);
    const std::int64_t missed = behind / step;
    // Lateness the loop's own beat accounts for is nobody's fault: only what is past it counts against the rate. The beat is a frame while the view
    // fills (twenty a second) and 100 ms while it rests (ten), when an update can be due any time in the beat before the loop looks.
    const std::int64_t beat = g.governor.slow() ? mv::kSlowStepUs : static_cast<std::int64_t>(panel_frame_us());
    const std::int64_t late = std::max<std::int64_t>(behind - beat, 0);
    const float seconds = g.last_us == 0 ? static_cast<float>(step) / 1.0e6F : std::clamp(static_cast<float>(now_us - g.last_us) / 1.0e6F, 0.02F, 0.25F);
    g.last_us = now_us;

    const std::uint64_t t0 = micros();
    if (tap.read_history(g.heavy->history)) {
        // The 17 bands up to 800 Hz look at the newest 171 ms, which changes little in 50: at twenty updates a second they are done every other one
        // (the bigger of the two transforms, two thirds of the work), and at ten, or the first time, every one.
        if (!g.have_spectrum || g.governor.slow() || (g.analyses & 1U) == 0) {
            g.heavy->analyzer.analyze_low(g.heavy->history, g.bands);
        }
        g.heavy->analyzer.analyze_high(g.heavy->history, g.bands);
        ++g.analyses;
        g.average.feed(g.bands, seconds, mv::kSpectrumAverageS, g.averaged);
        g.have_spectrum = true;
    }
    const std::uint64_t t1 = micros();
    if (g.have_spectrum) {
        update_spectrum(seconds);
    }
    const bool numbers = g.values_due_us == 0 || now >= g.values_due_us;
    if (numbers) {
        g.values_due_us = now + kValueEveryUs;
    }
    update_channels(tap.fast_levels(), seconds, numbers);
    const std::uint64_t t2 = micros();
    g.stats.analysis.add(t1 - t0);
    g.stats.widgets.add(t2 - t1);

    // What this update cost the task: its own work, and the refreshes since the last update, which it asked for (the one this asks for comes after).
    // None if it moved no pixel: a steady tone is a still picture, and the last refresh's time is not charged to it again.
    const std::int64_t cost = static_cast<std::int64_t>(t2 - t0) + static_cast<std::int64_t>(g.render_since_us);
    g.render_since_us = 0;
    const std::int64_t next = g.governor.record(now, cost, late, missed);
    g.due_us = next != step || g.due_us == 0 ? now + next : g.due_us + next * (1 + missed);
    return !g.governor.slow();
}

}  // namespace player::panel
