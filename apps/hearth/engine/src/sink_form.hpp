#pragma once

#include <array>
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
//      the stream as it is, to be played as its bed, where it is not. A stream of objects alone
//      (an AC-4 presentation that decodes to no channels) has no bed to be played as: it is
//      sent PCM, folded where the sink's formats need it, and else nothing.
//      One more condition for a karaoke AC-3 or E-AC-3 stream the singer wants reproduced (not "as
//      coded"): the sink's decoder settings list `karaoke`, and `karaoke_vocals` for the capable
//      reproduction, and its `karaoke_codecs` list the stream's data type (a sink that lists
//      none reproduces AC-3's alone). A sink whose decoder is not told how plays the coded
//      channels, and a group would then play two things: that sink is sent PCM decoded here
//      instead, with the reproduction applied, and where it takes no PCM it is sent the stream
//      as it is and the reason says it is not reproduced (planning/karaoke-phase2.md, W7;
//      planning/karaoke-eac3.md, D6).
//   2. Else PCM, if the sink offers `player@v1` with a PCM format that
//      carries the stream's rate and if the policy allows it. The layout is
//      the one the sink is configured to (its own reported `layout`, else the
//      last this app sent, else the player's own, else 2.0) - and where the
//      sink lists no PCM format with that many channels, the widest of 7.1,
//      5.1 and 2.0 it does list that is narrower, so a layout the board
//      cannot take is folded rather than refused.
//   3. Else nothing, and the reason says which of the sink's own limits it was.
//
// What this does not decide: whether the sink can SUSTAIN a PCM stream. An
// ESP32-C6 does on a quiet link and drops out on a busy one, its buffer being
// a fifth of a second (planning/esp32-sink-compatibility-matrix.md), a fact
// about the board that the sink does not state yet. The caller passes it as
// `pcm_fallback` and `pcm_caveat`: the one place that turns the fallback off
// for a board, or for a person who asked for "never decode in Hearth", is
// the first, and what a board is offered with a warning, the second.

namespace iclforge::hearth {

enum class SinkForm : std::uint8_t {
    // The stream's own bursts, over `_iclforge_player@v1`.
    kCoded,
    // Decoded here and rendered to SinkChoice::layout, over `player@v1`.
    kPcm,
    // Neither: the sink is sent nothing for this stream.
    kNone,
};

// The reproduction a karaoke stream is to be heard in (Annex C of ETSI TS 102 366), in the words
// of the role's settings: what a sink that decodes for itself is told, and what decoding for it
// here applies.
struct KaraokeChoice {
    sendspin::player::Karaoke reproduction = sendspin::player::Karaoke::kAware;
    // Which vocals the capable reproduction plays; read by no other.
    sendspin::player::KaraokeVocals vocals = sendspin::player::KaraokeVocals::kBoth;
    // The level of the melody and of each vocal, in dB against Table C.2.3 (DecoderSettings'
    // karaoke_*_db); read by the capable reproduction alone.
    double melody_db = 0.0;
    double vocal1_db = 0.0;
    double vocal2_db = 0.0;

    // The levels that change what is heard under this choice, held to the span and in the order
    // melody, vocal 1, vocal 2: 0 for a vocal the choice leaves out, and for all three under a
    // reproduction that has no levels. These are what a sink is told, and what it must be able
    // to be told (a sink that cannot is sent PCM).
    [[nodiscard]] std::array<double, 3> heard_levels_db() const;

    friend bool operator==(const KaraokeChoice&, const KaraokeChoice&) = default;
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
    // ItemFacts::objects_alone: the programme decodes to its objects and nothing else, so a sink
    // that places fewer than it has plays no bed - it plays nothing.
    bool objects_alone = false;
    // ItemFacts::frame_rate: the frames a second the programme is coded at, which a sink's
    // stated limit (`max_frame_rate`) is compared with; 0 where it is not known, and not compared.
    double frame_rate = 0.0;
    // ItemFacts::peak_kbps: the coded kbit/s the programme is declared to need at most, which a
    // sink's stated `max_data_rate`, and the size of a chunk it is sent, are compared with; 0
    // where it is not known and not compared.
    std::uint32_t peak_kbps = 0;
    // ItemFacts::presentations: the channels of each presentation a TrueHD stream's first substreams
    // make, lowest first. With more than one, a sink that cannot take the stream's best can be sent
    // a lower one (plan_group_forms()).
    std::vector<std::uint16_t> presentations{};
    // Set for a karaoke AC-3 stream whose reproduction is wanted (ItemFacts::karaoke with a
    // reproduction other than "as coded"), which a sink must be told for itself or be sent PCM
    // that has it applied. Unset for every other stream, and for a karaoke one played as coded.
    std::optional<KaraokeChoice> karaoke{};
    // The layout to send a sink that has said none of its own - a standard
    // player has none to say - which is the player's: such a sink then gets the
    // programme as the player renders it where it lists that width, and the
    // fold below it where it does not. Unset takes stereo.
    std::optional<render::OutputLayout> default_layout{};
};

// The codec decoded audio is sent in: PCM, or Opus for a sink on a link too poor for PCM.
enum class PcmCodec : std::uint8_t {
    kPcm,
    kOpus,
};

// Whether a sink lists Opus (48 kHz, stereo) in its player@v1 formats.
[[nodiscard]] bool sink_lists_opus(const SinkFacts& sink);

struct SinkFormPolicy {
    // Whether a sink that cannot take the stream as coded may be sent PCM.
    bool pcm_fallback = true;
    // Whether it is sent Opus instead, lossy and a twelfth of PCM's bytes: set for a sink the
    // person has chosen it for that lists it (SinkFacts::opus_fallback). Where the stream's
    // rate is not 48 kHz, or the sink lists no Opus, it is PCM as before.
    bool opus = false;
    // Whether a sink whose PCM is not at the stream's rate is sent PCM converted to a rate it
    // lists (a 96 kHz item to a board that plays 48 kHz), by a converter in this app. Off, it is
    // sent nothing and the reason says which rates it lists.
    bool convert_rate = true;
    // The rate to send PCM at, when the group has settled on one (plan_group_forms()): a sink that
    // does not list it is sent nothing. 0 takes the stream's rate, or the nearest the sink lists.
    std::uint32_t pcm_rate = 0;
    // A sentence added to the reason of a sink that is sent PCM: what may go wrong with it on
    // this board. Empty for a sink with nothing to add.
    std::string pcm_caveat{};
};

struct SinkChoice {
    SinkForm form = SinkForm::kNone;
    // kPcm: the layout rendered for this sink. Unset otherwise.
    std::optional<render::OutputLayout> layout{};
    // kPcm: the codec it is sent in.
    PcmCodec codec = PcmCodec::kPcm;
    // kPcm: the rate it is sent at when that is not the stream's, so that the audio is converted
    // here; 0 for the stream's own.
    std::uint32_t sample_rate = 0;
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
    // kPcm: the width the sink is asked for, and the codec it is asked to be played in.
    std::int32_t channels = 0;
    PcmCodec codec = PcmCodec::kPcm;
    // kPcm: the rate it is sent at when that is not the stream's (SinkChoice::sample_rate).
    std::uint32_t sample_rate = 0;
    // What the sink is sent, in a few words, for the column of a table: "E-AC-3 as it is",
    // "PCM · 2.0", "Opus · 2.0", "Nothing · held back". The reason is choice.reason.
    std::string label{};
};

struct GroupFormPlan {
    // One per sink given, in that order.
    std::vector<MemberForm> members{};
    // The layouts the members chosen for PCM take beyond the player's own, distinct by
    // width: a player lists a width, and a group sends one render of each.
    std::vector<render::OutputLayout> variants{};
    // The rate every member sent PCM is sent it at when that is not the stream's, and 0 when it is.
    // A group sends one rate: its members that list it get the audio converted here, and one that
    // does not is held back with the reason.
    std::uint32_t pcm_rate = 0;
    // For a TrueHD stream: how many of its first substreams the members sent it as it is get, when
    // that is fewer than it has (0 for all of them). No sink took the stream's best presentation and
    // some took a lower one, so the coded stream is cut to it for them; the others are sent PCM.
    int coded_substreams = 0;
};

// The policy for one sink: whether it may be sent PCM decoded here when it cannot take the
// stream as it is, and what to say about it. An ESP32-C6 is sent PCM with a caveat, its buffer
// being short enough that a busy WiFi drops the stream out (planning/esp32-sink-compatibility-
// matrix.md): the sink does not state that, so it is read from the hardware it names, until the
// board can say it. A sink the person has set to Opus (SinkFacts::opus_fallback) that lists it is
// sent that instead. This is the place a person's own "never decode in Hearth" setting will be
// read as well.
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
