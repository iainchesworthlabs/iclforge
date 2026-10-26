// The developer view. See dev_screen.hpp and planning/karaoke-p4x-panel.md.
//
// What it shows, and the words and numbers for it, are iclforge/dev_view.hpp's (libs/device, tested
// on the host): this file lays the rows out, draws them, and handles the taps. The rows are labels
// at fixed places in two columns, with the console's last lines under both; a row's text is set only
// when it changed, so that a refresh once a second (every two while a stream plays) draws the numbers that moved and nothing else.

#include "dev_screen.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "iclforge/dev_view.hpp"

#include "board_info.hpp"
#include "dev_info.hpp"
#include "fonts.hpp"

namespace dv = iclforge::devview;

namespace player::panel {

namespace {

// --- the grid: 1024 by 600, two columns of 476 px between 24 px margins, a header, and the console.
constexpr int kMargin = 24;
constexpr int kLeftX = kMargin;
constexpr int kColumnWidth = 476;
constexpr int kRightX = kLeftX + kColumnWidth + kMargin;
constexpr int kHeaderHeight = 48;
constexpr int kContentTop = 54;
constexpr int kHeadingHeight = 24;
constexpr int kRowPitch = 20;
constexpr int kKeyWidth = 88;
constexpr int kLogTop = 452;
constexpr int kLogLines = 5;
constexpr int kLogPitch = 22;
// The network preference's control: three segments, each at least 64 px high.
constexpr int kSegmentWidth = 118;
constexpr int kSegmentHeight = 64;
constexpr int kSegmentGap = 8;
constexpr int kConfirmHeight = 190;
constexpr int kConfirmButtonWidth = 140;
constexpr int kMeterWidth = 12;
constexpr int kMeterGap = 4;
constexpr int kMeterHeight = 34;

constexpr std::uint32_t kRefreshMs = 1000;
// While a stream plays the view asks half as often: its drawing shares the PSRAM with the decoder.
constexpr std::uint32_t kRefreshPlayingMs = 2000;
// A tap that closes the view comes after the three that opened it: ignored for this long, so a
// fourth tap does not shut it again.
constexpr std::uint32_t kCloseGuardMs = 500;
// A view nobody has touched for this long puts itself away: a panel on a stand is a lyric screen.
constexpr std::uint32_t kIdleCloseMs = 5 * 60 * 1000;
constexpr std::uint32_t kConfirmIdleMs = 8000;
constexpr std::uint32_t kResultShownMs = 6000;
constexpr int kFontSize = 18;

struct Chain {
    lv_font_t* latin = nullptr;
    lv_font_t* greek_cyrillic = nullptr;
    lv_font_t* hebrew = nullptr;
};

struct CellObjects {
    lv_obj_t* key = nullptr;
    lv_obj_t* value = nullptr;
    std::string key_text;
    std::string value_text;
    dv::Tone tone = dv::Tone::text;
};

struct Meter {
    lv_obj_t* fill = nullptr;
    // The track's bottom edge: the fill grows up from it.
    int bottom = 0;
    int height = -1;
    bool hot = false;
};

struct Dev {
    int width = 1024;
    int height = 600;
    DevColours c;

    bool fonts_ready = false;
    Chain regular;               // Archivo Regular 18, Greek and Cyrillic and Hebrew behind it
    lv_font_t* semi18 = nullptr; // headings
    lv_font_t* semi24 = nullptr; // the title and the buttons

    bool want_open = false;
    bool open = false;
    bool want_close = false;

    lv_obj_t* root = nullptr;
    lv_obj_t* name_label = nullptr;
    lv_obj_t* status_label = nullptr;
    std::array<lv_obj_t*, kLogLines> log_lines{};
    std::array<std::string, kLogLines> log_text{};
    std::vector<CellObjects> cells;
    std::vector<lv_obj_t*> rows_objects;  // everything rebuild_rows() made, deleted together
    std::vector<Meter> meters;
    std::array<lv_obj_t*, 3> segments{};
    std::array<lv_obj_t*, 3> segment_texts{};

    lv_obj_t* confirm = nullptr;
    lv_obj_t* confirm_headline = nullptr;
    lv_obj_t* confirm_detail = nullptr;
    lv_obj_t* confirm_switch = nullptr;
    lv_obj_t* confirm_switch_text = nullptr;
    bool confirming = false;
    bool confirm_blocked = false;
    dv::Preference confirm_target = dv::Preference::automatic;
    std::uint32_t confirm_since_ms = 0;

    dv::Facts facts;
    dv::Page page;
    std::string shape;
    std::string name_text;
    std::string status_text;
    dv::NetChange seen_change = dv::NetChange::idle;
    std::uint32_t change_seen_ms = 0;
    bool first_refresh = true;

    std::uint32_t opened_ms = 0;
    std::uint32_t refreshed_ms = 0;
    std::uint32_t touched_ms = 0;

    // What the refreshes have cost the panel's task, for GET :8081/panel.
    DevStats stats;
};

Dev g;

[[nodiscard]] std::uint32_t now_ms() { return static_cast<std::uint32_t>(time_us() / 1000); }

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

[[nodiscard]] std::uint32_t tone_rgb(dv::Tone tone) {
    switch (tone) {
        case dv::Tone::attention:
            return g.c.accent;
        case dv::Tone::muted:
            return g.c.muted;
        case dv::Tone::text:
            break;
    }
    return g.c.text;
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
    return box;
}

lv_obj_t* make_label(lv_obj_t* parent, const std::string& text, const lv_font_t* font, std::uint32_t rgb, int x,
                     int y, int width) {
    lv_obj_t* const label = lv_label_create(parent);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color(rgb), 0);
    // One line, with dots where a value is too long for it: LVGL puts the dots only in a label whose
    // height is fixed, and a label of automatic height wraps (a server's long name overlapped the row
    // under it on the first run on the kit).
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, lv_font_get_line_height(font));
    lv_obj_set_pos(label, x, y);
    return label;
}

lv_obj_t* make_rule(lv_obj_t* parent, int x, int y, int width, std::uint32_t rgb) {
    lv_obj_t* const rule = make_box(parent);
    lv_obj_set_size(rule, width, 1);
    lv_obj_set_pos(rule, x, y);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, color(rgb), 0);
    return rule;
}

void set_text(lv_obj_t* label, std::string& shown, const std::string& text) {
    if (shown != text) {
        shown = text;
        lv_label_set_text(label, text.c_str());
    }
}

bool ensure_fonts() {
    if (g.fonts_ready) {
        return true;
    }
    const FontFile regular = archivo_regular();
    const FontFile semibold = archivo_semibold();
    const FontFile greek = noto_sans_greek_cyrillic_regular();
    const FontFile hebrew = noto_sans_hebrew_regular();
    if (regular.data == nullptr || semibold.data == nullptr || greek.data == nullptr || hebrew.data == nullptr) {
        return false;
    }
    g.regular.hebrew = lv_tiny_ttf_create_data(hebrew.data, hebrew.size, kFontSize);
    g.regular.greek_cyrillic = lv_tiny_ttf_create_data(greek.data, greek.size, kFontSize);
    g.regular.latin = lv_tiny_ttf_create_data(regular.data, regular.size, kFontSize);
    g.semi18 = lv_tiny_ttf_create_data(semibold.data, semibold.size, kFontSize);
    g.semi24 = lv_tiny_ttf_create_data(semibold.data, semibold.size, 24);
    if (g.regular.hebrew == nullptr || g.regular.greek_cyrillic == nullptr || g.regular.latin == nullptr ||
        g.semi18 == nullptr || g.semi24 == nullptr) {
        return false;
    }
    g.regular.greek_cyrillic->fallback = g.regular.hebrew;
    g.regular.latin->fallback = g.regular.greek_cyrillic;
    g.fonts_ready = true;
    return true;
}

// --- events ----------------------------------------------------------------------------------------

void hide_confirm() {
    g.confirming = false;
    if (g.confirm != nullptr) {
        lv_obj_add_flag(g.confirm, LV_OBJ_FLAG_HIDDEN);
    }
}

// The confirming step's words and its button, from the facts as they are now.
void fill_confirm(dv::Preference target) {
    if (g.confirm == nullptr) {
        return;
    }
    const dv::Confirm text = dv::confirm_text(g.facts, target);
    g.confirm_target = target;
    g.confirm_blocked = text.blocked;
    lv_label_set_text(g.confirm_headline, text.headline.c_str());
    lv_label_set_text(g.confirm_detail, text.detail.c_str());
    lv_obj_set_style_text_color(g.confirm_detail, color(tone_rgb(text.tone == dv::Tone::text ? dv::Tone::muted : text.tone)), 0);
    // A change the board would refuse cannot be asked for: the button is there, dim, and does nothing.
    if (text.blocked) {
        lv_obj_remove_flag(g.confirm_switch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(g.confirm_switch, color(g.c.faint), 0);
        lv_obj_set_style_text_color(g.confirm_switch_text, color(g.c.muted), 0);
    } else {
        lv_obj_add_flag(g.confirm_switch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(g.confirm_switch, color(g.c.accent), 0);
        lv_obj_set_style_text_color(g.confirm_switch_text, color(g.c.bg), 0);
    }
}

void show_confirm(dv::Preference target) {
    if (g.confirm == nullptr) {
        return;
    }
    fill_confirm(target);
    lv_obj_remove_flag(g.confirm, LV_OBJ_FLAG_HIDDEN);
    g.confirming = true;
    g.confirm_since_ms = now_ms();
}

// A tap anywhere that is not a control: it cancels the confirming step if one is showing, and
// otherwise closes the view. The object tree is taken down by the next dev_update(), outside the
// event; it is hidden here.
void on_root_clicked(lv_event_t* /*event*/) {
    const std::uint32_t now = now_ms();
    g.touched_ms = now;
    if (g.confirming) {
        hide_confirm();
        return;
    }
    if (now - g.opened_ms < kCloseGuardMs) {
        return;
    }
    g.want_close = true;
    if (g.root != nullptr) {
        lv_obj_add_flag(g.root, LV_OBJ_FLAG_HIDDEN);
    }
}

void on_segment_clicked(lv_event_t* event) {
    g.touched_ms = now_ms();
    const auto index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event)));
    const auto target = static_cast<dv::Preference>(index);
    if (g.facts.net_change == dv::NetChange::working || g.facts.preference == dv::preference_word(target)) {
        return;
    }
    show_confirm(target);
}

void on_cancel_clicked(lv_event_t* /*event*/) {
    g.touched_ms = now_ms();
    hide_confirm();
}

void on_switch_clicked(lv_event_t* /*event*/) {
    g.touched_ms = now_ms();
    if (g.confirm_blocked) {
        return;
    }
    hide_confirm();
    (void)request_network_preference(g.confirm_target);
    // Show what the request did at the next frame, not the next refresh.
    g.refreshed_ms = 0;
}

lv_obj_t* make_button(lv_obj_t* parent, const char* text, int width, int height, std::uint32_t bg, std::uint32_t fg,
                      lv_event_cb_t callback, void* user_data, lv_obj_t** text_out) {
    lv_obj_t* const button = make_box(parent);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, color(bg), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t* const label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, g.semi24, 0);
    lv_obj_set_style_text_color(label, color(fg), 0);
    lv_obj_center(label);
    if (text_out != nullptr) {
        *text_out = label;
    }
    return button;
}

// --- building --------------------------------------------------------------------------------------

// A section's heading, its rule and its rows; returns where the next thing starts.
int add_section(lv_obj_t* parent, const dv::Section& section, int x, int y) {
    g.rows_objects.push_back(make_label(parent, section.title, g.semi18, g.c.text, x, y, kColumnWidth));
    g.rows_objects.push_back(make_rule(parent, x, y + kHeadingHeight - 3, kColumnWidth, g.c.faint));
    y += kHeadingHeight;
    for (const dv::Row& row : section.rows) {
        for (int i = 0; i < row.used; ++i) {
            const dv::Cell& cell = row.cells[static_cast<std::size_t>(i)];
            const int cell_width = row.used == 2 ? kColumnWidth / 2 : kColumnWidth;
            const int cell_x = x + i * (kColumnWidth / 2);
            CellObjects objects;
            objects.key = make_label(parent, cell.key, g.regular.latin, g.c.muted, cell_x, y, kKeyWidth);
            objects.value = make_label(parent, cell.value, g.regular.latin, tone_rgb(cell.tone), cell_x + kKeyWidth, y,
                                       cell_width - kKeyWidth - 6);
            objects.key_text = cell.key;
            objects.value_text = cell.value;
            objects.tone = cell.tone;
            g.rows_objects.push_back(objects.key);
            g.rows_objects.push_back(objects.value);
            g.cells.push_back(std::move(objects));
        }
        y += kRowPitch;
    }
    return y;
}

void rebuild_rows() {
    for (lv_obj_t* const object : g.rows_objects) {
        lv_obj_delete(object);
    }
    g.rows_objects.clear();
    g.cells.clear();
    g.meters.clear();
    g.confirm = nullptr;
    g.confirming = false;
    g.segments.fill(nullptr);
    g.segment_texts.fill(nullptr);
    g.status_label = nullptr;
    lv_obj_t* const parent = g.root;

    // Left: the network and its control, then the configuration.
    int y = kContentTop;
    const int network_y = y;
    y = add_section(parent, g.page.network, kLeftX, y);
    const bool has_choice = !g.facts.preference.empty();
    g.status_label = make_label(parent, g.status_text, g.regular.latin, g.c.muted, kLeftX + 120, network_y + 1,
                                kColumnWidth - 120);
    lv_obj_set_style_text_align(g.status_label, LV_TEXT_ALIGN_RIGHT, 0);
    g.rows_objects.push_back(g.status_label);
    int control_y = 0;
    if (has_choice) {
        control_y = y + 6;
        g.rows_objects.push_back(make_label(parent, "Prefer", g.regular.latin, g.c.muted, kLeftX, control_y + 22, kKeyWidth));
        static const char* const kNames[3] = {"Automatic", "Wired", "Wi-Fi"};
        for (int i = 0; i < 3; ++i) {
            lv_obj_t* label = nullptr;
            lv_obj_t* const button =
                make_button(parent, kNames[i], kSegmentWidth, kSegmentHeight, g.c.surface_up, g.c.text, &on_segment_clicked,
                            reinterpret_cast<void*>(static_cast<std::intptr_t>(i)), &label);
            lv_obj_set_pos(button, kLeftX + kKeyWidth + 8 + i * (kSegmentWidth + kSegmentGap), control_y);
            g.segments[static_cast<std::size_t>(i)] = button;
            g.segment_texts[static_cast<std::size_t>(i)] = label;
            g.rows_objects.push_back(button);
        }
        y = control_y + kSegmentHeight + 10;
    }
    add_section(parent, g.page.configuration, kLeftX, y);

    // Right: the stream and its levels, then the performance.
    y = kContentTop;
    y = add_section(parent, g.page.streaming, kRightX, y);
    if (g.facts.player) {
        g.rows_objects.push_back(make_label(parent, "Levels", g.regular.latin, g.c.muted, kRightX, y, kKeyWidth));
        if (g.facts.peak_db.empty()) {
            g.rows_objects.push_back(make_label(parent, "no stream", g.regular.latin, g.c.muted, kRightX + kKeyWidth, y,
                                                kColumnWidth - kKeyWidth));
        }
        for (std::size_t i = 0; i < g.facts.peak_db.size(); ++i) {
            const int meter_x = kRightX + kKeyWidth + static_cast<int>(i) * (kMeterWidth + kMeterGap);
            lv_obj_t* const track = make_box(parent);
            lv_obj_set_size(track, kMeterWidth, kMeterHeight);
            lv_obj_set_pos(track, meter_x, y + 2);
            lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(track, color(g.c.faint), 0);
            lv_obj_t* const fill = make_box(parent);
            lv_obj_set_size(fill, kMeterWidth, 1);
            lv_obj_set_pos(fill, meter_x, y + 2 + kMeterHeight - 1);
            lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(fill, color(g.c.text), 0);
            g.rows_objects.push_back(track);
            g.rows_objects.push_back(fill);
            Meter meter;
            meter.fill = fill;
            meter.bottom = y + 2 + kMeterHeight;
            g.meters.push_back(meter);
        }
        y += 2 * kRowPitch;
    }
    add_section(parent, g.page.performance, kRightX, y + 8);

    // The confirming step, over the left column from the control down. Always built with the control
    // and hidden: a tap on a segment shows it.
    if (has_choice) {
        const int box_y = std::min(control_y - 8, g.height - kConfirmHeight - 12);
        g.confirm = make_box(parent);
        lv_obj_set_size(g.confirm, kColumnWidth + 2 * 12, kConfirmHeight);
        lv_obj_set_pos(g.confirm, kLeftX - 12, box_y);
        lv_obj_add_flag(g.confirm, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(g.confirm, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g.confirm, color(g.c.surface), 0);
        lv_obj_set_style_border_side(g.confirm, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(g.confirm, 2, 0);
        lv_obj_set_style_border_color(g.confirm, color(g.c.accent), 0);
        g.confirm_headline = make_label(g.confirm, "", g.semi24, g.c.text, 12, 12, kColumnWidth);
        g.confirm_detail = make_label(g.confirm, "", g.regular.latin, g.c.muted, 12, 46, kColumnWidth);
        lv_label_set_long_mode(g.confirm_detail, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_height(g.confirm_detail, LV_SIZE_CONTENT);
        lv_obj_t* const cancel = make_button(g.confirm, "Cancel", kConfirmButtonWidth, kSegmentHeight, g.c.surface_up,
                                             g.c.text, &on_cancel_clicked, nullptr, nullptr);
        lv_obj_set_pos(cancel, 12, kConfirmHeight - kSegmentHeight - 12);
        g.confirm_switch = make_button(g.confirm, "Switch", kConfirmButtonWidth, kSegmentHeight, g.c.accent, g.c.bg,
                                       &on_switch_clicked, nullptr, &g.confirm_switch_text);
        lv_obj_set_pos(g.confirm_switch, 12 + kConfirmButtonWidth + 16, kConfirmHeight - kSegmentHeight - 12);
        lv_obj_add_flag(g.confirm, LV_OBJ_FLAG_HIDDEN);
        g.rows_objects.push_back(g.confirm);
    }
}

void build_view() {
    lv_obj_t* const root = lv_obj_create(lv_screen_active());
    g.root = root;
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, g.width, g.height);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, color(g.c.bg), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_outline_width(root, 0, 0);
    lv_obj_set_style_shadow_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, &on_root_clicked, LV_EVENT_CLICKED, nullptr);

    // Header.
    make_label(root, "Developer view", g.semi24, g.c.text, kMargin, 8, 260);
    g.name_label = make_label(root, "", g.regular.latin, g.c.muted, kMargin + 210, 14, 360);
    lv_obj_t* const hint = make_label(root, "Tap anywhere else to close", g.regular.latin, g.c.muted,
                                      g.width - kMargin - 320, 14, 320);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, 0);
    make_rule(root, 0, kHeaderHeight - 2, g.width, g.c.faint);

    // The console, under both columns.
    lv_obj_t* const log = make_box(root);
    lv_obj_set_size(log, g.width, g.height - kLogTop);
    lv_obj_set_pos(log, 0, kLogTop);
    lv_obj_set_style_bg_opa(log, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(log, color(g.c.surface), 0);
    make_label(log, "CONSOLE", g.semi18, g.c.text, kMargin, 4, 300);
    for (std::size_t i = 0; i < g.log_lines.size(); ++i) {
        g.log_lines[i] = make_label(log, "", g.regular.latin, g.c.muted, kMargin, 28 + static_cast<int>(i) * kLogPitch,
                                    g.width - 2 * kMargin);
        lv_label_set_long_mode(g.log_lines[i], LV_LABEL_LONG_MODE_CLIP);
        g.log_text[i].clear();
    }
    g.name_text.clear();
}

void take_down() {
    if (g.root != nullptr) {
        lv_obj_delete(g.root);
    }
    g.root = nullptr;
    g.name_label = nullptr;
    g.status_label = nullptr;
    g.confirm = nullptr;
    g.log_lines.fill(nullptr);
    g.rows_objects.clear();
    g.cells.clear();
    g.meters.clear();
    g.segments.fill(nullptr);
    g.segment_texts.fill(nullptr);
    g.shape.clear();
    g.confirming = false;
    g.open = false;
    g.want_close = false;
}

// --- refreshing ------------------------------------------------------------------------------------

// What decides whether the rows have to be made again rather than their text set: the sections' sizes
// and keys, whether there is a control, how many meters.
[[nodiscard]] std::string shape_of() {
    std::string shape;
    for (const dv::Section* const section :
         {&g.page.network, &g.page.configuration, &g.page.streaming, &g.page.performance}) {
        shape += section->title;
        for (const dv::Row& row : section->rows) {
            shape += static_cast<char>('0' + row.used);
            for (int i = 0; i < row.used; ++i) {
                shape += row.cells[static_cast<std::size_t>(i)].key;
                shape += '|';
            }
        }
        shape += ';';
    }
    shape += g.facts.preference.empty() ? 'n' : 'c';
    shape += g.facts.player ? 'p' : 'q';
    shape += std::to_string(g.facts.peak_db.size());
    return shape;
}

void apply(std::uint32_t now) {
    // The cells.
    std::size_t at = 0;
    for (const dv::Section* const section :
         {&g.page.network, &g.page.configuration, &g.page.streaming, &g.page.performance}) {
        for (const dv::Row& row : section->rows) {
            for (int i = 0; i < row.used; ++i, ++at) {
                // The shape says there is a cell for each; a mismatch would be a bug, and is skipped.
                if (at >= g.cells.size()) {
                    continue;
                }
                CellObjects& cell = g.cells[at];
                const dv::Cell& wanted = row.cells[static_cast<std::size_t>(i)];
                set_text(cell.key, cell.key_text, wanted.key);
                set_text(cell.value, cell.value_text, wanted.value);
                if (cell.tone != wanted.tone) {
                    cell.tone = wanted.tone;
                    lv_obj_set_style_text_color(cell.value, color(tone_rgb(wanted.tone)), 0);
                }
            }
        }
    }

    // The meters.
    for (std::size_t i = 0; i < g.meters.size() && i < g.facts.peak_db.size(); ++i) {
        Meter& meter = g.meters[i];
        const float db = g.facts.peak_db[i];
        const int height = static_cast<int>(dv::level_fraction(db) * static_cast<float>(kMeterHeight) + 0.5F);
        const bool hot = db > -1.0F;
        if (height != meter.height || hot != meter.hot) {
            meter.height = height;
            meter.hot = hot;
            const int shown = std::max(height, 1);
            lv_obj_set_height(meter.fill, shown);
            lv_obj_set_y(meter.fill, meter.bottom - shown);
            lv_obj_set_style_bg_color(meter.fill, color(hot ? g.c.accent : g.c.text), 0);
        }
    }

    // The control: the chosen segment is the accent, the others are quiet; none answers while a change
    // is under way.
    const bool working = g.facts.net_change == dv::NetChange::working;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* const button = g.segments[static_cast<std::size_t>(i)];
        if (button == nullptr) {
            continue;
        }
        const bool chosen = g.facts.preference == dv::preference_word(static_cast<dv::Preference>(i));
        lv_obj_set_style_bg_color(button, color(chosen ? g.c.accent : g.c.surface_up), 0);
        lv_obj_set_style_text_color(g.segment_texts[static_cast<std::size_t>(i)], color(chosen ? g.c.bg : (working ? g.c.muted : g.c.text)), 0);
    }

    // The result of a change: while it works, and a few seconds after.
    if (g.first_refresh) {
        g.first_refresh = false;
        g.seen_change = g.facts.net_change;
        g.change_seen_ms = now - kResultShownMs - 1;
    } else if (g.facts.net_change != g.seen_change) {
        g.seen_change = g.facts.net_change;
        g.change_seen_ms = now;
    }
    std::string status = dv::net_change_text(g.facts.net_change, g.facts.net_change_word);
    if (g.facts.net_change != dv::NetChange::working && now - g.change_seen_ms > kResultShownMs) {
        status.clear();
    }
    if (g.status_label != nullptr) {
        set_text(g.status_label, g.status_text, status);
        lv_obj_set_style_text_color(g.status_label,
                                    color(g.facts.net_change == dv::NetChange::refused ? g.c.accent : g.c.muted), 0);
    }

    // The header's name.
    if (g.name_label != nullptr) {
        set_text(g.name_label, g.name_text, g.facts.name);
    }
}

void refresh_log() {
    // An empty answer is a refresh the board skipped (its heap is tight), not an empty console.
    const std::string tail = read_log_tail(4096);
    if (tail.empty()) {
        return;
    }
    std::vector<std::string> lines = dv::log_tail(tail, kLogLines, 112);
    for (std::size_t i = 0; i < g.log_lines.size(); ++i) {
        // The newest line at the bottom, in the text colour; older ones quieter.
        const std::size_t from_bottom = g.log_lines.size() - 1 - i;
        const std::string text = from_bottom < lines.size() ? lines[lines.size() - 1 - from_bottom] : std::string();
        if (g.log_lines[i] != nullptr) {
            set_text(g.log_lines[i], g.log_text[i], text);
            lv_obj_set_style_text_color(g.log_lines[i], color(from_bottom == 0 ? g.c.text : g.c.muted), 0);
        }
    }
}

void refresh(std::uint32_t now) {
    const std::int64_t started = time_us();
    g.refreshed_ms = now;
    g.facts = read_dev_facts();
    g.page = dv::describe(g.facts);
    const std::string shape = shape_of();
    if (shape != g.shape) {
        g.shape = shape;
        rebuild_rows();
    }
    apply(now);
    refresh_log();
    const auto took = static_cast<std::uint32_t>(time_us() - started);
    ++g.stats.refreshes;
    g.stats.refresh_us_last = took;
    g.stats.refresh_us_max = std::max(g.stats.refresh_us_max, took);
}

}  // namespace

void dev_init(int width, int height, const DevColours& colours) {
    g.width = width;
    g.height = height;
    g.c = colours;
}

void dev_open() { g.want_open = true; }

DevStats dev_stats() { return g.stats; }

bool dev_is_open() { return (g.open && !g.want_close) || g.want_open; }

void dev_update(std::uint32_t now) {
    if (g.want_close && g.open) {
        take_down();
    }
    if (g.want_open) {
        g.want_open = false;
        if (!g.open && ensure_fonts()) {
            build_view();
            g.open = true;
            g.first_refresh = true;
            g.opened_ms = now;
            g.touched_ms = now;
            refresh(now);
        } else if (!g.open) {
            std::printf("panel: the developer view's fonts did not come up\n");
        }
    }
    if (!g.open) {
        return;
    }
    if (now - g.touched_ms > kIdleCloseMs) {
        take_down();
        return;
    }
    if (g.confirming && now - g.confirm_since_ms > kConfirmIdleMs) {
        hide_confirm();
    }
    const bool playing = g.facts.player && !g.facts.stream.empty() && g.facts.stream != "idle";
    if (now - g.refreshed_ms >= (playing ? kRefreshPlayingMs : kRefreshMs) || g.refreshed_ms == 0) {
        refresh(now);
        // The words of a confirming step follow the facts: a cable plugged in while it is up changes them.
        if (g.confirming) {
            fill_confirm(g.confirm_target);
        }
    }
}

}  // namespace player::panel
