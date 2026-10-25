// The speakers view and the title card. See now_screen.hpp and planning/p4x-now-playing.md.
//
// A speaker is three objects: a halo (the peak, a few steps of faint accent behind it), a core (the RMS,
// a few steps of accent, with the speaker's border) and its name inside. All of them are made when the
// room is built and left alone while it stands; a level changes a step and the step changes one object's
// opacity, which redraws that speaker's rectangle and nothing else.

#include "now_screen.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/render/room_plan.hpp"
#include "iclforge/wall_clock.hpp"

#include "fonts.hpp"
#include "lv_alloc.hpp"

namespace player::panel {

namespace {

namespace rp = iclforge::render;

// How a step shows: the opacity of the core's fill and of the halo, out of 255. The accent at its top
// step is 55 %: the lyric screen's own sung words are 70 %, and this is a thing for a dim room.
constexpr int kCoreOpaFloor = 28;
constexpr int kCoreOpaTop = 140;
constexpr int kHaloOpaStep = 14;
constexpr int kHaloOpaBase = 14;
// How far the halo stands out of the speaker, each side.
constexpr int kHaloReach = 12;
constexpr std::uint64_t kUpdateEveryUs = 200'000;

struct Node {
    lv_obj_t* halo = nullptr;
    lv_obj_t* core = nullptr;
    lv_obj_t* label = nullptr;
    std::size_t slot = 0;
    rp::Glow glow;
    // What is painted: -1 until the first update has painted anything.
    int core_step = -1;
    int halo_step = -1;
};

struct View {
    LvVector<Node> nodes;
    std::uint64_t updated_us = 0;
    NowStyle style;
};

View g;

// The clock view: three labels with a fixed size each, so that a change of text changes one rectangle and
// moves nothing. What they say is remembered as the broken-down time it was made from, and a reading that
// is the same to the minute is not looked at again.
constexpr int kClockSize = 200;
// The time is the muted colour at about 80 % and the date at about 60 %: the prototype's two steps, quiet in a
// dim room (planning/p4x-now-playing.md, option D).
constexpr lv_opa_t kClockTimeOpa = 200;
constexpr lv_opa_t kClockDateOpa = 150;
// How wide each of the three labels is: the widest time ("00:00" in the 200 px face is about 560 px) with room,
// and no wider, so that a minute's redraw is the width of the digits and not of the screen.
constexpr int kClockLabelWidth = 720;
// The small text beside the time (":59 PM"): its room, the space between it and the digits, and how far it stands above
// the digits' label's bottom edge, which is below their baseline by the face's descent.
constexpr int kClockSuffixWidth = 200;
constexpr int kClockSuffixGap = 14;
constexpr int kClockSuffixLift = 44;

struct Clock {
    lv_obj_t* time = nullptr;
    // What goes small beside the time (the seconds, AM or PM): hidden while the format has none.
    lv_obj_t* suffix = nullptr;
    lv_obj_t* date = nullptr;
    lv_obj_t* note = nullptr;
    // What the labels were set from: nothing at all (known false) until a reading with a time arrives.
    iclforge::wallclock::Reading shown;
    iclforge::wallclock::Face face;
    bool painted = false;
};

Clock ck;
// The 200 px face, made when the clock view is first built: it is the largest on the screen, and a board that
// never shows a clock never pays for it. Semibold Archivo only: the clock draws digits and a colon.
lv_font_t* clock_font = nullptr;

lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

lv_obj_t* make_shape(lv_obj_t* parent, const rp::PlanNode& node, int grow, int radius) {
    lv_obj_t* const object = lv_obj_create(parent);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_outline_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_size(object, node.w + grow, node.h + grow);
    lv_obj_set_pos(object, node.x - (node.w + grow) / 2, node.y - (node.h + grow) / 2);
    return object;
}

}  // namespace

bool now_build_speakers(lv_obj_t* parent, int width, int height, const NowPlaying& now, const NowStyle& style) {
    now_forget();
    if (now.spots.empty()) {
        return false;
    }
    g.style = style;
    const std::vector<rp::PlanNode> nodes =
        rp::plan_nodes(now.spots, rp::PlanArea{.width = width, .height = height});
    g.nodes.reserve(nodes.size());
    for (std::size_t slot = 0; slot < nodes.size(); ++slot) {
        const rp::PlanNode& plan = nodes[slot];
        if (plan.shape == rp::PlanShape::kNone) {
            continue;
        }
        const int radius = plan.shape == rp::PlanShape::kEar   ? LV_RADIUS_CIRCLE
                           : plan.shape == rp::PlanShape::kLfe ? 14
                                                               : 10;
        Node node;
        node.slot = slot;
        // The halo is a larger copy of the speaker's shape, behind it and invisible until the peak
        // asks for it.
        node.halo = make_shape(parent, plan, 2 * kHaloReach, plan.shape == rp::PlanShape::kEar ? LV_RADIUS_CIRCLE : radius + 8);
        lv_obj_set_style_bg_color(node.halo, color(style.accent), 0);
        node.core = make_shape(parent, plan, 0, radius);
        lv_obj_set_style_bg_color(node.core, color(style.accent), 0);
        lv_obj_set_style_border_width(node.core, 2, 0);
        lv_obj_set_style_border_color(node.core, color(style.faint), 0);
        // The name is inside the speaker: beside it, in a 7.1.4 room, it ran into its neighbours.
        node.label = lv_label_create(node.core);
        lv_label_set_text(node.label, now.spots[slot].name.c_str());
        lv_obj_set_style_text_font(node.label, style.label, 0);
        lv_obj_set_style_text_color(node.label, color(style.faint), 0);
        lv_label_set_long_mode(node.label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_width(node.label, LV_SIZE_CONTENT);
        lv_obj_center(node.label);
        g.nodes.push_back(node);
    }
    g.updated_us = 0;
    return !g.nodes.empty();
}

void now_update_speakers(const NowPlaying& now, std::uint64_t now_us) {
    if (g.nodes.empty()) {
        return;
    }
    if (g.updated_us != 0 && now_us - g.updated_us < kUpdateEveryUs) {
        return;
    }
    const float seconds = g.updated_us == 0 ? 0.2F : static_cast<float>(now_us - g.updated_us) / 1e6F;
    g.updated_us = now_us;
    for (Node& node : g.nodes) {
        const float rms = node.slot < now.rms_db.size() ? now.rms_db[node.slot] : -120.0F;
        const float peak = node.slot < now.peak_db.size() ? now.peak_db[node.slot] : -120.0F;
        const bool changed = node.glow.update(rms, peak, seconds);
        if (!changed && node.core_step >= 0) {
            continue;
        }
        const int core = node.glow.core();
        const int halo = node.glow.halo();
        if (core != node.core_step) {
            node.core_step = core;
            const bool live = node.glow.live();
            const int opa = live ? kCoreOpaFloor + (kCoreOpaTop - kCoreOpaFloor) * (core - 1) / (rp::kGlowCoreSteps - 1) : 0;
            lv_obj_set_style_bg_opa(node.core, static_cast<lv_opa_t>(opa), 0);
            lv_obj_set_style_border_color(node.core, color(live ? g.style.accent : g.style.faint), 0);
            lv_obj_set_style_border_opa(node.core, live ? 180 : LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(node.label, color(live ? g.style.text : g.style.faint), 0);
        }
        if (halo != node.halo_step) {
            node.halo_step = halo;
            lv_obj_set_style_bg_opa(node.halo, static_cast<lv_opa_t>(halo == 0 ? 0 : kHaloOpaBase + kHaloOpaStep * halo), 0);
        }
    }
}

namespace {

// How a line of the card sits against the others: the gap under the title and under the artist, pixels,
// and the opacity of the title and of the third line (the card is for a dim room: the title is not full
// white and the album is the muted tone a little back).
constexpr int kGapAfterTitle = 14;
constexpr int kGapAfterArtist = 8;
constexpr lv_opa_t kTitleOpa = 217;
constexpr lv_opa_t kDetailOpa = 204;

// A label of `text` set in `font`, `width` wide and at most `max_lines` rows: a text that needs more is
// cut with three dots, which LVGL does only for a label of fixed height.
lv_obj_t* make_card_text(lv_obj_t* parent, std::string_view text, const lv_font_t* font, std::uint32_t rgb,
                         lv_opa_t opa, int width, int max_lines) {
    lv_obj_t* const label = lv_label_create(parent);
    lv_label_set_text(label, std::string(text).c_str());
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color(rgb), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, width);
    lv_obj_update_layout(label);
    const int row = lv_font_get_line_height(font);
    if (lv_obj_get_height(label) > max_lines * row) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(label, max_lines * row);
    }
    return label;
}

}  // namespace

bool now_build_title(lv_obj_t* parent, int width, int height, std::string_view title, std::string_view artist,
                     std::string_view detail, const NowStyle& style) {
    now_forget();
    if (title.empty()) {
        return false;
    }
    const int content_w = width - 2 * style.margin;
    lv_obj_t* const title_label = make_card_text(parent, title, style.title, style.text, kTitleOpa, content_w, 2);
    lv_obj_t* const artist_label =
        artist.empty() ? nullptr : make_card_text(parent, artist, style.artist, style.muted, LV_OPA_COVER, content_w, 1);
    lv_obj_t* const detail_label =
        detail.empty() ? nullptr : make_card_text(parent, detail, style.label, style.muted, kDetailOpa, content_w, 1);
    // The block in the middle of the stage: its rows one under the other with a little air between.
    int block = lv_obj_get_height(title_label);
    if (artist_label != nullptr) {
        block += kGapAfterTitle + lv_obj_get_height(artist_label);
    }
    if (detail_label != nullptr) {
        block += (artist_label != nullptr ? kGapAfterArtist : kGapAfterTitle) + lv_obj_get_height(detail_label);
    }
    int y = std::max(0, (height - block) / 2);
    lv_obj_set_pos(title_label, style.margin, y);
    y += lv_obj_get_height(title_label);
    if (artist_label != nullptr) {
        y += kGapAfterTitle;
        lv_obj_set_pos(artist_label, style.margin, y);
        y += lv_obj_get_height(artist_label);
    }
    if (detail_label != nullptr) {
        y += artist_label != nullptr ? kGapAfterArtist : kGapAfterTitle;
        lv_obj_set_pos(detail_label, style.margin, y);
    }
    return true;
}

LvString now_title_detail(const iclforge::nowview::TitleStore& held) {
    LvString detail;
    detail.assign(held.album().data(), held.album().size());
    if (const auto year = held.year()) {
        char digits[16];
        std::snprintf(digits, sizeof digits, "%d", static_cast<int>(*year));
        if (!detail.empty()) {
            detail += " - ";
        }
        detail += digits;
    }
    return detail;
}

void now_copy_title(NowTitle& out, const iclforge::nowview::TitleStore* held, std::optional<std::int64_t> position_ms) {
    if (held == nullptr) {
        if (out.known) {
            out.known = false;
            out.title.clear();
            out.artist.clear();
            out.detail.clear();
            ++out.generation;
        }
        out.position_ms.reset();
        out.length_ms.reset();
        return;
    }
    if (!out.known || out.revision != held->revision()) {
        out.title.assign(held->title().data(), held->title().size());
        out.artist.assign(held->artist().data(), held->artist().size());
        out.detail = now_title_detail(*held);
        out.revision = held->revision();
        out.known = true;
        ++out.generation;
    }
    out.position_ms = position_ms;
    out.length_ms = held->length_ms();
}

bool now_build_clock(lv_obj_t* parent, int width, int height, const NowStyle& style) {
    now_forget();
    if (clock_font == nullptr) {
        const FontFile file = archivo_semibold();
        if (file.data == nullptr) {
            return false;
        }
        clock_font = lv_tiny_ttf_create_data(file.data, file.size, kClockSize);
        if (clock_font == nullptr) {
            return false;
        }
    }
    g.style = style;
    // One column, centred on the stage: the time, the date under it, and the note under that.
    lv_obj_t* const block = lv_obj_create(parent);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_outline_width(block, 0, 0);
    lv_obj_set_style_shadow_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_pad_row(block, 4, 0);
    lv_obj_set_size(block, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const int label_width = std::min(width, kClockLabelWidth);
    const auto make = [block, label_width](const lv_font_t* font, std::uint32_t rgb, lv_opa_t opa) {
        lv_obj_t* const label = lv_label_create(block);
        lv_label_set_text(label, "");
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, color(rgb), 0);
        lv_obj_set_style_text_opa(label, opa, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(label, label_width, lv_font_get_line_height(font));
        return label;
    };
    ck.time = make(clock_font, style.muted, kClockTimeOpa);
    // What is small beside the digits (the seconds, AM or PM) is a label of its own that the column does not lay out:
    // it floats at the right of the digits (placed in now_update_clock) and is hidden while there is nothing to say,
    // so that a clock with neither stays where it was, and a second that passes redraws this label alone.
    ck.suffix = make(style.date, style.muted, kClockTimeOpa);
    lv_obj_add_flag(ck.suffix, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(ck.suffix, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(ck.suffix, kClockSuffixWidth);
    lv_obj_set_style_text_align(ck.suffix, LV_TEXT_ALIGN_LEFT, 0);
    ck.date = make(style.date, style.muted, kClockDateOpa);
    ck.note = make(style.label, style.muted, LV_OPA_COVER);
    lv_obj_set_style_margin_top(ck.note, 28, 0);
    lv_obj_update_layout(block);
    // The note's room is kept whether or not it says anything, so that setting a zone moves nothing; the
    // block is put a little low of centre so that the time and the date, which are what is there with a zone,
    // sit where the prototype put them.
    lv_obj_align(block, LV_ALIGN_CENTER, 0, 28);
    (void)height;
    ck.shown = {};
    ck.face = {};
    ck.painted = false;
    return true;
}

void now_update_clock(const iclforge::wallclock::Reading& reading) {
    if (ck.time == nullptr) {
        return;
    }
    // The same to the minute (to the second when the seconds are shown), in the same zone and the same format:
    // nothing to look at. With no seconds shown this is the whole of a quiet minute.
    const iclforge::wallclock::LocalTime& at = reading.local;
    const iclforge::wallclock::LocalTime& was = ck.shown.local;
    if (ck.painted && reading.known == ck.shown.known && reading.zone_set == ck.shown.zone_set &&
        reading.format == ck.shown.format &&
        (!reading.known || (at.year == was.year && at.month == was.month && at.day == was.day &&
                            at.hour == was.hour && at.minute == was.minute &&
                            (!reading.format.seconds || at.second == was.second)))) {
        return;
    }
    ck.shown = reading;
    ck.painted = true;
    iclforge::wallclock::Face face = iclforge::wallclock::face(reading);
    // Only the label whose words changed is set, so only its rectangle is redrawn.
    if (face.time != ck.face.time) {
        lv_label_set_text(ck.time, face.time.c_str());
    }
    if (face.suffix != ck.face.suffix || (!face.suffix.empty() && face.time != ck.face.time)) {
        lv_label_set_text(ck.suffix, face.suffix.c_str());
        if (face.suffix.empty()) {
            lv_obj_add_flag(ck.suffix, LV_OBJ_FLAG_HIDDEN);
        } else {
            // At the right of the digits wherever their width puts that edge (the label is wider than they are, and
            // centres them), its bottom a little above the label's own.
            lv_point_t digits;
            lv_text_get_size(&digits, face.time.c_str(), clock_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lv_obj_align_to(ck.suffix, ck.time, LV_ALIGN_OUT_RIGHT_BOTTOM,
                            kClockSuffixGap - (lv_obj_get_width(ck.time) - digits.x) / 2, -kClockSuffixLift);
            lv_obj_remove_flag(ck.suffix, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (face.date != ck.face.date) {
        lv_label_set_text(ck.date, face.date.c_str());
    }
    if (face.note != ck.face.note) {
        lv_label_set_text(ck.note, face.note.c_str());
    }
    ck.face = std::move(face);
}

void now_forget() {
    g.nodes.clear();
    ck.time = nullptr;
    ck.suffix = nullptr;
    ck.date = nullptr;
    ck.note = nullptr;
    ck.painted = false;
}

std::uint32_t now_room_key(const NowPlaying& now) {
    // FNV-1a over what decides the picture's shape: each slot's name, its place and whether it is wired.
    std::uint32_t key = 2166136261U;
    const auto mix = [&key](std::uint32_t value) {
        key ^= value;
        key *= 16777619U;
    };
    for (const rp::PlanSpot& spot : now.spots) {
        for (const char c : spot.name) {
            mix(static_cast<unsigned char>(c));
        }
        mix(static_cast<std::uint32_t>(static_cast<std::int32_t>(spot.azimuth_deg * 10.0)));
        mix(static_cast<std::uint32_t>(static_cast<std::int32_t>(spot.elevation_deg * 10.0)));
        mix((spot.lfe ? 1U : 0U) | (spot.connected ? 2U : 0U));
        mix(0xFFU);
    }
    return key;
}

}  // namespace player::panel
