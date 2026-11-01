#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "encoder/render_bed.hpp"
#include "encoder/render_plan.hpp"

// The clustering step of the object encoder: the master's objects (up to 82, with the bed's
// channels as objects pinned to their speakers) become the N elements the object substream
// carries, 12, 14 or 16 of them. Rule and evidence: libs/truehd/notes/
// feature-truehd-atmos-encoder-render.md. Everything here is a pure function of its arguments.
//
// The elements 0 to 7 are the speaker-anchored ones: element j sits at the wall position of bed row
// j (bed_speaker_placement) and takes the objects pinned to that row and the dynamic objects that
// sit within `snap_radius` of that speaker. The elements 8 to N-1 are free: they take the other
// dynamic objects, a cluster each when the objects are few enough and merged otherwise, and sit
// where their members are, weighted by the members' energy. The assignment is made once for each
// section of `section_samples`; an object that changes element is faded out of the old one and
// into the new over `handover_samples` centred on the boundary, so no element's signal steps.

namespace iclforge::truehd::detail {

// One authored moment of an object: where it is, how loud, from this sample on. Between two keys
// the position and the gain move in a straight line; before the first and after the last they
// hold, the way the object metadata's ramps do.
struct ClusterKey {
    std::int64_t sample = 0;
    double x = 0.5;
    double y = 0.5;
    double z = 0.0;
    double gain = 1.0;  // linear
};

struct ClusterObject {
    std::span<const std::int32_t> pcm;  // one value per sample, every object the same length
    std::vector<ClusterKey> keys;       // at least one, strictly increasing in `sample`
    // 0 to 7: a bed channel, pinned to the element of that bed row whatever `keys` say about its
    // place (the keys still give its gain). -1: a dynamic object.
    int bed_row = -1;
};

struct ClusterConfig {
    int elements = 12;                      // 12, 14 or 16
    std::int64_t section_samples = 480;     // a multiple of 40 (an access unit) is the usual choice
    std::int64_t handover_samples = 40;     // even, at most section_samples
    // A dynamic object within this distance of a speaker's wall position (room units; the height
    // counts half) is that speaker's. The distance an object has to leave by is this plus
    // `hysteresis`, so one that hovers at the edge does not flip each section.
    double snap_radius = 0.12;
    double hysteresis = 0.08;
    int bits = 24;  // the element signals are clipped to this many bits
};

enum class ClusterErrorKind : std::uint8_t {
    kInvalidConfig,
    kInvalidObject,
};

struct ClusterError {
    ClusterErrorKind kind = ClusterErrorKind::kInvalidConfig;
    std::string detail;
};

struct ClusterResult {
    int elements = 0;
    std::int64_t section_samples = 0;
    std::int64_t total_samples = 0;
    // The element signals, what substream 3 has to output: the objects' PCM times their gains,
    // summed into the element each belongs to. [element][sample].
    std::vector<std::vector<std::int32_t>> signals;
    // The element each object belongs to in each section. [section][object].
    std::vector<std::vector<int>> assignment;
    // Where each element is at each section boundary (boundary s is the first sample of section s,
    // and there is one more boundary than there are sections). The gain is 1.0: the objects' gains
    // are in the signals. An element with no object at a boundary, or next to it, holds the place
    // it had. [boundary][element].
    std::vector<std::vector<ElementPlacement>> placements;
    std::uint64_t clipped_samples = 0;
};

[[nodiscard]] std::expected<ClusterResult, ClusterError> cluster_objects(
    std::span<const ClusterObject> objects, const ClusterConfig& config);

// The bed-mix plan for a clustering: the free elements' placements at the boundaries, as
// build_render_plan takes them.
[[nodiscard]] RenderPlan make_render_plan(const ClusterResult& clustering,
                                          PanLaw law = PanLaw::kRing);

}  // namespace iclforge::truehd::detail
