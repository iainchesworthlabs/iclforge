#pragma once

#include <cstdint>
#include <span>

// ---------------------------------------------------------------------------
// The AVX2 body of libs/truehd/src/encoder/coding.cpp's coded_bits, the count of the bits a block
// of residuals takes under one coding, which is 42 % of the instructions of a 5.1 encode
// (planning/truehd-parity-performance.md, section 4). Dispatched behind
// iclforge::internal::cpu::has_avx2() at the one call site, declared with plain signatures so that
// coding.cpp, compiled without any AVX2 flag, can call it across the object-library boundary
// without seeing an intrinsic: the mechanism libs/ac3/src/internal/avx2 uses for the MDCT.
//
// Exact integer arithmetic, so the result is the scalar's for every input rather than close to it:
// the lanes are the same additions, shifts and table reads, in a different order of summation that
// integers do not care about. Where an input is outside what 32-bit lanes hold, a function says so
// (kUnhandled) and the caller uses the scalar body, whose 64-bit arithmetic it matches everywhere
// else.
// ---------------------------------------------------------------------------

namespace iclforge::truehd::internal::avx2 {

// Returned when a residual is outside +-2^30, the range in which `residual + centre` fits 32 bits
// for every centre the coder can produce (|centre| < 2^27). The encoder's own residuals are inside
// it (compute_residual refuses a larger one), so the scalar body runs for no real block.
inline constexpr int kUnhandled = -1;

// coded_bits for a Huffman codebook (huff_type 1 to 3): for each residual, symbol =
// (residual + centre) >> lb, which has to lie in [min_symbol, max_symbol] and whose code length
// (lengths[symbol - min_symbol], max_symbol - min_symbol + 1 of them) plus lb has to be at most
// max_sample_bits; the sum of length + lb, or `infeasible` if a residual fails either.
[[nodiscard]] int huffman_bits(std::span<const std::int32_t> residuals, std::int32_t centre, int lb,
                               int min_symbol, int max_symbol, const std::uint8_t* lengths,
                               int max_sample_bits, int infeasible);

// coded_bits for raw bits (huff_type 0): every residual + centre has to lie in [0, 2^lb); the bits
// are lb a residual, or `infeasible`.
[[nodiscard]] int raw_bits(std::span<const std::int32_t> residuals, std::int32_t centre, int lb,
                           int infeasible);

}  // namespace iclforge::truehd::internal::avx2
