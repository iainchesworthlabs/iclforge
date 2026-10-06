#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac4/export.hpp"
#include "iclforge/ac4/core/toc.hpp"

// AC-4 in an elementary stream: Annex G.3.1's ac4_syncframe() and the walk
// over them, whole (scan()) or as a stream arrives (SyncFrameSplitter). A sync
// frame's raw_ac4_frame is what parse_raw_frame() (iclforge/ac4/toc.hpp) reads.

namespace iclforge::ac4 {

// Annex G.3.1 ac4_syncframe(). `raw_ac4_frame` is the frame_size-bounded
// span passed to parse_raw_frame() - everything between the frame_size
// field and the optional trailing crc_word.
struct SyncFrame {
    std::size_t offset = 0;
    std::uint16_t sync_word = 0;  // 0xAC40 or 0xAC41 (Annex G.4.1)
    std::span<const std::byte> raw_ac4_frame;
    // nullopt when sync_word == 0xAC40 (no crc_word transmitted); Annex
    // G.4.2's CRC-16 (poly x^16+x^15+x^2+1, init 0, no reflection, no
    // final XOR) otherwise.
    std::optional<bool> crc_ok;
};

struct ScanResult {
    std::vector<SyncFrame> frames;
    // Set when the walk stopped before consuming all of `data` - either a
    // sync word that did not match 0xAC40/0xAC41 (kLostSync) or a frame
    // whose declared frame_size runs past the end of `data` (kTruncated).
    // `frames` still holds everything found before that point.
    std::optional<Error> stopped_at;
    std::size_t stopped_at_offset = 0;
};

// Walks ac4_syncframe() elements back to back. Never throws; a malformed
// tail is reported via ScanResult::stopped_at rather than losing whatever
// parsed cleanly before it.
[[nodiscard]] ICLFORGE_AC4_EXPORT ScanResult scan(std::span<const std::byte> data);

// The storage a SyncFrameSplitter needs for a stream whose frames are all
// shorter than 64 KiB: every AC-4 frame this project has seen is, the largest
// a few kilobytes. frame_size's escape reaches 16 MiB (Annex G.3.1), which a
// caller expecting such frames sizes its storage for.
inline constexpr std::size_t kSplitterRecommendedBuffer = 65536 + 16;

// Sync frames from a stream that arrives in pieces - an HTTP body, a socket, a
// file read a block at a time - which scan() cannot walk until the last byte
// is in. The same framing as scan(), applied incrementally: each frame comes
// out once all of it has arrived, and a partial frame is held between reads.
// It owns no memory and allocates none; the caller's storage holds the frame
// being assembled, in the pattern of iclforge::ac3::io::AccessUnitAccumulator:
//
//     std::vector<std::byte> storage(iclforge::ac4::kSplitterRecommendedBuffer);
//     iclforge::ac4::SyncFrameSplitter splitter{storage};
//     for (;;) {
//         const auto next = splitter.next();
//         if (next.status == iclforge::ac4::SyncFrameSplitter::Status::kNeedMoreInput) {
//             const std::size_t n = read_from_somewhere(splitter.writable());
//             n == 0 ? splitter.finish() : splitter.commit(n);
//             continue;
//         }
//         if (next.status != iclforge::ac4::SyncFrameSplitter::Status::kFrame) break;
//         decoder.decode(next.frame.raw_ac4_frame);
//     }
//
// Where the stream does not start on a sync word, or something between frames
// is not a frame, the splitter skips to the next sync word and counts the
// bytes it skipped; a frame found that way is handed over only once a sync
// word follows it (or the stream ends there), so that a sync word's bit
// pattern inside a frame is not taken for one.
class ICLFORGE_AC4_EXPORT SyncFrameSplitter {
   public:
    enum class Status : std::uint8_t {
        // `frame` is one whole sync frame, its offset counted from the
        // stream's first byte. Its bytes are valid until the next call to
        // next(), writable() or commit(), which may move them.
        kFrame,
        // Feed more through writable() and commit(), or call finish() when
        // there is no more.
        kNeedMoreInput,
        // finish() was called and everything held has been handed over.
        kEndOfStream,
        // finish() was called with part of a frame held, which is dropped;
        // kEndOfStream follows.
        kTruncated,
        // The storage cannot hold the frame being assembled. Feeding more
        // does not help; the caller needs larger storage.
        kBufferTooSmall,
    };

    struct Result {
        Status status = Status::kNeedMoreInput;
        SyncFrame frame{};
    };

    explicit SyncFrameSplitter(std::span<std::byte> storage) noexcept : storage_(storage) {}

    // Where the caller appends: the storage after what is held, empty when it
    // is full.
    [[nodiscard]] std::span<std::byte> writable() noexcept;
    // How many of writable()'s bytes were written.
    void commit(std::size_t bytes) noexcept;
    // No more input will arrive.
    void finish() noexcept { finished_ = true; }

    [[nodiscard]] Result next() noexcept;

    // Bytes skipped looking for a sync word, over the splitter's life.
    [[nodiscard]] std::size_t resynchronised_bytes() const noexcept { return skipped_; }

   private:
    void consume(std::size_t count) noexcept;

    std::span<std::byte> storage_;
    std::size_t filled_ = 0;
    std::size_t handed_ = 0;    // the frame handed over last, still at the front
    std::size_t position_ = 0;  // the stream offset of storage_[0]
    std::size_t skipped_ = 0;
    bool resynchronising_ = false;
    bool finished_ = false;
    bool truncated_ = false;  // kTruncated reported
};

}  // namespace iclforge::ac4
