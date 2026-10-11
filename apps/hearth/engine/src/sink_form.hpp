#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/audio/passthrough.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "network_view.hpp"

// In what form one network sink is played one stream: the stream as it is, for
// the sink's own decoder, or PCM decoded and folded here.
//
// Pure, as output_decision.hpp is: it takes what the item is and what the sink
// has said about itself, and names a form, the layout a PCM form is rendered
// to, and a one-line reason. Nothing here opens a connection, which is what
// lets the whole table of boards (an ESP32-C6 that decodes 2.0 only, an S3, a
// P4, a player that takes PCM and nothing else) run on a machine with none.
//
// The rule, and the order the sink's own statements are read in:
//
//   1. Coded, if the sink can decode it. The stream goes to the sink as it is,
//      and the sink renders it to the layout configured there; this is the form
//      Hearth prefers, because one stream then reaches every endpoint and each
//      decodes it for the speakers it has. It needs ALL of: a coded form the
//      source has; the sink offers `_iclforge_player@v1` (SinkKind::kHearthSink
//      or kTestSink, with its support object); the sink is paired, since the
//      role is only taken on the long-term key; its `data_types` list the
//      stream's type; its `sample_rates` list the stream's rate; and the
//      stream has no more channels than the sink states it decodes of that
//      type (`max_coded_channels`; none stated is no limit).
//      Objects are a limit of their own: a sink that states how many it
//      places (`max_objects`, per data type) and would place this stream's -
//      its settings ask for objects, or leave it to its layout, which has
//      height speakers - is sent PCM decoded here where that is possible, and
//      the stream as it is, to be played as its bed, where it is not.
//   2. Else PCM, if the sink offers `player@v1` with a PCM format that
//      carries the stream's rate and if the policy allows it. The layout is
//      the one the sink is configured to (its own reported `layout`, else the
//      last this app sent, else the player's own, else 2.0) - and where the
//      sink lists no PCM format with that many channels, the widest of 7.1,
//      5.1 and 2.0 it does list that is narrower, so a layout the board
//      cannot take is folded rather than refused.
//   3. Else nothing, and the reason says which of the sink's own limits it was.
//
// What this does not decide: whether the sink can SUSTAIN a PCM stream (an
// ESP32-C6 cannot, at the figures measured) is a fact about the board that
// the sink does not state yet. The caller passes it as `pcm_fallback`, so the
// one place that turns the fallback off for a board, or for a person who asked
// for "never decode in Hearth", is that argument.

namespace iclforge::hearth {

enum class SinkForm : std::uint8_t {
    // The stream's own bursts, over `_iclforge_player@v1`.
    kCoded,
    // Decoded here and rendered to SinkChoice::layout, over `player@v1`.
    kPcm,
    // Neither: the sink is sent nothing for this stream.
    kNone,
};

// What the item is, as far as the form is concerned.
struct StreamNeeds {
    // The coded form the source has, and nullopt for one with none: a WAV, or
    // anything decoded before it got here.
    std::optional<audio::BitstreamFormat> stream{};
    // Zero when unknown, in which case the rate is not checked.
    std::uint32_t sample_rate = 0;
    // ItemFacts::channels: the channels the programme codes (an AC-4 stream's
    // are its chosen presentation's). Zero when unknown, which is not compared
    // with the sink's limit.
    std::uint16_t coded_channels = 0;
    // The most dynamic objects the programme places (ItemFacts::objects), which
    // a sink's stated limit is compared with; 0 for a programme with none, or
    // where it is not known.
    std::uint16_t objects = 0;
    // The layout to send a sink that has said none of its own - a standard
    // player has none to say - which is the player's: such a sink then gets the
    // programme as the player renders it where it lists that width, and the
    // fold below it where it does not. Unset takes stereo.
    std::optional<render::OutputLayout> default_layout{};
};

struct SinkFormPolicy {
    // Whether a sink that cannot take the stream as coded may be sent PCM.
    bool pcm_fallback = true;
};

struct SinkChoice {
    SinkForm form = SinkForm::kNone;
    // kPcm: the layout rendered for this sink. Unset otherwise.
    std::optional<render::OutputLayout> layout{};
    // One sentence, for the sink's row: what is sent and, when it is not the
    // stream as it is, which of the sink's own limits it was.
    std::string reason{};
};

[[nodiscard]] SinkChoice choose_sink_form(const StreamNeeds& stream, const SinkFacts& sink,
                                          const SinkFormPolicy& policy = {});

// One group member's part in a plan: what the sink is to be sent, and what to ask of the host
// for it.
enum class MemberAction : std::uint8_t {
    // ServerHost::use_pcm(false): the stream as it is.
    kCoded,
    // ServerHost::use_pcm(true, 0, channels): PCM of `channels` channels.
    kPcm,
    // Group::hold(): sent nothing of this programme.
    kHold,
};

struct MemberForm {
    // What choose_sink_form() said, with the reason made to say so where the plan
    // overruled it (a width another sink already has).
    SinkChoice choice{};
    MemberAction action = MemberAction::kHold;
    // kPcm: the width the sink is asked for.
    std::int32_t channels = 0;
    // What the sink is sent, in a few words, for the column of a table: "E-AC-3 as it is",
    // "PCM · 2.0", "Nothing · held back". The reason is choice.reason.
    std::string label{};
};

struct GroupFormPlan {
    // One per sink given, in that order.
    std::vector<MemberForm> members{};
    // The layouts the members chosen for PCM take beyond the player's own, distinct by
    // width: a player lists a width, and a group sends one render of each.
    std::vector<render::OutputLayout> variants{};
};

// The policy for one sink: whether it may be sent PCM decoded here when it cannot take the
// stream as it is. Off for an ESP32-C6, whose player cannot sustain a PCM stream at the figures
// measured (planning/esp32-sink-compatibility-matrix.md): the sink does not state that, so it is
// read from the hardware it names, until the board can say it. This is the place a person's own
// "never decode in Hearth" setting will be read as well.
[[nodiscard]] SinkFormPolicy form_policy(const SinkFacts& sink);

// Each sink's form for one stream, and the layouts the group needs rendered beyond `master`,
// the player's own: choose_sink_form() for each sink, then the plan's one rule of its own - a
// group sends one render of each width, so a sink that wants a layout whose width another's
// layout (or `master`) already has is held back rather than sent the other's.
[[nodiscard]] GroupFormPlan plan_group_forms(const StreamNeeds& stream,
                                             const render::OutputLayout& master,
                                             std::span<const SinkFacts> sinks);

// The role's data type for a stream's bursts: every AC-4 link is one AC-4
// stream to the role, which drops the link along with the sync words.
[[nodiscard]] sendspin::player::DataType data_type_of(audio::BitstreamFormat format);

}  // namespace iclforge::hearth
