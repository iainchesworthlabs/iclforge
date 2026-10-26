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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <span>
#include <string>
#include <string_view>

#include "iclforge/dsp/spectrum.hpp"
#include "iclforge/meters_tap.hpp"
#include "iclforge/meters_view.hpp"

#include "fonts.hpp"
#include "lv_alloc.hpp"

namespace player::panel {

namespace {

namespace mv = iclforge::meters;
namespace dsp = iclforge::dsp::spectrum;

constexpr std::uint32_t kGreen = 0x4FC28A;
constexpr std::uint32_t kAmber = 0xF0B24A;
constexpr std::uint32_t kRed = 0xFF5C40;
constexpr std::uint32_t kTrough = 0x201D1C;
constexpr std::uint32_t kTickColour = 0xECE7E5;
// The stage's own colour, for the lines of the scale that cross the bars.
constexpr std::uint32_t kStageBg = 0x151313;
constexpr std::int64_t kStepUs = 50'000;  // twenty updates a second
// A frame that comes this much before an update is due is that update's: the loop's own beat jitters by a tick or two.
constexpr std::int64_t kEarlyUs = 8'000;

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

void set_opa(lv_obj_t* object, bool on) { lv_obj_set_style_bg_opa(object, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0); }

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

// A level meter, vertical or horizontal: green, amber and red in a column or a row, a peak-hold tick, the numeric peak.
struct Meter {
    bool vertical = false;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::array<lv_obj_t*, 3> zone{};
    lv_obj_t* tick = nullptr;
    Value* value = nullptr;
    int decimals = 1;
    dsp::Ballistic bal;
    int px_level = -1;
    int px_tick = -1;

    void build(lv_obj_t* parent, int mx, int my, int mw, int mh, bool vert, Value* v) {
        x = mx;
        y = my;
        w = mw;
        h = mh;
        vertical = vert;
        value = v;
        rect(parent, x, y, w, h, kTrough);
        const std::array<std::uint32_t, 3> colours = {kGreen, kAmber, kRed};
        for (std::size_t i = 0; i < 3; ++i) {
            zone[i] = rect(parent, x, y, 1, 1, colours[i]);
            set_opa(zone[i], false);
        }
        tick = rect(parent, x, y, vert ? w : 2, vert ? 2 : h, kTickColour, 0);
    }

    void update(float db, float seconds) {
        bal.update(db, seconds, mv::kMeterFallDbPerS, mv::kMeterHoldS, mv::kMeterPeakFallDbPerS);
        const int total = vertical ? h : w;
        const int lit = pixels(bal.level, total);
        if (lit != px_level) {
            px_level = lit;
            const std::array<int, 3> from = {0, pixels(mv::kMeterAmberDb, total), pixels(mv::kMeterRedDb, total)};
            const std::array<int, 3> to = {std::min(lit, from[1]), std::min(lit, from[2]), lit};
            for (std::size_t i = 0; i < 3; ++i) {
                const int len = std::max(0, to[i] - from[i]);
                set_opa(zone[i], len > 0);
                if (len > 0) {
                    if (vertical) {
                        lv_obj_set_size(zone[i], w, len);
                        lv_obj_set_pos(zone[i], x, y + h - from[i] - len);
                    } else {
                        lv_obj_set_size(zone[i], len, h);
                        lv_obj_set_pos(zone[i], x + from[i], y);
                    }
                }
            }
        }
        const int tk = pixels(bal.peak, total);
        if (tk != px_tick) {
            px_tick = tk;
            lv_obj_set_style_bg_opa(tick, tk > 1 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            if (vertical) {
                lv_obj_set_y(tick, y + h - std::max(tk, 2));
            } else {
                lv_obj_set_x(tick, x + std::max(tk, 2) - 2);
            }
        }
        if (value != nullptr) {
            value->set(bal.peak, decimals);
        }
    }
};

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
    // The spectrum's plot, and each bar's three rectangles and its tick.
    int sx = 0;
    int sy = 0;
    int sw = 0;
    int sh = 0;
    LvVector<std::array<lv_obj_t*, 3>> bar;
    LvVector<lv_obj_t*> tick;
    LvVector<int> bar_px;
    LvVector<int> tick_px;
    LvVector<dsp::Ballistic> bal;
    // The channels: two horizontal meters for the pair, or a bar for each master or output.
    LvVector<Meter> meters;
    LvVector<Value> values;
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
};

State g;

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
    g.bar.assign(static_cast<std::size_t>(bars), {});
    g.tick.assign(static_cast<std::size_t>(bars), nullptr);
    g.bar_px.assign(static_cast<std::size_t>(bars), 0);
    g.tick_px.assign(static_cast<std::size_t>(bars), -1);
    g.bal.assign(static_cast<std::size_t>(bars), {});
    const float pitch = static_cast<float>(plot_w) / static_cast<float>(bars);
    const int bar_w = std::max(2, static_cast<int>(std::lround(pitch * (g.per_bar == 1 ? 0.78F : 0.8F))));
    const auto bar_x = [&](int j) { return left_x + static_cast<int>(std::lround(static_cast<float>(j) * pitch + (pitch - static_cast<float>(bar_w)) / 2.0F)); };
    // The plot's floor, so the bars have something to stand on.
    rect(parent, left_x, g.sy + g.sh, plot_w, 1, 0x4B4645);
    const std::array<std::uint32_t, 3> colours = {kGreen, kAmber, kRed};
    for (int j = 0; j < bars; ++j) {
        for (std::size_t z = 0; z < 3; ++z) {
            lv_obj_t* const r = rect(parent, bar_x(j), g.sy + g.sh, bar_w, 1, colours[z]);
            set_opa(r, false);
            g.bar[static_cast<std::size_t>(j)][z] = r;
        }
    }
    // The lines of the scale cross the bars, so that a bar is read against them.
    const int step = g.sh >= 120 ? 10 : 20;
    for (int db = 0; db >= static_cast<int>(mv::kFloorDb); db -= step) {
        const int ly = g.sy + static_cast<int>(std::lround((1.0F - unit(static_cast<float>(db), mv::kFloorDb, mv::kTopDb)) * static_cast<float>(g.sh)));
        if (db > static_cast<int>(mv::kFloorDb)) {
            rect(parent, left_x, std::min(ly, g.sy + g.sh - 1), plot_w, 1, kStageBg, 120);
        }
        std::array<char, 12> text{};
        std::snprintf(text.data(), text.size(), "%d", db);
        label(parent, text.data(), d.f_small, 0x8B8583, left_x - d.scale_w - 2, ly - d.f_small / 2 - 2, d.scale_w, Align::kRight);
    }
    for (int j = 0; j < bars; ++j) {
        g.tick[static_cast<std::size_t>(j)] = rect(parent, bar_x(j), g.sy + g.sh, bar_w, 2, kTickColour, 0);
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
    g.values.assign(centres.size(), {});
    for (std::size_t k = 0; k < centres.size(); ++k) {
        if (values) {
            g.values[k].object = label(parent, "", d.f_small, 0xECE7E5, centres[k] - pitch / 2, block.y, pitch, Align::kCentre);
        }
        g.meters[k].decimals = values && centres.size() <= 12 ? 1 : 0;
        g.meters[k].build(parent, centres[k] - bar_w / 2, bar_top, bar_w, bar_h, true, values ? &g.values[k] : nullptr);
        label(parent, names[k].c_str(), d.f_small + 1, 0x8B8583, centres[k] - pitch / 2, bar_top + bar_h + 3, pitch, Align::kCentre);
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
        const std::array<int, 2> rows = {my, my + meter_h + gap};
        const std::array<const char*, 2> names = {"L", "R"};
        for (std::size_t i = 0; i < 2; ++i) {
            g.values[i].object = label(parent, "", d.f_label, kTickColour, left_x + plot_w - value_w, rows[i] + (meter_h - d.f_label) / 2 - 2, value_w, Align::kRight);
            label(parent, names[i], d.f_small + 2, 0x8B8583, d.m, rows[i] + (meter_h - d.f_small) / 2 - 3, d.f_small + 6);
            g.meters[i].build(parent, left_x, rows[i], plot_w - value_w - 8, meter_h, false, &g.values[i]);
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
    g.built = true;
    mv::MeterTap& tap = meters_tap();
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
    }
    g = State{};
}

namespace {

void update_spectrum(float seconds) {
    const std::size_t bars = g.bar.size();
    for (std::size_t j = 0; j < bars; ++j) {
        const float level = mv::bar_db(g.averaged, static_cast<int>(j), g.per_bar);
        g.bal[j].update(level, seconds, mv::kSpectrumFallDbPerS, mv::kSpectrumHoldS, mv::kSpectrumPeakFallDbPerS);
        const int bh = pixels(g.bal[j].level, g.sh);
        if (bh != g.bar_px[j]) {
            g.bar_px[j] = bh;
            const std::array<int, 3> from = {0, pixels(mv::kSpectrumAmberDb, g.sh), pixels(mv::kSpectrumRedDb, g.sh)};
            const std::array<int, 3> to = {std::min(bh, from[1]), std::min(bh, from[2]), bh};
            for (std::size_t z = 0; z < 3; ++z) {
                const int len = std::max(0, to[z] - from[z]);
                set_opa(g.bar[j][z], len > 0);
                if (len > 0) {
                    lv_obj_set_height(g.bar[j][z], len);
                    lv_obj_set_y(g.bar[j][z], g.sy + g.sh - from[z] - len);
                }
            }
        }
        const int th = pixels(g.bal[j].peak, g.sh);
        if (th != g.tick_px[j]) {
            g.tick_px[j] = th;
            lv_obj_set_style_bg_opa(g.tick[j], th > 1 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_y(g.tick[j], g.sy + g.sh - std::max(th, 1) - 2);
        }
    }
}

void update_channels(const mv::FastLevels& fast, float seconds) {
    const std::span<const float> peaks(fast.peak_db.data(), fast.outputs);
    float loudest = mv::kSilentDb;
    for (const float db : peaks) {
        loudest = std::max(loudest, db);
    }
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
            g.meters[0].update(l, seconds);
            g.meters[1].update(r, seconds);
            g.peak_held = std::max(g.meters[0].bal.peak, g.meters[1].bal.peak);
            break;
        }
        case mv::Representation::kSum:
            for (std::size_t m = 0; m < mv::kMasters; ++m) {
                g.meters[m].update(mv::power_sum_db(peaks, g.fold.master[m].span()), seconds);
            }
            break;
        case mv::Representation::kGrid:
            for (std::size_t k = 0; k < g.meters.size(); ++k) {
                const std::size_t o = g.outputs[k];
                g.meters[k].update(o < peaks.size() ? peaks[o] : mv::kSilentDb, seconds);
            }
            break;
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
        return false;
    }
    // The step keeps its own beat rather than the frames': a loop of 30 frames a second serves a 50 ms step at 67 ms, 33 ms, 67 ms ... and
    // twenty a second is what comes of it; counting 50 ms from the last update would give fifteen.
    const auto now = static_cast<std::int64_t>(now_us);
    if (g.due_us != 0 && now + kEarlyUs < g.due_us) {
        return true;
    }
    const std::int64_t missed = g.due_us == 0 ? 0 : std::max<std::int64_t>(now - g.due_us, 0) / kStepUs;
    g.due_us = g.due_us == 0 ? now + kStepUs : g.due_us + kStepUs * (1 + missed);
    const float seconds = g.last_us == 0 ? 0.05F : std::clamp(static_cast<float>(now_us - g.last_us) / 1.0e6F, 0.02F, 0.25F);
    g.last_us = now_us;
    if (tap.read_history(g.heavy->history)) {
        g.heavy->analyzer.analyze(g.heavy->history, g.bands);
        g.average.feed(g.bands, seconds, mv::kSpectrumAverageS, g.averaged);
        g.have_spectrum = true;
    }
    if (g.have_spectrum) {
        update_spectrum(seconds);
    }
    update_channels(tap.fast_levels(), seconds);
    return true;
}

}  // namespace player::panel
