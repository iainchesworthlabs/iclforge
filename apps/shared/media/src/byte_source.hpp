#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

// Where a program's bytes are read from when it does not hold all of them in memory: a TrueHD file
// is the one stream Hearth plays that can run to gigabytes (planning/truehd-parity-hearth-player.md,
// row 5), and a container's audio track is the same stream with other tracks between its frames.
//
// A source answers reads at offsets. It is read from one thread at a time, by whoever owns it, and
// never needs to be seekable in any way but by offset: a file stream, a memory-mapped file or a
// vector are all one. The application supplies the file-backed one; this header and the engine have
// no file I/O of their own.

namespace iclforge::apps {

class ByteSource {
public:
    ByteSource() = default;
    ByteSource(const ByteSource&) = delete;
    ByteSource& operator=(const ByteSource&) = delete;
    virtual ~ByteSource() = default;

    // The number of bytes the source holds.
    [[nodiscard]] virtual std::uint64_t size() const = 0;

    // Copies up to out.size() bytes starting at `offset` to `out` and returns how many were copied:
    // fewer than asked only at the end of the source, and none past it or when it cannot be read.
    [[nodiscard]] virtual std::size_t read(std::uint64_t offset, std::span<std::byte> out) = 0;
};

// A source over bytes held in memory, which it owns.
class MemoryByteSource final : public ByteSource {
public:
    explicit MemoryByteSource(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

    [[nodiscard]] std::uint64_t size() const override { return bytes_.size(); }

    [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
        if (offset >= bytes_.size()) {
            return 0;
        }
        const std::size_t count =
            std::min<std::size_t>(out.size(), bytes_.size() - static_cast<std::size_t>(offset));
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), count, out.begin());
        return count;
    }

private:
    std::vector<std::byte> bytes_;
};

}  // namespace iclforge::apps
