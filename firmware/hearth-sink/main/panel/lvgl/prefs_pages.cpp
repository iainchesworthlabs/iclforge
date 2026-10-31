// The pages the System group of Preferences puts over the sheet. See prefs_pages.hpp.
//
// Everything a page SAYS is libs/device's: the confirming steps and the paired-server rows (system_actions.hpp), the keys and
// the rule for a name (device_name.hpp), the rows of About (about_view.hpp). This file measures them for the display it is
// given (screen_metrics.hpp, through the sheet's own tokens), draws them, and hands a tap to iclforge::prefs::set_value().
//
// Two layers. A PAGE is a whole screen with a bar of its own (the keyboard, About, the paired servers); a DIALOG is a card on
// a scrim over the sheet or over a page (a confirming step, the held button of the factory reset, a refusal). A page opens a
// dialog (Forget on a server), and closing the dialog returns to the page.
//
// Nothing is built or deleted inside an event: a tap sets a flag, and pages_update() does the work a frame later, as the sheet
// does.

#include "prefs_pages.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "iclforge/about_view.hpp"
#include "iclforge/device_name.hpp"
#include "iclforge/system_actions.hpp"

#include "board_info.hpp"
#include "prefs_screen.hpp"

namespace prefs = iclforge::prefs;
namespace sa = iclforge::sysactions;
namespace dn = iclforge::devname;

namespace player::panel {

namespace {

// How often About and the list of servers are read again while they are up.
constexpr std::uint32_t kReadEveryMs = 3000;

enum class Op : int {
    kCancel = 0,   // a dialog's Cancel
    kAccept,       // a dialog's accept button (tapped)
    kHoldDown,     // ... or held
    kHoldUp,
    kOk,           // a refusal's OK
    kClose,        // a page's Close or Cancel
    kSave,         // the keyboard's Save
    kKey,          // a key; the index is into the keys on the page
    kSpace,
    kBackspace,
    kTab,          // a page of keys; the index is a dn::Page
    kShift,
    kForget,       // a server's Forget; the index is into the rows
};

[[nodiscard]] std::uintptr_t pack(Op op, std::size_t index) {
    return (static_cast<std::uintptr_t>(index) << 8) | static_cast<std::uintptr_t>(static_cast<int>(op));
}

enum class Page : std::uint8_t { kNone, kName, kAbout, kServers };

struct State {
    // --- the page
    Page page = Page::kNone;
    bool want_page = false;
    bool want_close_page = false;
    const prefs::Pref* pref = nullptr;
    lv_obj_t* page_root = nullptr;

    // --- the dialog
    bool want_dialog = false;
    bool want_close_dialog = false;
    bool dialog_up = false;
    lv_obj_t* dialog = nullptr;
    const prefs::Pref* dialog_pref = nullptr;
    std::string dialog_word;
    sa::Confirm confirm;
    // The held step of the factory reset is showing, and the button that carries it.
    bool second_step = false;
    bool working = false;
    bool rebuild_dialog = false;
    sa::HoldConfirm hold;
    lv_obj_t* hold_button = nullptr;
    lv_obj_t* hold_fill = nullptr;
    int hold_width = 0;
    // What a dialog does when it is accepted.
    enum class Then : std::uint8_t { kSet, kForget, kNothing } then = Then::kSet;

    // --- the keyboard
    dn::Editor editor{};
    lv_obj_t* field_view = nullptr;
    lv_obj_t* field_text = nullptr;
    lv_obj_t* caret = nullptr;
    lv_obj_t* counter = nullptr;
    lv_obj_t* reason = nullptr;
    lv_obj_t* save = nullptr;
    lv_obj_t* back = nullptr;
    lv_obj_t* keys_box = nullptr;
    lv_obj_t* space = nullptr;
    std::vector<lv_obj_t*> tabs;
    lv_obj_t* shift = nullptr;
    std::vector<std::pair<lv_obj_t*, std::string>> keys;
    std::string typed_message;
    bool field_dirty = false;
    bool keys_rebuild = false;
    int field_w = 0;

    // --- About and the servers
    lv_obj_t* list = nullptr;
    std::string painted;
    std::uint32_t read_ms = 0;
    std::vector<sa::paired::Row> rows;
    std::size_t total_servers = 0;
};

State s;

[[nodiscard]] std::uint32_t now_ms() { return static_cast<std::uint32_t>(time_us() / 1000); }

[[nodiscard]] lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

[[nodiscard]] std::uint32_t tone_color(const PrefsUi& ui, iclforge::devview::Tone tone) {
    switch (tone) {
        case iclforge::devview::Tone::muted:
            return ui.c->muted;
        case iclforge::devview::Tone::attention:
            return ui.c->accent;
        case iclforge::devview::Tone::text:
            break;
    }
    return ui.c->text;
}

void on_event(lv_event_t* event);

lv_obj_t* make_button(const PrefsUi& ui, lv_obj_t* parent, const std::string& text, int width, int height, Op op,
                      std::size_t index = 0, std::uint32_t bg = 0, std::uint32_t fg = 0) {
    return prefs_button(parent, text, width, height, bg != 0 ? bg : ui.c->surface_up, fg != 0 ? fg : ui.c->text, &on_event,
                        pack(op, index), nullptr);
}

[[nodiscard]] int button_width(const PrefsUi& ui, const std::string& text) {
    return std::max(ui.m->row_target, prefs_text_width(ui.body, text) + 2 * ui.m->pad);
}

// A scrolling column that fills what is left of its parent.
lv_obj_t* make_scroller(const PrefsUi& ui, lv_obj_t* parent, int width) {
    lv_obj_t* const list = prefs_box(parent);
    lv_obj_set_width(list, width);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, color(ui.c->muted), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    return list;
}

// --- a page: a whole screen with a bar ----------------------------------------------------------------

struct BarButton {
    std::string text;
    Op op;
    bool primary;
};

// The screen's root, a column: the bar with `title` at its left and `buttons` at its right (the last one rightmost), and
// then the body, which the caller fills and which takes what is left.
lv_obj_t* make_page_root(const PrefsUi& ui, const std::string& title, const std::vector<BarButton>& buttons,
                         std::vector<lv_obj_t*>& made, lv_obj_t** body_out) {
    const iclforge::screen::Metrics& m = *ui.m;
    lv_obj_t* const root = prefs_box(lv_screen_active());
    lv_obj_set_size(root, m.width, m.height);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, color(ui.c->bg), 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* const bar = prefs_box(root);
    lv_obj_set_size(bar, m.width, m.bar);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, color(ui.c->faint), 0);

    int right = m.width - m.margin;
    for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
        const int w = std::max(m.target, prefs_text_width(ui.body, it->text) + 2 * m.pad);
        lv_obj_t* const button =
            prefs_button(bar, it->text, w, m.bar_button, it->primary ? ui.c->accent : ui.c->surface_up,
                         it->primary ? ui.c->bg : ui.c->text, &on_event, pack(it->op, 0), nullptr);
        right -= w;
        lv_obj_set_pos(button, right, (m.bar - m.bar_button) / 2);
        right -= m.gap;
        made.push_back(button);
    }
    std::reverse(made.begin(), made.end());
    // The title, where there is room for it.
    const int title_w = prefs_text_width(ui.title, title);
    if (m.margin + title_w + m.gap <= right) {
        lv_obj_t* const label = prefs_label(bar, title, ui.title, ui.c->text);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(label, m.margin, (m.bar - lv_font_get_line_height(ui.title)) / 2);
    }

    lv_obj_t* const body = prefs_box(root);
    lv_obj_set_width(body, m.width);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(body, m.margin, 0);
    lv_obj_set_style_pad_right(body, m.margin, 0);
    lv_obj_set_style_pad_top(body, m.gap, 0);
    *body_out = body;
    return root;
}

void delete_page() {
    if (s.page_root != nullptr) {
        lv_obj_delete(s.page_root);
    }
    s.page_root = nullptr;
    s.page = Page::kNone;
    s.pref = nullptr;
    s.list = nullptr;
    s.field_view = s.field_text = s.caret = s.counter = s.reason = s.save = s.back = s.keys_box = s.space = s.shift = nullptr;
    s.tabs.clear();
    s.keys.clear();
    s.rows.clear();
    s.painted.clear();
}

// --- the dialog: a confirming step, the held button, a refusal ---------------------------------------------

void delete_dialog() {
    if (s.dialog != nullptr) {
        lv_obj_delete(s.dialog);
    }
    s.dialog = nullptr;
    s.hold_button = nullptr;
    s.hold_fill = nullptr;
    s.dialog_up = false;
    s.second_step = false;
    s.working = false;
    s.hold.reset();
}

// The card for `s.confirm`: its title, its paragraphs (scrolling when the display is too small for them) and its buttons.
void build_dialog() {
    const PrefsUi ui = prefs_ui();
    const iclforge::screen::Metrics& m = *ui.m;
    const sa::Confirm& c = s.confirm;
    if (s.dialog != nullptr) {
        lv_obj_delete(s.dialog);
        s.dialog = nullptr;
        s.hold_button = nullptr;
        s.hold_fill = nullptr;
    }
    lv_obj_t* const scrim = prefs_box(lv_screen_active());
    s.dialog = scrim;
    lv_obj_set_size(scrim, m.width, m.height);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(scrim, color(ui.c->bg), 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(scrim, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scrim, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // The whole width on a small display, and three quarters of a wide one: a line of the card is long enough not to be
    // broken at the dots of an address.
    const int card_w = m.width <= 520 ? m.width - 2 * m.margin : m.width * 3 / 4;
    const int max_h = m.height - 2 * m.gap;
    const int inner_w = card_w - 2 * m.margin;
    lv_obj_t* const card = prefs_box(scrim);
    lv_obj_set_size(card, card_w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, color(ui.c->surface), 0);
    lv_obj_set_style_border_width(card, c.tone == iclforge::devview::Tone::attention ? 2 : 1, 0);
    lv_obj_set_style_border_color(card, color(c.tone == iclforge::devview::Tone::attention ? ui.c->accent : ui.c->faint), 0);
    lv_obj_set_style_pad_all(card, m.margin, 0);
    lv_obj_set_style_pad_row(card, m.gap, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* const title = prefs_paragraph(card, c.title, ui.title, c.tone == iclforge::devview::Tone::attention ? ui.c->accent : ui.c->text, inner_w);
    (void)title;

    lv_obj_t* const text = prefs_box(card);
    lv_obj_set_width(text, inner_w);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, m.gap, 0);
    for (const std::string& line : c.lines) {
        prefs_paragraph(text, line, ui.body, ui.c->text, inner_w - 8);
    }

    lv_obj_t* const buttons = prefs_box(card);
    lv_obj_set_size(buttons, inner_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(buttons, m.gap, 0);
    lv_obj_set_style_pad_row(buttons, m.gap, 0);
    if (!s.working) {
        if (!c.cancel.empty()) {
            make_button(ui, buttons, c.cancel, button_width(ui, c.cancel), m.row_target, Op::kCancel);
        }
        if (!c.accept.empty()) {
            const int w = button_width(ui, c.accept);
            if (c.hold) {
                // A held button carries its progress as a fill behind its label.
                lv_obj_t* const button = prefs_box(buttons);
                lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(button, kTouchTargetFlag);
                lv_obj_set_size(button, w, m.row_target);
                lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(button, color(ui.c->surface_up), 0);
                lv_obj_set_style_border_width(button, 2, 0);
                lv_obj_set_style_border_color(button, color(ui.c->accent), 0);
                lv_obj_add_event_cb(button, &on_event, LV_EVENT_PRESSED, reinterpret_cast<void*>(pack(Op::kHoldDown, 0)));
                lv_obj_add_event_cb(button, &on_event, LV_EVENT_RELEASED, reinterpret_cast<void*>(pack(Op::kHoldUp, 0)));
                lv_obj_add_event_cb(button, &on_event, LV_EVENT_PRESS_LOST, reinterpret_cast<void*>(pack(Op::kHoldUp, 0)));
                lv_obj_t* const fill = prefs_box(button);
                lv_obj_set_size(fill, 1, m.row_target - 4);
                lv_obj_set_pos(fill, 0, 0);
                lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(fill, color(ui.c->accent), 0);
                lv_obj_t* const label = prefs_label(button, c.accept, ui.body, ui.c->text);
                lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
                lv_obj_center(label);
                s.hold_button = button;
                s.hold_fill = fill;
                s.hold_width = w - 4;
            } else {
                lv_obj_t* const accept = make_button(ui, buttons, c.accept, w, m.row_target, Op::kAccept, 0,
                                                     c.blocked ? ui.c->surface : ui.c->accent, c.blocked ? ui.c->faint : ui.c->bg);
                if (c.blocked) {
                    prefs_style_button(accept, false, false);
                    lv_obj_remove_flag(accept, LV_OBJ_FLAG_CLICKABLE);
                }
            }
        }
    }

    // A card taller than the display scrolls its paragraphs, and keeps its buttons where a finger finds them.
    lv_obj_update_layout(card);
    const int over = static_cast<int>(lv_obj_get_height(card)) - max_h;
    if (over > 0) {
        const int shorter = std::max(static_cast<int>(lv_obj_get_height(text)) - over, 2 * m.row_target);
        lv_obj_set_height(text, shorter);
        lv_obj_add_flag(text, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(text, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(text, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(text, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_bg_opa(text, LV_OPA_COVER, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(text, color(ui.c->muted), LV_PART_SCROLLBAR);
        lv_obj_set_style_width(text, 6, LV_PART_SCROLLBAR);
        lv_obj_set_style_radius(text, 3, LV_PART_SCROLLBAR);
    }
    s.dialog_up = true;
}

// A refusal, or what the board answered, in a card of its own with an OK.
void show_message(const std::string& title, const std::string& message) {
    s.confirm = {};
    s.confirm.title = title;
    s.confirm.lines.push_back(message);
    s.confirm.cancel.clear();
    s.confirm.accept = "OK";
    s.confirm.tone = iclforge::devview::Tone::attention;
    s.then = State::Then::kNothing;
    s.second_step = false;
    s.working = false;
    s.rebuild_dialog = true;
}

void open_dialog(const prefs::Pref& pref, const std::string& word, sa::Confirm confirm, State::Then then) {
    s.dialog_pref = &pref;
    s.dialog_word = word;
    s.confirm = std::move(confirm);
    s.then = then;
    s.second_step = false;
    s.working = false;
    s.want_dialog = true;
}

// The dialog was accepted: make the change the person confirmed.
void accept_dialog() {
    if (s.dialog_pref == nullptr || s.working) {
        return;
    }
    if (s.then == State::Then::kNothing) {
        s.want_close_dialog = true;
        return;
    }
    const prefs::Pref& pref = *s.dialog_pref;
    // The factory reset is two steps: what is lost, and then a button that is held.
    if (!s.second_step && pref.then_hold != nullptr && s.then == State::Then::kSet) {
        s.second_step = true;
        s.confirm = pref.then_hold(preferences_backend(), s.dialog_word);
        s.rebuild_dialog = true;
        return;
    }
    const prefs::SetResult result = prefs::set_value(preferences_backend(), pref, s.dialog_word);
    if (!result.ok) {
        show_message("Not done", result.message);
        return;
    }
    const std::string id = pref.id;
    if (id == "restart" || id == "factory-reset") {
        // The board restarts in a moment, and nothing more can be asked of it: the card says so until it does.
        s.working = true;
        s.confirm = {};
        s.confirm.title = sa::working_text(id == "restart" ? sa::Action::restart : sa::Action::factory_reset);
        s.confirm.lines.push_back("This screen goes dark while the player restarts.");
        s.confirm.cancel.clear();
        s.confirm.accept.clear();
        s.rebuild_dialog = true;
        return;
    }
    if (id == "reset-preferences") {
        prefs_say(sa::working_text(sa::Action::reset_preferences));
    } else if (id == "network-preference") {
        prefs_say("Switching...");
    }
    prefs_changed();
    s.want_close_dialog = true;
}

// --- the keyboard --------------------------------------------------------------------------------------

[[nodiscard]] std::string tab_text(dn::Page page) {
    switch (page) {
        case dn::Page::lower:
        case dn::Page::upper:
            return "abc";
        case dn::Page::symbols:
            return "123";
        case dn::Page::accents_latin1:
            return "\xC3\xA0\xC3\xA9";  // a grave, e acute
        case dn::Page::accents_extended:
            return "\xC5\x8D\xC5\x82";  // o macron, l stroke
    }
    return {};
}

[[nodiscard]] bool same_tab(dn::Page a, dn::Page b) {
    const auto group = [](dn::Page p) { return p == dn::Page::upper ? dn::Page::lower : p; };
    return group(a) == group(b);
}

void paint_keys() {
    const PrefsUi ui = prefs_ui();
    for (const auto& [button, key] : s.keys) {
        prefs_style_button(button, false, s.editor.can_press(key));
    }
    if (s.space != nullptr) {
        prefs_style_button(s.space, false, s.editor.can_press(" "));
    }
    for (std::size_t i = 0; i < s.tabs.size(); ++i) {
        prefs_style_button(s.tabs[i], same_tab(static_cast<dn::Page>(i), s.editor.page()), true);
    }
    if (s.shift != nullptr) {
        prefs_style_button(s.shift, s.editor.shifted(), true);
    }
    (void)ui;
}

void build_keys() {
    const PrefsUi ui = prefs_ui();
    const iclforge::screen::Metrics& m = *ui.m;
    if (s.keys_box == nullptr) {
        return;
    }
    lv_obj_clean(s.keys_box);
    s.keys.clear();
    const std::vector<std::string> keys = dn::page_keys(s.editor.page(), s.editor.shifted());
    const int key_w = m.row_target;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        lv_obj_t* const button = make_button(ui, s.keys_box, keys[i], key_w, m.row_target, Op::kKey, i);
        s.keys.emplace_back(button, keys[i]);
    }
    paint_keys();
}

void paint_field() {
    const PrefsUi ui = prefs_ui();
    const iclforge::screen::Metrics& m = *ui.m;
    if (s.field_text == nullptr) {
        return;
    }
    const std::string& typed = s.editor.text();
    lv_label_set_text(s.field_text, typed.c_str());
    const int text_w = prefs_text_width(ui.body, typed);
    const int view_w = s.field_w;
    // The end of a long name stays in view, with the caret after it.
    const int x = text_w + 6 > view_w ? view_w - text_w - 6 : 0;
    lv_obj_set_pos(s.field_text, x, (m.row_target - lv_font_get_line_height(ui.body)) / 2);
    lv_obj_set_pos(s.caret, x + text_w + 2, (m.row_target - lv_font_get_line_height(ui.body)) / 2);

    const std::size_t used = dn::kMaxBytes - s.editor.bytes_left();
    lv_label_set_text(s.counter, (std::to_string(used) + "/" + std::to_string(dn::kMaxBytes)).c_str());
    lv_obj_set_style_text_color(s.counter, color(s.editor.bytes_left() == 0 ? ui.c->accent : ui.c->muted), 0);

    const dn::Check check = s.editor.check();
    std::string said;
    if (!s.typed_message.empty()) {
        said = s.typed_message;
    } else if (!typed.empty() && !check.ok()) {
        said = dn::problem_text(check.problem);
    }
    lv_label_set_text(s.reason, said.c_str());
    prefs_style_button(s.save, s.editor.can_accept(), s.editor.can_accept());
    prefs_style_button(s.back, false, !typed.empty());
    paint_keys();
    s.field_dirty = false;
}

void build_name() {
    const PrefsUi ui = prefs_ui();
    const iclforge::screen::Metrics& m = *ui.m;
    const prefs::Entry entry = prefs::resolve(*s.pref, preferences_backend(), preferences_backend().caps());
    s.editor = dn::Editor(entry.value);
    s.typed_message.clear();
    std::vector<lv_obj_t*> made;
    lv_obj_t* body = nullptr;
    s.page_root = make_page_root(ui, s.pref->label, {{"Cancel", Op::kClose, false}, {"Save", Op::kSave, true}}, made, &body);
    s.save = made.size() > 1 ? made[1] : nullptr;
    const int content_w = m.width - 2 * m.margin;
    lv_obj_set_style_pad_row(body, m.gap / 2, 0);

    // The name as typed, with how much of its 32 bytes is used, and the key that takes the last letter back.
    lv_obj_t* const field = prefs_box(body);
    lv_obj_set_size(field, content_w, m.row_target);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(field, color(ui.c->surface), 0);
    lv_obj_set_style_outline_width(field, 1, 0);
    lv_obj_set_style_outline_color(field, color(ui.c->faint), 0);
    s.back = prefs_button(field, "\xE2\x86\x90", m.row_target, m.row_target, ui.c->surface_up, ui.c->text, &on_event,
                          pack(Op::kBackspace, 0), nullptr);  // a left arrow
    lv_obj_set_pos(s.back, content_w - m.row_target, 0);
    s.counter = prefs_label(field, "", ui.note, ui.c->muted);
    const int counter_w = prefs_text_width(ui.note, "32/32");
    lv_obj_set_pos(s.counter, content_w - m.row_target - m.gap - counter_w, (m.row_target - lv_font_get_line_height(ui.note)) / 2);
    s.field_w = content_w - m.row_target - 3 * m.gap - counter_w - m.pad;
    s.field_view = prefs_box(field);
    lv_obj_set_size(s.field_view, s.field_w, m.row_target);
    lv_obj_set_pos(s.field_view, m.pad, 0);
    s.field_text = prefs_label(s.field_view, "", ui.body, ui.c->text);
    lv_label_set_long_mode(s.field_text, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s.field_text, LV_SIZE_CONTENT);
    s.caret = prefs_box(s.field_view);
    lv_obj_set_size(s.caret, 2, lv_font_get_line_height(ui.body));
    lv_obj_set_style_bg_opa(s.caret, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s.caret, color(ui.c->accent), 0);

    // Why the name is not one yet, or what the board answered.
    s.reason = prefs_paragraph(body, "", ui.note, ui.c->accent, content_w);

    // The keys scroll when the display is too small for them all: the strip of pages, the keys, the space bar.
    lv_obj_t* const scroll = make_scroller(ui, body, content_w);
    lv_obj_set_style_pad_row(scroll, m.gap, 0);
    lv_obj_set_style_pad_bottom(scroll, m.margin, 0);
    lv_obj_t* const strip = prefs_box(scroll);
    lv_obj_set_size(strip, content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(strip, m.gap, 0);
    lv_obj_set_style_pad_row(strip, m.gap, 0);
    s.tabs.clear();
    for (const dn::Page page : {dn::Page::lower, dn::Page::upper, dn::Page::symbols, dn::Page::accents_latin1, dn::Page::accents_extended}) {
        // Five tabs would be one too many: the upper-case page has no tab of its own (it is the shift of "abc").
        if (page == dn::Page::upper) {
            s.tabs.push_back(nullptr);
            continue;
        }
        const std::string text = tab_text(page);
        s.tabs.push_back(make_button(ui, strip, text, button_width(ui, text), m.row_target, Op::kTab, static_cast<std::size_t>(page)));
    }
    s.shift = make_button(ui, strip, "\xE2\x86\x91", m.row_target, m.row_target, Op::kShift);  // an up arrow
    s.keys_box = prefs_box(scroll);
    lv_obj_set_size(s.keys_box, content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s.keys_box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s.keys_box, m.gap, 0);
    lv_obj_set_style_pad_row(s.keys_box, m.gap, 0);
    s.space = make_button(ui, scroll, "Space", std::min(content_w, 5 * (m.row_target + m.gap)), m.row_target, Op::kSpace);
    build_keys();
    paint_field();
}

// --- About -------------------------------------------------------------------------------------------

[[nodiscard]] std::string about_key(const std::vector<iclforge::aboutview::Row>& rows) {
    std::string key;
    for (const iclforge::aboutview::Row& row : rows) {
        key += row.label + "=" + row.value + "|";
    }
    return key;
}

void fill_about(const std::vector<iclforge::aboutview::Row>& rows) {
    const PrefsUi ui = prefs_ui();
    const iclforge::screen::Metrics& m = *ui.m;
    const int content_w = m.width - 2 * m.margin;
    lv_obj_clean(s.list);
    // A label and its value side by side on a wide display, one over the other on a narrow one.
    const bool side_by_side = m.width >= 480;
    int label_w = 0;
    for (const iclforge::aboutview::Row& row : rows) {
        label_w = std::max(label_w, prefs_text_width(ui.note, row.label));
    }
    label_w += 2 * m.gap;
    for (const iclforge::aboutview::Row& row : rows) {
        lv_obj_t* const box = prefs_box(s.list);
        lv_obj_set_size(box, content_w - 8, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, side_by_side ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_top(box, m.gap, 0);
        lv_obj_set_style_pad_bottom(box, m.gap, 0);
        lv_obj_set_style_border_side(box, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, color(ui.c->faint), 0);
        lv_obj_t* const label = prefs_label(box, row.label, ui.note, ui.c->muted);
        if (side_by_side) {
            lv_obj_set_width(label, label_w);
            // The note is smaller than the body: the label sits on the value's first line.
            lv_obj_set_style_pad_top(label, (lv_font_get_line_height(ui.body) - lv_font_get_line_height(ui.note)) / 2, 0);
        }
        prefs_paragraph(box, row.value, ui.body, tone_color(ui, row.tone), side_by_side ? content_w - 8 - label_w : content_w - 8);
    }
}

void build_about() {
    const PrefsUi ui = prefs_ui();
    std::vector<lv_obj_t*> made;
    lv_obj_t* body = nullptr;
    s.page_root = make_page_root(ui, s.pref->label, {{"Close", Op::kClose, true}}, made, &body);
    s.list = make_scroller(ui, body, ui.m->width - 2 * ui.m->margin);
    const std::vector<iclforge::aboutview::Row> rows = iclforge::aboutview::about_rows(preferences_backend().facts());
    s.painted = about_key(rows);
    fill_about(rows);
    s.read_ms = now_ms();
}

// --- the paired servers ----------------------------------------------------------------------------

[[nodiscard]] bool streaming_now() {
    const BoardInfo info = read_board_info();
    return !info.stream.empty() && info.stream != "idle";
}

[[nodiscard]] std::string rows_key(const std::vector<sa::paired::Row>& rows) {
    std::string key;
    for (const sa::paired::Row& row : rows) {
        key += row.server_id + "|" + row.title + "|" + row.detail + "|" + (row.playing_now ? "1" : "0") + ";";
    }
    return key;
}

void fill_servers() {
    const PrefsUi ui = prefs_ui();
    const iclforge::screen::Metrics& m = *ui.m;
    const int content_w = m.width - 2 * m.margin - 8;
    lv_obj_clean(s.list);
    if (s.rows.empty()) {
        prefs_paragraph(s.list, sa::paired::empty_text(), ui.body, ui.c->muted, content_w);
        return;
    }
    for (std::size_t i = 0; i < s.rows.size(); ++i) {
        const sa::paired::Row& row = s.rows[i];
        lv_obj_t* const line = prefs_box(s.list);
        lv_obj_set_size(line, content_w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(line, m.gap, 0);
        lv_obj_set_style_pad_bottom(line, m.gap, 0);
        lv_obj_set_style_border_side(line, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(line, 1, 0);
        lv_obj_set_style_border_color(line, color(ui.c->faint), 0);
        const int button_w = button_width(ui, "Forget");
        const int text_w = content_w - button_w - m.gap;
        lv_obj_t* const words = prefs_box(line);
        lv_obj_set_size(words, text_w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(words, m.gap / 2, 0);
        // The server's name wraps rather than ending in dots: a person chooses what to forget by it, and on a narrow
        // display "Hearth on STUDIO-PC" and "Hearth on STUDIO-PC-2" would look alike in dots.
        prefs_paragraph(words, row.title, ui.body, tone_color(ui, row.tone), text_w);
        prefs_paragraph(words, row.detail, ui.note, row.playing_now ? ui.c->accent : ui.c->muted, text_w);
        make_button(ui, line, "Forget", button_w, m.row_target, Op::kForget, i);
    }
}

void read_servers() {
    prefs::Backend& backend = preferences_backend();
    const std::vector<sa::paired::Server> servers = backend.paired_servers();
    s.total_servers = servers.size();
    s.rows = sa::paired::describe(servers, streaming_now());
    s.read_ms = now_ms();
}

void build_servers() {
    const PrefsUi ui = prefs_ui();
    std::vector<lv_obj_t*> made;
    lv_obj_t* body = nullptr;
    s.page_root = make_page_root(ui, s.pref->label, {{"Close", Op::kClose, true}}, made, &body);
    s.list = make_scroller(ui, body, ui.m->width - 2 * ui.m->margin);
    read_servers();
    s.painted = rows_key(s.rows);
    fill_servers();
}

// --- events ------------------------------------------------------------------------------------------

void on_event(lv_event_t* event) {
    const auto code = reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event));
    const auto op = static_cast<Op>(code & 0xFFU);
    const std::size_t index = code >> 8;
    switch (op) {
        case Op::kCancel:
        case Op::kOk:
            s.want_close_dialog = true;
            break;
        case Op::kAccept:
            accept_dialog();
            break;
        case Op::kHoldDown:
            s.hold.press(now_ms());
            break;
        case Op::kHoldUp:
            s.hold.release();
            break;
        case Op::kClose:
            s.want_close_page = true;
            break;
        case Op::kSave: {
            if (!s.editor.can_accept()) {
                break;
            }
            const prefs::SetResult result = prefs::set_value(preferences_backend(), *s.pref, s.editor.result());
            if (result.ok) {
                prefs_say("");
                prefs_changed();
                s.want_close_page = true;
            } else {
                s.typed_message = result.message;
                s.field_dirty = true;
            }
            break;
        }
        case Op::kKey: {
            const dn::Page before = s.editor.page();
            if (index < s.keys.size() && s.editor.press(s.keys[index].second)) {
                s.typed_message.clear();
                s.field_dirty = true;
                // A capital typed on the one-shot shift puts the keyboard back to lower case.
                if (s.editor.page() != before) {
                    s.keys_rebuild = true;
                }
            }
            break;
        }
        case Op::kSpace:
            if (s.editor.press(" ")) {
                s.typed_message.clear();
                s.field_dirty = true;
            }
            break;
        case Op::kBackspace:
            s.editor.backspace();
            s.typed_message.clear();
            s.field_dirty = true;
            break;
        case Op::kTab:
            s.editor.show(static_cast<dn::Page>(index));
            s.keys_rebuild = true;
            s.field_dirty = true;
            break;
        case Op::kShift:
            s.editor.shift();
            s.keys_rebuild = true;
            s.field_dirty = true;
            break;
        case Op::kForget:
            if (index < s.rows.size() && s.pref != nullptr) {
                const sa::paired::Row& row = s.rows[index];
                const std::optional<std::string> body = sa::paired::forget_body(row.server_id);
                if (body) {
                    open_dialog(*s.pref, *body, sa::paired::forget_confirm(row, s.total_servers), State::Then::kSet);
                }
            }
            break;
    }
}

}  // namespace

void pages_open(const prefs::Pref& pref, const std::string& word) {
    if (pages_active()) {
        return;
    }
    switch (pref.kind) {
        case prefs::Kind::kText:
            s.pref = &pref;
            s.page = Page::kName;
            s.want_page = true;
            break;
        case prefs::Kind::kInfo:
            s.pref = &pref;
            s.page = Page::kAbout;
            s.want_page = true;
            break;
        case prefs::Kind::kList:
            s.pref = &pref;
            s.page = Page::kServers;
            s.want_page = true;
            break;
        case prefs::Kind::kChoice:
        case prefs::Kind::kStepper:
        case prefs::Kind::kToggle:
        case prefs::Kind::kAction:
            if (pref.confirm_step != nullptr) {
                open_dialog(pref, word, pref.confirm_step(preferences_backend(), word), State::Then::kSet);
            }
            break;
        case prefs::Kind::kSheet:
            break;  // the Speakers sheet is opened by sound_sheet_open(), not by a page of this file
    }
}

bool pages_active() {
    return s.page_root != nullptr || s.dialog != nullptr || s.want_page || s.want_dialog;
}

lv_obj_t* pages_root() { return s.dialog != nullptr ? s.dialog : s.page_root; }

void pages_close() {
    delete_dialog();
    delete_page();
    s = State{};
}

void pages_update(std::uint32_t /*sheet_ms*/) {
    const std::uint32_t now = now_ms();
    // A dialog taken down first, so that a page it was over is what is left.
    if (s.want_close_dialog) {
        s.want_close_dialog = false;
        delete_dialog();
        s.dialog_pref = nullptr;
        // The list a Forget was made from is read again, so that the row is gone.
        if (s.page == Page::kServers) {
            s.read_ms = 0;
        }
    }
    if (s.want_close_page) {
        s.want_close_page = false;
        delete_dialog();
        delete_page();
    }
    if (s.want_page) {
        s.want_page = false;
        switch (s.page) {
            case Page::kName:
                build_name();
                break;
            case Page::kAbout:
                build_about();
                break;
            case Page::kServers:
                build_servers();
                break;
            case Page::kNone:
                break;
        }
    }
    if (s.want_dialog || s.rebuild_dialog) {
        s.want_dialog = false;
        s.rebuild_dialog = false;
        build_dialog();
        s.hold.reset();
    }

    // The held button of the factory reset: its fill follows the finger, and the hold that goes the whole way is the answer.
    if (s.hold_button != nullptr && s.dialog != nullptr) {
        const float progress = s.hold.progress(now);
        lv_obj_set_width(s.hold_fill, std::max(1, static_cast<int>(progress * static_cast<float>(s.hold_width))));
        if (s.hold.done(now)) {
            accept_dialog();
        }
    }

    switch (s.page) {
        case Page::kName:
            if (s.keys_rebuild) {
                s.keys_rebuild = false;
                build_keys();
            }
            if (s.field_dirty) {
                paint_field();
            }
            break;
        case Page::kAbout:
            if (s.list != nullptr && now - s.read_ms >= kReadEveryMs) {
                s.read_ms = now;
                const std::vector<iclforge::aboutview::Row> rows = iclforge::aboutview::about_rows(preferences_backend().facts());
                const std::string key = about_key(rows);
                if (key != s.painted) {
                    s.painted = key;
                    fill_about(rows);
                }
            }
            break;
        case Page::kServers:
            if (s.list != nullptr && s.dialog == nullptr && (s.read_ms == 0 || now - s.read_ms >= kReadEveryMs)) {
                read_servers();
                const std::string key = rows_key(s.rows);
                if (key != s.painted) {
                    s.painted = key;
                    fill_servers();
                }
            }
            break;
        case Page::kNone:
            break;
    }
}

}  // namespace player::panel
