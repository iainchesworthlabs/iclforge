#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "iclforge/ac3/core/types.hpp"

// A/52 Table 5.18, the bit rates and the syncframe sizes, transcribed from ATSC A/52:2018.
// The types and base constants are core/types.hpp's, included here for a release.

namespace iclforge::ac3 {

// A/52 Table 5.18: the 19 nominal bit rates. frmsizecod / 2 indexes this list.
inline constexpr std::array<std::uint16_t, 19> kBitratesKbps = {
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
};

[[nodiscard]] constexpr std::optional<int> bitrate_index(std::uint32_t kbps) {
    for (std::size_t i = 0; i < kBitratesKbps.size(); ++i) {
        if (kBitratesKbps[i] == kbps) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool is_valid_bitrate(std::uint32_t kbps) {
    return bitrate_index(kbps).has_value();
}

// The largest Table 5.18 rung at or below `requested_kbps`, capped at 640 -
// what a caller wanting "as close to this rate as AC-3 can legally carry"
// needs (kBitratesKbps is sorted ascending, so the first entry is always a
// safe floor). Used where a plan's own bitrate - possibly an E-AC-3-only
// rung like 768, or simply not on the table at all - has to be reduced to
// something plain AC-3 can carry, such as the live session's parallel 5.1
// downmix receiver leg (see docs/forge/gui/live-session.md).
[[nodiscard]] constexpr std::uint32_t clamp_to_legal_ac3_bitrate(std::uint32_t requested_kbps) {
    const std::uint32_t capped = std::min<std::uint32_t>(requested_kbps, kBitratesKbps.back());
    std::uint32_t best = kBitratesKbps.front();
    for (const auto kbps : kBitratesKbps) {
        if (kbps <= capped) {
            best = kbps;
        }
    }
    return best;
}

// A/52 Table 5.18 "Frame Size Code Table (1 word = 16 bits)", transcribed
// verbatim. Row = bit-rate index; column = fscod (48 / 44.1 / 32 kHz). The
// table lists two frmsizecod values per bit rate: at 44.1 kHz the odd code is
// one word longer (the padding word CBR streams alternate to hit the exact
// rate); at 32 and 48 kHz both codes give the identical size shown here.
inline constexpr std::array<std::array<std::uint16_t, 3>, 19> kFrameSizeWords = {{
    // 48 kHz  44.1 kHz  32 kHz
    {64, 69, 96},          // 32 kbps
    {80, 87, 120},         // 40 kbps
    {96, 104, 144},        // 48 kbps
    {112, 121, 168},       // 56 kbps
    {128, 139, 192},       // 64 kbps
    {160, 174, 240},       // 80 kbps
    {192, 208, 288},       // 96 kbps
    {224, 243, 336},       // 112 kbps
    {256, 278, 384},       // 128 kbps
    {320, 348, 480},       // 160 kbps
    {384, 417, 576},       // 192 kbps
    {448, 487, 672},       // 224 kbps
    {512, 557, 768},       // 256 kbps
    {640, 696, 960},       // 320 kbps
    {768, 835, 1152},      // 384 kbps
    {896, 975, 1344},      // 448 kbps
    {1024, 1114, 1536},    // 512 kbps
    {1152, 1253, 1728},    // 576 kbps
    {1280, 1393, 1920},    // 640 kbps
}};

namespace detail {
// A 1536-sample frame spans exactly 32 ms at 48 kHz and 48 ms at 32 kHz, so
// those Table 5.18 columns must equal the closed-form kbps*2 / kbps*3 words.
consteval bool frame_table_matches_closed_form() {
    for (std::size_t i = 0; i < kBitratesKbps.size(); ++i) {
        if (kFrameSizeWords[i][0] != kBitratesKbps[i] * 2) return false;
        if (kFrameSizeWords[i][2] != kBitratesKbps[i] * 3) return false;
    }
    return true;
}
static_assert(frame_table_matches_closed_form());
}  // namespace detail

// Words per syncframe (A/52 Table 5.18). pad441 selects the odd frmsizecod,
// which adds one word at 44.1 kHz only. fscod2 is an Annex E (E-AC-3) concept
// with no frmsizecod table at all - classic AC-3 has no way to express one of
// its rates - so a reduced rate is refused here rather than indexed with a
// raw enum ordinal that would run past kFrameSizeWords' three columns.
[[nodiscard]] constexpr std::optional<std::uint32_t> frame_size_words(SampleRate sr,
                                                                      std::uint32_t bitrate_kbps,
                                                                      bool pad441 = false) {
    if (is_reduced_rate(sr)) {
        return std::nullopt;
    }
    const auto idx = bitrate_index(bitrate_kbps);
    if (!idx.has_value()) {
        return std::nullopt;
    }
    std::uint32_t words = kFrameSizeWords[static_cast<std::size_t>(*idx)]
                                         [static_cast<std::uint8_t>(sr)];
    if (sr == SampleRate::k44100 && pad441) {
        words += 1;
    }
    return words;
}

[[nodiscard]] constexpr std::optional<std::uint32_t> frame_size_bytes(SampleRate sr,
                                                                      std::uint32_t bitrate_kbps,
                                                                      bool pad441 = false) {
    const auto words = frame_size_words(sr, bitrate_kbps, pad441);
    if (!words.has_value()) {
        return std::nullopt;
    }
    return *words * 2;
}

// A/52 §7.10.1: the number of words in the first 5/8 of the syncframe —
// the region protected by crc1 (sync word included in the count but excluded
// from the CRC itself).
[[nodiscard]] constexpr std::uint32_t frame_size_58_words(std::uint32_t frame_words) {
    return (frame_words >> 1) + (frame_words >> 3);
}

}  // namespace iclforge::ac3
