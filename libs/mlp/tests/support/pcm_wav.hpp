#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

// Integer PCM from a canonical WAV, for the lossless tests: base::read_wav converts to floating
// point, which a bit-exact comparison has no use for. Reads WAVE_FORMAT_PCM (and
// WAVE_FORMAT_EXTENSIBLE with a PCM subformat) at 16, 20 in 24, or 24 bits, and returns samples at
// 24-bit scale, as the TrueHD decoder gives them: a 16-bit sample shifted left by 8.

namespace iclforge::mlp::test {

struct PcmWav {
    int channels = 0;
    std::uint32_t sample_rate = 0;
    int bits = 0;
    std::vector<std::int32_t> samples;  // interleaved, 24-bit scale
};

inline std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) {
        out[i] = static_cast<std::byte>(chars[i]);
    }
    return out;
}

inline std::optional<PcmWav> read_pcm_wav(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    if (!bytes || bytes->size() < 12) {
        return std::nullopt;
    }
    const auto& d = *bytes;
    auto u16 = [&](std::size_t at) {
        return static_cast<unsigned>(std::to_integer<unsigned>(d[at]) |
                                     std::to_integer<unsigned>(d[at + 1]) << 8U);
    };
    auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(u16(at) | u16(at + 2) << 16U);
    };
    PcmWav wav;
    std::size_t at = 12;
    while (at + 8 <= d.size()) {
        const std::uint32_t size = u32(at + 4);
        const std::size_t body = at + 8;
        if (d[at] == std::byte{'f'} && d[at + 1] == std::byte{'m'}) {
            wav.channels = static_cast<int>(u16(body + 2));
            wav.sample_rate = u32(body + 4);
            wav.bits = static_cast<int>(u16(body + 14));
        } else if (d[at] == std::byte{'d'} && d[at + 1] == std::byte{'a'}) {
            const int width = wav.bits / 8;
            if (width < 2 || width > 3 || wav.channels == 0) {
                return std::nullopt;
            }
            const std::size_t count = size / static_cast<std::size_t>(width);
            wav.samples.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t p = body + i * static_cast<std::size_t>(width);
                std::uint32_t v = 0;
                for (int b = 0; b < width; ++b) {
                    v |= std::to_integer<std::uint32_t>(d[p + static_cast<std::size_t>(b)])
                         << (8U * static_cast<unsigned>(b));
                }
                const unsigned sign = 1U << (8U * static_cast<unsigned>(width) - 1U);
                std::int32_t s =
                    static_cast<std::int32_t>(v ^ sign) - static_cast<std::int32_t>(sign);
                wav.samples[i] = s * (width == 2 ? 256 : 1);
            }
            return wav;
        }
        at = body + size + (size & 1U);
    }
    return std::nullopt;
}

}  // namespace iclforge::mlp::test
