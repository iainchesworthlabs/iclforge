#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "iclforge/render/layout.hpp"
#include "iclforge/render/routing.hpp"
#include "iclforge/render/speaker_trim.hpp"
#include "iclforge/render/trim_delay.hpp"

// What a player does to each output between the patch and the sink: the level and delay of every speaker
// (trim_delay.hpp), from two places that can both speak.
//
//   The listener's own table (speaker_trim.hpp): a level and a distance for each speaker role, kept on the board,
//   resolved through the layout and the patch to the outputs.
//   A server's settings (planning/hearth-sendspin-extension.md): a trim and a delay for each OUTPUT, which replace
//   the board's own whole while the server's settings stand - "a key left out takes the board's own value", and
//   the board's own value is the table. So a server that sends trims and no delays leaves the delays to the
//   table, and one that sends neither leaves the board as the listener set it.
//
// Four kinds of change, each cheap in its own way:
//
//   set_server()  the server's settings changed: stored, and in force at the next configure();
//   configure()   the patch or the outputs changed: every output set up from scratch, delay lines emptied;
//   set_table()   the listener changed a number, perhaps while a song plays: only what differs moves. A level
//                 ramps over the next block; a delay changes the output's line, which drops that speaker for as
//                 long as the new delay (at most the build's limit), once; an output whose delay is the same is
//                 not touched, and its line plays on;
//   reset()       a new stream: delay lines emptied, ramps over.
//
// The farthest speaker is never delayed (speaker_trim.hpp), so none of this moves the time the board plays a
// frame at, the time a server asked for: only the nearer speakers are late, by the sound's travel the farther
// ones have over them.
//
// A table that is all defaults and no server trims leave every output at 0 dB with no delay, and process() does
// nothing at all: the samples out are the samples in, bit for bit, and the cost is the loop over the outputs.
// Standard library only; the delay lines are the caller's storage (TrimDelay::storage_floats()), in PSRAM on a
// board that has some. Tested on the host in libs/render/tests/test_output_trim.cpp.

namespace iclforge::render {

class OutputTrim {
   public:
    static constexpr std::size_t kMaxOutputs = speaker_trim::kOutputs;

    // The delay lines (`storage`, for `capacity` outputs of `max_delay_samples` each at most), and the rate the
    // distances are turned into samples at. Not while the stage runs. Storage too small for a delay of that length
    // holds what it can: the limit is storage.size() / capacity.
    void setup(std::span<float> storage, std::size_t capacity, std::size_t max_delay_samples, std::uint32_t sample_rate_hz) {
        storage_ = storage;
        capacity_ = std::clamp<std::size_t>(capacity, 1, kMaxOutputs);
        max_delay_samples_ = std::min(max_delay_samples, storage.size() / capacity_);
        sample_rate_hz_ = sample_rate_hz;
    }

    [[nodiscard]] std::size_t max_delay_samples() const { return max_delay_samples_; }
    [[nodiscard]] std::uint32_t sample_rate_hz() const { return sample_rate_hz_; }

    // The server's trims (dB) and delays (ms) for the outputs, in output order; nothing for a key the server's
    // settings do not have. Outputs past the end of a list get 0. Takes effect at the next configure().
    void set_server(std::optional<std::span<const double>> level_db, std::optional<std::span<const double>> delay_ms) {
        server_level_.fill(0.0);
        server_delay_.fill(0);
        has_server_level_ = level_db.has_value();
        has_server_delay_ = delay_ms.has_value();
        if (level_db) {
            std::copy_n(level_db->begin(), std::min(level_db->size(), kMaxOutputs), server_level_.begin());
        }
        if (delay_ms) {
            for (std::size_t o = 0; o < std::min(delay_ms->size(), kMaxOutputs); ++o) {
                server_delay_[o] = static_cast<std::uint32_t>(
                    std::min(TrimDelay::samples_for_ms((*delay_ms)[o], sample_rate_hz_), max_delay_samples_));
            }
        }
    }

    [[nodiscard]] bool server_level() const { return has_server_level_; }
    [[nodiscard]] bool server_delay() const { return has_server_delay_; }

    // The patch: which speaker is at which of `outputs_open` outputs, and everything set up from scratch.
    void configure(const OutputLayout& layout, const Routing& routing, std::size_t outputs_open) {
        roles_ = speaker_trim::roles_of_outputs(layout, routing);
        outputs_ = std::min(outputs_open, capacity_);
        (void)stage_.configure(storage_, outputs_, max_delay_samples_);
        apply(false);
    }

    // The listener's table, in force from here: see the class comment.
    void set_table(const SpeakerTrimTable& table) {
        table_ = table;
        apply(true);
    }
    [[nodiscard]] const SpeakerTrimTable& table() const { return table_; }

    // A new stream.
    void reset() { stage_.reset(); }

    // One block of each output, in place, for the outputs in `live` (bit n for output n) and no others.
    void process(std::span<const std::span<float>> out, std::uint32_t live) { stage_.process(out, live); }

    // The outputs that can carry audio in a block no channel patched to them reached, because a delay line holds
    // some: a caller keeps them live.
    [[nodiscard]] std::uint32_t delayed_outputs() const { return stage_.delayed_outputs(); }

    // Whether any output has a level other than 0 dB or a delay, or a ramp to run.
    [[nodiscard]] bool active() const { return stage_.active(); }

    // The linear gain an output's level comes to.
    [[nodiscard]] float gain(std::size_t output) const { return stage_.gain(output); }

    // What is in force, by output.
    [[nodiscard]] std::size_t outputs() const { return outputs_; }
    [[nodiscard]] speaker_trim::RoleCode role(std::size_t output) const { return output < kMaxOutputs ? roles_[output] : speaker_trim::kNoRole; }
    [[nodiscard]] double level_db(std::size_t output) const { return output < kMaxOutputs ? level_db_[output] : 0.0; }
    [[nodiscard]] std::uint32_t delay_samples(std::size_t output) const { return output < kMaxOutputs ? delay_[output] : 0; }
    // The outputs whose delay is cut to what the build holds, or not given at all for want of delay lines.
    [[nodiscard]] std::uint32_t clamped() const { return clamped_; }
    // The distance the delays are counted from, in centimetres (0: no speaker has one).
    [[nodiscard]] std::uint16_t farthest_cm() const { return farthest_cm_; }

   private:
    // Works out the level and delay of every output and puts them in the stage. `ramp`: a level moves in a
    // line over the next block, and a delay that is the same as it was is left alone (a first setting after
    // configure() has no history to protect, and sets both at once).
    void apply(bool ramp) {
        const speaker_trim::TrimResolved own = speaker_trim::resolve_trim(roles_, table_, sample_rate_hz_, max_delay_samples_);
        // A server's delays are not cut to anything the table asked for.
        clamped_ = has_server_delay_ ? 0 : own.clamped;
        farthest_cm_ = own.farthest_cm;
        for (std::size_t o = 0; o < outputs_; ++o) {
            level_db_[o] = has_server_level_ ? server_level_[o] : own.level_db[o];
            delay_[o] = has_server_delay_ ? server_delay_[o] : own.delay[o];
            if (ramp) {
                (void)stage_.ramp_trim_db(o, level_db_[o]);
            } else {
                (void)stage_.set_trim_db(o, level_db_[o]);
            }
            (void)stage_.set_delay(o, delay_[o]);
        }
        for (std::size_t o = outputs_; o < kMaxOutputs; ++o) {
            level_db_[o] = 0.0;
            delay_[o] = 0;
        }
    }

    TrimDelay stage_;
    std::span<float> storage_;
    std::size_t capacity_ = kMaxOutputs;
    std::size_t max_delay_samples_ = 0;
    std::uint32_t sample_rate_hz_ = 48000;
    SpeakerTrimTable table_;
    std::array<speaker_trim::RoleCode, kMaxOutputs> roles_ = no_roles();
    std::size_t outputs_ = 0;
    bool has_server_level_ = false;
    bool has_server_delay_ = false;
    std::array<double, kMaxOutputs> server_level_{};
    std::array<std::uint32_t, kMaxOutputs> server_delay_{};
    std::array<double, kMaxOutputs> level_db_{};
    std::array<std::uint32_t, kMaxOutputs> delay_{};
    std::uint32_t clamped_ = 0;
    std::uint16_t farthest_cm_ = 0;

    static constexpr std::array<speaker_trim::RoleCode, kMaxOutputs> no_roles() {
        std::array<speaker_trim::RoleCode, kMaxOutputs> out{};
        for (speaker_trim::RoleCode& role : out) {
            role = speaker_trim::kNoRole;
        }
        return out;
    }
};

}  // namespace iclforge::render
