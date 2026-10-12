#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/lyrics/playhead.hpp"
#include "iclforge/pitch/reference.hpp"
#include "iclforge/pitch/scorer.hpp"
#include "iclforge/pitch/tracker.hpp"

// A singer's performance against a reference melody (planning/karaoke-microphone.md, M4: the
// microphone session's engine half). It owns no device and no thread: the caller hands it two
// things, and reads back what the singer did.
//
//   anchor()  where in the item the sound being heard is, at a time on the steady clock. The
//             engine's heard position (PlayPosition::heard) is what to pass: the device's clock
//             less its output path's delay, which is what a person singing along is hearing.
//   feed()    a block of the microphone's samples (mono, at the input rate) and the time its
//             first sample was captured, on the same clock. Audio's Capture gives each packet a
//             stamp and CaptureTimeline turns them into this time; nothing here knows that.
//
// A frame of the singer's voice is placed in the item at the heard position at the moment the sound
// reached the microphone, less the microphone's own declared delay (a webcam's adds tens of
// milliseconds, findings F2), plus an offset the person can move. That is the whole of the
// alignment, and it is exposed so that it can be shown and corrected.
//
// Nothing is scored while the item is not moving (paused, or held at the end), or before the first
// anchor: the singer's frames are counted as unplaced, not as wrong.

namespace iclforge::hearth {

struct PerformanceOptions {
    // The rate of the samples fed.
    std::uint32_t input_rate = 48000;
    // How long after a sound reaches the microphone its samples are stamped, in milliseconds:
    // taken off the capture time before the voice is placed. 0 until measured; the application
    // shows it, so it is declared, never assumed.
    std::int64_t input_delay_ms = 0;
    // Where the person has moved the alignment, in milliseconds: positive places the voice later in
    // the item (the singer is behind the words).
    std::int64_t offset_ms = 0;
    pitch::ScoreOptions score{};
    // The tracker's gates, as pitch::TrackerConfig has them.
    float clarity_min = 0.6F;
    float level_floor_db = -55.0F;
};

// What the singer is doing now, for a live view.
struct PerformanceLive {
    // The item position the latest frame was placed at.
    std::int64_t item_ms = 0;
    bool voiced = false;
    float hz = 0.0F;
    float level_db = -120.0F;
    // Against the reference at that position, in cents after folding octaves and the key shift:
    // positive is sharp. Nothing where the reference has no note there or the singer is not voiced.
    std::optional<double> cents_from_reference{};
    // The reference's pitch there, in Hz, 0 where it has none.
    float reference_hz = 0.0F;
};

// One placed frame of the singer's voice, for drawing: where in the item it was sung and its pitch,
// 0 when the singer was not voiced.
struct TracePoint {
    std::int64_t item_ms = 0;
    float hz = 0.0F;
};

class Performance {
   public:
    // How many placed frames are kept for drawing: 30 seconds at the 10 ms hop. The score does not
    // depend on them; a view of the last few seconds does.
    static constexpr std::size_t kTraceCapacity = 3000;

    // Nothing for a reference with no voiced point (there is nothing to score against), or an
    // input rate no tracker can work at.
    [[nodiscard]] static std::optional<Performance> make(pitch::Contour reference,
                                                         const PerformanceOptions& options = {});

    // The sound of the item is at `item_ms` at `at_ns`, and moves at `speed` times real time from
    // there: 1 while it plays, 0 held. Called whenever the position is read, which is every few
    // tens of milliseconds.
    void anchor(std::int64_t item_ms, std::int64_t at_ns, double speed);

    // The microphone's next samples, mono, contiguous with the last block fed, the first of them
    // captured at `first_sample_time_ns`. Blocks of any size are fine.
    void feed(std::span<const float> mono, std::int64_t first_sample_time_ns);

    // The same for interleaved channels, mixed down to one by their mean.
    void feed_interleaved(std::span<const float> samples, std::uint16_t channels,
                          std::int64_t first_sample_time_ns);

    // Forgets what the singer did (a new take, a seek to the start), keeping the reference and the
    // alignment.
    void clear_singer();

    // The alignment and the scoring, which a person may change while it runs.
    void set_input_delay_ms(std::int64_t ms) { options_.input_delay_ms = ms; }
    void set_offset_ms(std::int64_t ms) { options_.offset_ms = ms; }
    void set_score_options(const pitch::ScoreOptions& score) {
        options_.score = score;
        scorer_.set_options(score);
    }
    [[nodiscard]] const PerformanceOptions& options() const { return options_; }

    [[nodiscard]] pitch::Tally total() const { return scorer_.total(); }
    [[nodiscard]] pitch::Tally tally(std::int64_t start_ms, std::int64_t end_ms) const {
        return scorer_.tally(start_ms, end_ms);
    }
    // How early or late the singer is, in milliseconds, against the reference as aligned now.
    [[nodiscard]] std::optional<std::int32_t> lag_ms() const { return scorer_.best_lag_ms(); }
    // The key the singer is in, in cents from the reference after folding octaves.
    [[nodiscard]] std::optional<double> key_offset_cents() const {
        return scorer_.key_offset_cents();
    }

    // The singer's last placed frames in the order they were placed, oldest first: at most
    // kTraceCapacity. After a seek the item positions jump; a view breaks its line there.
    [[nodiscard]] const std::deque<TracePoint>& trace() const { return trace_; }

    [[nodiscard]] std::optional<PerformanceLive> live() const { return live_; }
    // Frames of the voice that could not be placed (before the first anchor, or while the item was
    // not moving): not scored, and not counted wrong.
    [[nodiscard]] std::uint64_t unplaced_frames() const { return unplaced_; }
    [[nodiscard]] std::uint64_t frames() const { return frames_; }

   private:
    Performance(pitch::Contour reference, const PerformanceOptions& options,
                pitch::Tracker tracker);

    struct Block {
        std::int64_t first_index = 0;  // the stream index of the block's first sample
        std::int64_t time_ns = 0;
    };

    void place(const pitch::Frame& frame);

    PerformanceOptions options_;
    pitch::Scorer scorer_;
    pitch::Tracker tracker_;
    lyrics::Playhead playhead_;

    std::int64_t samples_fed_ = 0;
    std::deque<Block> blocks_;
    std::vector<pitch::Frame> frames_scratch_;
    std::vector<float> mono_scratch_;

    std::optional<PerformanceLive> live_{};
    std::deque<TracePoint> trace_;
    std::uint64_t unplaced_ = 0;
    std::uint64_t frames_ = 0;
};

}  // namespace iclforge::hearth
