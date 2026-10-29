#include "../coded_bits_avx2.hpp"

#include <utility>

// The ICLFORGE_AVX2=OFF and non-x86-64 half of coded_bits_avx2.hpp, compiled straight into the
// library's objects with no AVX2 flag: there is no AVX2 code here to need one. The call site in
// encoder/coding.cpp sits behind iclforge::internal::cpu::has_avx2(), which cannot answer true in
// a configuration that has no AVX2 tier (libs/base/CMakeLists.txt selects the probe that always
// says no), so these bodies cannot be reached and std::unreachable() is the right one, as in
// libs/ac3/src/internal/avx2/none.

namespace iclforge::truehd::internal::avx2 {

int huffman_bits(std::span<const std::int32_t> /*residuals*/, std::int32_t /*centre*/, int /*lb*/,
                 int /*min_symbol*/, int /*max_symbol*/, const std::uint8_t* /*lengths*/,
                 int /*max_sample_bits*/, int /*infeasible*/) {
    std::unreachable();
}

int raw_bits(std::span<const std::int32_t> /*residuals*/, std::int32_t /*centre*/, int /*lb*/,
             int /*infeasible*/) {
    std::unreachable();
}

}  // namespace iclforge::truehd::internal::avx2
