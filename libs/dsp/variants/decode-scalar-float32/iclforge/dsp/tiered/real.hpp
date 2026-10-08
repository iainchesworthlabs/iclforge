#pragma once

// The FLOAT variant of the tiered kernels' explicit-instantiation scalar. See the
// double variant for what this seam is and
// why it is not iclforge::ac3::internal::decode_scalar_t's mechanism.
//
// float: the ESP32-P4 and -S3, which have a single-precision FPU
// (planning/ac4.md, "The ESP32"). ICLFORGE_DECODE_SCALAR=float's build compiles
// the kernels with -Wdouble-promotion as an error (src/ac4/CMakeLists.txt),
// so a float value implicitly widening to double anywhere in these kernels is
// a build failure rather than a silent, unmeasured cost - the same promise
// iclforge::ac3's own float decode path makes.

namespace iclforge::dsp::tiered {

using Real = float;

}  // namespace iclforge::dsp::tiered

// The float variant of ICLFORGE_DSP_ALSO_AT_DOUBLE (see the double variant): Real is float here,
// so the encoder's double instantiations are extra, and this passes them through.
#define ICLFORGE_DSP_ALSO_AT_DOUBLE(...) __VA_ARGS__
