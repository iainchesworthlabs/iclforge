#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "encoder/object_bed.hpp"
#include "encoder/render_bed.hpp"
#include "iclforge/render/spatial.hpp"

// The render rule of the object encoder (libs/truehd/notes/feature-truehd-atmos-encoder-render.md):
// where an element sits on the 7.1 bed, the straight-line trajectory of its gains between section
// boundaries, and the matrices that carry them. [no-external-oracle] for the matrices: the panner
// is held to libs/render's here, the matrices to the timeline functions of object_bed.hpp (the
// decoder's arithmetic) and to the definition written out in the test.

namespace {

using namespace iclforge::truehd::detail;
using iclforge::truehd::MatrixSpec;
using Catch::Approx;

// 7.1 bed rows and the ring azimuths libs/render pans onto: L R C Ls Rs Lrs Rrs (the LFE has none).
constexpr std::array<int, 7> kRingRows{0, 1, 2, 4, 5, 6, 7};
constexpr std::array<double, 7> kRingAzimuths{30.0, -30.0, 0.0, 90.0, -90.0, 150.0, -150.0};
constexpr auto kBedRows = static_cast<std::size_t>(kRenderBedRows);

}  // namespace

TEST_CASE("the bed panner agrees with libs/render's ring panner over the whole room",
          "[truehd][encoder][atmos][render]") {
    int compared = 0;
    for (int ix = 0; ix <= 20; ++ix) {
        for (int iy = 0; iy <= 20; ++iy) {
            const double x = ix / 20.0;
            const double y = iy / 20.0;
            if (ix == 10 && iy == 10) {
                continue;  // the centre has no direction
            }
            const double azimuth =
                std::atan2(0.5 - x, 0.5 - y) * 180.0 / 3.14159265358979323846;
            std::array<double, 7> reference{};
            iclforge::spatial::pan_ring(azimuth, kRingAzimuths, reference);
            const auto ours = pan_to_bed(x, y, 0.0);
            for (std::size_t r = 0; r < kRingRows.size(); ++r) {
                CAPTURE(x, y, r);
                CHECK(ours[static_cast<std::size_t>(kRingRows[r])] ==
                      Approx(reference[r]).margin(1e-9));
            }
            ++compared;
        }
    }
    CHECK(compared == 440);
}

TEST_CASE("the bed panner keeps power, leaves the LFE alone and ignores height",
          "[truehd][encoder][atmos][render]") {
    for (int ix = 0; ix <= 10; ++ix) {
        for (int iy = 0; iy <= 10; ++iy) {
            const auto flat = pan_to_bed(ix / 10.0, iy / 10.0, 0.0);
            const auto high = pan_to_bed(ix / 10.0, iy / 10.0, 0.9);
            double power = 0.0;
            for (const double g : flat) {
                power += g * g;
            }
            CHECK(power == Approx(1.0).margin(1e-12));
            CHECK(flat[static_cast<std::size_t>(kBedLfeRow)] == 0.0);
            CHECK(flat == high);
        }
    }
    // the centre of the room is rendered at the front
    CHECK(pan_to_bed(0.5, 0.5)[2] == Approx(1.0));
}

TEST_CASE("the wall pan law follows the shares of the owner's 7.1 presentations on a wall",
          "[truehd][encoder][atmos][render]") {
    // Normalised shares (each row's share of the vector's length) read from the 8 channel decode
    // of two DEE controls, a 1 kHz object moving along the front wall (ctl_e) and along the left
    // wall (ctl_k), at the window centres 1, 3, 7 and 9 s (ctl_e) and 1, 3, 5, 7 and 9 s (ctl_k).
    // Statistics of the owner's streams, no audio. Positions in the metadata's cube (x -1 left to
    // 1 right, y -1 rear to 1 front) become room positions by x = (dx + 1) / 2, y = (1 - dy) / 2.
    struct Share {
        double dx;
        double dy;
        std::array<double, kBedRows> row;  // L R C LFE Ls Rs Lrs Rrs
    };
    constexpr std::array<Share, 9> kShares{{
        {-0.8, 1.0, {0.950, 0, 0.314, 0, 0, 0, 0, 0}},
        {-0.4, 1.0, {0.579, 0, 0.815, 0, 0, 0, 0, 0}},
        {0.4, 1.0, {0, 0.588, 0.809, 0, 0, 0, 0, 0}},
        {0.8, 1.0, {0, 0.954, 0.300, 0, 0, 0, 0, 0}},
        {-1.0, 0.8, {0.950, 0, 0, 0, 0.313, 0, 0, 0}},
        {-1.0, 0.4, {0.580, 0, 0, 0, 0.814, 0, 0, 0}},
        {-1.0, 0.0, {0, 0, 0, 0, 1.000, 0, 0, 0}},
        {-1.0, -0.4, {0, 0, 0, 0, 0.819, 0, 0.574, 0}},
        {-1.0, -0.8, {0, 0, 0, 0, 0.302, 0, 0.953, 0}},
    }};
    double worst_wall = 0.0;
    double worst_ring = 0.0;
    for (const Share& share : kShares) {
        const double x = (share.dx + 1.0) / 2.0;
        const double y = (1.0 - share.dy) / 2.0;
        const auto wall = pan_to_bed(x, y, 0.0, PanLaw::kWall);
        const auto ring = pan_to_bed(x, y, 0.0, PanLaw::kRing);
        for (std::size_t r = 0; r < kBedRows; ++r) {
            worst_wall = std::max(worst_wall, std::abs(wall[r] - share.row[r]));
            worst_ring = std::max(worst_ring, std::abs(ring[r] - share.row[r]));
        }
    }
    CAPTURE(worst_wall, worst_ring);
    CHECK(worst_wall < 0.04);
    CHECK(worst_ring > 0.15);  // the azimuth ring is the decode side's law, not these streams'
}

TEST_CASE("the wall pan law keeps power, passes the speakers and is continuous round the room",
          "[truehd][encoder][atmos][render]") {
    // the speakers' own places, in room coordinates, pan to their own rows
    struct Place {
        double x;
        double y;
        std::size_t row;
    };
    constexpr std::array<Place, 7> kPlaces{{{0.0, 0.0, 0},
                                            {1.0, 0.0, 1},
                                            {0.5, 0.0, 2},
                                            {0.0, 0.5, 4},
                                            {1.0, 0.5, 5},
                                            {0.0, 1.0, 6},
                                            {1.0, 1.0, 7}}};
    for (const Place& place : kPlaces) {
        const auto gains = pan_to_bed(place.x, place.y, 0.0, PanLaw::kWall);
        for (std::size_t r = 0; r < kBedRows; ++r) {
            CAPTURE(place.row, r);
            CHECK(gains[r] == Approx(r == place.row ? 1.0 : 0.0).margin(1e-9));
        }
    }
    // power 1 anywhere, the LFE never, and no jump between neighbouring places
    std::array<double, kBedRows> previous = pan_to_bed(0.0, 0.0, 0.0, PanLaw::kWall);
    double largest = 0.0;
    for (int step = 1; step <= 400; ++step) {
        // once round the walls: the perimeter of the unit square, 4 sides of 100 steps
        const int side = (step - 1) / 100;
        const double f = static_cast<double>((step - 1) % 100 + 1) / 100.0;
        const double x = side == 0 ? f : (side == 1 ? 1.0 : (side == 2 ? 1.0 - f : 0.0));
        const double y = side == 0 ? 0.0 : (side == 1 ? f : (side == 2 ? 1.0 : 1.0 - f));
        const auto gains = pan_to_bed(x, y, 0.0, PanLaw::kWall);
        double power = 0.0;
        for (std::size_t r = 0; r < kBedRows; ++r) {
            power += gains[r] * gains[r];
            largest = std::max(largest, std::abs(gains[r] - previous[r]));
        }
        CHECK(power == Approx(1.0).margin(1e-9));
        CHECK(gains[static_cast<std::size_t>(kBedLfeRow)] == 0.0);
        previous = gains;
    }
    CHECK(largest < 0.05);
    // the middle of the room is at the front; a place off a wall is its projection onto the wall
    CHECK(pan_to_bed(0.5, 0.5, 0.0, PanLaw::kWall)[2] == Approx(1.0));
    CHECK(pan_to_bed(0.25, 0.25, 0.0, PanLaw::kWall) == pan_to_bed(0.0, 0.0, 0.0, PanLaw::kWall));
}

TEST_CASE("a speaker's wall position pans to its own row and to no other",
          "[truehd][encoder][atmos][render]") {
    for (const int row : {0, 1, 2, 4, 5, 6, 7}) {
        const ElementPlacement place = bed_speaker_placement(row);
        CAPTURE(row, place.x, place.y);
        CHECK(place.x >= -1e-12);
        CHECK(place.x <= 1.0 + 1e-12);
        CHECK(place.y >= -1e-12);
        CHECK(place.y <= 1.0 + 1e-12);
        const auto gains = pan_to_bed(place.x, place.y);
        for (std::size_t r = 0; r < gains.size(); ++r) {
            CHECK(gains[r] == Approx(r == static_cast<std::size_t>(row) ? 1.0 : 0.0).margin(1e-9));
        }
    }
    // L is in the front left corner region, Lrs at the back left
    CHECK(bed_speaker_placement(0).x < 0.5);
    CHECK(bed_speaker_placement(0).y == Approx(0.0).margin(1e-12));
    CHECK(bed_speaker_placement(6).x < 0.5);
    CHECK(bed_speaker_placement(6).y == Approx(1.0).margin(1e-12));
    CHECK(bed_speaker_placement(1).x > 0.5);
}

TEST_CASE("the gains move in a straight line between boundaries and meet at each one",
          "[truehd][encoder][atmos][render]") {
    // one free element that moves from the left front to the right front over three sections of
    // 480 samples (the last one 100 short)
    const std::int64_t length = 480;
    const std::int64_t total = 3 * length - 100;
    std::vector<std::vector<ElementPlacement>> boundaries;
    for (int b = 0; b < 4; ++b) {
        boundaries.push_back({ElementPlacement{0.2 + 0.2 * b, 0.1, 0.0, 1.0}});
    }
    const RenderPlan plan = build_render_plan(12, length, total, boundaries);
    REQUIRE(plan.sections.size() == 3);
    CHECK(plan.elements == 12);
    CHECK(plan.sections[2].samples == length - 100);
    for (std::size_t s = 0; s + 1 < plan.sections.size(); ++s) {
        CHECK(plan.sections[s].start + plan.sections[s].samples == plan.sections[s + 1].start);
        CHECK(plan.sections[s].end == plan.sections[s + 1].begin);
    }
    // at a boundary the gains are the panner's for the place there
    const BedMixGains at_480 = gains_at(plan, length);
    const auto pan_480 = pan_to_bed(0.4, 0.1);
    for (std::size_t row = 0; row < kBedRows; ++row) {
        CHECK(at_480[row][0] == Approx(pan_480[row]).margin(1e-12));
    }
    // half way through a section it is the mean of its two ends
    const BedMixGains middle = gains_at(plan, length + length / 2);
    for (std::size_t row = 0; row < kBedRows; ++row) {
        CHECK(middle[row][0] ==
              Approx(0.5 * (plan.sections[1].begin[row][0] + plan.sections[1].end[row][0]))
                  .margin(1e-12));
    }
    // no sample-to-sample step is larger than a section's change spread over the section
    double largest_step = 0.0;
    BedMixGains previous = gains_at(plan, 0);
    for (std::int64_t t = 1; t < total; ++t) {
        const BedMixGains now = gains_at(plan, t);
        for (std::size_t row = 0; row < kBedRows; ++row) {
            largest_step = std::max(largest_step, std::abs(now[row][0] - previous[row][0]));
        }
        previous = now;
    }
    CHECK(largest_step < 2.0 / static_cast<double>(length));
    // held outside the stream
    CHECK(gains_at(plan, -50) == gains_at(plan, 0));
    CHECK(gains_at(plan, total + 500) == plan.sections.back().end);
    // elements the placements do not name carry no gain
    for (std::size_t row = 0; row < kBedRows; ++row) {
        CHECK(at_480[row][1] == 0.0);
    }
}

TEST_CASE("a mix gain is quantised the way a wire coefficient is",
          "[truehd][encoder][atmos][render]") {
    CHECK(quantise_mix_gain(1.0, 12) == 4096);
    CHECK(quantise_mix_gain(0.5, 12) == 2048);
    CHECK(quantise_mix_gain(0.0, 12) == 0);
    CHECK(quantise_mix_gain(1.0 / 4096.0 * 0.4, 12) == 0);
    CHECK(quantise_mix_gain(1.0 / 4096.0 * 0.6, 12) == 1);
    CHECK(quantise_mix_gain(0.7071067811865476, 12) == 2896);
    // frac_bits + 2 bits signed: 14 bits at frac_bits 12
    CHECK(quantise_mix_gain(2.0, 12) == 8191);
    CHECK(quantise_mix_gain(-3.0, 12) == -8192);
    CHECK(quantise_mix_gain(1.0, 14) == 16384);
}

TEST_CASE("the bed-row matrices are primitive, carry the gains negated, and skip silent rows",
          "[truehd][encoder][atmos][render]") {
    BedMixGains gains{};
    gains[0][0] = 0.5;   // L gets half of element 8
    gains[0][2] = 0.25;  // and a quarter of element 10
    gains[5][1] = 1.0;   // Rs gets element 9
    gains[6][3] = 0.00001;  // below the coefficient's last place: no matrix
    const std::vector<MatrixSpec> matrices = bed_mix_matrices(gains, 12, 3);
    REQUIRE(matrices.size() == 2);
    CHECK(matrices[0].out_ch == 0);
    CHECK(matrices[0].frac_bits == 12);
    CHECK(matrices[0].f_shift == 0);
    CHECK(matrices[0].field_m4 == 3);
    CHECK_FALSE(matrices[0].lsb_bypass);
    CHECK(matrices[0].present[0]);
    CHECK(matrices[0].coeff[0] == 4096);
    CHECK(matrices[0].present[8]);
    CHECK(matrices[0].coeff[8] == -2048);
    CHECK_FALSE(matrices[0].present[9]);
    CHECK(matrices[0].present[10]);
    CHECK(matrices[0].coeff[10] == -1024);
    CHECK(matrices[1].out_ch == 5);
    CHECK(matrices[1].present[9]);
    CHECK(matrices[1].coeff[9] == -4096);
    // a 12 element stream has four free elements, 14 has six: a gain past them is not sent
    BedMixGains wide{};
    wide[0][5] = 1.0;  // element 13
    CHECK(bed_mix_matrices(wide, 12, 0).empty());
    CHECK(bed_mix_matrices(wide, 14, 0).size() == 1);
    wide[0][5] = 0.0;
    wide[0][6] = 1.0;  // element 14
    CHECK(bed_mix_matrices(wide, 12, 0).empty());
    CHECK(bed_mix_matrices(wide, 14, 0).empty());
    CHECK(bed_mix_matrices(wide, 16, 0).size() == 1);
}

TEST_CASE("the bed-row matrices run backwards exactly and give the definition's bed",
          "[truehd][encoder][atmos][render][object-bed]") {
    constexpr int kElements = 14;
    constexpr std::size_t kSamples = 160;
    BedMixGains gains{};
    gains[0][0] = 0.7071;
    gains[1][0] = 0.7071;
    gains[2][1] = 1.0;
    gains[4][2] = 0.3333;
    gains[5][2] = 0.9428;
    gains[7][5] = 0.5;
    const std::vector<MatrixSpec> chain = bed_mix_matrices(gains, kElements, 0);
    REQUIRE_FALSE(chain.empty());

    // the elements: a different mix of signs and sizes in each, some of them silent
    std::vector<std::vector<std::int32_t>> elements(kElements, std::vector<std::int32_t>(kSamples));
    std::uint32_t state = 17;
    for (std::size_t e = 0; e < elements.size(); ++e) {
        for (std::size_t t = 0; t < kSamples; ++t) {
            state = state * 1664525U + 1013904223U;
            const auto raw = static_cast<std::int32_t>(state >> 12U) - (1 << 19);
            elements[e][t] = (e == 7 || e == 13) ? 0 : raw;
        }
    }
    ObjectChainShape shape;
    shape.quant = 0;
    shape.units = 4;
    shape.nominal = 40;
    ObjectTimeline timeline;
    timeline.blocks.push_back(ObjectBlock{0, static_cast<int>(kSamples), chain, false, {}});
    std::vector<ObjectPlane> rows = elements;
    ObjectBypass bypass;
    REQUIRE(invert_object_timeline(timeline, shape, rows, bypass));

    // bed row j = element j less the matrix's floor of (sum of -raw * element * 8) / 2^15, which is
    // element j plus the rounded-up share of the coded elements
    for (const MatrixSpec& matrix : chain) {
        for (std::size_t t = 0; t < kSamples; ++t) {
            std::int64_t sum = 0;
            for (std::size_t c = 8; c < static_cast<std::size_t>(kElements); ++c) {
                if (matrix.present[c]) {
                    sum += std::int64_t{matrix.coeff[c]} * 8 * elements[c][t];
                }
            }
            CAPTURE(matrix.out_ch, t);
            CHECK(rows[matrix.out_ch][t] == elements[matrix.out_ch][t] - (sum >> 15));
        }
    }
    // rows no matrix names, and the coded elements, are the elements
    CHECK(rows[3] == elements[3]);
    for (std::size_t c = 8; c < static_cast<std::size_t>(kElements); ++c) {
        CHECK(rows[c] == elements[c]);
    }
    // and the decoder's side gives the elements back
    apply_object_timeline(timeline, shape, bypass, rows);
    CHECK(rows == elements);
}
