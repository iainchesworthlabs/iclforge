// The speakers view. See now_screen.hpp and planning/p4x-now-playing.md.
//
// A speaker is three objects: a halo (the peak, a few steps of faint accent behind it), a core (the RMS,
// a few steps of accent, with the speaker's border) and its name inside. All of them are made when the
// room is built and left alone while it stands; a level changes a step and the step changes one object's
// opacity, which redraws that speaker's rectangle and nothing else.

#include "now_screen.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "iclforge/render/room_plan.hpp"

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

void now_forget() { g.nodes.clear(); }

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
