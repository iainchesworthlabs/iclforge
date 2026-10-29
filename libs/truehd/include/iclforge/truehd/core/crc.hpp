#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/truehd/export.hpp"

// The check values of the three layers of a TrueHD / MLP stream. The polynomials are TH spec App.
// B's; the parts App. B leaves out (initial values, which bytes are covered, the final word XORed
// into the result) were found by running the candidates over the streams FFmpeg's encoder writes,
// and are entries of ERRATA.md.

namespace iclforge::truehd {

// TH spec §4.1: the CRC-16 (polynomial 0x002D, initial value 0) of the major sync. `covered` is the
// major sync from its first byte (the sync word) up to, not including, the two CRC bytes. The
// register runs over all of it but the last two bytes, and those two are then XORed into it as a
// big-endian word.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::uint16_t major_sync_crc(
    std::span<const std::byte> covered) noexcept;

// TH spec §7.1: the CRC-8 (polynomial 0x1D, initial value 0) of a restart header, over the bits
// from the first bit of the 14-bit sync up to, not including, the CRC field. The register runs over
// all of them but the last eight, and those eight are then XORed into it. `data` holds the bits MSB
// first; `first_bit` is the sync's first bit and `crc_bit` the first bit of the CRC field (so the
// span is [first_bit, crc_bit)).
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::uint8_t restart_header_crc(
    std::span<const std::byte> data, std::size_t first_bit, std::size_t crc_bit) noexcept;

// TH spec §6.2: the two bytes that end a substream segment when its directory entry says
// crc_present. `payload` is the segment without them. The parity byte is the XOR of the payload's
// bytes, XORed with 0xA9.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::uint8_t substream_parity(
    std::span<const std::byte> payload) noexcept;

// The CRC byte after the parity: CRC-8 (polynomial 0x63, initial value 0x3C) over the payload
// without its last byte, XORed with that last byte (TH spec App. B, v3.1's note). The parity byte
// is not covered.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::uint8_t substream_crc(
    std::span<const std::byte> payload) noexcept;

// TH spec §16.4: the check an extra-data field ends with.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::uint8_t extra_data_check_nibble(
    std::uint32_t extra_data_length) noexcept;

// TH spec §7.3, ERRATA.md entry 12: the lossless check of a restart interval. The decoder and the
// encoder fold the samples the same way, through these two functions, so that they cannot disagree.
//
// One output sample enters the fold masked to its low 24 bits and shifted left by the *matrix
// channel* the output channel takes (the channel `ch_assign` assigns to it: ERRATA entry 42), not by
// the output channel's position. In a stream whose ch_assign is the identity the two are the same,
// which is every stream FFmpeg's encoder writes; FFmpeg's decoder shifts by the matrix channel
// (`hb_matrices.thd`, ERRATA entry 12). **Provisional, owner to confirm** (open question 6).
[[nodiscard]] constexpr std::uint32_t lossless_fold_step(std::uint32_t fold, std::uint32_t sample,
                                                         unsigned matrix_channel) noexcept {
    return matrix_channel >= 32 ? fold : fold ^ ((sample & 0xFFFFFFU) << matrix_channel);
}

// The 8-bit value a restart header carries for the interval whose samples folded to `fold`: the
// four bytes of the 32-bit fold XORed together.
[[nodiscard]] constexpr std::uint8_t lossless_check_of(std::uint32_t fold) noexcept {
    return static_cast<std::uint8_t>((fold ^ (fold >> 8U) ^ (fold >> 16U) ^ (fold >> 24U)) & 0xFFU);
}

}  // namespace iclforge::truehd
