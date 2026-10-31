#pragma once

// The prototype's view of the analysis a board runs: iclforge/dsp/spectrum.hpp, the code of the firmware, unchanged. What is here is only
// the floating-point ring the prototype's made-up audio is pushed into, and the same names the first round's views were written with.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "iclforge/dsp/spectrum.hpp"

namespace meters {

namespace dsp = iclforge::dsp::spectrum;

using dsp::Ballistic;
using dsp::PowerAverage;
using dsp::band_high_hz;
using dsp::band_low_hz;
using dsp::kBands;
using dsp::kLowBands;
using dsp::kRate;
using dsp::kSilentDb;
using dsp::to_db;

inline constexpr std::size_t kRingLength = dsp::kHistory;
inline constexpr auto& kCentre = dsp::kCentreHz;

namespace detail {

// The newest `kRingLength` samples of a stream, as the 16-bit integers the player's ring holds.
class FloatRing {
   public:
    void push(std::span<const float> mono) {
        for (const float v : mono) {
            ring_[head_] = v;
            head_ = (head_ + 1) % kRingLength;
        }
    }

    void history(std::array<std::int16_t, kRingLength>& out) const {
        for (std::size_t n = 0; n < kRingLength; ++n) {
            const float v = std::clamp(ring_[(head_ + n) % kRingLength], -1.0F, 1.0F);
            out[n] = static_cast<std::int16_t>(std::lround(v * 32767.0F));
        }
    }

   private:
    std::vector<float> ring_ = std::vector<float>(kRingLength, 0.0F);
    std::size_t head_ = 0;
};

}  // namespace detail

class Analyzer {
   public:
    void push(std::span<const float> mono) { ring_.push(mono); }
    void analyze(std::array<float, kBands>& db) {
        ring_.history(history_);
        analyzer_->analyze(history_, db);
    }

   private:
    detail::FloatRing ring_;
    std::array<std::int16_t, kRingLength> history_{};
    std::unique_ptr<dsp::Analyzer> analyzer_ = std::make_unique<dsp::Analyzer>();
};

// One 8,192-point FFT of the same samples, for the tuning program to hold the analysis against.
class Reference {
   public:
    void push(std::span<const float> mono) { ring_.push(mono); }
    void analyze(std::array<float, kBands>& db) {
        ring_.history(history_);
        reference_->analyze(history_, db);
    }

   private:
    detail::FloatRing ring_;
    std::array<std::int16_t, kRingLength> history_{};
    std::unique_ptr<dsp::Reference> reference_ = std::make_unique<dsp::Reference>();
};

}  // namespace meters
