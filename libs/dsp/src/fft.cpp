#include "iclforge/dsp/fft.hpp"

#include <cstddef>
#include <span>

#include "iclforge/base/detail/simd.hpp"

#include "iclforge/dsp/detail/fft_stockham.hpp"

namespace iclforge {

namespace {

constexpr auto kLength = static_cast<std::size_t>(kDftLength);

const dsp::fft::StockhamTables<kLength>& tables() {
    static const dsp::fft::StockhamTables<kLength> t;
    return t;
}

// The float factors, built on first use like the double ones: a build whose
// decoder never takes the float form never constructs them, and a
// minimum-footprint decoder that only ever takes it lets --gc-sections drop
// the double table with the double function.
const dsp::fft::StockhamTables<kLength, float>& tables_f32() {
    static const dsp::fft::StockhamTables<kLength, float> t;
    return t;
}

}  // namespace

void dft512(std::span<const double, kDftLength> real_in,
           std::span<const double, kDftLength> imag_in, std::span<double, kDftLength> real_out,
           std::span<double, kDftLength> imag_out) {
    // The output spans are the transform's own (they were never permitted to alias the inputs).
    for (std::size_t n = 0; n < kLength; ++n) {
        real_out[n] = real_in[n];
        imag_out[n] = imag_in[n];
    }
    dsp::fft::stockham_forward<kLength, double, double>(tables(), real_out, imag_out);
    // The spec sum's own 1/N normalisation (see fft.hpp), two bins at a time
    // through the arch seam (SIMD kernels). Multiplication by the reciprocal
    // rather than division: N is 512, so 1/N is exactly representable and
    // x * (1/512) and x / 512 are the correctly-rounded result of the same
    // exact real number - identical for every input, denormal results
    // included. The seam carries no divide for exactly this reason (a
    // general reciprocal-multiply would NOT be safe, and offering the
    // operation would invite one).
    constexpr double kInvN = 1.0 / static_cast<double>(kDftLength);
    const auto inv = internal::arch::f64x2::broadcast(kInvN);
    double* const rp = real_out.data();
    double* const ip = imag_out.data();
    for (std::size_t k = 0; k < kLength; k += 2) {
        (internal::arch::f64x2::load(rp + k) * inv).store(rp + k);
        (internal::arch::f64x2::load(ip + k) * inv).store(ip + k);
    }
}

void dft512(std::span<const float, kDftLength> real_in, std::span<const float, kDftLength> imag_in,
            std::span<float, kDftLength> real_out, std::span<float, kDftLength> imag_out) {
    // The same shape as the double form above: copy-in, the transform, then
    // the spec sum's 1/N. A plain loop for the scale rather than the arch
    // seam - on the part this form exists for the seam is scalar anyway, and
    // 1,024 multiplies are not where a frame's time goes.
    for (std::size_t n = 0; n < kLength; ++n) {
        real_out[n] = real_in[n];
        imag_out[n] = imag_in[n];
    }
    dsp::fft::stockham_forward<kLength, float, float>(tables_f32(), real_out, imag_out);
    constexpr float kInvN = 1.0F / static_cast<float>(kDftLength);
    for (std::size_t k = 0; k < kLength; ++k) {
        real_out[k] *= kInvN;
        imag_out[k] *= kInvN;
    }
}

}  // namespace iclforge
