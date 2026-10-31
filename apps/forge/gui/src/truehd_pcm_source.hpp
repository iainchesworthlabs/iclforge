#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "probe_truehd.hpp"

// The decoded audio of a TrueHD or MLP file, a window at a time, for the Forge window's player and
// QC (planning/truehd-parity-forge.md, row U2 and question 12). A two-hour film at 48 kHz and eight
// channels is about 11 GB decoded, and the stream itself is larger than a machine may hold, so
// nothing here keeps more than one access unit's samples ahead of the caller: the file is read in
// blocks, an index of the units that carry a major sync (where a decode can begin) is built by one
// pass over the unit lengths, and a seek decodes from the restart before the time asked.
//
// Qt-free, so the Catch2 cases of iclforge-forge-gui-tests hold it. One caller at a time: the
// calls are serialised by a mutex, which is not a promise that two readers get sensible positions.

namespace forge_gui {

struct TruehdPcmInfo {
    bool mlp = false;
    std::uint32_t sample_rate = 0;
    int channels = 0;
    std::vector<iclforge::truehd::Channel> layout;  // the channels in decode order; empty if unnamed
    std::uint64_t frames = 0;                       // samples per channel, from the unit count
    std::uint64_t units = 0;
    std::uint64_t restarts = 0;  // units that carry a major sync
    int samples_per_unit = 0;
    // TH spec section 18 over the unit lengths (the declared peak rate, the largest unit, the units
    // that would arrive late), from the index pass.
    iclforge::apps::truehd_probe::RateReport rate{};
};

class TruehdPcmSource {
   public:
    // Reads the file's unit lengths once (the index) and decodes the first unit for the format. The
    // error is a sentence for the dialog.
    [[nodiscard]] static std::expected<std::unique_ptr<TruehdPcmSource>, std::string> open(
        const std::filesystem::path& path,
        iclforge::truehd::Presentation presentation = iclforge::truehd::Presentation::kBest);

    [[nodiscard]] const TruehdPcmInfo& info() const noexcept { return info_; }

    // Decodes up to `frames` samples per channel starting at sample `at` into `planar` (one vector
    // per channel, resized to what was delivered, scaled to +-1.0). Returns the number delivered:
    // fewer than asked only at the end of the stream, and 0 at or past it. A unit that does not
    // decode is counted (decode_errors()) and the decode goes on from the next restart.
    [[nodiscard]] std::size_t read(std::uint64_t at, std::size_t frames,
                                   std::vector<std::vector<float>>& planar);

    [[nodiscard]] std::uint64_t decode_errors() const noexcept { return decode_errors_; }
    // What the decoder has counted so far (the lossless check of every restart interval, the other
    // checks); a decode of the whole stream from the start has counted all of it.
    [[nodiscard]] iclforge::truehd::DecoderCounters counters() const {
        return decoder_ ? decoder_->counters() : iclforge::truehd::DecoderCounters{};
    }

    TruehdPcmSource(const TruehdPcmSource&) = delete;
    TruehdPcmSource& operator=(const TruehdPcmSource&) = delete;

   private:
    TruehdPcmSource() = default;

    struct Restart {
        std::uint64_t unit = 0;
        std::uint64_t offset = 0;
    };

    [[nodiscard]] std::string scan();
    void seek_to(std::uint64_t frame);
    // Decodes the next unit into pending_. False at the end of the file.
    [[nodiscard]] bool decode_next_unit();

    std::filesystem::path path_;
    iclforge::truehd::Presentation presentation_ = iclforge::truehd::Presentation::kBest;
    std::ifstream file_;
    std::uint64_t file_size_ = 0;
    std::uint64_t first_offset_ = 0;
    TruehdPcmInfo info_;
    std::vector<Restart> restarts_;

    std::unique_ptr<iclforge::truehd::Decoder> decoder_;
    std::uint64_t next_offset_ = 0;   // of the next unit to read
    std::uint64_t next_unit_ = 0;
    std::vector<std::vector<float>> pending_;  // decoded, from pending_start_
    std::uint64_t pending_start_ = 0;          // sample index of pending_[*][0]
    std::uint64_t decoded_end_ = 0;            // sample index after the last decoded sample
    std::uint64_t decode_errors_ = 0;
    std::vector<std::byte> unit_buffer_;
    std::mutex mutex_;
};

}  // namespace forge_gui

namespace forge_gui {

// The WAVE_FORMAT_EXTENSIBLE speaker mask `forge decode` writes for `layout`: the speakers' bits when
// the stream's channel order is the WAV's own (5.1 is), else 0.
[[nodiscard]] std::uint32_t truehd_wav_channel_mask(
    std::span<const iclforge::truehd::Channel> layout, int channels);

// Writes the whole decode of `source` as the 24-bit WAV `forge decode` writes (the same 68-byte
// header and the same samples), a window at a time. An empty string, or a sentence for the dialog.
[[nodiscard]] std::string write_truehd_wav24(TruehdPcmSource& source,
                                             const std::filesystem::path& path);

}  // namespace forge_gui
