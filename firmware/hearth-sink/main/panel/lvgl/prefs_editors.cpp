// The Preferences sheet's editors. See prefs_editors.hpp and planning/p4x-preferences.md.
//
// Two editors, for the two kinds of value the buttons of a row cannot hold:
//   - a pair of times (the night schedule): a row each for From and To, each a minus, the time and a plus. A tap steps a
//     quarter of an hour round the clock (iclforge::nowview::night_time_stepped, tested on the host), a held button
//     keeps stepping and goes faster, and the pair is written a moment after the last step.
//   - a place in a list (the time zone): the regions of iclforge/time_zones.hpp, then the places in one, opening at the
//     region of the zone already chosen with that zone in view.
// Everything is chosen through iclforge::prefs::set_value(), as a tap on a row of the sheet is.

#include "prefs_editors.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/now_view.hpp"
#include "iclforge/time_zones.hpp"

#include "board_info.hpp"
#include "prefs_screen.hpp"

namespace prefs = iclforge::prefs;
namespace nv = iclforge::nowview;
namespace wc = iclforge::wallclock;

namespace player::panel {

namespace {

// A pair of times is written this long after the last step of either.
constexpr std::uint32_t kWriteAfterMs = 500;
constexpr std::uint32_t kRefreshMs = 1000;
constexpr std::uint32_t kStatusShownMs = 6000;
// A held step button moves a quarter of an hour ten times a second; after this many it moves an hour at a time.
constexpr int kRepeatsBeforeHour = 8;
constexpr std::size_t kNoRegion = static_cast<std::size_t>(-1);
// A bar narrower than this has a chevron for its way back and not the word.
constexpr int kNarrowBarPx = 400;

enum class Act : int { kDone = 0, kStep = 1, kRegion = 2, kZone = 3, kNoZone = 4, kRegions = 5 };

struct Item {
    lv_obj_t* button = nullptr;
    lv_obj_t* label = nullptr;
    Act act = Act::kZone;
    std::size_t arg = 0;
    // The word the board takes for it (a zone's name, or none); empty for a region.
    std::string word;
};

struct Editor {
    EditorKit kit;
    const prefs::Pref* pref = nullptr;
    prefs::Editor kind = prefs::Editor::kButtons;

    lv_obj_t* root = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* summary = nullptr;
    bool want_close = false;

    // The pair of times: what the editor shows, and whether it has steps the board has not been told of.
    lv_obj_t* from_value = nullptr;
    lv_obj_t* to_value = nullptr;
    int from_min = 0;
    int to_min = 0;
    bool unwritten = false;
    std::uint32_t changed_ms = 0;
    int repeats = 0;
    bool repeated = false;

    // The zone list: the region whose places are listed (or none: the regions), and a list the next update builds.
    lv_obj_t* list = nullptr;
    lv_obj_t* regions_button = nullptr;
    // The width the title may take with the way back hidden (the regions) and shown (the places).
    int title_room = 0;
    int title_room_with_back = 0;
    std::size_t region = kNoRegion;
    bool want_list = false;
    std::vector<Item> items;

    std::string painted;
    std::string status_text;
    std::uint32_t status_ms = 0;
    std::uint32_t refreshed_ms = 0;
};

Editor g;

[[nodiscard]] std::uint32_t now_ms() { return static_cast<std::uint32_t>(time_us() / 1000); }

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

[[nodiscard]] int text_width(const lv_font_t* font, const std::string& text) {
    lv_point_t size{0, 0};
    lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<int>(size.x);
}

[[nodiscard]] std::uintptr_t pack(Act act, std::size_t arg) {
    return (static_cast<std::uintptr_t>(arg) << 8) | static_cast<std::uintptr_t>(static_cast<int>(act));
}

// --- objects ---------------------------------------------------------------------------------------

lv_obj_t* make_box(lv_obj_t* parent) {
    lv_obj_t* const box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_outline_width(box, 0, 0);
    lv_obj_set_style_shadow_width(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_set_style_pad_column(box, 0, 0);
    return box;
}

lv_obj_t* make_label(lv_obj_t* parent, const std::string& text, const lv_font_t* font, std::uint32_t rgb) {
    lv_obj_t* const label = lv_label_create(parent);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color(rgb), 0);
    return label;
}

void on_press(lv_event_t* event);

// A button: a touch target with `text` in it. A stepping button also fires while it is held.
lv_obj_t* make_button(lv_obj_t* parent, const std::string& text, int width, int height, std::uint32_t bg, std::uint32_t fg,
                      std::uintptr_t user_data, bool holds) {
    lv_obj_t* const button = make_box(parent);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(button, kTouchTargetFlag);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, color(bg), 0);
    lv_obj_add_event_cb(button, &on_press, LV_EVENT_CLICKED, reinterpret_cast<void*>(user_data));
    if (holds) {
        lv_obj_add_event_cb(button, &on_press, LV_EVENT_PRESSED, reinterpret_cast<void*>(user_data));
        lv_obj_add_event_cb(button, &on_press, LV_EVENT_LONG_PRESSED_REPEAT, reinterpret_cast<void*>(user_data));
    }
    lv_obj_t* const label = make_label(button, text, g.kit.body, fg);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_center(label);
    return button;
}

void style_choice(lv_obj_t* button, lv_obj_t* label, bool chosen) {
    lv_obj_set_style_bg_color(button, color(chosen ? g.kit.c.accent : g.kit.c.surface_up), 0);
    lv_obj_set_style_text_color(label, color(chosen ? g.kit.c.bg : g.kit.c.text), 0);
}

[[nodiscard]] int line_height(const lv_font_t* font) { return lv_font_get_line_height(font); }

// What the editor says under its controls: a refusal while there is one, else `text`.
void show_summary(const std::string& text) {
    if (g.summary == nullptr) {
        return;
    }
    const bool refused = !g.status_text.empty();
    lv_label_set_text(g.summary, refused ? g.status_text.c_str() : text.c_str());
    lv_obj_set_style_text_color(g.summary, color(refused ? g.kit.c.accent : g.kit.c.muted), 0);
}

void say(const std::string& text) {
    g.status_text = text;
    g.status_ms = now_ms();
    g.painted.clear();
}

// --- the pair of times -----------------------------------------------------------------------------

[[nodiscard]] std::string times_summary() {
    const nv::NightSchedule night = preferences_backend().night();
    std::string text;
    if (g.from_min == g.to_min) {
        text = "The same time twice is no time at all: the screen is never dark.";
    } else {
        text = "Dark from " + nv::time_of_day_text(g.from_min) + " until " + nv::time_of_day_text(g.to_min) +
               (g.from_min > g.to_min ? ", through midnight" : "") + ", by the board's own time. A touch lights the screen for " +
               std::to_string(nv::kDarkAwakeMs / 1000) + " seconds.";
    }
    if (!night.enabled) {
        text += " The night schedule is switched off.";
    }
    return text;
}

void paint_times() {
    const std::string key = nv::time_of_day_text(g.from_min) + nv::time_of_day_text(g.to_min) +
                            (preferences_backend().night().enabled ? "1" : "0") + g.status_text;
    if (key == g.painted) {
        return;
    }
    g.painted = key;
    lv_label_set_text(g.from_value, nv::time_of_day_text(g.from_min).c_str());
    lv_label_set_text(g.to_value, nv::time_of_day_text(g.to_min).c_str());
    show_summary(times_summary());
}

void flush_times() {
    g.unwritten = false;
    const nv::NightSchedule board = preferences_backend().night();
    if (board.from_min == g.from_min && board.to_min == g.to_min) {
        return;
    }
    const std::string word = nv::time_of_day_text(g.from_min) + "-" + nv::time_of_day_text(g.to_min);
    const prefs::SetResult result = prefs::set_value(preferences_backend(), *g.pref, word);
    say(result.ok ? std::string() : result.message);
}

void step_time(std::size_t arg) {
    const int which = static_cast<int>(arg / 2);
    const int direction = arg % 2 == 1 ? 1 : -1;
    const int steps = direction * (g.repeats >= kRepeatsBeforeHour ? 4 : 1);
    int& time = which == 0 ? g.from_min : g.to_min;
    time = nv::night_time_stepped(time, steps);
    g.unwritten = true;
    g.changed_ms = now_ms();
    paint_times();
}

void build_times() {
    const iclforge::screen::Metrics& m = g.kit.m;
    const nv::NightSchedule night = preferences_backend().night();
    g.from_min = night.from_min;
    g.to_min = night.to_min;
    const int content_w = m.width - 2 * m.margin;
    const int value_w = text_width(g.kit.body, "00:00") + 2 * m.gap;
    const int cluster_w = 2 * m.row_target + 2 * m.gap + value_w;
    const int cluster_x = m.width - m.margin - cluster_w;
    const int text_h = line_height(g.kit.body);
    int y = m.bar + m.margin;
    for (int which = 0; which < 2; ++which) {
        lv_obj_t* const name = make_label(g.root, which == 0 ? "From" : "To", g.kit.body, g.kit.c.text);
        lv_obj_set_pos(name, m.margin, y + (m.row_target - text_h) / 2);
        lv_obj_t* const minus = make_button(g.root, "-", m.row_target, m.row_target, g.kit.c.surface_up, g.kit.c.text,
                                            pack(Act::kStep, static_cast<std::size_t>(which * 2)), true);
        lv_obj_set_pos(minus, cluster_x, y);
        lv_obj_t* const value = make_label(g.root, "00:00", g.kit.body, g.kit.c.text);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(value, value_w, text_h);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(value, cluster_x + m.row_target + m.gap, y + (m.row_target - text_h) / 2);
        lv_obj_t* const plus = make_button(g.root, "+", m.row_target, m.row_target, g.kit.c.surface_up, g.kit.c.text,
                                           pack(Act::kStep, static_cast<std::size_t>(which * 2 + 1)), true);
        lv_obj_set_pos(plus, cluster_x + m.row_target + 2 * m.gap + value_w, y);
        (which == 0 ? g.from_value : g.to_value) = value;
        y += m.row_target + 2 * m.gap;
    }
    g.summary = make_label(g.root, "", g.kit.note, g.kit.c.muted);
    lv_label_set_long_mode(g.summary, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(g.summary, content_w);
    lv_obj_set_pos(g.summary, m.margin, y);
    paint_times();
}

// --- the zone list ---------------------------------------------------------------------------------

// The region the zone of this name is in, or none for a name no region has.
[[nodiscard]] std::size_t region_of(std::string_view name) {
    const std::string_view region = wc::zone_region(name);
    for (std::size_t i = 0; i < wc::region_count(); ++i) {
        if (wc::region_at(i).name == region) {
            return i;
        }
    }
    return kNoRegion;
}

// The zone the board holds, as the editor reads it: the place (a name in the table, the one the person chose or, for a
// zone that was set with a string and no name, the table's place for the string), what a person reads for it, and
// whether the board has none. The string itself is never shown (wallclock::zone_readable).
struct Current {
    std::string place;
    std::string readable;
    bool none = false;
};

[[nodiscard]] Current current_zone() {
    const wc::ZoneChoice zone = preferences_backend().time_zone();
    Current out;
    if (const std::optional<wc::NamedZone> place = wc::zone_place(zone.posix, zone.name)) {
        out.place = std::string(place->name);
    }
    out.none = zone.posix.empty() || zone.posix == wc::kNoZone;
    out.readable = wc::zone_readable(zone.posix, zone.name);
    return out;
}

[[nodiscard]] std::string zone_title() {
    if (g.region == kNoRegion) {
        return g.pref->label;
    }
    return std::string(wc::region_at(g.region).name);
}

void paint_zone() {
    const Current current = current_zone();
    const std::string key = current.place + "|" + current.readable + "|" + std::to_string(g.region) + "|" + g.status_text;
    if (key == g.painted) {
        return;
    }
    g.painted = key;
    const std::size_t current_region = current.place.empty() ? kNoRegion : region_of(current.place);
    for (const Item& item : g.items) {
        bool chosen = false;
        switch (item.act) {
            case Act::kZone:
                chosen = item.word == current.place;
                break;
            case Act::kNoZone:
                chosen = current.none && current.place.empty();
                break;
            case Act::kRegion:
                chosen = item.arg == current_region;
                break;
            default:
                break;
        }
        style_choice(item.button, item.label, chosen);
    }
    show_summary("Now: " + current.readable);
}

void add_item(Act act, std::size_t arg, const std::string& text, const std::string& word, bool more) {
    const iclforge::screen::Metrics& m = g.kit.m;
    const int content_w = m.width - 2 * m.margin;
    Item item;
    item.act = act;
    item.arg = arg;
    item.word = word;
    item.button = make_button(g.list, "", content_w, m.row_target, g.kit.c.surface_up, g.kit.c.text, pack(act, arg), false);
    // The sheet's own button centres its label; a list is read from the left.
    item.label = lv_obj_get_child(item.button, 0);
    lv_label_set_text(item.label, text.c_str());
    lv_label_set_long_mode(item.label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(item.label, content_w - 2 * m.pad - (more ? m.row_target / 2 : 0));
    lv_obj_align(item.label, LV_ALIGN_LEFT_MID, m.pad, 0);
    if (more) {
        lv_obj_t* const chevron = make_label(item.button, ">", g.kit.body, g.kit.c.muted);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -m.pad, 0);
    }
    g.items.push_back(std::move(item));
}

void build_list() {
    g.want_list = false;
    lv_obj_clean(g.list);
    g.items.clear();
    if (g.region == kNoRegion) {
        add_item(Act::kNoZone, 0, "No zone (UTC)", std::string(wc::kNoZone), false);
        for (std::size_t i = 0; i < wc::region_count(); ++i) {
            add_item(Act::kRegion, i, std::string(wc::region_at(i).name), std::string(), true);
        }
        lv_obj_add_flag(g.regions_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        const wc::ZoneRegion region = wc::region_at(g.region);
        for (std::size_t i = 0; i < region.count; ++i) {
            const wc::NamedZone zone = wc::time_zone_at(region.first + i);
            add_item(Act::kZone, region.first + i, wc::zone_label(zone.name), std::string(zone.name), false);
        }
        lv_obj_remove_flag(g.regions_button, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(g.title, zone_title().c_str());
    lv_obj_set_width(g.title, std::max(g.region == kNoRegion ? g.title_room : g.title_room_with_back, 1));
    g.painted.clear();
    paint_zone();
    // The zone already chosen in view, so that a person who wants the one next to it does not scroll for it.
    lv_obj_update_layout(g.list);
    const std::string current = current_zone().place;
    for (const Item& item : g.items) {
        if (item.act != Act::kRegion && !item.word.empty() && item.word == current) {
            lv_obj_scroll_to_view(item.button, LV_ANIM_OFF);
            break;
        }
    }
}

void build_zone() {
    const iclforge::screen::Metrics& m = g.kit.m;
    const int top = m.bar;
    g.summary = make_label(g.root, "", g.kit.note, g.kit.c.muted);
    lv_label_set_long_mode(g.summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(g.summary, m.width - 2 * m.margin, line_height(g.kit.note));
    lv_obj_set_pos(g.summary, m.margin, top + m.gap);
    const int list_y = top + 2 * m.gap + line_height(g.kit.note);

    g.list = make_box(g.root);
    lv_obj_set_size(g.list, m.width, m.height - list_y);
    lv_obj_set_pos(g.list, 0, list_y);
    lv_obj_add_flag(g.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g.list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(g.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g.list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(g.list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(g.list, color(g.kit.c.muted), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(g.list, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(g.list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(g.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(g.list, m.margin, 0);
    lv_obj_set_style_pad_right(g.list, m.margin, 0);
    lv_obj_set_style_pad_top(g.list, 0, 0);
    lv_obj_set_style_pad_bottom(g.list, m.margin, 0);
    lv_obj_set_style_pad_row(g.list, m.gap, 0);

    // Open where the zone already chosen is: the places in its region, with it in view.
    const std::string place = current_zone().place;
    g.region = place.empty() ? kNoRegion : region_of(place);
    build_list();
}

// --- the frame -------------------------------------------------------------------------------------

void build_frame() {
    const iclforge::screen::Metrics& m = g.kit.m;
    g.root = make_box(g.kit.parent);
    lv_obj_set_size(g.root, m.width, m.height);
    lv_obj_set_pos(g.root, 0, 0);
    lv_obj_set_style_bg_opa(g.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.root, color(g.kit.c.bg), 0);
    // A tap on the editor is the editor's: it never reaches the sheet under it.
    lv_obj_add_flag(g.root, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* const bar = make_box(g.root);
    lv_obj_set_size(bar, m.width, m.bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, color(g.kit.c.faint), 0);

    const int done_w = std::max(m.target, text_width(g.kit.body, "Done") + 2 * m.pad);
    const int done_x = m.width - m.margin - done_w;
    lv_obj_t* const done = make_button(bar, "Done", done_w, m.bar_button, g.kit.c.accent, g.kit.c.bg,
                                       pack(Act::kDone, 0), false);
    lv_obj_set_pos(done, done_x, (m.bar - m.bar_button) / 2);
    g.title_room = done_x - m.gap - m.margin;
    if (g.kind == prefs::Editor::kZone) {
        // "Regions" says where it goes; on a narrow bar a chevron does, and leaves the title room.
        const std::string text = m.width < kNarrowBarPx ? "<" : "Regions";
        const int w = std::max(m.target, text_width(g.kit.body, text) + 2 * m.pad);
        g.regions_button = make_button(bar, text, w, m.bar_button, g.kit.c.surface_up, g.kit.c.text, pack(Act::kRegions, 0),
                                       false);
        lv_obj_set_pos(g.regions_button, done_x - m.gap - w, (m.bar - m.bar_button) / 2);
        lv_obj_add_flag(g.regions_button, LV_OBJ_FLAG_HIDDEN);
        g.title_room_with_back = g.title_room - m.gap - w;
    }
    g.title = make_label(bar, g.kind == prefs::Editor::kTimes ? "Night times" : g.pref->label, g.kit.title, g.kit.c.text);
    lv_label_set_long_mode(g.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(g.title, std::max(g.title_room, 1), line_height(g.kit.title));
    lv_obj_set_pos(g.title, m.margin, (m.bar - line_height(g.kit.title)) / 2);
}

void destroy() {
    if (g.root != nullptr) {
        lv_obj_delete(g.root);
    }
    g = Editor{};
}

// --- events ----------------------------------------------------------------------------------------

void on_press(lv_event_t* event) {
    const auto code = reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event));
    const auto act = static_cast<Act>(code & 0xFFU);
    const std::size_t arg = static_cast<std::size_t>(code >> 8);
    const lv_event_code_t what = lv_event_get_code(event);
    if (g.root == nullptr) {
        return;
    }
    if (act == Act::kStep) {
        // A press begins a fresh hold. A hold steps while it lasts, and the click that ends it is not one more step.
        if (what == LV_EVENT_PRESSED) {
            g.repeats = 0;
            g.repeated = false;
        } else if (what == LV_EVENT_LONG_PRESSED_REPEAT) {
            g.repeated = true;
            step_time(arg);
            ++g.repeats;
        } else if (what == LV_EVENT_CLICKED) {
            if (!g.repeated) {
                step_time(arg);
            }
            g.repeated = false;
            g.repeats = 0;
        }
        return;
    }
    if (what != LV_EVENT_CLICKED) {
        return;
    }
    switch (act) {
        case Act::kDone:
            g.want_close = true;
            lv_obj_add_flag(g.root, LV_OBJ_FLAG_HIDDEN);
            break;
        case Act::kRegion:
            g.region = arg;
            g.want_list = true;
            break;
        case Act::kRegions:
            g.region = kNoRegion;
            g.want_list = true;
            break;
        case Act::kNoZone:
        case Act::kZone: {
            const std::string word =
                act == Act::kNoZone ? std::string(wc::kNoZone) : std::string(wc::time_zone_at(arg).name);
            const prefs::SetResult result = prefs::set_value(preferences_backend(), *g.pref, word);
            say(result.ok ? std::string() : result.message);
            paint_zone();
            break;
        }
        case Act::kStep:
            break;
    }
}

}  // namespace

bool editor_open(const EditorKit& kit, const prefs::Pref& pref) {
    if (g.root != nullptr || !prefs::has_screen_editor(pref)) {
        return false;
    }
    g = Editor{};
    g.kit = kit;
    g.pref = &pref;
    g.kind = pref.editor;
    g.refreshed_ms = now_ms();
    build_frame();
    if (g.kind == prefs::Editor::kTimes) {
        build_times();
    } else {
        build_zone();
    }
    return true;
}

void editor_close() {
    if (g.root == nullptr) {
        return;
    }
    if (g.unwritten) {
        flush_times();
    }
    destroy();
}

bool editor_is_open() { return g.root != nullptr && !g.want_close; }

lv_obj_t* editor_root() { return g.root; }

void editor_update(std::uint32_t now) {
    if (g.root == nullptr) {
        return;
    }
    if (g.want_close) {
        editor_close();
        return;
    }
    if (g.want_list) {
        build_list();
    }
    if (g.kind == prefs::Editor::kTimes && g.unwritten && now - g.changed_ms >= kWriteAfterMs) {
        flush_times();
    }
    if (!g.status_text.empty() && now - g.status_ms > kStatusShownMs) {
        g.status_text.clear();
        g.painted.clear();
    }
    if (now - g.refreshed_ms >= kRefreshMs || g.painted.empty()) {
        g.refreshed_ms = now;
        if (g.kind == prefs::Editor::kTimes) {
            // Times changed elsewhere (a PUT) show, unless a step is waiting to be written.
            if (!g.unwritten) {
                const nv::NightSchedule board = preferences_backend().night();
                g.from_min = board.from_min;
                g.to_min = board.to_min;
            }
            paint_times();
        } else {
            paint_zone();
        }
    }
}

}  // namespace player::panel
