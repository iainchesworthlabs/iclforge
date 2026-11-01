#include <algorithm>
#include <cstddef>

#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/encoder/layout.hpp"

namespace iclforge::truehd {

namespace {

constexpr std::uint8_t kExtraDataSeed = 0xA9;  // TH spec §16.4

}  // namespace

std::optional<OamdFrameAt> oamd_frame_in_unit(std::uint64_t unit, int unit_samples,
                                              int frame_samples) noexcept {
    if (unit_samples <= 0 || frame_samples <= 0) {
        return std::nullopt;
    }
    const auto unit_len = static_cast<std::uint64_t>(unit_samples);
    const auto frame_len = static_cast<std::uint64_t>(frame_samples);
    const std::uint64_t frame = (unit * unit_len + frame_len - 1) / frame_len;
    const std::uint64_t start = frame * frame_len;
    if (start >= (unit + 1) * unit_len) {
        return std::nullopt;
    }
    return OamdFrameAt{frame, static_cast<int>(start - unit * unit_len)};
}

OamdOffset split_oamd_offset(int offset) noexcept {
    return OamdOffset{offset % kOamdOffsetStep, offset / kOamdOffsetStep};
}

ExtraDataSource oamd_extra_data(OamdFrameSource source, int unit_samples, int frame_samples) {
    return [source = std::move(source), unit_samples,
            frame_samples](std::uint64_t unit) -> std::vector<std::byte> {
        const auto at = oamd_frame_in_unit(unit, unit_samples, frame_samples);
        if (!at) {
            return {};
        }
        return source(at->frame, at->offset);
    };
}

std::vector<std::byte> layout_frame_extra_data(std::span<const std::byte> payload) {
    const std::size_t content = 4 + payload.size() + 1;
    const std::size_t words = (content + 1) / 2;
    const std::size_t total = words * 2;
    std::vector<std::byte> out(total, std::byte{0});
    const auto length = static_cast<std::uint32_t>(words - 1);
    const auto word0 = static_cast<std::uint32_t>(
        (std::uint32_t{extra_data_check_nibble(length)} << 12U) | (length & 0xFFFU));
    out[0] = static_cast<std::byte>(word0 >> 8U);
    out[1] = static_cast<std::byte>(word0 & 0xFFU);
    const auto size = static_cast<std::uint32_t>(payload.size());
    out[2] = static_cast<std::byte>((size >> 8U) & 0xFU);
    out[3] = static_cast<std::byte>(size & 0xFFU);
    std::copy(payload.begin(), payload.end(), out.begin() + 4);
    unsigned accumulator = size ^ kExtraDataSeed;
    for (std::size_t i = 4; i + 1 < total; ++i) {
        accumulator ^= std::to_integer<unsigned>(out[i]);
    }
    out[total - 1] = static_cast<std::byte>(((accumulator >> 8U) ^ accumulator) & 0xFFU);
    return out;
}

}  // namespace iclforge::truehd
