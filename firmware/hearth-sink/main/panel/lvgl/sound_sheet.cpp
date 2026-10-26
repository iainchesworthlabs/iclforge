// The Speakers sheet. See sound_sheet.hpp.
//
// The room is libs/render's (room_plan.hpp: plan_spots and plan_nodes, read-only), the steps and the words are
// speaker_trim.hpp's (tested on the host), and the board's side - the tone, the trim, why it refuses - is main/sound.hpp's.
// This file measures the controls, draws them and carries a tap to those. The sheet is built when it is opened and again
// when the layout changes under it; a refresh five times a second repaints what moved (the lit speaker follows the tone)
// and nothing else, so a finger scrolling the page is never interrupted by a rebuild.

#include "sound_sheet.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"
#include "iclforge/render/speaker_trim.hpp"

#include "prefs_screen.hpp"
#include "sound.hpp"

namespace rp = iclforge::render;
namespace st = iclforge::render::speaker_trim;

namespace player::panel {

namespace {

constexpr std::uint32_t kRefreshMs = 200;
// How long a refusal stays up.
constexpr std::uint32_t kStatusShownMs = 6000;
// A step of a distance: a tenth of a metre, and a metre.
constexpr std::uint16_t kFineCm = 10;
constexpr std::uint16_t kCoarseCm = 100;
// Where a speaker with no distance starts when no other has one: three metres, a sofa's distance from a television.
constexpr std::uint16_t kSeedCm = 300;
// The room is drawn where the display is wide enough for it (the kit's 1024 and an 800 by 480); a narrower one has the
// speakers as a row of buttons.
constexpr int kRoomMinWidth = 760;
// The room's height: at least this, and taller by this much at a time until its speakers are apart, up to the most.
constexpr int kPlanMinHeight = 120;
constexpr int kPlanGrowStep = 10;
constexpr int kPlanMaxHeight = 520;

enum class Op : int {
    kNode = 0,
    kLevelMinus,
    kLevelPlus,
    kDistMinus,
    kDistPlus,
    kDistMinusCoarse,
    kDistPlusCoarse,
    kPlay,
    kRepeat,
    kEach,
    kStop,
    kReset,
    kBack,
};

// One speaker on the page: the object a tap on lands on (a shape in the room, or a button in the row), and whose it is.
struct Node {
    lv_obj_t* obj = nullptr;
    st::RoleCode role = st::kNoRole;
};

struct Sheet {
    bool want_open = false;
    bool want_close = false;
    bool open = false;
    bool dirty = true;

    lv_obj_t* root = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* name = nullptr;
    lv_obj_t* applied = nullptr;
    lv_obj_t* level_value = nullptr;
    lv_obj_t* dist_value = nullptr;
    lv_obj_t* notes = nullptr;
    lv_obj_t* level_minus = nullptr;
    lv_obj_t* level_plus = nullptr;
    lv_obj_t* dist_minus = nullptr;
    lv_obj_t* dist_plus = nullptr;
    lv_obj_t* dist_minus_coarse = nullptr;
    lv_obj_t* dist_plus_coarse = nullptr;
    lv_obj_t* play = nullptr;
    lv_obj_t* repeat = nullptr;
    lv_obj_t* each = nullptr;
    lv_obj_t* stop = nullptr;
    lv_obj_t* reset = nullptr;

    std::vector<Node> nodes;
    // What the board said at the last refresh.
    player::SoundSheet data;
    st::RoleCode selected = st::kNoRole;
    // The person asked for the tone to repeat on the chosen speaker: its button is the one that stops it.
    bool repeating = false;
    // The layout and the speakers it has at an output: a change rebuilds the sheet.
    std::string shape;
    // What was painted last: the sheet is repainted when this changes.
    std::string painted;

    std::string status_text;
    std::string status_shown;
    std::uint32_t status_ms = 0;
    std::uint32_t now_ms = 0;
    std::uint32_t refreshed_ms = 0;
};

Sheet g;

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

[[nodiscard]] std::uintptr_t pack(Op op, std::size_t index) {
    return (static_cast<std::uintptr_t>(index) << 8) | static_cast<std::uintptr_t>(static_cast<int>(op));
}

void on_target(lv_event_t* event);

[[nodiscard]] const player::SoundSheet::Speaker* speaker_of(st::RoleCode role) {
    for (const player::SoundSheet::Speaker& speaker : g.data.speakers) {
        if (speaker.role == role) {
            return &speaker;
        }
    }
    return nullptr;
}

// The layout and the roles at its outputs, as one string.
[[nodiscard]] std::string shape_of(const player::SoundSheet& data) {
    std::string key = data.layout;
    for (const player::SoundSheet::Speaker& speaker : data.speakers) {
        key += ',';
        key += std::string(st::role_name(speaker.role));
    }
    return key;
}

void say(const std::string& text) {
    g.status_text = text;
    g.status_ms = g.now_ms;
}

// --- what a tap does -------------------------------------------------------------------------------

// A request to the tone: it says in the bar why it was refused.
void tone(const std::string& request) {
    say(sound_identify_request(request));
    g.dirty = true;
}

void change(const std::string& statement) {
    say(sound_set_trim_text(statement));
    g.dirty = true;
}

[[nodiscard]] st::SpeakerTrim trim_of(st::RoleCode role) {
    const player::SoundSheet::Speaker* const speaker = speaker_of(role);
    return speaker != nullptr ? speaker->trim : st::SpeakerTrim{};
}

// A speaker with no distance starts at the farthest one that has one: it is not delayed, so setting it changes nothing
// until the person moves it.
[[nodiscard]] std::uint16_t seed_cm() { return g.data.farthest_cm != 0 ? g.data.farthest_cm : kSeedCm; }

void step_level(int direction) {
    const std::optional<std::int8_t> next = st::step_half_db(trim_of(g.selected).half_db, direction);
    if (next) {
        change(st::level_statement(g.selected, *next));
    }
}

void step_distance(int direction, std::uint16_t step) {
    const std::optional<std::uint16_t> next = st::step_distance_cm(trim_of(g.selected).cm, direction, step, seed_cm());
    if (next) {
        change(st::distance_statement(g.selected, *next));
    }
}

void on_target(lv_event_t* event) {
    const auto code = reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event));
    const auto op = static_cast<Op>(code & 0xFFU);
    const std::size_t index = (code >> 8) & 0xFFU;
    switch (op) {
        case Op::kNode:
            if (index < g.nodes.size()) {
                g.selected = g.nodes[index].role;
                g.repeating = false;
                tone(std::string(st::role_name(g.selected)));
            }
            break;
        case Op::kLevelMinus:
            step_level(-1);
            break;
        case Op::kLevelPlus:
            step_level(+1);
            break;
        case Op::kDistMinus:
            step_distance(-1, kFineCm);
            break;
        case Op::kDistPlus:
            step_distance(+1, kFineCm);
            break;
        case Op::kDistMinusCoarse:
            step_distance(-1, kCoarseCm);
            break;
        case Op::kDistPlusCoarse:
            step_distance(+1, kCoarseCm);
            break;
        case Op::kPlay:
            g.repeating = false;
            tone(std::string(st::role_name(g.selected)));
            break;
        case Op::kRepeat:
            if (g.repeating && g.data.tone_running) {
                g.repeating = false;
                tone("off");
            } else {
                g.repeating = true;
                tone(std::string(st::role_name(g.selected)) + " loop");
            }
            break;
        case Op::kEach:
            g.repeating = false;
            tone("all");
            break;
        case Op::kStop:
            g.repeating = false;
            tone("off");
            break;
        case Op::kReset:
            change(st::reset_statement(g.selected));
            break;
        case Op::kBack:
            g.want_close = true;
            if (g.root != nullptr) {
                lv_obj_add_flag(g.root, LV_OBJ_FLAG_HIDDEN);
            }
            break;
    }
}

// --- building --------------------------------------------------------------------------------------

[[nodiscard]] int button_width(const PrefsUi& ui, const std::string& text) {
    return std::max(ui.m->row_target, prefs_text_width(ui.body, text) + 2 * ui.m->pad);
}

// - value + : a group that stays together on a line.
lv_obj_t* make_stepper(const PrefsUi& ui, lv_obj_t* parent, int value_w, Op minus, Op plus, lv_obj_t*& minus_out,
                       lv_obj_t*& value_out, lv_obj_t*& plus_out) {
    lv_obj_t* const group = prefs_box(parent);
    lv_obj_set_size(group, 2 * ui.m->row_target + 2 * ui.m->gap + value_w, ui.m->row_target);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(group, ui.m->gap, 0);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    minus_out = prefs_button(group, "-", ui.m->row_target, ui.m->row_target, ui.c->surface_up, ui.c->text, &on_target,
                             pack(minus, 0), nullptr);
    value_out = prefs_label(group, "", ui.body, ui.c->text);
    lv_label_set_long_mode(value_out, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(value_out, value_w);
    lv_obj_set_style_text_align(value_out, LV_TEXT_ALIGN_CENTER, 0);
    plus_out = prefs_button(group, "+", ui.m->row_target, ui.m->row_target, ui.c->surface_up, ui.c->text, &on_target,
                            pack(plus, 0), nullptr);
    return group;
}

lv_obj_t* make_action(const PrefsUi& ui, lv_obj_t* parent, const std::string& text, Op op) {
    return prefs_button(parent, text, button_width(ui, text), ui.m->row_target, ui.c->surface_up, ui.c->text, &on_target,
                        pack(op, 0), nullptr);
}

// The two buttons that step a distance a metre: as wide as their text and a little room, never under a fingertip.
[[nodiscard]] int step_button_width(const PrefsUi& ui, const std::string& text) {
    return std::max(ui.m->row_target, prefs_text_width(ui.body, text) + ui.m->gap);
}

lv_obj_t* make_step_button(const PrefsUi& ui, lv_obj_t* parent, const std::string& text, Op op) {
    return prefs_button(parent, text, step_button_width(ui, text), ui.m->row_target, ui.c->surface_up, ui.c->text, &on_target,
                        pack(op, 0), nullptr);
}

// A speaker's shape: clickable, its name inside it (child 0, as every button's label is).
lv_obj_t* make_node(const PrefsUi& ui, lv_obj_t* parent, const std::string& name, int width, int height, int radius,
                    std::size_t index) {
    lv_obj_t* const node = prefs_button(parent, name, width, height, ui.c->surface_up, ui.c->text, &on_target,
                                        pack(Op::kNode, index), nullptr);
    lv_obj_set_style_radius(node, radius, 0);
    return node;
}

// Whether no two of the shapes are closer than a fingertip's gap (iclforge::screen::kMinGapPx), the LFE's among them.
[[nodiscard]] bool apart(const std::vector<rp::PlanNode>& places) {
    for (std::size_t a = 0; a + 1 < places.size(); ++a) {
        for (std::size_t b = a + 1; b < places.size(); ++b) {
            const rp::PlanNode& first = places[a];
            const rp::PlanNode& second = places[b];
            if (first.shape == rp::PlanShape::kNone || second.shape == rp::PlanShape::kNone) {
                continue;
            }
            const int across = std::abs(first.x - second.x) - (first.w + second.w) / 2;
            const int down = std::abs(first.y - second.y) - (first.h + second.h) / 2;
            if (across < iclforge::screen::kMinGapPx && down < iclforge::screen::kMinGapPx) {
                return false;
            }
        }
    }
    return true;
}

// The room as the layout draws it, the speakers that go to an output in the places they have, each with its ID inside it.
// False when the layout cannot be read back from its text, and the speakers are a row of buttons instead.
bool build_room(const PrefsUi& ui, lv_obj_t* parent, int content_w) {
    const std::optional<rp::OutputLayout> layout = rp::OutputLayout::parse(g.data.layout);
    if (!layout) {
        return false;
    }
    const std::vector<rp::PlanSpot> spots = rp::plan_spots(*layout);
    // As tall as the room needs for its speakers to be a fingertip apart, and no taller: a 2.0 room is short, a 9.1.6 room
    // is 260 px at the kit's width.
    int plan_h = kPlanMinHeight;
    std::vector<rp::PlanNode> places = rp::plan_nodes(spots, rp::PlanArea{.width = content_w, .height = plan_h});
    while (plan_h < kPlanMaxHeight && !apart(places)) {
        plan_h += kPlanGrowStep;
        places = rp::plan_nodes(spots, rp::PlanArea{.width = content_w, .height = plan_h});
    }
    lv_obj_t* const plan = prefs_box(parent);
    lv_obj_set_size(plan, content_w, plan_h);
    for (std::size_t slot = 0; slot < places.size(); ++slot) {
        const rp::PlanNode& place = places[slot];
        const st::RoleCode role = st::role_of_slot(*layout, slot);
        if (place.shape == rp::PlanShape::kNone || speaker_of(role) == nullptr) {
            continue;
        }
        const int radius = place.shape == rp::PlanShape::kEar ? LV_RADIUS_CIRCLE : place.shape == rp::PlanShape::kLfe ? 14 : 10;
        lv_obj_t* const node = make_node(ui, plan, std::string(st::role_name(role)), place.w, place.h, radius, g.nodes.size());
        lv_obj_set_pos(node, place.x - place.w / 2, place.y - place.h / 2);
        g.nodes.push_back(Node{node, role});
    }
    return !g.nodes.empty();
}

// The speakers as a row of buttons, in output order.
void build_chips(const PrefsUi& ui, lv_obj_t* parent, int content_w) {
    lv_obj_t* const row = prefs_box(parent);
    lv_obj_set_size(row, content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, ui.m->gap, 0);
    lv_obj_set_style_pad_row(row, ui.m->gap, 0);
    for (const player::SoundSheet::Speaker& speaker : g.data.speakers) {
        const std::string name(st::role_name(speaker.role));
        lv_obj_t* const chip = make_node(ui, row, name, button_width(ui, name), ui.m->row_target, 8, g.nodes.size());
        g.nodes.push_back(Node{chip, speaker.role});
    }
}

// A label that is as wide as its text and no wider.
lv_obj_t* make_caption(lv_obj_t* parent, const std::string& text, const lv_font_t* font, std::uint32_t rgb) {
    lv_obj_t* const label = prefs_label(parent, text, font, rgb);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    return label;
}

// A row of a group: a caption, then what it labels, on one line where the group's width allows and wrapped inside it where
// it does not. `width` is the group's, set outright so that the groups of a row wrap against one another.
lv_obj_t* make_group(const PrefsUi& ui, lv_obj_t* parent, int width) {
    lv_obj_t* const group = prefs_box(parent);
    lv_obj_set_size(group, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(group, ui.m->gap, 0);
    lv_obj_set_style_pad_row(group, ui.m->gap, 0);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return group;
}

// The chosen speaker: what it is and what is in force at it, why the board said no, the level and the distance (on one
// line where the display is wide enough for both, one under the other where it is not), and why a control does nothing.
void build_panel(const PrefsUi& ui, lv_obj_t* parent, int content_w) {
    const iclforge::screen::Metrics& m = *ui.m;
    const int gap = m.gap;
    lv_obj_t* const panel = prefs_box(parent);
    lv_obj_set_size(panel, content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, gap, 0);

    lv_obj_t* const who = prefs_box(panel);
    lv_obj_set_size(who, content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(who, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(who, 2 * gap, 0);
    lv_obj_set_flex_align(who, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    g.name = make_caption(who, "", ui.heading, ui.c->text);
    g.applied = make_caption(who, "", ui.note, ui.c->muted);
    // Why the board refused the last thing asked of it, in the accent: a few lines, wrapped, for six seconds.
    g.status = prefs_paragraph(panel, "", ui.note, ui.c->accent, content_w);
    lv_obj_add_flag(g.status, LV_OBJ_FLAG_HIDDEN);

    const int level_w = std::max(prefs_text_width(ui.body, "+11.5 dB"), prefs_text_width(ui.body, "-12 dB"));
    const int dist_w = std::max(prefs_text_width(ui.body, "9.95 m"), prefs_text_width(ui.body, "not set"));
    const std::string coarse_less = "-1 m";
    const std::string coarse_more = "+1 m";
    const int stepper_level_w = 2 * m.row_target + 2 * gap + level_w;
    const int stepper_dist_w = 2 * m.row_target + 2 * gap + dist_w;
    const int coarse_w = step_button_width(ui, coarse_less) + gap + step_button_width(ui, coarse_more);
    const int level_label_w = prefs_text_width(ui.body, "Level") + 2;
    const int dist_label_w = prefs_text_width(ui.body, "Distance") + 2;
    const int level_group_w = std::min(content_w, level_label_w + gap + stepper_level_w);
    const int dist_group_w = std::min(content_w, dist_label_w + gap + stepper_dist_w + gap + coarse_w);

    lv_obj_t* const steppers = prefs_box(panel);
    lv_obj_set_size(steppers, content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(steppers, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(steppers, 2 * gap, 0);
    lv_obj_set_style_pad_row(steppers, gap, 0);

    lv_obj_t* const level = make_group(ui, steppers, level_group_w);
    lv_obj_t* const level_caption = prefs_label(level, "Level", ui.body, ui.c->text);
    lv_obj_set_width(level_caption, level_label_w);
    lv_label_set_long_mode(level_caption, LV_LABEL_LONG_MODE_CLIP);
    make_stepper(ui, level, level_w, Op::kLevelMinus, Op::kLevelPlus, g.level_minus, g.level_value, g.level_plus);

    lv_obj_t* const distance = make_group(ui, steppers, dist_group_w);
    lv_obj_t* const distance_caption = prefs_label(distance, "Distance", ui.body, ui.c->text);
    lv_obj_set_width(distance_caption, dist_label_w);
    lv_label_set_long_mode(distance_caption, LV_LABEL_LONG_MODE_CLIP);
    make_stepper(ui, distance, dist_w, Op::kDistMinus, Op::kDistPlus, g.dist_minus, g.dist_value, g.dist_plus);
    lv_obj_t* const coarse = prefs_box(distance);
    lv_obj_set_size(coarse, coarse_w, m.row_target);
    lv_obj_set_flex_flow(coarse, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(coarse, gap, 0);
    lv_obj_set_flex_align(coarse, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g.dist_minus_coarse = make_step_button(ui, coarse, coarse_less, Op::kDistMinusCoarse);
    g.dist_plus_coarse = make_step_button(ui, coarse, coarse_more, Op::kDistPlusCoarse);

    g.notes = prefs_paragraph(panel, "", ui.note, ui.c->muted, content_w);
}

// What the bar at the foot of the sheet says on its five buttons, and how tall it is. The words are the long ones where
// they fit on one line and the short ones where only those do; where neither does the buttons wrap onto a second line (a
// 240 by 240 module), and the bar is as tall as that needs, so that it is always the whole of it that is on the glass.
struct BarPlan {
    std::array<const char*, 5> labels{"Play", "Repeat", "Play each", "Stop", "Reset"};
    int rows = 1;
    int height = 0;
};

[[nodiscard]] int rows_of(const PrefsUi& ui, const std::array<const char*, 5>& labels, int width) {
    int rows = 1;
    int used = 0;
    for (const char* const label : labels) {
        const int w = button_width(ui, label);
        if (used != 0 && used + ui.m->gap + w > width) {
            ++rows;
            used = 0;
        }
        used += (used != 0 ? ui.m->gap : 0) + w;
    }
    return rows;
}

[[nodiscard]] BarPlan plan_bar(const PrefsUi& ui, int content_w) {
    BarPlan plan;
    const std::array<const char*, 5> shorter{"Play", "Loop", "All", "Stop", "Reset"};
    plan.rows = rows_of(ui, plan.labels, content_w);
    if (plan.rows > 1 && rows_of(ui, shorter, content_w) < plan.rows) {
        plan.labels = shorter;
        plan.rows = rows_of(ui, shorter, content_w);
    }
    plan.height = plan.rows * ui.m->row_target + (plan.rows - 1) * ui.m->gap + 2 * ui.m->gap;
    return plan;
}

void build_view() {
    const PrefsUi ui = prefs_ui();
    const iclforge::screen::Metrics& m = *ui.m;
    const int content_w = m.width - 2 * m.margin;
    const BarPlan bar_plan = plan_bar(ui, content_w);
    // What is between the two bars, which are fixed: the room and the chosen speaker, scrolling where they are more than it.
    const int body_h = m.height - m.bar - bar_plan.height;

    g.nodes.clear();
    lv_obj_t* const root = prefs_box(lv_screen_active());
    g.root = root;
    lv_obj_set_size(root, m.width, m.height);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, color(ui.c->bg), 0);
    // A tap on the sheet is the sheet's: it never reaches the one under it.
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);

    // The bar at the top: the title and Back. A refusal is said in the page, where it can run to the lines it needs.
    lv_obj_t* const bar = prefs_box(root);
    lv_obj_set_size(bar, m.width, m.bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, color(ui.c->faint), 0);
    lv_obj_t* const title = prefs_label(bar, "Speakers", ui.title, ui.c->text);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(title, m.margin, (m.bar - lv_font_get_line_height(ui.title)) / 2);
    const int back_w = std::max(m.target, prefs_text_width(ui.body, "Back") + 2 * m.pad);
    const int back_x = m.width - m.margin - back_w;
    lv_obj_t* const back =
        prefs_button(bar, "Back", back_w, m.bar_button, ui.c->accent, ui.c->bg, &on_target, pack(Op::kBack, 0), nullptr);
    lv_obj_set_pos(back, back_x, (m.bar - m.bar_button) / 2);
    g.status_shown.clear();

    // The page between the bars: scrolls up and down where the room and the speaker are more than it.
    lv_obj_t* const list = prefs_box(root);
    lv_obj_set_size(list, m.width, body_h);
    lv_obj_set_pos(list, 0, m.bar);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, color(ui.c->muted), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(list, m.margin, 0);
    lv_obj_set_style_pad_right(list, m.margin, 0);
    lv_obj_set_style_pad_top(list, m.gap, 0);
    lv_obj_set_style_pad_bottom(list, m.gap, 0);

    // The bar at the foot, fixed: the five buttons are on the glass whatever the room is, and Stop is there to be pressed
    // while the tone is on.
    lv_obj_t* const foot = prefs_box(root);
    lv_obj_set_size(foot, m.width, bar_plan.height);
    lv_obj_set_pos(foot, 0, m.height - bar_plan.height);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(foot, color(ui.c->surface), 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(foot, 2, 0);
    lv_obj_set_style_border_color(foot, color(ui.c->faint), 0);
    lv_obj_set_style_pad_left(foot, m.margin, 0);
    lv_obj_set_style_pad_right(foot, m.margin, 0);
    lv_obj_set_style_pad_top(foot, m.gap - 2, 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(foot, m.gap, 0);
    lv_obj_set_style_pad_row(foot, m.gap, 0);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    g.play = make_action(ui, foot, bar_plan.labels[0], Op::kPlay);
    g.repeat = make_action(ui, foot, bar_plan.labels[1], Op::kRepeat);
    g.each = make_action(ui, foot, bar_plan.labels[2], Op::kEach);
    g.stop = make_action(ui, foot, bar_plan.labels[3], Op::kStop);
    g.reset = make_action(ui, foot, bar_plan.labels[4], Op::kReset);

    if (!g.data.player) {
        // A board with no player is not offered the row; a sheet that was open when the answer changed says why.
        prefs_paragraph(list, g.data.identify_reason, ui.body, ui.c->muted, content_w);
        return;
    }
    if (g.data.speakers.empty()) {
        prefs_paragraph(list, "The layout in force has no speaker at an output.", ui.body, ui.c->muted, content_w);
        return;
    }
    if (m.width < kRoomMinWidth || !build_room(ui, list, content_w)) {
        g.nodes.clear();
        build_chips(ui, list, content_w);
    }
    build_panel(ui, list, content_w);
}

void take_down() {
    // The tone is the sheet's to stop: nothing plays on after the page that asked for it has gone.
    if (sound_identify_running()) {
        static_cast<void>(sound_identify_request("off"));
    }
    if (g.root != nullptr) {
        lv_obj_delete(g.root);
    }
    g.root = nullptr;
    g.status = nullptr;
    g.name = nullptr;
    g.applied = nullptr;
    g.level_value = nullptr;
    g.dist_value = nullptr;
    g.notes = nullptr;
    g.level_minus = g.level_plus = g.dist_minus = g.dist_plus = nullptr;
    g.dist_minus_coarse = g.dist_plus_coarse = nullptr;
    g.play = g.repeat = g.each = g.stop = g.reset = nullptr;
    g.nodes.clear();
    g.repeating = false;
    g.open = false;
    g.want_close = false;
    prefs_changed();
}

// --- painting --------------------------------------------------------------------------------------

void style_node(const PrefsUi& ui, const Node& node, bool selected, bool lit) {
    lv_obj_t* const label = lv_obj_get_child(node.obj, 0);
    lv_obj_set_style_bg_color(node.obj, color(lit ? ui.c->accent : ui.c->surface_up), 0);
    lv_obj_set_style_border_width(node.obj, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(node.obj, color(selected ? ui.c->accent : ui.c->faint), 0);
    lv_obj_set_style_text_color(label, color(lit ? ui.c->bg : ui.c->text), 0);
}

[[nodiscard]] std::string applied_text(const player::SoundSheet::Speaker& speaker) {
    std::array<char, 24> milliseconds{};
    const double ms = static_cast<double>(speaker.applied_delay_samples) * 1000.0 / static_cast<double>(player::kSinkSampleRate);
    std::snprintf(milliseconds.data(), milliseconds.size(), "%.1f", ms);
    std::string text = "In force: " + st::level_text(static_cast<std::int8_t>(std::lround(speaker.applied_level_db * 2.0F)));
    text += speaker.applied_delay_samples == 0 ? ", not delayed" : std::string(", ") + milliseconds.data() + " ms later";
    return text;
}

// The lines under the controls: why a control is greyed, and what stands in the way of the tone.
[[nodiscard]] std::string notes_text(const player::SoundSheet::Speaker& speaker) {
    std::vector<std::string> lines;
    if (!g.data.tone_blocked.empty()) {
        lines.push_back(g.data.tone_blocked);
    }
    if (!g.data.delay) {
        lines.push_back("Distance: " + g.data.delay_reason);
    } else if (speaker.trim.cm == 0) {
        lines.emplace_back("A speaker with no distance counts as the farthest, and is not delayed.");
    }
    if (g.data.server_level || g.data.server_delay) {
        lines.emplace_back("A Sendspin server is setting the levels or the delays now; yours apply again when it stops.");
    }
    if (speaker.delay_clamped) {
        lines.emplace_back("This speaker's delay is cut to what the delay lines hold.");
    }
    std::string text;
    for (const std::string& line : lines) {
        text += text.empty() ? line : "\n" + line;
    }
    return text;
}

void paint() {
    const PrefsUi ui = prefs_ui();
    if (g.root == nullptr) {
        return;
    }
    if (speaker_of(g.selected) == nullptr && !g.data.speakers.empty()) {
        g.selected = g.data.speakers.front().role;
    }
    const player::SoundSheet::Speaker* const speaker = speaker_of(g.selected);
    if (speaker == nullptr || g.name == nullptr) {
        return;
    }

    std::string key = std::string(st::role_name(g.selected)) + "|" + std::to_string(g.data.tone_role) + "|" +
                      (g.data.tone_running ? "1" : "0") + (g.repeating ? "1" : "0") + (g.data.delay ? "1" : "0") + "|" +
                      std::to_string(speaker->trim.half_db) + "|" + std::to_string(speaker->trim.cm) + "|" +
                      std::to_string(speaker->applied_delay_samples) + "|" + std::to_string(std::lround(speaker->applied_level_db * 2.0F)) +
                      "|" + g.data.tone_blocked + (speaker->delay_clamped ? "c" : "-") + (g.data.server_level ? "l" : "-") +
                      (g.data.server_delay ? "d" : "-");
    if (key == g.painted) {
        return;
    }
    g.painted = key;

    for (const Node& node : g.nodes) {
        style_node(ui, node, node.role == g.selected, g.data.tone_running && node.role == g.data.tone_role);
    }
    const std::string label(st::role_label(g.selected));
    lv_label_set_text(g.name, (label + "  " + std::string(st::role_name(g.selected))).c_str());
    lv_label_set_text(g.applied, applied_text(*speaker).c_str());
    lv_label_set_text(g.level_value, st::level_text(speaker->trim.half_db).c_str());
    lv_label_set_text(g.dist_value, st::distance_text(speaker->trim.cm).c_str());

    const auto can = [](const std::optional<std::int8_t>& step) { return step.has_value(); };
    prefs_style_button(g.level_minus, false, can(st::step_half_db(speaker->trim.half_db, -1)));
    prefs_style_button(g.level_plus, false, can(st::step_half_db(speaker->trim.half_db, +1)));
    const bool distance = g.data.delay;
    prefs_style_button(g.dist_minus, false, distance && st::step_distance_cm(speaker->trim.cm, -1, kFineCm, seed_cm()));
    prefs_style_button(g.dist_plus, false, distance && st::step_distance_cm(speaker->trim.cm, +1, kFineCm, seed_cm()));
    prefs_style_button(g.dist_minus_coarse, false, distance && st::step_distance_cm(speaker->trim.cm, -1, kCoarseCm, seed_cm()));
    prefs_style_button(g.dist_plus_coarse, false, distance && st::step_distance_cm(speaker->trim.cm, +1, kCoarseCm, seed_cm()));

    const bool can_play = g.data.tone_blocked.empty();
    const bool repeating = g.repeating && g.data.tone_running;
    prefs_style_button(g.play, false, can_play);
    prefs_style_button(g.repeat, repeating, can_play || repeating);
    prefs_style_button(g.each, false, can_play);
    prefs_style_button(g.stop, false, g.data.tone_running);
    prefs_style_button(g.reset, false, !speaker->trim.neutral());

    const std::string notes = notes_text(*speaker);
    lv_label_set_text(g.notes, notes.c_str());
    if (notes.empty()) {
        lv_obj_add_flag(g.notes, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(g.notes, LV_OBJ_FLAG_HIDDEN);
    }
}

void refresh(std::uint32_t now) {
    g.refreshed_ms = now;
    g.dirty = false;
    g.data = sound_sheet();
    // The tone ended by itself (a limit, a stream starting): the repeat button is not left pressed.
    if (!g.data.tone_running) {
        g.repeating = false;
    }
    const std::string shape = shape_of(g.data);
    if (shape != g.shape && g.root != nullptr) {
        // The layout changed under the sheet (a PUT /layout from the web page): drawn again for the room it is now.
        g.shape = shape;
        const st::RoleCode selected = g.selected;
        lv_obj_delete(g.root);
        g.root = nullptr;
        g.nodes.clear();
        g.painted.clear();
        build_view();
        g.selected = selected;
    }
    paint();
    if (!g.status_text.empty() && now - g.status_ms > kStatusShownMs) {
        g.status_text.clear();
    }
    if (g.status != nullptr && g.status_text != g.status_shown) {
        g.status_shown = g.status_text;
        lv_label_set_text(g.status, g.status_text.c_str());
        if (g.status_text.empty()) {
            lv_obj_add_flag(g.status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(g.status, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

}  // namespace

void sound_sheet_open() { g.want_open = true; }

bool sound_sheet_serves(const iclforge::prefs::Pref& pref) {
    const std::string_view id = pref.id;
    return id == "identify-speakers" || id == "speaker-trim";
}

bool sound_sheet_active() { return (g.open && !g.want_close) || g.want_open; }

void sound_sheet_close() {
    g.want_open = false;
    if (g.open) {
        take_down();
    } else if (sound_identify_running()) {
        // The Identify speakers button of the list started it, and the list is going: nothing plays on.
        static_cast<void>(sound_identify_request("off"));
    }
}

lv_obj_t* sound_sheet_root() { return g.open ? g.root : nullptr; }

void sound_sheet_update(std::uint32_t now) {
    g.now_ms = now;
    if (g.want_close && g.open) {
        take_down();
    }
    if (g.want_open) {
        g.want_open = false;
        if (!g.open) {
            g.data = sound_sheet();
            g.shape = shape_of(g.data);
            g.selected = g.data.speakers.empty() ? st::kNoRole : g.data.speakers.front().role;
            g.painted.clear();
            g.status_text.clear();
            build_view();
            g.open = true;
            g.dirty = true;
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
