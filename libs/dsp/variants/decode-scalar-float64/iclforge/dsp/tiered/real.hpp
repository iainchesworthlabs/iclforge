#pragma once

// The type the tiered kernels (libs/dsp/src/tiered) and AC-4's own (libs/ac4/src/core) are
// explicitly instantiated at - the `template class Foo<Real>;` lines - in its DOUBLE variant. The
// float and fixed-point variants are the identically-pathed headers beside this directory;
// libs/ac4/CMakeLists.txt, which builds the kernels, picks the directory from
// ICLFORGE_DECODE_SCALAR, the root's one option for every codec, so no source file asks which it
// is with a preprocessor conditional (tools/checks/check_platform_macros.ps1's rule).
//
// This is not iclforge::ac3::internal::decode_scalar_t's mechanism: that name is used directly,
// as a concrete type, throughout AC-3's decoder; these kernels stay templated on `Real`, and
// `Real` here is the unqualified name every explicit-instantiation line resolves through
// ordinary enclosing-namespace lookup inside iclforge::dsp::tiered (iclforge::ac4::detail names
// it too, libs/ac4/src/iclforge/ac4/detail/real.hpp).
//
// double: every build outside the ESP32 boards, and the reference every other tier is measured
// against (planning/arithmetic-tiers.md).

namespace iclforge::dsp::tiered {

using Real = double;

}  // namespace iclforge::dsp::tiered

// Explicit instantiations a translation unit adds at double beside its
// `template class Foo<Real>;` when Real is not double: the AC-4 encoder (libs/ac4/src/encoder)
// always runs at double, decision 34 (planning/ac4.md), and the kernels
// are one set of objects the decoder and the encoder both call, so each one the encoder calls is
// instantiated at both. Here Real is double, so the line above is already that
// instantiation and a second one is ill-formed: this expands to nothing. It
// takes the instantiation as its argument, rather than a preprocessor
// conditional around it, for tools/checks/check_platform_macros.ps1's rule.
#define ICLFORGE_DSP_ALSO_AT_DOUBLE(...)
