#include "iclforge/mlp/io/extra_data.hpp"

#include "core/crc.hpp"

namespace iclforge::mlp {

namespace {

constexpr std::size_t kHeaderBytes = 2;
constexpr std::size_t kParityBytes = 2;
constexpr std::size_t kPayloadHeaderBytes = 3;

}  // namespace

std::expected<std::vector<std::byte>, Error> write_extra_data(
    std::span<const EvolutionPayload> payloads) {
    std::vector<std::byte> out(kHeaderBytes);
    for (const EvolutionPayload& p : payloads) {
        if (p.type == 0 || p.data.size() > 0xFFFFU) {
            return std::unexpected(Error::kInvalidConfiguration);
        }
        out.push_back(static_cast<std::byte>(p.type));
        out.push_back(static_cast<std::byte>(p.data.size() >> 8U));
        out.push_back(static_cast<std::byte>(p.data.size() & 0xFFU));
        out.insert(out.end(), p.data.begin(), p.data.end());
    }
    if (out.size() % 2U != 0) {
        out.push_back(std::byte{0});
    }
    const std::size_t words = (out.size() + kParityBytes) / 2U;
    if (words > 0x0FFFU) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    // The header word: the 12-bit length, and a check nibble that makes the word's nibbles XOR
    // to 0xF (the access unit header's rule, 3.2).
    const auto length = static_cast<unsigned>(words);
    const unsigned low = (length >> 8U) ^ ((length >> 4U) & 0xFU) ^ (length & 0xFU);
    const unsigned nibble = (low ^ 0xFU) & 0xFU;
    out[0] = static_cast<std::byte>((nibble << 4U) | (length >> 8U));
    out[1] = static_cast<std::byte>(length & 0xFFU);
    const std::uint8_t parity =
        static_cast<std::uint8_t>(detail::xor_bytes(out) ^ detail::kParityXor);
    out.push_back(static_cast<std::byte>(parity));
    out.push_back(std::byte{0});
    return out;
}

std::expected<std::vector<EvolutionPayload>, Error> read_extra_data(
    std::span<const std::byte> extra_data) {
    std::vector<EvolutionPayload> payloads;
    if (extra_data.empty()) {
        return payloads;
    }
    if (extra_data.size() < kHeaderBytes + kParityBytes) {
        return std::unexpected(Error::kExtraDataLength);
    }
    if (detail::nibble_xor(extra_data.first(kHeaderBytes)) != 0x0F) {
        return std::unexpected(Error::kExtraDataParity);
    }
    const std::size_t words = (std::to_integer<std::size_t>(extra_data[0]) & 0x0FU) << 8U |
                              std::to_integer<std::size_t>(extra_data[1]);
    const std::size_t size = words * 2U;
    if (size < kHeaderBytes + kParityBytes || size > extra_data.size()) {
        return std::unexpected(Error::kExtraDataLength);
    }
    const auto block = extra_data.first(size);
    const std::size_t parity_at = size - kParityBytes;
    const std::uint8_t parity =
        static_cast<std::uint8_t>(detail::xor_bytes(block.first(parity_at)) ^ detail::kParityXor);
    if (parity != std::to_integer<std::uint8_t>(block[parity_at])) {
        return std::unexpected(Error::kExtraDataParity);
    }
    std::size_t at = kHeaderBytes;
    while (at + kPayloadHeaderBytes <= parity_at) {
        const auto type = std::to_integer<std::uint8_t>(block[at]);
        if (type == 0) {
            break;
        }
        const std::size_t length = std::to_integer<std::size_t>(block[at + 1]) << 8U |
                                   std::to_integer<std::size_t>(block[at + 2]);
        at += kPayloadHeaderBytes;
        if (at + length > parity_at) {
            return std::unexpected(Error::kExtraDataLength);
        }
        EvolutionPayload p;
        p.type = type;
        p.data.assign(block.begin() + static_cast<std::ptrdiff_t>(at),
                      block.begin() + static_cast<std::ptrdiff_t>(at + length));
        payloads.push_back(std::move(p));
        at += length;
    }
    return payloads;
}

}  // namespace iclforge::mlp
