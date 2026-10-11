#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/ac3/oba/joc.hpp"
#include "iclforge/dsp/qmf.hpp"

// The single-precision QMF reconstruction (ReconstructionState::qmf_single_precision) is the
// reference reconstruction in float: the same ramp, the same sum, the same banks. It exists
// because a part whose FPU is single precision runs the double one in software. These tests hold
// it to the double one - to the arithmetic, over every rule of the ramp (flat, smooth over one
// and two data points, steep over one and two, the delayed tail, an absent object) and over a
// run of frames, so that what the previous frame leaves for the next is checked too.

namespace {

namespace joc = iclforge::ac3::oba::joc;

struct Noise {
    std::uint32_t state;
    float next() {
        state = (state * 1664525U) + 1013904223U;
        return (static_cast<float>(state >> 8) / 8388608.0F - 1.0F);
    }
};

joc::FrameParameters make_params(int frame, Noise& noise) {
    joc::FrameParameters params;
    params.objects = 4;
    params.channels = joc::kNumChannels5X;
    params.seq_count = frame;  // 0: no ramp to start from
    params.num_bands_idx = 4;
    params.shapes.resize(4);
    // smooth, one data point
    params.shapes[0] = {.present = true, .num_bands_idx = 4, .steep = false, .data_points = 1};
    // smooth, two data points, another band count
    params.shapes[1] = {.present = true, .num_bands_idx = 5, .steep = false, .data_points = 2};
    // steep, one data point, a step at slot 8
    params.shapes[2] = {.present = true, .num_bands_idx = 3, .steep = true, .data_points = 1,
                        .offset_ts = {8, 0}};
    // steep, two data points, steps at 6 and 16; absent in the fourth frame
    params.shapes[3] = {.present = frame != 3, .num_bands_idx = 6, .steep = true, .data_points = 2,
                        .offset_ts = {6, 16}};
    params.matrix.assign(params.coefficient_count(), 0.0);
    for (auto& value : params.matrix) {
        value = static_cast<double>(noise.next()) * 1.5;
    }
    return params;
}

double snr_db(const std::vector<std::vector<float>>& reference,
              const std::vector<std::vector<float>>& test) {
    double signal = 0.0;
    double error = 0.0;
    for (std::size_t o = 0; o < reference.size(); ++o) {
        for (std::size_t i = 0; i < reference[o].size(); ++i) {
            const double r = static_cast<double>(reference[o][i]);
            const double d = r - static_cast<double>(test[o][i]);
            signal += r * r;
            error += d * d;
        }
    }
    return 10.0 * std::log10(signal / std::max(error, 1e-30));
}

}  // namespace

TEST_CASE("Single-precision QMF reconstruction follows the double reconstruction", "[joc][qmf]") {
    Noise noise{20260707};
    joc::ReconstructionState reference;
    joc::ReconstructionState single;
    single.qmf_single_precision = true;
    CHECK_FALSE(reference.qmf_single_precision);

    std::array<std::vector<float>, joc::kNumChannels5X> bed_samples;
    for (int frame = 0; frame < 8; ++frame) {
        const joc::FrameParameters params = make_params(frame, noise);
        std::array<std::span<const float>, joc::kNumChannels5X> bed{};
        for (std::size_t ch = 0; ch < bed_samples.size(); ++ch) {
            bed_samples[ch].resize(1536);
            for (std::size_t i = 0; i < 1536; ++i) {
                const double n = static_cast<double>(frame * 1536 + static_cast<int>(i));
                bed_samples[ch][i] =
                    0.25F * noise.next() +
                    0.25F * static_cast<float>(std::sin(n * (0.03 + 0.011 * static_cast<double>(ch))));
            }
            bed[ch] = bed_samples[ch];
        }
        const auto expected = joc::reconstruct(bed, params, reference);
        const auto got = joc::reconstruct(bed, params, single);
        REQUIRE(got.size() == expected.size());
        CAPTURE(frame);
        REQUIRE(got.size() == 4);
        for (std::size_t o = 0; o < got.size(); ++o) {
            REQUIRE(got[o].size() == expected[o].size());
        }
        // float rounding of 64-point transforms, ten-tap windows and five-term sums: about 140 dB
        // under the signal; 110 leaves a wide margin and any rule or state mistake is far below it.
        CHECK(snr_db(expected, got) > 110.0);
    }
}

TEST_CASE("The single-precision bank pair reconstructs what the double pair does", "[qmf]") {
    iclforge::dsp::QmfAnalysis analysis;
    iclforge::dsp::QmfSynthesis synthesis;
    iclforge::dsp::QmfAnalysisF analysis_f;
    iclforge::dsp::QmfSynthesisF synthesis_f;
    Noise noise{99};
    std::array<float, 64> block{};
    std::array<double, 64> real{};
    std::array<double, 64> imag{};
    std::array<float, 64> real_f{};
    std::array<float, 64> imag_f{};
    std::array<float, 64> out{};
    std::array<float, 64> out_f{};
    double signal = 0.0;
    double error = 0.0;
    double sub_signal = 0.0;
    double sub_error = 0.0;
    for (int slot = 0; slot < 200; ++slot) {
        for (auto& x : block) {
            x = 0.3F * noise.next();
        }
        analysis.push(block, real, imag);
        analysis_f.push(block, real_f, imag_f);
        for (std::size_t k = 0; k < 64; ++k) {
            sub_signal += real[k] * real[k] + imag[k] * imag[k];
            const double dr = real[k] - static_cast<double>(real_f[k]);
            const double di = imag[k] - static_cast<double>(imag_f[k]);
            sub_error += dr * dr + di * di;
        }
        synthesis.pull(real, imag, out);
        synthesis_f.pull(real_f, imag_f, out_f);
        for (std::size_t n = 0; n < 64; ++n) {
            const double r = static_cast<double>(out[n]);
            const double d = r - static_cast<double>(out_f[n]);
            signal += r * r;
            error += d * d;
        }
    }
    CHECK(10.0 * std::log10(sub_signal / std::max(sub_error, 1e-30)) > 120.0);
    CHECK(10.0 * std::log10(signal / std::max(error, 1e-30)) > 120.0);
}
