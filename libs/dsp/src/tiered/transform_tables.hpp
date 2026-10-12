#pragma once

#include <cstddef>
#include <span>
#include <type_traits>

#include "iclforge/dsp/tiered/real.hpp"
#include "iclforge/dsp/detail/complex.hpp"

// The inverse transform's tables at the float and fixed-point tiers, in flash
// (planning/ac4.md, the decoder's memory): for a block of N lines, the roots of the N/2-point
// FFT's passes in Fft's order, Pseudocode 60's pre-twiddle, the fixed tier's post-twiddle and
// KBD_LEFT(N). The values are what Fft, Imdct and TransformSet compute when they build their
// own (tools/generators/gen_ac4_transform_tables.py computes the doubles the same way, and
// libs/dsp/src/tiered/transform_tables.cpp narrows them as those classes do), so a transform reads
// the same bits from either. Built in for every length Table 186 lists at 44.1 and 48 kHz, which is
// every block of every frame length the decoder has: 2048, 1920, 1536, 1024, 960, 768, 512 and 384
// samples, each halved down to a sixteenth where Table 186 has the length. Any other length,
// and every length at double, builds its tables when it is constructed: in software floating
// point on a part with no double-precision unit, which took an ESP32-S3 0.4 s at the first frame
// of a stream of 1,920 or 1,536 samples a frame (planning/ac4.md, the converter warm-up).

namespace iclforge::dsp::tiered {

template <typename Real>
struct TransformTable {
    std::span<const Complex<Real>> fft_roots;
    std::span<const Complex<Real>> pre_twiddle;
    std::span<const Complex<Real>> post_twiddle;  // empty where Real is floating
    std::span<const Real> kbd_left;
};

// The tables for a block of `length` lines at this build's scalar, or null.
[[nodiscard]] const TransformTable<Real>* built_in_transform_table(std::size_t length) noexcept;

template <typename Value>
[[nodiscard]] const TransformTable<Value>* transform_table(std::size_t length) noexcept {
    if constexpr (std::is_same_v<Value, Real> && !std::is_same_v<Value, double>) {
        return built_in_transform_table(length);
    } else {
        (void)length;
        return nullptr;
    }
}

}  // namespace iclforge::dsp::tiered
