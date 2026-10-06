#include "iclforge/ac4/io/elementary.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "iclforge/base/crc16.hpp"

namespace iclforge::ac4 {

// --- Annex G: AC-4 sync frame (G.4.2's CRC is iclforge/base/crc16.hpp) ----------------------

ScanResult scan(std::span<const std::byte> data) {
    ScanResult result;
    std::size_t pos = 0;
    while (pos + 4 <= data.size()) {
        const auto sync = static_cast<std::uint16_t>((std::to_integer<unsigned>(data[pos]) << 8) |
                                                     std::to_integer<unsigned>(data[pos + 1]));
        if (sync != 0xAC40 && sync != 0xAC41) {
            result.stopped_at = Error::kLostSync;
            result.stopped_at_offset = pos;
            return result;
        }
        std::uint32_t frame_size = (std::to_integer<unsigned>(data[pos + 2]) << 8) |
                                   std::to_integer<unsigned>(data[pos + 3]);
        std::size_t header = 4;
        if (frame_size == 0xFFFF) {
            if (pos + 7 > data.size()) {
                result.stopped_at = Error::kTruncated;
                result.stopped_at_offset = pos;
                return result;
            }
            frame_size = (std::to_integer<unsigned>(data[pos + 4]) << 16) |
                         (std::to_integer<unsigned>(data[pos + 5]) << 8) |
                         std::to_integer<unsigned>(data[pos + 6]);
            header = 7;
        }
        const std::size_t frame_start = pos + header;
        const std::size_t frame_end = frame_start + frame_size;
        const bool has_crc = sync == 0xAC41;
        const std::size_t total = frame_end + (has_crc ? 2 : 0);
        if (frame_end > data.size() || total > data.size()) {
            result.stopped_at = Error::kTruncated;
            result.stopped_at_offset = pos;
            return result;
        }
        std::optional<bool> crc_ok;
        if (has_crc) {
            const auto want =
                static_cast<std::uint16_t>((std::to_integer<unsigned>(data[frame_end]) << 8) |
                                           std::to_integer<unsigned>(data[frame_end + 1]));
            crc_ok = base::crc16(data.subspan(pos + 2, frame_end - (pos + 2))) == want;
        }
        result.frames.push_back(SyncFrame{
            .offset = pos,
            .sync_word = sync,
            .raw_ac4_frame = data.subspan(frame_start, frame_size),
            .crc_ok = crc_ok,
        });
        pos = total;
    }
    return result;
}

// --- SyncFrameSplitter ----------------------------------------------------------

std::span<std::byte> SyncFrameSplitter::writable() noexcept {
    consume(handed_);
    handed_ = 0;
    return storage_.subspan(filled_);
}

void SyncFrameSplitter::commit(std::size_t bytes) noexcept {
    filled_ = std::min(filled_ + bytes, storage_.size());
}

void SyncFrameSplitter::consume(std::size_t count) noexcept {
    count = std::min(count, filled_);
    if (count == 0) {
        return;
    }
    std::memmove(storage_.data(), storage_.data() + count, filled_ - count);
    filled_ -= count;
    position_ += count;
}

SyncFrameSplitter::Result SyncFrameSplitter::next() noexcept {
    consume(handed_);
    handed_ = 0;
    const auto byte = [this](std::size_t i) { return std::to_integer<unsigned>(storage_[i]); };
    const auto is_sync = [&byte](std::size_t i) {
        return byte(i) == 0xAC && (byte(i + 1) == 0x40 || byte(i + 1) == 0x41);
    };
    // What is held when no whole frame is: more is needed, or at the end
    // the part of a frame left over is dropped.
    const auto wait = [this]() {
        if (!finished_) {
            return Result{.status = Status::kNeedMoreInput};
        }
        if (filled_ > 0 && !truncated_) {
            truncated_ = true;
            consume(filled_);
            return Result{.status = Status::kTruncated};
        }
        consume(filled_);
        return Result{.status = Status::kEndOfStream};
    };
    // Skips to the next sync word after the first byte held; with none held,
    // skips everything but a last byte of 0xAC, which may begin a sync word
    // the next read completes. Called with two bytes or more held.
    const auto skip = [&]() {
        std::size_t at = 1;
        while (at + 1 < filled_ && !is_sync(at)) {
            ++at;
        }
        const bool found = at + 1 < filled_;
        const std::size_t count = found || byte(at) == 0xAC ? at : at + 1;
        skipped_ += count;
        resynchronising_ = true;
        consume(count);
    };
    for (;;) {
        if (filled_ < 2) {
            return wait();
        }
        if (!is_sync(0)) {
            skip();
            continue;
        }
        if (filled_ < 4) {
            return wait();
        }
        const auto sync = static_cast<std::uint16_t>((byte(0) << 8U) | byte(1));
        std::size_t header = 4;
        std::size_t frame_size = (byte(2) << 8U) | byte(3);
        if (frame_size == 0xFFFF) {
            if (filled_ < 7) {
                return wait();
            }
            frame_size = (byte(4) << 16U) | (byte(5) << 8U) | byte(6);
            header = 7;
        }
        const bool has_crc = sync == 0xAC41;
        const std::size_t total = header + frame_size + (has_crc ? 2U : 0U);
        if (total > storage_.size()) {
            // A sync word found by skipping may be a frame's bits, whose size
            // means nothing: try the next one. A stream that starts on a sync
            // word does not get that doubt.
            if (resynchronising_) {
                skip();
                continue;
            }
            return Result{.status = Status::kBufferTooSmall};
        }
        if (filled_ < total) {
            return wait();
        }
        if (resynchronising_ && !finished_) {
            // Confirmed by the sync word that follows, where the storage can
            // hold it.
            if (total + 2 <= storage_.size()) {
                if (filled_ < total + 2) {
                    return Result{.status = Status::kNeedMoreInput};
                }
                if (!is_sync(total)) {
                    skip();
                    continue;
                }
            }
        }
        resynchronising_ = false;
        std::optional<bool> crc_ok;
        if (has_crc) {
            const auto want = static_cast<std::uint16_t>((byte(total - 2) << 8U) | byte(total - 1));
            crc_ok = base::crc16(std::span<const std::byte>(storage_).subspan(2, total - 4)) == want;
        }
        handed_ = total;
        return Result{
            .status = Status::kFrame,
            .frame = SyncFrame{
                .offset = position_,
                .sync_word = sync,
                .raw_ac4_frame = std::span<const std::byte>(storage_).subspan(header, frame_size),
                .crc_ok = crc_ok}};
    }
}

}  // namespace iclforge::ac4
