#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "iclforge/mlp/export.hpp"

// What a TrueHD (MLP) major sync says about a stream: the format, the presentations and the channel
// meaning (truehd-spec.md sections 4, 5, 13 and 14). Field names follow the specification; where
// its text and the streams FFmpeg writes disagree, the reading taken is the one in
// planning/truehd.md's register, cited by entry (P1 to P28).

namespace iclforge::mlp {

// Appendix A.
inline constexpr std::uint32_t kMajorSyncTrueHd = 0xF8726FBAU;
inline constexpr std::uint32_t kMajorSyncMlp = 0xF8726FBBU;
inline constexpr std::uint16_t kSignatureMlp = 0xB752U;
inline constexpr std::uint16_t kSignatureTrueHd = 0xB753U;

inline constexpr int kMaxSubstreams = 4;
inline constexpr int kMaxChannels = 16;
// The two channels past max_chan that a primitive matrix may take a coefficient for (P21).
inline constexpr int kNoiseChannels = 2;
inline constexpr int kMaxMatrixInputs = kMaxChannels + kNoiseChannels;
inline constexpr int kMaxPrimitiveMatrices = 16;
inline constexpr int kMaxFirOrder = 8;
inline constexpr int kMaxIirOrder = 4;
inline constexpr int kMaxBlockSize = 160;
inline constexpr int kMinBlockSize = 8;
// An access unit is 1/1200 s whatever the rate (P1): 40 samples at 44.1 and 48 kHz.
inline constexpr int kBaseSamplesPerAccessUnit = 40;
inline constexpr int kMaxSamplesPerAccessUnit = 160;

enum class StreamType : std::uint8_t {
    kTrueHd,  // 0xBA, FBA
    kMlp,     // 0xBB, FBB (DVD-Audio): recognised, not decoded
};

// Section 1.4's sample rate codes. nullopt for a reserved code.
[[nodiscard]] ICLFORGE_MLP_EXPORT std::optional<std::uint32_t> sample_rate_from_code(
    std::uint8_t code) noexcept;
[[nodiscard]] ICLFORGE_MLP_EXPORT std::optional<std::uint8_t> sample_rate_code(
    std::uint32_t hz) noexcept;

// 40 samples at 44.1 and 48 kHz, 80 at 88.2 and 96 kHz, 160 at 176.4 and 192 kHz (P1).
[[nodiscard]] constexpr int samples_per_access_unit(std::uint32_t sample_rate) noexcept {
    return sample_rate > 96000U ? 160 : sample_rate > 48000U ? 80 : kBaseSamplesPerAccessUnit;
}

// Section 13.1's 13-bit channel assignment: bit 0 L and R, 1 C, 2 LFE, 3 Ls and Rs, 4 Lvh and
// Rvh, 5 Lc and Rc, 6 Lrs and Rrs, 7 Cs, 8 Ts, 9 Lsd and Rsd, 10 Lw and Rw, 11 Cvh, 12 LFE2. The
// codes of 13.2 are mislabelled (P25): stereo is 0x001, 5.1 0x00F, 7.1 0x04F.
inline constexpr std::uint16_t kAssignStereo = 0x001;
inline constexpr std::uint16_t kAssign51 = 0x00F;
inline constexpr std::uint16_t kAssign71 = 0x04F;

// The number of channels an assignment names (bits 0, 3, 4, 5, 6, 9 and 10 name two each).
[[nodiscard]] ICLFORGE_MLP_EXPORT int channel_count(std::uint16_t assignment) noexcept;

// The speaker labels of an assignment, in the order the channels are carried (13.1's bit order,
// the pair's left first): "L", "R", "C", "LFE", "Ls", "Rs", ... At most 16 labels.
struct SpeakerList {
    std::array<std::string_view, kMaxChannels> labels{};
    int count = 0;
};
[[nodiscard]] ICLFORGE_MLP_EXPORT SpeakerList speakers(std::uint16_t assignment) noexcept;

// Section 5.1's format_info of a 0xBA stream.
struct FormatInfo {
    std::uint8_t sample_rate_code = 0;  // audio_sampling_frequency, 1.4
    std::uint8_t reserved = 0;
    std::uint8_t twoch_modifier = 0;    // twoch_decoder_channel_modifier, 5.4
    std::uint8_t sixch_modifier = 0;
    std::uint8_t sixch_assignment = 0;  // 5 bits, 13.1's low five
    std::uint8_t eightch_modifier = 0;
    std::uint16_t eightch_assignment = 0;  // 13 bits, 13.1

    [[nodiscard]] std::uint32_t pack() const noexcept;
    [[nodiscard]] static FormatInfo unpack(std::uint32_t word) noexcept;
};

// Section 14.1's channel_meaning(), 64 bits.
struct ChannelMeaning {
    std::int8_t heavy_drc_start_up_gain = 0;  // 6 bits, signed
    bool twoch_control_enabled = false;
    bool sixch_control_enabled = false;
    bool eightch_control_enabled = false;
    bool reserved_bit = false;
    std::int8_t drc_start_up_gain = 0;  // 7 bits, signed
    std::uint8_t twoch_dialogue_norm = 0;
    std::uint8_t twoch_mix_level = 0;
    std::uint8_t sixch_dialogue_norm = 0;
    std::uint8_t sixch_mix_level = 0;
    std::uint8_t sixch_source_format = 0;
    std::uint8_t eightch_dialogue_norm = 0;
    std::uint8_t eightch_mix_level = 0;
    std::uint8_t eightch_source_format = 0;
    bool reserved_bit2 = false;
    bool extra_channel_meaning_present = false;
};

// Section 14.2's extra_channel_meaning(), the 16-channel presentation's description. It lies
// inside the major sync before its CRC and is (length + 1) 16-bit words long (P28).
struct ExtraChannelMeaning {
    std::uint8_t length = 0;  // extra_channel_meaning_length: (length + 1) 16-bit words
    std::uint8_t sixteench_dialogue_norm = 0;
    std::uint8_t sixteench_mix_level = 0;
    std::uint8_t sixteench_channel_count = 0;  // the presentation's channels, less one
    bool dyn_object_only = false;
    bool lfe_present = false;                    // when dyn_object_only
    std::uint8_t sixteench_content_description = 0;  // 4 bits, when not dyn_object_only
    bool chan_distribute = false;                    // content_description bit 0
    bool lfe_only = false;
    std::uint16_t sixteench_channel_assignment = 0;  // 10 bits, unless lfe_only
    std::uint8_t sixteench_isf = 0;                  // content_description bit 1
    std::uint8_t sixteench_dynamic_object_count = 0;  // content_description bit 2, 5 bits
};

// Section 4.1's major_sync_info().
struct MajorSync {
    StreamType stream_type = StreamType::kTrueHd;
    FormatInfo format;
    std::uint16_t signature = kSignatureMlp;  // FFmpeg writes 0xB752 in a 0xBA stream (P24)
    std::uint16_t flags = 0;
    std::uint16_t reserved = 0;
    bool is_vbr = true;
    // In units of sample_rate / 16 bits per second (P2): 6000 is 18 Mbps at 48 kHz.
    std::uint16_t peak_data_rate = 0;
    std::uint8_t num_substreams = 1;
    std::uint8_t extended_substream_info = 0;  // 4 bits, 4.5
    std::uint8_t substream_info = 0;           // 8 bits, 6.4
    ChannelMeaning channel_meaning;
    std::optional<ExtraChannelMeaning> extra_channel_meaning;

    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] int samples_per_access_unit() const noexcept;
    // peak_data_rate in bits per second.
    [[nodiscard]] std::uint64_t peak_bits_per_second() const noexcept;
};

}  // namespace iclforge::mlp
