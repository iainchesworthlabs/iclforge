#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/ac3/oba/joc.hpp"
#include "iclforge/ac3/oba/joc_tables.hpp"
#include "joc_qmf_mix.hpp"
#include "iclforge/dsp/qmf.hpp"
#include "iclforge/objects/joc_domain.hpp"

// The two QMF reconstructions that test_joc_qmf_rules.cpp does not reach: the single-precision
// one (ReconstructionState::qmf_single_precision, what a decoder built with a float decode
// scalar runs - the ESP32-S3 and ESP32-P4 images) and the short syncframe one. Both now settle
// §6.6.5's rule once for an object and a timeslot and run one loop for it (joc_qmf_mix.hpp), where
// they used to decide it for every band or every subband. These tests hold them to the code they
// replaced, to the bit:
//
//   - each row loop against the per-element expression it stands for, on the intermediate values
//     (a mixing coefficient that differs in its last bit seldom survives the rounding to the
//     decoded float, so a comparison of the decoded samples alone cannot see it);
//   - the whole reconstruction against a plain transcription of the old function, over every
//     rule, frame after frame (the state each frame leaves the next is checked too).
//
// The coefficients and the sums are the same IEEE operations in the same order, so a reciprocal
// in place of a division, a reordered sum, a fused multiply-add, or an operation done in double
// that was done in float (or the reverse) fails them.

namespace {

namespace joc = iclforge::ac3::oba::joc;
namespace dsp = iclforge::dsp;
namespace mix = iclforge::ac3::oba::joc::qmf_mix;

constexpr std::size_t kSub = dsp::kQmfSubbands;
constexpr int kChannels = joc::kNumChannels5X;

struct Noise {
    std::uint32_t state;
    double next() {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<double>(state >> 8) / 8388608.0 - 1.0;
    }
    float nextf() { return static_cast<float>(next()); }
};

std::size_t differing(const float* a, const float* b, std::size_t count) {
    std::size_t different = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (std::bit_cast<std::uint32_t>(a[i]) != std::bit_cast<std::uint32_t>(b[i])) {
            ++different;
        }
    }
    return different;
}

std::size_t differing(const double* a, const double* b, std::size_t count) {
    std::size_t different = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (std::bit_cast<std::uint64_t>(a[i]) != std::bit_cast<std::uint64_t>(b[i])) {
            ++different;
        }
    }
    return different;
}

bool identical(const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t o = 0; o < a.size(); ++o) {
        if (a[o].size() != b[o].size() ||
            differing(a[o].data(), b[o].data(), a[o].size()) != 0) {
            return false;
        }
    }
    return true;
}

// Values with a full mantissa, and the zeros of both signs a reordered sum or a dropped term
// would change.
void fill(std::vector<float>& v, Noise& noise, float scale) {
    for (auto& x : v) {
        x = noise.nextf() * scale;
    }
    if (v.size() > 4) {
        v[1] = 0.0F;
        v[2] = -0.0F;
    }
}

// ---------------------------------------------------------------------------------------------
// Rows of the single-precision path, against the per-band expression of the switch they replaced.
// ---------------------------------------------------------------------------------------------

// The remainder after four-wide steps is every band count's 0 to 3 last bands, so all of 1 to 23
// are run.
TEST_CASE("the single precision mixing rows match the per band expression to the bit",
          "[atmos][joc][qmf]") {
    constexpr int kHalf = joc::kQmfTimeslots / 2;
    Noise noise{31337U};
    std::size_t compared = 0;
    for (std::size_t bands = 1; bands <= mix::kMaxBands - 1; ++bands) {
        CAPTURE(bands);
        std::vector<float> dq0(bands);
        std::vector<float> dq1(bands);
        std::vector<float> previous(bands);
        std::vector<float> older(bands);
        fill(dq0, noise, 4.0F);
        fill(dq1, noise, 4.0F);
        fill(previous, noise, 4.0F);
        fill(older, noise, 4.0F);
        if (bands > 4) {
            older[1] = -0.0F;
            previous[2] = 0.0F;
        }
        // Past the last band the rows must not be written: a guard that stays as it was.
        constexpr float kGuard = 12345.0F;
        std::array<float, 32> got{};
        std::array<float, 32> want{};

        const auto check = [&] {
            CHECK(differing(got.data(), want.data(), bands) == 0);
            for (std::size_t i = bands; i < got.size(); ++i) {
                CHECK(got[i] == kGuard);
            }
            compared += bands;
        };

        for (int slot = 0; slot < dsp::kQmfDelaySlots; ++slot) {
            CAPTURE(slot);
            const float tail_frac =
                static_cast<float>(dsp::kQmfSlotsPerFrame - dsp::kQmfDelaySlots + slot + 1) /
                static_cast<float>(dsp::kQmfSlotsPerFrame);
            got.fill(kGuard);
            want.fill(kGuard);
            mix::single::tail_blend<true, true>(got.data(), dq0.data(), previous.data(),
                                                older.data(), tail_frac, bands);
            for (std::size_t b = 0; b < bands; ++b) {
                want[b] = older[b] + tail_frac * (previous[b] - older[b]);
            }
            check();

            got.fill(kGuard);
            want.fill(kGuard);
            mix::single::tail_blend<true, false>(got.data(), dq0.data(), previous.data(), nullptr,
                                                 tail_frac, bands);
            for (std::size_t b = 0; b < bands; ++b) {
                want[b] = previous[b] + tail_frac * (previous[b] - previous[b]);
            }
            check();

            got.fill(kGuard);
            want.fill(kGuard);
            mix::single::tail_blend<false, false>(got.data(), dq0.data(), nullptr, nullptr,
                                                  tail_frac, bands);
            for (std::size_t b = 0; b < bands; ++b) {
                want[b] = dq0[b] + tail_frac * (dq0[b] - dq0[b]);
            }
            check();
        }

        for (int ts = 0; ts < dsp::kQmfSlotsPerFrame - dsp::kQmfDelaySlots; ++ts) {
            CAPTURE(ts);
            const float whole = static_cast<float>(ts + 1) / static_cast<float>(joc::kQmfTimeslots);
            const float first = static_cast<float>(ts + 1) / static_cast<float>(kHalf);
            const float second = static_cast<float>(ts - kHalf + 1) /
                                 static_cast<float>(joc::kQmfTimeslots - kHalf);
            for (const float fraction : {whole, first}) {
                got.fill(kGuard);
                want.fill(kGuard);
                mix::single::smooth_from_previous(got.data(), dq0.data(), previous.data(),
                                                  fraction, bands);
                for (std::size_t b = 0; b < bands; ++b) {
                    want[b] = previous[b] + fraction * (dq0[b] - previous[b]);
                }
                check();
            }
            got.fill(kGuard);
            want.fill(kGuard);
            mix::single::smooth_between(got.data(), dq0.data(), dq1.data(), second, bands);
            for (std::size_t b = 0; b < bands; ++b) {
                want[b] = dq0[b] + second * (dq1[b] - dq0[b]);
            }
            check();
        }
    }
    CHECK(compared > 0);
}

TEST_CASE("the single precision combination matches the per subband loop to the bit",
          "[atmos][joc][qmf]") {
    // Each channel's coefficient for a subband is its parameter band's, read through the band
    // mapping; the sum over the five channels runs from zero in channel order.
    Noise noise{2718U};
    for (int idx = 0; idx < 8; ++idx) {
        CAPTURE(idx);
        const auto& map = joc::kSubbandToBand[static_cast<std::size_t>(idx)];
        const auto bands = static_cast<std::size_t>(joc::kNumBands[static_cast<std::size_t>(idx)]);
        for (int round = 0; round < 8; ++round) {
            CAPTURE(round);
            std::array<std::vector<float>, kChannels> band_rows;
            std::array<const float*, kChannels> pointers{};
            for (std::size_t c = 0; c < band_rows.size(); ++c) {
                band_rows[c].resize(bands);
                fill(band_rows[c], noise, 3.0F);
                pointers[c] = band_rows[c].data();
            }
            std::vector<float> spectra(static_cast<std::size_t>(kChannels) * 2U * kSub);
            for (auto& x : spectra) {
                x = noise.nextf() * (round % 3 == 0 ? 1e-4F : 100.0F);
            }
            std::array<float, kSub> got_real{};
            std::array<float, kSub> got_imag{};
            mix::single::combine_banded(pointers, map.data(), spectra.data(), got_real.data(),
                                        got_imag.data());
            std::array<float, kSub> want_real{};
            std::array<float, kSub> want_imag{};
            for (std::size_t k = 0; k < kSub; ++k) {
                float real = 0.0F;
                float imag = 0.0F;
                for (std::size_t c = 0; c < band_rows.size(); ++c) {
                    const float m = band_rows[c][map[k]];
                    const float* const channel = spectra.data() + c * 2U * kSub;
                    real += m * channel[k];
                    imag += m * channel[kSub + k];
                }
                want_real[k] = real;
                want_imag[k] = imag;
            }
            CHECK(differing(got_real.data(), want_real.data(), kSub) == 0);
            CHECK(differing(got_imag.data(), want_imag.data(), kSub) == 0);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The short syncframe's rows, against `interpolate`, one coefficient at a time, in double.
// ---------------------------------------------------------------------------------------------

// §6.6.5 Pseudocode 6 for one coefficient, as reconstruct_qmf_short had it (and still states it
// in `interpolate`): a division for every coefficient.
double interpolate_reference(const joc::ObjectShape& shape, double previous, double dq0,
                             double dq1, int ts, int slots) {
    if (!shape.steep) {
        if (shape.data_points == 1) {
            return previous + static_cast<double>(ts + 1) * (dq0 - previous) /
                                  static_cast<double>(slots);
        }
        const int half = slots / 2;
        if (ts < half) {
            return previous + static_cast<double>(ts + 1) * (dq0 - previous) /
                                  static_cast<double>(half);
        }
        return dq0 + static_cast<double>(ts - half + 1) * (dq1 - dq0) /
                         static_cast<double>(slots - half);
    }
    if (ts < shape.offset_ts[0]) {
        return previous;
    }
    if (shape.data_points == 1 || ts < shape.offset_ts[1]) {
        return dq0;
    }
    return dq1;
}

TEST_CASE("the short syncframe mixing rows match the per element expression to the bit",
          "[atmos][joc][qmf]") {
    Noise noise{8086U};
    const std::vector<joc::ObjectShape> shapes = {
        {.present = true, .num_bands_idx = 4, .steep = false, .data_points = 1},
        {.present = true, .num_bands_idx = 5, .steep = false, .data_points = 2},
        {.present = true, .num_bands_idx = 3, .steep = true, .data_points = 1, .offset_ts = {2, 0}},
        {.present = true, .num_bands_idx = 7, .steep = true, .data_points = 2, .offset_ts = {1, 3}},
        {.present = true, .num_bands_idx = 0, .steep = true, .data_points = 2, .offset_ts = {0, 40}},
        {.present = true, .num_bands_idx = 6, .steep = true, .data_points = 1, .offset_ts = {30, 0}},
    };
    std::size_t compared = 0;
    for (const int slots : {4, 8, 12}) {
        for (std::size_t s = 0; s < shapes.size(); ++s) {
            const auto& shape = shapes[s];
            const auto& map = joc::kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];
            const int bands = shape.bands();
            std::vector<double> dq0(static_cast<std::size_t>(bands));
            std::vector<double> dq1(static_cast<std::size_t>(bands));
            for (auto& v : dq0) {
                v = noise.next() * 4.0;
            }
            for (auto& v : dq1) {
                v = noise.next() * 4.0;
            }
            std::array<float, kSub> previous{};
            for (auto& v : previous) {
                v = noise.nextf() * 4.0F;
            }
            previous[5] = -0.0F;
            for (const bool has_ramp : {false, true}) {
                for (int ts = 0; ts < slots; ++ts) {
                    CAPTURE(slots, s, has_ramp, ts);
                    const auto ramp = mix::short_ramp(has_ramp, shape, ts, slots);
                    std::array<double, kSub> got{};
                    mix::short_row(ramp, got.data(), dq0.data(),
                                   shape.data_points > 1 ? dq1.data() : nullptr, bands, map.data(),
                                   previous.data());
                    std::array<double, kSub> want{};
                    for (std::size_t k = 0; k < kSub; ++k) {
                        const double a = dq0[map[k]];
                        const double b = shape.data_points > 1 ? dq1[map[k]] : a;
                        const double p =
                            has_ramp ? static_cast<double>(previous[k]) : a;
                        want[k] = has_ramp
                                      ? interpolate_reference(shape, p, a, b, ts, slots)
                                      : (shape.data_points > 1 ? b : a);
                    }
                    CHECK(differing(got.data(), want.data(), kSub) == 0);
                    compared += kSub;
                }
            }
        }
    }
    CHECK(compared > 0);
}

TEST_CASE("the short syncframe combination sums the five channels in order to the bit",
          "[atmos][joc][qmf]") {
    Noise noise{99U};
    for (int round = 0; round < 40; ++round) {
        CAPTURE(round);
        std::array<std::array<double, kSub>, kChannels> rows{};
        mix::Rows real{};
        mix::Rows imag{};
        std::array<const double*, kChannels> pointers{};
        for (std::size_t c = 0; c < rows.size(); ++c) {
            pointers[c] = rows[c].data();
            for (std::size_t k = 0; k < kSub; ++k) {
                rows[c][k] = noise.next() * 3.0;
                real[c][k] = noise.next() * 1000.0 * (round % 3 == 0 ? 1e-9 : 1.0);
                imag[c][k] = noise.next() * 0.001;
            }
        }
        std::array<double, kSub> got_real{};
        std::array<double, kSub> got_imag{};
        mix::combine_rows(pointers, real, imag, got_real.data(), got_imag.data());
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

// ---------------------------------------------------------------------------------------------
// The whole reconstructions, against the functions as they were before the rules moved out.
// ---------------------------------------------------------------------------------------------

// A set of objects that, over a run of frames, reaches every rule: smooth over one data point and
// over two, steep over one and over two (before, at and after each step), the flat first frame,
// the absent object, and an object whose band count changes from frame to frame.
joc::FrameParameters make_params(int frame, int seq_count, int slots, Noise& noise) {
    constexpr int kObjects = 6;
    // The step positions are in timeslots of the frame: scale them with it.
    const int third = std::max(1, slots / 3);
    joc::FrameParameters params;
    params.objects = kObjects;
    params.channels = kChannels;
    params.seq_count = seq_count;
    params.num_bands_idx = 4;
    params.shapes.resize(kObjects);
    params.shapes[0] = {.present = true, .num_bands_idx = 4, .steep = false, .data_points = 1};
    params.shapes[1] = {.present = true, .num_bands_idx = 5, .steep = false, .data_points = 2};
    params.shapes[2] = {.present = true, .num_bands_idx = 3, .steep = true, .data_points = 1,
                        .offset_ts = {third, 0}};
    params.shapes[3] = {.present = true, .num_bands_idx = 7, .steep = true, .data_points = 2,
                        .offset_ts = {third - 1, (2 * third) + 1}};
    params.shapes[4] = {.present = frame != 2, .num_bands_idx = frame % 2 == 0 ? 0 : 6,
                        .steep = false, .data_points = 2};
    params.shapes[5] = {.present = true, .num_bands_idx = 2, .steep = frame % 2 == 1,
                        .data_points = frame % 2 == 1 ? 2 : 1, .offset_ts = {2, 14}};
    params.matrix.assign(params.coefficient_count(), 0.0);
    for (auto& value : params.matrix) {
        value = noise.next() * 3.0;
    }
    return params;
}

// reconstruct_qmf_short before the rule moved out of its loop: one coefficient at a time, with
// the rule decided for each, and the sum over the five channels a subband at a time.
class ShortReference {
public:
    std::vector<std::vector<float>> reconstruct(std::span<const std::span<const float>> bed,
                                                const joc::FrameParameters& params, int slots) {
        const int objects = params.objects;
        if (static_cast<int>(synthesis_.size()) != objects) {
            synthesis_.assign(static_cast<std::size_t>(objects), dsp::QmfSynthesis{});
            pending_.clear();
        }
        const std::size_t coeffs = static_cast<std::size_t>(objects) * kChannels * kSub;
        const bool has_ramp = params.seq_count != 0 && previous_objects_ == objects &&
                              previous_.size() == coeffs;
        if (previous_.size() != coeffs) {
            previous_.assign(coeffs, 0.0F);
        }
        for (int ts = 0; ts < slots; ++ts) {
            std::vector<double> m(coeffs, 0.0);
            for (int object = 0; object < objects; ++object) {
                const auto shape = params.shape(object);
                if (!shape.present) {
                    continue;
                }
                const auto& mapping =
                    joc::kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];
                for (int ch = 0; ch < kChannels; ++ch) {
                    for (int subband = 0; subband < dsp::kQmfSubbands; ++subband) {
                        const int band = mapping[static_cast<std::size_t>(subband)];
                        const double dq0 = params.at(object, 0, ch, band);
                        const double dq1 = shape.data_points > 1 ? params.at(object, 1, ch, band)
                                                                 : dq0;
                        const std::size_t index =
                            (static_cast<std::size_t>(object) * kChannels +
                             static_cast<std::size_t>(ch)) *
                                kSub +
                            static_cast<std::size_t>(subband);
                        const double previous =
                            has_ramp ? static_cast<double>(previous_[index]) : dq0;
                        m[index] = has_ramp ? interpolate_reference(shape, previous, dq0, dq1, ts,
                                                                    slots)
                                            : (shape.data_points > 1 ? dq1 : dq0);
                    }
                }
            }
            pending_.push_back(std::move(m));
        }
        while (pending_.size() < static_cast<std::size_t>(slots + dsp::kQmfDelaySlots)) {
            pending_.push_front(std::vector<double>(pending_.front()));
        }

        std::vector<std::vector<float>> out(
            static_cast<std::size_t>(objects),
            std::vector<float>(static_cast<std::size_t>(slots) * dsp::kQmfHop));
        for (int slot = 0; slot < slots; ++slot) {
            for (int ch = 0; ch < kChannels; ++ch) {
                const std::span<const float, dsp::kQmfHop> hop{
                    bed[static_cast<std::size_t>(ch)].data() + slot * dsp::kQmfHop,
                    static_cast<std::size_t>(dsp::kQmfHop)};
                analysis_[static_cast<std::size_t>(ch)].push(
                    hop, bed_real_[static_cast<std::size_t>(ch)],
                    bed_imag_[static_cast<std::size_t>(ch)]);
            }
            const std::vector<double>& m = pending_.front();
            for (int object = 0; object < objects; ++object) {
                for (int k = 0; k < dsp::kQmfSubbands; ++k) {
                    double real = 0.0;
                    double imag = 0.0;
                    for (int ch = 0; ch < kChannels; ++ch) {
                        const std::size_t index =
                            (static_cast<std::size_t>(object) * kChannels +
                             static_cast<std::size_t>(ch)) *
                                kSub +
                            static_cast<std::size_t>(k);
                        real += m[index] * bed_real_[static_cast<std::size_t>(ch)]
                                                    [static_cast<std::size_t>(k)];
                        imag += m[index] * bed_imag_[static_cast<std::size_t>(ch)]
                                                    [static_cast<std::size_t>(k)];
                    }
                    object_real_[static_cast<std::size_t>(k)] = real;
                    object_imag_[static_cast<std::size_t>(k)] = imag;
                }
                const std::span<float, dsp::kQmfHop> emitted{
                    out[static_cast<std::size_t>(object)].data() + slot * dsp::kQmfHop,
                    static_cast<std::size_t>(dsp::kQmfHop)};
                synthesis_[static_cast<std::size_t>(object)].pull(object_real_, object_imag_,
                                                                  emitted);
            }
            pending_.pop_front();
        }

        for (int object = 0; object < objects; ++object) {
            const auto shape = params.shape(object);
            const auto& mapping =
                joc::kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];
            for (int ch = 0; ch < kChannels; ++ch) {
                for (int subband = 0; subband < dsp::kQmfSubbands; ++subband) {
                    const std::size_t index =
                        (static_cast<std::size_t>(object) * kChannels +
                         static_cast<std::size_t>(ch)) *
                            kSub +
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
    std::array<dsp::QmfAnalysis, kChannels> analysis_{};
    std::vector<dsp::QmfSynthesis> synthesis_{};
    std::array<std::array<double, kSub>, kChannels> bed_real_{};
    std::array<std::array<double, kSub>, kChannels> bed_imag_{};
    std::array<double, kSub> object_real_{};
    std::array<double, kSub> object_imag_{};
    std::deque<std::vector<double>> pending_{};
    std::vector<float> previous_{};
    int previous_objects_ = 0;
};

// reconstruct_qmf_single before the rule moved out of its loop: the rule decided for every band,
// the coefficient formed for the band and copied to its subbands, the sum a subband at a time,
// all of it in float, over the single-precision banks.
class SingleReference {
public:
    std::vector<std::vector<float>> reconstruct(std::span<const std::span<const float>> bed,
                                                const joc::FrameParameters& params) {
        constexpr int kSlots = dsp::kQmfSlotsPerFrame;
        constexpr int kHalfWindow = joc::kQmfTimeslots / 2;
        const int objects = params.objects;
        if (static_cast<int>(synthesis_.size()) != objects) {
            synthesis_.assign(static_cast<std::size_t>(objects), dsp::QmfSynthesisF{});
            silent_.assign(static_cast<std::size_t>(objects), std::uint8_t{10});
        }
        const std::size_t previous_size = static_cast<std::size_t>(objects) * kChannels * kSub;
        const bool has_previous = params.seq_count != 0 && previous_objects_ == objects &&
                                  previous_.size() == previous_size;
        const bool has_older = has_previous && older_.size() == previous_size;
        if (previous_.size() != previous_size) {
            previous_.assign(previous_size, 0.0F);
        }

        std::vector<std::vector<float>> out(
            static_cast<std::size_t>(objects),
            std::vector<float>(static_cast<std::size_t>(kSlots * dsp::kQmfHop)));

        // The bed's subband values for the whole frame, [slot][channel][real, imag][subband].
        std::vector<float> spectra(static_cast<std::size_t>(kSlots) * kChannels * 2U * kSub);
        for (int ch = 0; ch < kChannels; ++ch) {
            for (int slot = 0; slot < kSlots; ++slot) {
                const std::span<const float, dsp::kQmfHop> hop{
                    bed[static_cast<std::size_t>(ch)].data() + slot * dsp::kQmfHop,
                    static_cast<std::size_t>(dsp::kQmfHop)};
                float* const at = spectra.data() +
                                  ((static_cast<std::size_t>(slot) * kChannels +
                                    static_cast<std::size_t>(ch)) *
                                   2U * kSub);
                analysis_[static_cast<std::size_t>(ch)].push(
                    hop, std::span<float, dsp::kQmfSubbands>{at, kSub},
                    std::span<float, dsp::kQmfSubbands>{at + kSub, kSub});
            }
        }

        for (int object = 0; object < objects; ++object) {
            const auto shape = params.shape(object);
            const int bands = shape.bands();
            const auto& mapping = joc::kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];
            const auto dq = [&](int data_point, int ch, int band) {
                return static_cast<float>(params.at(object, data_point, ch, band));
            };
            const std::size_t object_row =
                static_cast<std::size_t>(object) * kChannels * kSub;
            std::array<int, 32> first_subband{};
            for (int k = dsp::kQmfSubbands - 1; k >= 0; --k) {
                first_subband[static_cast<std::size_t>(mapping[static_cast<std::size_t>(k)])] = k;
            }
            std::array<std::array<float, 32>, kChannels> band_mix{};
            std::array<std::array<float, kSub>, kChannels> mix_rows{};
            std::array<float, kSub> object_real{};
            std::array<float, kSub> object_imag{};
            for (int slot = 0; slot < kSlots; ++slot) {
                const std::span<float, dsp::kQmfHop> emitted{
                    out[static_cast<std::size_t>(object)].data() + slot * dsp::kQmfHop,
                    static_cast<std::size_t>(dsp::kQmfHop)};
                if (!shape.present) {
                    std::uint8_t& silent = silent_[static_cast<std::size_t>(object)];
                    if (silent >= 10) {
                        continue;
                    }
                    ++silent;
                    object_real.fill(0.0F);
                    object_imag.fill(0.0F);
                    synthesis_[static_cast<std::size_t>(object)].pull(object_real, object_imag,
                                                                      emitted);
                    continue;
                }
                silent_[static_cast<std::size_t>(object)] = 0;
                const bool previous_frame = slot < dsp::kQmfDelaySlots;
                const float tail_frac =
                    static_cast<float>(dsp::kQmfSlotsPerFrame - dsp::kQmfDelaySlots + slot + 1) /
                    static_cast<float>(dsp::kQmfSlotsPerFrame);
                const int ts = slot - dsp::kQmfDelaySlots;
                const float frac_whole =
                    static_cast<float>(ts + 1) / static_cast<float>(joc::kQmfTimeslots);
                const float frac_first = static_cast<float>(ts + 1) / static_cast<float>(kHalfWindow);
                const float frac_second = static_cast<float>(ts - kHalfWindow + 1) /
                                          static_cast<float>(joc::kQmfTimeslots - kHalfWindow);
                for (int ch = 0; ch < kChannels; ++ch) {
                    const std::size_t row =
                        object_row + (static_cast<std::size_t>(ch) * kSub);
                    for (int band = 0; band < bands; ++band) {
                        const std::size_t index =
                            row + static_cast<std::size_t>(first_subband[static_cast<std::size_t>(band)]);
                        const float dq0 = dq(0, ch, band);
                        float m = 0.0F;
                        if (previous_frame) {
                            const float previous_val = has_previous ? previous_[index] : dq0;
                            const float older_val = has_older ? older_[index] : previous_val;
                            m = older_val + tail_frac * (previous_val - older_val);
                        } else if (!has_previous) {
                            m = shape.data_points > 1 ? dq(1, ch, band) : dq0;
                        } else if (!shape.steep) {
                            if (shape.data_points == 1) {
                                m = previous_[index] + frac_whole * (dq0 - previous_[index]);
                            } else if (ts < kHalfWindow) {
                                m = previous_[index] + frac_first * (dq0 - previous_[index]);
                            } else {
                                m = dq0 + frac_second * (dq(1, ch, band) - dq0);
                            }
                        } else if (ts < shape.offset_ts[0]) {
                            m = previous_[index];
                        } else if (shape.data_points == 1 || ts < shape.offset_ts[1]) {
                            m = dq0;
                        } else {
                            m = dq(1, ch, band);
                        }
                        band_mix[static_cast<std::size_t>(ch)][static_cast<std::size_t>(band)] = m;
                    }
                    for (int k = 0; k < dsp::kQmfSubbands; ++k) {
                        mix_rows[static_cast<std::size_t>(ch)][static_cast<std::size_t>(k)] =
                            band_mix[static_cast<std::size_t>(ch)]
                                    [mapping[static_cast<std::size_t>(k)]];
                    }
                }
                const float* const slot_spectra =
                    spectra.data() + (static_cast<std::size_t>(slot) * kChannels * 2U * kSub);
                for (std::size_t k = 0; k < kSub; ++k) {
                    float real = 0.0F;
                    float imag = 0.0F;
                    for (std::size_t c = 0; c < static_cast<std::size_t>(kChannels); ++c) {
                        const float m = mix_rows[c][k];
                        const float* const channel = slot_spectra + (c * 2U * kSub);
                        real += m * channel[k];
                        imag += m * channel[kSub + k];
                    }
                    object_real[k] = real;
                    object_imag[k] = imag;
                }
                synthesis_[static_cast<std::size_t>(object)].pull(object_real, object_imag,
                                                                  emitted);
            }
        }

        older_ = std::move(previous_);
        previous_.assign(previous_size, 0.0F);
        for (int object = 0; object < objects; ++object) {
            const auto shape = params.shape(object);
            if (!shape.present) {
                continue;
            }
            const auto& mapping =
                joc::kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];
            for (int ch = 0; ch < kChannels; ++ch) {
                for (int subband = 0; subband < dsp::kQmfSubbands; ++subband) {
                    const std::size_t index =
                        (static_cast<std::size_t>(object) * kChannels +
                         static_cast<std::size_t>(ch)) *
                            kSub +
                        static_cast<std::size_t>(subband);
                    previous_[index] = static_cast<float>(
                        params.at(object, shape.data_points - 1, ch,
                                  mapping[static_cast<std::size_t>(subband)]));
                }
            }
        }
        previous_objects_ = objects;
        return out;
    }

private:
    std::array<dsp::QmfAnalysisF, kChannels> analysis_{};
    std::vector<dsp::QmfSynthesisF> synthesis_{};
    std::vector<std::uint8_t> silent_{};
    std::vector<float> previous_{};
    std::vector<float> older_{};
    int previous_objects_ = 0;
};

// One call of the library against one of the reference, over `frames` frames of `slots` timeslots.
template <typename Reference, typename Call>
void run_frames(int frames, int slots, int first_seq_count, std::uint32_t seed, float scale,
                Reference& reference, joc::ReconstructionState& state, Call&& reference_call) {
    Noise noise{seed};
    std::array<std::vector<float>, kChannels> channels;
    std::vector<std::span<const float>> bed(kChannels);
    const int samples = slots * dsp::kQmfHop;
    for (int frame = 0; frame < frames; ++frame) {
        CAPTURE(frame, slots);
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            channels[ch].resize(static_cast<std::size_t>(samples));
            for (auto& sample : channels[ch]) {
                sample = noise.nextf() * scale;
            }
            bed[ch] = channels[ch];
        }
        const auto params = make_params(frame, frame == 0 ? first_seq_count : frame + 20, slots, noise);
        const auto expected = reference_call(reference, std::span<const std::span<const float>>(bed), params);
        const auto actual = joc::reconstruct(bed, params, state, true, true,
                                             iclforge::objects::oba::joc::Domain::kQmf);
        REQUIRE(actual.size() == expected.size());
        CHECK(identical(actual, expected));
    }
}

}  // namespace

TEST_CASE("the single precision QMF reconstruction matches the per band loop to the bit",
          "[atmos][joc][qmf]") {
    // Frame 0 is a splice (seq_count 0); the next five ramp from what the one before left.
    SingleReference reference;
    joc::ReconstructionState state;
    state.qmf_single_precision = true;
    run_frames(6, dsp::kQmfSlotsPerFrame, 0, 4711U, 0.5F, reference, state,
               [](SingleReference& r, std::span<const std::span<const float>> bed,
                  const joc::FrameParameters& params) { return r.reconstruct(bed, params); });
}

TEST_CASE("the single precision QMF reconstruction matches the per band loop mid stream",
          "[atmos][joc][qmf]") {
    // seq_count is not zero but there is nothing to ramp from: the flat rule, and the delayed
    // tail with the first data point standing in for both snapshots.
    SingleReference reference;
    joc::ReconstructionState state;
    state.qmf_single_precision = true;
    run_frames(4, dsp::kQmfSlotsPerFrame, 50, 1234U, 0.25F, reference, state,
               [](SingleReference& r, std::span<const std::span<const float>> bed,
                  const joc::FrameParameters& params) { return r.reconstruct(bed, params); });
}

TEST_CASE("the short syncframe QMF reconstruction matches the per coefficient loop to the bit",
          "[atmos][joc][qmf]") {
    // One, two and three blocks to a frame: four, eight and twelve timeslots, each with a splice
    // first and then frames that ramp. A short frame takes this function whichever precision the
    // state asks for, so both are run.
    for (const int slots : {4, 8, 12}) {
        for (const bool single : {false, true}) {
            CAPTURE(slots, single);
            ShortReference reference;
            joc::ReconstructionState state;
            state.qmf_single_precision = single;
            run_frames(6, slots, 0, 99U + static_cast<std::uint32_t>(slots), 0.5F, reference,
                       state,
                       [slots](ShortReference& r, std::span<const std::span<const float>> bed,
                               const joc::FrameParameters& params) {
                           return r.reconstruct(bed, params, slots);
                       });
        }
    }
}

TEST_CASE("the short syncframe QMF reconstruction matches the per coefficient loop mid stream",
          "[atmos][joc][qmf]") {
    for (const int slots : {4, 8, 12}) {
        CAPTURE(slots);
        ShortReference reference;
        joc::ReconstructionState state;
        run_frames(3, slots, 60, 5150U + static_cast<std::uint32_t>(slots), 0.25F, reference, state,
                   [slots](ShortReference& r, std::span<const std::span<const float>> bed,
                           const joc::FrameParameters& params) {
                       return r.reconstruct(bed, params, slots);
                   });
    }
}
