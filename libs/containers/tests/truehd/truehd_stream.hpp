#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

// What the container tests and the container tool need to know about a TrueHD elementary stream: its access
// units, which of them open with a major sync, and the numbers a container's track description carries. The
// containers library knows no codec (tools/checks/projects.json), so this is the one place the tests meet the
// truehd library: a test-support header, not part of either.

namespace iclforge::containers::testing {

struct TruehdStream {
    std::vector<std::byte> bytes;                   // the file
    std::vector<std::span<const std::byte>> units;  // whole access units, views into `bytes`
    std::vector<bool> major;                        // per unit: it opens with a major sync
    truehd::StreamInfo info;
    int channels = 0;           // of the best presentation the stream has
    int padding_samples = 0;    // the end-of-stream marker's count (ERRATA 14 of libs/truehd)
    std::uint32_t samples_per_unit = 0;
};

[[nodiscard]] inline std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

[[nodiscard]] inline bool write_file(const std::string& path, std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// The units of an in-memory stream. The result holds views into `stream.bytes`, so it is returned by value
// from a function that builds it in place and must not be copied or moved afterwards: the vector's buffer
// moves with it, which keeps the views valid.
[[nodiscard]] inline std::optional<TruehdStream> load_truehd_bytes(std::vector<std::byte> bytes) {
    TruehdStream stream;
    stream.bytes = std::move(bytes);
    const auto info = truehd::probe_stream(stream.bytes);
    if (!info) {
        return std::nullopt;
    }
    stream.info = *info;
    stream.samples_per_unit = static_cast<std::uint32_t>(info->samples_per_access_unit);
    truehd::ElementaryReader reader(stream.bytes);
    truehd::Decoder decoder(truehd::DecoderConfig{.strictness = truehd::Strictness::kLenient});
    while (const auto unit = reader.next()) {
        stream.units.push_back(*unit);
        stream.major.push_back(truehd::starts_with_major_sync(unit->subspan(4)));
        const auto decoded = decoder.decode(*unit);
        if (decoded && decoded->has_value()) {
            stream.channels = (*decoded)->channels;
            stream.padding_samples += (*decoded)->padding_samples;
        }
    }
    if (stream.units.empty() || stream.channels == 0) {
        return std::nullopt;
    }
    return stream;
}

[[nodiscard]] inline std::optional<TruehdStream> load_truehd(const std::string& path) {
    return load_truehd_bytes(read_file(path));
}

}  // namespace iclforge::containers::testing
