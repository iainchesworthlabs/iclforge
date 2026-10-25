// The lyric screen on a computer (see CMakeLists.txt): lyric_screen.cpp, as the board runs it, fed a
// document and a position instead of the Sendspin player, drawn by LVGL's software renderer into a
// 1024 by 600 buffer and written as one 24-bit BMP a scenario. The scenarios are the screens of
// planning/karaoke-p4x-panel.md, "The screens", S1 to S9.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lvgl.h"

#include "iclforge/board_variants.hpp"
#include "iclforge/dev_view.hpp"
#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/preferences.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"
#include "iclforge/title_card.hpp"
#include "iclforge/wall_clock.hpp"
#include "panel/lvgl/board_info.hpp"
#include "panel/lvgl/dev_info.hpp"
#include "panel/lvgl/fonts.hpp"
#include "panel/lvgl/lyric_screen.hpp"
#include "panel/lvgl/now_screen.hpp"
#include "panel/lvgl/prefs_screen.hpp"
#include "panel/lyrics_reader.hpp"

namespace fs = std::filesystem;

namespace {

// The display the screen is drawn on: the kit's 1024 by 600 for the screens of planning/karaoke-p4x-panel.md, and
// then each size the Adjust and Preferences sheets are checked at (open_display()).
int g_width = 1024;
int g_height = 600;
constexpr int kMaxWidth = 1024;
constexpr int kBufferRows = 40;

// --- the preview's player: one document, one position, one clock.
iclforge::lyrics::Lyrics g_doc;
bool g_held = false;
std::optional<std::int64_t> g_position;
std::int64_t g_item = 1;
std::int64_t g_now_us = 0;
player::panel::BoardInfo g_board;
// What the developer view reads, and the console under it.
iclforge::devview::Facts g_dev;
std::string g_log;
// The no-lyrics screen's data: what the player says is playing, and what the settings say to show.
player::panel::NowPlaying g_now;
bool g_now_playing = false;
player::panel::NowSettings g_settings;
int g_backlight = 80;
// What the server says is playing (metadata@v1): the player's store, and where the song is on the clock.
iclforge::nowview::TitleStore g_title;
bool g_title_held = false;
std::optional<std::int64_t> g_title_position;
// The clock view's time (what the board's wall clock would say), and what LVGL has flushed so far.
iclforge::wallclock::Reading g_wall;
std::uint64_t g_flushed_pixels = 0;
std::uint32_t g_flushed_areas = 0;

std::vector<std::uint16_t> g_frame;
lv_indev_t* g_pointer_device = nullptr;
std::uint16_t g_buffer_a[kMaxWidth * kBufferRows];
std::uint16_t g_buffer_b[kMaxWidth * kBufferRows];
// The board the preferences are drawn for (iclforge/board_variants.hpp): the kit, until a scenario says another.
iclforge::BoardCaps g_caps = iclforge::variants::p4_rev3_kit();

struct Pointer {
    int x = 0;
    int y = 0;
    bool down = false;
} g_pointer;

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string read_text(const fs::path& path) {
    const auto bytes = read_file(path);
    return {bytes.begin(), bytes.end()};
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
        std::copy(src, src + width, g_frame.begin() + static_cast<std::ptrdiff_t>(y) * g_width + area->x1);
        src += width;
    }
    lv_display_flush_ready(disp);
}

void read_pointer(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = g_pointer.x;
    data->point.y = g_pointer.y;
    data->state = g_pointer.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// One 24-bit BMP, bottom row first.
void write_bmp(const fs::path& path) {
    const std::size_t row_bytes = (static_cast<std::size_t>(g_width) * 3 + 3) / 4 * 4;
    const std::size_t image = row_bytes * g_height;
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
    put(18, g_width);
    put(22, g_height);
    out[26] = 1;
    out[28] = 24;
    put(34, static_cast<std::uint32_t>(image));
    for (int y = 0; y < g_height; ++y) {
        std::uint8_t* row = out.data() + 54 + static_cast<std::size_t>(g_height - 1 - y) * row_bytes;
        for (int x = 0; x < g_width; ++x) {
            const std::uint16_t p = g_frame[static_cast<std::size_t>(y) * g_width + x];
            const std::uint8_t r5 = (p >> 11) & 0x1F;
            const std::uint8_t g6 = (p >> 5) & 0x3F;
            const std::uint8_t b5 = p & 0x1F;
            *row++ = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
            *row++ = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
            *row++ = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
        }
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

// Frames of the screen's own loop: the clock moves 33 ms a frame, as the panel's task's does.
void pump(int frames) {
    for (int i = 0; i < frames; ++i) {
        g_now_us += 33'000;
        player::panel::screen_update();
        lv_timer_handler();
    }
}

void hold(const std::string& lrc, std::optional<std::int64_t> position_ms) {
    const auto parsed = iclforge::lyrics::parse_lyrics(lrc);
    if (!parsed) {
        std::fprintf(stderr, "scenario document: %s\n", parsed.error().c_str());
        std::exit(2);
    }
    g_doc = *parsed;
    g_held = true;
    g_position = position_ms;
    ++g_item;
}

void tap(int x, int y) {
    g_pointer = {x, y, true};
    pump(4);
    g_pointer.down = false;
    pump(4);
}

// A finger dragged from one height to another along a column, as a person scrolls a list.
void drag(int x, int y_from, int y_to) {
    g_pointer = {x, y_from, true};
    pump(2);
    constexpr int kSteps = 8;
    for (int i = 1; i <= kSteps; ++i) {
        g_pointer.y = y_from + (y_to - y_from) * i / kSteps;
        pump(1);
    }
    g_pointer.down = false;
    // Long enough for the list's throw and its elastic edge to come to rest.
    pump(45);
}

// The display, at a size: a frame to flush into, and the pointer attached to it. The one before it, if there is one,
// is deleted with the screen that was built on it.
lv_display_t* open_display(int width, int height, lv_display_t* previous) {
    g_width = width;
    g_height = height;
    g_frame.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    lv_display_t* const disp = lv_display_create(width, height);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_buffer_a, g_buffer_b, sizeof g_buffer_a, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, &flush);
    lv_display_set_default(disp);
    lv_indev_set_display(g_pointer_device, disp);
    if (previous != nullptr) {
        lv_display_delete(previous);
    }
    return disp;
}

// --- finding things on the screen the way a person does: by what they say ---------------------------------------

void collect_touch_targets(lv_obj_t* object, std::vector<lv_obj_t*>& out) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_has_flag(object, player::panel::kTouchTargetFlag)) {
        out.push_back(object);
    }
    const std::uint32_t count = lv_obj_get_child_count(object);
    for (std::uint32_t i = 0; i < count; ++i) {
        collect_touch_targets(lv_obj_get_child(object, static_cast<std::int32_t>(i)), out);
    }
}

std::string label_text(lv_obj_t* object) {
    return object != nullptr && lv_obj_check_type(object, &lv_label_class) ? std::string(lv_label_get_text(object))
                                                                           : std::string();
}

// What a button says: its label.
std::string button_text(lv_obj_t* button) { return label_text(lv_obj_get_child(button, 0)); }

lv_obj_t* find_button(const std::string& text) {
    std::vector<lv_obj_t*> targets;
    collect_touch_targets(lv_screen_active(), targets);
    for (lv_obj_t* const target : targets) {
        if (button_text(target) == text) {
            return target;
        }
    }
    return nullptr;
}

// Whether a label under `object` that is shown says `text` (or, with `contains`, has it in what it says).
bool shows_text(lv_obj_t* object, const std::string& text, bool contains = false) {
    if (object == nullptr || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    const std::string said = label_text(object);
    if (!said.empty() && (contains ? said.find(text) != std::string::npos : said == text)) {
        return true;
    }
    const std::uint32_t count = lv_obj_get_child_count(object);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (shows_text(lv_obj_get_child(object, static_cast<std::int32_t>(i)), text, contains)) {
            return true;
        }
    }
    return false;
}

// A tap on the middle of the button that says `text`, scrolled into view first if it is on a list.
bool tap_button(const std::string& text) {
    lv_obj_t* const button = find_button(text);
    if (button == nullptr) {
        std::printf("  no button \"%s\" is shown\n", text.c_str());
        return false;
    }
    lv_obj_scroll_to_view_recursive(button, LV_ANIM_OFF);
    lv_obj_update_layout(button);
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    tap((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
    return true;
}

// --- holding a sheet to what it promises ---------------------------------------------------------------------------

struct Rect {
    int x1;
    int y1;
    int x2;
    int y2;
};

Rect rect_of(lv_obj_t* object) {
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    return {area.x1, area.y1, area.x2, area.y2};
}

// Walks the sheet under `root` and fails the run for text that is cut off, a control that sticks out of what holds it
// or off the screen, a touch target smaller than a fingertip (iclforge::screen::kMinTargetPx), two of them
// overlapping or closer than iclforge::screen::kMinGapPx. A list that scrolls is checked in full, not only the part
// that is on the glass. Returns the number of failures, having said each.
int audit(lv_obj_t* root, const std::string& what) {
    if (root == nullptr) {
        std::printf("  FAIL %s: there is no sheet to look at\n", what.c_str());
        return 1;
    }
    lv_obj_update_layout(root);
    int failures = 0;
    const auto fail = [&](lv_obj_t* object, const char* why) {
        const Rect r = rect_of(object);
        std::string name = label_text(object);
        if (name.empty()) {
            name = button_text(object);
        }
        std::printf("  FAIL %s: \"%s\" %s (%d,%d)-(%d,%d)\n", what.c_str(), name.c_str(), why, r.x1, r.y1, r.x2, r.y2);
        ++failures;
    };
    std::vector<lv_obj_t*> targets;
    const auto walk = [&](auto&& self, lv_obj_t* parent, bool scrolling) -> void {
        const bool under_scroll = scrolling || lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
        const Rect p = rect_of(parent);
        const std::uint32_t count = lv_obj_get_child_count(parent);
        for (std::uint32_t i = 0; i < count; ++i) {
            lv_obj_t* const child = lv_obj_get_child(parent, static_cast<std::int32_t>(i));
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            const Rect r = rect_of(child);
            if (r.x1 < p.x1 || r.x2 > p.x2) {
                fail(child, "sticks out of what holds it, sideways");
            }
            if (!under_scroll && (r.y1 < p.y1 || r.y2 > p.y2)) {
                fail(child, "sticks out of what holds it, vertically");
            }
            if (!under_scroll && (r.x1 < 0 || r.y1 < 0 || r.x2 >= g_width || r.y2 >= g_height)) {
                fail(child, "is off the screen");
            }
            if (lv_obj_check_type(child, &lv_label_class) && !label_text(child).empty()) {
                const std::string text = label_text(child);
                const bool wraps = lv_label_get_long_mode(child) == LV_LABEL_LONG_MODE_WRAP;
                const std::int32_t width = lv_obj_get_content_width(child);
                lv_point_t size{0, 0};
                lv_text_get_size(&size, text.c_str(), lv_obj_get_style_text_font(child, LV_PART_MAIN),
                                 lv_obj_get_style_text_letter_space(child, LV_PART_MAIN),
                                 lv_obj_get_style_text_line_space(child, LV_PART_MAIN), wraps ? width : LV_COORD_MAX,
                                 LV_TEXT_FLAG_NONE);
                if (wraps ? size.y > lv_obj_get_content_height(child) + 1 : size.x > width + 1) {
                    fail(child, "is clipped: the text does not fit its label");
                }
            }
            if (lv_obj_has_flag(child, player::panel::kTouchTargetFlag)) {
                targets.push_back(child);
                const int w = r.x2 - r.x1 + 1;
                const int h = r.y2 - r.y1 + 1;
                if (std::min(w, h) < iclforge::screen::kMinTargetPx) {
                    fail(child, ("is a touch target of " + std::to_string(w) + " by " + std::to_string(h) +
                                 ", smaller than a fingertip")
                                    .c_str());
                }
            }
            self(self, child, under_scroll);
        }
    };
    walk(walk, root, false);
    int smallest = 1 << 20;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const Rect a = rect_of(targets[i]);
        smallest = std::min({smallest, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1});
        for (std::size_t j = i + 1; j < targets.size(); ++j) {
            const Rect b = rect_of(targets[j]);
            // The space between them along each axis, negative where they overlap on it.
            const int dx = std::max(a.x1, b.x1) - std::min(a.x2, b.x2) - 1;
            const int dy = std::max(a.y1, b.y1) - std::min(a.y2, b.y2) - 1;
            if (dx < 0 && dy < 0) {
                fail(targets[i], "overlaps another touch target");
            } else if ((dx < 0 && dy < iclforge::screen::kMinGapPx) || (dy < 0 && dx < iclforge::screen::kMinGapPx)) {
                fail(targets[i], "is closer to another touch target than the least gap");
            }
        }
    }
    std::printf("  %-26s %2zu touch targets, the smallest %d px, %d failures\n", what.c_str(), targets.size(),
                targets.empty() ? 0 : smallest, failures);
    return failures;
}

// One output's level over the last 100 ms, in dB of full scale: the peak and the RMS.
struct Level {
    float peak;
    float rms;
};

// A stream playing to the room `layout` names, with the decoder's findings and a level for every slot.
void play_room(const char* layout, const iclforge::nowview::StreamFacts& facts, const std::vector<Level>& levels,
               int kbps = 0, const char* server = "") {
    const auto parsed = iclforge::render::OutputLayout::parse(layout);
    if (!parsed) {
        std::fprintf(stderr, "scenario layout %s\n", layout);
        std::exit(2);
    }
    g_now = {};
    const auto spots = iclforge::render::plan_spots(*parsed);
    g_now.spots.assign(spots.begin(), spots.end());
    const std::string heading = iclforge::nowview::heading(facts, layout, spots.size());
    g_now.heading.assign(heading.data(), heading.size());
    g_now.facts = facts;
    g_now.kbps = kbps;
    g_board.server = server;
    for (const Level& level : levels) {
        g_now.peak_db.push_back(level.peak);
        g_now.rms_db.push_back(level.rms);
    }
    g_now_playing = true;
    g_board.stream = "bursts";
    g_held = false;
    g_position.reset();
}

// A music mix over a few rooms: sample levels, not measurements.
// The server says a song is playing (the specification's whole object), and where it is: the title card's
// and the speakers' header's data. An empty artist or album is not said.
void say_title(const char* title, const char* artist, const char* album, std::int32_t year,
               std::optional<std::int64_t> position_ms, std::int64_t length_ms) {
    namespace nv = iclforge::nowview;
    g_title.clear();
    nv::TitleUpdate update;
    update.whole = true;
    const auto text = [](const char* value) {
        return *value == '\0' ? nv::TitleUpdate::Text{} : nv::TitleUpdate::Text{.say = nv::Say::kSet, .value = value};
    };
    update.title = text(title);
    update.artist = text(artist);
    update.album = text(album);
    if (year != 0) {
        update.year = {.say = nv::Say::kSet, .value = year};
    }
    if (position_ms) {
        update.progress = {.say = nv::Say::kSet, .position_ms = *position_ms, .duration_ms = length_ms, .speed = 1000};
    }
    g_title.apply(update);
    g_title_held = true;
    g_title_position = position_ms;
}

void say_no_title() {
    g_title.clear();
    g_title_held = false;
    g_title_position.reset();
}

const std::vector<Level> kStereo = {{-9, -19}, {-11, -21}};
const std::vector<Level> kFiveOne = {{-10, -19}, {-14, -24}, {-11, -20}, {-28, -37}, {-26, -35}, {-16, -27}};
const std::vector<Level> kFiveOneNoCentre = {{-10, -19}, {-120, -120}, {-11, -20}, {-27, -36}, {-26, -35}, {-16, -27}};
const std::vector<Level> kSevenOneFour = {{-12, -21}, {-17, -27}, {-13, -22}, {-24, -33}, {-22, -31}, {-30, -40},
                                          {-31, -41}, {-36, -46}, {-35, -45}, {-42, -52}, {-44, -54}, {-18, -29}};
// 9.1.6: L C R Ls Rs Lrs Rrs Lw Rw, six heights, the LFE.
const std::vector<Level> kNineOneSix = {{-12, -21}, {-17, -27}, {-13, -22}, {-24, -33}, {-22, -31}, {-30, -40}, {-31, -41},
                                        {-26, -35}, {-27, -36}, {-36, -46}, {-35, -45}, {-38, -48}, {-42, -52}, {-44, -54},
                                        {-46, -56}, {-18, -29}};

constexpr iclforge::nowview::StreamFacts kDolbyFiveOne{.codec = iclforge::nowview::Codec::kEac3, .have_decoder = true,
                                                       .acmod = 7, .lfe = true, .objects = 0};

struct Scenario {
    const char* name;
    std::function<void()> set_up;
    // How far the clock moves before the picture, so that a glow settles on the levels it is given.
    std::int64_t settle_us = 1'000'000;
};

// A P4X kit with its cable in and its radio joined, a stream playing and nothing gone wrong: the
// developer view's first picture.
iclforge::devview::Facts healthy_board() {
    using namespace iclforge::devview;
    Facts f;
    f.active = "ethernet";
    f.preference = "auto";
    f.ethernet = {.present = true, .enabled = true, .link = true, .address = "192.168.1.163",
                  .mac = "e8:f6:0a:e7:53:fb", .speed_mbps = 100, .full_duplex = true};
    f.wifi = {.present = true, .enabled = true, .link = true, .address = "192.168.1.251",
              .mac = "e8:f6:0a:e7:53:f9", .configured = true, .ssid = "HomeNet", .rssi_dbm = -52};
    f.name = "hearth-e753f9";
    f.mdns = "hearth-e753f9.local";
    f.web_address = "192.168.1.163";
    f.profile = "p4-rev3";
    f.profile_hash = "ff9b27a6c4fd";
    f.firmware = "0.10.0-beta.1-5127-g2337af6d2";
    f.slot = "ota_0";
    f.idf = "v6.1.0";
    f.chip = "ESP32-P4";
    f.revision_major = 3;
    f.revision_minor = 2;
    f.cores = 2;
    f.cpu_mhz = 400;
    f.psram_mhz = 200;
    f.psram_bytes = 32ULL * 1024 * 1024;
    f.layout = "5.1";
    f.sink = "i2s-tdm";
    f.slots = 16;
    f.slot_bits = 32;
    f.uptime_ms = (3ULL * 3600 + 12 * 60) * 1000;
    f.reset_reason = "wdt";
    f.player = true;
    f.server = "Hearth on STUDIO-PC";
    f.connections = 1;
    f.paired = 2;
    f.clock_converged = true;
    f.clock_error_us = 120;
    f.stream = "bursts";
    f.bursts = 1234;
    f.error_us = 400;
    f.worst_error_us = 2100;
    f.burst_us = 410;
    f.worst_burst_us = 1120;
    f.peak_db = {-9.0F, -11.5F, -30.0F, -18.0F, -80.0F, -16.0F};
    f.internal_free = 100'762;
    f.internal_low = 62'464;
    f.internal_largest = 53'248;
    f.psram_free = 29'800'000;
    f.psram_low = 29'000'000;
    f.refreshes = 4200;
    f.render_us_mean = 6100;
    f.render_us_max = 21'000;
    f.touch_reads = 12'345;
    f.touch_points = 41;
    return f;
}

const char* const kHealthyLog =
    "I (11210) sendspin: server 'Hearth on STUDIO-PC' admitted, clock converged\n"
    "I (11420) sendspin: stream started: bursts, 5.1, 48000 Hz\n"
    "I (21460) panel: lyrics, 2150 refreshes, render 6100 us mean 21000 max, rebuild 3100 us max, 0 late\n"
    "I (31470) net: ethernet: link up, 100 Mb/s full duplex\n"
    "I (41480) panel: lyrics, 2290 refreshes, render 6100 us mean 21000 max, rebuild 3100 us max, 0 late\n";

// The Adjust and Preferences sheets at one display size (planning/p4x-preferences.md): drawn for the kit and for a
// board whose screen has no touch, held to a fingertip's size and to text that is not cut off (audit()), and driven by
// taps the way a person does. Returns the number of failures, having said each.
int check_sheets(int width, int height, const fs::path& out, const std::string& lrc, lv_display_t*& disp) {
    namespace nv = iclforge::nowview;
    const std::string size = std::to_string(width) + "x" + std::to_string(height);
    std::printf("-- %s\n", size.c_str());
    disp = open_display(width, height, disp);
    if (!player::panel::screen_init(disp, width, height)) {
        std::puts("  FAIL the screen did not come up");
        return 1;
    }
    int failures = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::printf("  FAIL %s: %s\n", size.c_str(), what.c_str());
            ++failures;
        }
    };
    const auto scroller_of = [](lv_obj_t* root) -> lv_obj_t* {
        const std::uint32_t count = lv_obj_get_child_count(root);
        for (std::uint32_t i = 0; i < count; ++i) {
            lv_obj_t* const child = lv_obj_get_child(root, static_cast<std::int32_t>(i));
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE)) {
                return child;
            }
        }
        return nullptr;
    };

    // A kit-like board whose screen is this size, playing a song with lyrics.
    g_caps = iclforge::variants::p4_rev3_kit();
    g_caps.display.width = width;
    g_caps.display.height = height;
    g_settings = {nv::View::kSpeakers, 5};
    g_board = {};
    g_board.name = "hearth-4f2a91";
    g_board.address = "192.168.1.50";
    g_board.network = "HomeNet";
    g_board.firmware = "0.11.0";
    g_board.player = true;
    g_board.stream = "bursts";
    g_now_playing = false;
    hold(lrc, 5'750);
    g_now_us += 1'000'000;
    pump(6);
    write_bmp(out / ("footer-" + size + ".bmp"));
    failures += audit(player::panel::screen_footer_object(), "footer " + size);
    // A screen wide enough for the kit's columns still says where in the song it is and the delay, beside the buttons.
    if (width >= 960) {
        expect(shows_text(player::panel::screen_footer_object(), "Line ", true), "the footer says which line is being sung");
        expect(shows_text(player::panel::screen_footer_object(), "Delay +0 ms"), "and the lyric delay");
    }

    // Adjust: the lyric delay and the text size, and nothing else.
    expect(tap_button("Adjust"), "the footer has an Adjust button");
    pump(6);
    lv_obj_t* const sheet = player::panel::screen_sheet_object();
    expect(sheet != nullptr, "Adjust opens its sheet");
    if (sheet != nullptr) {
        write_bmp(out / ("adjust-" + size + ".bmp"));
        const Rect r = rect_of(sheet);
        const int sheet_h = r.y2 - r.y1 + 1;
        std::printf("  the Adjust sheet is %d px tall\n", sheet_h);
        failures += audit(sheet, "adjust " + size);
        for (const char* gone : {"Speakers", "Title", "Clock", "Dark", "Off", "5 min", "30 min", "Never"}) {
            expect(find_button(gone) == nullptr, std::string("Adjust no longer has the ") + gone + " button");
        }
        for (const char* kept : {"-", "+", "Smaller", "Larger", "Close"}) {
            expect(find_button(kept) != nullptr, std::string("Adjust has a ") + kept + " button");
        }
        if (width == 1024 && height == 600) {
            expect(sheet_h < 352, "the sheet is shorter than the 352 px it was");
        }
        tap_button("+");
        pump(2);
        // "Delay +50 ms", or "+50 ms" on a screen too narrow for the longer between the two buttons.
        expect(shows_text(sheet, "+50 ms", true), "+ adds 50 ms to the lyric delay");
        tap_button("-");
        tap_button("-");
        pump(2);
        expect(shows_text(sheet, "-50 ms", true), "- takes 50 ms off the lyric delay");
        tap_button("+");
        tap_button("Larger");
        pump(2);
        expect(!shows_text(sheet, "Normal"), "Larger changes the text size");
        tap_button("Smaller");
        pump(2);
        expect(shows_text(sheet, "Normal"), "Smaller puts it back");
    }
    // Left alone, the sheet puts itself away.
    g_now_us += 9'000'000;
    pump(4);
    expect(player::panel::screen_sheet_object() == nullptr, "the Adjust sheet puts itself away after eight seconds");

    // Preferences, for the kit: everything is available.
    expect(tap_button("Preferences"), "the footer has a Preferences button");
    pump(8);
    lv_obj_t* const prefs = player::panel::prefs_root();
    expect(prefs != nullptr, "Preferences opens its sheet");
    if (prefs != nullptr) {
        write_bmp(out / ("prefs-" + size + ".bmp"));
        failures += audit(prefs, "preferences " + size);
        for (const char* shown : {"Screen without lyrics", "Dim after", "DISPLAY"}) {
            expect(shows_text(prefs, shown), std::string("Preferences shows ") + shown);
        }
        // The view is chosen by a tap, and the board follows at once.
        expect(tap_button("Clock"), "the view row has a Clock button");
        expect(g_settings.view == nv::View::kClock, "a tap on Clock chooses the clock view");
        expect(tap_button("Dark"), "the view row has a Dark button");
        expect(g_settings.view == nv::View::kDark, "a tap on Dark chooses the dark view");
        pump(4);
        write_bmp(out / ("prefs-dark-" + size + ".bmp"));
        expect(shows_text(prefs, "Not used by the Dark view", true), "the dim row says the dark view has no dim time");
        expect(tap_button("Speakers"), "the view row has a Speakers button");
        expect(g_settings.view == nv::View::kSpeakers, "a tap on Speakers chooses the speakers");
        pump(4);
        expect(!shows_text(prefs, "Not used by the Dark view", true), "and the note goes with it");
        // The dim time steps: 5 minutes, 15, back to 5, and down to the first stop, where - has nowhere to go.
        expect(shows_text(prefs, "5 min"), "the dim time starts at 5 minutes");
        tap_button("+");
        pump(4);
        expect(g_settings.dim_after_minutes == 15, "+ steps the dim time to 15 minutes");
        expect(shows_text(prefs, "15 min"), "and the row says so");
        tap_button("-");
        tap_button("-");
        tap_button("-");
        pump(4);
        expect(g_settings.dim_after_minutes == 1, "- steps down to 1 minute and stops there");
        tap_button("+");
        tap_button("+");
        pump(4);
        expect(g_settings.dim_after_minutes == 15, "and + steps back up");
        tap_button("-");
        pump(4);
        // A list that is longer than the glass scrolls under a finger, and a control scrolled to is still a control.
        lv_obj_t* const list = scroller_of(prefs);
        expect(list != nullptr, "Preferences has a list that scrolls");
        if (list != nullptr) {
            lv_obj_update_layout(list);
            const int beyond = lv_obj_get_scroll_bottom(list);
            std::printf("  the list runs %d px past the glass\n", beyond);
            if (beyond > 0) {
                drag(width / 2, height * 3 / 4, height / 4);
                std::printf("  scrolled to %d\n", static_cast<int>(lv_obj_get_scroll_y(list)));
                expect(lv_obj_get_scroll_y(list) > 0, "a finger dragged up scrolls the list");
                write_bmp(out / ("prefs-scrolled-" + size + ".bmp"));
                drag(width / 2, height / 4, height * 3 / 4);
                std::printf("  scrolled back to %d\n", static_cast<int>(lv_obj_get_scroll_y(list)));
                expect(lv_obj_get_scroll_y(list) == 0, "and dragged down scrolls it back");
            }
        }
        expect(tap_button("Off"), "a control past the first screenful is reached");
        expect(g_settings.view == nv::View::kOff, "and works");
        tap_button("Speakers");
        tap_button("Close");
        pump(6);
        expect(player::panel::prefs_root() == nullptr, "Close puts the Preferences sheet away");
    }

    // Dimming and Preferences: the tap that wakes a dimmed screen does not also open Preferences, the sheet keeps the
    // screen lit while it is open, and the dim time counts again from when it is put away.
    {
        g_settings = {nv::View::kSpeakers, 1};
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        g_now_us += 10'000'000;
        pump(6);
        g_now_us += 90'000'000;
        pump(6);
        expect(player::panel::screen_stats().dimmed, "the speakers dim after a minute alone");
        tap_button("Preferences");
        pump(6);
        expect(player::panel::prefs_root() == nullptr, "the tap that wakes the screen does not open Preferences");
        expect(!player::panel::screen_stats().dimmed, "but it wakes it");
        expect(tap_button("Preferences"), "the footer's Preferences button is there once the screen is awake");
        pump(8);
        expect(player::panel::prefs_root() != nullptr, "and a second tap opens it");
        g_now_us += 50'000'000;
        pump(6);
        expect(player::panel::prefs_root() != nullptr, "Preferences is still open after 50 s");
        expect(!player::panel::screen_stats().dimmed, "and it keeps the screen lit past the dim time");
        g_now_us += 15'000'000;
        pump(6);
        expect(player::panel::prefs_root() == nullptr, "left alone for a minute, Preferences puts itself away");
        pump(6);
        expect(player::panel::screen_stats().dimmed, "and then the screen dims, as it would have");
        tap(width / 2, height / 3);
        pump(6);
        g_now_playing = false;
        g_settings = {nv::View::kSpeakers, 5};
        hold(lrc, 5'750);
        pump(6);
    }

    // The same sheet for a board whose screen has no touch: the dark view and the dim time are greyed with their
    // reason, never hidden, and a tap on them does nothing.
    g_caps = iclforge::variants::c3_spi_320x240_no_touch();
    g_caps.display.width = width;
    g_caps.display.height = height;
    g_settings = {nv::View::kSpeakers, 5};
    expect(tap_button("Preferences"), "the footer has a Preferences button on a board with no touch");
    pump(8);
    lv_obj_t* const greyed = player::panel::prefs_root();
    expect(greyed != nullptr, "Preferences opens on a board with no touch");
    if (greyed != nullptr) {
        write_bmp(out / ("prefs-greyed-" + size + ".bmp"));
        failures += audit(greyed, "preferences, no touch " + size);
        expect(find_button("Dark") != nullptr, "the Dark choice is shown, greyed, not hidden");
        expect(shows_text(greyed, "no touch", true), "and the reason is on the sheet");
        tap_button("Dark");
        pump(4);
        expect(g_settings.view == nv::View::kSpeakers, "a tap on a greyed choice does nothing");
        tap_button("+");
        pump(4);
        expect(g_settings.dim_after_minutes == 5, "nor does a tap on the greyed dim time");
        tap_button("Clock");
        expect(g_settings.view == nv::View::kClock, "the choices it can make still work");
        tap_button("Close");
        pump(6);
    }
    return failures;
}

}  // namespace

namespace player::panel {

bool read_lyrics(LyricsReader& reader, std::int64_t delay_ms,
                 const iclforge::lyrics::StageOptions& options) {
    const iclforge::lyrics::Lyrics* const lyrics = g_held ? &g_doc : nullptr;
    const iclforge::lyrics::Stage stage =
        iclforge::lyrics::make_stage(lyrics, g_position, delay_ms, options);
    reader.on_lyrics(g_held ? g_item : -1, lyrics, stage);
    return true;
}

BoardInfo read_board_info() { return g_board; }

iclforge::devview::Facts read_dev_facts() { return g_dev; }

std::string read_log_tail(std::size_t /*limit*/) { return g_log; }

// The board takes a moment to change; the preview changes at once and says it is done.
bool request_network_preference(iclforge::devview::Preference preference) {
    g_dev.preference = iclforge::devview::preference_word(preference);
    g_dev.net_change = iclforge::devview::NetChange::done;
    g_dev.net_change_word = g_dev.preference;
    return true;
}

bool read_now_playing(NowPlaying& out) {
    if (!g_now_playing) {
        return false;
    }
    out = g_now;
    return true;
}

bool read_now_title(NowTitle& out) {
    now_copy_title(out, g_title_held && g_title.known() ? &g_title : nullptr, g_title_position);
    return true;
}

NowSettings read_now_settings() { return g_settings; }

iclforge::wallclock::Reading read_wall_clock() { return g_wall; }

void set_backlight(int percent) { g_backlight = percent; }

// The board's preferences as the preview holds them: the settings, and the capability description of the board being drawn.
class PreviewBackend final : public iclforge::prefs::Backend {
   public:
    [[nodiscard]] iclforge::BoardCaps caps() const override { return g_caps; }
    [[nodiscard]] iclforge::nowview::View now_view() const override { return g_settings.view; }
    [[nodiscard]] bool set_now_view(iclforge::nowview::View view) override {
        if (!now_view_drawn(view)) {
            return false;
        }
        g_settings.view = view;
        return true;
    }
    [[nodiscard]] int dim_after_minutes() const override { return g_settings.dim_after_minutes; }
    [[nodiscard]] bool set_dim_after_minutes(int minutes) override {
        g_settings.dim_after_minutes = minutes;
        return true;
    }
};

iclforge::prefs::Backend& preferences_backend() {
    static PreviewBackend backend;
    return backend;
}

std::int64_t time_us() { return g_now_us; }

FontFile archivo_regular() {
    static const FontFile font = load_font("Archivo-Regular.ttf");
    return font;
}
FontFile archivo_semibold() {
    static const FontFile font = load_font("Archivo-SemiBold.ttf");
    return font;
}
FontFile noto_sans_greek_cyrillic_regular() {
    static const FontFile font = load_font("NotoSansGreekCyrillic-Regular.ttf");
    return font;
}
FontFile noto_sans_greek_cyrillic_semibold() {
    static const FontFile font = load_font("NotoSansGreekCyrillic-SemiBold.ttf");
    return font;
}
FontFile noto_sans_hebrew_regular() {
    static const FontFile font = load_font("NotoSansHebrew-Regular.ttf");
    return font;
}
FontFile noto_sans_hebrew_semibold() {
    static const FontFile font = load_font("NotoSansHebrew-SemiBold.ttf");
    return font;
}

}  // namespace player::panel

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path("panel-preview");
    fs::create_directories(out);
    const fs::path examples = PREVIEW_EXAMPLES_DIR;
    const std::string auld = read_text(examples / "auld-lang-syne" / "auld-lang-syne.lrc");
    const std::string auld_lines = read_text(examples / "auld-lang-syne" / "auld-lang-syne-lines-only.lrc");
    const std::string johnny = read_text(examples / "leave-her-johnny" / "leave-her-johnny.lrc");

    // "Hava nagila", as UTF-8 bytes (nothing here depends on the compiler's source encoding).
    const std::string hava = "\xD7\x94\xD7\x91\xD7\x94 ";
    const std::string nagila = "\xD7\xA0\xD7\x92\xD7\x99\xD7\x9C\xD7\x94";
    const std::string venismeha = "\xD7\x95\xD7\xA0\xD7\xA9\xD7\x9E\xD7\x97\xD7\x94";
    const std::string hebrew =
        "[ti:Hava nagila]\n[00:00.00]<00:00.00>" + hava + "<00:00.80>" + nagila + "\n"
        "[00:03.00]<00:03.00>" + hava + "<00:03.80>" + nagila + "\n"
        "[00:06.00]<00:06.00>" + hava + "<00:06.70>" + nagila + " <00:07.60>" + venismeha + "\n"
        "[00:10.00]<00:10.00>" + hava + "<00:10.70>" + nagila + " <00:11.60>" + venismeha + "\n";
    // Russian and Greek, which the fonts have, among Latin ones: "Privet, mir" and "Kalimera kosme".
    const std::string privet = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    const std::string mir = "\xD0\xBC\xD0\xB8\xD1\x80";
    const std::string kalimera = "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1";
    const std::string kosme = "\xCE\xBA\xCF\x8C\xCF\x83\xCE\xBC\xCE\xB5";
    const std::string cyrillic_greek =
        "[ti:" + privet + "]\n[00:00.00]<00:00.00>" + privet + ", <00:01.00>" + mir +
        "\n[00:03.00]<00:03.00>" + kalimera + " <00:04.00>" + kosme +
        "\n[00:06.00]Back to the Latin letters\n[00:09.00]The end\n";
    // A line in Arabic, which the fonts have not, among Latin ones, and a line with music notes.
    const std::string marhaba = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";
    const std::string note = "\xE2\x99\xAA";
    const std::string mixed =
        "[ti:" + marhaba + "]\n[00:00.00]<00:00.00>" + note + " <00:00.60>la <00:01.20>la <00:01.80>la " + note +
        "\n[00:03.00]" + marhaba + "\n[00:06.00]Back to the Latin letters\n[00:09.00]The end\n";
    const std::string plain =
        "Should auld acquaintance be forgot,\nAnd never brought to mind?\n"
        "Should auld acquaintance be forgot,\nAnd auld lang syne!\n\n"
        "For auld lang syne, my dear,\nFor auld lang syne,\n"
        "We'll tak a cup o' kindness yet,\nFor auld lang syne.\n";

    lv_init();
    lv_tick_set_cb([]() -> std::uint32_t { return static_cast<std::uint32_t>(g_now_us / 1000); });
    g_pointer_device = lv_indev_create();
    lv_indev_set_type(g_pointer_device, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_pointer_device, &read_pointer);
    lv_display_t* disp = open_display(1024, 600, nullptr);

    g_board.name = "hearth-4f2a91";
    g_board.address = "192.168.1.50";
    g_board.network = "HomeNet";
    g_board.firmware = "0.11.0";
    g_board.player = true;
    if (!player::panel::screen_init(disp, g_width, g_height)) {
        return 1;
    }

    const std::vector<Scenario> scenarios = {
        {"s1-idle-unpaired", [&] { g_held = false; g_position.reset(); }},
        {"s1-idle-pairing",
         [&] {
             g_board.pairing_code = "4821";
             g_held = false;
         }},
        {"s1-idle-paired",
         [&] {
             g_board.pairing_code.clear();
             g_board.server = "Hearth on STUDIO-PC";
             g_board.paired = 1;
             g_board.connections = 1;
             g_held = false;
         }},
        {"s7-no-lyrics", [&] {
             g_board.stream = "bursts";
             g_held = false;
             g_settings.view = iclforge::nowview::View::kOff;
         }},
        {"s13-speakers-2-0", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             play_room("2.0", {.codec = iclforge::nowview::Codec::kPcm}, kStereo);
         }, 10'000'000},
        {"s13-speakers-5-1", [&] { play_room("5.1", kDolbyFiveOne, kFiveOne); }, 10'000'000},
        {"s13-speakers-5-1-silent-centre", [&] { play_room("5.1", kDolbyFiveOne, kFiveOneNoCentre); }, 10'000'000},
        {"s13-speakers-7-1-4", [&] {
             play_room("7.1.4", {.codec = iclforge::nowview::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true,
                                 .objects = 15},
                       kSevenOneFour);
         }, 10'000'000},
        {"s13-speakers-9-1-6", [&] { play_room("9.1.6", kDolbyFiveOne, kNineOneSix); }, 10'000'000},
        // Stream details (a server that is not Hearth: Music Assistant): decoded on this board, the bitrate and
        // who sends it, in the footer; and PCM, which the server decoded.
        {"s17-ma-atmos-7-1-4", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             play_room("7.1.4", {.codec = iclforge::nowview::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true,
                                 .objects = 15},
                       kSevenOneFour, 768, "Music Assistant (d5369777-music-assistant)");
         }, 10'000'000},
        {"s17-ma-title-card", [&] {
             g_settings.view = iclforge::nowview::View::kTitle;
             play_room("5.1", kDolbyFiveOne, kFiveOne, 448, "Music Assistant (d5369777-music-assistant)");
             say_title("Blue in Green", "Miles Davis", "Kind of Blue", 1959, 83'000, 337'000);
         }},
        {"s17-ma-pcm", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             play_room("2.0", {.codec = iclforge::nowview::Codec::kPcm}, kStereo, 0,
                       "Music Assistant (d5369777-music-assistant)");
             say_no_title();
         }, 10'000'000},
        {"s17-long-server-name", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             play_room("5.1", kDolbyFiveOne, kFiveOne, 640, "Living room Hearth server on a very long host name (id)");
             say_no_title();
         }, 10'000'000},
        // Stage 2: the title card, once a server has said a title. A song of a record's length, partway.
        {"s16-title-card", [&] {
             g_settings.view = iclforge::nowview::View::kTitle;
             play_room("5.1", kDolbyFiveOne, kFiveOne);
             say_title("Blue in Green", "Miles Davis", "Kind of Blue", 1959, 83'000, 337'000);
         }},
        // A long title is cut at two rows with three dots, and the type steps down to fit.
        {"s16-title-card-long", [&] {
             say_title("Symphony No. 9 in D minor, Op. 125 \"Choral\": IV. Finale - Presto - Allegro assai - "
                       "Allegro energico, sempre ben marcato",
                       "Berliner Philharmoniker, Herbert von Karajan",
                       "Beethoven: The Nine Symphonies (Remastered 2018, Deutsche Grammophon)", 1963, 1'412'000, 1'485'000);
         }},
        // Only a title and an artist: nothing is made up for the lines the server did not send.
        {"s16-title-card-no-album", [&] { say_title("Bohemian Rhapsody", "Queen", "", 0, 15'000, 355'000); }},
        // A title and nothing else, no progress: no time and no line over it.
        {"s16-title-card-title-only", [&] { say_title("Untitled radio programme", "", "", 0, std::nullopt, 0); }},
        // A title in a script the fonts lack is not drawn as boxes: the speakers stand in its place.
        {"s16-title-card-undrawable", [&] { say_title("\xE3\x83\x9C\xE3\x83\x98\xE3\x83\x9F\xE3\x82\xA2\xE3\x83\xB3", "Queen", "", 0, 15'000, 355'000); }},
        // The title card is chosen and no server has said anything: the speakers, never an empty card.
        {"s16-title-chosen-none-said", [&] { say_no_title(); }},
        // The speakers with a title said: the header is the song, with its time and the progress line.
        {"s16-speakers-with-title", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             say_title("Blue in Green", "Miles Davis", "Kind of Blue", 1959, 83'000, 337'000);
         }, 10'000'000},
        // The clock view (stage 3): Tuesday 20 October 2026, 21:47 in Sydney, the prototype's picture; the same
        // with no time zone set, which says UTC; and before a time server has answered, which says nothing.
        {"s16-clock", [&] {
             g_settings.view = iclforge::nowview::View::kClock;
             play_room("5.1", kDolbyFiveOne, kFiveOne);
             g_wall = {.known = true, .local = {2026, 10, 20, 21, 47, 12, 2}, .zone_set = true};
         }},
        {"s16-clock-utc", [&] { g_wall = {.known = true, .local = {2026, 10, 20, 10, 47, 12, 2}, .zone_set = false}; }},
        {"s16-clock-waiting", [&] { g_wall = {}; }},
        // The clock's format (a person's choice): 12 hours, with the AM or PM small beside the time; 24 hours with
        // the seconds beside it; and 12 hours with both.
        {"s16-clock-12h", [&] {
             g_wall = {.known = true, .local = {2026, 10, 20, 21, 47, 12, 2}, .zone_set = true, .format = {.twelve_hour = true}};
         }},
        {"s16-clock-seconds", [&] {
             g_wall = {.known = true, .local = {2026, 10, 20, 21, 47, 12, 2}, .zone_set = true, .format = {.seconds = true}};
         }},
        {"s16-clock-12h-seconds", [&] {
             g_wall = {.known = true,
                       .local = {2026, 10, 20, 9, 5, 7, 2},
                       .zone_set = true,
                       .format = {.twelve_hour = true, .seconds = true}};
         }},
        {"s0-waiting", [&] {
             g_board.stream = "bursts";
             hold(auld, std::nullopt);
         }},
        // Half way through "quain" of "acquaintance": three words sung, one being sung.
        {"s2-singing", [&] { hold(auld, 1'750); }},
        {"s2-singing-late-line", [&] { hold(auld, 13'750); }},
        {"s4-instrumental-gap", [&] {
             hold("[00:02.00]<00:02.00>Come <00:02.60>on <00:03.20>in\n[00:05.00]\n"
                  "[00:14.00]<00:14.00>The <00:14.60>second <00:15.20>verse\n",
                  11'400);
         }},
        {"s5-line-timed", [&] { hold(auld_lines, 5'000); }},
        {"s6-plain-text", [&] { hold(plain, 0); }},
        {"s3-duet-first", [&] { hold(johnny, 6'000); }},
        {"s3-duet-second", [&] { hold(johnny, 20'000); }},
        {"s8-right-to-left", [&] { hold(hebrew, 7'000); }},
        {"s10-script-undrawable", [&] { hold(mixed, 4'000); }},
        {"s11-notes-dropped", [&] { hold(mixed, 1'200); }},
        {"s12-cyrillic-greek", [&] { hold(cyrillic_greek, 3'500); }},
    };

    int number = 0;
    for (const Scenario& scenario : scenarios) {
        scenario.set_up();
        g_now_us += scenario.settle_us;  // so that the board's info is read again, and a glow has settled
        pump(6);
        write_bmp(out / (std::string(scenario.name) + ".bmp"));
        const player::panel::ScreenStats stats = player::panel::screen_stats();
        std::printf("%2d  %-26s screen %-9s rebuilds %u\n", ++number, scenario.name, stats.name, stats.rebuilds);
    }

    // S14: the speakers dimmed after six minutes of nothing touched, and a touch waking them. Checked
    // as well as drawn: a screen that stayed dark, or a backlight that stayed down, is a failure.
    {
        g_settings.view = iclforge::nowview::View::kSpeakers;
        g_settings.dim_after_minutes = 5;
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        g_now_us += 10'000'000;
        pump(6);
        const bool lit_before = !player::panel::screen_stats().dimmed && g_backlight == 80;
        g_now_us += 4LL * 60 * 1'000'000;
        pump(6);
        const bool lit_at_four = !player::panel::screen_stats().dimmed && g_backlight == 80;
        g_now_us += 2LL * 60 * 1'000'000;
        pump(6);
        write_bmp(out / "s14-dimmed.bmp");
        const bool dark = player::panel::screen_stats().dimmed && g_backlight == 4;
        tap(500, 300);
        pump(6);
        const bool woken = !player::panel::screen_stats().dimmed && g_backlight == 80;
        std::printf("%2d  %-26s lit %s, lit at 4 min %s, dark at 6 min %s, a touch wakes it %s\n", ++number,
                    "s14-dimmed", lit_before ? "yes" : "NO", lit_at_four ? "yes" : "NO", dark ? "yes" : "NO",
                    woken ? "yes" : "NO");
        if (!lit_before || !lit_at_four || !dark || !woken) {
            return 3;
        }
        // Never dims: the same quiet, no dark.
        g_settings.dim_after_minutes = 0;
        g_now_us += 60LL * 60 * 1'000'000;
        pump(6);
        if (player::panel::screen_stats().dimmed) {
            std::puts("dim after never dimmed the screen");
            return 3;
        }
        g_settings.dim_after_minutes = 5;
    }

    // S16: the title card dims as the speakers do, its dark screen names the song, and a touch wakes it.
    {
        g_settings.view = iclforge::nowview::View::kTitle;
        g_settings.dim_after_minutes = 5;
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        say_title("Blue in Green", "Miles Davis", "Kind of Blue", 1959, 83'000, 337'000);
        // The last scenario left the screen untouched for an hour: a touch is what starts the count.
        tap(500, 300);
        g_now_us += 10'000'000;
        pump(6);
        const bool card = std::string(player::panel::screen_stats().name) == "nolyrics" && !player::panel::screen_stats().dimmed;
        g_now_us += 6LL * 60 * 1'000'000;
        pump(6);
        write_bmp(out / "s16-title-card-dimmed.bmp");
        const bool dark = player::panel::screen_stats().dimmed && g_backlight == 4;
        tap(500, 300);
        pump(6);
        const bool woken = !player::panel::screen_stats().dimmed && g_backlight == 80;
        std::printf("%2d  %-26s card %s, dark at 6 min %s, a touch wakes it %s\n", ++number, "s16-title-dimmed",
                    card ? "yes" : "NO", dark ? "yes" : "NO", woken ? "yes" : "NO");
        if (!card || !dark || !woken) {
            return 3;
        }
        // The server says nothing is playing: the card gives way to the speakers.
        say_no_title();
        g_now_us += 1'000'000;
        pump(6);
        write_bmp(out / "s16-title-cleared.bmp");
        say_no_title();
        g_settings.dim_after_minutes = 5;
        // The sheet below shows the default choice, as a board out of the box does.
        g_settings.view = iclforge::nowview::View::kSpeakers;
    }

    // S16: the clock. A minute that changes redraws the time and nothing else, which is what keeps it cheap
    // beside the audio; a second that changes redraws nothing; and a dim leaves the clock on the glass.
    {
        g_settings.view = iclforge::nowview::View::kClock;
        g_settings.dim_after_minutes = 5;
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        g_wall = {.known = true, .local = {2026, 10, 20, 21, 47, 12, 2}, .zone_set = true};
        g_now_us += 10'000'000;
        pump(6);
        const std::uint64_t before_seconds = g_flushed_pixels;
        g_wall.local.second = 40;
        g_now_us += 1'000'000;
        pump(6);
        const std::uint64_t by_a_second = g_flushed_pixels - before_seconds;
        const std::uint64_t before_minute = g_flushed_pixels;
        const std::uint32_t areas_before = g_flushed_areas;
        g_wall.local.minute = 48;
        g_wall.local.second = 1;
        g_now_us += 1'000'000;
        pump(6);
        const std::uint64_t by_a_minute = g_flushed_pixels - before_minute;
        const std::uint32_t minute_areas = g_flushed_areas - areas_before;
        const auto bright_pixels = [](int x0, int y0, int x1, int y1) {
            int count = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const std::uint16_t p = g_frame[static_cast<std::size_t>(y) * g_width + static_cast<std::size_t>(x)];
                    count += ((p >> 11) & 0x1F) > 8 ? 1 : 0;
                }
            }
            return count;
        };
        const int lit_pixels = bright_pixels(240, 140, 780, 330);
        // Dim: the backlight goes down and the clock stays.
        g_now_us += 6LL * 60 * 1'000'000;
        g_wall.local.minute = 54;
        pump(6);
        const bool dimmed = player::panel::screen_stats().dimmed && g_backlight == 4;
        const int dim_pixels = bright_pixels(240, 140, 780, 330);
        tap(500, 300);
        pump(6);
        const bool woken = !player::panel::screen_stats().dimmed && g_backlight == 80;
        std::printf("%2d  %-26s a second flushed %llu px, a minute %llu px in %u areas, %d clock pixels lit, dimmed %s "
                    "with %d still lit, a touch wakes it %s\n",
                    ++number, "s16-clock", static_cast<unsigned long long>(by_a_second),
                    static_cast<unsigned long long>(by_a_minute), static_cast<unsigned>(minute_areas), lit_pixels,
                    dimmed ? "yes" : "NO", dim_pixels, woken ? "yes" : "NO");
        // One rectangle (the time's), about 300,000 pixels at most, and nothing for the seconds.
        if (by_a_second != 0 || by_a_minute == 0 || by_a_minute > 300'000 || lit_pixels < 2000 || !dimmed ||
            dim_pixels < 2000 || !woken) {
            return 3;
        }
        // Chosen away from the clock, the clock is gone and the speakers are back.
        g_settings.view = iclforge::nowview::View::kSpeakers;
        g_now_us += 1'000'000;
        pump(6);
        g_settings.dim_after_minutes = 5;
    }

    // S16b: the clock's format. The seconds are small beside the time, so a second that passes redraws only
    // them (a rectangle of about 9,600 pixels, not the digits' 300,000); and with no seconds a second redraws
    // nothing, as before. Checked as well as drawn.
    {
        g_settings.view = iclforge::nowview::View::kClock;
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        g_wall = {.known = true,
                  .local = {2026, 10, 20, 21, 47, 12, 2},
                  .zone_set = true,
                  .format = {.twelve_hour = true, .seconds = true}};
        g_now_us += 10'000'000;
        pump(6);
        const std::uint64_t before_second = g_flushed_pixels;
        g_wall.local.second = 13;
        g_now_us += 1'000'000;
        pump(6);
        const std::uint64_t by_a_second = g_flushed_pixels - before_second;
        // Without the seconds, a second that passes is nothing (the format change itself is a repaint).
        g_wall.format = {.twelve_hour = true};
        g_now_us += 1'000'000;
        pump(6);
        const std::uint64_t before_quiet = g_flushed_pixels;
        g_wall.local.second = 40;
        g_now_us += 1'000'000;
        pump(6);
        const std::uint64_t by_a_quiet_second = g_flushed_pixels - before_quiet;
        std::printf("%2d  %-26s a second with seconds shown flushed %llu px, one without %llu px\n", ++number,
                    "s16-clock-format", static_cast<unsigned long long>(by_a_second),
                    static_cast<unsigned long long>(by_a_quiet_second));
        if (by_a_second == 0 || by_a_second > 40'000 || by_a_quiet_second != 0) {
            return 3;
        }
        g_wall.format = {};
        g_settings.view = iclforge::nowview::View::kSpeakers;
        g_now_us += 1'000'000;
        pump(6);
    }

    // S14b: the dark view. Dark from the moment a song with no lyrics begins, however long nothing has
    // touched the panel; a touch lights the speakers for 30 s from the last touch, and a second touch
    // moves that on; then it is dark again. Checked as well as drawn, like S14.
    {
        g_settings.view = iclforge::nowview::View::kDark;
        g_now_us += 3600LL * 1'000'000;  // the panel has been left alone for an hour
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        pump(6);
        const auto state = [] { return player::panel::screen_stats(); };
        const bool dark_at_start = state().dimmed && g_backlight == 0;
        tap(500, 300);
        pump(6);
        const bool woken = !state().dimmed && g_backlight == 80;
        g_now_us += 28'000'000;
        pump(6);
        const bool lit_at_28 = !state().dimmed && g_backlight == 80;
        g_now_us += 4'000'000;
        pump(6);
        const bool dark_at_32 = state().dimmed && g_backlight == 0;
        // A touch 20 s after the first moves the dark on by as much: lit 28 s after it, dark 32 s after.
        tap(500, 300);
        pump(6);
        g_now_us += 20'000'000;
        tap(500, 300);
        pump(6);
        g_now_us += 28'000'000;
        pump(6);
        const bool lit_after_second = !state().dimmed && g_backlight == 80;
        g_now_us += 4'000'000;
        pump(6);
        const bool dark_again = state().dimmed && g_backlight == 0;
        std::printf("%2d  %-26s dark at the start %s, a touch lights it %s, lit at 28 s %s, dark at 32 s %s, "
                    "a second touch moves it on %s, dark again %s\n",
                    ++number, "s14b-dark", dark_at_start ? "yes" : "NO", woken ? "yes" : "NO",
                    lit_at_28 ? "yes" : "NO", dark_at_32 ? "yes" : "NO", lit_after_second ? "yes" : "NO",
                    dark_again ? "yes" : "NO");
        if (!dark_at_start || !woken || !lit_at_28 || !dark_at_32 || !lit_after_second || !dark_again) {
            return 3;
        }
        // The sheet with Dark chosen: the dim row says what a touch does instead of offering dim times.
        tap(500, 300);
        pump(6);
        tap_button("Adjust");
        pump(6);
        write_bmp(out / "s16-controls-dark.bmp");
        std::printf("%2d  %-26s sheet, dark view chosen\n", ++number, "s16-controls-dark");
        g_now_us += 9'000'000;  // the sheet puts itself away after eight seconds untouched
        pump(6);
        g_settings.view = iclforge::nowview::View::kSpeakers;
        pump(6);
    }

    // S9: the sheet, over a line being sung.
    hold(auld, 5'750);
    g_now_us += 1'000'000;
    pump(6);
    tap_button("Adjust");
    pump(6);
    write_bmp(out / "s9-controls.bmp");
    std::printf("%2d  %-26s sheet\n", ++number, "s9-controls");

    // S13: the developer view, opened as a person opens it: three taps on the board's name.
    g_dev = healthy_board();
    g_log = kHealthyLog;
    g_board.name = g_dev.name;
    g_now_us += 10'000'000;
    pump(6);
    const int name_x = 100;
    const int name_y = 600 - 32;
    tap(name_x, name_y);
    tap(name_x + 6, name_y - 4);
    tap(name_x - 3, name_y + 5);
    g_now_us += 1'000'000;
    pump(6);
    write_bmp(out / "s15-dev-view.bmp");
    std::printf("%2d  %-26s developer view %s\n", ++number, "s15-dev-view",
                player::panel::screen_stats().dev_view ? "open" : "NOT OPEN");

    // A tap on a segment of the preference control asks first.
    tap(430, 236);  // Wi-Fi
    g_now_us += 600'000;
    pump(6);
    write_bmp(out / "s15-dev-confirm.bmp");
    std::printf("%2d  %-26s confirming the switch to Wi-Fi\n", ++number, "s15-dev-confirm");

    // Cancelling asks nothing of the board; going on does it and says so.
    tap(90, 342);  // Cancel
    g_now_us += 600'000;
    pump(6);
    std::printf("%2d  %-26s preference after Cancel: %s\n", ++number, "s13-cancel", g_dev.preference.c_str());
    tap(430, 236);
    tap(250, 342);  // Switch
    g_now_us += 600'000;
    pump(6);
    write_bmp(out / "s15-dev-switched.bmp");
    std::printf("%2d  %-26s preference after Switch: %s\n", ++number, "s15-dev-switched", g_dev.preference.c_str());

    // Nothing playing, no server connected: the stream's rows are quiet and the meters say so.
    g_dev = healthy_board();
    g_dev.stream = "idle";
    g_dev.peak_db.clear();
    g_dev.server.clear();
    g_dev.connections = 0;
    g_dev.clock_converged = false;
    g_now_us += 600'000;
    pump(6);
    write_bmp(out / "s15-dev-idle.bmp");
    std::printf("%2d  %-26s nothing playing\n", ++number, "s15-dev-idle");

    // The same board with a cable pulled, a weak radio, a stream that has been glitching, a panic
    // behind it and a core dump in flash.
    g_dev = healthy_board();
    g_dev.active = "wifi";
    g_dev.web_address = g_dev.wifi.address;
    // A server whose name is longer than its row: Music Assistant gives its id with it.
    g_dev.server = "Music Assistant (d5369777-music-assistant)";
    g_dev.ethernet.link = false;
    g_dev.ethernet.address.clear();
    g_dev.ethernet.speed_mbps.reset();
    g_dev.ethernet.full_duplex.reset();
    g_dev.wifi.rssi_dbm = -79;
    g_dev.reset_reason = "panic";
    g_dev.core_dump = true;
    g_dev.core_dump_bytes = 262'144;
    g_dev.uptime_ms = 94'000;
    g_dev.underruns = 3;
    g_dev.late = 1'200;
    g_dev.dropped = 2;
    g_dev.resyncs = 1;
    g_dev.clock_converged = false;
    g_dev.internal_low = 9'000;
    g_dev.late_cycles = 4;
    g_dev.played = true;
    g_dev.us_per_frame = 412;
    g_dev.worst_frame_us = 1'120;
    g_dev.sink_us_per_frame = 250;
    g_dev.render_us_per_frame = 80;
    g_now_us += 600'000;
    pump(6);
    write_bmp(out / "s15-dev-trouble.bmp");
    std::printf("%2d  %-26s cable out, glitches, a panic\n", ++number, "s15-dev-trouble");

    // A tap on the view anywhere else closes it.
    g_now_us += 2'000'000;
    tap(900, 300);
    pump(6);
    std::printf("%2d  %-26s closed by a tap: %s\n", ++number, "s15-dev-closed",
                player::panel::screen_stats().dev_view ? "STILL OPEN" : "yes");

    // Adjust and Preferences at every size the product is drawn at: the kit's panel, and the smaller ones a board
    // with a screen of its own could have. Checked as well as drawn: text that is cut off, a control smaller than a
    // fingertip or sticking out of its sheet, a choice that does nothing, fails the run.
    std::puts("Adjust and Preferences at each display size:");
    int failures = 0;
    // 240 by 240 is the first square one (a JetKVM Mini's); its board has no touch, which every size's second pass
    // (a board whose screen has none) draws and checks as well.
    for (const auto& [width, height] :
         {std::pair{1024, 600}, std::pair{800, 480}, std::pair{480, 320}, std::pair{320, 240}, std::pair{240, 240}}) {
        failures += check_sheets(width, height, out, auld, disp);
    }
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 3;
    }
    return 0;
}
