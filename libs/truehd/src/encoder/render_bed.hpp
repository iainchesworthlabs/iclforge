#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "encoder/render_plan.hpp"
#include "iclforge/truehd/core/syntax.hpp"

// The render rule of the object encoder: where an element sits, how much of it each row of the 7.1
// bed gets, and the matrices that carry those shares (libs/truehd/notes/
// feature-truehd-atmos-encoder-render.md). Everything here is a pure function of its arguments.
//
// An element's place is a room position as the object metadata has it (TS 103 420 §4.2.1: x 0 at
// the left wall to 1 at the right, y 0 at the front wall to 1 at the back, z -1 at the floor to +1
// at the ceiling). The legacy 7.1 bed has no height, so a high element is panned by its azimuth
// alone at full level, the rule libs/render states for a target without an upper layer.

namespace iclforge::truehd::detail {

// What the metadata says about an element for a stretch of time: the place the render and the
// object metadata writer share, and the gain the element's signal already carries (1.0 when the
// objects' gains are mixed into the signal, as cluster_elements does).
struct ElementPlacement {
    double x = 0.5;
    double y = 0.5;
    double z = 0.0;
    double gain = 1.0;
};

// The bed rows in stream order, and the LFE's, which no panning reaches.
inline constexpr int kBedLfeRow = 3;

// Where the speaker of a bed row sits on the room's wall: the place whose azimuth is the speaker's
// own (30 degrees for L, 0 for C, 90 for Ls, 150 for Lrs, mirrored on the right), so that
// pan_to_bed of it puts the whole signal on that row. Row 3 (the LFE) is at the centre of the
// front wall, which has no direction of its own.
[[nodiscard]] ElementPlacement bed_speaker_placement(int row) noexcept;

// The share of the 8 bed rows an element at (x, y, z) has: constant-power pairwise panning on the
// 7.1 ring (L 30, C 0, R -30, Ls 90, Rs -90, Lrs 150, Rrs -150 degrees, counter-clockwise from the
// front), height ignored. The squares of the shares add up to 1; the LFE row is 0. A place at the
// exact centre of the room has no direction and is rendered at the front.
[[nodiscard]] std::array<double, kRenderBedRows> pan_to_bed(double x, double y,
                                                            double z = 0.0) noexcept;

// The gains of the free elements (8 and up) into the bed rows, for elements placed as given:
// gain[row][k] = pan_to_bed(placement k)[row] times the placement's gain. Elements beyond
// `free_elements.size()` get 0.
[[nodiscard]] BedMixGains mix_gains(std::span<const ElementPlacement> free_elements) noexcept;

// The plan for elements placed at the boundaries of sections: boundary s is the first sample of
// section s, so there is one more boundary than there are sections, and `placements[s][k]` is the
// place of the free element 8 + k there. A section's gains run in a straight line from the boundary
// that opens it to the one that closes it. `section_samples` is the length of every section but
// the last, which ends at `total_samples`.
[[nodiscard]] RenderPlan build_render_plan(
    int elements, std::int64_t section_samples, std::int64_t total_samples,
    std::span<const std::vector<ElementPlacement>> placements);

// The gains at one sample of the stream: the plan's trajectory in a straight line from the
// boundary before the sample to the one after it. Held at the first and last boundary outside.
[[nodiscard]] BedMixGains gains_at(const RenderPlan& plan, std::int64_t sample) noexcept;

// A mix gain as the wire carries a coefficient: gain * 2^frac_bits rounded to nearest, in the
// range of a signed coefficient of frac_bits + 2 bits. Gains are 0 to 1, so only a gain a little
// above 1 can clip.
[[nodiscard]] std::int32_t quantise_mix_gain(double gain, int frac_bits) noexcept;

// The matrices of the 8 bed rows for one set of gains, in row order: bed row j becomes the row it
// holds less the share of each coded element (own coefficient +1.0, -gain on row 8 + k, frac_bits
// 12 so that +1.0 is 4096). A row whose shares all quantise to 0 has no matrix. `noise_shift` is
// the matrices' (0 draws none). The flip of row 8 and the LSB bypass of the owner's streams are not
// here: they belong to the chain the matrix side builds (object_bed.hpp), which puts these after
// them.
[[nodiscard]] std::vector<MatrixSpec> bed_mix_matrices(const BedMixGains& gains, int elements,
                                                       int noise_shift);

}  // namespace iclforge::truehd::detail
