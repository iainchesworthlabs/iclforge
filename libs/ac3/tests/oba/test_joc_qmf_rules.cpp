#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/ac3/oba/joc.hpp"
#include "iclforge/ac3/oba/joc_tables.hpp"
#include "joc_qmf_mix.hpp"
#include "iclforge/dsp/qmf.hpp"
#include "iclforge/objects/joc_domain.hpp"

// reconstruct_qmf picks §6.6.5's interpolation rule once for each object and timeslot and
// runs one loop over the 64 subbands for it. These tests hold that to the loop it replaced,
// which decided the rule for every element: a plain transcription of that loop (the switch
// inside the subband loop, one matrix read through the band mapping and one division per
// element, the sum over the five bed channels after it) over the same filterbanks. The
// coefficients and the sums are the same IEEE operations in the same order, so the decoded
// samples have to agree to the bit - not to a tolerance - and a reciprocal in place of a
// division, a reassociated sum or a fused multiply-add fails them.
//
// The frames below reach all eight rules: the delayed tail and the flat frame in the first
// frame (nothing to ramp from), the delayed tail again with two snapshots from the second
// frame on, a smooth ramp over one data point, a smooth one over two (its first and second
// half), and a steep one before, at and after each of its steps.

namespace {

namespace joc = iclforge::ac3::oba::joc;
namespace dsp = iclforge::dsp;

constexpr int kObjects = 6;
constexpr int kFrames = 5;
constexpr int kFrameSamples = dsp::kQmfSlotsPerFrame * dsp::kQmfHop;

struct Noise {
    std::uint32_t state;
    double next() {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<double>(state >> 8) / 8388608.0 - 1.0;
    }
};

// Every rule is reached by one of these, and no two frames carry the same set.
joc::FrameParameters make_params(int frame, int seq_count, Noise& noise) {
    joc::FrameParameters params;
    params.objects = kObjects;
    params.channels = joc::kNumChannels5X;
    params.seq_count = seq_count;
    params.num_bands_idx = 4;
    params.shapes.resize(kObjects);
    // smooth, one data point
    params.shapes[0] = {.present = true, .num_bands_idx = 4, .steep = false, .data_points = 1};
    // smooth, two data points: the first half ramps from the previous frame, the second between
    // the two points
    params.shapes[1] = {.present = true, .num_bands_idx = 5, .steep = false, .data_points = 2};
    // steep, one data point, a step at slot 8
    params.shapes[2] = {.present = true, .num_bands_idx = 3, .steep = true, .data_points = 1,
                        .offset_ts = {8, 0}};
    // steep, two data points, steps at 4 and 10 (the ordinary frame shows slots 0 to 14)
    params.shapes[3] = {.present = true, .num_bands_idx = 7, .steep = true, .data_points = 2,
                        .offset_ts = {4, 10}};
    // one band for the whole spectrum, and then an absent object in the third frame
    params.shapes[4] = {.present = frame != 2, .num_bands_idx = 0, .steep = false,
                        .data_points = 2};
    params.shapes[5] = {.present = true, .num_bands_idx = 2, .steep = frame % 2 == 1,
                        .data_points = frame % 2 == 1 ? 2 : 1, .offset_ts = {2, 14}};
    params.matrix.assign(params.coefficient_count(), 0.0);
    for (auto& value : params.matrix) {
        value = noise.next() * 3.0;
    }
    return params;
}

// The loop reconstruct_qmf had before the rule moved out of it, for the ordinary six-block
// frame: one mixing coefficient at a time, the rule decided for each.
class Reference {
public:
    std::vector<std::vector<float>> reconstruct(std::span<const std::span<const float>> bed,
                                                const joc::FrameParameters& params) {
        const int objects = params.objects;
        if (static_cast<int>(synthesis_.size()) != objects) {
            synthesis_.assign(static_cast<std::size_t>(objects), dsp::QmfSynthesis{});
        }
        const std::size_t previous_size = static_cast<std::size_t>(objects) *
                                          joc::kNumChannels5X * joc::kQmfSubbands;
        const bool has_previous = params.seq_count != 0 && previous_objects_ == objects &&
                                  previous_.size() == previous_size;
        const bool has_older = has_previous && older_.size() == previous_size;
        if (previous_.size() != previous_size) {
            previous_.assign(previous_size, 0.0F);
        }

        std::vector<std::vector<float>> out(static_cast<std::size_t>(objects),
                                            std::vector<float>(kFrameSamples));
        std::array<std::array<double, dsp::kQmfSubbands>, joc::kNumChannels5X> bed_real{};
        std::array<std::array<double, dsp::kQmfSubbands>, joc::kNumChannels5X> bed_imag{};
        std::array<double, dsp::kQmfSubbands> object_real{};
        std::array<double, dsp::kQmfSubbands> object_imag{};

        for (int slot = 0; slot < dsp::kQmfSlotsPerFrame; ++slot) {
            for (int ch = 0; ch < joc::kNumChannels5X; ++ch) {
                const std::span<const float, dsp::kQmfHop> hop{
                    bed[static_cast<std::size_t>(ch)].data() + slot * dsp::kQmfHop,
                    static_cast<std::size_t>(dsp::kQmfHop)};
                analysis_[static_cast<std::size_t>(ch)].push(
                    hop, bed_real[static_cast<std::size_t>(ch)],
                    bed_imag[static_cast<std::size_t>(ch)]);
            }
            const bool previous_frame = slot < dsp::kQmfDelaySlots;
            const double tail_frac =
                static_cast<double>(dsp::kQmfSlotsPerFrame - dsp::kQmfDelaySlots + slot + 1) /
                static_cast<double>(dsp::kQmfSlotsPerFrame);
            const int ts = slot - dsp::kQmfDelaySlots;

            for (int object = 0; object < objects; ++object) {
                const auto shape = params.shape(object);
                auto& synth = synthesis_[static_cast<std::size_t>(object)];
                const std::span<float, dsp::kQmfHop> emitted{
                    out[static_cast<std::size_t>(object)].data() + slot * dsp::kQmfHop,
                    static_cast<std::size_t>(dsp::kQmfHop)};
                if (!shape.present) {
                    object_real.fill(0.0);
                    object_imag.fill(0.0);
                    synth.pull(object_real, object_imag, emitted);
                    continue;
                }
                const auto& mapping =
                    joc::kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];
                constexpr int kHalfWindow = joc::kQmfTimeslots / 2;
                for (int k = 0; k < dsp::kQmfSubbands; ++k) {
                    double real = 0.0;
                    double imag = 0.0;
                    for (int ch = 0; ch < joc::kNumChannels5X; ++ch) {
                        const auto band = mapping[static_cast<std::size_t>(k)];
                        const std::size_t index =
                            (static_cast<std::size_t>(object) * joc::kNumChannels5X +
                             static_cast<std::size_t>(ch)) *
                                joc::kQmfSubbands +
                            static_cast<std::size_t>(k);
                        const double dq0 = params.at(object, 0, ch, band);
                        const double previous = static_cast<double>(previous_[index]);
                        double m = 0.0;
                        if (previous_frame) {
                            const double previous_val = has_previous ? previous : dq0;
                            const double older_val =
                                has_older ? static_cast<double>(older_[index]) : previous_val;
                            m = older_val + tail_frac * (previous_val - older_val);
                        } else if (!has_previous) {
                            m = shape.data_points > 1 ? params.at(object, 1, ch, band) : dq0;
                        } else if (!shape.steep) {
                            if (shape.data_points == 1) {
                                m = previous + static_cast<double>(ts + 1) * (dq0 - previous) /
                                                   static_cast<double>(joc::kQmfTimeslots);
                            } else if (ts < kHalfWindow) {
                                m = previous + static_cast<double>(ts + 1) * (dq0 - previous) /
                                                   static_cast<double>(kHalfWindow);
                            } else {
                                m = dq0 + static_cast<double>(ts - kHalfWindow + 1) *
                                              (params.at(object, 1, ch, band) - dq0) /
                                              static_cast<double>(joc::kQmfTimeslots - kHalfWindow);
                            }
                        } else if (ts < shape.offset_ts[0]) {
                            m = previous;
                        } else if (shape.data_points == 1 || ts < shape.offset_ts[1]) {
                            m = dq0;
                        } else {
                            m = params.at(object, 1, ch, band);
                        }
                        real += m * bed_real[static_cast<std::size_t>(ch)]
                                            [static_cast<std::size_t>(k)];
                        imag += m * bed_imag[static_cast<std::size_t>(ch)]
                                            [static_cast<std::size_t>(k)];
                    }
                    object_real[static_cast<std::size_t>(k)] = real;
                    object_imag[static_cast<std::size_t>(k)] = imag;
                }
                synth.pull(object_real, object_imag, emitted);
            }
        }

        older_ = std::move(previous_);
        previous_.assign(previous_size, 0.0F);
        for (int object = 0; object < objects; ++object) {
            const auto shape = params.shape(object);
            const auto& mapping =
                joc::kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];
            for (int ch = 0; ch < joc::kNumChannels5X; ++ch) {
                for (int subband = 0; subband < joc::kQmfSubbands; ++subband) {
                    const std::size_t index =
                        (static_cast<std::size_t>(object) * joc::kNumChannels5X +
                         static_cast<std::size_t>(ch)) *
                            joc::kQmfSubbands +
                        static_cast<std::size_t>(subband);
                    previous_[index] = static_cast<float>(
                        shape.present
                            ? params.at(object, shape.data_points - 1, ch,
                                        mapping[static_cast<std::size_t>(subband)])
                            : 0.0);
                }
            }
        }
        previous_objects_ = objects;
        return out;
    }

private:
    std::array<dsp::QmfAnalysis, joc::kNumChannels5X> analysis_{};
    std::vector<dsp::QmfSynthesis> synthesis_{};
    std::vector<float> previous_{};
    std::vector<float> older_{};
    int previous_objects_ = 0;
};

bool identical(const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t o = 0; o < a.size(); ++o) {
        if (a[o].size() != b[o].size()) {
            return false;
        }
        for (std::size_t i = 0; i < a[o].size(); ++i) {
            if (std::bit_cast<std::uint32_t>(a[o][i]) != std::bit_cast<std::uint32_t>(b[o][i])) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("the QMF reconstruction matches the per element loop to the bit over every rule",
          "[atmos][joc][qmf]") {
    // Frame 0 is a splice (seq_count 0); the next four ramp from what the one before left.
    Noise noise{12345U};
    joc::ReconstructionState state;
    Reference reference;

    std::array<std::vector<float>, joc::kNumChannels5X> channels;
    std::vector<std::span<const float>> bed(joc::kNumChannels5X);
    for (int frame = 0; frame < kFrames; ++frame) {
        CAPTURE(frame);
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            channels[ch].resize(kFrameSamples);
            for (auto& sample : channels[ch]) {
                sample = static_cast<float>(noise.next() * 0.5);
            }
            bed[ch] = channels[ch];
        }
        const auto params = make_params(frame, frame, noise);
        const auto expected = reference.reconstruct(bed, params);
        const auto actual = joc::reconstruct(bed, params, state, true, true,
                                             iclforge::objects::oba::joc::Domain::kQmf);
        REQUIRE(actual.size() == expected.size());
        CHECK(identical(actual, expected));
    }
}

TEST_CASE("the QMF reconstruction matches the per element loop when decoding starts mid stream",
          "[atmos][joc][qmf]") {
    // seq_count is not zero but there is nothing to ramp from: the flat rule, and the delayed
    // tail with the first data point standing in for both snapshots.
    Noise noise{777U};
    joc::ReconstructionState state;
    Reference reference;
    std::array<std::vector<float>, joc::kNumChannels5X> channels;
    std::vector<std::span<const float>> bed(joc::kNumChannels5X);
    for (int frame = 0; frame < 3; ++frame) {
        CAPTURE(frame);
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            channels[ch].resize(kFrameSamples);
            for (auto& sample : channels[ch]) {
                sample = static_cast<float>(noise.next() * 0.25);
            }
            bed[ch] = channels[ch];
        }
        const auto params = make_params(frame + 1, 40 + frame, noise);
        const auto expected = reference.reconstruct(bed, params);
        const auto actual = joc::reconstruct(bed, params, state, true, true,
                                             iclforge::objects::oba::joc::Domain::kQmf);
        CHECK(identical(actual, expected));
    }
}

// The decoded samples are floats, and a last-bit difference in a double coefficient seldom
// survives the rounding to float, so the cases above cannot see it. These hold each loop of
// joc_qmf_mix.hpp to the expression it stands for on the doubles themselves.
namespace {

namespace mix = iclforge::ac3::oba::joc::qmf_mix;

std::size_t differing(const double* a, const double* b, std::size_t count) {
    std::size_t different = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (std::bit_cast<std::uint64_t>(a[i]) != std::bit_cast<std::uint64_t>(b[i])) {
            ++different;
        }
    }
    return different;
}

}  // namespace

TEST_CASE("the QMF mixing rows match the per element expression to the bit on the doubles",
          "[atmos][joc][qmf]") {
    constexpr std::size_t kSub = dsp::kQmfSubbands;
    constexpr int kHalf = joc::kQmfTimeslots / 2;
    Noise noise{99U};

    std::size_t compared = 0;
    for (int idx = 0; idx < 8; ++idx) {
        CAPTURE(idx);
        const auto& map = joc::kSubbandToBand[static_cast<std::size_t>(idx)];
        const int bands = joc::kNumBands[static_cast<std::size_t>(idx)];
        std::vector<double> dq0(static_cast<std::size_t>(bands));
        std::vector<double> dq1(static_cast<std::size_t>(bands));
        for (auto& v : dq0) {
            v = noise.next() * 4.0;
        }
        for (auto& v : dq1) {
            v = noise.next() * 4.0;
        }
        std::array<float, kSub> previous{};
        std::array<float, kSub> older{};
        for (auto& v : previous) {
            v = static_cast<float>(noise.next() * 4.0);
        }
        for (auto& v : older) {
            v = static_cast<float>(noise.next() * 4.0);
        }
        // Zeros of both signs, which a reordered sum or a dropped term would change.
        previous[3] = 0.0F;
        older[3] = -0.0F;
        older[4] = 0.0F;
        previous[4] = -0.0F;

        std::array<double, kSub> got{};
        std::array<double, kSub> want{};

        // the delayed tail, nine slots, with each of its three sets of snapshots
        for (int slot = 0; slot < dsp::kQmfDelaySlots; ++slot) {
            const double tail_frac =
                static_cast<double>(dsp::kQmfSlotsPerFrame - dsp::kQmfDelaySlots + slot + 1) /
                static_cast<double>(dsp::kQmfSlotsPerFrame);
            mix::tail_blend<true, true>(got.data(), dq0.data(), map.data(), previous.data(),
                                        older.data(), tail_frac);
            for (std::size_t k = 0; k < kSub; ++k) {
                const double p = static_cast<double>(previous[k]);
                const double o = static_cast<double>(older[k]);
                want[k] = o + tail_frac * (p - o);
            }
            CHECK(differing(got.data(), want.data(), kSub) == 0);

            mix::tail_blend<true, false>(got.data(), dq0.data(), map.data(), previous.data(),
                                         nullptr, tail_frac);
            for (std::size_t k = 0; k < kSub; ++k) {
                const double p = static_cast<double>(previous[k]);
                want[k] = p + tail_frac * (p - p);
            }
            CHECK(differing(got.data(), want.data(), kSub) == 0);

            mix::tail_blend<false, false>(got.data(), dq0.data(), map.data(), nullptr, nullptr,
                                          tail_frac);
            for (std::size_t k = 0; k < kSub; ++k) {
                const double d = dq0[map[k]];
                want[k] = d + tail_frac * (d - d);
            }
            CHECK(differing(got.data(), want.data(), kSub) == 0);
            compared += 3 * kSub;
        }

        // the ramps and the steps, for each of the frame's own fifteen slots
        for (int ts = 0; ts < dsp::kQmfSlotsPerFrame - dsp::kQmfDelaySlots; ++ts) {
            CAPTURE(ts);
            mix::smooth_from_previous(got.data(), dq0.data(), map.data(), previous.data(),
                                      static_cast<double>(ts + 1),
                                      static_cast<double>(joc::kQmfTimeslots));
            for (std::size_t k = 0; k < kSub; ++k) {
                const double p = static_cast<double>(previous[k]);
                want[k] = p + static_cast<double>(ts + 1) * (dq0[map[k]] - p) /
                                  static_cast<double>(joc::kQmfTimeslots);
            }
            CHECK(differing(got.data(), want.data(), kSub) == 0);

            mix::smooth_from_previous(got.data(), dq0.data(), map.data(), previous.data(),
                                      static_cast<double>(ts + 1), static_cast<double>(kHalf));
            for (std::size_t k = 0; k < kSub; ++k) {
                const double p = static_cast<double>(previous[k]);
                want[k] = p + static_cast<double>(ts + 1) * (dq0[map[k]] - p) /
                                  static_cast<double>(kHalf);
            }
            CHECK(differing(got.data(), want.data(), kSub) == 0);

            mix::smooth_between(got.data(), dq0.data(), dq1.data(), bands, map.data(),
                                static_cast<double>(ts - kHalf + 1),
                                static_cast<double>(joc::kQmfTimeslots - kHalf));
            for (std::size_t k = 0; k < kSub; ++k) {
                want[k] = dq0[map[k]] + static_cast<double>(ts - kHalf + 1) *
                                            (dq1[map[k]] - dq0[map[k]]) /
                                            static_cast<double>(joc::kQmfTimeslots - kHalf);
            }
            CHECK(differing(got.data(), want.data(), kSub) == 0);
            compared += 3 * kSub;
        }

        mix::copy_previous(got.data(), previous.data());
        for (std::size_t k = 0; k < kSub; ++k) {
            want[k] = static_cast<double>(previous[k]);
        }
        CHECK(differing(got.data(), want.data(), kSub) == 0);
        mix::spread(got.data(), dq1.data(), map.data());
        for (std::size_t k = 0; k < kSub; ++k) {
            want[k] = dq1[map[k]];
        }
        CHECK(differing(got.data(), want.data(), kSub) == 0);
    }
    CHECK(compared > 0);
}

TEST_CASE("the QMF combination sums the five channels in order to the bit on the doubles",
          "[atmos][joc][qmf]") {
    constexpr std::size_t kSub = dsp::kQmfSubbands;
    Noise noise{4242U};
    for (int round = 0; round < 40; ++round) {
        mix::Rows rows{};
        mix::Rows real{};
        mix::Rows imag{};
        for (std::size_t c = 0; c < rows.size(); ++c) {
            for (std::size_t k = 0; k < kSub; ++k) {
                rows[c][k] = noise.next() * 3.0;
                real[c][k] = noise.next() * 1000.0 * (round % 3 == 0 ? 1e-9 : 1.0);
                imag[c][k] = noise.next() * 0.001;
            }
        }
        std::array<double, kSub> got_real{};
        std::array<double, kSub> got_imag{};
        mix::combine(rows, real, imag, got_real.data(), got_imag.data());
        std::array<double, kSub> want_real{};
        std::array<double, kSub> want_imag{};
        for (std::size_t k = 0; k < kSub; ++k) {
            double r = 0.0;
            double i = 0.0;
            for (std::size_t c = 0; c < rows.size(); ++c) {
                r += rows[c][k] * real[c][k];
                i += rows[c][k] * imag[c][k];
            }
            want_real[k] = r;
            want_imag[k] = i;
        }
        CHECK(differing(got_real.data(), want_real.data(), kSub) == 0);
        CHECK(differing(got_imag.data(), want_imag.data(), kSub) == 0);
    }
}
