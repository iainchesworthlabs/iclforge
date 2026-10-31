#include "network_group_sink.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "iclforge/dsp/stream_resampler.hpp"
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
 GroupSink(GroupResolver resolve, MemberPlanner plan, KaraokeApplier karaoke, LyricsApplier lyrics)
     : resolve_(std::move(resolve)),
       plan_(std::move(plan)),
       karaoke_(std::move(karaoke)),
       lyrics_(std::move(lyrics)) {}

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
     // The rate the members are sent PCM at, when it is not the stream's: the audio is converted
     // to it here.
     std::uint32_t group_rate = format.sample_rate;
     int coded_substreams = 0;
     if (plan_) {
         const GroupPlan group_plan =
             plan_({.group_name = group_name,
                    .stream = format.stream,
                    .sample_rate = format.sample_rate,
                    .coded_channels = format.coded_channels,
                    .objects = format.objects,
                    .objects_alone = format.objects_alone,
                    .frame_rate = format.frame_rate,
                    .peak_kbps = format.peak_kbps,
                    .presentations = format.presentations,
                    .karaoke = format.karaoke,
                    .layout = format.layout});
         const std::vector<render::OutputLayout>& planned = group_plan.variants;
         if (group_plan.pcm_rate != 0) {
             group_rate = group_plan.pcm_rate;
         }
         coded_substreams = group_plan.coded_substreams;
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
                                 .sample_rate = static_cast<std::int32_t>(group_rate),
                                 .bit_depth = kBitDepth});
         }
     }
     const bool converting = group_rate != format.sample_rate;
     if (converting) {
         // The planner sends PCM at another rate only to a group none of whose members take the
         // stream as it is (sink_form.hpp, plan_group_forms()), so there are no bursts to send,
         // and burst frames counted at the stream's rate would not fit a timeline run at this one.
         bursts.reset();
     }
     m::AudioFormat group_pcm = pcm;
     group_pcm.sample_rate = static_cast<std::int32_t>(group_rate);
     if (!group->start(
             {.pcm = group_pcm, .more_pcm = more_pcm, .bursts = bursts, .buffered = true})) {
         return std::unexpected(fmt::format("The group \"{}\" would not start.", group_name));
     }
     group_ = std::move(group);
     open_group_name_ = group_name;
     variants_ = std::move(variants);
     channels_ = static_cast<std::size_t>(channels);
     sample_rate_ = format.sample_rate;
     group_rate_ = group_rate;
     coded_substreams_ = coded_substreams;
     converters_.clear();
     pending_.clear();
     pending_frames_ = 0;
     if (converting) {
         // One converter for each layout the group renders: the player's first, then the variants.
         converters_.emplace_back(format.sample_rate, group_rate, channels_);
         for (const render::OutputLayout& layout : variants_) {
             converters_.emplace_back(format.sample_rate, group_rate, layout.slots());
         }
         pending_.resize(converters_.size());
     }
     taken_ = 0;
     flushed_ = 0;
     lyrics_item_.reset();
     now_item_.reset();
     return OpenOutputFormat{.sample_rate = format.sample_rate,
                             .channels = static_cast<std::uint16_t>(channels),
                             .mode = OutputMode::kNetworkGroup,
                             .stream = format.stream};
 }

    void close() override {
        if (group_) {
            if (now_item_) {
                // Nothing is playing once the output is closed: the members that showed the item stop
                // showing it, and are told again if the next item opens the group.
                group_->set_metadata(std::nullopt);
            }
            group_->stop();
        }
        group_.reset();
        variants_.clear();
        converters_.clear();
        pending_.clear();
        pending_frames_ = 0;
        channels_ = 0;
        taken_ = 0;
        flushed_ = 0;
        lyrics_item_.reset();
        now_item_.reset();
    }

    [[nodiscard]] bool is_open() const override { return group_ != nullptr; }

    void set_karaoke(const KaraokeChoice& choice) override {
        if (group_ && karaoke_) {
            karaoke_(open_group_name_, choice);
        }
    }

    void set_lyrics(std::int64_t item, const std::string& document) override {
        if (!group_ || !lyrics_) {
            return;
        }
        // A new item: nothing is known of where its sound is until the next sample.
        lyrics_item_ = item;
        anchors_.reset();
        lyrics_(open_group_name_, LyricsUpdate{.item = item, .document = document, .anchor = std::nullopt});
    }

    void follow_lyrics(std::int64_t item, std::int64_t heard_ms) override {
        if (!group_ || !lyrics_ || !lyrics_item_ || *lyrics_item_ != item) {
            return;
        }
        // On the group's own clock, which is the server's: a member's time filter maps it to its own.
        if (const std::optional<LyricsUpdate::Anchor> anchor = anchors_.sample(heard_ms, clock_.now_us())) {
            lyrics_(open_group_name_, LyricsUpdate{.item = item, .document = std::nullopt, .anchor = *anchor});
        }
    }

    void set_now_playing(std::int64_t item, const NowPlaying& now) override {
        if (!group_) {
            return;
        }
        now_item_ = item;
        now_ = now;
        // Nothing is known of where the sound is until the next sample.
        progress_.reset();
        send_now_playing(std::nullopt);
    }

    void follow_now_playing(std::int64_t item, std::int64_t heard_ms) override {
        if (!group_ || !now_item_ || *now_item_ != item) {
            return;
        }
        if (const std::optional<LyricsUpdate::Anchor> anchor = progress_.sample(heard_ms, clock_.now_us())) {
            send_now_playing(anchor);
        }
    }

    [[nodiscard]] std::vector<render::OutputLayout> variants() const override { return variants_; }
    [[nodiscard]] int coded_substreams() const override { return coded_substreams_; }

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
        if (!converters_.empty()) {
            return submit_converted(slots, {}, frames);
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
        if (!converters_.empty()) {
            return submit_converted(slots, variants, frames);
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
        if (!converters_.empty()) {
            return true;  // no member of a converting group takes bursts (open())
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

    // The audio of one block, at the stream's rate, converted to the group's and queued for it. The
    // group takes what its timeline allows, so what it has not yet taken waits here; this takes
    // the block (and returns its frames, which are in the stream's rate) while that is under
    // kPendingSeconds, and none otherwise, so the caller offers the same block again. Every layout
    // is converted alike, so their queues stay the same length.
    std::size_t submit_converted(
        std::span<const std::span<const float>> slots,
        std::span<const std::span<const std::span<const float>>> variants, std::size_t frames) {
        constexpr std::size_t kPendingDivisor = 4;  // a quarter of a second
        std::size_t consumed = 0;
        if (pending_frames_ < group_rate_ / kPendingDivisor) {
            std::size_t produced = 0;
            for (std::size_t index = 0; index < converters_.size(); ++index) {
                const std::span<const std::span<const float>> block =
                    index == 0 ? slots
                    : index - 1 < variants.size() ? variants[index - 1]
                                                  : std::span<const std::span<const float>>{};
                converted_.clear();
                converters_[index].process(block, frames, converted_);
                const std::size_t width = converters_[index].channels();
                const std::size_t out_frames = converted_.empty() ? 0 : converted_[0].size();
                std::vector<std::int32_t>& queue = pending_[index];
                const std::size_t before = queue.size();
                queue.resize(before + out_frames * width);
                for (std::size_t frame = 0; frame < out_frames; ++frame) {
                    for (std::size_t channel = 0; channel < width; ++channel) {
                        queue[before + (frame * width) + channel] = to_sample(converted_[channel][frame]);
                    }
                }
                produced = out_frames;
            }
            pending_frames_ += produced;
            consumed = frames;
            taken_ += frames;
        }
        push_pending();
        return consumed;
    }

    // Gives the group what it will take of the converted audio waiting for it. Also called from
    // position(), which the player asks at every turn: after the last block of an item the player
    // has nothing more to submit, and what is still waiting would otherwise never be sent.
    void push_pending() const {
        if (pending_frames_ == 0 || !group_) {
            return;
        }
        std::vector<std::span<const std::int32_t>> blocks;
        blocks.reserve(pending_.size());
        for (const std::vector<std::int32_t>& queue : pending_) {
            blocks.emplace_back(queue);
        }
        const std::size_t pushed =
            pending_.size() == 1 ? group_->push(blocks[0]) : group_->push_variants(blocks);
        if (pushed == 0) {
            return;
        }
        for (std::size_t index = 0; index < pending_.size(); ++index) {
            const std::size_t width = converters_[index].channels();
            pending_[index].erase(
                pending_[index].begin(),
                pending_[index].begin() + static_cast<std::ptrdiff_t>(pushed * width));
        }
        pending_frames_ -= pushed;
    }

    [[nodiscard]] std::optional<audio::MonitorPosition> position() const override {
        if (!group_) {
            return std::nullopt;
        }
        push_pending();
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
        // What was converted and not yet taken is not played, and the converters start again.
        for (std::vector<std::int32_t>& queue : pending_) {
            queue.clear();
        }
        pending_frames_ = 0;
        for (dsp::StreamResampler& converter : converters_) {
            converter.reset();
        }
    }

    bool pause() override { return true; }
    bool resume() override { return true; }

private:
 // `slots` as interleaved 32-bit samples at `channels`, a slot the block
 // does not have as silence.
 // metadata@v1's state for the item playing: its tags, and where the sound is when an anchor says,
 // which a member moves on at the speed from the time (metadata::position_ms()). The state takes
 // effect at the anchor's time on the server's clock, which is the group's.
 void send_now_playing(const std::optional<LyricsUpdate::Anchor>& anchor) {
     sendspin::metadata::State state;
     state.timestamp = anchor ? anchor->at_us : clock_.now_us();
     if (!now_.title.empty()) {
         state.title = now_.title;
     }
     state.artist = now_.artist;
     state.album = now_.album;
     if (anchor) {
         state.progress = sendspin::metadata::Progress{
             .track_progress_ms = anchor->position_ms,
             .track_duration_ms = now_.duration_ms,
             .playback_speed = static_cast<std::int32_t>(std::lround(anchor->speed * 1000.0))};
     }
     group_->set_metadata(std::move(state));
 }

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
    KaraokeApplier karaoke_;
    LyricsApplier lyrics_;
    // The id the open group was resolved from, for a choice made while it plays.
    std::string open_group_name_;
    // The lyrics the members were last told of: the item, and when its anchors are due.
    std::optional<std::int64_t> lyrics_item_;
    LyricsAnchors anchors_;
    // What the members that show what is playing were last told: the item, its tags, and when its
    // position is due again (the same terms as the lyrics' anchors, and no lyrics applier needed).
    std::optional<std::int64_t> now_item_;
    NowPlaying now_;
    LyricsAnchors progress_;
    std::shared_ptr<sendspin::Group> group_;
    // The layouts beyond the player's the open group carries, and the
    // interleaved block of each, reused across calls.
    std::vector<render::OutputLayout> variants_;
    std::vector<std::vector<std::int32_t>> variant_buffers_;
    std::size_t channels_ = 0;
    std::uint32_t sample_rate_ = 0;
    // The rate the group is sent PCM at: the stream's, or another that the audio is converted to
    // (GroupPlan::pcm_rate), with a converter for each layout, the player's first, and what each
    // has made that the group has not yet taken, interleaved, and its frames.
    std::uint32_t group_rate_ = 0;
    int coded_substreams_ = 0;
    std::vector<dsp::StreamResampler> converters_;
    mutable std::vector<std::vector<std::int32_t>> pending_;
    mutable std::size_t pending_frames_ = 0;
    std::vector<std::vector<float>> converted_;
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

std::unique_ptr<NetworkGroupSink> make_group_sink(GroupResolver resolve, MemberPlanner plan,
                                                  KaraokeApplier karaoke, LyricsApplier lyrics) {
    return std::make_unique<GroupSink>(std::move(resolve), std::move(plan), std::move(karaoke),
                                       std::move(lyrics));
}

}  // namespace iclforge::hearth
