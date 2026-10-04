#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Constants and the fixed-point upsampler shared by the AudioDataDLC decoder (dlc.cpp) and
// encoder (dlc_encoder.cpp). SMPTE ST 2098-2:2022 §10.7.9 and Annex B.

namespace iclforge::iab::detail {

// §10.7.9 Table 30. The sub block count and the 48 kHz sub block size for one IAFrame rate; the
// 96 kHz size is twice the 48 kHz one.
struct DlcLayout {
    unsigned num_sub_blocks = 0;
    unsigned sub_block_size_48 = 0;

    [[nodiscard]] unsigned sub_block_size_96() const { return sub_block_size_48 * 2; }
    [[nodiscard]] unsigned sample_count_48() const { return num_sub_blocks * sub_block_size_48; }
    [[nodiscard]] unsigned sample_count_96() const { return num_sub_blocks * sub_block_size_96(); }
};

// Keyed by the §10.2.4 Table 17 FrameRate code: 24, 25, 30, 48, 50, 60, 96, 100, 120 fps. Code 9
// (24000/1001) has no row: AudioDataDLC "shall not be present in IAFrames with non-integer frame
// rates" (§10.7). Returns nullopt for it and for the reserved codes.
[[nodiscard]] constexpr std::optional<DlcLayout> dlc_layout(std::uint8_t frame_rate_code) {
    switch (frame_rate_code) {
        case 0x0: return DlcLayout{10, 200};  // 24 fps
        case 0x1: return DlcLayout{10, 192};  // 25 fps
        case 0x2: return DlcLayout{8, 200};   // 30 fps
        case 0x3: return DlcLayout{5, 200};   // 48 fps
        case 0x4: return DlcLayout{5, 192};   // 50 fps
        case 0x5: return DlcLayout{4, 200};   // 60 fps
        case 0x6: return DlcLayout{5, 100};   // 96 fps
        case 0x7: return DlcLayout{4, 120};   // 100 fps
        case 0x8: return DlcLayout{4, 100};   // 120 fps
        default: return std::nullopt;
    }
}

// B.9 Table B.1: the half band interpolation filter. Only the odd indices are used by the
// upsampler; index 16 is the centre tap, which the pseudocode realises as an 8 sample delay.
inline constexpr std::array<std::int32_t, 33> kInterp{
    0,     -138, 0, 305,  0, -618, 0, 1128, 0, -1952, 0, 3377, 0, -6450, 0, 20688, 32767,
    20688, 0,    -6450, 0, 3377, 0, -1952, 0, 1128,  0, -618, 0, 305,   0, -138,  0};

// B.9: upsamples a 48 kHz base layer to 96 kHz, `count96` output samples (twice the base layer
// length). Integer arithmetic as specified, so the decoder and an encoder compute the same
// approximation.
[[nodiscard]] std::vector<std::int32_t> upsample_base_layer(std::span<const std::int32_t> pcm48,
                                                            std::size_t count96);

}  // namespace iclforge::iab::detail
