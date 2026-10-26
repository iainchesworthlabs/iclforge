// The Speaker layout sheet. See layout_sheet.hpp.
//
// The layouts, the greyed buttons and the sentence that says why are libs/device's (layout_builder.hpp over the one
// verdict in layout_options.hpp), the room is libs/render's (room_plan.hpp: plan_spots and plan_nodes_fit), and the board's
// side - the set, the playing - is the preferences' Backend. This file measures the controls, draws them and carries a
// tap to those. Everything is built when the sheet opens and painted again when anything moved; nothing is deleted inside
// an event, a repaint is the next frame's.

#include "layout_sheet.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/layout_builder.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"
#include "iclforge/screen_metrics.hpp"

#include "board_info.hpp"
#include "prefs_screen.hpp"

namespace lb = iclforge;
namespace rp = iclforge::render;

namespace player::panel {

namespace {

constexpr std::uint32_t kRefreshMs = 1000;
// How long a refusal stays in the card.
constexpr std::uint32_t kHintShownMs = 6000;
// The room is drawn beside the buttons when the content is at least this wide (a 480 by 320 panel and wider); a narrower
// display has it at the top of the scrolling part.
constexpr int kPaneMinContentWidth = 440;
// The card is fixed under the buttons where the area between the bar and the footer is at least this tall (the kit's 448 and
// an 800 by 480's 344); on a shorter one it scrolls with them.
constexpr int kCardFixedMinArea = 330;

enum class Op : int {
    kQuick = 0,
    kStep,
    kTabQuick,
    kTabBuild,
    kApply,
    kCancel,
    kConfirm,
    kDecline,
};

struct Sheet {
    bool want_open = false;
    bool want_close = false;
    bool open = false;
    bool dirty = true;

    lv_obj_t* root = nullptr;
    lv_obj_t* tab_quick = nullptr;
    lv_obj_t* tab_build = nullptr;
    lv_obj_t* scroll = nullptr;
    lv_obj_t* card_holder = nullptr;
    lv_obj_t* card = nullptr;
    lv_obj_t* card_title = nullptr;
    lv_obj_t* card_text = nullptr;
    lv_obj_t* capacity = nullptr;
    lv_obj_t* quick_page = nullptr;
    lv_obj_t* build_page = nullptr;
    lv_obj_t* mount_note = nullptr;
    lv_obj_t* diagram = nullptr;
    lv_obj_t* foot_name = nullptr;
    lv_obj_t* foot_state = nullptr;
    lv_obj_t* apply = nullptr;
    lv_obj_t* confirm = nullptr;

    std::vector<lb::QuickState> quick_states;
    std::vector<lv_obj_t*> chips;
    std::array<std::vector<lv_obj_t*>, 4> steps;

    // Whether the room is a pane beside the buttons, or the top of the scrolling part.
    bool pane = false;
    // Whether the card is fixed under the buttons (a display tall enough to spare it the room), or at the top of what scrolls.
    bool card_fixed = false;
    // What the card said last: where it scrolls, a card with something new to say brings the top of the page into view.
    std::string card_said;
    // The geometry the room is drawn in, and the width the footer's text may take.
    int diagram_w = 0;
    int diagram_h = 0;
    int footer_text_w = 0;

    bool build_tab = false;
    // The working layout, and what is in force: as the word the board holds, and as a pick (empty when it is a speaker
    // list the sheet cannot show as buttons; ":upfiring" and ":height" read as High, which plays the same).
    lb::LayoutPick pick;
    std::string in_force_text;
    std::optional<lb::LayoutPick> in_force;
    // A sentence about the layout in force that is not the verdict's: a speaker list that choosing here replaces.
    std::string info;
    // The last refusal, or a greyed button's reason, for a few seconds, and what the card calls it.
    std::string hint;
    std::string hint_title;
    std::uint32_t hint_ms = 0;
    std::uint32_t now_ms = 0;
    std::uint32_t refreshed_ms = 0;
    // What was painted last: the sheet is repainted when this changes.
    std::string painted;
};

Sheet g;

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

[[nodiscard]] std::uintptr_t pack(Op op, std::size_t a, std::size_t b) {
    return (static_cast<std::uintptr_t>(b) << 16) | (static_cast<std::uintptr_t>(a) << 8) |
           static_cast<std::uintptr_t>(static_cast<int>(op));
}

void on_target(lv_event_t* event);

constexpr std::array<lb::PickField, 4> kFields{lb::PickField::kAround, lb::PickField::kSubwoofers,
                                               lb::PickField::kHeights, lb::PickField::kMount};

[[nodiscard]] const char* field_heading(lb::PickField field) {
    switch (field) {
        case lb::PickField::kAround: return "Around you";
        case lb::PickField::kSubwoofers: return "Subwoofers";
        case lb::PickField::kHeights: return "Heights";
        case lb::PickField::kMount: return "Heights are";
    }
    return "";
}

// The value of the index-th button of a field.
[[nodiscard]] int step_value(lb::PickField field, std::size_t index) {
    switch (field) {
        case lb::PickField::kAround: return lb::kAroundSteps[std::min(index, lb::kAroundSteps.size() - 1)];
        case lb::PickField::kSubwoofers: return lb::kSubwooferSteps[std::min(index, lb::kSubwooferSteps.size() - 1)];
        case lb::PickField::kHeights: return lb::kHeightSteps[std::min(index, lb::kHeightSteps.size() - 1)];
        case lb::PickField::kMount: return static_cast<int>(std::min<std::size_t>(index, lb::kMountSteps - 1));
    }
    return 0;
}

[[nodiscard]] std::size_t field_index(lb::PickField field) { return static_cast<std::size_t>(field); }

[[nodiscard]] bool hint_live() { return !g.hint.empty() && g.now_ms - g.hint_ms < kHintShownMs; }

// Whether the working layout is the one in force: the same pick, so that High is High however the board was told.
[[nodiscard]] bool is_in_force() { return g.in_force.has_value() && *g.in_force == g.pick; }

// --- building --------------------------------------------------------------------------------------

[[nodiscard]] int chip_height(const PrefsUi& ui) {
    return std::max(iclforge::screen::kMinTargetPx, ui.m->row_target - 8);
}

// A button as wide as its text and a half-pad each side asks, and never narrower than a fingertip.
[[nodiscard]] int chip_width(const PrefsUi& ui, const std::string& text) {
    return std::max(iclforge::screen::kMinTargetPx, prefs_text_width(ui.body, text) + ui.m->pad);
}

lv_obj_t* make_column(lv_obj_t* parent, int width, int row_gap) {
    lv_obj_t* const column = prefs_box(parent);
    lv_obj_set_size(column, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, row_gap, 0);
    return column;
}

lv_obj_t* make_wrap_row(const PrefsUi& ui, lv_obj_t* parent, int width) {
    lv_obj_t* const row = prefs_box(parent);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, ui.m->gap, 0);
    lv_obj_set_style_pad_row(row, ui.m->gap, 0);
    return row;
}

lv_obj_t* make_heading(const PrefsUi& ui, lv_obj_t* parent, const std::string& text, int width) {
    lv_obj_t* const heading = prefs_label(parent, text, ui.heading, ui.c->muted);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(heading, width);
    return heading;
}

void build_quick_page(const PrefsUi& ui, lv_obj_t* parent, int cw) {
    g.quick_page = make_column(parent, cw, ui.m->gap);
    g.chips.assign(g.quick_states.size(), nullptr);
    for (const lb::QuickGroup group : {lb::QuickGroup::kAroundYou, lb::QuickGroup::kWithHeights}) {
        make_heading(ui, g.quick_page, std::string{lb::group_name(group)}, cw);
        lv_obj_t* const row = make_wrap_row(ui, g.quick_page, cw);
        for (std::size_t i = 0; i < g.quick_states.size(); ++i) {
            const lb::QuickState& state = g.quick_states[i];
            if (state.group != group) {
                continue;
            }
            g.chips[i] = prefs_button(row, state.verdict.name, chip_width(ui, state.verdict.name), chip_height(ui),
                                      ui.c->surface_up, ui.c->text, &on_target, pack(Op::kQuick, i, 0), nullptr);
        }
    }
}

void build_build_page(const PrefsUi& ui, lv_obj_t* parent, int cw) {
    const int gap = ui.m->gap;
    g.build_page = make_column(parent, cw, gap);
    const iclforge::BoardCaps caps = preferences_backend().caps();
    // A group's name goes at the left of its buttons where the widest group fits on one line, and above them where not.
    int label_w = 0;
    for (const lb::PickField field : kFields) {
        label_w = std::max(label_w, prefs_text_width(ui.heading, field_heading(field)) + 2);
    }
    for (const lb::PickField field : kFields) {
        const std::vector<lb::PickStep> steps = lb::pick_steps(caps, g.pick, field);
        int buttons_w = 0;
        for (const lb::PickStep& step : steps) {
            buttons_w += chip_width(ui, step.label) + gap;
        }
        buttons_w = std::max(0, buttons_w - gap);
        const bool one_line = label_w + 2 * gap + buttons_w <= cw;
        lv_obj_t* row = nullptr;
        if (one_line) {
            row = prefs_box(g.build_page);
            lv_obj_set_size(row, cw, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 2 * gap, 0);
            make_heading(ui, row, field_heading(field), label_w);
            lv_obj_t* const buttons = prefs_box(row);
            lv_obj_set_size(buttons, buttons_w, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(buttons, gap, 0);
            row = buttons;
        } else {
            make_heading(ui, g.build_page, field_heading(field), cw);
            row = make_wrap_row(ui, g.build_page, cw);
        }
        std::vector<lv_obj_t*>& buttons = g.steps[field_index(field)];
        buttons.clear();
        for (std::size_t i = 0; i < steps.size(); ++i) {
            buttons.push_back(prefs_button(row, steps[i].label, chip_width(ui, steps[i].label), chip_height(ui),
                                           ui.c->surface_up, ui.c->text, &on_target,
                                           pack(Op::kStep, field_index(field), i), nullptr));
        }
    }
    g.mount_note = prefs_paragraph(g.build_page,
                                   "High is on the wall or up-firing; the ceiling is straight overhead.",
                                   ui.note, ui.c->muted, cw);
}

// The card: what the layout cannot do, or has as a limit, or a refusal. A bordered panel; its texts are set by repaint().
lv_obj_t* build_card(const PrefsUi& ui, lv_obj_t* parent, int width) {
    const iclforge::screen::Metrics& m = *ui.m;
    lv_obj_t* const card = prefs_box(parent);
    lv_obj_set_size(card, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, color(ui.c->surface), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, color(ui.c->faint), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, m.gap, 0);
    lv_obj_set_style_pad_row(card, m.gap / 2, 0);
    const int inner = width - 2 * m.gap - 2;
    g.card_title = prefs_label(card, "", ui.heading, ui.c->text);
    lv_label_set_long_mode(g.card_title, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(g.card_title, inner);
    g.card_text = prefs_paragraph(card, "", ui.note, ui.c->muted, inner);
    return card;
}

void build_view() {
    const PrefsUi ui = prefs_ui();
    const iclforge::screen::Metrics& m = *ui.m;
    const int content_w = m.width - 2 * m.margin;
    const bool pane = content_w >= kPaneMinContentWidth;
    g.pane = pane;
    g.card_said.clear();
    const int footer_h = m.row_target + 2 * m.gap;
    const int area_h = m.height - m.bar - footer_h;
    // A card of three or four lines under the buttons would leave a short display a few pixels to scroll them in.
    g.card_fixed = pane && area_h >= kCardFixedMinArea;

    g.quick_states = lb::quick_states(preferences_backend().caps());
    g.chips.clear();
    for (auto& buttons : g.steps) {
        buttons.clear();
    }

    lv_obj_t* const root = prefs_box(lv_screen_active());
    g.root = root;
    lv_obj_set_size(root, m.width, m.height);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, color(ui.c->bg), 0);
    // A tap on the sheet is the sheet's: it never reaches the one under it.
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);

    // The bar: the title and Cancel.
    lv_obj_t* const bar = prefs_box(root);
    lv_obj_set_size(bar, m.width, m.bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, color(ui.c->faint), 0);
    lv_obj_t* const title = prefs_label(bar, "Speaker layout", ui.title, ui.c->text);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(title, m.margin, (m.bar - lv_font_get_line_height(ui.title)) / 2);
    const int cancel_w = std::max(m.target, prefs_text_width(ui.body, "Cancel") + 2 * m.pad);
    lv_obj_t* const cancel = prefs_button(bar, "Cancel", cancel_w, m.bar_button, ui.c->surface_up, ui.c->text, &on_target,
                                          pack(Op::kCancel, 0, 0), nullptr);
    lv_obj_set_pos(cancel, m.width - m.margin - cancel_w, (m.bar - m.bar_button) / 2);

    // The footer: the layout's name and whether it can be had, and Apply.
    lv_obj_t* const footer = prefs_box(root);
    lv_obj_set_size(footer, m.width, footer_h);
    lv_obj_set_pos(footer, 0, m.height - footer_h);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(footer, 2, 0);
    lv_obj_set_style_border_color(footer, color(ui.c->faint), 0);
    const int apply_w = std::max(2 * m.row_target, prefs_text_width(ui.body, "Apply") + 2 * m.pad);
    g.apply = prefs_button(footer, "Apply", apply_w, m.row_target, ui.c->accent, ui.c->bg, &on_target,
                           pack(Op::kApply, 0, 0), nullptr);
    lv_obj_set_pos(g.apply, m.width - m.margin - apply_w, (footer_h - m.row_target) / 2);
    g.footer_text_w = std::max(m.width - 2 * m.margin - apply_w - m.gap, 1);
    const int name_h = lv_font_get_line_height(ui.heading);
    const int state_h = lv_font_get_line_height(ui.note);
    const int block_y = (footer_h - name_h - state_h) / 2;
    g.foot_name = prefs_label(footer, "", ui.heading, ui.c->text);
    lv_label_set_long_mode(g.foot_name, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(g.foot_name, g.footer_text_w, name_h);
    lv_obj_set_pos(g.foot_name, m.margin, block_y);
    g.foot_state = prefs_label(footer, "", ui.note, ui.c->muted);
    lv_label_set_long_mode(g.foot_state, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(g.foot_state, g.footer_text_w, state_h);
    lv_obj_set_pos(g.foot_state, m.margin, block_y + name_h);

    // Where the room goes: a pane of its own beside the buttons where the display is wide enough; on a narrow one, the end
    // of what scrolls, under the buttons that are the point of the sheet.
    int column_w = m.width;
    int cw = content_w;
    int right_pad = m.margin;
    if (pane) {
        const int right_w = std::clamp(content_w * 36 / 100, 150, 420);
        const int right_x = m.width - m.margin - right_w;
        column_w = right_x - m.gap;
        right_pad = m.gap;
        cw = column_w - m.margin - right_pad;
        g.diagram_w = right_w;
        g.diagram_h = area_h - 2 * m.gap;
        g.diagram = prefs_box(root);
        lv_obj_set_pos(g.diagram, right_x, m.bar + m.gap);
    } else {
        g.diagram_w = cw;
        g.diagram_h = std::clamp(cw * 45 / 100, 90, 200);
    }

    // The column of the buttons. In a pane the tabs (and, on a display tall enough, the card) are fixed and the rest
    // scrolls; on a narrow display everything scrolls, the tabs first, so that the buttons have all the room there is.
    lv_obj_t* const column = prefs_box(root);
    lv_obj_set_size(column, column_w, area_h);
    lv_obj_set_pos(column, 0, m.bar);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);

    const int tab_w = std::max(chip_width(ui, "Quick picks"), chip_width(ui, "Build yours"));
    const auto make_tabs = [&](lv_obj_t* parent, int width, int pad_left, int pad_right, int pad_top) {
        lv_obj_t* const tabs = prefs_box(parent);
        lv_obj_set_size(tabs, width, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_left(tabs, pad_left, 0);
        lv_obj_set_style_pad_right(tabs, pad_right, 0);
        lv_obj_set_style_pad_top(tabs, pad_top, 0);
        lv_obj_set_style_pad_column(tabs, m.gap, 0);
        g.tab_quick = prefs_button(tabs, "Quick picks", tab_w, chip_height(ui), ui.c->surface_up, ui.c->text, &on_target,
                                   pack(Op::kTabQuick, 0, 0), nullptr);
        g.tab_build = prefs_button(tabs, "Build yours", tab_w, chip_height(ui), ui.c->surface_up, ui.c->text, &on_target,
                                   pack(Op::kTabBuild, 0, 0), nullptr);
    };
    if (pane) {
        make_tabs(column, column_w, m.margin, right_pad, m.gap);
    }

    lv_obj_t* const scroll = prefs_box(column);
    g.scroll = scroll;
    lv_obj_set_size(scroll, column_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll, color(ui.c->muted), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(scroll, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll, 3, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(scroll, m.margin, 0);
    lv_obj_set_style_pad_right(scroll, right_pad, 0);
    lv_obj_set_style_pad_top(scroll, m.gap, 0);
    lv_obj_set_style_pad_bottom(scroll, m.gap, 0);
    lv_obj_set_style_pad_row(scroll, m.gap, 0);

    if (!pane) {
        make_tabs(scroll, cw, 0, 0, 0);
    }
    if (g.card_fixed) {
        // The card is fixed under the buttons, where the footer's Apply is: it shrinks what scrolls from the bottom, so
        // that the buttons a person is looking at do not move when it appears.
        g.card_holder = prefs_box(column);
        lv_obj_set_size(g.card_holder, column_w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_left(g.card_holder, m.margin, 0);
        lv_obj_set_style_pad_right(g.card_holder, right_pad, 0);
        lv_obj_set_style_pad_top(g.card_holder, m.gap, 0);
        lv_obj_set_style_pad_bottom(g.card_holder, m.gap, 0);
        g.card = build_card(ui, g.card_holder, cw);
    } else {
        g.card = build_card(ui, scroll, cw);
    }
    g.capacity = prefs_paragraph(scroll, "", ui.note, ui.c->muted, cw);
    build_quick_page(ui, scroll, cw);
    build_build_page(ui, scroll, cw);
    if (!pane) {
        g.diagram = prefs_box(scroll);
    }
    lv_obj_set_size(g.diagram, g.diagram_w, g.diagram_h);
    lv_obj_set_style_bg_opa(g.diagram, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.diagram, color(ui.c->surface), 0);
    lv_obj_set_style_border_width(g.diagram, 1, 0);
    lv_obj_set_style_border_color(g.diagram, color(ui.c->faint), 0);
    lv_obj_set_style_radius(g.diagram, 10, 0);
}

void take_down() {
    if (g.root != nullptr) {
        lv_obj_delete(g.root);
    }
    g.root = nullptr;
    g.tab_quick = g.tab_build = nullptr;
    g.scroll = g.card_holder = nullptr;
    g.card = g.card_title = g.card_text = nullptr;
    g.capacity = nullptr;
    g.quick_page = g.build_page = g.mount_note = nullptr;
    g.diagram = nullptr;
    g.foot_name = g.foot_state = nullptr;
    g.apply = nullptr;
    g.confirm = nullptr;  // a child of the root: deleted with it
    g.chips.clear();
    for (auto& buttons : g.steps) {
        buttons.clear();
    }
    g.painted.clear();
    g.open = false;
    g.want_close = false;
    prefs_changed();
}

// --- the room ----------------------------------------------------------------------------------------

void draw_room(const PrefsUi& ui, const lb::PickVerdict& verdict) {
    lv_obj_clean(g.diagram);
    const std::optional<rp::OutputLayout> layout = rp::OutputLayout::parse(lb::pick_name(g.pick));
    if (!layout) {
        // More than sixteen outputs: there is no room to draw, and the card says why.
        lv_obj_t* const label = prefs_paragraph(g.diagram, "No room for that many speakers.", ui.note, ui.c->muted,
                                                std::max(g.diagram_w - 2 * ui.m->gap, 1));
        lv_obj_center(label);
        return;
    }
    const int pad = ui.m->gap;
    const std::vector<rp::PlanSpot> spots = rp::plan_spots(*layout);
    const std::vector<rp::PlanNode> nodes = rp::plan_nodes_fit(spots, g.diagram_w - 2 * pad, g.diagram_h - 2 * pad);
    // A layout the board cannot carry is drawn quiet: the room it would be, not one that will play.
    const bool quiet = !verdict.accepted();
    for (std::size_t i = 0; i < nodes.size() && i < spots.size(); ++i) {
        const rp::PlanNode& node = nodes[i];
        if (node.shape == rp::PlanShape::kNone) {
            continue;
        }
        lv_obj_t* const shape = prefs_box(g.diagram);
        lv_obj_set_size(shape, node.w, node.h);
        lv_obj_set_pos(shape, pad + node.x - node.w / 2, pad + node.y - node.h / 2);
        lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
        const bool height = node.shape == rp::PlanShape::kHeight;
        lv_obj_set_style_bg_color(shape, color(height ? ui.c->surface : ui.c->surface_up), 0);
        lv_obj_set_style_border_width(shape, std::min(node.w, node.h) < 24 ? 1 : 2, 0);
        lv_obj_set_style_border_color(shape, color(quiet ? ui.c->faint : height ? ui.c->muted : ui.c->text), 0);
        const int radius = node.shape == rp::PlanShape::kEar   ? LV_RADIUS_CIRCLE
                           : node.shape == rp::PlanShape::kLfe ? node.h / 3
                                                               : std::max(3, node.w / 5);
        lv_obj_set_style_radius(shape, radius, 0);
        // The name goes inside a shape that is big enough for it to be read.
        const std::string& name = spots[i].name;
        if (prefs_text_width(ui.note, name) + 6 <= node.w) {
            lv_obj_t* const label = prefs_label(shape, name, ui.note, quiet ? ui.c->faint : ui.c->text);
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_center(label);
        }
    }
}

// --- painting --------------------------------------------------------------------------------------

[[nodiscard]] std::string capital(std::string text) {
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') {
        text[0] = static_cast<char>(text[0] - 'a' + 'A');
    }
    return text;
}

// A key for everything that decides what is painted: the sheet is painted again when it changes.
[[nodiscard]] std::string paint_key() {
    const iclforge::BoardCaps caps = preferences_backend().caps();
    std::string key = lb::pick_name(g.pick) + "|" + (g.build_tab ? "b" : "q") + "|" + g.in_force_text + "|" +
                      lb::capacity_line(caps) + "|" + g.hint + "|" + g.info + "|";
    key += hint_live() ? "h" : "-";
    return key;
}

void repaint() {
    const PrefsUi ui = prefs_ui();
    if (g.root == nullptr) {
        return;
    }
    const iclforge::BoardCaps caps = preferences_backend().caps();
    const lb::PickVerdict verdict = lb::judge_pick(caps, g.pick);
    const bool same = is_in_force();
    g.quick_states = lb::quick_states(caps);

    // Which page, and which tab says so.
    prefs_style_button(g.tab_quick, !g.build_tab, true);
    prefs_style_button(g.tab_build, g.build_tab, true);
    if (g.build_tab) {
        lv_obj_add_flag(g.quick_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g.build_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(g.quick_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g.build_page, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(g.capacity, lb::capacity_line(caps).c_str());

    for (std::size_t i = 0; i < g.chips.size() && i < g.quick_states.size(); ++i) {
        if (g.chips[i] != nullptr) {
            prefs_style_button(g.chips[i], g.quick_states[i].pick == g.pick, g.quick_states[i].verdict.accepted());
        }
    }
    for (const lb::PickField field : kFields) {
        const std::vector<lb::PickStep> steps = lb::pick_steps(caps, g.pick, field);
        const std::vector<lv_obj_t*>& buttons = g.steps[field_index(field)];
        for (std::size_t i = 0; i < buttons.size() && i < steps.size(); ++i) {
            prefs_style_button(buttons[i], steps[i].value == lb::field_value(g.pick, field), steps[i].available);
        }
    }
    if (g.pick.heights > 0) {
        lv_obj_remove_flag(g.mount_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g.mount_note, LV_OBJ_FLAG_HIDDEN);
    }

    // The footer: the summary where it fits and the name where it does not; and whether it can be had.
    const std::string summary = lb::pick_summary(g.pick);
    const bool full_fits = prefs_text_width(ui.heading, summary) <= g.footer_text_w;
    lv_label_set_text(g.foot_name, (full_fits ? summary : lb::pick_name(g.pick)).c_str());
    const char* const state = same                                                ? "In use now"
                              : verdict.verdict == lb::LayoutVerdict::supported   ? "Supported"
                              : verdict.verdict == lb::LayoutVerdict::with_limits ? "With a limit"
                                                                                  : "Not available";
    lv_label_set_text(g.foot_state, state);
    lv_obj_set_style_text_color(g.foot_state, color(verdict.accepted() ? ui.c->muted : ui.c->accent), 0);
    // Apply is the accent when there is something to apply, and quiet otherwise.
    prefs_style_button(g.apply, verdict.accepted() && !same, verdict.accepted() && !same);

    // The card: a refusal first, then what is wrong with the layout or limits it, then a sentence about what is in force;
    // and the words for the layout where the footer had no room for them.
    std::string title;
    std::string text;
    std::uint32_t title_rgb = ui.c->text;
    if (hint_live()) {
        title = g.hint_title;
        text = g.hint;
        title_rgb = ui.c->accent;
    } else if (!verdict.accepted()) {
        title = "Not available on this board";
        text = verdict.reason;
        title_rgb = ui.c->accent;
    } else if (verdict.verdict == lb::LayoutVerdict::with_limits) {
        title = "Works, with a limit";
        text = verdict.reason;
    } else if (!g.info.empty()) {
        title = "In use now";
        text = g.info;
    }
    if (!full_fits) {
        const std::string words = capital(lb::pick_words(g.pick)) + ".";
        if (title.empty()) {
            title = lb::pick_name(g.pick);
            text = words;
        } else {
            text = words + " " + text;
        }
    }
    lv_obj_t* const shown = g.card_holder != nullptr ? g.card_holder : g.card;
    if (title.empty()) {
        lv_obj_add_flag(shown, LV_OBJ_FLAG_HIDDEN);
        g.card_said.clear();
    } else {
        lv_obj_remove_flag(shown, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g.card_title, title.c_str());
        lv_obj_set_style_text_color(g.card_title, color(title_rgb), 0);
        lv_label_set_text(g.card_text, text.c_str());
        // On a narrow display the card is at the top of what scrolls: something new to say brings it into view.
        const std::string said = title + "|" + text;
        if (!g.card_fixed && said != g.card_said && g.scroll != nullptr) {
            lv_obj_scroll_to_y(g.scroll, 0, LV_ANIM_OFF);
        }
        g.card_said = said;
    }

    draw_room(ui, verdict);
    g.painted = paint_key();
}

// --- what a tap does -------------------------------------------------------------------------------

void say(const std::string& text, const char* title) {
    g.hint = text;
    g.hint_title = title;
    g.hint_ms = g.now_ms;
    g.dirty = true;
}

void close_confirm() {
    if (g.confirm != nullptr) {
        lv_obj_add_flag(g.confirm, LV_OBJ_FLAG_HIDDEN);
    }
}

// The question before a change that breaks the music that plays (Pref::confirm): the sheet under it is covered, so that a
// tap cannot reach it. An answer hides it, and the next question builds another: an object is not deleted from inside the
// event of a button it holds.
void open_confirm() {
    if (g.confirm != nullptr) {
        lv_obj_delete(g.confirm);
        g.confirm = nullptr;
    }
    const PrefsUi ui = prefs_ui();
    const iclforge::screen::Metrics& m = *ui.m;
    const iclforge::prefs::Pref* const pref = iclforge::prefs::find("speaker-layout");
    lv_obj_t* const cover = prefs_box(g.root);
    g.confirm = cover;
    lv_obj_set_size(cover, m.width, m.height);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(cover, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(cover, color(ui.c->bg), 0);

    const int panel_w = std::min(m.width - 2 * m.margin, 22 * m.text);
    const int inner_w = panel_w - 2 * m.margin;
    lv_obj_t* const panel = prefs_box(cover);
    lv_obj_set_width(panel, panel_w);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, color(ui.c->surface), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, color(ui.c->accent), 0);
    lv_obj_set_style_pad_all(panel, m.margin, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, m.gap, 0);

    const std::string question = std::string(pref != nullptr ? pref->confirm : "") + " Change to " + lb::pick_name(g.pick) + "?";
    prefs_paragraph(panel, question, ui.body, ui.c->text, inner_w);

    lv_obj_t* const buttons = prefs_box(panel);
    lv_obj_set_size(buttons, inner_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(buttons, m.gap, 0);
    lv_obj_set_style_pad_row(buttons, m.gap, 0);
    const auto width_of = [&](const std::string& text) {
        return std::max(m.row_target, prefs_text_width(ui.body, text) + 2 * m.pad);
    };
    prefs_button(buttons, "Change", width_of("Change"), m.row_target, ui.c->accent, ui.c->bg, &on_target,
                 pack(Op::kConfirm, 0, 0), nullptr);
    prefs_button(buttons, "Cancel", width_of("Cancel"), m.row_target, ui.c->surface_up, ui.c->text, &on_target,
                 pack(Op::kDecline, 0, 0), nullptr);
}

// Sets the layout the working pick makes. The sheet goes away when it was taken; a refusal stays on it, in the card.
void commit() {
    const iclforge::prefs::SetResult result = lb::apply_pick(preferences_backend(), g.pick);
    if (!result.ok) {
        say(result.message.empty() ? std::string("The board could not take that.") : result.message, "Not changed");
        return;
    }
    prefs_changed();
    g.want_close = true;
    if (g.root != nullptr) {
        lv_obj_add_flag(g.root, LV_OBJ_FLAG_HIDDEN);
    }
}

void apply() {
    const lb::PickVerdict verdict = lb::judge_pick(preferences_backend().caps(), g.pick);
    if (!verdict.accepted()) {
        say(verdict.reason, "Not available on this board");
        return;
    }
    if (is_in_force()) {
        say("That is the layout in use now.", "Not changed");
        return;
    }
    // A change that breaks the music that plays asks first.
    const iclforge::prefs::Pref* const pref = iclforge::prefs::find("speaker-layout");
    if (pref != nullptr && pref->confirm[0] != '\0' && preferences_backend().playing()) {
        open_confirm();
        return;
    }
    commit();
}

void on_target(lv_event_t* event) {
    const auto code = reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event));
    const auto op = static_cast<Op>(code & 0xFFU);
    const std::size_t a = (code >> 8) & 0xFFU;
    const std::size_t b = (code >> 16) & 0xFFU;
    switch (op) {
        case Op::kQuick:
            if (a < g.quick_states.size()) {
                // A greyed layout does not become the working one: the card says why, as it does for a greyed button.
                if (!g.quick_states[a].verdict.accepted()) {
                    say(g.quick_states[a].verdict.reason, "Not available on this board");
                    break;
                }
                g.pick = g.quick_states[a].pick;
                g.hint.clear();
                g.info.clear();
                g.dirty = true;
            }
            break;
        case Op::kStep: {
            if (a >= kFields.size()) {
                break;
            }
            const lb::PickField field = kFields[a];
            const std::vector<lb::PickStep> steps = lb::pick_steps(preferences_backend().caps(), g.pick, field);
            if (b < steps.size() && !steps[b].available) {
                say(steps[b].reason, field == lb::PickField::kMount ? "Nothing to mount" : "Not available on this board");
                break;
            }
            g.pick = lb::with_field(g.pick, field, step_value(field, b));
            g.hint.clear();
            g.info.clear();
            g.dirty = true;
            break;
        }
        case Op::kTabQuick:
            g.build_tab = false;
            g.dirty = true;
            break;
        case Op::kTabBuild:
            g.build_tab = true;
            g.dirty = true;
            break;
        case Op::kApply:
            apply();
            break;
        case Op::kCancel:
            g.want_close = true;
            if (g.root != nullptr) {
                lv_obj_add_flag(g.root, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        case Op::kConfirm:
            close_confirm();
            commit();
            break;
        case Op::kDecline:
            close_confirm();
            break;
    }
}

// What the board holds now: the layout in force, and the pick the sheet starts from.
void read_in_force(bool starting) {
    g.in_force_text = preferences_backend().layout();
    g.in_force = lb::pick_of(g.in_force_text);
    if (!starting) {
        return;
    }
    const std::optional<lb::LayoutPick> shown = g.in_force;
    g.pick = shown.value_or(lb::LayoutPick{});
    g.info = shown ? std::string()
                   : "This board is set to a speaker list (" + g.in_force_text +
                         "). Choosing a layout here replaces it.";
}

void refresh(std::uint32_t now) {
    g.refreshed_ms = now;
    g.dirty = false;
    // The layout changed under the sheet (a PUT /layout from the web page): "in use now" follows it.
    read_in_force(false);
    if (g.painted != paint_key()) {
        repaint();
    }
}

}  // namespace

void layout_sheet_open() { g.want_open = true; }

bool layout_sheet_active() { return (g.open && !g.want_close) || g.want_open; }

void layout_sheet_close() {
    g.want_open = false;
    if (g.open) {
        take_down();
    }
}

lv_obj_t* layout_sheet_root() { return g.open ? g.root : nullptr; }

void layout_sheet_update(std::uint32_t now) {
    g.now_ms = now;
    if (g.want_close && g.open) {
        take_down();
    }
    if (g.want_open) {
        g.want_open = false;
        if (!g.open) {
            g.hint.clear();
            g.build_tab = false;
            read_in_force(true);
            build_view();
            g.open = true;
            repaint();
            g.refreshed_ms = now;
            g.dirty = false;
        }
    }
    if (!g.open) {
        return;
    }
    if (g.dirty || now - g.refreshed_ms >= kRefreshMs) {
        refresh(now);
    }
}

}  // namespace player::panel
