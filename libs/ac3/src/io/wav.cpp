#include "iclforge/ac3/io/wav.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/types.hpp"

namespace iclforge::ac3::io {

namespace {

// WAVE_FORMAT_EXTENSIBLE dwChannelMask bits (mmreg.h's SPEAKER_*). A WAV
// frame is interleaved in increasing bit order, which is what makes these
// values - rather than A/52's coded order - decide where a channel sits in
// the file. SPEAKER_BACK_CENTER is the standard mono-surround position, so
// acmods 2/1 and 3/1 are as well served by this convention as any other.
constexpr std::uint32_t kFrontLeft = 0x1;
constexpr std::uint32_t kFrontRight = 0x2;
constexpr std::uint32_t kFrontCenter = 0x4;
constexpr std::uint32_t kLowFrequency = 0x8;
constexpr std::uint32_t kBackLeft = 0x10;
constexpr std::uint32_t kBackRight = 0x20;
constexpr std::uint32_t kBackCenter = 0x100;

// Each acmod's full-bandwidth channels, in A/52 Table 5.8 coded order, as the
// speaker each one feeds. Indexed by acmod; 1+1 is deliberately empty because
// it is two programmes rather than a soundfield (see wav_channel_order). The
// LFE is not here: it is coded last whatever the acmod, and is appended by
// the caller with kLowFrequency.
constexpr std::array<std::array<std::uint32_t, 5>, 8> kAcmodSpeakers = {{
    {},                                                              // 1+1
    {kFrontCenter},                                                  // 1/0
    {kFrontLeft, kFrontRight},                                       // 2/0
    {kFrontLeft, kFrontCenter, kFrontRight},                         // 3/0
    {kFrontLeft, kFrontRight, kBackCenter},                          // 2/1
    {kFrontLeft, kFrontCenter, kFrontRight, kBackCenter},            // 3/1
    {kFrontLeft, kFrontRight, kBackLeft, kBackRight},                // 2/2
    {kFrontLeft, kFrontCenter, kFrontRight, kBackLeft, kBackRight},  // 3/2
}};

}  // namespace

std::optional<Ac3Layout> ac3_layout_for(std::size_t wav_channels) {
    // WAV order per channel count, mapped onto the A/52 Table 5.8 array. Only
    // the counts an acmod can express appear; 5.1 is the interesting one,
    // where every channel but L moves.
    switch (wav_channels) {
        case 1:  // FC                      -> C
            return Ac3Layout{.acmod = Acmod::k1_0, .lfe = false, .wav_index = {0}};
        case 2:  // FL FR                   -> L R
            return Ac3Layout{.acmod = Acmod::k2_0, .lfe = false, .wav_index = {0, 1}};
        case 3:  // FL FR FC                -> L C R
            return Ac3Layout{.acmod = Acmod::k3_0, .lfe = false, .wav_index = {0, 2, 1}};
        case 4:  // FL FR BL BR             -> L R SL SR
            return Ac3Layout{.acmod = Acmod::k2_2, .lfe = false, .wav_index = {0, 1, 2, 3}};
        case 5:  // FL FR FC BL BR          -> L C R SL SR
            return Ac3Layout{.acmod = Acmod::k3_2, .lfe = false, .wav_index = {0, 2, 1, 3, 4}};
        case 6:  // FL FR FC LFE BL BR      -> L C R SL SR LFE
            return Ac3Layout{.acmod = Acmod::k3_2, .lfe = true, .wav_index = {0, 2, 1, 4, 5, 3}};
        default: return std::nullopt;
    }
}

std::vector<std::size_t> wav_channel_order(Acmod acmod, bool lfe) {
    const auto fullbw = static_cast<std::size_t>(fullbw_channel_count(acmod));
    const auto count = fullbw + (lfe ? 1u : 0u);
    std::vector<std::size_t> order(count);

    if (acmod == Acmod::kDualMono) {
        // 1+1 is the one acmod with no speaker positions to sort: it carries
        // two independent programmes (Ch1, Ch2) rather than a soundfield, so
        // there is nothing for dwChannelMask to say about it. The channels go
        // out in the order the codec holds them.
        std::iota(order.begin(), order.end(), std::size_t{0});
        return order;
    }

    // Pair each coded channel with its speaker's mask bit and sort. Sorting is
    // the whole rule: a WAV frame is interleaved in increasing dwChannelMask
    // bit order, so the bit *is* the slot index once the set is known. That
    // also puts the LFE where WAV wants it (bit 3, straight after FC) rather
    // than where A/52 codes it (always last), which is the difference for
    // every acmod below that has an LFE.
    std::vector<std::pair<std::uint32_t, std::size_t>> slots;
    slots.reserve(count);
    const auto& speakers = kAcmodSpeakers[static_cast<std::uint8_t>(acmod)];
    for (std::size_t ch = 0; ch < fullbw; ++ch) {
        slots.emplace_back(speakers[ch], ch);
    }
    if (lfe) {
        slots.emplace_back(kLowFrequency, fullbw);
    }
    std::ranges::sort(slots);
    for (std::size_t slot = 0; slot < count; ++slot) {
        order[slot] = slots[slot].second;
    }
    return order;
}

}  // namespace iclforge::ac3::io
