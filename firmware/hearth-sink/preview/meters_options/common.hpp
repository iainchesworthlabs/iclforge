#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"

#include "iclforge/now_view.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"

#include "../speakers_options/scene.hpp"
#include "spectrum.hpp"

namespace fs = std::filesystem;
namespace rp = iclforge::render;
namespace nv = iclforge::nowview;
using meters::kBands;

namespace {

constexpr int kMaxW = 1024;
constexpr int kMaxH = 600;
constexpr int kBufferRows = 40;
constexpr std::uint64_t kHopUs = 50'000;  // the analysis runs at 20 Hz
constexpr float kHopSeconds = 0.05F;

constexpr std::uint32_t kBg = 0x151313;
constexpr std::uint32_t kSurface = 0x1D1A1A;
constexpr std::uint32_t kSurfaceUp = 0x272423;
constexpr std::uint32_t kText = 0xECE7E5;
constexpr std::uint32_t kAccent = 0xFF5C40;
constexpr std::uint32_t kMuted = 0x8B8583;
constexpr std::uint32_t kFaint = 0x4B4645;
constexpr std::uint32_t kGreen = 0x4FC28A;
constexpr std::uint32_t kAmber = 0xF0B24A;
constexpr std::uint32_t kTrough = 0x201D1C;

std::uint64_t g_now_us = 0;
std::uint64_t g_flushed_pixels = 0;
std::uint32_t g_flushed_areas = 0;
// The spectrum's bars in the zone colours of the meters (green, amber, red by level) instead of the accent.
bool g_zoned_spectrum = false;
std::vector<std::uint16_t> g_frame(static_cast<std::size_t>(kMaxW) * kMaxH, 0);

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void flush(lv_display_t* disp, const lv_area_t* area, std::uint8_t* px_map) {
    const int width = lv_area_get_width(area);
    g_flushed_pixels += static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(lv_area_get_height(area));
    ++g_flushed_areas;
    const auto* src = reinterpret_cast<const std::uint16_t*>(px_map);
    for (int y = area->y1; y <= area->y2; ++y) {
        std::copy(src, src + width, g_frame.begin() + static_cast<std::ptrdiff_t>(y) * kMaxW + area->x1);
        src += width;
    }
    lv_display_flush_ready(disp);
}

// A 24-bit BMP of the top left w x h of the frame (bottom row first); with `half` averaged down by two.
void write_bmp(const fs::path& path, int w, int h, bool half = false) {
    const int out_w = half ? w / 2 : w;
    const int out_h = half ? h / 2 : h;
    const std::size_t row_bytes = (static_cast<std::size_t>(out_w) * 3 + 3) / 4 * 4;
    const std::size_t image = row_bytes * static_cast<std::size_t>(out_h);
    std::vector<std::uint8_t> out(54 + image, 0);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    };
    out[0] = 'B';
    out[1] = 'M';
    put(2, static_cast<std::uint32_t>(out.size()));
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(out_w));
    put(22, static_cast<std::uint32_t>(out_h));
    out[26] = 1;
    out[28] = 24;
    put(34, static_cast<std::uint32_t>(image));
    const int span = half ? 2 : 1;
    for (int y = 0; y < out_h; ++y) {
        std::uint8_t* row = out.data() + 54 + static_cast<std::size_t>(out_h - 1 - y) * row_bytes;
        for (int x = 0; x < out_w; ++x) {
            int r = 0;
            int g = 0;
            int b = 0;
            for (int dy = 0; dy < span; ++dy) {
                for (int dx = 0; dx < span; ++dx) {
                    const std::uint16_t p = g_frame[static_cast<std::size_t>(y * span + dy) * kMaxW + static_cast<std::size_t>(x * span + dx)];
                    const int r5 = (p >> 11) & 0x1F;
                    const int g6 = (p >> 5) & 0x3F;
                    const int b5 = p & 0x1F;
                    r += (r5 << 3) | (r5 >> 2);
                    g += (g6 << 2) | (g6 >> 4);
                    b += (b5 << 3) | (b5 >> 2);
                }
            }
            const int n = span * span;
            *row++ = static_cast<std::uint8_t>(b / n);
            *row++ = static_cast<std::uint8_t>(g / n);
            *row++ = static_cast<std::uint8_t>(r / n);
        }
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

// --- drawing helpers --------------------------------------------------------------------------------------------

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

lv_font_t* font(int px) {
    static std::map<int, lv_font_t*> fonts;
    static const std::vector<std::uint8_t> data = read_file(fs::path(PREVIEW_FONT_DIR) / "Archivo-Regular.ttf");
    const auto it = fonts.find(px);
    if (it != fonts.end()) {
        return it->second;
    }
    lv_font_t* const created = lv_tiny_ttf_create_data(data.data(), data.size(), px);
    fonts[px] = created;
    return created;
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

lv_obj_t* rect(lv_obj_t* parent, int x, int y, int w, int h, std::uint32_t rgb, int opa = 255, int radius = 0) {
    lv_obj_t* const object = plain(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, std::max(w, 1), std::max(h, 1));
    lv_obj_set_style_bg_color(object, color(rgb), 0);
    lv_obj_set_style_bg_opa(object, static_cast<lv_opa_t>(std::clamp(opa, 0, 255)), 0);
    lv_obj_set_style_radius(object, radius, 0);
    return object;
}

enum class Align { kLeft, kCentre, kRight };

// A label in a box of w x h at (x, y); `align` is how the text sits in the box.
lv_obj_t* text(lv_obj_t* parent, const std::string& s, int px, std::uint32_t rgb, int x, int y, int w, Align align = Align::kLeft) {
    lv_obj_t* const label = lv_label_create(parent);
    lv_label_set_text(label, s.c_str());
    lv_obj_set_style_text_font(label, font(px), 0);
    lv_obj_set_style_text_color(label, color(rgb), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(label, w);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_align(label, align == Align::kLeft ? LV_TEXT_ALIGN_LEFT : align == Align::kCentre ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_RIGHT, 0);
    return label;
}

float unit(float db, float floor_db, float top_db) { return std::clamp((db - floor_db) / (top_db - floor_db), 0.0F, 1.0F); }

std::string db_text(float db, int decimals) {
    if (db <= -99.0F) {
        return "-inf";
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, decimals == 0 ? "%.0f" : "%.1f", static_cast<double>(db));
    return buf;
}

// --- the geometry of a size -----------------------------------------------------------------------------------------

struct Size {
    const char* key;
    int w;
    int h;
    int header;
    int footer;
};

constexpr std::array<Size, 3> kSizes = {{{"1024x600", 1024, 600, 64, 64}, {"480x320", 480, 320, 48, 56}, {"240x240", 240, 240, 40, 56}}};

// What a stage of this size allows.
struct Dim {
    int w = 0;
    int h = 0;
    int m = 0;        // the margin
    int f_small = 0;  // the scale and the band labels
    int f_label = 0;  // a name, a value
    int merge = 1;    // third-octave bands to a bar: 1, 2 (15 bars) or 3 (10 bars)
    int scale_w = 0;  // the dB scale's column
    bool wide = false;
};

Dim dim_of(int w, int h) {
    Dim d;
    d.w = w;
    d.h = h;
    d.wide = w >= 800;
    d.m = w >= 800 ? 20 : w >= 400 ? 10 : 6;
    d.f_small = w >= 800 ? 16 : w >= 400 ? 12 : 11;
    d.f_label = w >= 800 ? 20 : w >= 400 ? 14 : 12;
    d.merge = w >= 800 ? 1 : w >= 400 ? 2 : 3;
    d.scale_w = w >= 800 ? 44 : w >= 400 ? 28 : 22;
    return d;
}

// --- the room ------------------------------------------------------------------------------------------------------------

struct Room {
    std::string layout;
    std::vector<rp::PlanSpot> spots;
    // The outputs of each group of speakers: front (with the LFE), surround, height.
    std::vector<std::vector<int>> groups;
    std::vector<std::string> group_names;
    int left = -1;
    int right = -1;
};

Room make_room(const char* layout) {
    const auto parsed = rp::OutputLayout::parse(layout);
    if (!parsed) {
        std::fprintf(stderr, "layout %s\n", layout);
        std::exit(2);
    }
    Room room;
    room.layout = layout;
    room.spots = rp::plan_spots(*parsed);
    std::vector<int> front;
    std::vector<int> surround;
    std::vector<int> height;
    for (std::size_t i = 0; i < room.spots.size(); ++i) {
        const auto& s = room.spots[i];
        if (s.name == "L") {
            room.left = static_cast<int>(i);
        } else if (s.name == "R") {
            room.right = static_cast<int>(i);
        }
        if (!s.connected) {
            continue;
        }
        if (!s.lfe && s.elevation_deg > 20.0) {
            height.push_back(static_cast<int>(i));
        } else if (!s.lfe && std::fabs(s.azimuth_deg) > 80.0) {
            surround.push_back(static_cast<int>(i));
        } else {
            front.push_back(static_cast<int>(i));
        }
    }
    const auto add = [&](std::vector<int>& g, const char* name) {
        if (!g.empty()) {
            room.groups.push_back(g);
            room.group_names.emplace_back(name);
        }
    };
    add(front, "Front");
    add(surround, "Surround");
    add(height, "Height");
    return room;
}

// --- the model: what the board would give the view each 50 ms --------------------------------------------------------

struct Hop {
    std::array<float, kBands> main{};
    // The spectrum of the one output a screen has focused (Model's `focus_output`), when there is one.
    std::array<float, kBands> focus{};
    std::vector<std::array<float, kBands>> groups;
    std::array<float, scene::kMaxOut> peak_db{};
    std::array<float, scene::kMaxOut> rms_db{};
    std::vector<float> group_peak_db;
};

std::vector<float> tap(const scene::Scene& s, const std::vector<int>& outputs, int first_sample) {
    int shift = 0;
    while ((1ULL << (2 * static_cast<unsigned>(shift))) < outputs.size()) {
        ++shift;
    }
    std::vector<float> mix(2400, 0.0F);
    const auto& out = s.outputs();
    for (const int o : outputs) {
        for (int k = 0; k < 2400; ++k) {
            mix[static_cast<std::size_t>(k)] += out[static_cast<std::size_t>(o)][static_cast<std::size_t>(first_sample + k)];
        }
    }
    const float scale = 1.0F / static_cast<float>(1ULL << shift);
    for (float& v : mix) {
        v *= scale;
    }
    return mix;
}

class Model {
   public:
    explicit Model(const Room& room, int focus_output = -1)
        : room_(room), groups_(room.groups.size()), group_avg_(room.groups.size()), focus_output_(focus_output) {
        for (std::size_t i = 0; i < room.spots.size(); ++i) {
            if (room.spots[i].connected) {
                all_.push_back(static_cast<int>(i));
            }
        }
    }

    // `window` is the scene's 100 ms; two hops of 50 ms.
    void hops(const scene::Scene& s, std::array<Hop, 2>& out) {
        for (int h = 0; h < 2; ++h) {
            Hop& hop = out[static_cast<std::size_t>(h)];
            hop.groups.assign(room_.groups.size(), {});
            hop.group_peak_db.assign(room_.groups.size(), meters::kSilentDb);
            const int first = h * 2400;
            const auto& outs = s.outputs();
            for (std::size_t o = 0; o < scene::kMaxOut; ++o) {
                float peak = 0.0F;
                double sum = 0.0;
                for (int k = 0; k < 2400; ++k) {
                    const float v = outs[o][static_cast<std::size_t>(first + k)];
                    peak = std::max(peak, std::fabs(v));
                    sum += static_cast<double>(v) * v;
                }
                hop.peak_db[o] = peak <= 1e-6F ? meters::kSilentDb : 20.0F * std::log10(peak);
                hop.rms_db[o] = sum <= 1e-12 ? meters::kSilentDb : static_cast<float>(10.0 * std::log10(sum / 2400.0));
            }
            const std::vector<float> mix = tap(s, all_, first);
            main_.push(mix);
            std::array<float, kBands> raw{};
            main_.analyze(raw);
            main_avg_.feed(raw, kHopSeconds, kAverage, hop.main);
            if (focus_output_ >= 0) {
                focus_.push(tap(s, {focus_output_}, first));
                std::array<float, kBands> fraw{};
                focus_.analyze(fraw);
                focus_avg_.feed(fraw, kHopSeconds, kAverage, hop.focus);
            }
            for (std::size_t g = 0; g < room_.groups.size(); ++g) {
                const std::vector<float> gm = tap(s, room_.groups[g], first);
                groups_[g].push(gm);
                std::array<float, kBands> graw{};
                groups_[g].analyze(graw);
                hop.groups[g] = {};
                group_avg_[g].feed(graw, kHopSeconds, kAverage, hop.groups[g]);
                for (const int o : room_.groups[g]) {
                    hop.group_peak_db[g] = std::max(hop.group_peak_db[g], hop.peak_db[static_cast<std::size_t>(o)]);
                }
            }
        }
    }

    static constexpr float kAverage = 0.08F;  // the analyser's time average: 80 ms

   private:
    const Room& room_;
    std::vector<int> all_;
    meters::Analyzer main_;
    meters::PowerAverage main_avg_;
    std::vector<meters::Analyzer> groups_;
    std::vector<meters::PowerAverage> group_avg_;
    int focus_output_ = -1;
    meters::Analyzer focus_;
    meters::PowerAverage focus_avg_;
};

// --- components ---------------------------------------------------------------------------------------------------------------

const char* const kBandLabels[kBands] = {"20", "25", "31", "40", "50", "63", "80", "100", "125", "160", "200", "250", "315", "400", "500", "630",
                                         "800", "1k", "1.2k", "1.6k", "2k", "2.5k", "3.1k", "4k", "5k", "6.3k", "8k", "10k", "12k", "16k", "20k"};

float group_db(const std::array<float, kBands>& db, int first, int count) {
    double sum = 0.0;
    for (int b = first; b < first + count && b < kBands; ++b) {
        if (db[static_cast<std::size_t>(b)] > meters::kSilentDb) {
            sum += 0.5 * std::pow(10.0, db[static_cast<std::size_t>(b)] / 10.0);
        }
    }
    return meters::to_db(sum);
}

// The spectrum: bars of third-octave bands (or two or three to a bar), peak-hold ticks, a dB scale, band labels.
struct Spectrum {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int bars = 0;
    int merge = 1;
    float floor_db = -60.0F;
    float top_db = 0.0F;
    std::vector<lv_obj_t*> bar;
    std::vector<lv_obj_t*> bar_amber;
    std::vector<lv_obj_t*> bar_red;
    bool zoned = false;
    static constexpr float kAmberAt = -30.0F;
    static constexpr float kRedAt = -15.0F;
    std::vector<lv_obj_t*> tick;
    std::vector<int> bar_px;
    std::vector<int> tick_px;
    std::vector<meters::Ballistic> bal;

    int first_band(int j) const { return merge == 1 ? j : 1 + j * merge; }

    // `scale`: draw the dB scale in the column to the left of x. `rows`: the label rows under the bars (0, 1 or 2).
    void build(lv_obj_t* parent, const Dim& d, int px, int py, int pw, int ph, int merge_, bool scale, int rows) {
        x = px;
        y = py;
        w = pw;
        h = ph;
        merge = merge_;
        bars = merge == 1 ? kBands : (kBands - 1) / merge;
        zoned = g_zoned_spectrum;
        bar.assign(static_cast<std::size_t>(bars), nullptr);
        bar_amber.assign(static_cast<std::size_t>(bars), nullptr);
        bar_red.assign(static_cast<std::size_t>(bars), nullptr);
        tick.assign(static_cast<std::size_t>(bars), nullptr);
        bar_px.assign(static_cast<std::size_t>(bars), 0);
        tick_px.assign(static_cast<std::size_t>(bars), -1);
        bal.assign(static_cast<std::size_t>(bars), {});
        const float pitch = static_cast<float>(w) / static_cast<float>(bars);
        const int bar_w = std::max(2, static_cast<int>(std::lround(pitch * (merge == 1 ? 0.78F : 0.8F))));
        // The plot's floor, so the bars have something to stand on.
        rect(parent, x, y + h, w, 1, kFaint);
        for (int j = 0; j < bars; ++j) {
            const int bx = x + static_cast<int>(std::lround(static_cast<float>(j) * pitch + (pitch - static_cast<float>(bar_w)) / 2.0F));
            bar[static_cast<std::size_t>(j)] = rect(parent, bx, y + h, bar_w, 1, zoned ? kGreen : kAccent);
            lv_obj_set_style_bg_opa(bar[static_cast<std::size_t>(j)], LV_OPA_TRANSP, 0);
            if (zoned) {
                bar_amber[static_cast<std::size_t>(j)] = rect(parent, bx, y + h, bar_w, 1, kAmber);
                bar_red[static_cast<std::size_t>(j)] = rect(parent, bx, y + h, bar_w, 1, kAccent);
                lv_obj_set_style_bg_opa(bar_amber[static_cast<std::size_t>(j)], LV_OPA_TRANSP, 0);
                lv_obj_set_style_bg_opa(bar_red[static_cast<std::size_t>(j)], LV_OPA_TRANSP, 0);
            }
        }
        // The lines of the scale cross the bars, so that a bar is read against them.
        const int step = h >= 120 ? 10 : 20;
        for (float db = top_db; db >= floor_db - 0.01F; db -= static_cast<float>(step)) {
            const int ly = y + static_cast<int>(std::lround((1.0F - unit(db, floor_db, top_db)) * static_cast<float>(h)));
            if (db > floor_db + 0.01F) {
                rect(parent, x, std::min(ly, y + h - 1), w, 1, kBg, 120);
            }
            if (scale) {
                text(parent, db_text(db, 0), d.f_small, kMuted, x - d.scale_w - 2, ly - d.f_small / 2 - 2, d.scale_w, Align::kRight);
            }
        }
        for (int j = 0; j < bars; ++j) {
            const int bx = x + static_cast<int>(std::lround(static_cast<float>(j) * pitch + (pitch - static_cast<float>(bar_w)) / 2.0F));
            tick[static_cast<std::size_t>(j)] = rect(parent, bx, y + h, bar_w, 2, kText, 0);
        }
        if (rows > 0) {
            const int stride = merge == 1 ? 4 : merge == 2 ? 3 : 1;
            for (int j = 0; j < bars; j += (rows == 2 ? stride / 2 : stride)) {
                const int row = rows == 2 ? ((j / (stride / 2)) % 2) : 0;
                if (rows == 2 && stride / 2 == 0) {
                    break;
                }
                const int band = merge == 1 ? j : first_band(j) + merge / 2;
                const int cx = x + static_cast<int>(std::lround((static_cast<float>(j) + 0.5F) * pitch));
                text(parent, kBandLabels[band], d.f_small, kMuted, cx - 30, y + h + 3 + row * (d.f_small + 3), 60, Align::kCentre);
            }
        }
    }

    void update(const std::array<float, kBands>& db) {
        for (int j = 0; j < bars; ++j) {
            const auto idx = static_cast<std::size_t>(j);
            const float level = merge == 1 ? db[idx] : group_db(db, first_band(j), merge);
            bal[idx].update(level, kHopSeconds, 30.0F, 1.0F, 15.0F);
            const int bh = static_cast<int>(std::lround(unit(bal[idx].level, floor_db, top_db) * static_cast<float>(h)));
            if (bh != bar_px[idx] && !zoned) {
                bar_px[idx] = bh;
                lv_obj_set_style_bg_opa(bar[idx], bh > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                lv_obj_set_size(bar[idx], lv_obj_get_width(bar[idx]), std::max(bh, 1));
                lv_obj_set_y(bar[idx], y + h - std::max(bh, 1));
            } else if (bh != bar_px[idx]) {
                bar_px[idx] = bh;
                const int amber = static_cast<int>(std::lround(unit(kAmberAt, floor_db, top_db) * static_cast<float>(h)));
                const int red = static_cast<int>(std::lround(unit(kRedAt, floor_db, top_db) * static_cast<float>(h)));
                const int from[3] = {0, amber, red};
                const int to[3] = {std::min(bh, amber), std::min(bh, red), bh};
                lv_obj_t* const zone[3] = {bar[idx], bar_amber[idx], bar_red[idx]};
                for (int i = 0; i < 3; ++i) {
                    const int len = std::max(0, to[i] - from[i]);
                    lv_obj_set_style_bg_opa(zone[i], len > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                    if (len > 0) {
                        lv_obj_set_size(zone[i], lv_obj_get_width(zone[i]), len);
                        lv_obj_set_y(zone[i], y + h - from[i] - len);
                    }
                }
            }
            const int th = static_cast<int>(std::lround(unit(bal[idx].peak, floor_db, top_db) * static_cast<float>(h)));
            if (th != tick_px[idx]) {
                tick_px[idx] = th;
                lv_obj_set_style_bg_opa(tick[idx], th > 1 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                lv_obj_set_y(tick[idx], y + h - std::max(th, 1) - 2);
            }
        }
    }
};

// A level meter, horizontal or vertical: green, amber and red zones, a peak-hold tick and the numeric peak.
struct Meter {
    bool vertical = false;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float floor_db = -60.0F;
    float top_db = 0.0F;
    lv_obj_t* zone[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* tick = nullptr;
    lv_obj_t* value = nullptr;
    meters::Ballistic bal;
    int px_level = -1;
    int px_tick = -1;
    std::string shown;
    int decimals = 1;

    static constexpr float kAmberAt = -18.0F;
    static constexpr float kRedAt = -6.0F;

    void build(lv_obj_t* parent, int mx, int my, int mw, int mh, bool vert, lv_obj_t* value_label) {
        x = mx;
        y = my;
        w = mw;
        h = mh;
        vertical = vert;
        rect(parent, x, y, w, h, kTrough);
        const std::uint32_t zc[3] = {kGreen, kAmber, kAccent};
        for (int i = 0; i < 3; ++i) {
            zone[i] = rect(parent, x, y, 1, 1, zc[i]);
            lv_obj_set_style_bg_opa(zone[i], LV_OPA_TRANSP, 0);
        }
        tick = rect(parent, x, y, vert ? w : 2, vert ? 2 : h, kText, 0);
        value = value_label;
    }

    int span() const { return vertical ? h : w; }

    // Where the zones end, in pixels along the meter, for a level.
    void update(float level_db, float peak_db) {
        (void)level_db;
        bal.update(level_db, kHopSeconds, 24.0F, 1.5F, 10.0F);
        const int total = span();
        const int lit = static_cast<int>(std::lround(unit(bal.level, floor_db, top_db) * static_cast<float>(total)));
        if (lit != px_level) {
            px_level = lit;
            const int amber = static_cast<int>(std::lround(unit(kAmberAt, floor_db, top_db) * static_cast<float>(total)));
            const int red = static_cast<int>(std::lround(unit(kRedAt, floor_db, top_db) * static_cast<float>(total)));
            const int from[3] = {0, amber, red};
            const int to[3] = {std::min(lit, amber), std::min(lit, red), lit};
            for (int i = 0; i < 3; ++i) {
                const int len = std::max(0, to[i] - from[i]);
                lv_obj_set_style_bg_opa(zone[i], len > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
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
        const int tk = static_cast<int>(std::lround(unit(bal.peak, floor_db, top_db) * static_cast<float>(total)));
        if (tk != px_tick) {
            px_tick = tk;
            lv_obj_set_style_bg_opa(tick, tk > 1 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            if (vertical) {
                lv_obj_set_y(tick, y + h - std::max(tk, 2));
            } else {
                lv_obj_set_x(tick, x + std::max(tk, 2) - 2);
            }
        }
        (void)peak_db;
        if (value != nullptr) {
            const std::string s = db_text(bal.peak, decimals);
            if (s != shown) {
                shown = s;
                lv_label_set_text(value, s.c_str());
            }
        }
    }
};

// The scale under (or beside) a meter: 0, -6, -18, -60.
void meter_scale_h(lv_obj_t* parent, const Dim& d, int x, int y, int w) {
    for (const float db : {-60.0F, -40.0F, -20.0F, -10.0F, 0.0F}) {
        const int tx = x + static_cast<int>(std::lround(unit(db, -60.0F, 0.0F) * static_cast<float>(w)));
        text(parent, db_text(db, 0), d.f_small, kFaint, tx - 20, y, 40, Align::kCentre);
    }
}

// --- the views ----------------------------------------------------------------------------------------------------------

struct Scenario {
    const char* key;
    const char* layout;
    scene::Content content;
    nv::StreamFacts facts;
    const char* status;  // the status line's stream text
    int still;
};

struct Stage {
    lv_obj_t* parent = nullptr;
    Dim dim;
    const Room* room = nullptr;
    const Scenario* scenario = nullptr;
};

struct View {
    virtual ~View() = default;
    virtual void build(const Stage& stage) = 0;
    virtual void hop(const Hop& hop) = 0;
    // How many times a view that binds its bars to channels has changed a binding (for the views that do not: 0).
    [[nodiscard]] virtual int rebinds() const { return 0; }
};

float loudest(const Hop& hop, const Room& room) {
    float loud = meters::kSilentDb;
    for (std::size_t i = 0; i < room.spots.size(); ++i) {
        loud = std::max(loud, hop.peak_db[i]);
    }
    return loud;
}

// --- the screen -----------------------------------------------------------------------------------------------------------

struct Screen {
    lv_obj_t* stage = nullptr;
    lv_obj_t* header = nullptr;
    lv_obj_t* info = nullptr;
};

Screen make_screen(const Size& size) {
    Screen screen;
    lv_obj_t* const root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, color(kBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    const int pad = size.w >= 800 ? 32 : size.w >= 400 ? 16 : 10;
    const int fpx = size.w >= 800 ? 28 : size.w >= 400 ? 18 : 14;
    screen.header = text(root, "", fpx, kText, pad, (size.header - fpx) / 2 - 3, size.w - 2 * pad);
    rect(root, 0, size.header - 2, size.w, 2, kFaint);
    screen.stage = plain(root);
    lv_obj_set_size(screen.stage, size.w, size.h - size.header - size.footer);
    lv_obj_set_pos(screen.stage, 0, size.header);
    lv_obj_t* const footer = rect(root, 0, size.h - size.footer, size.w, size.footer, kSurface);
    const int fb = size.w >= 800 ? 20 : 13;
    text(footer, "hearth-4f2a91", fb, kMuted, pad, (size.footer - fb) / 2 - 3, 200);
    const int bw = size.w >= 800 ? 160 : size.w >= 400 ? 100 : 82;
    const int bh = 48;
    const int by = (size.footer - bh) / 2;
    lv_obj_t* const adjust = rect(footer, size.w - pad - bw, by, bw, bh, kSurfaceUp, 255, 4);
    lv_obj_center(text(adjust, "Adjust", fb, kText, 0, 0, bw, Align::kCentre));
    lv_obj_t* const prefs = rect(footer, size.w - pad - 2 * bw - 12, by, bw, bh, kSurfaceUp, 255, 4);
    lv_obj_center(text(prefs, "Preferences", fb, kText, 0, 0, bw, Align::kCentre));
    if (size.w >= 400) {
        screen.info = nullptr;
    }
    return screen;
}

// --- running ---------------------------------------------------------------------------------------------------------

struct Counters {
    std::uint64_t pixels = 0;
    std::uint32_t areas = 0;
    std::uint32_t ticks = 0;
    std::uint32_t drawn_ticks = 0;
    double render_us = 0.0;
    double update_us = 0.0;
};

using Clock = std::chrono::steady_clock;

// One hop: the view is told, then LVGL runs two frames of the panel's loop (a 40 Hz loop; about 30 on the board).
void run_hop(View& view, const Hop& hop, Counters* counters) {
    const auto a = Clock::now();
    view.hop(hop);
    const auto b = Clock::now();
    if (counters != nullptr) {
        counters->update_us += std::chrono::duration<double, std::micro>(b - a).count();
    }
    for (int tick = 0; tick < 2; ++tick) {
        g_now_us += kHopUs / 2;
        const auto c = Clock::now();
        const std::uint64_t before_px = g_flushed_pixels;
        lv_timer_handler();
        const auto e = Clock::now();
        if (counters != nullptr) {
            ++counters->ticks;
            counters->drawn_ticks += g_flushed_pixels != before_px ? 1U : 0U;
            counters->render_us += std::chrono::duration<double, std::micro>(e - c).count();
        }
    }
}

std::size_t count_objects(lv_obj_t* object) {
    std::size_t n = 1;
    const std::uint32_t children = lv_obj_get_child_count(object);
    for (std::uint32_t i = 0; i < children; ++i) {
        n += count_objects(lv_obj_get_child(object, static_cast<std::int32_t>(i)));
    }
    return n;
}

std::size_t heap_used() {
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    return monitor.total_size - monitor.free_size;
}

}  // namespace
