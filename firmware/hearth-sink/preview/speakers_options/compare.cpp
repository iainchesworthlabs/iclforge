// The speakers view before and after planning/p4x-speakers-options.md: the old glow, the responsive glow and the pulse
// style over a synthetic music scene (scene.hpp). The new views are the real now_screen.cpp. (The earlier options
// prototype is in the side-session notes.)
// Prototypes of ways the no-lyrics screen could show the room (see CMakeLists.txt). Each option is an object tree
// on the same 1024 x 472 stage the speakers view has, brought up to date from the windows of a synthetic scene
// (scene.hpp) at its own rate, drawn by LVGL's software renderer into the panel's RGB565 buffer; what the panel's
// flush callback is given is counted, which is the exact part of what a view costs.
//
//   speakers_options <out> [measure]
//     stills and short sequences of every option in three rooms, as BMPs in <out>; with "measure", a minute of
//     each option in each room: pixels flushed, redraws, LVGL objects and heap, written as <out>/measure.csv.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"

#include "iclforge/now_view.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"
#include "panel/lvgl/board_info.hpp"
#include "panel/lvgl/fonts.hpp"
#include "panel/lvgl/now_screen.hpp"

#include "scene.hpp"

namespace fs = std::filesystem;
namespace rp = iclforge::render;

namespace {

constexpr int kWidth = 1024;
constexpr int kHeight = 600;
constexpr int kHeaderHeight = 64;
constexpr int kFooterHeight = 64;
constexpr int kStageHeight = kHeight - kHeaderHeight - kFooterHeight;
constexpr int kBufferRows = 40;
constexpr std::uint64_t kTickUs = 100'000 / 3;  // the panel's own loop: about 30 a second, three to a window

constexpr std::uint32_t kBg = 0x151313;
constexpr std::uint32_t kSurface = 0x1D1A1A;
constexpr std::uint32_t kSurfaceUp = 0x272423;
constexpr std::uint32_t kText = 0xECE7E5;
constexpr std::uint32_t kAccent = 0xFF5C40;
constexpr std::uint32_t kMuted = 0x8B8583;
constexpr std::uint32_t kFaint = 0x4B4645;

std::uint64_t g_now_us = 0;
std::uint64_t g_flushed_pixels = 0;
std::uint32_t g_flushed_areas = 0;
std::vector<std::uint16_t> g_frame(static_cast<std::size_t>(kWidth) * kHeight, 0);

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

player::panel::FontFile load_font(const char* name) {
    static std::vector<std::vector<std::uint8_t>> keep;
    keep.push_back(read_file(fs::path(PREVIEW_FONT_DIR) / name));
    return {keep.back().data(), keep.back().size()};
}

void flush(lv_display_t* disp, const lv_area_t* area, std::uint8_t* px_map) {
    const int width = lv_area_get_width(area);
    g_flushed_pixels += static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(lv_area_get_height(area));
    ++g_flushed_areas;
    const auto* src = reinterpret_cast<const std::uint16_t*>(px_map);
    for (int y = area->y1; y <= area->y2; ++y) {
        std::copy(src, src + width, g_frame.begin() + static_cast<std::ptrdiff_t>(y) * kWidth + area->x1);
        src += width;
    }
    lv_display_flush_ready(disp);
}

// One 24-bit BMP, bottom row first; with `half` the picture is averaged down to 512 x 300 (a sequence's frames).
void write_bmp(const fs::path& path, bool half = false) {
    const int out_w = half ? kWidth / 2 : kWidth;
    const int out_h = half ? kHeight / 2 : kHeight;
    const std::size_t row_bytes = (static_cast<std::size_t>(out_w) * 3 + 3) / 4 * 4;
    const std::size_t image = row_bytes * static_cast<std::size_t>(out_h);
    std::vector<std::uint8_t> out(54 + image, 0);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
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
                    const std::uint16_t p = g_frame[static_cast<std::size_t>(y * span + dy) * kWidth + static_cast<std::size_t>(x * span + dx)];
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

lv_font_t* g_font_label = nullptr;   // Archivo Regular 24: a speaker's name
lv_font_t* g_font_header = nullptr;  // Archivo Regular 28: the header

// --- small drawing helpers -------------------------------------------------------------------------------------

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

float unit(float db, float floor_db, float top_db) { return std::clamp((db - floor_db) / (top_db - floor_db), 0.0F, 1.0F); }

// The level a programme is at, as the loudest recent window of any output: rises at once and falls slowly, so that
// a view can show levels against it ("2 dB under the loudest it has been") and react the same at any volume.
struct Ceiling {
    float db = -40.0F;
    void feed(float loudest_db, float seconds) { db = std::max(loudest_db, db - 1.5F * seconds); }
    // 0 at `span` dB under the ceiling, 1 at `top` dB under it.
    float rel(float level_db, float span, float top) const { return unit(level_db, db - span, db - top); }
};

std::uint32_t mix_rgb(std::uint32_t a, std::uint32_t b, float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    const auto ch = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<std::uint32_t>(x + (y - x) * t + 0.5F) & 0xFFU;
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
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

// A box of w x h centred on (cx, cy).
lv_obj_t* box_at(lv_obj_t* parent, int cx, int cy, int w, int h, int radius, std::uint32_t rgb, int opa) {
    lv_obj_t* const object = plain(parent);
    lv_obj_set_size(object, w, h);
    lv_obj_set_pos(object, cx - w / 2, cy - h / 2);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_bg_color(object, color(rgb), 0);
    lv_obj_set_style_bg_opa(object, static_cast<lv_opa_t>(std::clamp(opa, 0, 255)), 0);
    return object;
}

void centre_on(lv_obj_t* object, int cx, int cy, int w, int h) {
    lv_obj_set_size(object, w, h);
    lv_obj_set_pos(object, cx - w / 2, cy - h / 2);
}

void set_opa(lv_obj_t* object, int opa) { lv_obj_set_style_bg_opa(object, static_cast<lv_opa_t>(std::clamp(opa, 0, 255)), 0); }

void set_ring(lv_obj_t* object, std::uint32_t rgb, int width, int opa) {
    lv_obj_set_style_border_width(object, width, 0);
    lv_obj_set_style_border_color(object, color(rgb), 0);
    lv_obj_set_style_border_opa(object, static_cast<lv_opa_t>(std::clamp(opa, 0, 255)), 0);
}

lv_obj_t* text_label(lv_obj_t* parent, const char* text, const lv_font_t* font, std::uint32_t rgb) {
    lv_obj_t* const label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color(rgb), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    return label;
}

// --- the room: a layout, where the plan puts its speakers, and how big they are drawn ------------------------------

struct Room {
    std::string layout;
    std::vector<rp::PlanSpot> spots;
    std::vector<rp::PlanNode> nodes;
    // How much larger than the plan's own sizes a speaker is drawn: a room of two has room to be bigger.
    double scale = 1.0;
    std::size_t speakers = 0;
    // The distance from each node to its nearest neighbour, pixels.
    std::vector<double> nearest;

    int size_of(const rp::PlanNode& node) const { return static_cast<int>(std::lround(node.w * scale)); }
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
    room.nodes = rp::plan_nodes(room.spots, rp::PlanArea{.width = kWidth, .height = kStageHeight});
    for (std::size_t i = 0; i < room.spots.size(); ++i) {
        room.speakers += (room.nodes[i].shape == rp::PlanShape::kEar || room.nodes[i].shape == rp::PlanShape::kHeight) ? 1 : 0;
    }
    room.scale = room.speakers <= 2 ? 2.3 : room.speakers <= 6 ? 1.2 : 1.0;
    room.nearest.assign(room.nodes.size(), 1e9);
    for (std::size_t a = 0; a < room.nodes.size(); ++a) {
        if (room.nodes[a].shape == rp::PlanShape::kNone) {
            continue;
        }
        for (std::size_t b = 0; b < room.nodes.size(); ++b) {
            if (a == b || room.nodes[b].shape == rp::PlanShape::kNone) {
                continue;
            }
            room.nearest[a] = std::min(room.nearest[a], std::hypot(room.nodes[a].x - room.nodes[b].x, room.nodes[a].y - room.nodes[b].y));
        }
        if (room.nearest[a] > 1e8) {
            room.nearest[a] = 400.0;
        }
    }
    return room;
}

struct Ctx {
    lv_obj_t* stage = nullptr;
    const Room* room = nullptr;
    scene::Content content = scene::Content::kBed;
};

// An option: what it builds, and how it follows the music. window() is called when a 100 ms window closes
// (what the board would publish), tick() every frame of the panel's loop.
struct Option {
    virtual ~Option() = default;
    virtual void build(const Ctx& ctx) = 0;
    virtual void window(const scene::Window& win) = 0;
    virtual void tick(std::uint64_t now_us) { (void)now_us; }
};

// The levels the speakers view reads from the player, and the settings it follows: the harness's own, set by the
// views below (the real now_screen.cpp asks for them through player::panel::read_now_levels and
// read_now_settings, defined at the end of this file).
player::panel::NowLevels g_levels;
bool g_have_levels = false;
player::panel::NowSettings g_settings;

// --- the view as it was: the glow of 2026-10-21 to 25, before planning/p4x-speakers-options.md ------------------------------
// The same code as now_screen.cpp had (absolute thresholds, five core steps, 14 dB a second), reading the 5 Hz copy
// of the status: every other window.

namespace old {

inline constexpr int kGlowCoreSteps = 5;
inline constexpr int kGlowHaloSteps = 3;
inline constexpr float kGlowSilentDb = -80.0F;
inline constexpr float kGlowRmsFloorDb = -62.0F;
inline constexpr float kGlowRmsTopDb = -14.0F;
inline constexpr float kGlowPeakFloorDb = -50.0F;
inline constexpr float kGlowPeakTopDb = -8.0F;
inline constexpr float kGlowRmsFallDbPerSecond = 14.0F;
inline constexpr float kGlowPeakFallDbPerSecond = 30.0F;

class Glow {
   public:
    bool update(float rms_db, float peak_db, float seconds) {
        seconds = std::max(seconds, 0.0F);
        rms_ = std::max(rms_db, rms_ - kGlowRmsFallDbPerSecond * seconds);
        peak_ = std::max(peak_db, peak_ - kGlowPeakFallDbPerSecond * seconds);
        const int core = rms_ > kGlowSilentDb ? 1 + hold(core_ - 1, unit(rms_, kGlowRmsFloorDb, kGlowRmsTopDb) * (kGlowCoreSteps - 1)) : 0;
        const int halo = rms_ > kGlowSilentDb ? hold(halo_, unit(peak_, kGlowPeakFloorDb, kGlowPeakTopDb) * kGlowHaloSteps) : 0;
        const bool changed = core != core_ || halo != halo_;
        core_ = core;
        halo_ = halo;
        return changed;
    }
    [[nodiscard]] int core() const { return std::max(core_, 0); }
    [[nodiscard]] int halo() const { return std::max(halo_, 0); }
    [[nodiscard]] bool live() const { return core_ > 0; }

   private:
    [[nodiscard]] static float unit(float db, float floor_db, float top_db) {
        return std::clamp((db - floor_db) / (top_db - floor_db), 0.0F, 1.0F);
    }
    [[nodiscard]] static int hold(int current, float position) {
        constexpr float kMargin = 0.25F;
        if (current >= 0 && position > static_cast<float>(current) - 0.5F - kMargin &&
            position < static_cast<float>(current) + 0.5F + kMargin) {
            return current;
        }
        return static_cast<int>(std::lround(position));
    }
    float rms_ = -120.0F;
    float peak_ = -120.0F;
    int core_ = -1;
    int halo_ = -1;
};

}  // namespace old

struct OldView final : Option {
    struct Node {
        lv_obj_t* halo = nullptr;
        lv_obj_t* core = nullptr;
        lv_obj_t* label = nullptr;
        std::size_t slot = 0;
        old::Glow glow;
        int core_step = -1;
        int halo_step = -1;
    };
    std::vector<Node> nodes;
    scene::Window latest;
    std::uint64_t updated_us = 0;

    void build(const Ctx& ctx) override {
        const Room& room = *ctx.room;
        nodes.clear();
        updated_us = 0;
        for (std::size_t slot = 0; slot < room.nodes.size(); ++slot) {
            const rp::PlanNode& plan = room.nodes[slot];
            if (plan.shape == rp::PlanShape::kNone) {
                continue;
            }
            const int radius = plan.shape == rp::PlanShape::kEar ? LV_RADIUS_CIRCLE : plan.shape == rp::PlanShape::kLfe ? 14 : 10;
            Node node;
            node.slot = slot;
            node.halo = box_at(ctx.stage, plan.x, plan.y, plan.w + 24, plan.h + 24, plan.shape == rp::PlanShape::kEar ? LV_RADIUS_CIRCLE : radius + 8, kAccent, 0);
            node.core = box_at(ctx.stage, plan.x, plan.y, plan.w, plan.h, radius, kAccent, 0);
            set_ring(node.core, kFaint, 2, 255);
            node.label = text_label(node.core, room.spots[slot].name.c_str(), g_font_label, kFaint);
            lv_obj_center(node.label);
            nodes.push_back(node);
        }
    }
    void window(const scene::Window& win) override { latest = win; }
    void tick(std::uint64_t now_us) override {
        if (updated_us != 0 && now_us - updated_us < 200'000) {
            return;
        }
        const float seconds = updated_us == 0 ? 0.2F : static_cast<float>(now_us - updated_us) / 1e6F;
        updated_us = now_us;
        for (Node& node : nodes) {
            const float rms = node.slot < static_cast<std::size_t>(latest.outputs) ? latest.rms_db[node.slot] : -120.0F;
            const float peak = node.slot < static_cast<std::size_t>(latest.outputs) ? latest.peak_db[node.slot] : -120.0F;
            const bool changed = node.glow.update(rms, peak, seconds);
            if (!changed && node.core_step >= 0) {
                continue;
            }
            const int core = node.glow.core();
            const int halo = node.glow.halo();
            if (core != node.core_step) {
                node.core_step = core;
                const bool live = node.glow.live();
                set_opa(node.core, live ? 28 + (140 - 28) * (core - 1) / (old::kGlowCoreSteps - 1) : 0);
                set_ring(node.core, live ? kAccent : kFaint, 2, live ? 180 : 255);
                lv_obj_set_style_text_color(node.label, color(live ? kText : kFaint), 0);
            }
            if (halo != node.halo_step) {
                node.halo_step = halo;
                set_opa(node.halo, halo == 0 ? 0 : 14 + 14 * halo);
            }
        }
    }
};

// --- the view as built now: the real now_screen.cpp, in a style ---------------------------------------------------------------

struct NewView final : Option {
    player::panel::NowPlaying now;
    player::panel::NowStyle style;
    iclforge::nowview::SpeakersStyle speakers_style;
    explicit NewView(iclforge::nowview::SpeakersStyle chosen) : speakers_style(chosen) {}
    void build(const Ctx& ctx) override {
        style.text = kText;
        style.accent = kAccent;
        style.muted = kMuted;
        style.faint = kFaint;
        style.label = g_font_label;
        now = {};
        now.spots.assign(ctx.room->spots.begin(), ctx.room->spots.end());
        g_have_levels = false;
        g_levels = {};
        g_settings.speakers_style = speakers_style;
        player::panel::now_build_speakers(ctx.stage, kWidth, kStageHeight, now, style);
    }
    void window(const scene::Window& win) override {
        g_levels.outputs = static_cast<std::size_t>(win.outputs);
        for (int o = 0; o < win.outputs; ++o) {
            g_levels.peak_db[static_cast<std::size_t>(o)] = win.peak_db[static_cast<std::size_t>(o)];
            g_levels.rms_db[static_cast<std::size_t>(o)] = win.rms_db[static_cast<std::size_t>(o)];
        }
        ++g_levels.serial;
        g_have_levels = true;
    }
    void tick(std::uint64_t now_us) override { player::panel::now_update_speakers(now, now_us); }
    ~NewView() override {
        player::panel::now_forget();
        g_have_levels = false;
    }
};

// --- the screen around the stage ------------------------------------------------------------------------------------------------

struct Screen {
    lv_obj_t* stage = nullptr;
    lv_obj_t* header = nullptr;
    lv_obj_t* info = nullptr;
};

Screen make_screen() {
    Screen screen;
    lv_obj_t* const root = lv_screen_active();
    lv_obj_set_style_bg_color(root, color(kBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    screen.header = text_label(root, "", g_font_header, kText);
    lv_obj_set_pos(screen.header, 32, 16);
    lv_obj_set_size(screen.header, 760, 36);
    lv_obj_t* const rule = plain(root);
    lv_obj_set_size(rule, kWidth, 2);
    lv_obj_set_pos(rule, 0, kHeaderHeight - 2);
    lv_obj_set_style_bg_color(rule, color(kFaint), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    screen.stage = plain(root);
    lv_obj_set_size(screen.stage, kWidth, kStageHeight);
    lv_obj_set_pos(screen.stage, 0, kHeaderHeight);
    lv_obj_t* const footer = plain(root);
    lv_obj_set_size(footer, kWidth, kFooterHeight);
    lv_obj_set_pos(footer, 0, kHeight - kFooterHeight);
    lv_obj_set_style_bg_color(footer, color(kSurface), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_t* const name = text_label(footer, "hearth-4f2a91", g_font_label, kMuted);
    lv_obj_set_pos(name, 32, 18);
    screen.info = text_label(footer, "", g_font_label, kMuted);
    lv_obj_set_size(screen.info, 540, 30);
    lv_obj_set_pos(screen.info, 250, 18);
    lv_obj_t* const adjust = plain(footer);
    lv_obj_set_size(adjust, 160, 48);
    lv_obj_set_pos(adjust, 832, 8);
    lv_obj_set_style_radius(adjust, 4, 0);
    lv_obj_set_style_bg_color(adjust, color(kSurfaceUp), 0);
    lv_obj_set_style_bg_opa(adjust, LV_OPA_COVER, 0);
    lv_obj_t* const adjust_label = text_label(adjust, "Adjust", g_font_label, kText);
    lv_obj_center(adjust_label);
    return screen;
}

// --- running -----------------------------------------------------------------------------------------------------------------------

struct Counters {
    std::uint64_t pixels = 0;
    std::uint32_t areas = 0;
    std::uint32_t ticks = 0;
    std::uint32_t drawn_ticks = 0;
    double render_us = 0.0;
    double update_us = 0.0;
};

using Clock = std::chrono::steady_clock;

// One window of the scene: the option is told, then three frames of the panel's loop run.
void run_window(Option& option, const scene::Window& win, Counters* counters) {
    option.window(win);
    for (int tick = 0; tick < 3; ++tick) {
        g_now_us += kTickUs;
        const auto a = Clock::now();
        option.tick(g_now_us);
        const auto b = Clock::now();
        const std::uint64_t before_px = g_flushed_pixels;
        lv_timer_handler();
        const auto c = Clock::now();
        if (counters != nullptr) {
            ++counters->ticks;
            counters->drawn_ticks += g_flushed_pixels != before_px ? 1U : 0U;
            counters->update_us += std::chrono::duration<double, std::micro>(b - a).count();
            counters->render_us += std::chrono::duration<double, std::micro>(c - b).count();
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

struct Maker {
    const char* key;
    const char* title;
    std::function<std::unique_ptr<Option>()> make;
};

struct SceneDef {
    const char* key;
    const char* layout;
    scene::Content content;
    iclforge::nowview::StreamFacts facts;
    const char* source;
    // Where the stills are taken, as a window of the scene.
    int still;
};

}  // namespace

namespace player::panel {
bool read_now_levels(NowLevels& out) {
    if (!g_have_levels) {
        return false;
    }
    out = g_levels;
    return true;
}
NowSettings read_now_settings() { return g_settings; }
FontFile archivo_regular() {
    static const FontFile font = load_font("Archivo-Regular.ttf");
    return font;
}
FontFile archivo_semibold() {
    static const FontFile font = load_font("Archivo-SemiBold.ttf");
    return font;
}
}  // namespace player::panel

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path("speakers-options");
    const bool measure = argc > 2 && std::string(argv[2]) == "measure";
    fs::create_directories(out);

    lv_init();
    lv_tick_set_cb([]() -> std::uint32_t { return static_cast<std::uint32_t>(g_now_us / 1000); });
    lv_display_t* const disp = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static std::uint16_t first[kWidth * kBufferRows];
    static std::uint16_t second[kWidth * kBufferRows];
    lv_display_set_buffers(disp, first, second, sizeof first, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, &flush);
    {
        const player::panel::FontFile regular = player::panel::archivo_regular();
        g_font_label = lv_tiny_ttf_create_data(regular.data, regular.size, 24);
        g_font_header = lv_tiny_ttf_create_data(regular.data, regular.size, 28);
    }
    Screen screen = make_screen();

    namespace nv = iclforge::nowview;
    const std::vector<SceneDef> scenes = {
        {"2-0", "2.0", scene::Content::kStereo, {.codec = nv::Codec::kPcm}, "Music Assistant", 163},
        {"5-1", "5.1", scene::Content::kBed, {.codec = nv::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true}, "", 163},
        {"7-1-4", "7.1.4", scene::Content::kBedAndObjects,
         {.codec = nv::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true, .objects = 7}, "", 292},
        {"9-1-6", "9.1.6", scene::Content::kBed, {.codec = nv::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true}, "", 163},
    };
    const std::vector<Maker> options = {
        {"0-old", "Glow, as it was", [] { return std::make_unique<OldView>(); }},
        {"1-glow", "Glow, responsive", [] { return std::make_unique<NewView>(iclforge::nowview::SpeakersStyle::kGlow); }},
        {"2-pulse", "Pulse", [] { return std::make_unique<NewView>(iclforge::nowview::SpeakersStyle::kPulse); }},
    };

    std::ofstream csv;
    if (measure) {
        csv.open(out / "measure.csv");
        csv << "scene,option,seconds,pixels_per_s,areas_per_s,drawn_ticks_pct,render_us_per_tick,update_us_per_tick,objects,heap_bytes\n";
    }
    constexpr int kWarm = 60;
    constexpr int kSeq = 8;
    constexpr int kGif = 50;
    for (const SceneDef& def : scenes) {
        const Room room = make_room(def.layout);
        // The scene is rendered once: every option sees the same music.
        const int windows = measure ? 600 : def.still + kGif + 4;
        std::vector<scene::Window> track;
        {
            auto scene_audio = std::make_unique<scene::Scene>(room.spots, def.content);
            track.reserve(static_cast<std::size_t>(windows));
            for (int i = 0; i < windows; ++i) {
                track.push_back(scene_audio->next());
            }
        }
        if (!measure) {
            for (int w : {def.still - 2, def.still - 1, def.still, def.still + 1, def.still + 2}) {
                const scene::Window& win = track[static_cast<std::size_t>(w)];
                std::printf("%s w%d:", def.key, w);
                for (int o = 0; o < win.outputs; ++o) {
                    std::printf(" %s %.0f/%.0f[%.0f %.0f %.0f]", room.spots[static_cast<std::size_t>(o)].name.c_str(), win.peak_db[o], win.rms_db[o],
                                win.low_db[o], win.mid_db[o], win.high_db[o]);
                }
                std::printf(" | objs %d\n", win.objects);
            }
        }
        const std::string heading = nv::heading(def.facts, def.layout, room.spots.size());
        lv_label_set_text(screen.header, heading.c_str());
        lv_label_set_text(screen.info, def.source[0] != '\0' ? "PCM from Music Assistant" : "Decoded on this board - 768 kb/s");
        for (const Maker& maker : options) {
            lv_obj_clean(screen.stage);
            g_now_us += 1'000'000;
            const std::size_t heap_before = heap_used();
            std::unique_ptr<Option> option = maker.make();
            option->build(Ctx{screen.stage, &room, def.content});
            lv_obj_update_layout(screen.stage);
            if (!measure) {
                // Warm up so that the glow and the trails have settled, then the still and a sequence.
                const int start = def.still - kWarm;
                for (int w = start; w < def.still; ++w) {
                    run_window(*option, track[static_cast<std::size_t>(w)], nullptr);
                }
                run_window(*option, track[static_cast<std::size_t>(def.still)], nullptr);
                const std::string stem = std::string(def.key) + "_" + maker.key;
                write_bmp(out / ("still_" + stem + ".bmp"));
                for (int w = 1; w <= kGif; ++w) {
                    run_window(*option, track[static_cast<std::size_t>(def.still + w)], nullptr);
                    char name[64];
                    std::snprintf(name, sizeof name, "seq_%s_%02d.bmp", stem.c_str(), w);
                    write_bmp(out / name, true);
                }
            } else {
                for (int w = 0; w < kWarm; ++w) {
                    run_window(*option, track[static_cast<std::size_t>(w)], nullptr);
                }
                g_flushed_pixels = 0;
                g_flushed_areas = 0;
                Counters counters;
                for (int w = kWarm; w < windows; ++w) {
                    run_window(*option, track[static_cast<std::size_t>(w)], &counters);
                }
                const double seconds = static_cast<double>(windows - kWarm) / 10.0;
                const std::size_t heap_after = heap_used();
                const std::size_t objects = count_objects(screen.stage);
                char line[512];
                std::snprintf(line, sizeof line, "%s,%s,%.1f,%.0f,%.1f,%.1f,%.1f,%.2f,%zu,%zu\n", def.key, maker.key, seconds,
                              static_cast<double>(g_flushed_pixels) / seconds, static_cast<double>(g_flushed_areas) / seconds,
                              100.0 * counters.drawn_ticks / std::max<std::uint32_t>(1, counters.ticks),
                              counters.render_us / std::max<std::uint32_t>(1, counters.ticks),
                              counters.update_us / std::max<std::uint32_t>(1, counters.ticks), objects, heap_after - heap_before);
                csv << line;
                std::fputs(line, stdout);
            }
            option.reset();
        }
    }
    return 0;
}
