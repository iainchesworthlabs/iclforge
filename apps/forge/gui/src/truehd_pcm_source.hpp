#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "byte_source.hpp"
#include "container_input.hpp"
#include "probe_truehd.hpp"
#include "truehd_stream.hpp"

// The decoded audio of a TrueHD or MLP file, a window at a time, for the Forge window's player and
// QC. A two-hour film at 48 kHz and eight
// channels is about 11 GB decoded, and the stream itself is larger than a machine may hold, so
// nothing here keeps more than one access unit's samples ahead of the caller: the file is read a
// window at a time by the TruehdCursor Hearth plays from (apps/shared/media/src/truehd_stream.hpp),
// its index says where a decode can begin and how many samples the stream holds, and a seek decodes
// from the restart before the time asked.
//
// The file is a bare stream, or a Matroska, MP4 or Blu-ray transport stream whose first TrueHD track
// is played. A Matroska or MP4 track is read where it lies in the file (iclforge::apps::ExtentSource,
// as Hearth reads one), so a remux is never held beside its lossless track; a transport stream is read
// whole, as its reader needs. An MP4 edit list's priming and padding are decoded and not delivered
// (ElementaryStreamResult::trim), so the stream starts and ends where the container says it does.
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
    // The container the stream was read from (kUnknown for a bare stream), and, for an MP4 whose edit
    // list has a shape the player does not apply, the sentence the reader gave.
    iclforge::apps::ContainerKind container = iclforge::apps::ContainerKind::kUnknown;
    std::string trim_note;
    // Samples at the start of the stream's own count that are not delivered (an edit list's priming);
    // `frames` is already net of them and of the trimmed end.
    std::uint64_t skipped_frames = 0;
};

class TruehdPcmSource {
   public:
    // Reads the file's unit lengths once (the index) and decodes the first unit for the format. The
    // error is a sentence for the dialog. `no_truehd_track`, when given, is set when the file is a
    // container that is sound and holds no TrueHD track, so that a caller can hand it to the readers
    // of the other codecs instead of reporting the error.
    [[nodiscard]] static std::expected<std::unique_ptr<TruehdPcmSource>, std::string> open(
        const std::filesystem::path& path,
        iclforge::truehd::Presentation presentation = iclforge::truehd::Presentation::kBest,
        bool* no_truehd_track = nullptr);

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

    [[nodiscard]] std::string scan();
    void seek_to(std::uint64_t frame);
    // Decodes the next unit into pending_. False at the end of the file.
    [[nodiscard]] bool decode_next_unit();

    std::filesystem::path path_;
    iclforge::truehd::Presentation presentation_ = iclforge::truehd::Presentation::kBest;
    std::shared_ptr<iclforge::apps::ByteSource> bytes_;
    std::unique_ptr<iclforge::apps::TruehdCursor> cursor_;
    iclforge::apps::TruehdIndex index_;
    TruehdPcmInfo info_;
    // Where the delivered stream begins in the stream's own samples, and the samples that stream
    // holds: read() takes and gives positions in the delivered stream.
    std::uint64_t skip_ = 0;
    std::uint64_t stream_samples_ = 0;

    std::unique_ptr<iclforge::truehd::Decoder> decoder_;
    bool ended_ = false;  // the cursor is out of units, or the stream changed shape
    std::vector<std::vector<float>> pending_;  // decoded, from pending_start_
    std::uint64_t pending_start_ = 0;          // sample index of pending_[*][0]
    std::uint64_t decoded_end_ = 0;            // sample index after the last decoded sample
    std::uint64_t decode_errors_ = 0;
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
