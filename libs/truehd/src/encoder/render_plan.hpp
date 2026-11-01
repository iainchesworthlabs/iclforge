#pragma once

#include <array>
#include <cstdint>
#include <vector>

// The small interface between the render side of the object encoder (render_bed.*, cluster_*) and
// the matrix side (object_bed.*): what the bed-mix matrices of substream 3 have to carry, as real
// numbers, before they are quantised to the wire. Plain data; no function lives here.
//
// The model (a guess, libs/truehd/notes/feature-truehd-atmos-encoder-render.md section 1): the
// stream's N elements are the output channels 0 to N-1 of substream 3. The 7.1 bed rows 0 to 7
// (L R C LFE Ls Rs Lrs Rrs) that substreams 0 to 2 carry are the speaker-anchored elements 0 to 7
// plus the free elements 8 to N-1 rendered onto the speakers:
//
//     bed[j] = element[j] + sum over k of gain[j][k] * element[8 + k]
//
// The decoder's matrices run that backwards: row j becomes bed[j] less the mix of the coded
// channels, and the coded channels 8 and up are the elements themselves. A mix gain is therefore a
// positive pan gain here and a negative coefficient on the wire.

namespace iclforge::truehd::detail {

inline constexpr int kRenderBedRows = 8;
inline constexpr int kRenderMaxElements = 16;
inline constexpr int kRenderMaxFree = kRenderMaxElements - kRenderBedRows;

// gain[bed row][free element k]: the share of element 8 + k that bed row `row` has.
using BedMixGains = std::array<std::array<double, kRenderMaxFree>, kRenderBedRows>;

// One stretch of samples over which every gain moves in a straight line from `begin` to `end`.
// `end` of a section equals `begin` of the next, so the trajectory has no step.
struct RenderSection {
    std::int64_t start = 0;    // the first sample of the section
    std::int64_t samples = 0;  // its length; the next section starts at start + samples
    BedMixGains begin{};       // the gains at `start`
    BedMixGains end{};         // the gains at `start + samples`
};

struct RenderPlan {
    int elements = 0;  // 12, 14 or 16
    std::vector<RenderSection> sections;
};

}  // namespace iclforge::truehd::detail
