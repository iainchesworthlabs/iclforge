#pragma once

#include <cstdint>
#include <optional>
#include <string>

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
//   2. Else PCM, if the sink offers `player@v1` with a PCM format that
//      carries the stream's rate and if the policy allows it. The layout is
//      the one the sink is configured to (its own reported `layout`, else the
//      last this app sent, else 2.0) - and where the sink lists no PCM format
//      with that many channels, the widest of 7.1, 5.1 and 2.0 it does list
//      that is narrower, so a layout the board cannot take is folded rather
//      than refused.
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

// The role's data type for a stream's bursts: every AC-4 link is one AC-4
// stream to the role, which drops the link along with the sync words.
[[nodiscard]] sendspin::player::DataType data_type_of(audio::BitstreamFormat format);

}  // namespace iclforge::hearth
