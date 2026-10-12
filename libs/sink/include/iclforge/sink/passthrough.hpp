#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sink/sink.hpp"

// A sink output that hands the coded stream to a receiver as it is (planning/hearth-android.md,
// "How the audio is carried"): each Sendspin burst chunk is rebuilt into the IEC 61937 burst it
// came from (iclforge::containers::iec61937::burst_from_parts), written to a link at the time it
// is due, and the link is kept alive between them with null bursts, never silence, so that a
// receiver keeps its lock.
//
// The link is a seam, as the program's outputs are: on Android it is an AudioTrack
// (apps/hearth/android), in a test a model of one, so that what is decided here (which burst goes
// into which slot, what is late, what is dropped) is tested on the host.
//
// What the clock can promise is a burst's length: a rigid stream cannot be nudged by less
// (planning/hearth-android.md, D10). A burst that is due more than half a burst after the slot it
// would fill is dropped and counted, and one that is not due for more than half a burst is waited
// for with null bursts, also counted; so a pass-through member of a group is within a burst of the
// others, and says how often it was not.

namespace iclforge::sink {

// What one stream looks like on the link.
struct LinkFormat {
    sendspin::player::DataType data_type = sendspin::player::DataType::kAc3;
    // The content's sample rate. The carrier the link itself runs at is `carrier_ratio` times it:
    // 1 for AC-3, 4 for E-AC-3 and for AC-4 HBR4.
    std::uint32_t sample_rate = 48000;
    std::uint32_t carrier_ratio = 1;
    // One burst on the link, in bytes, and the content frames it lasts.
    std::size_t burst_bytes = 0;
    std::uint32_t frames_per_burst = 1536;
    // The burst's own data type word, for a link that opens a track by its codec (Pc as the
    // Sendspin chunk carried it).
    std::uint16_t pc = 0;
};

// Where a link has got to, in content frames since it opened or was flushed.
struct LinkPosition {
    // Handed to the device and taken by it. Underrun silence counts as taken, as the device's own
    // clock counts it.
    std::uint64_t frames_played = 0;
    // Handed over and not yet taken.
    std::uint64_t frames_queued = 0;
};

class BurstLink {
   public:
    BurstLink() = default;
    virtual ~BurstLink() = default;
    BurstLink(const BurstLink&) = delete;
    BurstLink& operator=(const BurstLink&) = delete;
    BurstLink(BurstLink&&) = delete;
    BurstLink& operator=(BurstLink&&) = delete;

    // Opens the link for `format`. A refusal says why: the route does not take that format.
    [[nodiscard]] virtual std::expected<void, std::string> open(const LinkFormat& format) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;
    // One whole burst. False when the link has no room for it now, which is not an error: the
    // caller tries again at its next step.
    [[nodiscard]] virtual bool submit(std::span<const std::byte> burst) = 0;
    [[nodiscard]] virtual std::optional<LinkPosition> position() const = 0;
    // Drops what has been handed over and not played; position() counts from zero again.
    virtual void flush() = 0;
    // Stops the link without closing it, and starts it again. The link's lock is lost with it.
    virtual void pause(bool paused) = 0;
    // The gaps the device played where no burst was queued, since the link opened.
    [[nodiscard]] virtual std::uint64_t underruns() const = 0;
};

struct PassthroughOptions {
    // How many bursts the link is kept filled to: the Shield took 4 ahead without an underrun and
    // not 3 (planning/hearth-android-findings.md, F12), so 6 leaves two bursts of margin for a
    // step that is late.
    std::uint32_t bursts_ahead = 6;
    // How many bursts are queued before the device is expected to start moving: it did not with
    // fewer than 8.
    std::uint32_t prime_bursts = 8;
    // How long from the first burst handed over to the device's first movement, which is as much
    // as the time to a first report of the position; used until the position moves.
    std::int64_t start_latency_us = 300'000;
    // How long the link is kept alive with null bursts after a stream ends, so that the next
    // one, which is usually a song later, does not wait for the receiver to relock.
    std::int64_t idle_hold_us = 3'000'000;
    // How many position samples the link's clock is the earliest of: about eight seconds at the
    // pump's pace. The link's own drift is a few parts per million, so the longer the window the less
    // the grid of slots wobbles, and a burst whose time falls near the edge of a slot does not flip
    // between being placed and being dropped.
    std::size_t clock_window = 2048;
    // A burst is dropped when it is more than this much of its length later than the slot it would
    // fill, and a null burst is inserted while it is more than half its length earlier. The band
    // between (a quarter of a burst) is the hysteresis: a burst that is a little late is played a
    // little late, and not dropped for what the estimate of the clock does between two looks.
    double late_fraction = 0.75;
    // How often a link that went away while a stream is open is opened again: a TV box loses its HDMI
    // route when the display sleeps, and has it again when it wakes.
    std::int64_t reopen_period_us = 1'000'000;
    // Told what the output did that a person tuning it would want to know: how long the link took to
    // open, when the first burst was placed against when it was due, and each burst dropped for being
    // late and by how much. Called with the output's lock held, so it must not call back into it.
    std::function<void(std::string_view)> note;
};

class PassthroughOutput : public BurstStreamOutput {
   public:
    // `now_us` is the clock a chunk's local time is on (the sink's steady clock).
    PassthroughOutput(BurstLink& link, std::function<std::int64_t()> now_us, PassthroughOptions options = {});
    ~PassthroughOutput() override;
    PassthroughOutput(const PassthroughOutput&) = delete;
    PassthroughOutput& operator=(const PassthroughOutput&) = delete;
    PassthroughOutput(PassthroughOutput&&) = delete;
    PassthroughOutput& operator=(PassthroughOutput&&) = delete;

    // --- sink::BurstStreamOutput ---------------------------------------------------------
    bool start(const sendspin::player::StreamStart& stream) override;
    void clear() override;
    void end() override;
    // The burst is rebuilt and queued for its time; this does not touch the link, so it may be
    // called under a session's lock.
    void write(const sendspin::BurstChunk& chunk, std::int64_t local_time) override;
    void set_karaoke(sendspin::player::Karaoke, sendspin::player::KaraokeVocals,
                     const std::array<double, 3>&) override {}
    [[nodiscard]] OutputCounts counts() const override;
    [[nodiscard]] const std::filesystem::path& file() const override { return no_file_; }
    [[nodiscard]] const std::optional<sendspin::player::DecoderReport>& decoder() const override {
        return decoder_;
    }

    // Another source holds the device: nothing is written to the link while it does, and what comes
    // due is dropped; given back, the stream carries on from what is due.
    void set_paused(bool paused);

    // Whether a stream is open on the link now (started, not ended).
    [[nodiscard]] bool streaming() const;

    // One step of what keeps the link filled: reads the position, decides each slot to be filled
    // and fills as many as the target depth asks. Called every few milliseconds, by run() or by a
    // test with its own clock.
    void pump();

    // Calls pump() every `period` on a thread of its own until stop_thread() or the destructor.
    void run(std::chrono::microseconds period = std::chrono::milliseconds(4));
    void stop_thread();

   private:
    struct Pending {
        std::vector<std::byte> burst;
        std::int64_t due_us = 0;
    };

    void fill_slots_locked(std::int64_t now_us);
    void note_position_locked(std::int64_t now_us, const LinkPosition& position);
    [[nodiscard]] std::int64_t frames_to_us(std::uint64_t frames) const;
    void reset_clock_locked();

    BurstLink* link_;
    std::function<std::int64_t()> now_us_;
    PassthroughOptions options_;

    mutable std::mutex mutex_;
    // The format the link was opened with, and whether a stream is open on it.
    std::optional<LinkFormat> format_;
    bool streaming_ = false;
    bool paused_ = false;
    std::int64_t ended_at_us_ = 0;
    std::vector<std::byte> null_burst_;
    std::deque<Pending> pending_;
    // The time the link's frame 0 played at, as the earliest of the last samples of "now less what
    // the device says it has played": the device's position is a step behind its clock, never
    // ahead of it, so the least is the closest.
    std::deque<std::int64_t> clock_samples_;
    std::optional<std::int64_t> origin_us_;
    bool moving_ = false;
    std::int64_t opened_at_us_ = 0;
    // When the stream started, and whether its first real burst has been placed (for note).
    std::int64_t started_at_us_ = 0;
    bool first_placed_ = false;
    std::int64_t reopened_at_us_ = 0;
    OutputCounts counts_;
    // The link's own count when its device began to move: before that it has nothing to play and is
    // not starved, whatever it counts.
    std::uint64_t underruns_base_ = 0;
    bool underruns_based_ = false;

    std::optional<sendspin::player::DecoderReport> decoder_;
    std::filesystem::path no_file_;

    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
};

}  // namespace iclforge::sink
