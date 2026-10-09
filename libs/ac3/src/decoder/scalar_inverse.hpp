#pragma once

#include <algorithm>
#include <array>
#include <type_traits>

#include "iclforge/ac3/core/mdct.hpp"
#include "iclforge/base/arithmetic/fixed32.hpp"
#include "mdct_fixed.hpp"
#include "iclforge/ac3/detail/decode_scalar.hpp"
#include "iclforge/ac3/detail/profile.hpp"

// The §7.9.4 inverse pair, selected by the scalar type the decoder stores its
// coefficients in (iclforge::ac3::internal::decode_scalar_t, float32 in the minimal profile).
// Shared by both decoders - decoder.cpp's AC-3 and eac3_decoder.cpp's Annex E -
// because both make exactly this choice at exactly this point.
//
// It is a TEMPLATE for one specific reason, and not for generality: `if
// constexpr` only discards the untaken branch inside a template. In an ordinary
// function both arms are still fully type-checked, so the double-only call
// would fail to compile in a float32 build even though it could never run -
// which is the trap src/internal/cpu/minimal/cpu_features.cpp's header records
// ("the discarded branch of a non-template is still semantically checked and
// its callees still ODR-used").
//
// The float arm takes `fast` too. The float32 inverses have no direct form of
// their own (the direct form is the spec's own evaluation and stays double), so
// fast_imdct=false widens the block to double, runs the direct form and
// narrows the result - the same shape oba/joc.cpp's inverse_512 uses. Without
// it a full build configured ICLFORGE_DECODE_SCALAR=float served the fast
// transform for a request of the direct one. A build without the direct form
// (the minimum-footprint profile) has already refused fast_imdct=false with
// kNoReferenceTransform long before reaching here, and compiles none of this.
// The fixed-point tier has one form and ignores `fast`.

namespace iclforge::ac3::internal {

template <typename Scalar>
void inverse_transform_into(const std::array<Scalar, 256>& coeffs, std::array<Scalar, 512>& x,
                            bool short_block, bool fast) {
    if constexpr (std::is_same_v<Scalar, iclforge::internal::Fixed32>) {
        // The fixed-point tier's own pair (mdct_fixed.hpp), integer end to
        // end: the coefficients arrive under their block exponent
        // (block_norm.hpp), which is what keeps them inside the pair's
        // precondition, and the output leaves under the same exponent for
        // the overlap-add to apply. No `fast`: there is one form.
        (void)fast;
        if (short_block) {
            imdct256_pair_windowed_fixed(coeffs, x);
        } else {
            imdct512_windowed_fixed(coeffs, x);
        }
    } else if constexpr (std::is_same_v<Scalar, float>) {
        if constexpr (kReferenceTransformAvailable) {
            if (!fast) {
                std::array<double, 256> wide_coeffs{};
                std::array<double, 512> wide_x{};
                std::ranges::copy(coeffs, wide_coeffs.begin());
                if (short_block) {
                    imdct256_pair_windowed(wide_coeffs, wide_x, /*fast=*/false);
                } else {
                    imdct512_windowed(wide_coeffs, wide_x, /*fast=*/false);
                }
                std::ranges::transform(wide_x, x.begin(),
                                       [](double v) { return static_cast<float>(v); });
                return;
            }
        } else {
            (void)fast;
        }
        if (short_block) {
            imdct256_pair_windowed(coeffs, x);
        } else {
            imdct512_windowed(coeffs, x);
        }
    } else {
        if (short_block) {
            imdct256_pair_windowed(coeffs, x, fast);
        } else {
            imdct512_windowed(coeffs, x, fast);
        }
    }
}

}  // namespace iclforge::ac3::internal
