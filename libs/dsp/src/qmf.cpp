#include "iclforge/dsp/qmf.hpp"

#include <array>
#include <cstddef>
#include <span>

#include "qmf_prototype.hpp"
#include "iclforge/dsp/detail/complex.hpp"
#include "tiered/qmf.hpp"
#include "tiered/qmf_constants.hpp"
#include "tiered/qmf_slot.hpp"

namespace iclforge::dsp {

namespace {

using Complex = tiered::Complex<double>;

constexpr std::size_t kSubbandCount = static_cast<std::size_t>(kQmfSubbands);
constexpr std::size_t kTapCount = static_cast<std::size_t>(kQmfTaps);

// The prototype as the engine reads it (tiered/qmf_kernels.hpp). The fold alternates sign every
// 2M = 128 taps, which this bank's modulation exp(-i pi (k + 1/2) m / M) being anti-periodic in m
// with period 2M asks for, and which QWIN carries in its own values. The engine's delay line holds
// the newest sample first and this bank's window runs from the oldest, so the analysis reads the
// prototype backwards; synthesis, whose delay line is the overlap in time order, forwards.
[[nodiscard]] constexpr double fold_sign(std::size_t j) noexcept {
    return (j / 128) % 2 == 0 ? 1.0 : -1.0;
}

consteval std::array<double, kTapCount> make_analysis_window() {
    std::array<double, kTapCount> w{};
    for (std::size_t j = 0; j < kTapCount; ++j) {
        w[j] = fold_sign(j) * kQmfPrototype[kTapCount - 1 - j];
    }
    return w;
}

consteval std::array<double, kTapCount> make_synthesis_window() {
    std::array<double, kTapCount> w{};
    for (std::size_t j = 0; j < kTapCount; ++j) {
        w[j] = fold_sign(j) * kQmfPrototype[j];
    }
    return w;
}

constexpr auto kAnalysisWindow = make_analysis_window();
constexpr auto kSynthesisWindow = make_synthesis_window();

// The engine modulates by e^(i pi (2k + 1)(2n - 1) / 256) over a window that runs newest first;
// this bank by e^(-i pi (2k + 1) m / 128) over one that runs oldest first. With n = 127 - m the
// two differ by a turn of each subband k: its output times e^(-i pi (2k + 1) 253 / 256) is this
// bank's, and this bank's synthesis input times e^(+i pi (2k + 1) 255 / 256) is the engine's.
// Each a whole number of units of pi / 256, so each factor is one rounded value
// (tiered/qmf_constants.hpp).
struct Turns {
    std::array<double, kSubbandCount> analysis_re{};
    std::array<double, kSubbandCount> analysis_im{};
    std::array<double, kSubbandCount> synthesis_re{};
    std::array<double, kSubbandCount> synthesis_im{};
};

consteval Turns make_turns() {
    Turns t{};
    for (std::size_t k = 0; k < kSubbandCount; ++k) {
        const auto odd = static_cast<long long>(2 * k + 1);
        t.analysis_re[k] = tiered::qmf::cos_units(-253 * odd);
        t.analysis_im[k] = tiered::qmf::sin_units(-253 * odd);
        t.synthesis_re[k] = tiered::qmf::cos_units(255 * odd);
        t.synthesis_im[k] = tiered::qmf::sin_units(255 * odd);
    }
    return t;
}

constexpr Turns kTurns = make_turns();

}  // namespace

std::span<const double, kQmfTaps> qmf_prototype() {
    return std::span<const double, kQmfTaps>{kQmfPrototype};
}

void QmfAnalysis::reset() {
    history_.fill(0.0);
    head_ = 0;
}

void QmfAnalysis::push(std::span<const float, kQmfHop> block,
                       std::span<double, kQmfSubbands> real,
                       std::span<double, kQmfSubbands> imag) {
    std::array<double, kSubbandCount> pcm{};
    for (std::size_t n = 0; n < kSubbandCount; ++n) {
        pcm[n] = static_cast<double>(block[n]);
    }
    std::array<Complex, kSubbandCount> slot{};
    tiered::QmfScratch<double> scratch{};
    tiered::qmf::analysis_slot(history_.data(), head_, pcm.data(), kAnalysisWindow.data(),
                               slot.data(), scratch);
    for (std::size_t k = 0; k < kSubbandCount; ++k) {
        const double wr = kTurns.analysis_re[k];
        const double wi = kTurns.analysis_im[k];
        real[k] = slot[k].re * wr - slot[k].im * wi;
        imag[k] = slot[k].re * wi + slot[k].im * wr;
    }
}

void QmfSynthesis::reset() {
    overlap_.fill(0.0);
    head_ = 0;
}

void QmfSynthesis::pull(std::span<const double, kQmfSubbands> real,
                        std::span<const double, kQmfSubbands> imag,
                        std::span<float, kQmfHop> out) {
    std::array<Complex, kSubbandCount> slot{};
    for (std::size_t k = 0; k < kSubbandCount; ++k) {
        const double wr = kTurns.synthesis_re[k];
        const double wi = kTurns.synthesis_im[k];
        slot[k] = Complex{real[k] * wr - imag[k] * wi, real[k] * wi + imag[k] * wr};
    }
    std::array<double, kSubbandCount> pcm{};
    tiered::QmfScratch<double> scratch{};
    tiered::qmf::synthesis_slot(slot.data(), overlap_.data(), head_, kSynthesisWindow.data(),
                                pcm.data(), scratch);
    for (std::size_t n = 0; n < kSubbandCount; ++n) {
        out[n] = static_cast<float>(pcm[n]);
    }
}

}  // namespace iclforge::dsp
