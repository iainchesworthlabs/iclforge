#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/mlp/core/error.hpp"
#include "iclforge/mlp/export.hpp"

// The extra data of an access unit (16.1): the bytes after the last substream segment, which carry
// evolution payloads such as the Atmos object audio metadata and, later, a content signature.
//
// Section 16.1 gives only a 12-bit length "part of AU header parity", a payload and a parity
// byte, so the layout below is this library's reading of it (planning/truehd.md, P16), chosen to
// mirror the access unit's own framing:
//
//   extra_data() {
//       check_nibble            4   the XOR of this word's four nibbles is 0xF
//       extra_data_length      12   16-bit words, this word through the parity word
//       evolution_payload()     *   payload_type 8 (0 ends the list), payload_size 16 (bytes),
//                                   then payload_size bytes
//       zero padding                to the parity word
//       extra_data_parity       8   XOR of every byte before it, XOR 0xA9 (as 6.2's parity)
//       reserved                8   0
//   }
//
// FFmpeg's decoder ignores the bytes after the last segment, so a stream carrying them still
// decodes there (planning/truehd.md, the black-box findings). A Dolby encoder's extra data is
// not this layout; the decoder reports such bytes as unreadable rather than failing the access
// unit.

namespace iclforge::mlp {

enum class EvolutionPayloadType : std::uint8_t {
    kOamd = 0x01,              // object audio metadata, iclforge::objects' serialisation (15)
    kContentSignature = 0x02,  // a content signature (22); carried, not produced or checked
};

struct EvolutionPayload {
    std::uint8_t type = 0;
    std::vector<std::byte> data;
};

// The extra data holding `payloads`, padded to whole 16-bit words. Fails when a payload exceeds
// 65,535 bytes or the whole exceeds the 12-bit length.
[[nodiscard]] ICLFORGE_MLP_EXPORT std::expected<std::vector<std::byte>, Error> write_extra_data(
    std::span<const EvolutionPayload> payloads);

// The payloads of an access unit's extra data. Empty input is no payloads.
[[nodiscard]] ICLFORGE_MLP_EXPORT std::expected<std::vector<EvolutionPayload>, Error>
read_extra_data(std::span<const std::byte> extra_data);

// The hook a content signature will be produced through (22; planning/truehd.md, decision 1).
// The encoder calls it once per access unit with the access unit's bytes up to the end of its
// last substream segment and the unit's index; a payload it returns is carried as that unit's
// kContentSignature evolution payload. No signing scheme is implemented in this library; the
// codec-agnostic signing core planned for iclforge::base is what a caller would call from here.
using SignatureHook = std::function<std::optional<EvolutionPayload>(
    std::span<const std::byte> signed_bytes, std::uint64_t access_unit_index)>;

}  // namespace iclforge::mlp
