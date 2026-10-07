#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "iclforge/dsp/qmf.hpp"

// planning/consolidation.md decision 21: JOC's bank runs on the AC-4 banks' engine with its own
// prototype. This holds it to the bank as it was before, written out directly - the window
// folded with its alternating sign over the history oldest first, the odd-stacked sum
// exp(-i pi (2k + 1) m / 128), the synthesis's real part, unfold and overlap-add - so that the
// window, the turns between the two modulations and the 576-sample alignment are all checked, and
// only the order the sums are taken in may differ.

namespace {

using iclforge::dsp::kQmfHop;
using iclforge::dsp::kQmfSubbands;
using iclforge::dsp::kQmfTaps;

constexpr std::size_t kM = kQmfSubbands;
constexpr std::size_t kL = kQmfTaps;
constexpr std::size_t kFold = 2 * kM;

double sign(std::size_t n) { return (n / kFold) % 2 == 0 ? 1.0 : -1.0; }

struct DirectAnalysis {
    std::array<double, kL> history{};
    void push(std::span<const float> block, std::span<double> re, std::span<double> im) {
        std::rotate(history.begin(), history.begin() + kM, history.end());
        for (std::size_t n = 0; n < kM; ++n) {
            history[kL - kM + n] = block[n];
        }
        const auto proto = iclforge::dsp::qmf_prototype();
        std::array<double, kFold> folded{};
        for (std::size_t n = 0; n < kL; ++n) {
            folded[n % kFold] += sign(n) * proto[n] * history[n];
        }
        for (std::size_t k = 0; k < kM; ++k) {
            double r = 0.0;
            double i = 0.0;
            for (std::size_t m = 0; m < kFold; ++m) {
                const double a = -std::numbers::pi * static_cast<double>((2 * k + 1) * m) / 128.0;
                r += folded[m] * std::cos(a);
                i += folded[m] * std::sin(a);
            }
            re[k] = r;
            im[k] = i;
        }
    }
};

struct DirectSynthesis {
    std::array<double, kL> overlap{};
    void pull(std::span<const double> re, std::span<const double> im, std::span<double> out) {
        const auto proto = iclforge::dsp::qmf_prototype();
        std::array<double, kFold> folded{};
        for (std::size_t m = 0; m < kFold; ++m) {
            double sum = 0.0;
            for (std::size_t k = 0; k < kM; ++k) {
                const double a = std::numbers::pi * static_cast<double>((2 * k + 1) * m) / 128.0;
                sum += re[k] * std::cos(a) - im[k] * std::sin(a);
            }
            folded[m] = sum / static_cast<double>(kM);
        }
        for (std::size_t n = 0; n < kL; ++n) {
            overlap[n] += sign(n) * proto[n] * folded[n % kFold];
        }
        for (std::size_t n = 0; n < kM; ++n) {
            out[n] = overlap[n];
        }
        std::rotate(overlap.begin(), overlap.begin() + kM, overlap.end());
        std::fill(overlap.end() - kM, overlap.end(), 0.0);
    }
};

}  // namespace

TEST_CASE("JOC's QMF analysis on the shared engine is the bank written out", "[dsp][qmf]") {
    std::mt19937 rng(103420);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    iclforge::dsp::QmfAnalysis bank;
    DirectAnalysis direct;
    double worst = 0.0;
    for (int slot = 0; slot < 40; ++slot) {
        std::array<float, kQmfHop> block{};
        for (auto& v : block) {
            v = dist(rng);
        }
        std::array<double, kQmfSubbands> re{}, im{}, dre{}, dim{};
        bank.push(block, re, im);
        direct.push(block, dre, dim);
        for (std::size_t k = 0; k < kM; ++k) {
            worst = std::max({worst, std::abs(re[k] - dre[k]), std::abs(im[k] - dim[k])});
        }
    }
    CAPTURE(worst);
    CHECK(worst < 1e-12);
}

TEST_CASE("JOC's QMF synthesis on the shared engine is the bank written out", "[dsp][qmf]") {
    std::mt19937 rng(103421);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    iclforge::dsp::QmfSynthesis bank;
    DirectSynthesis direct;
    double worst = 0.0;
    for (int slot = 0; slot < 40; ++slot) {
        std::array<double, kQmfSubbands> re{}, im{};
        for (std::size_t k = 0; k < kM; ++k) {
            re[k] = dist(rng);
            im[k] = dist(rng);
        }
        std::array<float, kQmfHop> out{};
        std::array<double, kQmfHop> expected{};
        bank.pull(re, im, out);
        direct.pull(re, im, expected);
        for (std::size_t n = 0; n < kM; ++n) {
            // The bank's output is float; its error is the rounding to float.
            worst = std::max(worst, std::abs(static_cast<double>(out[n]) - expected[n]));
        }
    }
    CAPTURE(worst);
    CHECK(worst < 1e-6);
}
