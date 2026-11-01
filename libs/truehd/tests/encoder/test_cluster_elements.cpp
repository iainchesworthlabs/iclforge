#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <set>
#include <vector>

#include "encoder/cluster_elements.hpp"
#include "encoder/encoder_support.hpp"
#include "encoder/object_bed.hpp"
#include "encoder/render_bed.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/encoder/object_paths.hpp"

// The clustering step and the render of the object encoder (libs/truehd/notes/
// feature-truehd-atmos-encoder-render.md): objects with positions become 12, 14 or 16 elements, the
// elements' places become the gains of the bed, and the element signals are written as substream 3.
// [no-external-oracle]: no decoder outside this repository reads restart sync 0x31EC, and DEE's
// clustering is not known, so these tests hold the rule written in the notes and the stream's
// losslessness, not DEE's choices.

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::detail;
using namespace iclforge::truehd::testing;
using Catch::Approx;

constexpr std::int64_t kSection = 480;
constexpr auto kBedRows = static_cast<std::size_t>(kRenderBedRows);

std::vector<std::int32_t> tone(std::size_t samples, double hz, double amplitude) {
    std::vector<std::int32_t> pcm(samples);
    for (std::size_t t = 0; t < samples; ++t) {
        pcm[t] = static_cast<std::int32_t>(std::lround(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t) / 48000.0)));
    }
    return pcm;
}

ClusterKey key(std::int64_t sample, double x, double y, double z = 0.0, double gain = 1.0) {
    return ClusterKey{sample, x, y, z, gain};
}

ClusterConfig config_of(int elements = 12) {
    ClusterConfig config;
    config.elements = elements;
    config.section_samples = kSection;
    config.handover_samples = 40;
    return config;
}

ClusterResult cluster(const std::vector<ClusterObject>& objects, const ClusterConfig& config) {
    auto result = cluster_objects(objects, config);
    REQUIRE(result.has_value());
    return *result;
}

// The render the intended way, one object at a time: each dynamic object panned to the bed at its
// own place and gain, a pinned object straight to its row. [row][sample], from the definition.
std::vector<std::vector<double>> intended_bed(const std::vector<ClusterObject>& objects,
                                              std::size_t samples) {
    std::vector<std::vector<double>> bed(kBedRows, std::vector<double>(samples, 0.0));
    for (const ClusterObject& object : objects) {
        for (std::size_t t = 0; t < samples; ++t) {
            const auto at = static_cast<std::int64_t>(t);
            // straight-line track, held outside the keys
            const auto after = std::ranges::upper_bound(object.keys, at, {}, &ClusterKey::sample);
            ClusterKey k = object.keys.front();
            if (after == object.keys.end()) {
                k = object.keys.back();
            } else if (after != object.keys.begin()) {
                const ClusterKey& from = *(after - 1);
                const double f = static_cast<double>(at - from.sample) /
                                 static_cast<double>(after->sample - from.sample);
                k = ClusterKey{at,
                               from.x + (after->x - from.x) * f,
                               from.y + (after->y - from.y) * f,
                               from.z + (after->z - from.z) * f,
                               from.gain + (after->gain - from.gain) * f};
            }
            const double v = k.gain * static_cast<double>(object.pcm[t]);
            if (object.bed_row >= 0) {
                bed[static_cast<std::size_t>(object.bed_row)][t] += v;
            } else {
                const auto pan = pan_to_bed(k.x, k.y, k.z);
                for (std::size_t row = 0; row < kBedRows; ++row) {
                    bed[row][t] += v * pan[row];
                }
            }
        }
    }
    return bed;
}

// The bed the plan gives for the clustering's signals, in reals: element j plus the gains at the
// sample times the element 8 + k.
std::vector<std::vector<double>> planned_bed(const ClusterResult& clustering,
                                             const RenderPlan& plan) {
    const auto samples = static_cast<std::size_t>(clustering.total_samples);
    std::vector<std::vector<double>> bed(kBedRows, std::vector<double>(samples, 0.0));
    for (std::size_t t = 0; t < samples; ++t) {
        const BedMixGains gains = gains_at(plan, static_cast<std::int64_t>(t));
        for (std::size_t row = 0; row < kBedRows; ++row) {
            double v = clustering.signals[row][t];
            const auto elements = static_cast<std::size_t>(clustering.elements);
            for (std::size_t k = 0; k + kBedRows < elements; ++k) {
                v += gains[row][k] * clustering.signals[kBedRows + k][t];
            }
            bed[row][t] = v;
        }
    }
    return bed;
}

// 20 log10 of the error's power over the reference's, over all the rows.
double error_db(const std::vector<std::vector<double>>& reference,
                const std::vector<std::vector<double>>& actual) {
    double error = 0.0;
    double power = 0.0;
    for (std::size_t row = 0; row < reference.size(); ++row) {
        for (std::size_t t = 0; t < reference[row].size(); ++t) {
            const double d = actual[row][t] - reference[row][t];
            error += d * d;
            power += reference[row][t] * reference[row][t];
        }
    }
    return 10.0 * std::log10(std::max(error, 1e-30) / std::max(power, 1e-30));
}

}  // namespace

TEST_CASE("two objects that cross keep their elements, move their elements with them, and render "
          "close to the intended bed",
          "[truehd][encoder][atmos][render][cluster]") {
    const std::size_t samples = 24000;  // half a second: fifty sections
    const std::vector<std::int32_t> a_pcm = tone(samples, 440.0, 2.0e6);
    const std::vector<std::int32_t> b_pcm = tone(samples, 1130.0, 1.5e6);
    // A runs from the left to the right and a little back, B from the left to the right and a
    // little forward: they pass each other in front of the listener half way through, 0.12 or more
    // from every speaker (a path through the middle of the room is avoided: the direction of a
    // source there turns half a circle, which no straight line of gains can follow)
    std::vector<ClusterObject> objects(2);
    objects[0].pcm = a_pcm;
    objects[0].keys = {key(0, 0.2, 0.25), key(23999, 0.8, 0.45)};
    objects[1].pcm = b_pcm;
    objects[1].keys = {key(0, 0.2, 0.45), key(23999, 0.8, 0.25)};

    const ClusterResult result = cluster(objects, config_of());
    REQUIRE(result.assignment.size() == 50);
    // each object keeps one element of its own for the whole crossing
    for (const std::vector<int>& section : result.assignment) {
        CHECK(section[0] == 8);
        CHECK(section[1] == 9);
    }
    // so the element signals are the objects, unchanged, and the rest are silent
    CHECK(result.signals[8] == a_pcm);
    CHECK(result.signals[9] == b_pcm);
    for (std::size_t e = 0; e < result.signals.size(); ++e) {
        if (e != 8 && e != 9) {
            CHECK(std::ranges::all_of(result.signals[e], [](std::int32_t v) { return v == 0; }));
        }
    }
    CHECK(result.clipped_samples == 0);
    // the element sits where its object is at every boundary, and the two pass through each other
    REQUIRE(result.placements.size() == 51);
    for (std::size_t b = 0; b < result.placements.size(); ++b) {
        const double f = std::min(static_cast<double>(b * kSection) / 23999.0, 1.0);
        CAPTURE(b);
        CHECK(result.placements[b][8].x == Approx(0.2 + 0.6 * f).margin(1e-9));
        CHECK(result.placements[b][8].y == Approx(0.25 + 0.2 * f).margin(1e-9));
        CHECK(result.placements[b][9].x == Approx(0.2 + 0.6 * f).margin(1e-9));
        CHECK(result.placements[b][9].y == Approx(0.45 - 0.2 * f).margin(1e-9));
    }
    // the render: gains meet at every boundary and the decoded bed is close to the intended one
    const RenderPlan plan = make_render_plan(result);
    REQUIRE(plan.sections.size() == 50);
    for (std::size_t s = 0; s + 1 < plan.sections.size(); ++s) {
        CHECK(plan.sections[s].end == plan.sections[s + 1].begin);
    }
    const double db = error_db(intended_bed(objects, samples), planned_bed(result, plan));
    CAPTURE(db);
    CHECK(db < -35.0);
}

TEST_CASE("the crossing's bed matrices, a block at a time, rebuild the elements and give a bed "
          "close to the intended one",
          "[truehd][encoder][atmos][render][cluster][object-bed]") {
    // the writer's chain is not here yet (the notes, question 4), so this holds the part of it
    // that is: the 8 bed-row matrices for the gains at each 40-sample block, run backwards over the
    // elements and forwards again the way the decoder does
    constexpr std::size_t kBlock = 40;
    const std::size_t samples = 24000;
    const std::vector<std::int32_t> a_pcm = tone(samples, 440.0, 2.0e6);
    const std::vector<std::int32_t> b_pcm = tone(samples, 1130.0, 1.5e6);
    std::vector<ClusterObject> objects(2);
    objects[0].pcm = a_pcm;
    objects[0].keys = {key(0, 0.2, 0.25), key(23999, 0.8, 0.45)};
    objects[1].pcm = b_pcm;
    objects[1].keys = {key(0, 0.2, 0.45), key(23999, 0.8, 0.25)};
    const ClusterResult result = cluster(objects, config_of());
    const RenderPlan plan = make_render_plan(result);

    std::vector<std::vector<double>> decoded(kBedRows, std::vector<double>(samples, 0.0));
    ObjectChainShape shape;
    shape.units = 1;
    shape.nominal = static_cast<int>(kBlock);
    for (std::size_t start = 0; start < samples; start += kBlock) {
        const BedMixGains gains = gains_at(plan, static_cast<std::int64_t>(start + kBlock / 2));
        const std::vector<MatrixSpec> chain = bed_mix_matrices(gains, result.elements, 0);
        std::vector<ObjectPlane> rows;
        for (const std::vector<std::int32_t>& signal : result.signals) {
            rows.emplace_back(signal.begin() + static_cast<std::ptrdiff_t>(start),
                              signal.begin() + static_cast<std::ptrdiff_t>(start + kBlock));
        }
        const std::vector<ObjectPlane> elements = rows;
        ObjectBypass bypass;
        REQUIRE(invert_object_chain(chain, shape, rows, bypass));
        for (std::size_t row = 0; row < kBedRows; ++row) {
            for (std::size_t t = 0; t < kBlock; ++t) {
                decoded[row][start + t] = rows[row][t];
            }
        }
        apply_object_chain(chain, shape, bypass, rows);
        REQUIRE(rows == elements);
    }
    const double db = error_db(intended_bed(objects, samples), decoded);
    CAPTURE(db);
    CHECK(db < -40.0);
}

TEST_CASE("the planned bed is much closer to the intended one than a bed that holds the gains of "
          "each section's start",
          "[truehd][encoder][atmos][render][cluster]") {
    // the reason the plan ramps: with held gains the same crossing is far worse
    const std::size_t samples = 24000;
    const std::vector<std::int32_t> a_pcm = tone(samples, 440.0, 2.0e6);
    std::vector<ClusterObject> objects(1);
    objects[0].pcm = a_pcm;
    objects[0].keys = {key(0, 0.2, 0.25), key(23999, 0.8, 0.45)};
    const ClusterResult result = cluster(objects, config_of());
    RenderPlan plan = make_render_plan(result);
    const auto intended = intended_bed(objects, samples);
    const double ramped = error_db(intended, planned_bed(result, plan));
    for (RenderSection& section : plan.sections) {
        section.end = section.begin;
    }
    const double held = error_db(intended, planned_bed(result, plan));
    CAPTURE(ramped, held);
    CHECK(ramped < held - 10.0);
}

TEST_CASE("objects beyond the free elements merge by position, the loudest seed the elements, and "
          "the elements sit at the energy-weighted centre of their members",
          "[truehd][encoder][atmos][render][cluster]") {
    const std::size_t samples = 2400;
    // six objects for the four free elements of a 12 element stream: two tight pairs, two singles
    const std::array<double, 6> amplitude{1.0e6, 0.5e6, 1.0e6, 0.5e6, 0.9e6, 0.8e6};
    const std::array<std::array<double, 2>, 6> where{{{0.20, 0.30},
                                                       {0.22, 0.30},
                                                       {0.80, 0.30},
                                                       {0.82, 0.30},
                                                       {0.50, 0.80},
                                                       {0.50, 0.45}}};
    std::vector<std::vector<std::int32_t>> pcm;
    std::vector<ClusterObject> objects(6);
    for (std::size_t i = 0; i < 6; ++i) {
        pcm.push_back(std::vector<std::int32_t>(samples, static_cast<std::int32_t>(amplitude[i])));
    }
    for (std::size_t i = 0; i < 6; ++i) {
        objects[i].pcm = pcm[i];
        objects[i].keys = {key(0, where[i][0], where[i][1])};
    }
    const ClusterResult result = cluster(objects, config_of());
    for (const std::vector<int>& section : result.assignment) {
        CHECK(section[0] == section[1]);
        CHECK(section[2] == section[3]);
        const std::set<int> distinct{section.begin(), section.end()};
        CHECK(distinct.size() == 4);
        for (const int element : section) {
            CHECK(element >= 8);
            CHECK(element < 12);
        }
    }
    // the element of the first pair is at the centre of its two objects, weighted by their
    // energies (the amplitudes are 1 : 0.5, the energies 4 : 1), not at their plain middle
    const auto pair = static_cast<std::size_t>(result.assignment[2][0]);
    for (std::size_t b = 1; b + 1 < result.placements.size(); ++b) {
        CHECK(result.placements[b][pair].x == Approx((0.20 * 4.0 + 0.22) / 5.0).margin(2e-4));
        CHECK(result.placements[b][pair].y == Approx(0.30).margin(1e-9));
    }
    // nothing is lost in the merge: the elements add up to the objects
    for (std::size_t t = 0; t < samples; ++t) {
        std::int64_t in = 0;
        std::int64_t out = 0;
        for (const auto& object_pcm : pcm) {
            in += object_pcm[t];
        }
        for (const auto& signal : result.signals) {
            out += signal[t];
        }
        CHECK(out == in);
    }
    // the same input gives the same clustering
    const ClusterResult again = cluster(objects, config_of());
    CHECK(again.assignment == result.assignment);
    CHECK(again.signals == result.signals);
    CHECK(again.placements.size() == result.placements.size());
}

TEST_CASE("an object that changes element is handed over without a step",
          "[truehd][encoder][atmos][render][cluster]") {
    const std::size_t samples = 2400;
    // a constant, so that a step in an element shows as a step: at the left speaker for three
    // sections, then in the middle of the room
    const std::vector<std::int32_t> level(samples, 1000000);
    const ElementPlacement left = bed_speaker_placement(0);
    std::vector<ClusterObject> objects(1);
    objects[0].pcm = level;
    objects[0].keys = {key(0, left.x, left.y), key(1200, left.x, left.y), key(1201, 0.5, 0.5),
                       key(2399, 0.6, 0.5)};
    const ClusterResult result = cluster(objects, config_of());
    REQUIRE(result.assignment.size() == 5);
    CHECK(result.assignment[2][0] == 0);
    CHECK(result.assignment[3][0] == 8);
    // the change is at the boundary of sections 2 and 3, sample 1440; the handover is 40 samples
    // centred on it
    const std::vector<std::int32_t>& speaker = result.signals[0];
    const std::vector<std::int32_t>& free = result.signals[8];
    CHECK(speaker[1419] == 1000000);
    CHECK(free[1419] == 0);
    CHECK(free[1460] == 1000000);
    CHECK(speaker[1460] == 0);
    for (std::size_t t = 1420; t < 1460; ++t) {
        CAPTURE(t);
        // the object is split between the two, the parts add to it, and the free element rises
        // by the same amount at every sample
        CHECK(std::abs(speaker[t] + free[t] - 1000000) <= 1);
        CHECK(free[t] > free[t - 1]);
        CHECK(speaker[t] < speaker[t - 1]);
        CHECK(std::abs((free[t] - free[t - 1]) - 1000000 / 41) <= 2);
    }
    // the element left behind keeps the speaker's place; the free one is where the object is at
    // each boundary, from the boundary it first appears at (3) to the end of the stream (5)
    const auto x_at = [](double t) { return 0.5 + 0.1 * (t - 1201.0) / 1198.0; };
    CHECK(result.placements[3][0].x == Approx(left.x));
    CHECK(result.placements[3][8].x == Approx(x_at(1440.0)).margin(1e-9));
    CHECK(result.placements[4][8].x == Approx(x_at(1920.0)).margin(1e-9));
    CHECK(result.placements[5][8].x == Approx(0.6).margin(1e-9));
}

TEST_CASE("an object at the edge of a speaker's snap radius does not flip between elements",
          "[truehd][encoder][atmos][render][cluster]") {
    const std::size_t samples = 4800;
    const std::vector<std::int32_t> level(samples, 500000);
    const ElementPlacement centre = bed_speaker_placement(2);  // the centre speaker
    ClusterConfig config = config_of();
    std::vector<ClusterObject> objects(1);
    objects[0].pcm = level;
    // inside the radius (0.12) in the first section, then 0.15 from the speaker (outside the
    // radius, inside radius + hysteresis) in the odd sections, 0.10 in the even ones
    for (std::int64_t s = 0; s < 10; ++s) {
        const double offset = s % 2 == 1 ? 0.15 : 0.10;
        objects[0].keys.push_back(key(s * kSection, centre.x, centre.y + offset));
        objects[0].keys.push_back(key(s * kSection + kSection - 1, centre.x, centre.y + offset));
    }
    const ClusterResult held = cluster(objects, config);
    for (const std::vector<int>& section : held.assignment) {
        CHECK(section[0] == 2);
    }
    // without the margin it flips at every odd section
    config.hysteresis = 0.0;
    const ClusterResult flipped = cluster(objects, config);
    int in_free = 0;
    for (const std::vector<int>& section : flipped.assignment) {
        in_free += section[0] >= 8 ? 1 : 0;
    }
    CHECK(in_free >= 4);
}

TEST_CASE("bed objects stay in their rows and nothing dynamic reaches the LFE",
          "[truehd][encoder][atmos][render][cluster]") {
    const std::size_t samples = 1200;
    const std::vector<std::int32_t> lfe_pcm = tone(samples, 60.0, 3.0e6);
    const std::vector<std::int32_t> front_pcm = tone(samples, 700.0, 1.0e6);
    const std::vector<std::int32_t> drifter = tone(samples, 900.0, 1.0e6);
    std::vector<ClusterObject> objects(3);
    objects[0].pcm = lfe_pcm;
    objects[0].bed_row = kBedLfeRow;
    objects[0].keys = {key(0, 0.9, 0.9)};  // the keys' place does not matter for a bed object
    objects[1].pcm = front_pcm;
    objects[1].bed_row = 2;
    objects[1].keys = {key(0, 0.5, 0.0, 0.0, 0.5)};  // but its gain does
    // a dynamic object at the LFE's own wall position is the centre speaker's, not the LFE's
    objects[2].pcm = drifter;
    objects[2].keys = {key(0, 0.5, 0.0)};
    const ClusterResult result = cluster(objects, config_of());
    for (const std::vector<int>& section : result.assignment) {
        CHECK(section[0] == 3);
        CHECK(section[1] == 2);
        CHECK(section[2] == 2);
    }
    CHECK(result.signals[3] == lfe_pcm);
    for (std::size_t t = 0; t < samples; ++t) {
        const double expected = 0.5 * front_pcm[t] + drifter[t];
        CHECK(std::abs(result.signals[2][t] - expected) <= 1.0);
    }
    // the speaker-anchored elements are at their speakers' wall positions
    for (const int row : {0, 1, 2, 4, 5, 6, 7}) {
        const ElementPlacement speaker = bed_speaker_placement(row);
        CHECK(result.placements[0][static_cast<std::size_t>(row)].x == Approx(speaker.x));
        CHECK(result.placements[0][static_cast<std::size_t>(row)].y == Approx(speaker.y));
    }
}

TEST_CASE("a sum past the sample width is clipped and counted",
          "[truehd][encoder][atmos][render][cluster]") {
    const std::size_t samples = 200;
    const std::vector<std::int32_t> loud(samples, 6000000);
    std::vector<ClusterObject> objects(2);
    objects[0].pcm = loud;
    objects[0].bed_row = 0;
    objects[0].keys = {key(0, 0.5, 0.5)};
    objects[1].pcm = loud;
    objects[1].bed_row = 0;
    objects[1].keys = {key(0, 0.5, 0.5)};
    const ClusterResult result = cluster(objects, config_of());
    CHECK(result.clipped_samples == samples);
    CHECK(std::ranges::all_of(result.signals[0], [](std::int32_t v) { return v == 8388607; }));
    ClusterConfig narrow = config_of();
    narrow.bits = 16;
    const ClusterResult small = cluster(objects, narrow);
    CHECK(std::ranges::all_of(small.signals[0], [](std::int32_t v) { return v == 32767; }));
}

TEST_CASE("clustering refuses what it cannot cluster",
          "[truehd][encoder][atmos][render][cluster]") {
    const std::vector<std::int32_t> pcm(100, 1);
    const std::vector<std::int32_t> shorter(50, 1);
    std::vector<ClusterObject> objects(1);
    objects[0].pcm = pcm;
    objects[0].keys = {key(0, 0.5, 0.5)};

    const auto refused = [&](const std::vector<ClusterObject>& set, const ClusterConfig& config,
                             ClusterErrorKind kind) {
        const auto result = cluster_objects(set, config);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == kind);
        CHECK_FALSE(result.error().detail.empty());
    };
    ClusterConfig bad = config_of(13);
    refused(objects, bad, ClusterErrorKind::kInvalidConfig);
    bad = config_of();
    bad.handover_samples = 41;
    refused(objects, bad, ClusterErrorKind::kInvalidConfig);
    bad = config_of();
    bad.handover_samples = kSection + 2;
    refused(objects, bad, ClusterErrorKind::kInvalidConfig);
    bad = config_of();
    bad.section_samples = 0;
    refused(objects, bad, ClusterErrorKind::kInvalidConfig);

    std::vector<ClusterObject> two = objects;
    two.push_back(objects[0]);
    two[1].pcm = shorter;
    refused(two, config_of(), ClusterErrorKind::kInvalidObject);
    two = objects;
    two[0].bed_row = 8;
    refused(two, config_of(), ClusterErrorKind::kInvalidObject);
    two = objects;
    two[0].keys.clear();
    refused(two, config_of(), ClusterErrorKind::kInvalidObject);
    two = objects;
    two[0].keys = {key(10, 0.5, 0.5), key(10, 0.6, 0.5)};
    refused(two, config_of(), ClusterErrorKind::kInvalidObject);

    // an empty scene is fine and gives silent elements
    const auto none = cluster_objects({}, config_of());
    REQUIRE(none.has_value());
    CHECK(none->signals.size() == 12);
}

TEST_CASE("the crossing is written with the plan's paths and its bed is close to the intended one, "
          "12, 14 and 16",
          "[truehd][encoder][atmos][render][cluster]") {
    const int elements = GENERATE(12, 14, 16);
    const bool slopes = GENERATE(false, true);
    CAPTURE(elements, slopes);
    const std::size_t samples = 24000;
    const std::vector<std::int32_t> a_pcm = tone(samples, 440.0, 2.0e6);
    const std::vector<std::int32_t> b_pcm = tone(samples, 1130.0, 1.5e6);
    std::vector<ClusterObject> objects(2);
    objects[0].pcm = a_pcm;
    objects[0].keys = {key(0, 0.2, 0.25), key(23999, 0.8, 0.45)};
    objects[1].pcm = b_pcm;
    objects[1].keys = {key(0, 0.2, 0.45), key(23999, 0.8, 0.25)};
    ClusterConfig config = config_of(elements);
    config.section_samples = 480;
    const ClusterResult result = cluster(objects, config);
    const RenderPlan plan = make_render_plan(result);

    EncoderConfig writer;
    writer.channels = 8;
    writer.object_channels = elements;
    writer.object_layout = ObjectLayout::kBedMatrices;
    writer.object_meaning.dyn_object_only = true;
    writer.object_meaning.lfe_present = true;
    writer.object_paths = object_paths_of(plan);
    // with the slopes the matrices side works out the deltas (it decides them, not this test)
    writer.object_extension = slopes ? ObjectExtension::kSlopes : ObjectExtension::kNone;
    writer.verify = true;
    std::vector<std::int32_t> interleaved(samples * static_cast<std::size_t>(elements));
    for (std::size_t t = 0; t < samples; ++t) {
        for (std::size_t e = 0; e < static_cast<std::size_t>(elements); ++e) {
            interleaved[t * static_cast<std::size_t>(elements) + e] = result.signals[e][t];
        }
    }
    const Encoded encoded = encode_all(writer, interleaved);
    REQUIRE_FALSE(encoded.units.empty());
    CHECK(encoded.stats.verified_units == encoded.units.size());
    CHECK(encoded.stats.object_mix_reduced_intervals == 0);
    // substream 3 gives the elements back, and the 7.1 bed of substreams 0 to 2 is the plan's
    CHECK(decode_all(encoded.units, 24, elements, Presentation::kAll) == interleaved);
    const std::vector<std::int32_t> bed = decode_all(encoded.units, 24, 8);
    REQUIRE(bed.size() >= samples * kBedRows);
    std::vector<std::vector<double>> decoded(kBedRows, std::vector<double>(samples, 0.0));
    for (std::size_t t = 0; t < samples; ++t) {
        for (std::size_t row = 0; row < kBedRows; ++row) {
            decoded[row][t] = bed[t * kBedRows + row];
        }
    }
    const double db = error_db(intended_bed(objects, samples), decoded);
    CAPTURE(db);
    CHECK(db < -35.0);
}

TEST_CASE("clustered elements are written as substream 3 and decode losslessly, 12, 14 and 16",
          "[truehd][encoder][atmos][render][cluster]") {
    const int elements = GENERATE(12, 14, 16);
    CAPTURE(elements);
    constexpr int kUnitsPerInterval = 8;
    const int frames = 40 * kUnitsPerInterval * 4 + 17;
    const auto samples = static_cast<std::size_t>(frames);

    // the bed channels of a 7.1 programme as objects pinned to their rows, and ten moving objects
    // (more than the free elements of a 12 element stream, so that the merge is exercised there)
    std::vector<std::vector<std::int32_t>> pcm;
    std::vector<ClusterObject> objects;
    for (int row = 0; row < kRenderBedRows; ++row) {
        pcm.push_back(tone(samples, 110.0 + 90.0 * row, 1.0e6));
    }
    for (int i = 0; i < 10; ++i) {
        pcm.push_back(tone(samples, 410.0 + 130.0 * i, 0.4e6));
    }
    for (int row = 0; row < kRenderBedRows; ++row) {
        ClusterObject object;
        object.pcm = pcm[static_cast<std::size_t>(row)];
        object.bed_row = row;
        object.keys = {key(0, 0.5, 0.5)};
        objects.push_back(object);
    }
    for (int i = 0; i < 10; ++i) {
        ClusterObject object;
        object.pcm = pcm[static_cast<std::size_t>(kRenderBedRows + i)];
        const double phase = 0.1 * i;
        object.keys = {key(0, 0.5 + 0.3 * std::cos(phase), 0.5 + 0.3 * std::sin(phase), 0.2),
                       key(frames / 2, 0.5 + 0.3 * std::cos(phase + 2.0),
                           0.5 + 0.3 * std::sin(phase + 2.0), 0.4),
                       key(frames, 0.5 + 0.3 * std::cos(phase + 4.0),
                           0.5 + 0.3 * std::sin(phase + 4.0), 0.0)};
        objects.push_back(object);
    }
    ClusterConfig config = config_of(elements);
    const ClusterResult result = cluster(objects, config);
    REQUIRE(result.signals.size() == static_cast<std::size_t>(elements));
    CHECK(result.clipped_samples == 0);

    // interleaved, halved so that the bed the writer makes from them has room
    std::vector<std::int32_t> interleaved(samples * static_cast<std::size_t>(elements));
    for (std::size_t t = 0; t < samples; ++t) {
        for (std::size_t e = 0; e < static_cast<std::size_t>(elements); ++e) {
            interleaved[t * static_cast<std::size_t>(elements) + e] = result.signals[e][t] / 4;
        }
    }
    EncoderConfig writer;
    writer.channels = 8;
    writer.object_channels = elements;
    writer.object_layout = ObjectLayout::kBedMatrices;
    writer.restart_interval_units = kUnitsPerInterval;
    writer.object_meaning.dyn_object_only = true;
    writer.object_meaning.lfe_present = true;
    writer.verify = true;
    // the bed the plan asks for, not the fixed render: the plan's gains as the encoder's paths
    // (the signals are written a quarter their size, the gains are not)
    writer.object_paths = object_paths_of(make_render_plan(result));
    CHECK(writer.object_paths.size() == static_cast<std::size_t>(elements - kRenderBedRows));
    const Encoded encoded = encode_all(writer, interleaved);
    REQUIRE_FALSE(encoded.units.empty());
    CHECK(encoded.stats.verified_units == encoded.units.size());
    // Presentation::kAll is substream 3's output, decoded by the strict decoder, so that the
    // lossless check of every restart interval passed on the way: the elements, as they went in
    CHECK(decode_all(encoded.units, 24, elements, Presentation::kAll) == interleaved);
}
