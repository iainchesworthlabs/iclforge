#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// A bare TrueHD (or MLP) stream wrapped as Matroska, MP4 or a Blu-ray transport stream: the writers
// `forge mkv`, `mp4`, `ts` and `truehd-encode` use, and the Forge window's TrueHD encode (planning/
// truehd-parity-forge.md, rows U1 and U8). Nothing here prints or writes a file; a problem comes back
// as a sentence the caller says its own way.
//
// The containers library knows no codec, so the facts a track description needs (the access units,
// which of them open with a major sync, the channel count, the end-of-stream padding) are read
// here, with iclforge::truehd, and handed to it as plain values. What each container writes has no
// layout in the TH spec (§21 names the transports and nothing more); libs/containers/ERRATA.md
// records each layout and its single oracle, FFmpeg's muxers.
//
// Compiled into each application that uses it, like the rest of apps/shared/media/src: it needs
// iclforge::truehd and iclforge::containers.

namespace iclforge::apps::truehd_wrap {

enum class Container { kMatroska, kMp4, kTransport };

// A stream scanned for what a container needs.
struct Scan {
    std::vector<std::span<const std::byte>> units;  // whole access units, views into the stream
    std::vector<bool> major;                        // per unit: it opens with a major sync
    iclforge::truehd::StreamInfo info;
    int channels = 0;         // of the best presentation the stream has
    int padding_samples = 0;  // the end-of-stream marker's count (libs/truehd ERRATA 14)
    std::uint32_t samples_per_unit = 0;
    std::vector<std::string> warnings;  // bytes left out; said, not fatal
};

// The units of `stream`, decoded lenient so that a container can still be written from a stream with
// a fault in it: `probe` and `decode` are where a fault is reported. `name` leads the sentences. The
// error is a sentence.
[[nodiscard]] std::expected<Scan, std::string> scan_stream(std::span<const std::byte> stream,
                                                           std::string_view name);

// "TrueHD" or "MLP".
[[nodiscard]] const char* codec_label(const iclforge::truehd::MajorSync& major_sync);

// The container's file, for a scanned stream. The error is a sentence.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> mux(Container container,
                                                                     const Scan& scan);

// The container a file name's extension asks for (.mkv .mka .webm; .mp4 .m4a .mov; .ts .m2ts, in any
// case); none for anything else, a bare stream included.
[[nodiscard]] std::optional<Container> container_of(std::string_view path);

}  // namespace iclforge::apps::truehd_wrap
