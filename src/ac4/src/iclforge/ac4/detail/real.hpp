#pragma once

#include "iclforge/dsp/tiered/real.hpp"

// The scalar AC-4's kernels are instantiated at: the decode tier's, which dsp's tiered kernels
// take (iclforge/dsp/tiered/real.hpp, from src/dsp/variants/decode-scalar-<tier>/), so that the
// codec and the transforms it calls are one tier.

namespace iclforge::ac4::detail {

using Real = dsp::tiered::Real;

}  // namespace iclforge::ac4::detail

// The encoder's double instantiations beside the tier's (dsp's ICLFORGE_DSP_ALSO_AT_DOUBLE).
#define ICLFORGE_AC4_ALSO_AT_DOUBLE(...) ICLFORGE_DSP_ALSO_AT_DOUBLE(__VA_ARGS__)
