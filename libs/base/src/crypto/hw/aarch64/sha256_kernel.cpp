#include <arm_neon.h>

#include <cstddef>
#include <cstdint>

#include "crypto/sha256_constants.hpp"
#include "crypto/sha256_hw.hpp"

// SHA-256's compression function on the ARMv8 Cryptographic Extension's SHA2 instructions
// (vsha256h / vsha256h2 for the rounds, vsha256su0 / vsha256su1 for the message schedule).
// Selected by libs/base/CMakeLists.txt with -march=armv8-a+crypto on GCC/Clang; MSVC's ARM64
// intrinsics are not gated on a flag. The ONLY file in the library built that way. The probe that
// says the CPU has them is the sibling hw/aarch64-{linux,macos,windows}/sha256_probe.cpp.
//
// Here the state stays a..h (abcd, efgh), and V[g] is the four schedule words for rounds 4g..4g+3:
// V[g + 4] = su1(su0(V[g], V[g + 1]), V[g + 2], V[g + 3]).

namespace iclforge::internal::crypto::hw {

void compress(std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) noexcept {
    uint32x4_t abcd = vld1q_u32(state);
    uint32x4_t efgh = vld1q_u32(state + 4);

    for (; blocks != 0; --blocks, data += 64) {
        const uint32x4_t abcd_save = abcd;
        const uint32x4_t efgh_save = efgh;

        // w[g & 3] is V[g]; the window of four slides along the 16 groups of a block.
        uint32x4_t w[4];
        for (int i = 0; i < 4; ++i) {
            w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16 * i)));
        }

        for (int g = 0; g < 16; ++g) {
            const uint32x4_t wk =
                vaddq_u32(w[g & 3], vld1q_u32(kSha256RoundConstants.data() + 4 * g));
            if (g < 12) {
                // V[g + 4] takes the place of V[g], which wk has just consumed.
                w[g & 3] = vsha256su1q_u32(vsha256su0q_u32(w[g & 3], w[(g + 1) & 3]),
                                           w[(g + 2) & 3], w[(g + 3) & 3]);
            }
            const uint32x4_t previous_abcd = abcd;
            abcd = vsha256hq_u32(abcd, efgh, wk);
            efgh = vsha256h2q_u32(efgh, previous_abcd, wk);
        }

        abcd = vaddq_u32(abcd, abcd_save);
        efgh = vaddq_u32(efgh, efgh_save);
    }

    vst1q_u32(state, abcd);
    vst1q_u32(state + 4, efgh);
}

}  // namespace iclforge::internal::crypto::hw
