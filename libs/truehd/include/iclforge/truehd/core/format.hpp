#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/truehd/export.hpp"

// The constants, the sample rates and the channel vocabulary of a TrueHD / MLP stream. "TH spec" is
// the owner's Dolby TrueHD (MLP) Reference Specification, v3.5 (planning/truehd.md names its status
// and where it disagrees with the streams; each such place is an entry of libs/truehd/ERRATA.md).

namespace iclforge::truehd {

// TH spec §4.2: 0xF8726FBA is TrueHD (Blu-ray, HD DVD), 0xF8726FBB is MLP (DVD-Audio).
enum class StreamKind : std::uint8_t { kTrueHd, kMlp };

inline constexpr std::uint8_t kMajorSyncByte0 = 0xF8;
inline constexpr std::uint8_t kMajorSyncByte1 = 0x72;
inline constexpr std::uint8_t kMajorSyncByte2 = 0x6F;
inline constexpr std::uint8_t kTrueHdStreamByte = 0xBA;
inline constexpr std::uint8_t kMlpStreamByte = 0xBB;

// TH spec §4.3 (v3.3 to v3.5): 0xB752 for both stream types. v3.0 gave TrueHD 0xB753; no stream has it
// (ERRATA.md).
inline constexpr std::uint16_t kSignature = 0xB752;

// TH spec §7.2: the 14-bit restart sync. Bit 0 is noise_type for the first two. 0x31EC marks the
// Atmos substreams 2 and 3.
inline constexpr std::uint16_t kRestartSyncStandard = 0x31EA;
inline constexpr std::uint16_t kRestartSyncNoise = 0x31EB;
inline constexpr std::uint16_t kRestartSyncAtmos = 0x31EC;

inline constexpr std::uint8_t kSubstreamParitySeed = 0xA9;  // TH spec §6.2, App. B

inline constexpr int kMaxSubstreams = 4;  // TH spec §4.1, App. A
inline constexpr int kMaxChannels = 16;   // matrix channels 0..15, TH spec §7.1
inline constexpr int kNoiseChannels = 2;  // the two channels a matrix can draw noise from
inline constexpr int kMaxMatrices = 15;   // TH spec §10.3, substream 3
inline constexpr int kMaxFirOrder = 8;    // TH spec §9.3
inline constexpr int kMaxIirOrder = 4;
inline constexpr int kMaxFilterOrder = 8;     // FIR order + IIR order
inline constexpr int kMaxBlockSamples = 255;  // TH spec §3.3: a unit holds fewer than 256 samples
inline constexpr int kMaxSamplesPerAccessUnit = 255;
inline constexpr std::size_t kMajorSyncBaseBytes =
    28;  // TH spec §4.1: sync .. CRC, without an extension

// TH spec §1.4 / §5.3: ratebits. 0..2 are 48 kHz times 1, 2, 4; 8..10 are 44.1 kHz times 1, 2, 4.
// Returns 0 for a code the specification does not define.
[[nodiscard]] constexpr std::uint32_t sample_rate_hz(unsigned ratebits) noexcept {
    if (ratebits <= 2) {
        return 48000U << ratebits;
    }
    if (ratebits >= 8 && ratebits <= 10) {
        return 44100U << (ratebits - 8U);
    }
    return 0;
}

// 0 for 48/44.1 kHz, 1 for 96/88.2, 2 for 192/176.4 (the stream's "rate factor"); -1 for an
// undefined code.
[[nodiscard]] constexpr int rate_factor(unsigned ratebits) noexcept {
    if (ratebits <= 2) {
        return static_cast<int>(ratebits);
    }
    if (ratebits >= 8 && ratebits <= 10) {
        return static_cast<int>(ratebits - 8U);
    }
    return -1;
}

// The samples in an access unit: 40 at 48 and 44.1 kHz, doubled at each rate factor. TH spec §3.3
// says 40 at every rate; the streams show 80 at 96 kHz and 160 at 192 kHz (ERRATA.md). Decoding
// does not depend on it: the blocks of a segment add up to the unit.
[[nodiscard]] constexpr int nominal_samples_per_access_unit(unsigned ratebits) noexcept {
    const int factor = rate_factor(ratebits);
    return factor < 0 ? 0 : 40 << factor;
}

// TH spec §13.1: the speakers of a channel assignment, in the order the bits of the mask list them
// (a bit with two speakers lists them left, then right). A TrueHD stream's channels are in this
// order.
enum class Channel : std::uint8_t {
    kL,
    kR,
    kC,
    kLfe,
    kLs,
    kRs,
    kLvh,  // left vertical height
    kRvh,
    kLc,  // left centre
    kRc,
    kLrs,  // left rear surround
    kRrs,
    kCs,   // centre surround
    kTs,   // top surround
    kLsd,  // left surround direct
    kRsd,
    kLw,  // left wide
    kRw,
    kCvh,  // centre vertical height
    kLfe2,
};

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::string_view channel_name(Channel channel) noexcept;

inline constexpr int kChannelAssignmentBits = 13;

// The channels a 13-bit TrueHD channel assignment (TH spec §13.1; format_info's eightch field, or
// its low five bits for the sixch field) lists, in stream order. Empty for 0.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::vector<Channel> channels_of_assignment(
    std::uint32_t assignment);

// The number of channels a TrueHD channel assignment lists.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT int channel_count_of_assignment(
    std::uint32_t assignment) noexcept;

// TH spec App. D: the channel count of an MLP channel arrangement (format_info's low five bits for
// 0xBB). -1 for an index the table leaves reserved.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT int channel_count_of_mlp_arrangement(
    unsigned arrangement) noexcept;

// TH spec §1.4: the word length group1_bits / group2_bits index. 0 for a reserved code.
[[nodiscard]] constexpr int mlp_word_bits(unsigned code) noexcept {
    switch (code) {
        case 0:
            return 16;
        case 1:
            return 20;
        case 2:
            return 24;
        default:
            return 0;
    }
}

}  // namespace iclforge::truehd
