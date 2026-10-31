// The Preferences sheet. See prefs_screen.hpp and planning/p4x-preferences.md.
//
// What a preference is, which choices it has, whether this board can make them and what the words are is
// iclforge/preferences.hpp's (libs/device, tested on the host against every known board): this file measures the
// rows, draws them, and carries a tap to prefs::set_value(), which refuses what the board cannot do before it asks
// the board. The rows are made when the sheet opens, from the registry; a refresh once a second repaints the rows
// whose state moved (a value changed by a PUT elsewhere, a choice that became unavailable) and nothing else, so a
// finger scrolling the list is never interrupted by a rebuild.

#include "prefs_screen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/layout_builder.hpp"
#include "iclforge/preferences.hpp"

#include "board_info.hpp"
#include "fonts.hpp"
#include "layout_sheet.hpp"
#include "prefs_editors.hpp"
#include "prefs_pages.hpp"
#include "sound_sheet.hpp"

namespace prefs = iclforge::prefs;

namespace player::panel {

namespace {

constexpr std::uint32_t kRefreshMs = 1000;
// A sheet nobody has touched for this long puts itself away: a panel on a stand is a lyric screen.
constexpr std::uint32_t kIdleCloseMs = 60'000;
// How long a refusal ("This screen has no touch.") stays up.
constexpr std::uint32_t kStatusShownMs = 6000;

struct Chain {
    lv_font_t* latin = nullptr;
    lv_font_t* greek_cyrillic = nullptr;
    lv_font_t* hebrew = nullptr;
};

// What a touch target does, packed with its row and its index into the user data of its event.
enum class Op : int {
    kChoose = 0,
    kMinus = 1,
    kPlus = 2,
    kRun = 3,
    kEdit = 4,     // a text is typed on its keyboard (prefs_pages.cpp)
    kOpen = 5,     // About and a list open on a page of their own
    kConfirm = 6,  // the question before a change that breaks the music (Pref::confirm)
    kCancel = 7,
    kEditor = 8,   // a value chosen in an editor over the sheet (a pair of times, a place: prefs_editors.cpp)
    kOpenSheet = 9,    // the Speakers sheet of the speaker trim (sound_sheet.cpp)
    kOpenLayout = 10,  // the Speaker layout sheet (layout_sheet.cpp)
};

struct Row {
    std::size_t entry = 0;
    lv_obj_t* label = nullptr;
    lv_obj_t* details = nullptr;
    // kChoice and kToggle: one button a choice.
    std::vector<lv_obj_t*> choices;
    // kStepper: - value +. kText: the value.
    lv_obj_t* minus = nullptr;
    lv_obj_t* plus = nullptr;
    lv_obj_t* value = nullptr;
    // kAction, kInfo, kList, kSheet and a layout (Editor::kBuilder): the button. kText: the button that holds the value and opens the keyboard.
    lv_obj_t* action = nullptr;
    // kText with an editor: the value as a button, which opens it.
    lv_obj_t* edit = nullptr;
    // What was painted last: the row is repainted when this changes.
    std::string painted;
};

struct Prefs {
    iclforge::screen::Metrics m;
    DevColours c;

    bool fonts_ready = false;
    Chain body;  // Archivo Regular at the body size, Greek and Cyrillic and Hebrew behind it
    Chain note;  // ... at the note size
    lv_font_t* heading = nullptr;  // Archivo SemiBold at the note size: a group's name
    lv_font_t* title = nullptr;    // Archivo SemiBold at the title size

    bool want_open = false;
    bool want_close = false;
    bool open = false;
    bool dirty = true;

    lv_obj_t* root = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* status = nullptr;

    // The question asked before a change that breaks the music that plays (Pref::confirm): the cover over the sheet
    // while it is up, and the change it would make.
    lv_obj_t* confirm = nullptr;
    std::size_t confirm_row = 0;
    std::string confirm_word;

    std::vector<prefs::Entry> entries;
    std::vector<Row> rows;

    std::string status_text;
    std::string status_shown;
    std::uint32_t status_ms = 0;
    std::uint32_t opened_ms = 0;
    std::uint32_t refreshed_ms = 0;

    PrefsStats stats;
};

Prefs g;

[[nodiscard]] std::uint32_t now_ms() { return static_cast<std::uint32_t>(time_us() / 1000); }

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

[[nodiscard]] int text_width(const lv_font_t* font, const std::string& text) {
    lv_point_t size{0, 0};
    lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<int>(size.x);
}

bool make_chain(Chain& chain, const FontFile& latin, const FontFile& greek, const FontFile& hebrew, int px) {
    chain.hebrew = lv_tiny_ttf_create_data(hebrew.data, hebrew.size, px);
    chain.greek_cyrillic = lv_tiny_ttf_create_data(greek.data, greek.size, px);
    chain.latin = lv_tiny_ttf_create_data(latin.data, latin.size, px);
    if (chain.hebrew == nullptr || chain.greek_cyrillic == nullptr || chain.latin == nullptr) {
        return false;
    }
    chain.greek_cyrillic->fallback = chain.hebrew;
    chain.latin->fallback = chain.greek_cyrillic;
    return true;
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
    if (!make_chain(g.body, regular, greek, hebrew, g.m.text) || !make_chain(g.note, regular, greek, hebrew, g.m.note)) {
        return false;
    }
    g.heading = lv_tiny_ttf_create_data(semibold.data, semibold.size, g.m.note);
    g.title = lv_tiny_ttf_create_data(semibold.data, semibold.size, g.m.title);
    if (g.heading == nullptr || g.title == nullptr) {
        return false;
    }
    g.fonts_ready = true;
    return true;
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
    // The default theme puts a gap between the children of a flex row and column; none is wanted unless asked for.
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

// A line or lines of text that wraps at `width` rather than being cut.
lv_obj_t* make_paragraph(lv_obj_t* parent, const std::string& text, const lv_font_t* font, std::uint32_t rgb, int width) {
    lv_obj_t* const label = make_label(parent, text, font, rgb);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, width);
    return label;
}

[[nodiscard]] std::uintptr_t pack(Op op, std::size_t row, std::size_t index) {
    return (static_cast<std::uintptr_t>(row) << 16) | (static_cast<std::uintptr_t>(index) << 8) |
           static_cast<std::uintptr_t>(static_cast<int>(op));
}

void on_target(lv_event_t* event);

void on_close(lv_event_t* /*event*/) {
    g.want_close = true;
    if (g.root != nullptr) {
        lv_obj_add_flag(g.root, LV_OBJ_FLAG_HIDDEN);
    }
}

// A button of the sheet: a touch target, `text` centred in it.
lv_obj_t* make_button(lv_obj_t* parent, const std::string& text, int width, int height, std::uint32_t bg, std::uint32_t fg,
                      lv_event_cb_t callback, std::uintptr_t user_data, lv_obj_t** label_out) {
    lv_obj_t* const button = make_box(parent);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(button, kTouchTargetFlag);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, color(bg), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, reinterpret_cast<void*>(user_data));
    lv_obj_t* const label = make_label(button, text, g.body.latin, fg);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_center(label);
    if (label_out != nullptr) {
        *label_out = label;
    }
    return button;
}

[[nodiscard]] int button_width(const std::string& text) {
    return std::max(g.m.row_target, text_width(g.body.latin, text) + 2 * g.m.pad);
}

// What the editors are drawn with: this sheet's own sizes, colours and type, over this sheet.
[[nodiscard]] EditorKit editor_kit() {
    EditorKit kit;
    kit.m = g.m;
    kit.c = g.c;
    kit.body = g.body.latin;
    kit.note = g.note.latin;
    kit.title = g.title;
    kit.parent = g.root;
    return kit;
}

// The widest a value that has an editor reads: a place with its country, or a pair of times.
[[nodiscard]] int edit_button_width() {
    const int widest = std::max(text_width(g.body.latin, "Melbourne (Australia)"), text_width(g.body.latin, "22:00 to 07:00"));
    return widest + 2 * g.m.pad;
}

// --- what a row says -------------------------------------------------------------------------------

// The lines under a row's control: why the whole entry is greyed, or its note; why another setting makes it moot; why
// a choice is greyed; and when a change takes effect.
[[nodiscard]] std::string details_of(const prefs::Entry& entry) {
    std::vector<std::string> lines;
    if (!entry.availability.available) {
        lines.push_back(entry.availability.reason);
    } else {
        if (entry.pref->note[0] != '\0') {
            lines.emplace_back(entry.pref->note);
        }
        if (!entry.inert.empty()) {
            lines.push_back(entry.inert);
        }
        if (entry.pref->editor == prefs::Editor::kBuilder) {
            // The reasons are on the sheet the row opens. What is in use is said in words here, or, for a list of
            // speakers a person wrote, as it was written.
            const std::optional<iclforge::LayoutPick> pick = iclforge::pick_of(entry.value);
            if (pick) {
                std::string words = iclforge::pick_words(*pick) + ".";
                words[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(words[0])));
                lines.push_back(words);
            } else if (!entry.value.empty()) {
                lines.push_back("In use: " + entry.value);
            }
        } else {
            for (const prefs::ChoiceState& choice : entry.choices) {
                if (!choice.availability.available) {
                    lines.push_back(choice.label + ": " + choice.availability.reason);
                }
            }
        }
    }
    if (entry.pref->apply == prefs::Apply::kNextPlay) {
        lines.emplace_back("Applies at the next play.");
    } else if (entry.pref->apply == prefs::Apply::kRestart) {
        lines.emplace_back("Applies after a restart.");
    }
    std::string text;
    for (const std::string& line : lines) {
        text += text.empty() ? line : "\n" + line;
    }
    return text;
}

// Everything a row paints, as one string: the row is repainted when it changes.
[[nodiscard]] std::string paint_key(const prefs::Entry& entry) {
    std::string key = entry.value + "|" + entry.value_label + "|" + (entry.availability.available ? "1" : "0") + "|" +
                      details_of(entry) + "|";
    for (const prefs::ChoiceState& choice : entry.choices) {
        key += choice.availability.available ? '1' : '0';
    }
    key += prefs::step_target(entry, -1) ? 'l' : '-';
    key += prefs::step_target(entry, +1) ? 'm' : '-';
    return key;
}

void style_button(lv_obj_t* button, bool chosen, bool available) {
    if (button == nullptr) {
        return;
    }
    lv_obj_t* const label = lv_obj_get_child(button, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    if (!available) {
        // Greyed: quiet type on the sheet's own colour, with an outline so that it is still seen to be a control;
        // the one that is chosen but cannot be made keeps the accent in its outline.
        lv_obj_set_style_bg_color(button, color(g.c.surface), 0);
        lv_obj_set_style_border_width(button, chosen ? 2 : 1, 0);
        lv_obj_set_style_border_color(button, color(chosen ? g.c.accent : g.c.faint), 0);
        lv_obj_set_style_text_color(label, color(g.c.faint), 0);
    } else if (chosen) {
        lv_obj_set_style_bg_color(button, color(g.c.accent), 0);
        lv_obj_set_style_text_color(label, color(g.c.bg), 0);
    } else {
        lv_obj_set_style_bg_color(button, color(g.c.surface_up), 0);
        lv_obj_set_style_text_color(label, color(g.c.text), 0);
    }
}

void paint_row(Row& row, const prefs::Entry& entry) {
    const bool on = entry.availability.available;
    lv_obj_set_style_text_color(row.label, color(on ? g.c.text : g.c.muted), 0);
    for (std::size_t i = 0; i < row.choices.size() && i < entry.choices.size(); ++i) {
        style_button(row.choices[i], entry.value == entry.choices[i].id, entry.choices[i].availability.available);
    }
    if (row.value != nullptr) {
        // A layout reads as the word the board holds ("7.1.4"); the other values as their label.
        lv_label_set_text(row.value,
                          (entry.pref->editor == prefs::Editor::kBuilder ? entry.value : entry.value_label).c_str());
        lv_obj_set_style_text_color(row.value, color(on ? g.c.text : g.c.faint), 0);
    }
    if (row.edit != nullptr) {
        lv_label_set_text(lv_obj_get_child(row.edit, 0), entry.value_label.c_str());
        style_button(row.edit, false, on);
    }
    if (row.minus != nullptr) {
        style_button(row.minus, false, on && prefs::step_target(entry, -1).has_value());
    }
    if (row.plus != nullptr) {
        style_button(row.plus, false, on && prefs::step_target(entry, +1).has_value());
    }
    if (row.action != nullptr) {
        // Greyed by the board (it cannot) or by the moment (an update is under way): the reason is under the row.
        style_button(row.action, false, on && entry.inert.empty());
    }
    const std::string details = details_of(entry);
    if (row.details != nullptr) {
        lv_label_set_text(row.details, details.c_str());
        if (details.empty()) {
            lv_obj_add_flag(row.details, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(row.details, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// --- building --------------------------------------------------------------------------------------

[[nodiscard]] std::string upper(const char* text) {
    std::string out = text;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

void build_row(lv_obj_t* parent, std::size_t index, int content_w) {
    const prefs::Entry& entry = g.entries[index];
    const prefs::Pref& pref = *entry.pref;
    const int gap = g.m.gap;
    Row row;
    row.entry = index;

    // How wide the control is, to decide whether the label and the control share a line.
    int control_w = 0;
    const std::string button_text = pref.button[0] != '\0' ? std::string(pref.button)
                                    : pref.kind == prefs::Kind::kAction && !entry.choices.empty()
                                        ? entry.choices.front().label
                                        : std::string("Run");
    int value_w = 0;
    // A layout is chosen on a sheet of its own: the row has the value and the button that opens it.
    const bool as_builder = pref.kind == prefs::Kind::kChoice && pref.editor == prefs::Editor::kBuilder;
    switch (pref.kind) {
        case prefs::Kind::kChoice:
        case prefs::Kind::kToggle:
            if (as_builder) {
                value_w = text_width(g.body.latin, "9.1.6:upfiring");
                control_w = value_w + gap + button_width("Change");
                break;
            }
            for (const prefs::ChoiceState& choice : entry.choices) {
                control_w += button_width(choice.label) + gap;
            }
            control_w = std::max(0, control_w - gap);
            break;
        case prefs::Kind::kStepper:
            value_w = text_width(g.body.latin, "000 min");
            for (const prefs::ChoiceState& choice : entry.choices) {
                value_w = std::max(value_w, text_width(g.body.latin, choice.label));
            }
            control_w = 2 * g.m.row_target + 2 * gap + value_w;
            break;
        case prefs::Kind::kText:
            if (prefs::has_screen_editor(pref)) {
                value_w = std::min(content_w, edit_button_width());
            } else {
                // The value is a button: a tap on it opens the keyboard. Where it and the label do not fit on one
                // line it goes on the next, with the whole width, so a name is not cut to half a narrow display.
                value_w = std::min(content_w, std::max(text_width(g.body.latin, entry.value) + 2 * g.m.pad, g.m.row_target));
            }
            control_w = value_w;
            break;
        case prefs::Kind::kAction:
        case prefs::Kind::kInfo:
        case prefs::Kind::kList:
            control_w = button_width(button_text);
            break;
        case prefs::Kind::kSheet:
            // What is set, and the button that opens the sheet it is set on.
            value_w = std::max(text_width(g.body.latin, "16 speakers set"), text_width(g.body.latin, entry.value));
            control_w = value_w + gap + button_width("Edit");
            break;
    }
    const int label_w = text_width(g.body.latin, pref.label);
    const bool one_line = label_w + 2 * gap + control_w <= content_w;

    lv_obj_t* const box = make_box(parent);
    lv_obj_set_size(box, content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, gap / 2, 0);
    lv_obj_set_style_pad_top(box, gap, 0);
    lv_obj_set_style_pad_bottom(box, gap, 0);
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, color(g.c.faint), 0);

    lv_obj_t* const top = make_box(box);
    lv_obj_set_size(top, content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(top, gap / 2, 0);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    row.label = make_label(top, pref.label, g.body.latin, g.c.text);
    lv_label_set_long_mode(row.label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(row.label, one_line ? label_w + 2 : content_w);

    lv_obj_t* const controls = make_box(top);
    lv_obj_set_size(controls, one_line ? control_w : content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(controls, gap, 0);
    lv_obj_set_style_pad_row(controls, gap, 0);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    switch (pref.kind) {
        case prefs::Kind::kChoice:
        case prefs::Kind::kToggle:
            if (as_builder) {
                row.value = make_label(controls, "", g.body.latin, g.c.text);
                lv_label_set_long_mode(row.value, LV_LABEL_LONG_MODE_CLIP);
                lv_obj_set_width(row.value, value_w);
                lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_RIGHT, 0);
                row.action = make_button(controls, "Change", button_width("Change"), g.m.row_target, g.c.surface_up,
                                         g.c.text, &on_target, pack(Op::kOpenLayout, index, 0), nullptr);
                break;
            }
            for (std::size_t i = 0; i < entry.choices.size(); ++i) {
                const std::string& text = entry.choices[i].label;
                row.choices.push_back(make_button(controls, text, button_width(text), g.m.row_target, g.c.surface_up,
                                                  g.c.text, &on_target, pack(Op::kChoose, index, i), nullptr));
            }
            break;
        case prefs::Kind::kStepper:
            row.minus = make_button(controls, "-", g.m.row_target, g.m.row_target, g.c.surface_up, g.c.text, &on_target,
                                    pack(Op::kMinus, index, 0), nullptr);
            row.value = make_label(controls, "", g.body.latin, g.c.text);
            lv_label_set_long_mode(row.value, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_width(row.value, value_w);
            lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_CENTER, 0);
            row.plus = make_button(controls, "+", g.m.row_target, g.m.row_target, g.c.surface_up, g.c.text, &on_target,
                                   pack(Op::kPlus, index, 0), nullptr);
            break;
        case prefs::Kind::kText:
            if (prefs::has_screen_editor(pref)) {
                // Chosen in an editor of its own (prefs_editors.cpp): the value is the button that opens it.
                row.edit = make_button(controls, "", value_w, g.m.row_target, g.c.surface_up, g.c.text, &on_target,
                                       pack(Op::kEditor, index, 0), nullptr);
                lv_label_set_long_mode(lv_obj_get_child(row.edit, 0), LV_LABEL_LONG_MODE_DOTS);
                lv_obj_set_width(lv_obj_get_child(row.edit, 0), value_w - 2 * g.m.gap);
                lv_obj_set_style_text_align(lv_obj_get_child(row.edit, 0), LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_center(lv_obj_get_child(row.edit, 0));
                break;
            }
            // Typed on the keyboard of prefs_pages.cpp: the value is the button that opens it.
            row.action = make_button(controls, "", value_w, g.m.row_target, g.c.surface_up, g.c.text, &on_target,
                                     pack(Op::kEdit, index, 0), &row.value);
            lv_label_set_long_mode(row.value, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(row.value, value_w - 2 * g.m.pad);
            lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_CENTER, 0);
            break;
        case prefs::Kind::kAction:
            row.action = make_button(controls, button_text, button_width(button_text), g.m.row_target, g.c.surface_up,
                                     g.c.text, &on_target, pack(Op::kRun, index, 0), nullptr);
            break;
        case prefs::Kind::kInfo:
        case prefs::Kind::kList:
            row.action = make_button(controls, button_text, button_width(button_text), g.m.row_target, g.c.surface_up,
                                     g.c.text, &on_target, pack(Op::kOpen, index, 0), nullptr);
            break;
        case prefs::Kind::kSheet:
            row.value = make_label(controls, "", g.body.latin, g.c.muted);
            lv_label_set_long_mode(row.value, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_width(row.value, value_w);
            lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_RIGHT, 0);
            row.action = make_button(controls, "Edit", button_width("Edit"), g.m.row_target, g.c.surface_up, g.c.text,
                                     &on_target, pack(Op::kOpenSheet, index, 0), nullptr);
            break;
    }

    row.details = make_paragraph(box, "", g.note.latin, g.c.muted, content_w);
    paint_row(row, entry);
    row.painted = paint_key(entry);
    g.rows.push_back(std::move(row));
}

void build_view() {
    const iclforge::screen::Metrics& m = g.m;
    const int content_w = m.width - 2 * m.margin;

    lv_obj_t* const root = make_box(lv_screen_active());
    g.root = root;
    lv_obj_set_size(root, m.width, m.height);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, color(g.c.bg), 0);
    // A tap on the sheet is the sheet's: it never reaches the screen under it.
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);

    // The bar: the title, what the last change said if it was refused, and Close.
    lv_obj_t* const bar = make_box(root);
    lv_obj_set_size(bar, m.width, m.bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, color(g.c.faint), 0);
    lv_obj_t* const title = make_label(bar, "Preferences", g.title, g.c.text);
    const int title_w = text_width(g.title, "Preferences");
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(title, m.margin, (m.bar - lv_font_get_line_height(g.title)) / 2);
    const int close_w = std::max(m.target, text_width(g.body.latin, "Close") + 2 * m.pad);
    const int close_x = m.width - m.margin - close_w;
    lv_obj_t* const close = make_button(bar, "Close", close_w, m.bar_button, g.c.accent, g.c.bg, &on_close, 0, nullptr);
    lv_obj_set_pos(close, close_x, (m.bar - m.bar_button) / 2);
    const int status_x = m.margin + title_w + 2 * m.gap;
    const int status_w = close_x - m.gap - status_x;
    g.status = make_label(bar, "", g.note.latin, g.c.accent);
    lv_label_set_long_mode(g.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(g.status, std::max(status_w, 1), lv_font_get_line_height(g.note.latin));
    lv_obj_set_pos(g.status, status_x, (m.bar - lv_font_get_line_height(g.note.latin)) / 2);
    if (status_w < 80) {
        lv_obj_add_flag(g.status, LV_OBJ_FLAG_HIDDEN);
    }
    g.status_shown.clear();

    // The list, under the bar: scrolls up and down.
    lv_obj_t* const list = make_box(root);
    g.list = list;
    lv_obj_set_size(list, m.width, m.height - m.bar);
    lv_obj_set_pos(list, 0, m.bar);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, color(g.c.muted), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(list, m.margin, 0);
    lv_obj_set_style_pad_right(list, m.margin, 0);
    lv_obj_set_style_pad_top(list, m.gap, 0);
    lv_obj_set_style_pad_bottom(list, m.margin, 0);

    g.entries = prefs::resolve(preferences_backend());
    g.rows.clear();
    g.rows.reserve(g.entries.size());
    bool first_group = true;
    for (std::size_t i = 0; i < g.entries.size(); ++i) {
        if (i == 0 || g.entries[i].pref->group != g.entries[i - 1].pref->group) {
            lv_obj_t* const heading = make_label(list, upper(prefs::group_name(g.entries[i].pref->group)), g.heading, g.c.muted);
            lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_style_pad_top(heading, first_group ? 0 : 2 * m.gap, 0);
            first_group = false;
        }
        build_row(list, i, content_w);
    }
}

void take_down() {
    // The Speakers sheet covers this one, and stops a test tone the sheet or the list started.
    sound_sheet_close();
    layout_sheet_close();
    // A page or an editor is a child of the sheet's root; it goes first, with whatever it has not yet written.
    pages_close();
    editor_close();
    if (g.root != nullptr) {
        lv_obj_delete(g.root);
    }
    g.root = nullptr;
    g.list = nullptr;
    g.status = nullptr;
    g.confirm = nullptr;  // a child of the root: deleted with it
    g.confirm_word.clear();
    g.rows.clear();
    g.entries.clear();
    g.open = false;
    g.want_close = false;
}

// --- events ----------------------------------------------------------------------------------------

// The question before a change that breaks the music that plays (Pref::confirm): the sheet under it is covered, so
// that a tap cannot reach it, and the two answers are the only things to touch. An answer hides it, and the next
// question builds another: an object is not deleted from inside the event of a button it holds.
void close_confirm() {
    if (g.confirm != nullptr) {
        lv_obj_add_flag(g.confirm, LV_OBJ_FLAG_HIDDEN);
    }
    g.confirm_word.clear();
}

void open_confirm(std::size_t row, const std::string& word, const std::string& label) {
    if (g.confirm != nullptr) {
        lv_obj_delete(g.confirm);
        g.confirm = nullptr;
    }
    const iclforge::screen::Metrics& m = g.m;
    const prefs::Pref& pref = *g.entries[row].pref;
    g.confirm_row = row;
    g.confirm_word = word;
    lv_obj_t* const cover = make_box(g.root);
    g.confirm = cover;
    lv_obj_set_size(cover, m.width, m.height);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(cover, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(cover, color(g.c.bg), 0);

    const int panel_w = std::min(m.width - 2 * m.margin, 22 * m.text);
    const int inner_w = panel_w - 2 * m.margin;
    lv_obj_t* const panel = make_box(cover);
    lv_obj_set_width(panel, panel_w);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, color(g.c.surface), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, color(g.c.accent), 0);
    lv_obj_set_style_pad_all(panel, m.margin, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, m.gap, 0);

    const std::string question = std::string(pref.confirm) + " Change to " + label + "?";
    make_paragraph(panel, question, g.body.latin, g.c.text, inner_w);

    lv_obj_t* const buttons = make_box(panel);
    lv_obj_set_size(buttons, inner_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(buttons, m.gap, 0);
    lv_obj_set_style_pad_row(buttons, m.gap, 0);
    const std::string yes = "Change";
    const std::string no = "Cancel";
    make_button(buttons, yes, button_width(yes), m.row_target, g.c.accent, g.c.bg, &on_target, pack(Op::kConfirm, row, 0),
                nullptr);
    make_button(buttons, no, button_width(no), m.row_target, g.c.surface_up, g.c.text, &on_target,
                pack(Op::kCancel, row, 0), nullptr);
}

// A word chosen on a row: refused by the registry before the board is asked, or taken; a refusal is said in the bar.
void apply_value(std::size_t row, const std::string& word) {
    if (row >= g.entries.size()) {
        return;
    }
    const prefs::SetResult result = prefs::set_value(preferences_backend(), *g.entries[row].pref, word);
    g.status_text = result.ok ? std::string() : result.message;
    g.status_ms = now_ms();
    g.dirty = true;
}

void on_target(lv_event_t* event) {
    const auto code = reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event));
    const std::size_t row = (code >> 16) & 0xFFU;
    const std::size_t index = (code >> 8) & 0xFFU;
    const auto op = static_cast<Op>(code & 0xFFU);
    if (op == Op::kCancel) {
        close_confirm();
        return;
    }
    if (op == Op::kConfirm) {
        const std::size_t asked = g.confirm_row;
        const std::string word = g.confirm_word;
        close_confirm();
        apply_value(asked, word);
        return;
    }
    if (row >= g.entries.size()) {
        return;
    }
    const prefs::Entry& entry = g.entries[row];
    std::string word;
    switch (op) {
        case Op::kChoose:
            // A greyed choice says why beside the control; a tap on it does nothing.
            if (index >= entry.choices.size() || !entry.choices[index].availability.available) {
                return;
            }
            word = entry.choices[index].id;
            // A change that asks first (the switch between the cable and Wi-Fi) is made on the page that asks.
            if (entry.pref->confirm_step != nullptr && word != entry.value) {
                pages_open(*entry.pref, word);
                return;
            }
            // A change that breaks the music that plays asks first; the same layout again, or nothing playing, does not.
            if (entry.pref->confirm[0] != '\0' && word != entry.value && preferences_backend().playing()) {
                open_confirm(row, word, entry.choices[index].label);
                return;
            }
            break;
        case Op::kMinus:
        case Op::kPlus: {
            const std::optional<std::string> target = prefs::step_target(entry, op == Op::kPlus ? +1 : -1);
            if (!target) {
                return;
            }
            word = *target;
            break;
        }
        case Op::kRun:
            // An action the Speakers sheet serves (Identify speakers) is run, and the sheet that shows which speaker it is
            // on opens over the list, whether or not the board said yes: the sheet says why not.
            if (sound_sheet_serves(*entry.pref) && entry.availability.available) {
                apply_value(row, "run");
                sound_sheet_open();
                return;
            }
            // An action that asks first is run from the page that asks; one the moment forbids says why under its row.
            if (entry.pref->confirm_step != nullptr && entry.availability.available) {
                if (entry.inert.empty()) {
                    pages_open(*entry.pref, "run");
                }
                return;
            }
            word = "run";
            break;
        case Op::kEdit:
        case Op::kOpen:
            // A text is typed, a list or About is read, each on a page of its own.
            if (!entry.availability.available) {
                g.status_text = entry.availability.reason;
                g.status_ms = now_ms();
                g.dirty = true;
            } else {
                pages_open(*entry.pref, "");
            }
            return;
        case Op::kOpenSheet:
            // The sheet is built by its own update, outside this event; a board that cannot says why in the bar.
            if (!entry.availability.available) {
                g.status_text = entry.availability.reason;
                g.status_ms = now_ms();
                g.dirty = true;
            } else {
                sound_sheet_open();
            }
            return;
        case Op::kOpenLayout:
            // The sheet is built by its own update, outside this event; a board with no output says why in the bar.
            if (!entry.availability.available) {
                g.status_text = entry.availability.reason;
                g.status_ms = now_ms();
                g.dirty = true;
            } else {
                layout_sheet_open();
            }
            return;
        case Op::kConfirm:
        case Op::kCancel:
            return;  // answered above
        case Op::kEditor:
            // A greyed entry says why beside the control; a tap on it opens nothing.
            if (entry.availability.available) {
                (void)editor_open(editor_kit(), *entry.pref);
            }
            return;
    }
    apply_value(row, word);
}

// --- refreshing ------------------------------------------------------------------------------------

void refresh(std::uint32_t now) {
    const std::int64_t started = time_us();
    g.refreshed_ms = now;
    g.dirty = false;
    std::vector<prefs::Entry> entries = prefs::resolve(preferences_backend());
    // The registry does not change while the board runs; a mismatch would be a bug, and the rows are left alone.
    if (entries.size() == g.rows.size()) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].pref != g.entries[i].pref || entries[i].choices.size() != g.entries[i].choices.size()) {
                continue;
            }
            const std::string key = paint_key(entries[i]);
            g.entries[i] = std::move(entries[i]);
            if (key != g.rows[i].painted) {
                g.rows[i].painted = key;
                paint_row(g.rows[i], g.entries[i]);
            }
        }
    }
    if (!g.status_text.empty() && now - g.status_ms > kStatusShownMs) {
        g.status_text.clear();
    }
    if (g.status != nullptr && g.status_text != g.status_shown) {
        g.status_shown = g.status_text;
        lv_label_set_text(g.status, g.status_text.c_str());
    }
    const auto took = static_cast<std::uint32_t>(time_us() - started);
    ++g.stats.refreshes;
    g.stats.refresh_us_max = std::max(g.stats.refresh_us_max, took);
}

}  // namespace

void prefs_init(const iclforge::screen::Metrics& metrics, const DevColours& colours) {
    g.m = metrics;
    g.c = colours;
    // Fonts are made at the size the display asks for: made again if the screen is built again at another size.
    g.fonts_ready = false;
    g.open = false;
    g.want_open = false;
    g.want_close = false;
    g.root = nullptr;
    g.list = nullptr;
    g.status = nullptr;
    g.rows.clear();
    g.entries.clear();
}

void prefs_open() { g.want_open = true; }

bool prefs_is_open() { return (g.open && !g.want_close) || g.want_open; }

lv_obj_t* prefs_root() { return g.open ? g.root : nullptr; }

PrefsStats prefs_stats() { return g.stats; }

void prefs_update(std::uint32_t now) {
    if (g.want_close && g.open) {
        take_down();
    }
    if (g.want_open) {
        g.want_open = false;
        if (!g.open && ensure_fonts()) {
            const std::int64_t started = time_us();
            build_view();
            g.open = true;
            g.opened_ms = now;
            g.status_text.clear();
            g.refreshed_ms = now;
            const auto took = static_cast<std::uint32_t>(time_us() - started);
            ++g.stats.builds;
            g.stats.build_us_max = std::max(g.stats.build_us_max, took);
        } else if (!g.open) {
            std::printf("panel: the preferences' fonts did not come up\n");
        }
    }
    if (!g.open) {
        return;
    }
    if (now - g.opened_ms > kIdleCloseMs && lv_display_get_inactive_time(nullptr) > kIdleCloseMs) {
        take_down();
        return;
    }
    pages_update(now);
    editor_update(now);
    sound_sheet_update(now);
    layout_sheet_update(now);
    if (g.dirty || now - g.refreshed_ms >= kRefreshMs) {
        refresh(now);
    }
}

// --- for the pages the sheet opens ---------------------------------------------------------------------------------

PrefsUi prefs_ui() { return {&g.m, &g.c, g.body.latin, g.note.latin, g.heading, g.title}; }

lv_obj_t* prefs_box(lv_obj_t* parent) { return make_box(parent); }

lv_obj_t* prefs_label(lv_obj_t* parent, const std::string& text, const lv_font_t* font, std::uint32_t rgb) {
    return make_label(parent, text, font, rgb);
}

lv_obj_t* prefs_paragraph(lv_obj_t* parent, const std::string& text, const lv_font_t* font, std::uint32_t rgb, int width) {
    return make_paragraph(parent, text, font, rgb, width);
}

lv_obj_t* prefs_button(lv_obj_t* parent, const std::string& text, int width, int height, std::uint32_t bg, std::uint32_t fg,
                       lv_event_cb_t callback, std::uintptr_t user_data, lv_obj_t** label_out) {
    return make_button(parent, text, width, height, bg, fg, callback, user_data, label_out);
}

void prefs_style_button(lv_obj_t* button, bool chosen, bool available) { style_button(button, chosen, available); }

int prefs_text_width(const lv_font_t* font, const std::string& text) { return text_width(font, text); }

void prefs_say(const std::string& text) {
    g.status_text = text;
    g.status_ms = now_ms();
    g.dirty = true;
}

void prefs_changed() { g.dirty = true; }

}  // namespace player::panel
