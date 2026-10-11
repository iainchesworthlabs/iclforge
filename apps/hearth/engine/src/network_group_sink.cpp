#include "network_group_sink.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/server_host.hpp"
#include "iclforge/sendspin/session_driver.hpp"
#include "sink_form.hpp"

// The NetworkGroupSink over a real iclforge::sendspin::Group: everything here is a
// translation between the two interfaces, as passthrough_sink.cpp and
// device_sink.cpp are for their own sinks. What differs is that a group is
// resolved by name at open() rather than held fixed from construction (see
// network_group_sink.hpp's own comment on GroupResolver), and that pushing
// PCM is a partial-take call (Group::push() can take fewer frames than
// offered) rather than the all-or-nothing bool PcmSink::submit() is -
// Player's own drain_group() (player.cpp) is what retries the remainder.

namespace iclforge::hearth {

namespace {

namespace m = sendspin::messages;

// Full-scale: the least lossy choice for the group's own declared bit depth
// - Group::push() rescales down for a member at a lower depth itself
// (server_host.cpp's rescaled()), so this sink never needs to know what any
// member actually is.
constexpr std::int32_t kBitDepth = 32;

[[nodiscard]] std::int32_t to_sample(float value) {
    const double scaled = std::clamp(static_cast<double>(value), -1.0, 1.0) * 2147483647.0;
    return static_cast<std::int32_t>(std::lround(scaled));
}

class GroupSink final : public NetworkGroupSink {
public:
 GroupSink(GroupResolver resolve, MemberPlanner plan)
     : resolve_(std::move(resolve)), plan_(std::move(plan)) {}

 ~GroupSink() override { close(); }

 GroupSink(const GroupSink&) = delete;
 GroupSink& operator=(const GroupSink&) = delete;

 std::expected<OpenOutputFormat, std::string> open(const std::string& group_name,
                                                   const Format& format) override {
     close();
     std::shared_ptr<sendspin::Group> group = resolve_ ? resolve_(group_name) : nullptr;
     if (!group) {
         return std::unexpected(fmt::format("The group \"{}\" is not available.", group_name));
     }
     const auto channels = static_cast<std::int32_t>(format.layout.slots());
     const m::AudioFormat pcm{.codec = m::Codec::kPcm,
                              .channels = channels,
                              .sample_rate = static_cast<std::int32_t>(format.sample_rate),
                              .bit_depth = kBitDepth};
     std::optional<sendspin::player::StreamStart> bursts;
     if (format.stream) {
         bursts = sendspin::player::StreamStart{
             .data_type = data_type_of(*format.stream),
             .sample_rate = static_cast<std::int32_t>(format.sample_rate)};
     }
     // The members' forms are decided now, with the item's facts, and what
     // they are to be sent PCM at, beyond the player's layout, comes back.
     std::vector<render::OutputLayout> variants;
     std::vector<m::AudioFormat> more_pcm;
     if (plan_) {
         const std::vector<render::OutputLayout> planned =
             plan_({.group_name = group_name,
                    .stream = format.stream,
                    .sample_rate = format.sample_rate,
                    .coded_channels = format.coded_channels,
                    .layout = format.layout});
         for (const render::OutputLayout& layout : planned) {
             // One variant per width: a player lists a width, not a layout,
             // and the group refuses two of one.
             const auto width = static_cast<std::int32_t>(layout.slots());
             const bool taken = width == channels || std::any_of(more_pcm.begin(), more_pcm.end(),
                                                                 [&](const m::AudioFormat& other) {
                                                                     return other.channels == width;
                                                                 });
             if (taken || width < 1) {
                 continue;
             }
             variants.push_back(layout);
             more_pcm.push_back({.codec = m::Codec::kPcm,
                                 .channels = width,
                                 .sample_rate = static_cast<std::int32_t>(format.sample_rate),
                                 .bit_depth = kBitDepth});
         }
     }
     if (!group->start({.pcm = pcm, .more_pcm = more_pcm, .bursts = bursts, .buffered = true})) {
         return std::unexpected(fmt::format("The group \"{}\" would not start.", group_name));
     }
     group_ = std::move(group);
     variants_ = std::move(variants);
     channels_ = static_cast<std::size_t>(channels);
     sample_rate_ = format.sample_rate;
     taken_ = 0;
     flushed_ = 0;
     return OpenOutputFormat{.sample_rate = format.sample_rate,
                             .channels = static_cast<std::uint16_t>(channels),
                             .mode = OutputMode::kNetworkGroup,
                             .stream = format.stream};
 }

    void close() override {
        if (group_) {
            group_->stop();
        }
        group_.reset();
        variants_.clear();
        channels_ = 0;
        taken_ = 0;
        flushed_ = 0;
    }

    [[nodiscard]] bool is_open() const override { return group_ != nullptr; }

    [[nodiscard]] std::vector<render::OutputLayout> variants() const override { return variants_; }

    std::size_t submit_pcm(std::span<const std::span<const float>> slots, std::size_t frames) override {
        if (!group_) {
            return 0;
        }
        if (!variants_.empty()) {
            // A group that carries variants takes a block of each; a caller
            // that has none for them sends them silence, which keeps the
            // layouts in step rather than leaving the group to take nothing.
            return submit_pcm_variants(slots, {}, frames);
        }
        interleave(slots, frames, channels_, interleaved_);
        const std::size_t taken = group_->push(interleaved_);
        taken_ += taken;
        return taken;
    }

    std::size_t submit_pcm_variants(
        std::span<const std::span<const float>> slots,
        std::span<const std::span<const std::span<const float>>> variants,
        std::size_t frames) override {
        if (!group_) {
            return 0;
        }
        if (variants_.empty()) {
            return submit_pcm(slots, frames);
        }
        interleave(slots, frames, channels_, interleaved_);
        variant_buffers_.resize(variants_.size());
        for (std::size_t index = 0; index < variants_.size(); ++index) {
            const std::span<const std::span<const float>> block =
                index < variants.size() ? variants[index]
                                        : std::span<const std::span<const float>>{};
            interleave(block, frames, variants_[index].slots(), variant_buffers_[index]);
        }
        std::vector<std::span<const std::int32_t>> blocks;
        blocks.reserve(1 + variants_.size());
        blocks.emplace_back(interleaved_);
        for (const std::vector<std::int32_t>& buffer : variant_buffers_) {
            blocks.emplace_back(buffer);
        }
        const std::size_t taken = group_->push_variants(blocks);
        taken_ += taken;
        return taken;
    }

    bool submit_burst(std::uint16_t pc, std::uint16_t pd, std::span<const std::byte> payload,
                      std::int64_t frame, std::int64_t frames) override {
        if (!group_) {
            return false;
        }
        // Group::Burst::payload is std::span<const std::uint8_t> - sendspin's
        // own byte type, not forge's std::byte this interface otherwise
        // matches (network_group_sink.hpp's own submit_burst() comment).
        // Both alias unsigned char, so reinterpreting the span is well-defined.
        const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                                   payload.size());
        return group_->push_burst(
            {.pc = pc, .pd = pd, .payload = bytes, .frame = frame, .frames = frames});
    }

    [[nodiscard]] std::optional<audio::MonitorPosition> position() const override {
        if (!group_) {
            return std::nullopt;
        }
        // Played is what the group's timeline has run through - the frame
        // every member is playing now - and never more than has been taken.
        std::uint64_t played = 0;
        if (const std::optional<std::int64_t> start = group_->start_time()) {
            const std::int64_t elapsed_us = clock_.now_us() - *start;
            if (elapsed_us > 0) {
                const auto elapsed =
                    static_cast<std::uint64_t>((static_cast<double>(elapsed_us) * static_cast<double>(sample_rate_)) / 1e6);
                // The timeline runs on through a flush; what came before it is
                // not this count's.
                played = elapsed > flushed_ ? std::min(elapsed - flushed_, taken_) : 0;
            }
        }
        return audio::MonitorPosition{.frames_played = played, .frames_queued = taken_ - played, .latency_frames = 0};
    }

    void flush() override {
        // The group's own frame count goes on from where it was: the next
        // frame taken is frame flushed_ of its timeline.
        flushed_ += taken_;
        taken_ = 0;
    }

    bool pause() override { return true; }
    bool resume() override { return true; }

private:
 // `slots` as interleaved 32-bit samples at `channels`, a slot the block
 // does not have as silence.
 static void interleave(std::span<const std::span<const float>> slots, std::size_t frames,
                        std::size_t channels, std::vector<std::int32_t>& out) {
     out.resize(frames * channels);
     for (std::size_t frame = 0; frame < frames; ++frame) {
         for (std::size_t channel = 0; channel < channels; ++channel) {
             const float sample = channel < slots.size() ? slots[channel][frame] : 0.0F;
             out[(frame * channels) + channel] = to_sample(sample);
         }
     }
 }

    GroupResolver resolve_;
    MemberPlanner plan_;
    std::shared_ptr<sendspin::Group> group_;
    // The layouts beyond the player's the open group carries, and the
    // interleaved block of each, reused across calls.
    std::vector<render::OutputLayout> variants_;
    std::vector<std::vector<std::int32_t>> variant_buffers_;
    std::size_t channels_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::uint64_t taken_ = 0;
    // Frames taken before the last flush() since open().
    std::uint64_t flushed_ = 0;
    // The clock a group's timeline is on (ServerHost's own).
    sendspin::SteadyClock clock_;
    // Reused across submit_pcm() calls so steady playback allocates nothing
    // once it has grown to the largest block it has seen, the same reason
    // Player::Pending's own buffers are reused (player.hpp).
    std::vector<std::int32_t> interleaved_;
};

}  // namespace

std::unique_ptr<NetworkGroupSink> make_group_sink(GroupResolver resolve, MemberPlanner plan) {
    return std::make_unique<GroupSink>(std::move(resolve), std::move(plan));
}

}  // namespace iclforge::hearth
