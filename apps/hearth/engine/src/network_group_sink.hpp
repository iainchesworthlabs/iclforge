#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/audio/monitor.hpp"
#include "iclforge/audio/passthrough.hpp"
#include "iclforge/render/layout.hpp"
#include "sink_form.hpp"
#include "transport.hpp"

// Forward-declared, not included: player.hpp includes this header
// unconditionally (PlayerOutputs::group), and player.cpp/player.hpp must
// stay buildable without libs/sendspin at all (ICLFORGE_SENDSPIN_CORE_ONLY,
// tools/fuzz/run.sh) - the same reason output_decision.hpp carries a group by
// plain std::string rather than a sendspin type, and pcm_sink.hpp/
// bitstream_sink.hpp name no backend. GroupResolver only names
// std::shared_ptr<Group>, never constructs or dereferences one, so the
// incomplete type is enough here; network_group_sink.cpp includes the real
// header where a Group is actually used.
namespace iclforge::sendspin {
class Group;
}  // namespace iclforge::sendspin

// Where a network group's programme goes (planning/hearth-reference-player.md,
// A6: "a group of two test sinks and the reference Python player plays one
// programme").
//
// A group needs BOTH forms from the one decode at once: rendered PCM for a
// member playing player@v1 (iclforge::sendspin::Group::push()), and the item's own
// coded units, packed into IEC 61937 bursts, for a member playing
// _iclforge_player@v1 (Group::push_burst()) - a mixed group takes both from
// the same session together (apps/hearth/engine/tests/test_group.cpp's own proof).
// Neither PcmSink nor BitstreamSink fits alone - pcm_sink.hpp's own comment
// says why: "a network group takes a stream... a seam of its own" - so this
// is its own interface rather than a third mode bent into either.
//
// push_burst() takes one whole burst at an absolute programme frame
// (iclforge::sendspin::Group::Burst::frame), not a running byte stream, so a
// caller does not have to keep the PCM and the bursts in lock-step - Player
// paces each independently (player.cpp), and this interface mirrors that:
// submit_pcm() and submit_burst() are unrelated calls, each with its own
// backpressure.

namespace iclforge::hearth {

// What the open group's members that show what is playing (metadata@v1) are told of an item (D7,
// planning/hearth-android.md): the engine knows a title, and an artist and an album only from the
// item's lyrics file, whose tags a sink reads from the document too. A field it does not know is left
// out and not guessed.
struct NowPlaying {
    // The item's name as the engine shows it, or the lyrics file's [ti:] when it has one.
    std::string title{};
    std::optional<std::string> artist{};
    std::optional<std::string> album{};
    // How long the item is, 0 when that is not known (the role's "unknown or unlimited").
    std::int64_t duration_ms = 0;
};

class NetworkGroupSink {
public:
    virtual ~NetworkGroupSink() = default;

    struct Format {
        std::uint32_t sample_rate = 0;
        // What the renderer produces, for a member playing player@v1.
        render::OutputLayout layout{};
        // The item's own coded form, for a member playing
        // _iclforge_player@v1; unset when the item carries nothing IEC 61937
        // can wrap - submit_burst() is then never called, and the group
        // plays to player@v1 members only.
        std::optional<audio::BitstreamFormat> stream{};
        // The channels the item codes (ItemFacts::channels), which a member's
        // stated limit is compared with; 0 when not known.
        std::uint16_t coded_channels = 0;
        // The objects it places (ItemFacts::objects); 0 for none or not known.
        std::uint16_t objects = 0;
        // ItemFacts::objects_alone.
        bool objects_alone = false;
        // ItemFacts::frame_rate, in chunks a second (a TrueHD chunk is up to 24 access units, so
        // 1,200 units a second is 50); 0 when not known.
        double frame_rate = 0.0;
        // ItemFacts::peak_kbps; 0 when not known.
        std::uint32_t peak_kbps = 0;
        // ItemFacts::presentations; empty when not known or not TrueHD.
        std::vector<std::uint16_t> presentations{};
        // The karaoke reproduction the item is to be heard in, when it is a karaoke stream and
        // one is wanted (StreamNeeds::karaoke).
        std::optional<KaraokeChoice> karaoke{};
    };

    // The layouts the open group's members are to be sent PCM at, beyond
    // Format::layout, which a MemberPlanner chose when the group opened: each
    // is the programme rendered to a layout of its own, delivered by
    // submit_pcm_variants() in this order. Empty for a group that plays one
    // layout, or before open().
    [[nodiscard]] virtual std::vector<render::OutputLayout> variants() const { return {}; }
    // The first substreams of a TrueHD stream the group's coded members are sent, when the plan cut
    // it to a lower presentation; 0 for the whole stream.
    [[nodiscard]] virtual int coded_substreams() const { return 0; }

    // Opens `group_name` - resolved against whatever this sink was built
    // with (make_group_sink()'s `resolve`), not a fixed group chosen once at
    // construction, since which group the user has selected can change
    // without the player being rebuilt. The error is a sentence for the
    // Output screen.
    [[nodiscard]] virtual std::expected<OpenOutputFormat, std::string> open(const std::string& group_name,
                                                                            const Format& format) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;

    // The singer changed the karaoke reproduction while the open group plays: the members that
    // decode for themselves are told (NetworkSinks::apply_karaoke()), the ones sent PCM have it
    // applied where the PCM is made. Nothing when the group is not open, or has no applier.
    virtual void set_karaoke(const KaraokeChoice& choice) { static_cast<void>(choice); }

    // The lyrics of the item playing, for the members that hold them (W7d, planning/karaoke-
    // phase2.md): `item` is a number that is new with every document, and `document` an LRC text
    // (iclforge::lyrics::write_lrc), empty when the item has none. Nothing when the group is not
    // open or has no applier.
    virtual void set_lyrics(std::int64_t item, const std::string& document) {
        static_cast<void>(item);
        static_cast<void>(document);
    }
    // Where in item `item` the sound is now, which is the heard position Player's position() has, in
    // milliseconds. Called as often as the engine turns; the sink sends its members an anchor only
    // when what they are holding would be off: a change of speed (a pause, a resume), a jump (a seek)
    // or drift, and one every few seconds to correct the rest.
    virtual void follow_lyrics(std::int64_t item, std::int64_t heard_ms) {
        static_cast<void>(item);
        static_cast<void>(heard_ms);
    }

    // The item playing, for the members that show it (metadata@v1): `item` is the number follow_lyrics()
    // is given for it, new with every item. Its title, artist and album go out at once, and where the
    // sound is follows (follow_now_playing()). Nothing when the group is not open; the members' state
    // is cleared when it closes, so a sink shows nothing playing once the programme has ended.
    virtual void set_now_playing(std::int64_t item, const NowPlaying& now) {
        static_cast<void>(item);
        static_cast<void>(now);
    }
    // Where in item `item` the sound is, as follow_lyrics() has it. The members are sent a position
    // on the terms LyricsAnchors gives: when the speed changed (a pause, a resume), the position jumped
    // or drifted, and one every few seconds; they move it on themselves from the time and the speed.
    virtual void follow_now_playing(std::int64_t item, std::int64_t heard_ms) {
        static_cast<void>(item);
        static_cast<void>(heard_ms);
    }

    // One rendered block, planar like PcmSink::submit() - converted to the
    // group's own interleaved form internally. Returns the frames actually
    // taken, which can be fewer than offered (Group::push()'s own
    // backpressure) or 0 while a member's player holds enough; the caller
    // retries what was not taken.
    [[nodiscard]] virtual std::size_t submit_pcm(std::span<const std::span<const float>> slots,
                                                 std::size_t frames) = 0;
    // The same `frames` of the programme rendered to Format::layout (`slots`)
    // and to each of variants(), in order: `variants[i]` is the planar block
    // of variants()[i]'s slots. All of them or none are taken, so the layouts
    // stay in step. Without variants() it is submit_pcm().
    [[nodiscard]] virtual std::size_t submit_pcm_variants(
        std::span<const std::span<const float>> slots,
        std::span<const std::span<const std::span<const float>>> variants, std::size_t frames) {
        static_cast<void>(variants);
        return submit_pcm(slots, frames);
    }
    // One burst: `pc`/`pd` as iclforge::containers::iec61937 writes them, `payload` the
    // elementary-stream bytes they describe (not the IEC 61937 carrier
    // bytes - a group's members are not S/PDIF, so there is nothing to
    // word-swizzle or zero-pad here), `frame` the programme frame that is
    // its first decoded sample (Group::Burst::frame) and `frames` the
    // samples it decodes to: 1,536 for AC-3 and E-AC-3, and an AC-4 frame's
    // own length (Group::Burst::frames). False, taking nothing, on the same
    // terms as submit_pcm(); the caller retries.
    [[nodiscard]] virtual bool submit_burst(std::uint16_t pc, std::uint16_t pd,
                                            std::span<const std::byte> payload, std::int64_t frame,
                                            std::int64_t frames) = 0;

    // Where the group has got to. A group plays frame n at a fixed time on its
    // own timeline (iclforge::sendspin::Group::start_time() plus n at the sample
    // rate), and every member plays it then, so what has been taken counts
    // as played once that time has passed and as queued until it has - a
    // group reads a buffered source well ahead (a second and a half and its
    // members' lead), and treating "taken" as "heard" would end the
    // programme that far short of its last frame. Nothing is played before
    // the timeline starts (no member playing yet). Shaped like
    // PcmSink::position()'s own MonitorPosition so Player's timeline
    // arithmetic does not need to know the difference.
    [[nodiscard]] virtual std::optional<audio::MonitorPosition> position() const = 0;

    // Counts from zero again, matching submitted_since_open_'s own reset at
    // a seek (player.cpp). It cannot recall bytes already sent over the
    // wire, so a seek during network playback is not click-free the way a
    // local device's flush() is: a member briefly finishes what it was
    // already sent before the new position's audio arrives.
    virtual void flush() = 0;

    // A group has no wire-level pause: with nothing left to send, a
    // member's own player simply runs out and goes quiet, so these are
    // trivial - the transport's own state already stops fill()/drain() from
    // being called (Player::pump()) while paused.
    virtual bool pause() = 0;
    virtual bool resume() = 0;
};

// The real one: resolves `group_name` through `resolve` at each open() -
// which iclforge::sendspin::Group backs a name, and whether that has changed
// since the last open, is NetworkController's own business (the
// HearthController<->NetworkController coupling, still to land - see
// planning/hearth-reference-player.md#a6-network-outputs-in-the-application),
// not this sink's.
using GroupResolver = std::function<std::shared_ptr<sendspin::Group>(const std::string& group_name)>;

// What a group is about to play, as far as its members' forms are concerned.
struct GroupPlanRequest {
    std::string group_name{};
    // The coded form the item has, as NetworkGroupSink::Format::stream.
    std::optional<audio::BitstreamFormat> stream{};
    std::uint32_t sample_rate = 0;
    std::uint16_t coded_channels = 0;
    std::uint16_t objects = 0;
    bool objects_alone = false;
    double frame_rate = 0.0;
    std::uint32_t peak_kbps = 0;
    std::vector<std::uint16_t> presentations{};
    // As NetworkGroupSink::Format::karaoke.
    std::optional<KaraokeChoice> karaoke{};
    // The layout the player renders to, which a member that wants it is sent.
    render::OutputLayout layout{};
};

// What a MemberPlanner answers: the layouts, beyond the request's, that the members chosen for PCM
// take; and the rate they take it at when that is not the stream's, which the group's sink then
// converts to (dsp::StreamResampler). A list of layouts alone is a plan with no conversion.
struct GroupPlan {
    std::vector<render::OutputLayout> variants{};
    std::uint32_t pcm_rate = 0;
    // GroupFormPlan::coded_substreams: the first substreams of a TrueHD stream the members sent it
    // coded get, when not all of them.
    int coded_substreams = 0;

    GroupPlan() = default;
    // NOLINTNEXTLINE(google-explicit-constructor): a planner that has no rate to change answers a list.
    GroupPlan(std::vector<render::OutputLayout> layouts, std::uint32_t rate = 0)
        : variants(std::move(layouts)), pcm_rate(rate) {}
};

// Decides, for each member of a group, the form it is sent the programme in
// (choose_sink_form()), tells the host (ServerHost::use_pcm()), and answers
// the layouts, beyond the request's, that the members chosen for PCM are to
// get. Called when a group opens, on the engine's thread. With none, the
// group plays PCM at the player's layout to whichever members take it.
using MemberPlanner = std::function<GroupPlan(const GroupPlanRequest& request)>;

// Tells the members of group `group_id` that decode for themselves how to reproduce a karaoke
// stream, for a choice made while it plays (NetworkGroupSink::set_karaoke()).
using KaraokeApplier = std::function<void(const std::string& group_id, const KaraokeChoice& choice)>;

// What a group's members that hold lyrics are told of the item playing (NetworkGroupSink::
// set_lyrics() and follow_lyrics()).
struct LyricsUpdate {
    struct Anchor {
        std::int64_t position_ms = 0;
        // On the group's own clock, which is the server's: a member's time filter maps it to its own.
        std::int64_t at_us = 0;
        // 1 while the item plays, 0 held.
        double speed = 1.0;
    };
    // The item's number, new with every document, so that an anchor is known for the item it is for.
    std::int64_t item = 0;
    // The item's lyrics as an LRC text, empty for an item with none. Set when the item starts; an
    // update with only an anchor leaves the document where it is.
    std::optional<std::string> document;
    std::optional<Anchor> anchor;
};

// When a group's members are sent a new anchor (W7d): from the heard position of the item at each
// turn of the engine, and the time on the group's clock. A member holds the last anchor and moves it
// on at its speed, so a new one is only needed when that would be wrong: the sound's speed changed (a
// pause, and a resume, and the stop of a member's buffer running out after one), the position jumped
// (a seek) or drifted, and one every few seconds so that the rest is corrected.
//
// The speed is measured, not taken from the transport: a pause takes a buffer's worth of time to be
// heard, and the position stops by itself when it is. It is judged over a stretch of at least
// kSampleUs, so that two turns a few milliseconds apart do not read as a stop.
class LyricsAnchors {
public:
    static constexpr std::int64_t kOffMs = 150;
    static constexpr std::int64_t kRefreshUs = 5'000'000;
    static constexpr std::int64_t kSampleUs = 40'000;

    // A new item, or one heard from the start again: nothing is known of where the sound is.
    void reset() {
        anchor_.reset();
        sample_.reset();
    }

    // The sound is at `heard_ms` at `now_us`. The anchor to send, or nothing when what the members
    // hold is right.
    [[nodiscard]] std::optional<LyricsUpdate::Anchor> sample(std::int64_t heard_ms, std::int64_t now_us) {
        double speed = anchor_ ? anchor_->speed : 1.0;
        if (!sample_) {
            sample_ = Sample{.position_ms = heard_ms, .at_us = now_us};
        } else if (now_us - sample_->at_us >= kSampleUs) {
            const double moved = static_cast<double>(heard_ms - sample_->position_ms);
            const double elapsed = static_cast<double>(now_us - sample_->at_us) / 1000.0;
            if (moved >= 0.0 && moved <= 0.5 * elapsed) {
                speed = 0.0;
            } else if (moved - elapsed <= static_cast<double>(kOffMs) &&
                       elapsed - moved <= static_cast<double>(kOffMs)) {
                speed = 1.0;
            }
            // Otherwise the position jumped (a seek, either way): the speed is what it was, and the
            // jump itself is what the check below sends an anchor for.
            sample_ = Sample{.position_ms = heard_ms, .at_us = now_us};
        }
        bool send = !anchor_.has_value();
        if (anchor_) {
            const double predicted = static_cast<double>(anchor_->position_ms) +
                                     static_cast<double>(now_us - anchor_->at_us) / 1000.0 * anchor_->speed;
            const double off = static_cast<double>(heard_ms) - predicted;
            send = speed != anchor_->speed || off > static_cast<double>(kOffMs) ||
                   off < -static_cast<double>(kOffMs) || now_us - anchor_->at_us >= kRefreshUs;
        }
        if (!send) {
            return std::nullopt;
        }
        anchor_ = LyricsUpdate::Anchor{.position_ms = heard_ms, .at_us = now_us, .speed = speed};
        return anchor_;
    }

private:
    struct Sample {
        std::int64_t position_ms = 0;
        std::int64_t at_us = 0;
    };
    std::optional<LyricsUpdate::Anchor> anchor_;
    std::optional<Sample> sample_;
};

// Tells the members of group `group_id` that hold lyrics (NetworkSinks::apply_lyrics()).
using LyricsApplier = std::function<void(const std::string& group_id, const LyricsUpdate& update)>;

[[nodiscard]] std::unique_ptr<NetworkGroupSink> make_group_sink(GroupResolver resolve,
                                                                MemberPlanner plan = {},
                                                                KaraokeApplier karaoke = {},
                                                                LyricsApplier lyrics = {});

}  // namespace iclforge::hearth
