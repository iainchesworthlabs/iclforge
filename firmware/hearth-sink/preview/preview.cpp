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
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lvgl.h"

#include "meter_groove.hpp"

#include "iclforge/board_variants.hpp"
#include "iclforge/dev_view.hpp"
#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"
#include "iclforge/meters_view.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/preferences.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"
#include "iclforge/render/speaker_trim.hpp"
#include "iclforge/title_card.hpp"
#include "iclforge/wall_clock.hpp"
#include "panel/lvgl/board_info.hpp"
#include "panel/lvgl/dev_info.hpp"
#include "panel/lvgl/fonts.hpp"
#include "panel/lvgl/layout_sheet.hpp"
#include "panel/lvgl/lyric_screen.hpp"
#include "panel/lvgl/meters_screen.hpp"
#include "panel/lvgl/now_screen.hpp"
#include "panel/lvgl/prefs_editors.hpp"
#include "panel/lvgl/prefs_pages.hpp"
#include "panel/lvgl/prefs_screen.hpp"
#include "panel/lvgl/sound_sheet.hpp"
#include "panel/lyrics_reader.hpp"
#include "sound.hpp"

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
// What the Meters view hears (meter_groove.hpp), written into the tap by pump().
preview::MeterGroove g_groove;
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

// The Speakers sheet's board: the layout in force with a role at each output, what is set for each, and the tone; and
// what the sheet asked of it, for the checks to read.
player::SoundSheet g_sound;
std::vector<std::string> g_tone_requests;
std::vector<std::string> g_trim_requests;
// Late night, as the Preferences sheet last set it.
bool g_late_night = false;
// The System group's side is fixtures: a name that is stored when it is saved, a network preference, the facts a kit
// reports of itself, a few paired servers that Forget takes away, and what a restart or a reset was asked for (none of
// which restarts anything on a computer).
struct SystemFixture {
    std::string name = "hearth-4f2a91";
    std::string network_preference = "auto";
    iclforge::devview::Facts facts;
    iclforge::sysactions::Situation situation;
    std::vector<iclforge::sysactions::paired::Server> paired;
    std::optional<std::string> refusal;
    int restarts = 0;
    int factory_resets = 0;
    int forgotten = 0;
};
SystemFixture g_system;
// What the preferences hold that the panel does not read back: the backlight's lit level, and the zone chosen.
int g_brightness = 80;
iclforge::wallclock::ZoneChoice g_zone;
// The speaker layout the board holds (the preference of that name), as the text PUT /layout takes.
std::string g_layout = "2.0";

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
        if (g_now_playing && player::panel::meters_tap().wants_blocks()) {
            g_groove.feed(player::panel::meters_tap(), g_now_us);
        }
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

// What a button says: its first label.
std::string button_text(lv_obj_t* button) {
    const std::uint32_t count = lv_obj_get_child_count(button);
    for (std::uint32_t i = 0; i < count; ++i) {
        lv_obj_t* const child = lv_obj_get_child(button, static_cast<std::int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class)) {
            return label_text(child);
        }
    }
    return {};
}

lv_obj_t* find_button(const std::string& text) {
    std::vector<lv_obj_t*> targets;
    // The page or the dialog of the Preferences editors covers the sheet: it is the one a finger reaches.
    collect_touch_targets(player::panel::pages_root() != nullptr ? player::panel::pages_root() : lv_screen_active(), targets);
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

// Scrolls every scrolling box that holds `object` until the object is inside it. LVGL's own scroll_to_view_recursive brings
// each container into view of the one above it, which is not the object when a container is taller than the box that
// scrolls it (a page of buttons under a sheet's tabs).
void show_in_scroller(lv_obj_t* object) {
    lv_obj_update_layout(object);
    for (lv_obj_t* box = lv_obj_get_parent(object); box != nullptr; box = lv_obj_get_parent(box)) {
        if (!lv_obj_has_flag(box, LV_OBJ_FLAG_SCROLLABLE)) {
            continue;
        }
        lv_area_t at;
        lv_area_t in;
        lv_obj_get_coords(object, &at);
        lv_obj_get_coords(box, &in);
        int dy = 0;
        if (at.y2 > in.y2) {
            dy = -(at.y2 - in.y2 + 4);
        } else if (at.y1 < in.y1) {
            dy = in.y1 - at.y1 + 4;
        }
        if (dy != 0) {
            lv_obj_scroll_by(box, 0, dy, LV_ANIM_OFF);
            lv_obj_update_layout(box);
        }
    }
}

// A tap on the middle of a button, scrolled into view first if it is on a list.
bool tap_object(lv_obj_t* button) {
    if (button == nullptr) {
        return false;
    }
    show_in_scroller(button);
    lv_obj_update_layout(button);
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    tap((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
    return true;
}

// A finger put down on the button that says `text`, held for `ms` of the board's clock (time only moves as the frames
// do), and lifted unless `lift` is false.
bool hold_button(const std::string& text, int ms, bool lift = true) {
    lv_obj_t* const button = find_button(text);
    if (button == nullptr) {
        std::printf("  no button \"%s\" is shown\n", text.c_str());
        return false;
    }
    lv_obj_scroll_to_view_recursive(button, LV_ANIM_OFF);
    lv_obj_update_layout(button);
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    g_pointer = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2, true};
    pump(3);
    for (int held = 0; held < ms; held += 33) {
        pump(1);
    }
    if (lift) {
        g_pointer.down = false;
        pump(3);
    }
    return true;
}

// A tap on the middle of the button that says `text`, scrolled into view first if it is on a list.
bool tap_button(const std::string& text) {
    lv_obj_t* const button = find_button(text);
    if (button == nullptr) {
        std::printf("  no button \"%s\" is shown\n", text.c_str());
        return false;
    }
    return tap_object(button);
}

// The label under `root` that is shown and says exactly `text`.
lv_obj_t* find_label(lv_obj_t* root, const std::string& text) {
    if (root == nullptr || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
        return nullptr;
    }
    if (lv_obj_check_type(root, &lv_label_class) && label_text(root) == text) {
        return root;
    }
    const std::uint32_t count = lv_obj_get_child_count(root);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (lv_obj_t* const found = find_label(lv_obj_get_child(root, static_cast<std::int32_t>(i)), text)) {
            return found;
        }
    }
    return nullptr;
}

// The touch targets under `root` that say `text`, in the order they were made.
std::vector<lv_obj_t*> find_buttons_in(lv_obj_t* root, const std::string& text) {
    std::vector<lv_obj_t*> targets;
    std::vector<lv_obj_t*> found;
    if (root != nullptr) {
        collect_touch_targets(root, targets);
    }
    for (lv_obj_t* const target : targets) {
        if (button_text(target) == text) {
            found.push_back(target);
        }
    }
    return found;
}

// The way back up from a region's places: "Regions", or a chevron on a narrow bar.
std::vector<lv_obj_t*> back_buttons(lv_obj_t* editor) {
    std::vector<lv_obj_t*> found = find_buttons_in(editor, "Regions");
    const std::vector<lv_obj_t*> chevron = find_buttons_in(editor, "<");
    found.insert(found.end(), chevron.begin(), chevron.end());
    return found;
}

// Scrolls the list that holds `label` until the label is at its top.
void scroll_label_to_top(lv_obj_t* label) {
    lv_obj_t* list = lv_obj_get_parent(label);
    while (list != nullptr && !lv_obj_has_flag(list, LV_OBJ_FLAG_SCROLLABLE)) {
        list = lv_obj_get_parent(list);
    }
    if (list == nullptr) {
        return;
    }
    lv_obj_update_layout(list);
    lv_area_t at;
    lv_area_t in;
    lv_obj_get_coords(label, &at);
    lv_obj_get_coords(list, &in);
    lv_obj_scroll_by(list, 0, -(at.y1 - in.y1 - 4), LV_ANIM_OFF);
}

// The button that says `text` in the row of the Preferences list that is labelled `row`.
lv_obj_t* find_row_button(lv_obj_t* sheet, const std::string& row, const std::string& text) {
    lv_obj_t* const label = find_label(sheet, row);
    if (label == nullptr) {
        return nullptr;
    }
    const std::vector<lv_obj_t*> found = find_buttons_in(lv_obj_get_parent(label), text);
    return found.empty() ? nullptr : found.front();
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

// Whether `object` has a scrolling list for an ancestor below `root`; that list is returned.
lv_obj_t* scroller_above(lv_obj_t* object, lv_obj_t* root) {
    for (lv_obj_t* p = lv_obj_get_parent(object); p != nullptr && p != root; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) {
            return p;
        }
    }
    return nullptr;
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
    // The list each touch target is in, or none: what scrolls under a bar is clipped by it and is never on the glass beside it.
    std::vector<lv_obj_t*> holders;
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
                holders.push_back(scroller_above(child, root));
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
    // What of a target a finger can reach: the part of it inside every box that scrolls it. A target scrolled out of
    // its box (behind a fixed footer, in the coordinates) is not under anything.
    const auto reachable_part = [](lv_obj_t* object) {
        Rect r = rect_of(object);
        for (lv_obj_t* box = lv_obj_get_parent(object); box != nullptr; box = lv_obj_get_parent(box)) {
            if (lv_obj_has_flag(box, LV_OBJ_FLAG_SCROLLABLE)) {
                const Rect in = rect_of(box);
                r = {std::max(r.x1, in.x1), std::max(r.y1, in.y1), std::min(r.x2, in.x2), std::min(r.y2, in.y2)};
            }
        }
        return r;
    };
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const Rect whole = rect_of(targets[i]);
        smallest = std::min({smallest, whole.x2 - whole.x1 + 1, whole.y2 - whole.y1 + 1});
        const Rect a = reachable_part(targets[i]);
        if (a.x1 > a.x2 || a.y1 > a.y2) {
            continue;
        }
        for (std::size_t j = i + 1; j < targets.size(); ++j) {
            if (holders[i] != holders[j]) {
                continue;
            }
            const Rect b = reachable_part(targets[j]);
            if (b.x1 > b.x2 || b.y1 > b.y2) {
                continue;
            }
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
    // What the Meters view hears: a groove over this room (a stereo programme plays only L and R).
    g_groove.reset(std::vector<iclforge::render::PlanSpot>(spots.begin(), spots.end()), facts.codec == iclforge::nowview::Codec::kPcm);
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

// The System group's fixtures: a kit on its cable with Wi-Fi joined as well, a stream playing from Music Assistant, and
// two servers paired.
const char* const kMusicAssistantId = "gS3cmMlDUaQGhxYd0PF0x0jWR2OGdhxwUwBBwyD3O1c";
const char* const kHearthId = "Q1vGr0WkzZ5c2hXU8eYy0fKp3tNnJmAs7LbD4oHqIwE";

void set_up_system_fixture() {
    g_system = {};
    iclforge::devview::Facts& f = g_system.facts;
    f.active = "ethernet";
    f.preference = "auto";
    f.ethernet.present = f.ethernet.enabled = f.ethernet.link = true;
    f.ethernet.address = "192.168.1.163";
    f.ethernet.mac = "e8:f6:0a:e7:53:fb";
    f.ethernet.speed_mbps = 100;
    f.ethernet.full_duplex = true;
    f.wifi.present = f.wifi.enabled = f.wifi.link = f.wifi.configured = true;
    f.wifi.address = "192.168.1.251";
    f.wifi.mac = "e8:f6:0a:e7:53:f9";
    f.wifi.ssid = "HomeNet";
    f.wifi.rssi_dbm = -52;
    f.name = g_system.name;
    f.mdns = "hearth-4f2a91.local";
    f.web_address = "192.168.1.163";
    f.firmware = "v0.10.0-beta.1-5228-g0e45f23a7";
    f.slot = "ota_0";
    f.chip = "ESP32-P4";
    f.revision_major = 3;
    f.revision_minor = 2;
    f.layout = "5.1";
    f.slots = 16;
    f.player = true;
    f.paired = 2;
    iclforge::sysactions::Situation& s = g_system.situation;
    s.streaming = true;
    s.has_cable = s.cable_link = s.has_wifi = true;
    s.paired = 2;
    iclforge::sysactions::paired::Server music;
    music.server_id = kMusicAssistantId;
    music.name = "Music Assistant";
    music.connected = music.last_playback = music.seen = true;
    iclforge::sysactions::paired::Server hearth;
    hearth.server_id = kHearthId;
    hearth.name = "Hearth on STUDIO-PC";
    hearth.seen = true;
    g_system.paired = {music, hearth};
}

// The Network and System groups of the Preferences sheet (E4), driven by taps the way a person does: the switch between the
// cable and Wi-Fi with its confirming step, the name on its keyboard, About, the paired servers with Forget, Restart, Reset
// preferences and the factory reset with its held button. Each page is drawn to a picture and held to the sheet's rules
// (audit()). The sheet is open on entry and still open on return. Returns the number of failures, having said each.
int check_system_group(int width, int height, const fs::path& out, const std::string& size, lv_obj_t* prefs,
                       lv_obj_t* list) {
    namespace nv = iclforge::nowview;
    int failures = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::printf("  FAIL %s: %s\n", size.c_str(), what.c_str());
            ++failures;
        }
    };
    const auto page = [] { return player::panel::pages_root(); };
    const auto says = [&](const std::string& text) { return shows_text(lv_screen_active(), text, true); };
    const std::string arrow_left = "\xE2\x86\x90";
    const std::string arrow_up = "\xE2\x86\x91";

    for (const char* shown : {"NETWORK", "Wired or Wi-Fi", "Name of this player", "SYSTEM", "About", "Paired servers", "Restart",
                              "Reset preferences", "Factory reset"}) {
        expect(shows_text(prefs, shown), std::string("Preferences shows ") + shown);
    }
    // The group at the foot of the list, as it is first seen when scrolled to.
    if (list != nullptr) {
        lv_obj_update_layout(list);
        lv_obj_scroll_by_bounded(list, 0, -lv_obj_get_scroll_bottom(list), LV_ANIM_OFF);
        pump(4);
        write_bmp(out / ("prefs-system-" + size + ".bmp"));
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
        pump(4);
    }

    // The cable or Wi-Fi: a tap asks first, Cancel changes nothing, Switch changes it.
    expect(tap_button("Wi-Fi"), "the network row has a Wi-Fi button");
    pump(6);
    expect(page() != nullptr, "a switch of network asks first");
    expect(says("Use Wi-Fi only?") && says("Wired is switched off."), "and says what it does");
    expect(says("192.168.1.251"), "and where the page will be");
    if (page() != nullptr) {
        write_bmp(out / ("system-network-" + size + ".bmp"));
        failures += audit(page(), "network confirm " + size);
        tap_button("Cancel");
        pump(6);
        expect(page() == nullptr && g_system.network_preference == "auto", "Cancel changes nothing");
        tap_button("Wi-Fi");
        pump(6);
        tap_button("Switch");
        pump(6);
        expect(page() == nullptr && g_system.network_preference == "wifi", "Switch makes the change");
    }
    // A switch the board is certain to refuse says so, and cannot be made.
    g_system.facts.ethernet.link = false;
    tap_button("Wired");
    pump(6);
    expect(says("Wired has no link"), "a switch to wired when it has no link says so");
    tap_button("Switch");
    pump(4);
    expect(g_system.network_preference == "wifi", "and cannot be made");
    if (page() != nullptr) {
        write_bmp(out / ("system-network-blocked-" + size + ".bmp"));
        tap_button("Cancel");
        pump(6);
    }
    g_system.facts.ethernet.link = true;
    g_system.network_preference = "auto";
    pump(40);

    // The name, typed on the keyboard.
    expect(tap_button(g_system.name), "the name is a button that opens the keyboard");
    pump(8);
    expect(page() != nullptr, "and the keyboard is a page of its own");
    if (page() != nullptr) {
        write_bmp(out / ("system-name-" + size + ".bmp"));
        failures += audit(page(), "name editor " + size);
        expect(find_button("Save") != nullptr && find_button("Cancel") != nullptr, "the keyboard has Save and Cancel");
        for (std::size_t i = 0; i < g_system.name.size(); ++i) {
            tap_button(arrow_left);
        }
        tap_button("Save");
        pump(4);
        expect(page() != nullptr && g_system.name == "hearth-4f2a91", "a name that is empty cannot be saved");
        expect(tap_button(arrow_up) && tap_button("S") && tap_button("t") && tap_button("u") && tap_button("d") && tap_button("y"),
               "the keys are there to be tapped, and the shift gives a capital");
        pump(4);
        expect(says("Study") && says("5/32"), "the name as typed, and how much of its 32 bytes it uses");
        write_bmp(out / ("system-name-typed-" + size + ".bmp"));
        tap_button("Space");
        tap_button("\xC3\xA0\xC3\xA9");  // the accent page
        pump(4);
        write_bmp(out / ("system-name-accents-" + size + ".bmp"));
        failures += audit(page(), "name editor, accents " + size);
        expect(tap_button("\xC3\xA9"), "the accent page has the letters of Latin-1");
        pump(4);
        expect(says("Study \xC3\xA9") && says("8/32"), "an accented letter costs two bytes");
        tap_button(arrow_left);
        tap_button(arrow_left);
        pump(4);
        expect(says("Study") && says("5/32"), "a backspace takes a whole letter, and a space after the name is not stored");
        tap_button("Save");
        pump(8);
        expect(page() == nullptr && g_system.name == "Study", "Save stores the name");
        expect(shows_text(prefs, "Study"), "and the row shows it");
    }

    // About.
    expect(tap_button("View"), "About has a View button");
    pump(8);
    expect(page() != nullptr, "About is a page");
    if (page() != nullptr) {
        expect(shows_text(page(), "Version") && shows_text(page(), "v0.10.0-beta.1-5228"), "About says the version, without git's tail");
        expect(shows_text(page(), "ESP32-P4 v3.2") && shows_text(page(), "ota_0"), "and the chip and the slot");
        write_bmp(out / ("system-about-" + size + ".bmp"));
        failures += audit(page(), "about " + size);
        tap_button("Close");
        pump(6);
        expect(page() == nullptr, "Close puts About away");
    }

    // The paired servers, each with a Forget.
    expect(tap_button("Manage"), "the paired servers have a Manage button");
    pump(8);
    expect(page() != nullptr, "the list is a page");
    if (page() != nullptr) {
        expect(shows_text(page(), "Music Assistant") && shows_text(page(), "Hearth on STUDIO-PC"), "it lists both servers");
        expect(shows_text(page(), "playing now", true), "and says which is playing");
        write_bmp(out / ("system-servers-" + size + ".bmp"));
        failures += audit(page(), "paired servers " + size);
        expect(tap_button("Forget"), "each has a Forget");
        pump(6);
        expect(says("Forget Music Assistant?") && says("the music stops"), "forgetting the one that plays asks, and says what stops");
        write_bmp(out / ("system-forget-" + size + ".bmp"));
        failures += audit(page(), "forget " + size);
        tap_button("Cancel");
        pump(6);
        expect(g_system.forgotten == 0 && g_system.paired.size() == 2, "Cancel forgets nothing");
        tap_button("Forget");
        pump(6);
        tap_button("Forget");
        pump(10);
        expect(g_system.forgotten == 1 && g_system.paired.size() == 1, "Forget forgets that server alone");
        pump(120);
        expect(!shows_text(page(), "Music Assistant"), "and the list no longer has it");
        write_bmp(out / ("system-servers-after-" + size + ".bmp"));
        tap_button("Close");
        pump(6);
        expect(page() == nullptr, "Close puts the list away");
    }

    // Restart: asks, says what stops, goes through the board.
    expect(tap_button("Restart"), "the sheet has a Restart button");
    pump(6);
    expect(says("Restart this player?") && says("What is playing stops."), "Restart asks first");
    if (page() != nullptr) {
        write_bmp(out / ("system-restart-" + size + ".bmp"));
        failures += audit(page(), "restart " + size);
        tap_button("Cancel");
        pump(6);
        expect(g_system.restarts == 0, "Cancel does not restart");
        tap_button("Restart");
        pump(6);
        tap_button("Restart");
        pump(8);
        expect(g_system.restarts == 1, "Restart restarts the board");
        expect(says("Restarting..."), "and the screen says so until it does");
        write_bmp(out / ("system-restarting-" + size + ".bmp"));
        player::panel::pages_close();
        pump(4);
    }
    // ... and is greyed, with its reason, while the new firmware is on trial.
    g_system.situation.firmware_on_trial = true;
    g_system.refusal = "refused";
    pump(40);
    expect(shows_text(prefs, "still being checked", true), "while the firmware is on trial Restart says why it waits");
    tap_button("Restart");
    pump(6);
    expect(page() == nullptr, "and a tap on it does nothing");
    g_system.situation.firmware_on_trial = false;
    g_system.refusal.reset();
    pump(40);

    // Reset preferences: names what changes on this board, and puts it back.
    g_settings = {nv::View::kClock, 30};
    pump(40);
    expect(tap_button("Reset"), "the sheet has a Reset button");
    pump(6);
    expect(says("Reset preferences?") && says("screen without lyrics") && says("dim after") && says("brightness"),
           "Reset preferences says what it changes");
    if (page() != nullptr) {
        expect(says("These stay as they are") && says("paired servers"), "and what stays");
        write_bmp(out / ("system-reset-" + size + ".bmp"));
        failures += audit(page(), "reset preferences " + size);
        tap_button("Reset");
        pump(6);
        expect(g_settings.view == nv::View::kSpeakers && g_settings.dim_after_minutes == 5, "and puts the choices back");
        expect(g_system.name == "Study", "the name stays");
    }

    // Factory reset: what is lost, then a button that is held.
    expect(tap_button("Erase"), "the sheet has an Erase button");
    pump(6);
    expect(says("Erase everything?") && says("The wired network stays in use"), "the factory reset says what is lost, and that wired stays");
    if (page() != nullptr) {
        write_bmp(out / ("system-factory-" + size + ".bmp"));
        failures += audit(page(), "factory reset " + size);
        tap_button("Continue");
        pump(8);
        expect(find_button("Hold to erase everything") != nullptr, "the second step is a button to hold");
        write_bmp(out / ("system-hold-" + size + ".bmp"));
        failures += audit(page(), "factory reset, hold " + size);
        tap_button("Hold to erase everything");
        pump(6);
        expect(g_system.factory_resets == 0, "a tap does not erase");
        hold_button("Hold to erase everything", 1500);
        expect(g_system.factory_resets == 0, "a hold that lets go early does not erase");
        hold_button("Hold to erase everything", 1500, false);
        write_bmp(out / ("system-hold-half-" + size + ".bmp"));
        hold_button("Hold to erase everything", 1900);
        expect(g_system.factory_resets == 1, "a hold of three seconds erases");
        expect(says("Erasing everything"), "and the screen says so");
        write_bmp(out / ("system-erasing-" + size + ".bmp"));
        player::panel::pages_close();
        pump(4);
    }
    // The board that would be stranded says so: no cable plugged in.
    g_system.situation.cable_link = false;
    g_system.factory_resets = 0;
    tap_button("Erase");
    pump(6);
    expect(says("The wired network has no link.") && says("improv-wifi.com"), "a board with no wired link says what will bring it back");
    if (page() != nullptr) {
        write_bmp(out / ("system-factory-stranded-" + size + ".bmp"));
        failures += audit(page(), "factory reset, no cable " + size);
        tap_button("Cancel");
        pump(6);
    }
    g_system.situation.cable_link = true;
    expect(width > 0 && height > 0, "a size");
    return failures;
}

// The display and time preferences at one size (planning/p4x-preferences.md), driven by taps the way a person does:
// the brightness steps, the clock's hours and seconds are chosen, the night schedule is a switch and a pair of times in
// an editor of its own, and the time zone a region and then a city. Each editor is held to what the sheet is held to
// (audit()). The sheet is open, for the kit. Returns the number of failures, having said each.
int check_display_and_time(lv_obj_t* prefs, int width, int height, const fs::path& out, const std::string& size) {
    namespace nv = iclforge::nowview;
    int failures = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::printf("  FAIL %s: %s\n", size.c_str(), what.c_str());
            ++failures;
        }
    };
    g_brightness = 80;
    g_wall = {.known = true, .local = {2026, 10, 20, 21, 47, 12, 2}, .zone_set = true};
    g_settings.night = {};
    g_zone = {};
    pump(4);
    for (const char* shown : {"Brightness", "Clock hours", "Clock seconds", "Night schedule", "Night from and to",
                              "Night covers idle screen", "Time zone", "TIME"}) {
        expect(shows_text(prefs, shown), std::string("Preferences shows ") + shown);
    }
    expect(shows_text(prefs, "80%"), "the brightness starts at 80%");
    expect(shows_text(prefs, "22:00 to 07:00"), "the night's times read as a pair");
    expect(shows_text(prefs, "UTC"), "a board with no zone reads UTC");
    if (lv_obj_t* const label = find_label(prefs, "Brightness")) {
        scroll_label_to_top(label);
        pump(2);
        write_bmp(out / ("prefs-display-" + size + ".bmp"));
    }

    // The brightness: a stepper of ten stops.
    expect(tap_object(find_row_button(prefs, "Brightness", "+")), "the brightness row has a + button");
    expect(g_brightness == 90, "+ steps the brightness to 90%");
    expect(tap_object(find_row_button(prefs, "Brightness", "-")), "the brightness row has a - button");
    expect(tap_object(find_row_button(prefs, "Brightness", "-")), "and again");
    expect(g_brightness == 70, "- steps it down to 70%");
    pump(4);
    expect(shows_text(prefs, "70%"), "and the row says so");
    for (int i = 0; i < 8; ++i) {
        (void)tap_object(find_row_button(prefs, "Brightness", "-"));
    }
    expect(g_brightness == 10, "it stops at 10%, where - has nowhere to go");
    g_brightness = 80;

    // The clock: 24 or 12 hours, and the seconds.
    expect(tap_object(find_row_button(prefs, "Clock hours", "12 hour")), "the clock row has a 12 hour button");
    expect(g_wall.format.twelve_hour && !g_wall.format.seconds, "a tap on 12 hour chooses the 12-hour clock");
    expect(tap_object(find_row_button(prefs, "Clock seconds", "On")), "the seconds row has an On button");
    expect(g_wall.format.twelve_hour && g_wall.format.seconds, "a tap on On adds the seconds and keeps the hours");
    expect(tap_object(find_row_button(prefs, "Clock hours", "24 hour")), "the clock row has a 24 hour button");
    expect(!g_wall.format.twelve_hour && g_wall.format.seconds, "a tap on 24 hour keeps the seconds");
    expect(tap_object(find_row_button(prefs, "Clock seconds", "Off")), "the seconds row has an Off button");
    expect(!g_wall.format.seconds, "a tap on Off takes the seconds away");

    // The night schedule: a switch, and a pair of times in an editor.
    expect(tap_object(find_row_button(prefs, "Night schedule", "On")), "the night schedule row has an On button");
    expect(g_settings.night.enabled, "a tap on On switches the schedule on");
    lv_obj_t* const times_button = find_row_button(prefs, "Night from and to", "22:00 to 07:00");
    expect(tap_object(times_button), "the night times are a button that opens their editor");
    pump(4);
    lv_obj_t* const times = player::panel::editor_root();
    expect(times != nullptr, "a tap on the times opens the editor");
    if (times != nullptr) {
        write_bmp(out / ("prefs-night-times-" + size + ".bmp"));
        failures += audit(times, "night times editor " + size);
        for (const char* shown : {"From", "To", "22:00", "07:00", "Done"}) {
            expect(shows_text(times, shown), std::string("the times editor shows ") + shown);
        }
        // A bar this narrow keeps what it can of its title.
        expect(width < 300 || shows_text(times, "Night times"), "and its title");
        expect(shows_text(times, "Dark from 22:00 until 07:00, through midnight", true), "and says what it means");
        const std::vector<lv_obj_t*> plus = find_buttons_in(times, "+");
        const std::vector<lv_obj_t*> minus = find_buttons_in(times, "-");
        expect(plus.size() == 2 && minus.size() == 2, "each time has a - and a +");
        if (plus.size() == 2 && minus.size() == 2) {
            (void)tap_object(plus[0]);
            expect(shows_text(times, "22:15"), "+ on the first time steps a quarter of an hour");
            expect(g_settings.night.from_min == 22 * 60, "and nothing is written until the steps stop");
            g_now_us += 700'000;
            pump(3);
            expect(g_settings.night.from_min == 22 * 60 + 15, "a moment after the last step it is written");
            expect(g_settings.night.enabled, "and the schedule stays on");
            (void)tap_object(minus[1]);
            g_now_us += 700'000;
            pump(3);
            expect(g_settings.night.to_min == 6 * 60 + 45, "- on the second time steps it back a quarter of an hour");
            // A held button keeps stepping, and goes faster.
            lv_area_t area;
            lv_obj_get_coords(plus[1], &area);
            g_pointer = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2, true};
            pump(45);
            g_pointer.down = false;
            pump(4);
            g_now_us += 700'000;
            pump(3);
            const int held = g_settings.night.to_min;
            std::printf("  held for 1.5 s, the end time went from 06:45 to %s\n", nv::time_of_day_text(held).c_str());
            expect(held != 6 * 60 + 45 + 15, "a held button steps more than once");
            expect(shows_text(times, nv::time_of_day_text(held)), "and the editor shows what was written");
            // Equal times are said to mean no night at all.
            g_settings.night.from_min = 6 * 60;
            g_settings.night.to_min = 6 * 60;
            g_now_us += 2'000'000;
            pump(4);
            expect(shows_text(times, "never dark", true), "the same time twice says the screen is never dark");
            g_settings.night.from_min = 22 * 60;
            g_settings.night.to_min = 7 * 60;
            g_now_us += 2'000'000;
            pump(4);
        }
        expect(tap_button("Done"), "the editor has a Done button");
        pump(4);
        expect(player::panel::editor_root() == nullptr, "Done puts the editor away");
        expect(shows_text(prefs, "22:00 to 07:00"), "and the row says the times");
    }
    g_settings.night = {};

    // A zone set through a page that kept only the string (Melbourne's rule) reads as the place the table holds for it, and
    // the editor opens at it as if it had been picked; one in no table reads as its offset. Never the string.
    {
        const auto refresh = [&] {
            g_now_us += 1'500'000;
            pump(4);
        };
        g_zone = {.posix = "AEST-10AEDT,M10.1.0,M4.1.0/3", .name = ""};
        refresh();
        expect(shows_text(prefs, "Melbourne (Australia)"), "a zone set by its string alone reads as its place");
        expect(!shows_text(prefs, "AEST", true), "and the string is nowhere on the sheet");
        if (lv_obj_t* const label = find_label(prefs, "Time zone")) {
            scroll_label_to_top(label);
            pump(2);
            write_bmp(out / ("prefs-time-posix-" + size + ".bmp"));
        }
        expect(tap_object(find_row_button(prefs, "Time zone", "Melbourne (Australia)")), "its row opens the editor");
        pump(4);
        lv_obj_t* const at_place = player::panel::editor_root();
        expect(at_place != nullptr, "and the editor opens");
        if (at_place != nullptr) {
            expect(shows_text(at_place, "Now: Melbourne (Australia)"), "saying where the board is, as a place");
            expect(shows_text(at_place, "Sydney"), "among the places of its region");
            expect(!shows_text(at_place, "AEST", true), "and not by its string");
            write_bmp(out / ("prefs-zone-posix-" + size + ".bmp"));
            expect(tap_button("Done"), "with a Done");
            pump(4);
        }
        g_zone = {.posix = "ZZZ-9:45", .name = ""};
        refresh();
        expect(shows_text(prefs, "UTC+09:45 (custom)"), "a zone in no table reads as its offset");
        expect(!shows_text(prefs, "ZZZ", true), "and not as its string");
        if (lv_obj_t* const label = find_label(prefs, "Time zone")) {
            scroll_label_to_top(label);
            pump(2);
            write_bmp(out / ("prefs-time-custom-" + size + ".bmp"));
        }
        expect(tap_object(find_row_button(prefs, "Time zone", "UTC+09:45 (custom)")), "its row opens the editor too");
        pump(4);
        lv_obj_t* const custom = player::panel::editor_root();
        expect(custom != nullptr, "and it opens");
        if (custom != nullptr) {
            expect(shows_text(custom, "Now: UTC+09:45 (custom)"), "saying what the zone is");
            expect(!shows_text(custom, "ZZZ", true), "and not by its string");
            expect(tap_button("Done"), "and has its Done");
            pump(4);
        }
        g_zone = {};
        refresh();
    }

    // The time zone: the regions, then the places in one, opening where the zone already is.
    lv_obj_t* const zone_button = find_row_button(prefs, "Time zone", "UTC");
    if (lv_obj_t* const label = find_label(prefs, "Time zone")) {
        lv_obj_scroll_to_view_recursive(label, LV_ANIM_OFF);
        pump(2);
        write_bmp(out / ("prefs-time-" + size + ".bmp"));
    }
    expect(tap_object(zone_button), "the time zone is a button that opens its editor");
    pump(4);
    lv_obj_t* zone = player::panel::editor_root();
    expect(zone != nullptr, "a tap on the zone opens the editor");
    if (zone != nullptr) {
        write_bmp(out / ("prefs-zone-regions-" + size + ".bmp"));
        failures += audit(zone, "time zone regions " + size);
        for (const char* shown : {"No zone (UTC)", "Australia", "America", "Europe", "Done", "Now: UTC"}) {
            expect(shows_text(zone, shown), std::string("the regions list shows ") + shown);
        }
        expect(width < 300 || shows_text(zone, "Time zone"), "and its title");
        expect(back_buttons(zone).empty(), "and has no way back up from the top");
        expect(tap_object(find_buttons_in(zone, "Australia").empty() ? nullptr : find_buttons_in(zone, "Australia").front()),
               "Australia is a region to open");
        pump(4);
        expect(shows_text(zone, "Melbourne"), "the places of Australia include Melbourne");
        write_bmp(out / ("prefs-zone-cities-" + size + ".bmp"));
        failures += audit(zone, "time zone places " + size);
        expect(!back_buttons(zone).empty(), "and there is a way back to the regions");
        const std::vector<lv_obj_t*> melbourne = find_buttons_in(zone, "Melbourne");
        expect(!melbourne.empty() && tap_object(melbourne.front()), "Melbourne is a place to choose");
        expect(g_zone.name == "Australia/Melbourne", "a tap on Melbourne chooses Australia/Melbourne");
        expect(g_zone.posix == "AEST-10AEDT,M10.1.0,M4.1.0/3", "with the rule that name stands for");
        pump(4);
        expect(shows_text(zone, "Now: Melbourne (Australia)"), "and the editor says where the board is now");
        const std::vector<lv_obj_t*> regions = back_buttons(zone);
        expect(!regions.empty() && tap_object(regions.front()), "the way back goes up to the regions");
        pump(4);
        expect(shows_text(zone, "No zone (UTC)"), "to the list of regions");
        // The longest list: the places of America, a screen of them at a time.
        const std::vector<lv_obj_t*> america = find_buttons_in(zone, "America");
        expect(!america.empty() && tap_object(america.front()), "America is a region to open");
        pump(4);
        failures += audit(zone, "time zone places, America " + size);
        expect(shows_text(zone, "New York"), "and has New York");
        expect(tap_button("Done"), "the zone editor has a Done button");
        pump(4);
        expect(player::panel::editor_root() == nullptr, "Done puts it away");
        expect(shows_text(prefs, "Melbourne (Australia)"), "and the row says where the board is");
        // Opened again it is where the zone is: the places in its region, and the board's own city among them.
        expect(tap_object(find_row_button(prefs, "Time zone", "Melbourne (Australia)")), "the zone row opens it again");
        pump(4);
        zone = player::panel::editor_root();
        expect(zone != nullptr && (width < 300 || shows_text(zone, "Australia")) && shows_text(zone, "Sydney"),
               "at the places of the region the zone is in");
        write_bmp(out / ("prefs-zone-reopened-" + size + ".bmp"));
        expect(tap_button("Done"), "and has its Done");
        pump(4);
        // None again: No zone.
        g_zone = {};
    }
    (void)width;
    (void)height;
    return failures;
}

// --- Sound: the Identify speakers button and the Speakers sheet (planning/hearth-speaker-trim-identify.md) --------------------

namespace st = iclforge::render::speaker_trim;

// The board's sound answers for a room: a role at each output of the layout, the trims and the tone at rest.
void refresh_sound_trims() {
    std::uint16_t farthest = 0;
    for (player::SoundSheet::Speaker& speaker : g_sound.speakers) {
        speaker.trim = g_sound.table.get(speaker.role);
        farthest = std::max(farthest, speaker.trim.cm);
    }
    g_sound.farthest_cm = farthest;
    for (player::SoundSheet::Speaker& speaker : g_sound.speakers) {
        speaker.applied_level_db = static_cast<float>(speaker.trim.level_db());
        const std::uint32_t extra = speaker.trim.cm == 0 || farthest == 0 ? 0U : static_cast<std::uint32_t>(farthest - speaker.trim.cm);
        speaker.applied_delay_samples = st::delay_samples_for(extra, player::kSinkSampleRate);
    }
}

void set_room(const char* text) {
    const auto layout = iclforge::render::OutputLayout::parse(text);
    if (!layout) {
        std::fprintf(stderr, "no such layout: %s\n", text);
        std::exit(2);
    }
    g_sound.player = true;
    g_sound.layout = text;
    g_sound.speakers.clear();
    for (std::size_t slot = 0; slot < layout->slots(); ++slot) {
        const st::RoleCode role = st::role_of_slot(*layout, slot);
        if (role != st::kNoRole) {
            player::SoundSheet::Speaker speaker;
            speaker.role = role;
            speaker.output = static_cast<std::uint8_t>(slot);
            g_sound.speakers.push_back(speaker);
        }
    }
    g_sound.delay = true;
    g_sound.delay_reason.clear();
    g_sound.max_span_cm = 686;
    refresh_sound_trims();
}

// Every touch target under `root` that says `text`, in tree order.
std::vector<lv_obj_t*> buttons_in(lv_obj_t* root, const std::string& text) {
    std::vector<lv_obj_t*> targets;
    collect_touch_targets(root, targets);
    std::vector<lv_obj_t*> found;
    for (lv_obj_t* const target : targets) {
        if (button_text(target) == text) {
            found.push_back(target);
        }
    }
    return found;
}

// A tap on the `nth` button under `root` that says `text`, scrolled into view first.
bool tap_in(lv_obj_t* root, const std::string& text, std::size_t nth = 0) {
    const std::vector<lv_obj_t*> found = buttons_in(root, text);
    if (nth >= found.size()) {
        std::printf("  no button \"%s\" (%zu) is shown\n", text.c_str(), nth);
        return false;
    }
    show_in_scroller(found[nth]);
    lv_obj_update_layout(found[nth]);
    lv_area_t area;
    lv_obj_get_coords(found[nth], &area);
    tap((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
    return true;
}

// A button of the bar at the foot of the Speakers sheet, by the long name or by the short one it has where the long ones do not
// fit one line (a 320 by 240 and a 240 by 240 module).
std::string bar_name(lv_obj_t* sheet, const std::string& long_name) {
    const std::string shorter = long_name == "Repeat" ? "Loop" : long_name == "Play each" ? "All" : long_name;
    return buttons_in(sheet, long_name).empty() ? shorter : long_name;
}

// The list that scrolls under `root`, or none.
lv_obj_t* find_scroller(lv_obj_t* root) {
    const std::uint32_t count = lv_obj_get_child_count(root);
    for (std::uint32_t i = 0; i < count; ++i) {
        lv_obj_t* const child = lv_obj_get_child(root, static_cast<std::int32_t>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE)) {
            return child;
        }
    }
    return nullptr;
}

// audit() lets a list that scrolls run off the glass, as a list may, and so it let a bar of buttons at the end of the Speakers
// sheet be below the screen on the kit (a 9.1.6 room is taller than a 5.1 one, and the five buttons went after it). This is the
// check that a control cannot hide there: every touch target of the sheet is on the glass, and a control that is not in the list
// (the bars at the top and the foot) is wholly on the screen at any size; one that is in the list is wholly inside the part of
// the list that shows, with the list at rest, where `list_must_fit` says the room and the speaker are to be seen without a finger.
// Prints how far a list runs past its glass, so that a size that scrolls says so. Returns the failures.
int audit_on_glass(lv_obj_t* root, const std::string& what, bool list_must_fit) {
    int failures = 0;
    lv_obj_t* const list = find_scroller(root);
    if (list != nullptr) {
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    }
    lv_obj_update_layout(root);
    std::vector<lv_obj_t*> targets;
    collect_touch_targets(root, targets);
    for (lv_obj_t* const target : targets) {
        const Rect r = rect_of(target);
        lv_obj_t* const holder = scroller_above(target, root);
        const Rect glass = holder != nullptr ? rect_of(holder) : Rect{0, 0, g_width - 1, g_height - 1};
        if (holder != nullptr && !list_must_fit) {
            continue;
        }
        if (r.x1 < glass.x1 || r.x2 > glass.x2 || r.y1 < glass.y1 || r.y2 > glass.y2) {
            std::string name = button_text(target);
            std::printf("  FAIL %s: \"%s\" is not on the glass (%d,%d)-(%d,%d) where the %s shows (%d,%d)-(%d,%d)\n", what.c_str(),
                        name.c_str(), r.x1, r.y1, r.x2, r.y2, holder != nullptr ? "list" : "screen", glass.x1, glass.y1, glass.x2,
                        glass.y2);
            ++failures;
        }
    }
    if (list != nullptr) {
        std::printf("  %-26s the list runs %d px past its glass\n", what.c_str(), static_cast<int>(lv_obj_get_scroll_bottom(list)));
    }
    return failures;
}

// Whether the speaker `id` is lit on the sheet: its shape is the accent's, as the Back button is.
bool is_lit(lv_obj_t* sheet, const std::string& id) {
    const std::vector<lv_obj_t*> node = buttons_in(sheet, id);
    const std::vector<lv_obj_t*> back = buttons_in(sheet, "Back");
    return !node.empty() && !back.empty() &&
           lv_color_to_u32(lv_obj_get_style_bg_color(node.front(), LV_PART_MAIN)) ==
               lv_color_to_u32(lv_obj_get_style_bg_color(back.front(), LV_PART_MAIN));
}

// The Sound group of the Preferences sheet that is open, and the Speakers sheet it opens, at one size. Returns the number of
// failures, having said each.
int check_sound_group(int width, int height, const fs::path& out, const std::string& size) {
    int failures = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::printf("  FAIL %s: %s\n", size.c_str(), what.c_str());
            ++failures;
        }
    };
    lv_obj_t* const prefs = player::panel::prefs_root();
    if (prefs == nullptr) {
        expect(false, "the Preferences sheet is open for the Sound group");
        return failures;
    }

    // The list: the group, the button, and what is set.
    g_sound = {};
    g_tone_requests.clear();
    g_trim_requests.clear();
    set_room("5.1");
    pump(8);
    for (const char* shown : {"SOUND", "Identify speakers", "Speaker levels and distances", "none set"}) {
        expect(shows_text(prefs, shown), std::string("Preferences shows ") + shown);
    }
    expect(shows_text(prefs, "lights the one you should hear", true), "the Identify row says what it does");
    {
        // The Sound group is some way down the list: scrolled to, for the picture of it.
        const std::vector<lv_obj_t*> identify = buttons_in(prefs, "Identify");
        if (!identify.empty()) {
            lv_obj_scroll_to_view_recursive(identify.front(), LV_ANIM_OFF);
            // ... and a half screen further, so that the row is in the middle and what follows it is in the picture too.
            lv_obj_t* const list = find_scroller(prefs);
            if (list != nullptr) {
                lv_obj_scroll_by(list, 0, -(height / 2), LV_ANIM_OFF);
            }
        }
        pump(2);
        write_bmp(out / ("prefs-sound-" + size + ".bmp"));
    }

    // A song is playing: the board says so under the row, the tone does not start, and the sheet is there to say why.
    g_sound.tone_blocked = "A song is playing: the test tone waits until it stops.";
    g_now_us += 1'000'000;  // the list looks at the board once a second
    pump(8);
    expect(shows_text(prefs, "A song is playing", true), "a song playing is said under the Identify row");
    tap_in(prefs, "Identify");
    pump(8);
    lv_obj_t* sheet = player::panel::sound_sheet_root();
    expect(sheet != nullptr, "Identify opens the Speakers sheet even where the board cannot play");
    expect(!g_sound.tone_running, "and does not play over the song");
    if (sheet != nullptr) {
        expect(shows_text(sheet, "A song is playing", true), "the sheet says why");
        tap_in(sheet, "Back");
        pump(8);
    }
    g_sound.tone_blocked.clear();
    pump(8);

    // Identify speakers is the Speakers sheet, with the sequence running: every speaker has its ID on it, and the one the tone
    // is on is lit.
    expect(tap_in(prefs, "Identify"), "the Identify row has an Identify button");
    pump(8);
    sheet = player::panel::sound_sheet_root();
    expect(sheet != nullptr, "Identify opens the Speakers sheet");
    if (sheet == nullptr) {
        return failures;
    }
    expect(!g_tone_requests.empty() && g_tone_requests.back() == "all", "and asks for every speaker in turn");
    expect(g_sound.tone_running, "so the tone is running");
    for (const char* role : {"L", "C", "R", "Ls", "Rs", "LFE"}) {
        expect(!buttons_in(sheet, role).empty(), std::string("the room has the ") + role + " speaker, with its ID on it");
    }
    for (const player::SoundSheet::Speaker& speaker : g_sound.speakers) {
        g_sound.tone_role = speaker.role;
        pump(8);
        for (const player::SoundSheet::Speaker& other : g_sound.speakers) {
            const std::string id(st::role_name(other.role));
            expect(is_lit(sheet, id) == (other.role == speaker.role),
                   "while the tone is on " + std::string(st::role_name(speaker.role)) + ", " + id +
                       (other.role == speaker.role ? " is lit" : " is not"));
        }
    }
    g_sound.tone_role = *st::parse_role("Ls");
    pump(8);
    write_bmp(out / ("speakers-lit-" + size + ".bmp"));
    failures += audit(sheet, "speakers " + size);
    failures += audit_on_glass(sheet, "speakers on glass " + size, width >= 1024);
    // Stop is a button of the bar, and the controls are still there: the sheet is not only a Stop.
    expect(tap_in(sheet, "Stop"), "the bar has a Stop button");
    expect(g_tone_requests.back() == "off" && !g_sound.tone_running, "Stop stops the tone");
    pump(8);
    expect(shows_text(sheet, "Level") && shows_text(sheet, "Distance"), "and the level and the distance are still there");
    expect(player::panel::sound_sheet_root() != nullptr, "and so is the sheet");
    tap_in(sheet, "Back");
    pump(8);
    expect(player::panel::sound_sheet_root() == nullptr, "Back puts the sheet away");
    expect(player::panel::prefs_root() != nullptr, "the Preferences list is under it");

    // The worst case, which is what a person on the kit meets: every layout the sheet draws, a speaker lit in each, and the
    // bars on the glass in all of them. A room that is taller than the glass lets the list scroll; the bars never move.
    for (const char* layout : {"2.0", "5.1", "7.1.4", "9.1.6"}) {
        set_room(layout);
        g_sound.tone_running = true;
        g_sound.tone_role = g_sound.speakers.back().role;
        pump(8);
        expect(tap_in(prefs, "Edit"), std::string("the Edit button is there for ") + layout);
        pump(8);
        lv_obj_t* const room = player::panel::sound_sheet_root();
        expect(room != nullptr, std::string("the sheet opens for ") + layout);
        if (room == nullptr) {
            continue;
        }
        const std::string what = std::string("speakers ") + layout + " " + size;
        for (const player::SoundSheet::Speaker& speaker : g_sound.speakers) {
            expect(!buttons_in(room, std::string(st::role_name(speaker.role))).empty(),
                   std::string(layout) + " has " + std::string(st::role_name(speaker.role)) + " with its ID on it");
        }
        // The speaker the tone is on, and no other, is lit.
        int lit = 0;
        for (const player::SoundSheet::Speaker& speaker : g_sound.speakers) {
            lit += is_lit(room, std::string(st::role_name(speaker.role))) ? 1 : 0;
        }
        expect(lit == 1, std::string(layout) + " has exactly one speaker lit");
        failures += audit(room, what);
        // The kit's panel shows the whole sheet without a finger, the 9.1.6 room among them; an 800 by 480 one, all but a 9.1.6;
        // the smaller ones scroll what is between the bars.
        const bool whole = width >= 1024 || (width >= 800 && std::string(layout) != "9.1.6");
        failures += audit_on_glass(room, what + " on glass", whole);
        for (const char* label : {"Play", "Repeat", "Play each", "Stop", "Reset"}) {
            expect(!buttons_in(room, bar_name(room, label)).empty(), what + " has the " + label + " button");
        }
        write_bmp(out / (std::string("speakers-") + layout + "-lit-" + size + ".bmp"));
        tap_in(room, "Back");
        pump(8);
    }
    g_sound.tone_running = false;
    g_sound.tone_role = st::kNoRole;
    set_room("5.1");
    pump(8);

    // The Speakers sheet, for the speaker trim.
    expect(tap_in(prefs, "Edit"), "the speaker trim row has an Edit button");
    pump(8);
    sheet = player::panel::sound_sheet_root();
    expect(sheet != nullptr, "Edit opens the Speakers sheet");
    if (sheet == nullptr) {
        return failures;
    }
    write_bmp(out / ("speakers-" + size + ".bmp"));
    for (const char* shown : {"Speakers", "Level", "Distance", "Front left", "0 dB", "not set"}) {
        expect(shows_text(sheet, shown, std::string(shown) == "Front left"), std::string("the sheet shows ") + shown);
    }

    // A tap on a speaker chooses it and plays the tone through it.
    expect(tap_in(sheet, "Ls"), "a speaker of the room can be tapped");
    expect(g_tone_requests.back() == "Ls", "a tap on Ls plays Ls");
    pump(8);
    expect(shows_text(sheet, "Left surround", true), "and the sheet is about the left surround");
    expect(is_lit(sheet, "Ls") && !is_lit(sheet, "Rs"), "the speaker the tone is on is lit and the others are not");
    g_sound.tone_role = *st::parse_role("Rs");
    pump(8);
    expect(is_lit(sheet, "Rs") && !is_lit(sheet, "Ls"), "and the light follows the tone to the next speaker");

    // The level: a half decibel a step, in both directions, and the sheet says what it is.
    tap_in(sheet, "+", 0);
    pump(4);
    expect(g_trim_requests.back() == "Ls level=0.5", "+ asks for half a decibel more");
    expect(shows_text(sheet, "+0.5 dB"), "and the sheet says +0.5 dB");
    tap_in(sheet, "-", 0);
    tap_in(sheet, "-", 0);
    pump(4);
    expect(g_trim_requests.back() == "Ls level=-0.5", "- twice goes to half a decibel less");
    expect(shows_text(sheet, "-0.5 dB"), "and the sheet says -0.5 dB");
    for (int i = 0; i < 30; ++i) {
        tap_in(sheet, "-", 0);
    }
    pump(4);
    expect(g_sound.table.get(*st::parse_role("Ls")).half_db == -24, "the level stops at -12 dB");
    expect(shows_text(sheet, "-12 dB"), "and the sheet says so");
    write_bmp(out / ("speakers-level-" + size + ".bmp"));

    // The distance: from nothing it starts at three metres, by a tenth or a metre a step.
    tap_in(sheet, "+", 1);
    pump(4);
    expect(g_trim_requests.back() == "Ls distance=3.00", "+ on a distance that is not set starts it at 3 m");
    expect(shows_text(sheet, "3 m"), "and the sheet says 3 m");
    tap_in(sheet, "+1 m");
    tap_in(sheet, "+", 1);
    pump(4);
    expect(g_trim_requests.back() == "Ls distance=4.10", "+1 m then + is 4.1 m");
    expect(shows_text(sheet, "4.1 m"), "and the sheet says 4.1 m");
    // Another speaker starts where the farthest is, which moves nothing yet.
    tap_in(sheet, "R");
    pump(4);
    tap_in(sheet, "+", 1);
    pump(4);
    expect(g_trim_requests.back() == "R distance=4.10", "a second speaker starts at the farthest distance");
    tap_in(sheet, "-1 m");
    tap_in(sheet, "-1 m");
    tap_in(sheet, "-1 m");
    pump(4);
    expect(g_trim_requests.back() == "R distance=1.10", "and moves by metres");
    write_bmp(out / ("speakers-distance-" + size + ".bmp"));
    // Past what the delay lines hold, the board says no and the sheet says why, where the person looks (above the controls).
    tap_in(sheet, "Ls");
    for (int i = 0; i < 6; ++i) {
        tap_in(sheet, "+1 m");
    }
    pump(4);
    expect(g_sound.table.get(*st::parse_role("Ls")).cm <= 1100 + 686, "the board keeps the speakers within what its delay lines hold");
    if (width >= 480) {
        expect(shows_text(sheet, "at most 6.86 m", true), "and the sheet says how far apart they can be");
        write_bmp(out / ("speakers-refused-" + size + ".bmp"));
    }
    tap_in(sheet, "R");
    pump(4);

    // Repeat, Play each and Stop.
    tap_in(sheet, bar_name(sheet, "Repeat"));
    expect(g_tone_requests.back() == "R loop", "Repeat asks for the chosen speaker over and over");
    pump(4);
    tap_in(sheet, bar_name(sheet, "Repeat"));
    expect(g_tone_requests.back() == "off", "and Repeat again stops it");
    tap_in(sheet, bar_name(sheet, "Play each"));
    expect(g_tone_requests.back() == "all", "Play each plays every speaker in turn");
    pump(4);
    tap_in(sheet, "Stop");
    expect(g_tone_requests.back() == "off", "Stop stops it");

    // Reset puts the chosen speaker back.
    tap_in(sheet, "Reset");
    pump(4);
    expect(g_trim_requests.back() == "R reset", "Reset puts the chosen speaker back");
    expect(g_sound.table.get(*st::parse_role("R")).neutral(), "to 0 dB and no distance");

    // A song starts: the tone buttons say why they do nothing.
    g_sound.tone_blocked = "A song is playing: the test tone waits until it stops.";
    pump(8);
    expect(shows_text(sheet, "A song is playing", true), "the sheet says a song is playing");
    write_bmp(out / ("speakers-blocked-" + size + ".bmp"));
    const std::size_t before = g_tone_requests.size();
    tap_in(sheet, "Play");
    pump(4);
    expect(!g_sound.tone_running, "and Play does not play over it");
    expect(g_tone_requests.size() == before + 1, "(the board was asked)");
    g_sound.tone_blocked.clear();

    // The layout changes under the sheet: it is drawn again for the room it is now.
    set_room("7.1.4");
    pump(8);
    expect(player::panel::sound_sheet_root() != nullptr, "the sheet is still up when the layout changes");
    for (const char* role : {"Lrs", "Rrs", "Vhl", "Vhr"}) {
        expect(!buttons_in(player::panel::sound_sheet_root(), role).empty(), std::string("7.1.4 has the ") + role + " speaker to touch");
    }

    // A board with no delay lines keeps the level and greys the distance, with its reason.
    g_sound.delay = false;
    g_sound.delay_reason = "This board has no memory for delay lines.";
    pump(8);
    expect(shows_text(player::panel::sound_sheet_root(), "no memory for delay lines", true), "no delay lines is said on the sheet");
    write_bmp(out / ("speakers-no-delay-" + size + ".bmp"));
    failures += audit(player::panel::sound_sheet_root(), "speakers no delay " + size);
    g_sound.delay = true;
    g_sound.delay_reason.clear();
    set_room("5.1");
    pump(8);

    // A server sets the levels: the sheet says so.
    g_sound.server_level = true;
    pump(8);
    expect(shows_text(player::panel::sound_sheet_root(), "Sendspin server", true), "a server setting the levels is said on the sheet");
    g_sound.server_level = false;
    pump(8);

    // Back: the page goes, the tone with it, and the list is under it as it was.
    tap_in(player::panel::sound_sheet_root(), bar_name(player::panel::sound_sheet_root(), "Play each"));
    expect(g_sound.tone_running, "the tone is running when the sheet is left");
    expect(tap_in(player::panel::sound_sheet_root(), "Back"), "the sheet has a Back button");
    pump(8);
    expect(player::panel::sound_sheet_root() == nullptr, "Back puts the sheet away");
    expect(!g_sound.tone_running, "and the tone with it");
    expect(player::panel::prefs_root() != nullptr, "the Preferences list is under it");
    expect(shows_text(player::panel::prefs_root(), "2 speakers set", true) || shows_text(player::panel::prefs_root(), "1 speaker set", true),
           "and says what is set");
    return failures;
}

// The Adjust and Preferences sheets at one display size (planning/p4x-preferences.md): drawn for the kit and for a
// board whose screen has no touch, held to a fingertip's size and to text that is not cut off (audit()), and driven by
// taps the way a person does. Returns the number of failures, having said each.
// Every touch target under `root` can be brought onto the glass: scrolled to, it is on the screen and inside every box
// that scrolls it. audit() holds the fixed controls to the screen; this holds the ones that scroll, so that a button off the
// bottom of a sheet that cannot be scrolled to it is a failure and not a surprise.
void scroll_all_to_top(lv_obj_t* object) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE)) {
        lv_obj_scroll_to(object, 0, 0, LV_ANIM_OFF);
    }
    const std::uint32_t count = lv_obj_get_child_count(object);
    for (std::uint32_t i = 0; i < count; ++i) {
        scroll_all_to_top(lv_obj_get_child(object, static_cast<std::int32_t>(i)));
    }
}

int reachable(lv_obj_t* root, const std::string& what) {
    if (root == nullptr) {
        std::printf("  FAIL %s: there is no sheet to look at\n", what.c_str());
        return 1;
    }
    lv_obj_update_layout(root);
    std::vector<lv_obj_t*> targets;
    collect_touch_targets(root, targets);
    int failures = 0;
    for (lv_obj_t* const target : targets) {
        show_in_scroller(target);
        lv_obj_update_layout(root);
        lv_area_t at;
        lv_obj_get_coords(target, &at);
        bool ok = at.x1 >= 0 && at.y1 >= 0 && at.x2 < g_width && at.y2 < g_height;
        for (lv_obj_t* parent = lv_obj_get_parent(target); parent != nullptr && ok; parent = lv_obj_get_parent(parent)) {
            if (!lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE)) {
                continue;
            }
            lv_area_t box;
            lv_obj_get_coords(parent, &box);
            ok = at.x1 >= box.x1 && at.x2 <= box.x2 && at.y1 >= box.y1 && at.y2 <= box.y2;
        }
        if (!ok) {
            std::string boxes;
            for (lv_obj_t* parent = lv_obj_get_parent(target); parent != nullptr; parent = lv_obj_get_parent(parent)) {
                if (lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE)) {
                    lv_area_t box;
                    lv_obj_get_coords(parent, &box);
                    boxes += " in (" + std::to_string(box.x1) + "," + std::to_string(box.y1) + ")-(" +
                             std::to_string(box.x2) + "," + std::to_string(box.y2) + ") scrolled " +
                             std::to_string(lv_obj_get_scroll_y(parent)) + ", " +
                             std::to_string(lv_obj_get_scroll_bottom(parent)) + " more below";
                }
            }
            std::printf("  FAIL %s: \"%s\" cannot be brought onto the glass (%d,%d)-(%d,%d)%s\n", what.c_str(),
                        button_text(target).c_str(), at.x1, at.y1, at.x2, at.y2, boxes.c_str());
            ++failures;
        }
    }
    scroll_all_to_top(root);
    lv_obj_update_layout(root);
    std::printf("  %-26s %2zu touch targets, every one can be brought onto the glass: %s\n", what.c_str(), targets.size(),
                failures == 0 ? "yes" : "NO");
    return failures;
}

// The Speaker layout sheet (layout_sheet.cpp): the Preferences row that opens it, the quick picks, the buttons that build a
// layout, what a board that cannot carry them says, the question while a stream plays, and the worst case - nine around you,
// one subwoofer and six heights, in the ceiling, on the kit - held to the glass at every size.
int check_layout_sheet(int width, int height, const fs::path& out, const std::string& lrc) {
    namespace nv = iclforge::nowview;
    const std::string size = std::to_string(width) + "x" + std::to_string(height);
    int failures = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::printf("  FAIL %s: %s\n", size.c_str(), what.c_str());
            ++failures;
        }
    };
    const auto open_sheet = [&](lv_obj_t* prefs) -> lv_obj_t* {
        if (!tap_object(find_row_button(prefs, "Speaker layout", "Change"))) {
            expect(false, "the Speaker layout row has a Change button");
            return nullptr;
        }
        pump(8);
        lv_obj_t* const sheet = player::panel::layout_sheet_root();
        expect(sheet != nullptr, "Change opens the Speaker layout sheet");
        return sheet;
    };
    // Scrolled to the top, as a person finds it when the sheet opens (a tap on a button scrolled to it before).
    const auto settle = [&](lv_obj_t* root) {
        scroll_all_to_top(root);
        lv_obj_update_layout(root);
        pump(2);
    };
    const auto close_prefs = [&] {
        g_now_playing = false;
        g_settings = {nv::View::kSpeakers, 5};
        hold(lrc, 5'750);
        pump(6);
        tap_button("Close");
        pump(6);
    };

    // A board that can carry only some of the layouts (an ESP32-S3, four 32-bit slots on one line).
    g_caps = iclforge::variants::s3_spi_320x240();
    g_caps.display.width = width;
    g_caps.display.height = height;
    g_layout = "2.0";
    g_settings = {nv::View::kSpeakers, 5};
    g_now_playing = false;
    expect(tap_button("Preferences"), "the footer has a Preferences button on an S3");
    pump(8);
    lv_obj_t* const prefs = player::panel::prefs_root();
    expect(prefs != nullptr, "Preferences opens on an S3");
    if (prefs == nullptr) {
        return failures;
    }
    failures += audit(prefs, "preferences, speaker layout row " + size);
    expect(shows_text(prefs, "Speaker layout"), "the Sound group has the speaker layout");
    expect(shows_text(prefs, "Two around you.", true), "and says in words what is in use");
    lv_obj_t* sheet = open_sheet(prefs);
    if (sheet == nullptr) {
        return failures;
    }
    settle(sheet);
    write_bmp(out / ("layout-quick-s3-" + size + ".bmp"));
    failures += audit(sheet, "speaker layout, quick picks, S3 " + size);
    failures += reachable(sheet, "speaker layout, quick picks, S3 " + size);
    for (const char* shown : {"Speaker layout", "Quick picks", "Build yours", "Around you", "With heights"}) {
        expect(shows_text(sheet, shown), std::string("the sheet shows ") + shown);
    }
    expect(shows_text(sheet, "This board carries up to 4 speakers (32-bit slots, one line); 16-bit slots would carry 8.", true),
           "the sheet says what the board carries and what would carry more");
    for (const char* chip : {"1.0", "2.0", "2.1", "3.1", "5.1", "7.1", "9.1", "5.1.2", "7.1.4", "9.1.4", "9.1.6"}) {
        expect(!buttons_in(sheet, chip).empty(), std::string("the quick picks have ") + chip);
    }
    expect(!buttons_in(sheet, "Apply").empty() && !buttons_in(sheet, "Cancel").empty(), "Apply and Cancel are always there");

    // A layout the board cannot carry is greyed: a tap says why and changes nothing.
    expect(tap_in(sheet, "5.1"), "5.1 can be touched");
    pump(6);
    expect(g_layout == "2.0", "a tap on a layout the board cannot carry changes nothing");
    expect(shows_text(sheet, "Not available on this board"), "and the card says so");
    expect(shows_text(sheet, "5.1 needs 6 slots; this board's output carries 4 (32-bit slots, one line).", true),
           "with what it needs and what the board has");
    expect(shows_text(sheet, "16-bit slots would carry 8", true), "and what would help");
    settle(sheet);
    write_bmp(out / ("layout-unavailable-s3-" + size + ".bmp"));
    failures += audit(sheet, "speaker layout, a greyed layout, S3 " + size);
    expect(tap_in(sheet, "Apply"), "Apply can be touched");
    pump(6);
    expect(g_layout == "2.0", "and applies nothing while the working layout is the one in use");

    // One the board carries with a limit is taken.
    expect(tap_in(sheet, "2.1"), "2.1 can be touched");
    pump(6);
    expect(shows_text(sheet, "Works, with a limit"), "a layout nobody has measured on this board says it has a limit");
    expect(shows_text(sheet, "Not verified", true), "and says what");
    settle(sheet);
    write_bmp(out / ("layout-limit-s3-" + size + ".bmp"));
    failures += audit(sheet, "speaker layout, a layout with a limit, S3 " + size);

    // Build yours: the same working layout, as buttons.
    expect(tap_in(sheet, "Build yours"), "the sheet has a Build yours tab");
    pump(6);
    settle(sheet);
    write_bmp(out / ("layout-build-s3-" + size + ".bmp"));
    failures += audit(sheet, "speaker layout, build yours, S3 " + size);
    failures += reachable(sheet, "speaker layout, build yours, S3 " + size);
    for (const char* heading : {"Around you", "Subwoofers", "Heights", "Heights are"}) {
        expect(shows_text(sheet, heading), std::string("Build yours has the ") + heading + " group");
    }
    expect(tap_in(sheet, "9"), "nine around you can be touched");
    pump(6);
    expect(shows_text(sheet, "Nine around you needs 9 slots", true), "nine around you says it needs nine slots");
    expect(tap_in(sheet, "Two"), "two subwoofers can be touched");
    pump(6);
    expect(shows_text(sheet, "2.2", true), "two around you and two subwoofers is 2.2");
    expect(tap_in(sheet, "High"), "heights are mounted somehow");
    pump(6);
    expect(shows_text(sheet, "Only a layout with heights has them to mount.", true), "but not when there are none");
    expect(g_layout == "2.0", "nothing was applied");
    expect(tap_in(sheet, "Apply"), "Apply can be touched on 2.2");
    pump(8);
    expect(g_layout == "2.2", "Apply makes the working layout the board's");
    expect(player::panel::layout_sheet_root() == nullptr, "and puts the sheet away");
    pump(4);
    expect(shows_text(prefs, "2.2"), "and the Preferences row follows");

    // Music playing: Apply asks first, and Cancel leaves it as it was.
    play_room("5.1", kDolbyFiveOne, kFiveOne);
    pump(4);
    sheet = open_sheet(prefs);
    if (sheet != nullptr) {
        expect(tap_in(sheet, "1.0"), "1.0 can be touched");
        pump(4);
        expect(tap_in(sheet, "Apply"), "Apply can be touched while a stream plays");
        pump(6);
        expect(g_layout == "2.2", "while a stream plays Apply asks before it changes the layout");
        expect(shows_text(sheet, "drop out for a moment", true), "and the question says what it costs");
        expect(shows_text(sheet, "Change to 1.0?", true), "and what the change is");
    settle(sheet);
    write_bmp(out / ("layout-confirm-" + size + ".bmp"));
        // The question covers the sheet (a tap cannot reach it); what is held to the fingertip rule is the question.
        lv_obj_t* const question = lv_obj_get_child(sheet, static_cast<std::int32_t>(lv_obj_get_child_count(sheet)) - 1);
        failures += audit(question, "speaker layout question " + size);
        expect(tap_in(question, "Cancel"), "the question has a Cancel");
        pump(4);
        expect(g_layout == "2.2", "Cancel leaves the layout as it was");
        expect(!shows_text(sheet, "drop out for a moment", true), "and puts the question away");
        expect(tap_in(sheet, "Apply"), "Apply asks again");
        pump(6);
        lv_obj_t* const again = lv_obj_get_child(sheet, static_cast<std::int32_t>(lv_obj_get_child_count(sheet)) - 1);
        expect(tap_in(again, "Change"), "the question has a Change");
        pump(8);
        expect(g_layout == "1.0", "Change makes it");
        expect(player::panel::layout_sheet_root() == nullptr, "and puts the sheet away");
    }
    pump(4);
    // Cancel in the bar leaves everything as it was.
    sheet = open_sheet(prefs);
    if (sheet != nullptr) {
        g_now_playing = false;
        expect(tap_in(sheet, "9.1"), "9.1 can be touched");
        expect(tap_in(sheet, "Cancel"), "the bar has a Cancel");
        pump(8);
        expect(player::panel::layout_sheet_root() == nullptr, "Cancel puts the sheet away");
        expect(g_layout == "1.0", "and changes nothing");
    }
    close_prefs();

    // A board that cannot clock a TDM frame (an ESP32-P4 below v3.0) says why and offers stereo.
    g_caps = iclforge::variants::p4_rev1();
    g_caps.display.width = width;
    g_caps.display.height = height;
    g_layout = "2.0";
    if (tap_button("Preferences")) {
        pump(8);
        lv_obj_t* const old_chip = player::panel::prefs_root();
        sheet = open_sheet(old_chip);
        if (sheet != nullptr) {
    settle(sheet);
    write_bmp(out / ("layout-p4-rev1-" + size + ".bmp"));
            failures += audit(sheet, "speaker layout, P4 rev 1 " + size);
            failures += reachable(sheet, "speaker layout, P4 rev 1 " + size);
            expect(shows_text(sheet, "so only 1.0 and 2.0 can play.", true), "a chip with no TDM clock says why");
            expect(tap_in(sheet, "2.1"), "2.1 can be touched");
            pump(4);
            expect(shows_text(sheet, "Not available on this board"), "and is not available");
            tap_in(sheet, "Cancel");
            pump(6);
        }
        close_prefs();
    }

    // A kit (sixteen 32-bit slots, a chip that clocks them) carries every layout, with no note on any, and the worst case.
    g_caps = iclforge::variants::p4_rev3_kit();
    g_caps.display.width = width;
    g_caps.display.height = height;
    g_layout = "7.1.4";
    if (tap_button("Preferences")) {
        pump(8);
        lv_obj_t* const kit = player::panel::prefs_root();
        expect(kit != nullptr, "Preferences opens on the kit");
        sheet = kit != nullptr ? open_sheet(kit) : nullptr;
        if (sheet != nullptr) {
    settle(sheet);
    write_bmp(out / ("layout-quick-kit-" + size + ".bmp"));
            failures += audit(sheet, "speaker layout, quick picks, kit " + size);
            failures += reachable(sheet, "speaker layout, quick picks, kit " + size);
            expect(!shows_text(sheet, "Not available", true), "nothing is greyed on a board that carries them all");
            expect(tap_in(sheet, "9.1.6"), "9.1.6 can be touched");
            pump(6);
            expect(tap_in(sheet, "Build yours"), "Build yours");
            pump(6);
            // High is the one rendering of the wall and up-firing: the board cannot tell them apart.
            if (width >= 800) {
                expect(shows_text(sheet, "9.1.6: nine around you, one subwoofer, six high speakers"),
                       "9.1.6 is high speakers until they are put in the ceiling");
            }
            // The worst case: nine around you, one subwoofer, six heights, in the ceiling - the longest summary, the
            // most buttons selected, the room with the most speakers, all of them overhead - at the display's size.
            expect(tap_in(sheet, "In the ceiling"), "9.1.6 can be mounted in the ceiling");
            pump(6);
    settle(sheet);
    write_bmp(out / ("layout-worst-kit-" + size + ".bmp"));
            failures += audit(sheet, "speaker layout, worst case, kit " + size);
            failures += reachable(sheet, "speaker layout, worst case, kit " + size);
            expect(shows_text(sheet, "9.1.6:top", true), "the footer names the layout");
            if (width >= 800) {
                expect(shows_text(sheet, "9.1.6:top: nine around you, one subwoofer, six ceiling speakers"),
                       "and, with room, says it in words");
            }
            // Seventeen outputs is not a layout.
            expect(tap_in(sheet, "Two"), "two subwoofers can be touched");
            pump(6);
            expect(shows_text(sheet, "9.2.6:top would be 17 outputs, and a layout has at most 16.", true),
                   "nine around you, two subwoofers and six heights is seventeen outputs");
    settle(sheet);
    write_bmp(out / ("layout-seventeen-kit-" + size + ".bmp"));
            failures += audit(sheet, "speaker layout, seventeen outputs, kit " + size);
            expect(tap_in(sheet, "One"), "one subwoofer again");
            pump(6);
            expect(tap_in(sheet, "Apply"), "Apply can be touched on the kit");
            pump(8);
            expect(g_layout == "9.1.6:top", "9.1.6 in the ceiling is taken on the kit");
            expect(player::panel::layout_sheet_root() == nullptr, "and the sheet goes away");
            pump(4);
            expect(shows_text(kit, "9.1.6:top"), "and the row shows it");
            expect(shows_text(kit, "Nine around you, one subwoofer, six ceiling speakers.", true) ||
                       shows_text(kit, "Nine around you", true),
                   "in words");
            settle(kit);
            failures += audit(kit, "preferences, speaker layout row, kit " + size);
        }
        close_prefs();
    }

    // A layout the board was told as ":upfiring" is High, and the sheet says High is in use.
    g_caps = iclforge::variants::p4_rev3_kit();
    g_caps.display.width = width;
    g_caps.display.height = height;
    g_layout = "7.1.4:upfiring";
    if (tap_button("Preferences")) {
        pump(8);
        lv_obj_t* const alias = player::panel::prefs_root();
        sheet = alias != nullptr ? open_sheet(alias) : nullptr;
        if (sheet != nullptr) {
            expect(shows_text(sheet, "In use now"), "7.1.4:upfiring is the high 7.1.4, in use now");
            tap_in(sheet, "Cancel");
            pump(6);
        }
        close_prefs();
    }

    // A speaker list a person wrote is shown as it was, and choosing a layout replaces it.
    g_caps = iclforge::variants::p4_rev3_kit();
    g_caps.display.width = width;
    g_caps.display.height = height;
    g_layout = "L,R,C,LFE,Ls,Rs";
    if (tap_button("Preferences")) {
        pump(8);
        lv_obj_t* const listed = player::panel::prefs_root();
        expect(shows_text(listed, "L,R,C,LFE,Ls,Rs"), "a speaker list is shown as it was written");
        sheet = listed != nullptr ? open_sheet(listed) : nullptr;
        if (sheet != nullptr) {
            expect(shows_text(sheet, "This board is set to a speaker list", true), "and the sheet says so");
    settle(sheet);
    write_bmp(out / ("layout-list-kit-" + size + ".bmp"));
            failures += audit(sheet, "speaker layout, a speaker list, kit " + size);
            tap_in(sheet, "Cancel");
            pump(6);
        }
        close_prefs();
    }
    g_layout = "2.0";
    return failures;
}

// The Meters view at a display size, in each room the product has (planning/p4x-meters-view.md): the representation the rule gives for
// the width and the room's outputs, twenty updates a second, no fall to ten, and the pixels LVGL flushes a second, which is what keeps
// the invalid-area table at 256 honest: with LVGL's 32 every spectrum update redraws the whole glass (11 to 12 million pixels a second
// at 1024 by 600), and a change that lost the 256 would show here as a number several times the bound. One picture a room.
int check_meters(int width, int height, const fs::path& out, lv_display_t*& disp) {
    namespace nv = iclforge::nowview;
    namespace mv = iclforge::meters;
    const std::string size = std::to_string(width) + "x" + std::to_string(height);
    std::printf("-- meters %s\n", size.c_str());
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
    g_caps = iclforge::variants::p4_rev3_kit();
    g_caps.display.width = width;
    g_caps.display.height = height;
    set_up_system_fixture();
    g_settings = {nv::View::kMeters, 5};
    g_board = {};
    g_board.name = "hearth-4f2a91";
    g_board.address = "192.168.1.50";
    g_board.network = "HomeNet";
    g_board.firmware = "0.11.0";
    g_board.player = true;
    g_board.stream = "bursts";
    // What an update may cost LVGL, in the two things the kit's first run showed it pays for. AREAS: every area is a pass over the object tree and a
    // copy to the frame buffer (about a millisecond on the P4), and the first drawing, an object for each part of each bar, left 100 to 200 of them
    // an update (130 to 440 ms): the charts are one buffer each, and an update is a few areas, at most 12 flushes (a chart's rows through the
    // 40-row render buffers). PIXELS: the rows a change touches, across the chart, are copied whole, which is more than the bars' own pixels (up to 7
    // screens' worth a second at the worst, a 2.0 room whose spectrum has the screen); a screen redrawn in full every update is 20.
    constexpr double kMaxAreasAnUpdate = 12.0;
    const double bound_mpx = 7.0 * static_cast<double>(width) * static_cast<double>(height) / 1.0e6;

    struct Room {
        const char* layout;
        nv::StreamFacts facts;
        const std::vector<Level>* levels;
        int kbps;
        int outputs;  // connected, not the LFE alone: what representation_for() is given
    };
    const Room rooms[] = {
        {"2.0", {.codec = nv::Codec::kPcm}, &kStereo, 0, 2},
        {"5.1", kDolbyFiveOne, &kFiveOne, 640, 6},
        {"7.1.4", {.codec = nv::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true, .objects = 15}, &kSevenOneFour, 768, 12},
        {"9.1.6", kDolbyFiveOne, &kNineOneSix, 640, 16},
    };
    for (const Room& room : rooms) {
        const std::string what = std::string(room.layout) + " at " + size;
        play_room(room.layout, room.facts, *room.levels, room.kbps);
        if (room.facts.codec == nv::Codec::kPcm) {
            say_no_title();
        }
        g_now_us += 3'000'000;
        pump(6);
        const std::uint64_t before = g_flushed_pixels;
        const std::uint32_t areas_before = g_flushed_areas;
        const std::uint64_t updates_before = player::panel::now_meters_stats().governor.updates;
        const std::int64_t began = g_now_us;
        pump(150);  // five seconds of 33 ms frames
        const double seconds = static_cast<double>(g_now_us - began) / 1.0e6;
        const double mpx = static_cast<double>(g_flushed_pixels - before) / seconds / 1.0e6;
        const iclforge::panelstats::MetersStats stats = player::panel::now_meters_stats();
        const double areas_an_update = static_cast<double>(g_flushed_areas - areas_before) / std::max<double>(1.0, static_cast<double>(stats.governor.updates - updates_before));
        expect(std::string(player::panel::screen_stats().name) == "nolyrics", what + ": the no-lyrics screen shows");
        expect(stats.showing, what + ": the Meters view is up");
        expect(stats.outputs == room.outputs, what + ": it meters " + std::to_string(room.outputs) + " outputs, not " + std::to_string(stats.outputs));
        const mv::Representation want = mv::representation_for(width, room.outputs);
        expect(stats.representation == want, what + ": the representation is " + std::string(iclforge::panelstats::representation_name(want)) +
                                                  ", not " + std::string(iclforge::panelstats::representation_name(stats.representation)));
        expect(stats.rate_hz == 20 && stats.governor.falls == 0, what + ": it holds twenty updates a second");
        expect(stats.governor.updates >= 100 && stats.governor.updates <= 140, what + ": about twenty updates a second (" + std::to_string(stats.governor.updates) + " in all)");
        expect(stats.governor.dropped == 0, what + ": no step went by with no update");
        expect(mpx <= bound_mpx, what + ": LVGL flushes " + std::to_string(mpx) + " Mpx/s, over the bound of " + std::to_string(bound_mpx));
        expect(areas_an_update <= kMaxAreasAnUpdate, what + ": LVGL flushes " + std::to_string(areas_an_update) + " areas an update, over " + std::to_string(kMaxAreasAnUpdate));
        std::printf("  %-6s %-5s %6.2f Mpx/s %5.1f areas an update  %llu updates, rate %d Hz, mean update %.0f us\n", room.layout,
                    std::string(iclforge::panelstats::representation_name(stats.representation)).c_str(), mpx, areas_an_update,
                    static_cast<unsigned long long>(stats.governor.updates), stats.rate_hz,
                    stats.analysis.mean_us() + stats.widgets.mean_us());
        write_bmp(out / (std::string("meters-") + room.layout + "-" + size + ".bmp"));
    }
    // A screen that dims stops the meters and stops asking the player for its output.
    g_now_us += 3600LL * 1'000'000;
    pump(6);
    expect(!player::panel::meters_tap().wanted(), size + ": a dimmed screen does not ask the player for its output");
    return failures;
}

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

    g_sound = {};
    set_room("5.1");

    // A kit-like board whose screen is this size, playing a song with lyrics.
    g_caps = iclforge::variants::p4_rev3_kit();
    g_caps.display.width = width;
    g_caps.display.height = height;
    set_up_system_fixture();
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
        failures += check_display_and_time(prefs, width, height, out, size);
        lv_obj_scroll_to(scroller_of(prefs), 0, 0, LV_ANIM_OFF);
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
                // A longer list takes more than one drag to bring back to its top.
                for (int again = 0; again < 8 && lv_obj_get_scroll_y(list) > 0; ++again) {
                    drag(width / 2, height / 4, height * 3 / 4);
                }
                std::printf("  scrolled back to %d\n", static_cast<int>(lv_obj_get_scroll_y(list)));
                expect(lv_obj_get_scroll_y(list) == 0, "and dragged down scrolls it back");
            }
        }
        expect(tap_button("Off"), "a control past the first screenful is reached");
        expect(g_settings.view == nv::View::kOff, "and works");
        tap_button("Speakers");
        failures += check_sound_group(width, height, out, size);
        failures += check_system_group(width, height, out, size, prefs, scroller_of(prefs));
        tap_button("Close");
        pump(6);
        expect(player::panel::prefs_root() == nullptr, "Close puts the Preferences sheet away");
        expect(!g_sound.tone_running, "and no tone is left playing");
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

    // The night schedule (planning/p4x-preferences.md): by the board's own time, the no-lyrics view goes dark as the dark
    // view does, a touch lights it for 30 seconds, and a board that does not know the time is never dark by accident.
    {
        g_caps = iclforge::variants::p4_rev3_kit();
        g_caps.display.width = width;
        g_caps.display.height = height;
        g_settings = {nv::View::kSpeakers, nv::kDimNever};
        g_settings.night = {.enabled = true, .from_min = 22 * 60, .to_min = 7 * 60, .idle = false};
        g_settings.minute_of_day = 12 * 60;
        g_now_us += 40'000'000;
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        pump(6);
        expect(!player::panel::screen_stats().dimmed, "at noon the night schedule leaves the speakers lit");
        g_settings.minute_of_day = 23 * 60;
        pump(6);
        expect(player::panel::screen_stats().dimmed, "at 23:00 the speakers go dark");
        tap(width / 2, height / 3);
        pump(6);
        expect(!player::panel::screen_stats().dimmed, "a touch lights them");
        g_now_us += 20'000'000;
        pump(6);
        expect(!player::panel::screen_stats().dimmed, "for 30 seconds");
        g_now_us += 15'000'000;
        pump(6);
        expect(player::panel::screen_stats().dimmed, "and then they go dark again");
        g_settings.minute_of_day = 7 * 60;
        pump(6);
        expect(!player::panel::screen_stats().dimmed, "at 07:00 the night is over");
        g_settings.minute_of_day = std::nullopt;
        pump(6);
        expect(!player::panel::screen_stats().dimmed, "a board that does not know the time is lit");
        // The idle screen, between songs: covered only when the schedule is told to cover it.
        g_now_playing = false;
        g_board.stream = "idle";
        g_held = false;
        g_settings.minute_of_day = 23 * 60;
        g_now_us += 40'000'000;
        pump(6);
        expect(!player::panel::screen_stats().dimmed, "the idle screen is lit at night by default");
        g_settings.night.idle = true;
        pump(6);
        expect(player::panel::screen_stats().dimmed, "and dark when the night is told to cover it");
        g_settings.minute_of_day = 12 * 60;
        pump(6);
        expect(!player::panel::screen_stats().dimmed, "but not by day");
        g_board.stream = "bursts";
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
        expect(find_button("Wired") != nullptr, "the choice of network is shown on a board that has one network");
        expect(shows_text(greyed, "Wi-Fi only, so there is nothing to choose between", true), "greyed, with the reason");
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

    failures += check_layout_sheet(width, height, out, lrc);
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

// What the speakers view reads from the player every frame: the scenario's levels, as a new window every 100 ms of
// the preview's clock.
bool read_now_levels(NowLevels& out) {
    if (!g_now_playing || g_now.rms_db.empty()) {
        return false;
    }
    out.serial = static_cast<std::uint32_t>(g_now_us / 100'000);
    out.outputs = std::min({g_now.rms_db.size(), g_now.peak_db.size(), out.rms_db.size()});
    for (std::size_t o = 0; o < out.outputs; ++o) {
        out.rms_db[o] = g_now.rms_db[o];
        out.peak_db[o] = g_now.peak_db[o];
    }
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
    [[nodiscard]] bool identify_running() const override { return g_sound.tone_running; }
    [[nodiscard]] std::string identify_blocked() const override { return g_sound.tone_blocked; }
    [[nodiscard]] bool identify_each(bool start) override { return player::sound_identify_request(start ? "all" : "off").empty(); }
    [[nodiscard]] std::string speaker_trim_summary() const override { return player::sound_trim_summary(); }
    [[nodiscard]] bool set_speaker_trim_text(std::string_view statements) override {
        return player::sound_set_trim_text(statements).empty();
    }
    [[nodiscard]] bool late_night() const override { return g_late_night; }
    [[nodiscard]] bool set_late_night(bool on) override {
        g_late_night = on;
        return true;
    }
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
    [[nodiscard]] iclforge::nowview::SpeakersStyle speakers_style() const override { return g_settings.speakers_style; }
    [[nodiscard]] bool set_speakers_style(iclforge::nowview::SpeakersStyle style) override {
        g_settings.speakers_style = style;
        return true;
    }

    [[nodiscard]] std::string network_preference() const override { return g_system.network_preference; }
    [[nodiscard]] bool set_network_preference(std::string_view word) override {
        g_system.network_preference = std::string(word);
        return true;
    }
    [[nodiscard]] std::string name() const override { return g_system.name; }
    [[nodiscard]] bool set_name(std::string_view name) override {
        g_system.name = std::string(name);
        return true;
    }
    [[nodiscard]] iclforge::devview::Facts facts() const override { return g_system.facts; }
    [[nodiscard]] iclforge::sysactions::Situation situation() const override { return g_system.situation; }
    [[nodiscard]] std::optional<std::string> restart() override {
        if (g_system.refusal) {
            return g_system.refusal;
        }
        ++g_system.restarts;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> factory_reset() override {
        if (g_system.refusal) {
            return g_system.refusal;
        }
        ++g_system.factory_resets;
        return std::nullopt;
    }
    [[nodiscard]] std::vector<iclforge::sysactions::paired::Server> paired_servers() const override {
        return g_system.paired;
    }
    [[nodiscard]] bool forget_server(std::string_view server_id) override {
        for (auto it = g_system.paired.begin(); it != g_system.paired.end(); ++it) {
            if (it->server_id == server_id) {
                g_system.paired.erase(it);
                ++g_system.forgotten;
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] int brightness() const override { return g_brightness; }
    [[nodiscard]] bool set_brightness(int percent) override {
        if (!g_caps.display.backlight) {
            return false;
        }
        g_brightness = percent;
        return true;
    }
    [[nodiscard]] iclforge::wallclock::ClockFormat clock_format() const override { return g_wall.format; }
    [[nodiscard]] bool set_clock_format(iclforge::wallclock::ClockFormat format) override {
        g_wall.format = format;
        return true;
    }
    [[nodiscard]] iclforge::nowview::NightSchedule night() const override { return g_settings.night; }
    [[nodiscard]] bool set_night(const iclforge::nowview::NightSchedule& night) override {
        g_settings.night = night;
        return true;
    }
    [[nodiscard]] iclforge::wallclock::ZoneChoice time_zone() const override { return g_zone; }
    [[nodiscard]] bool set_time_zone(const iclforge::wallclock::ZoneChoice& zone) override {
        g_zone = zone;
        return true;
    }
    [[nodiscard]] std::string layout() const override { return g_layout; }
    [[nodiscard]] bool set_layout(std::string_view layout) override {
        g_layout = std::string(layout);
        return true;
    }
    [[nodiscard]] bool playing() const override { return g_now_playing; }
};

iclforge::prefs::Backend& preferences_backend() {
    static PreviewBackend backend;
    return backend;
}

std::int64_t time_us() { return g_now_us; }

int panel_frame_us() { return 33'333; }

iclforge::meters::MeterTap& meters_tap() {
    // The scenarios that show the Meters view write into it (a tap the preview feeds itself); nothing plays here.
    static iclforge::meters::MeterTap tap;
    return tap;
}

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

// What main/sound.cpp answers on a board, answered from the fixture the checks set up.
namespace player {

SoundSheet sound_sheet() { return g_sound; }

bool sound_identify_running() { return g_sound.tone_running; }

std::string sound_identify_blocked() { return g_sound.tone_blocked; }

std::string sound_trim_summary() { return iclforge::render::speaker_trim::summary_text(g_sound.table); }

std::string sound_identify_request(std::string_view request) {
    namespace st = iclforge::render::speaker_trim;
    g_tone_requests.emplace_back(request);
    const bool off = request == "off";
    if (!off && !g_sound.tone_blocked.empty()) {
        return g_sound.tone_blocked;
    }
    g_sound.tone_running = !off;
    if (off) {
        g_sound.tone_role = st::kNoRole;
    } else if (request == "all") {
        g_sound.tone_role = g_sound.speakers.empty() ? st::kNoRole : g_sound.speakers.front().role;
    } else if (const std::optional<st::RoleCode> role = st::parse_role(request.substr(0, request.find(' ')))) {
        g_sound.tone_role = *role;
    }
    return {};
}

std::string sound_set_trim_text(std::string_view statements) {
    namespace st = iclforge::render::speaker_trim;
    g_trim_requests.emplace_back(statements);
    st::SpeakerTrimTable next = g_sound.table;
    const st::ParseResult parsed = next.apply_text(statements);
    if (!parsed.ok) {
        return std::string("PUT /trim: ") + parsed.error;
    }
    if (!g_sound.delay) {
        for (st::RoleCode role = 0; role < st::kRoles; ++role) {
            if (next.get(role).cm != g_sound.table.get(role).cm) {
                return "A distance is a delay, and " + g_sound.delay_reason;
            }
        }
    }
    if (next.span_cm() > g_sound.max_span_cm && next.span_cm() > g_sound.table.span_cm()) {
        return "The nearest and the farthest speaker can be at most 6.86 m apart on this board (its delay lines hold 20 ms).";
    }
    g_sound.table = next;
    refresh_sound_trims();
    return {};
}

}  // namespace player

// --- --drive: the screen as a board's, driven by a script
// ----------------------------------------------------------
//
// tools/hearth/screen_script.py starts this and sends one command a line on stdin; each is answered
// with one line of JSON on stdout. The commands are the ones a board's HTTP side answers (POST
// :8081/panel/tap, GET :8081/panel.bmp, GET /preferences) plus the two a board cannot: `tap` finds
// a button by what it says and `shown` reads the screen's own words. A tap that needs the list
// scrolled first scrolls it with the finger (swipes), not by calling LVGL, and `events` in the
// answer is every touch made, so that the same touches can be made on a board (README.md).
namespace {

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out + '"';
}

// The words of a command: bare, or in double quotes with \" and \\ inside.
std::vector<std::string> words_of(const std::string& line) {
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ' || line[i] == '\r') {
            ++i;
            continue;
        }
        std::string word;
        if (line[i] == '"') {
            for (++i; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    ++i;
                }
                word += line[i];
            }
            ++i;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\r') {
                word += line[i++];
            }
        }
        words.push_back(word);
    }
    return words;
}

std::string g_events;  // the touches of the command being answered, as JSON array members

void note_event(const std::string& member) {
    g_events += (g_events.empty() ? "" : ",") + member;
}

void finger_tap(int x, int y) {
    note_event("[\"tap\"," + std::to_string(x) + "," + std::to_string(y) + "]");
    tap(x, y);
}

void finger_swipe(int x, int y_from, int y_to) {
    note_event("[\"swipe\"," + std::to_string(x) + "," + std::to_string(y_from) + "," +
               std::to_string(y_to) + "]");
    drag(x, y_from, y_to);
}

std::string reply(bool ok, const std::string& more = {}) {
    std::string out = std::string("{\"ok\":") + (ok ? "true" : "false");
    if (!more.empty()) {
        out += "," + more;
    }
    return out + ",\"events\":[" + g_events + "]}";
}

std::string failed(const std::string& why) {
    return reply(false, "\"error\":" + json_string(why));
}

// The part of the glass an object can be seen on: the display, cut down by every scrolling
// container it is in.
lv_area_t visible_part(lv_obj_t* object) {
    lv_area_t clip{0, 0, g_width - 1, g_height - 1};
    for (lv_obj_t* parent = lv_obj_get_parent(object); parent != nullptr;
         parent = lv_obj_get_parent(parent)) {
        if (lv_obj_get_scroll_top(parent) > 0 || lv_obj_get_scroll_bottom(parent) > 0) {
            lv_area_t box;
            lv_obj_get_coords(parent, &box);
            clip = {std::max(clip.x1, box.x1), std::max(clip.y1, box.y1), std::min(clip.x2, box.x2),
                    std::min(clip.y2, box.y2)};
        }
    }
    return clip;
}

bool on_glass(lv_obj_t* object) {
    lv_obj_update_layout(object);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const lv_area_t clip = visible_part(object);
    return area.x1 >= clip.x1 && area.x2 <= clip.x2 && area.y1 >= clip.y1 && area.y2 <= clip.y2;
}

// The list that holds `object` and has more to scroll to, the nearest one.
lv_obj_t* scroller_of(lv_obj_t* object) {
    for (lv_obj_t* parent = lv_obj_get_parent(object); parent != nullptr;
         parent = lv_obj_get_parent(parent)) {
        if (lv_obj_get_scroll_top(parent) > 0 || lv_obj_get_scroll_bottom(parent) > 0) {
            return parent;
        }
    }
    return nullptr;
}

// Swipes the list under a finger until `object` is on the glass. False when it will not come.
bool bring_into_view(lv_obj_t* object) {
    constexpr int kAttempts = 16;
    for (int attempt = 0; attempt < kAttempts && !on_glass(object); ++attempt) {
        lv_obj_t* const list = scroller_of(object);
        if (list == nullptr) {
            return false;
        }
        lv_area_t at;
        lv_area_t box;
        lv_obj_get_coords(object, &at);
        lv_obj_get_coords(list, &box);
        const int height = box.y2 - box.y1 + 1;
        const bool below = at.y2 > box.y2;
        const int wanted = below ? at.y2 - box.y2 + 8 : box.y1 - at.y1 + 8;
        const int distance = std::clamp(wanted, 40, std::max(40, height - 60));
        const int x = box.x1 + 6;
        // Content follows the finger: a target below the glass wants the finger moved up.
        const int from = below ? box.y2 - 20 : box.y1 + 20;
        finger_swipe(x, from, below ? from - distance : from + distance);
    }
    return on_glass(object);
}

void set_up(const std::string& board, int width, int height, const std::string& scene,
            lv_display_t*& disp) {
    disp = open_display(width, height, disp);
    if (!player::panel::screen_init(disp, width, height)) {
        std::puts("{\"ok\":false,\"error\":\"the screen did not come up\"}");
        std::exit(1);
    }
    g_sound = {};
    set_room("5.1");
    for (const iclforge::BoardVariant& variant : iclforge::known_variants()) {
        if (board == variant.name) {
            g_caps = variant.caps;
        }
    }
    g_caps.display.width = width;
    g_caps.display.height = height;
    set_up_system_fixture();
    g_settings = {iclforge::nowview::View::kSpeakers, 5};
    g_board = {};
    g_board.name = "hearth-4f2a91";
    g_board.address = "192.168.1.50";
    g_board.network = "HomeNet";
    g_board.firmware = "0.11.0";
    g_board.player = true;
    g_board.stream = "bursts";
    g_now_playing = false;
    if (scene == "lyrics") {
        hold(read_text(fs::path(PREVIEW_EXAMPLES_DIR) / "auld-lang-syne" / "auld-lang-syne.lrc"),
             5'750);
    }
    g_now_us += 1'000'000;
    pump(6);
}

std::string answer(const std::vector<std::string>& w, lv_display_t*& disp) {
    g_events.clear();
    const std::string& verb = w.empty() ? std::string() : w[0];
    const auto number = [&](std::size_t at) {
        return at < w.size() ? std::atoi(w[at].c_str()) : 0;
    };
    if (verb == "init" && w.size() == 5) {
        // init BOARD WIDTH HEIGHT SCENE
        bool known = false;
        for (const iclforge::BoardVariant& variant : iclforge::known_variants()) {
            known = known || w[1] == variant.name;
        }
        if (!known) {
            return failed("no board variant is called " + w[1]);
        }
        set_up(w[1], number(2), number(3), w[4], disp);
        return reply(true);
    }
    if (verb == "tap-at" && w.size() == 3) {
        finger_tap(number(1), number(2));
        return reply(true);
    }
    if (verb == "swipe" && w.size() == 5 && number(1) == number(3)) {
        finger_swipe(number(1), number(2), number(4));
        return reply(true);
    }
    if (verb == "tap" && (w.size() == 2 || w.size() == 3)) {
        // tap LABEL [ROW]: the button that says LABEL, in the Preferences row that says ROW if one
        // is named.
        lv_obj_t* target = nullptr;
        if (w.size() == 3) {
            if (player::panel::prefs_root() == nullptr) {
                return failed("no Preferences sheet is open to look for a row in");
            }
            target = find_row_button(player::panel::prefs_root(), w[2], w[1]);
        } else {
            target = find_button(w[1]);
        }
        if (target == nullptr) {
            return failed("no button \"" + w[1] + "\" is shown" +
                          (w.size() == 3 ? " in the row \"" + w[2] + "\"" : ""));
        }
        if (!bring_into_view(target)) {
            return failed("the button \"" + w[1] + "\" cannot be brought onto the glass");
        }
        lv_area_t area;
        lv_obj_get_coords(target, &area);
        finger_tap((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
        pump(4);
        return reply(true);
    }
    if (verb == "wait" && w.size() == 2) {
        g_now_us += static_cast<std::int64_t>(number(1)) * 1000;
        pump(6);
        return reply(true);
    }
    if (verb == "shown" && (w.size() == 2 || w.size() == 3)) {
        // shown TEXT [contains]
        return reply(true,
                     std::string("\"value\":") +
                         (shows_text(lv_screen_active(), w[1], w.size() == 3 && w[2] == "contains")
                              ? "true"
                              : "false"));
    }
    if (verb == "prefs" && w.size() == 1) {
        std::string body = iclforge::prefs::to_json(player::panel::preferences_backend());
        while (!body.empty() && (body.back() == '\n' || body.back() == ' ')) {
            body.pop_back();
        }
        return reply(true, "\"prefs\":" + body);
    }
    if (verb == "ui" && w.size() == 1) {
        std::vector<lv_obj_t*> targets;
        collect_touch_targets(lv_screen_active(), targets);
        std::string list;
        for (lv_obj_t* const target : targets) {
            const Rect r = rect_of(target);
            list += (list.empty() ? "" : ",") + std::string("{\"text\":") +
                    json_string(button_text(target)) + ",\"rect\":[" + std::to_string(r.x1) + "," +
                    std::to_string(r.y1) + "," + std::to_string(r.x2) + "," + std::to_string(r.y2) +
                    "],\"on_glass\":" + (on_glass(target) ? "true" : "false") + "}";
        }
        return reply(true, "\"targets\":[" + list + "]");
    }
    if (verb == "shot" && w.size() == 2) {
        write_bmp(w[1]);
        return reply(true);
    }
    return failed("not a command: " + (w.empty() ? std::string("(empty)") : w[0]));
}

int drive() {
    lv_init();
    lv_tick_set_cb([]() -> std::uint32_t { return static_cast<std::uint32_t>(g_now_us / 1000); });
    g_pointer_device = lv_indev_create();
    lv_indev_set_type(g_pointer_device, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_pointer_device, &read_pointer);
    lv_display_t* disp = nullptr;
    std::string line;
    while (std::getline(std::cin, line)) {
        const std::vector<std::string> w = words_of(line);
        if (!w.empty() && w[0] == "quit") {
            break;
        }
        if (w.empty() || (w[0] != "init" && disp == nullptr)) {
            std::puts(w.empty() ? failed("an empty command").c_str()
                                : failed("init comes first").c_str());
        } else {
            std::puts(answer(w, disp).c_str());
        }
        std::fflush(stdout);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--drive") {
        return drive();
    }
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
        // The pulse style (planning/p4x-speakers-options.md): the filled circle grows and shrinks a little with its
        // level, here in the same rooms as the glow above; the style is put back to glow after the scenarios.
        {"s18-speakers-2-0-pulse", [&] {
             g_settings.speakers_style = iclforge::nowview::SpeakersStyle::kPulse;
             g_settings.view = iclforge::nowview::View::kSpeakers;
             play_room("2.0", {.codec = iclforge::nowview::Codec::kPcm}, kStereo);
             say_no_title();
         }, 10'000'000},
        {"s18-speakers-5-1-pulse", [&] { play_room("5.1", kDolbyFiveOne, kFiveOne); }, 10'000'000},
        {"s18-speakers-7-1-4-pulse", [&] {
             play_room("7.1.4", {.codec = iclforge::nowview::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true,
                                 .objects = 15},
                       kSevenOneFour);
         }, 10'000'000},
        // The Meters view (planning/p4x-meters-view.md): a spectrum of what the tap hears, a groove over the room, and the channels in the
        // representation the display's width and the room's outputs ask for.
        {"s19-meters-2-0", [&] {
             g_settings.speakers_style = iclforge::nowview::SpeakersStyle::kGlow;
             g_settings.view = iclforge::nowview::View::kMeters;
             play_room("2.0", {.codec = iclforge::nowview::Codec::kPcm}, kStereo);
             say_no_title();
         }, 10'000'000},
        {"s19-meters-5-1", [&] { play_room("5.1", kDolbyFiveOne, kFiveOne, 640); }, 10'000'000},
        {"s19-meters-7-1-4", [&] {
             play_room("7.1.4", {.codec = iclforge::nowview::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true, .objects = 15},
                       kSevenOneFour, 768);
         }, 10'000'000},
        {"s19-meters-9-1-6", [&] { play_room("9.1.6", kDolbyFiveOne, kNineOneSix, 640); }, 10'000'000},
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

    g_settings.speakers_style = iclforge::nowview::SpeakersStyle::kGlow;

    // S14: the speakers dimmed after six minutes of nothing touched, and a touch waking them. Checked
    // as well as drawn: a screen that stayed dark, or a backlight that stayed down, is a failure.
    {
        g_settings.view = iclforge::nowview::View::kSpeakers;
        g_settings.dim_after_minutes = 5;
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        // The scenarios before this one have left the screen untouched for minutes: a touch is what starts the count.
        tap(500, 300);
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
    // The Meters view, at the sizes it is offered at (a screen under 240 by 240 does not have it).
    std::puts("The Meters view at each display size:");
    for (const auto& [width, height] : {std::pair{1024, 600}, std::pair{800, 480}, std::pair{480, 320}, std::pair{320, 240}, std::pair{240, 240}}) {
        failures += check_meters(width, height, out, disp);
    }
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 3;
    }
    return 0;
}
