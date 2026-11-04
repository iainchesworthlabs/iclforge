#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "json_sink.hpp"

// The report of a TrueHD or MLP elementary stream: what `forge probe` says of one, held where Hearth
// and the Forge window can call it too (planning/truehd-parity-forge.md, row M1). Wire facts only:
// every number is read from the access units' headers, the major sync and the restart headers, or
// from a decode when the caller asks for the lossless check. Nothing here is a verdict on the
// specification: the channel meaning of TH spec §14 is carried as the library parses it
// (provisional, libs/truehd/ERRATA.md questions 51 to 55) and shown by the caller.
//
// Compiled into each application that uses it, like the rest of apps/shared/media/src: it needs
// iclforge::truehd, which Forge and Hearth link.

namespace iclforge::apps::truehd_probe {

// The data rate of a stream against what it declares (TH spec §18, as libs/truehd/ERRATA.md entry
// 99 records): the channel carries one unit at a time, and a unit takes duration = bytes x 8 x
// sample rate / declared rate samples to arrive. The stream says when each unit starts
// (input_timing) and, at a restart header, when it is decoded (output_timing); the lead is the
// samples from the one to the other. A unit is out of the law when
//   delta[n] = (input_timing[n] - input_timing[n-1]) mod 65536  <  duration[n-1]   (the one before had not arrived), or
//   delta[n]  <  nominal / 4                                                       (a start too soon after the last), or
//   lead[n]  <  max(duration[n], nominal)                                          (it arrives after its decode time,
//                                                                                    or leads by under a unit), or
//   lead[n]  >  75 ms.
// The last three bounds are what the streams the project holds show (ERRATA.md entry 99), not a
// reading of the specification's text. The law is per unit, with no queue carried forward: a stream
// that sends a burst early, as a real encoder does, is within it. The declared rate is
// MajorSync::peak_data_rate_bps() (question 12).
struct RateReport {
    std::uint64_t declared_bps = 0;
    std::uint64_t max_unit_rate_bps = 0;  // the largest unit's bytes over its own duration
    std::uint64_t violations = 0;         // units out of the law
    double worst_overrun_ms = 0.0;        // the most by which one was (a bound missed, in ms)
};

class RateModel {
   public:
    // The next access unit, whole; `first_sync` the major sync the unit carries, if any. Units
    // before the first major sync are not counted.
    void add(std::span<const std::byte> unit, const std::optional<truehd::MajorSync>& first_sync);
    // The same from the numbers: the unit's size, its input_timing, the output_timing of its
    // restart header if it has one that read, and the major sync it carries, if any.
    void add_unit(std::size_t unit_bytes, std::uint16_t input_timing,
                  std::optional<std::uint16_t> output_timing,
                  const std::optional<truehd::MajorSync>& first_sync);
    [[nodiscard]] const RateReport& report() const noexcept { return report_; }

   private:
    RateReport report_{};
    bool started_ = false;
    std::uint32_t rate_ = 0;
    int nominal_ = 0;
    std::uint16_t last_timing_ = 0;
    std::int64_t last_duration_ = 0;  // samples the last unit takes to arrive at the declared rate
    std::int64_t lead_ = 0;           // samples from the last unit's start to its decode time
    bool have_lead_ = false;          // a restart header has fixed the lead
    std::uint64_t units_ = 0;
};

// One of the stream's presentations: substream 0 is the 2 channel one, 1 the 6 channel one and 2
// the 8 channel one (ERRATA.md question 51).
struct Presentation {
    std::string name;                 // "2ch", "6ch" or "8ch"
    int substream = 0;
    int channels = 0;                 // what a decode of it delivers
    std::vector<std::string> layout;  // the channel names in decode order; empty when unnamed
};

// What a decode of every substream counted, for the lossless check (TH spec §7.3) and the other
// checks. A lenient decode, so a failure is counted and the decode goes on.
struct LosslessResult {
    std::uint64_t intervals_ok = 0;
    std::uint64_t intervals_failed = 0;
    std::uint64_t other_check_failures = 0;  // access unit, CRC and parity checks inside the decode
    std::uint64_t units_waiting = 0;         // units that waited for a restart header
    std::uint64_t decode_errors = 0;         // units the decoder could not read
};

// One access unit, for detail=frames.
struct FrameInfo {
    std::uint64_t index = 0;
    std::size_t offset = 0;  // in the stream given
    std::size_t bytes = 0;
    double time_seconds = 0.0;  // of its first sample
    bool major_sync = false;
    std::uint16_t input_timing = 0;  // TH spec §3.4
    int substreams = 0;
    std::array<std::size_t, truehd::kMaxSubstreams> substream_bytes{};  // each segment
    bool checks_ok = true;     // every segment with a parity and CRC byte passes
    bool extra_data = false;
};

struct Report {
    truehd::StreamInfo info;
    std::uint64_t units = 0;           // access units that read
    std::uint64_t major_syncs = 0;
    std::uint64_t segments = 0;          // segments with a parity and CRC byte
    std::uint64_t segment_failures = 0;  // of them, those that fail
    std::uint64_t extra_data_units = 0;
    std::uint64_t unreadable_units = 0;
    std::size_t skipped_bytes = 0;  // between units; not the leading bytes
    std::size_t leading_bytes = 0;  // ahead of the first unit: not a loss of the stream
    std::uint64_t bytes = 0;        // the units' bytes
    double duration_seconds = 0.0;  // units x samples per unit / rate
    double average_bps = 0.0;       // bytes x 8 over the duration
    RateReport rate{};
    // The distance between one unit with a major sync and the next, in units (one restart
    // interval), over the stream; 0 when there is one major sync or none.
    std::uint64_t restart_min_units = 0;
    std::uint64_t restart_max_units = 0;
    // The different channel meanings the major syncs carry (TrueHD only); the first is shown.
    std::vector<truehd::ChannelMeaning> meanings;
    std::vector<Presentation> presentations;
    // The word length: an MLP stream sends two (TH spec §1.4); a TrueHD stream sends none and a
    // decode delivers 24-bit words (the source's depth is not a field of the stream).
    std::optional<int> group1_bits;
    std::optional<int> group2_bits;
    std::optional<LosslessResult> lossless;
    std::vector<FrameInfo> frames;  // only with ReportOptions::frames
    [[nodiscard]] bool damaged() const noexcept {
        return segment_failures != 0 || unreadable_units != 0 || skipped_bytes != 0 ||
               (lossless && (lossless->intervals_failed != 0 || lossless->decode_errors != 0));
    }
};

struct ReportOptions {
    // Decode every substream (lenient, the samples discarded) for the lossless check. The cost is
    // that of a decode of the stream.
    bool lossless = false;
    // Keep one FrameInfo per unit that reads (a long stream has many).
    bool frames = false;
    // Called with each unit that reads, in order and with its index: a caller that surveys more
    // than the report does (forge's detail=evolution and structure) adds to it here.
    std::function<void(std::uint64_t, const truehd::AccessUnit&)> on_unit;
};

// kBadSync when `stream` holds no major sync (truehd::probe_stream).
[[nodiscard]] std::expected<Report, truehd::Error> build_report(std::span<const std::byte> stream,
                                                                const ReportOptions& options = {});

// "TrueHD" or "MLP".
[[nodiscard]] const char* codec_label(const truehd::MajorSync& major_sync) noexcept;

// The JSON callbacks the shared writer takes: what only the caller can write.
struct JsonExtras {
    // The value of "channel_meaning" (an object; the writer writes null for MLP).
    std::function<void(JsonSink&)> channel_meaning;
    // Further members of the "truehd" object, after the report's own.
    std::function<void(JsonSink&)> tail;
};

// The "truehd" member's value: an object. Keys (all additive to the first form): codec,
// sample_rate, substreams, channels, access_units, samples_per_access_unit, duration_seconds,
// bytes, average_bit_rate_bps, variable_rate, peak_data_rate, peak_data_rate_bps,
// rate_model{declared_bps, max_unit_rate_bps, violations, worst_overrun_ms},
// object_substream, presentations[{name, substream, channels, layout}], word_length{...},
// channel_meaning, restart_spacing{major_syncs, min_units, max_units}, extra_data_units,
// ac3_core_frames, major_syncs, segments_checked, segment_failures, unreadable_units,
// skipped_bytes, leading_bytes, lossless (null unless it was asked for).
void write_json(JsonSink& json, const Report& report, std::size_t ac3_core_frames,
                const JsonExtras& extras = {});

// The report's lines as `forge probe` prints them, `label` padded to 16 columns and `text` after.
// The caller prints the container's line before them and the channel meaning's lines between the
// two halves: `text_head` is codec to rate, `text_tail` is the rest.
struct TextLine {
    std::string label;
    std::string text;
};
[[nodiscard]] std::vector<TextLine> text_head(const Report& report);
[[nodiscard]] std::vector<TextLine> text_tail(const Report& report, std::size_t ac3_core_frames);
// detail=frames: one line per access unit.
[[nodiscard]] std::vector<std::string> frame_lines(const Report& report);

}  // namespace iclforge::apps::truehd_probe
