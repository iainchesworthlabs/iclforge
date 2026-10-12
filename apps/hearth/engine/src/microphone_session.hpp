#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <filesystem>

#include "iclforge/lyrics/playhead.hpp"
#include "microphone_input.hpp"
#include "performance.hpp"
#include "take_recorder.hpp"

// A singer's microphone, from the person turning it on to what it heard (planning/karaoke-
// microphone.md, M4): the input, a level meter, the loss of the device, and a Performance against
// the reference once there is one. Qt-free and clockless: the caller says what time it is, so a
// test runs it as fast as it likes and the window runs it from a timer.
//
// What it does not do is as deliberate as what it does:
//   * nothing opens until start() (MI2), and stop() closes it;
//   * nothing is stored: the samples are read, measured and dropped (PV2, RC1 - a recording is its
//     own thing with its own switch);
//   * the score is withheld unless the person has said they wear headphones (SC5, until a check
//     that the microphone is not just the speakers exists): bleed reads as perfect singing.

namespace iclforge::hearth {

struct MicrophoneSnapshot {
    bool open = false;
    // The input's RMS over the last block, smoothed with a half second release, in dBFS; -120 when
    // there is nothing. And whether a sample reached full scale in the last second.
    float level_db = -120.0F;
    bool clipping = false;
    // How long the stream has been delivering silence (exact zeros), in seconds: a muted or
    // unplugged microphone looks like this and a person should be told.
    double silent_seconds = 0.0;
    // What the person said about their equipment, and what the score is therefore worth.
    bool headphones = false;
    bool score_withheld = true;

    std::optional<PerformanceLive> live{};
    pitch::Tally total{};
    std::optional<std::int32_t> lag_ms{};
    std::optional<double> key_offset_cents{};
    std::uint64_t unplaced_frames = 0;

    // A take being recorded (RC1: its own state, not the microphone's): how long it is so far.
    bool taking = false;
    double take_seconds = 0.0;
    // Why the take stopped by itself (the disk filled), empty otherwise.
    std::string take_error;

    // The alignment in force: what was declared and what the person moved.
    std::int64_t input_delay_ms = 0;
    std::int64_t offset_ms = 0;
    // The platform gave no capture times, so the voice was placed by when it was read.
    bool timing_estimated = false;
    std::uint32_t period_frames = 0;
    std::uint32_t sample_rate = 0;
};

enum class MicrophoneHealth : std::uint8_t {
    kOk,
    // Open, and nothing has arrived for three seconds: the device is there and not delivering.
    kStalled,
    // The device went away under the stream.
    kGone,
    kNotOpen,
};

class MicrophoneSession {
   public:
    explicit MicrophoneSession(std::unique_ptr<MicrophoneInput> input);

    [[nodiscard]] std::vector<InputDevice> devices() { return input_->devices(); }

    // Opens `device_id` (empty: the default input). The error is a sentence for the page.
    [[nodiscard]] std::expected<void, std::string> start(const std::string& device_id);
    // Closes it and forgets what the singer did; the reference stays.
    void stop();
    // Forgets what the singer did and scores from nothing, still open: a new take.
    void clear_singer() { rebuild_performance(); }
    [[nodiscard]] bool open() const { return open_; }

    // The reference melody the singer is scored against, once it has been read; nothing
    // withdraws one (the item changed, or there is none).
    void set_reference(std::optional<pitch::Contour> reference);
    [[nodiscard]] bool has_reference() const { return reference_.has_value(); }

    // The engine's heard position: the item is at `position_ms`, moving at 1 when it plays and 0
    // when it is held, as of `now_ns` on the steady clock.
    void anchor(std::int64_t position_ms, bool playing, std::int64_t now_ns);

    // What the person says and moves.
    void set_headphones(bool headphones) { headphones_ = headphones; }
    void set_input_delay_ms(std::int64_t ms);
    void set_offset_ms(std::int64_t ms);
    void set_score_options(const pitch::ScoreOptions& options);

    // A take: the dry voice, mono, kept as a file until end_take() (planning/karaoke-microphone.md,
    // M6). Only while the microphone is open; closing the microphone ends it, and keeps what was
    // written. `partial` carries what the caller knows (the item, the reference, the headphones
    // answer); the alignment and where the take starts in the item are filled in here.
    [[nodiscard]] std::expected<void, std::string> begin_take(const std::filesystem::path& file,
                                                              const TakeMetadata& partial);
    [[nodiscard]] bool taking() const { return recorder_.recording(); }
    // Ends the take and writes its description. The error is a sentence.
    [[nodiscard]] std::expected<TakeInfo, std::string> end_take();
    // Ends it and removes the file.
    void discard_take();
    // A take the session ended itself (the disk failed): what was written is a take, handed over
    // once.
    [[nodiscard]] std::optional<TakeInfo> take_ended_by_itself() {
        std::optional<TakeInfo> out = std::move(ended_take_);
        ended_take_.reset();
        return out;
    }

    // Reads what the microphone has captured, measures and feeds it. Called from a timer a few
    // times a second at least; `now_ns` is the steady clock.
    MicrophoneHealth pump(std::int64_t now_ns);

    [[nodiscard]] MicrophoneSnapshot snapshot() const;

    // What a view of the performance is drawn from (planning/karaoke-microphone.md, M5). All of it
    // is empty or null until a reference has been read and the microphone is open.
    //
    // The counts for [start_ms, end_ms) of the item: a line's result.
    [[nodiscard]] pitch::Tally tally(std::int64_t start_ms, std::int64_t end_ms) const;
    // The singer's last placed frames, oldest first, or null.
    [[nodiscard]] const std::deque<TracePoint>* trace() const;
    // The reference, and the tolerance a frame is judged against.
    [[nodiscard]] const std::optional<pitch::Contour>& reference() const { return reference_; }
    [[nodiscard]] const pitch::ScoreOptions& score_options() const { return score_options_; }

   private:
    void rebuild_performance();
    [[nodiscard]] PerformanceOptions performance_options() const;
    // Writes the block just read to the take, and ends the take if the disk fails.
    void record_block(std::size_t channels, std::size_t frames);

    std::unique_ptr<MicrophoneInput> input_;
    bool open_ = false;
    std::optional<pitch::Contour> reference_{};
    std::optional<Performance> performance_{};
    pitch::ScoreOptions score_options_{};
    std::int64_t input_delay_ms_ = 0;
    std::int64_t offset_ms_ = 0;
    bool headphones_ = false;

    // The last anchor, re-applied when a Performance is made after it.
    struct Anchor {
        std::int64_t position_ms = 0;
        double speed = 0.0;
        std::int64_t at_ns = 0;
    };
    std::optional<Anchor> anchor_{};

    float level_db_ = -120.0F;
    std::int64_t last_level_ns_ = 0;
    std::int64_t clipped_at_ns_ = 0;
    bool ever_clipped_ = false;
    std::int64_t last_audio_ns_ = 0;

    lyrics::Playhead playhead_{};
    TakeRecorder recorder_{};
    TakeMetadata take_{};
    bool take_started_ = false;
    std::optional<TakeInfo> ended_take_{};
    std::string take_error_;
    std::vector<float> take_mono_;
    std::int64_t silent_since_ns_ = 0;
    bool silent_ = false;
    double silent_seconds_ = 0.0;
    bool timing_estimated_ = false;
    InputBlock block_{};
};

}  // namespace iclforge::hearth
