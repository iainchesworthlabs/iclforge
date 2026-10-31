#include <immintrin.h>

#include <cstddef>
#include <cstdint>

#include "crypto/sha256_constants.hpp"
#include "crypto/sha256_hw.hpp"

// SHA-256's compression function on the x86-64 SHA extensions (SHA-NI: Intel from Ice Lake and
// Goldmont, AMD from Zen). Selected by libs/base/CMakeLists.txt together with the compile flags
// the instructions need (-msha -msse4.1 -mssse3 on GCC/Clang; nothing on MSVC, whose intrinsics
// are not gated on a flag), and the ONLY file in the library built that way, so the rest of
// iclforge::base can never be compiled to use an instruction the deployment CPU may lack. The
// probe that says the CPU has them is the sibling hw/x86_64-{msvc,builtin}/sha256_probe.cpp.
//
// The instruction pair sha256rnds2 does two rounds and keeps the working variables as ABEF and
// CDGH rather than a..h, and sha256msg1/msg2 extend the message schedule four words at a time.
// The loop below is the schedule from FIPS 180-4 section 6.2.2 written on those: with V[j] the
// four words W[4j..4j+3], V[j] = msg2(msg1(V[j-4], V[j-3]) + (V[j-1]:V[j-2] >> 4 bytes), V[j-1]).

namespace iclforge::internal::crypto::hw {

void compress(std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) noexcept {
    // Big-endian message words to little-endian lanes.
    const __m128i byte_swap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);

    // The state words in memory are a..h; the instructions want ABEF and CDGH.
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state));           // DCBA
    __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4));    // HGFE
    tmp = _mm_shuffle_epi32(tmp, 0xB1);                                               // CDAB
    state1 = _mm_shuffle_epi32(state1, 0x1B);                                         // EFGH
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);                                 // ABEF
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);                                      // CDGH

    for (; blocks != 0; --blocks, data += 64) {
        const __m128i abef_save = state0;
        const __m128i cdgh_save = state1;

        // w[g & 3] is V[g], the schedule words for rounds 4g..4g+3, and w[(g + 1..3) & 3] the next
        // three vectors: a window of four that slides along the 16 groups of a block.
        __m128i w[4];
        for (int i = 0; i < 4; ++i) {
            w[i] = _mm_shuffle_epi8(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), byte_swap);
        }

        for (int g = 0; g < 16; ++g) {
            __m128i msg = _mm_add_epi32(
                w[g & 3], _mm_loadu_si128(reinterpret_cast<const __m128i*>(
                              kSha256RoundConstants.data() + 4 * g)));
            state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
            msg = _mm_shuffle_epi32(msg, 0x0E);
            state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

            if (g < 12) {
                // V[g + 4] takes the place of V[g], which has just been used.
                __m128i next = _mm_sha256msg1_epu32(w[g & 3], w[(g + 1) & 3]);
                next = _mm_add_epi32(next, _mm_alignr_epi8(w[(g + 3) & 3], w[(g + 2) & 3], 4));
                w[g & 3] = _mm_sha256msg2_epu32(next, w[(g + 3) & 3]);
            }
        }

        state0 = _mm_add_epi32(state0, abef_save);
        state1 = _mm_add_epi32(state1, cdgh_save);
    }

    tmp = _mm_shuffle_epi32(state0, 0x1B);        // FEBA
    state1 = _mm_shuffle_epi32(state1, 0xB1);     // DCHG
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);  // DCBA
    state1 = _mm_alignr_epi8(state1, tmp, 8);     // HGFE
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), state1);
}

}  // namespace iclforge::internal::crypto::hw
