#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/audio/passthrough.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "network_view.hpp"
#include "sink_form.hpp"

// iclforge::hearth::choose_sink_form's case table (apps/hearth/engine/src/sink_form.cpp).
//
// Every row is a board this desk may not have plugged in - an ESP32-C6 that
// decodes 2.0 only, an S3, a player that takes PCM and nothing else, a sink
// that has not been paired - put in front of the same function the engine
// calls. Nothing here opens a connection.
//
// The reason string is checked for the substance a person needs (which limit
// of the sink it was, and what is sent instead), not word for word.

namespace {

namespace sp = iclforge::sendspin::player;
using iclforge::audio::BitstreamFormat;
using iclforge::hearth::choose_sink_form;
using iclforge::hearth::data_type_of;
using iclforge::hearth::MemberAction;
using iclforge::hearth::PairState;
using iclforge::hearth::PcmCodec;
using iclforge::hearth::PcmFormat;
using iclforge::hearth::plan_group_forms;
using iclforge::hearth::SinkFacts;
using iclforge::hearth::SinkForm;
using iclforge::hearth::SinkFormPolicy;
using iclforge::hearth::SinkKind;
using iclforge::hearth::StreamNeeds;

bool mentions(const std::string& reason, std::string_view needle) {
    return reason.find(needle) != std::string::npos;
}

constexpr std::uint32_t k48k = 48000;

// A paired Hearth sink that lists `types` at 48 kHz, and PCM of `pcm` channel counts at 48 kHz on
// player@v1 the way a board lists it.
SinkFacts hearth_sink(std::vector<sp::DataType> types, std::vector<std::uint16_t> pcm = {2}) {
    SinkFacts sink;
    sink.id = "board";
    sink.name = "Board";
    sink.kind = SinkKind::kHearthSink;
    sink.pair_state = PairState::kPaired;
    sp::Support support;
    support.data_types = std::move(types);
    support.sample_rates = {48000};
    sink.iclforge_support = support;
    for (const std::uint16_t channels : pcm) {
        sink.pcm_formats.push_back({.channels = channels, .sample_rate = k48k, .bit_depth = 16});
    }
    return sink;
}

void limit(SinkFacts& sink, sp::DataType type, std::uint8_t channels) {
    sink.iclforge_support->max_coded_channels[static_cast<std::size_t>(type)] = channels;
}

void configured(SinkFacts& sink, std::string layout) {
    sp::State state;
    state.layout = std::move(layout);
    sink.iclforge_state = state;
}

StreamNeeds ac3(std::uint16_t channels) {
    return {.stream = BitstreamFormat::kAc3, .sample_rate = k48k, .coded_channels = channels};
}

}  // namespace

TEST_CASE("sink form: a sink that decodes the stream is sent it as it is", "[hearth][sink_form]") {
    const SinkFacts sink =
        hearth_sink({sp::DataType::kAc3, sp::DataType::kEac3, sp::DataType::kAc4});
    for (const StreamNeeds& stream :
         {ac3(6),
          StreamNeeds{.stream = BitstreamFormat::kEac3, .sample_rate = k48k, .coded_channels = 6},
          StreamNeeds{
              .stream = BitstreamFormat::kAc4, .sample_rate = k48k, .coded_channels = 12}}) {
        const auto choice = choose_sink_form(stream, sink);
        CHECK(choice.form == SinkForm::kCoded);
        CHECK_FALSE(choice.layout.has_value());
        CHECK_FALSE(choice.reason.empty());
    }
}

TEST_CASE("sink form: every AC-4 link is the one AC-4 data type", "[hearth][sink_form]") {
    CHECK(data_type_of(BitstreamFormat::kAc3) == sp::DataType::kAc3);
    CHECK(data_type_of(BitstreamFormat::kEac3) == sp::DataType::kEac3);
    CHECK(data_type_of(BitstreamFormat::kAc4) == sp::DataType::kAc4);
    CHECK(data_type_of(BitstreamFormat::kAc4Hbr4) == sp::DataType::kAc4);
    CHECK(data_type_of(BitstreamFormat::kAc4Hbr16) == sp::DataType::kAc4);

    // So a sink that lists AC-4 takes the high-bit-rate links too.
    const SinkFacts sink = hearth_sink({sp::DataType::kAc4});
    const StreamNeeds hbr{
        .stream = BitstreamFormat::kAc4Hbr16, .sample_rate = k48k, .coded_channels = 6};
    CHECK(choose_sink_form(hbr, sink).form == SinkForm::kCoded);
}

TEST_CASE("sink form: a channel limit the sink states sends a wider stream as PCM",
          "[hearth][sink_form]") {
    // The C6's build: it decodes 2.0 of AC-3 and E-AC-3, and refuses a 5.1 syncframe.
    SinkFacts c6 = hearth_sink({sp::DataType::kAc3, sp::DataType::kEac3});
    limit(c6, sp::DataType::kAc3, 2);
    limit(c6, sp::DataType::kEac3, 2);
    configured(c6, "2.0");

    SECTION("a stream within the limit is still sent as it is") {
        CHECK(choose_sink_form(ac3(2), c6).form == SinkForm::kCoded);
    }
    SECTION("a stream past it is decoded here and sent as PCM at the sink's layout") {
        const auto choice = choose_sink_form(ac3(6), c6);
        REQUIRE(choice.form == SinkForm::kPcm);
        REQUIRE(choice.layout.has_value());
        CHECK(choice.layout->slots() == 2);
        CHECK(mentions(choice.reason, "up to 2 channels"));
        CHECK(mentions(choice.reason, "has 6"));
        CHECK(mentions(choice.reason, "PCM"));
    }
    SECTION("with the fallback off it is sent nothing, and the reason says both why") {
        const auto choice = choose_sink_form(ac3(6), c6, {.pcm_fallback = false});
        CHECK(choice.form == SinkForm::kNone);
        CHECK_FALSE(choice.layout.has_value());
        CHECK(mentions(choice.reason, "up to 2 channels"));
        CHECK(mentions(choice.reason, "switched off"));
    }
    SECTION("a limit stated for AC-3 is not one for AC-4") {
        SinkFacts both = hearth_sink({sp::DataType::kAc3, sp::DataType::kAc4});
        limit(both, sp::DataType::kAc3, 2);
        const StreamNeeds ac4{
            .stream = BitstreamFormat::kAc4, .sample_rate = k48k, .coded_channels = 6};
        CHECK(choose_sink_form(ac4, both).form == SinkForm::kCoded);
        CHECK(choose_sink_form(ac3(6), both).form == SinkForm::kPcm);
    }
}

TEST_CASE("sink form: a sink that states no limit is not taken to have none of a stream",
          "[hearth][sink_form]") {
    // 0 is "not stated", and a stream is sent: the sink refuses it itself if it must.
    const SinkFacts sink = hearth_sink({sp::DataType::kAc3});
    CHECK(sink.iclforge_support->max_coded_channels_of(sp::DataType::kAc3) == 0);
    CHECK(choose_sink_form(ac3(6), sink).form == SinkForm::kCoded);
    // And a stream whose channel count is not known is not compared with a limit that is.
    SinkFacts limited = hearth_sink({sp::DataType::kAc3});
    limit(limited, sp::DataType::kAc3, 2);
    CHECK(choose_sink_form(ac3(0), limited).form == SinkForm::kCoded);
}

TEST_CASE("sink form: a type the sink does not list is sent as PCM", "[hearth][sink_form]") {
    // An S3 or C6 that lists AC-3 and E-AC-3 only, given an AC-4 stream.
    const SinkFacts sink = hearth_sink({sp::DataType::kAc3, sp::DataType::kEac3}, {2, 6});
    const StreamNeeds ac4{
        .stream = BitstreamFormat::kAc4, .sample_rate = k48k, .coded_channels = 6};
    const auto choice = choose_sink_form(ac4, sink);
    REQUIRE(choice.form == SinkForm::kPcm);
    CHECK(mentions(choice.reason, "does not decode AC-4"));
    CHECK(mentions(choice.reason, "decoded here"));
}

TEST_CASE("sink form: a stream at a rate the sink does not play coded is sent as PCM",
          "[hearth][sink_form]") {
    SinkFacts sink = hearth_sink({sp::DataType::kAc3});
    sink.pcm_formats.push_back({.channels = 2, .sample_rate = 44100, .bit_depth = 16});
    const StreamNeeds cd{
        .stream = BitstreamFormat::kAc3, .sample_rate = 44100, .coded_channels = 2};
    const auto choice = choose_sink_form(cd, sink);
    REQUIRE(choice.form == SinkForm::kPcm);
    CHECK(mentions(choice.reason, "48000 Hz only"));
    CHECK(mentions(choice.reason, "44100"));
}

TEST_CASE("sink form: a sink that is not paired takes no coded stream", "[hearth][sink_form]") {
    SinkFacts sink = hearth_sink({sp::DataType::kAc3});
    sink.pair_state = PairState::kNotPaired;
    const auto choice = choose_sink_form(ac3(2), sink);
    CHECK(choice.form == SinkForm::kPcm);
    CHECK(mentions(choice.reason, "not paired"));
}

TEST_CASE("sink form: a standard player is sent PCM at stereo", "[hearth][sink_form]") {
    SinkFacts player;
    player.kind = SinkKind::kStandardPlayer;
    player.pcm_formats.push_back({.channels = 2, .sample_rate = k48k, .bit_depth = 16});
    const auto choice = choose_sink_form(ac3(6), player);
    REQUIRE(choice.form == SinkForm::kPcm);
    REQUIRE(choice.layout.has_value());
    CHECK(choice.layout->slots() == 2);
    CHECK(mentions(choice.reason, "takes PCM"));
}

TEST_CASE("sink form: a source with no coded form is PCM, and is not said to be decoded",
          "[hearth][sink_form]") {
    const SinkFacts sink = hearth_sink({sp::DataType::kAc3});
    const auto choice = choose_sink_form({.sample_rate = k48k, .coded_channels = 2}, sink);
    REQUIRE(choice.form == SinkForm::kPcm);
    CHECK(mentions(choice.reason, "no coded form"));
    CHECK_FALSE(mentions(choice.reason, "decoded here"));
}

TEST_CASE("sink form: the layout is the sink's own, then this app's last, then 2.0",
          "[hearth][sink_form]") {
    // A sink that cannot take AC-4, so every case below is a PCM one.
    const StreamNeeds ac4{
        .stream = BitstreamFormat::kAc4, .sample_rate = k48k, .coded_channels = 6};
    const auto slots = [&](const SinkFacts& sink) {
        const auto choice = choose_sink_form(ac4, sink);
        REQUIRE(choice.form == SinkForm::kPcm);
        REQUIRE(choice.layout.has_value());
        return choice.layout->slots();
    };

    SinkFacts sink = hearth_sink({sp::DataType::kAc3}, {2, 6, 8});
    CHECK(slots(sink) == 2);  // nothing said

    sp::Settings intended;
    intended.layout = "7.1";
    sink.intended_settings = intended;
    CHECK(slots(sink) == 8);  // what this app last sent

    configured(sink, "5.1");
    CHECK(slots(sink) == 6);  // what the sink says is in force wins

    configured(sink, "not a layout");
    CHECK(slots(sink) == 8);  // a layout that does not parse says nothing
}

TEST_CASE(
    "sink form: a sink that has said no layout is sent the player's own where it lists that width",
    "[hearth][sink_form]") {
    const auto five_one = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(five_one.has_value());
    const auto slots_for = [&](std::vector<std::uint16_t> widths) {
        SinkFacts player;
        player.kind = SinkKind::kStandardPlayer;
        for (const std::uint16_t channels : widths) {
            player.pcm_formats.push_back(
                {.channels = channels, .sample_rate = k48k, .bit_depth = 16});
        }
        StreamNeeds needs = ac3(6);
        needs.default_layout = five_one;
        const auto choice = choose_sink_form(needs, player);
        REQUIRE(choice.form == SinkForm::kPcm);
        REQUIRE(choice.layout.has_value());
        return choice.layout->slots();
    };
    CHECK(slots_for({2, 6}) == 6);
    // It lists stereo only: the fold below the player's layout, not the layout itself.
    CHECK(slots_for({2}) == 2);
}

TEST_CASE("sink form: a group's plan sends each member its own form and one render of each width",
          "[hearth][sink_form][plan]") {
    const auto master = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(master.has_value());

    SinkFacts decodes = hearth_sink({sp::DataType::kAc3, sp::DataType::kEac3});
    decodes.name = "Decodes";
    // A C6 build: it decodes 2.0 of AC-3 and is sent PCM with a caveat, read from its hardware.
    SinkFacts c6 = hearth_sink({sp::DataType::kAc3, sp::DataType::kEac3});
    c6.name = "C6";
    c6.hardware = "ESP32-C6";
    limit(c6, sp::DataType::kAc3, 2);
    SinkFacts stereo;
    stereo.name = "Stereo";
    stereo.kind = SinkKind::kStandardPlayer;
    stereo.pcm_formats.push_back({.channels = 2, .sample_rate = k48k, .bit_depth = 16});
    SinkFacts surround;
    surround.name = "Surround";
    surround.kind = SinkKind::kStandardPlayer;
    surround.pcm_formats.push_back({.channels = 6, .sample_rate = k48k, .bit_depth = 16});
    surround.pcm_formats.push_back({.channels = 2, .sample_rate = k48k, .bit_depth = 16});

    const std::vector<SinkFacts> sinks{decodes, c6, stereo, surround};
    const auto plan = plan_group_forms(ac3(6), *master, sinks);
    REQUIRE(plan.members.size() == 4);

    CHECK(plan.members[0].action == MemberAction::kCoded);
    CHECK(plan.members[0].label == "AC-3 as it is");
    CHECK(plan.members[1].label == "PCM · 2.0");
    CHECK(plan.members[2].label == "PCM · 2.0");
    CHECK(plan.members[3].label == "PCM · 5.1");
    // Past what it decodes: sent PCM at 2.0, the stereo player's render, with the caveat.
    CHECK(plan.members[1].action == MemberAction::kPcm);
    CHECK(plan.members[1].channels == 2);
    CHECK(mentions(plan.members[1].choice.reason, "up to 2 channels"));
    CHECK(mentions(plan.members[1].choice.reason, "drop out"));
    // A stereo player: the 2.0 fold, a render of its own.
    CHECK(plan.members[2].action == MemberAction::kPcm);
    CHECK(plan.members[2].channels == 2);
    // A player that lists 5.1 first takes the player's own layout, which is not a variant.
    CHECK(plan.members[3].action == MemberAction::kPcm);
    CHECK(plan.members[3].channels == 6);
    REQUIRE(plan.variants.size() == 1);
    CHECK(plan.variants.front().slots() == 2);
}

TEST_CASE("sink form: a layout of a width another has is held back and not sent the other's",
          "[hearth][sink_form][plan]") {
    const auto master = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(master.has_value());
    // Two boards that cannot decode AC-4 and are set to different layouts of eight speakers.
    SinkFacts first = hearth_sink({sp::DataType::kAc3}, {8});
    first.name = "First";
    configured(first, "7.1");
    SinkFacts second = hearth_sink({sp::DataType::kAc3}, {8});
    second.name = "Second";
    configured(second, "5.1.2");
    const StreamNeeds ac4{
        .stream = BitstreamFormat::kAc4, .sample_rate = k48k, .coded_channels = 8};

    const std::vector<SinkFacts> sinks{first, second};
    const auto plan = plan_group_forms(ac4, *master, sinks);
    REQUIRE(plan.members.size() == 2);
    CHECK(plan.members[0].action == MemberAction::kPcm);
    CHECK(plan.members[0].channels == 8);
    CHECK(plan.members[1].action == MemberAction::kHold);
    CHECK(plan.members[1].choice.form == SinkForm::kNone);
    CHECK_FALSE(plan.members[1].choice.layout.has_value());
    CHECK(mentions(plan.members[1].choice.reason, "7.1"));
    REQUIRE(plan.variants.size() == 1);
    CHECK(plan.variants.front().text() == "7.1");

    // The same layout twice is one render, sent to both.
    configured(second, "7.1");
    const std::vector<SinkFacts> alike{first, second};
    const auto shared = plan_group_forms(ac4, *master, alike);
    CHECK(shared.members[0].action == MemberAction::kPcm);
    CHECK(shared.members[1].action == MemberAction::kPcm);
    CHECK(shared.variants.size() == 1);
}

TEST_CASE("sink form: only an ESP32-C6 is offered PCM decoded here with a caveat",
          "[hearth][sink_form]") {
    SinkFacts sink;
    CHECK(iclforge::hearth::form_policy(sink).pcm_fallback);
    CHECK(iclforge::hearth::form_policy(sink).pcm_caveat.empty());
    sink.hardware = "ESP32-S3";
    CHECK(iclforge::hearth::form_policy(sink).pcm_fallback);
    CHECK(iclforge::hearth::form_policy(sink).pcm_caveat.empty());
    sink.hardware = "ESP32-P4";
    CHECK(iclforge::hearth::form_policy(sink).pcm_fallback);
    CHECK(iclforge::hearth::form_policy(sink).pcm_caveat.empty());
    sink.hardware = "esp32-c6";
    CHECK(iclforge::hearth::form_policy(sink).pcm_fallback);
    CHECK_FALSE(iclforge::hearth::form_policy(sink).pcm_caveat.empty());
    sink.hardware = "ESP32-C6-DevKitC";
    CHECK(iclforge::hearth::form_policy(sink).pcm_fallback);
    CHECK_FALSE(iclforge::hearth::form_policy(sink).pcm_caveat.empty());
}

TEST_CASE("sink form: a caveat is added to the reason of a sink sent PCM, and to no other",
          "[hearth][sink_form]") {
    SinkFacts c6 = hearth_sink({sp::DataType::kAc3});
    c6.hardware = "ESP32-C6";
    limit(c6, sp::DataType::kAc3, 2);
    c6.pcm_formats.push_back({.channels = 2, .sample_rate = k48k, .bit_depth = 16});
    const auto policy = iclforge::hearth::form_policy(c6);
    const auto sent = choose_sink_form(ac3(6), c6, policy);
    REQUIRE(sent.form == SinkForm::kPcm);
    CHECK(mentions(sent.reason, "up to 2 channels"));
    CHECK(mentions(sent.reason, "drop out"));
    // A stream it takes as it is carries no caveat: nothing about it is at risk.
    const auto coded = choose_sink_form(ac3(2), c6, policy);
    REQUIRE(coded.form == SinkForm::kCoded);
    CHECK_FALSE(mentions(coded.reason, "drop out"));
}

TEST_CASE(
    "sink form: a sink that places fewer objects than the stream has is sent PCM decoded here",
    "[hearth][sink_form][objects]") {
    // An E-AC-3 stream with sixteen objects, to a Hearth sink set to 7.1.4, which has heights.
    const StreamNeeds joc{
        .stream = BitstreamFormat::kEac3, .sample_rate = k48k, .coded_channels = 6, .objects = 16};
    SinkFacts sink = hearth_sink({sp::DataType::kEac3}, {2, 12});
    configured(sink, "7.1.4");

    SECTION("a sink that states no limit is sent the stream as it is") {
        CHECK(choose_sink_form(joc, sink).form == SinkForm::kCoded);
    }
    SECTION("a limit the stream is within leaves it coded") {
        sink.iclforge_support->max_objects[static_cast<std::size_t>(sp::DataType::kEac3)] = 16;
        CHECK(choose_sink_form(joc, sink).form == SinkForm::kCoded);
    }
    SECTION("past the limit it is decoded here, with the objects placed, and the reason says so") {
        sink.iclforge_support->max_objects[static_cast<std::size_t>(sp::DataType::kEac3)] = 4;
        const auto choice = choose_sink_form(joc, sink);
        REQUIRE(choice.form == SinkForm::kPcm);
        REQUIRE(choice.layout.has_value());
        CHECK(choice.layout->slots() == 12);
        CHECK(mentions(choice.reason, "up to 4 objects"));
        CHECK(mentions(choice.reason, "has 16"));
    }
    SECTION("0 is a limit: a sink that places none is sent PCM too") {
        sink.iclforge_support->max_objects[static_cast<std::size_t>(sp::DataType::kEac3)] = 0;
        const auto choice = choose_sink_form(joc, sink);
        CHECK(choice.form == SinkForm::kPcm);
        CHECK(mentions(choice.reason, "does not place objects"));
    }
    SECTION("a limit for another data type is not this one's") {
        sink.iclforge_support->data_types = {sp::DataType::kEac3, sp::DataType::kAc4};
        sink.iclforge_support->max_objects[static_cast<std::size_t>(sp::DataType::kAc4)] = 0;
        CHECK(choose_sink_form(joc, sink).form == SinkForm::kCoded);
    }
    SECTION("a layout with no heights leaves the objects out, so the stream is as it is") {
        configured(sink, "5.1");
        sink.iclforge_support->max_objects[static_cast<std::size_t>(sp::DataType::kEac3)] = 0;
        CHECK(choose_sink_form(joc, sink).form == SinkForm::kCoded);
    }
    SECTION("the listener's own objects setting decides over the layout") {
        sink.iclforge_support->max_objects[static_cast<std::size_t>(sp::DataType::kEac3)] = 0;
        sp::Settings settings;
        settings.decoder.objects = sp::ObjectsPolicy::kNever;
        sink.intended_settings = settings;
        CHECK(choose_sink_form(joc, sink).form == SinkForm::kCoded);
        configured(sink, "5.1");
        settings.decoder.objects = sp::ObjectsPolicy::kAlways;
        sink.intended_settings = settings;
        CHECK(choose_sink_form(joc, sink).form == SinkForm::kPcm);
    }
    SECTION("a stream with no objects is not held to the limit") {
        sink.iclforge_support->max_objects[static_cast<std::size_t>(sp::DataType::kEac3)] = 0;
        StreamNeeds bed = joc;
        bed.objects = 0;
        CHECK(choose_sink_form(bed, sink).form == SinkForm::kCoded);
    }
    SECTION(
        "where PCM decoded here is not possible the stream goes as it is, to be played as its "
        "bed") {
        sink.iclforge_support->max_objects[static_cast<std::size_t>(sp::DataType::kEac3)] = 0;
        const auto choice = choose_sink_form(joc, sink, {.pcm_fallback = false});
        CHECK(choice.form == SinkForm::kCoded);
        CHECK(mentions(choice.reason, "plays the bed without them"));
        // And a sink with no PCM to be sent says the same.
        SinkFacts no_pcm = hearth_sink({sp::DataType::kEac3}, {});
        configured(no_pcm, "7.1.4");
        no_pcm.iclforge_support->max_objects[static_cast<std::size_t>(sp::DataType::kEac3)] = 0;
        CHECK(choose_sink_form(joc, no_pcm).form == SinkForm::kCoded);
    }
}

// A sink the person has set to Opus, on a link too poor for PCM: sent decoded audio as Opus, at 2.0,
// where it lists Opus and the stream is at 48 kHz; PCM as before where it is not set, does not list
// it, or the stream is at another rate.
TEST_CASE("sink form: a sink set to Opus is sent it where it lists it", "[hearth][sink_form]") {
    SinkFacts c6 = hearth_sink({sp::DataType::kAc3});
    c6.hardware = "ESP32-C6";
    limit(c6, sp::DataType::kAc3, 2);
    c6.codecs = {"pcm", "opus"};
    c6.opus_fallback = true;

    SECTION("it is sent Opus at 2.0, and the reason says so and carries no PCM caveat") {
        const auto policy = iclforge::hearth::form_policy(c6);
        CHECK(policy.opus);
        const auto sent = choose_sink_form(ac3(6), c6, policy);
        REQUIRE(sent.form == SinkForm::kPcm);
        CHECK(sent.codec == PcmCodec::kOpus);
        REQUIRE(sent.layout.has_value());
        CHECK(sent.layout->slots() == 2);
        CHECK(mentions(sent.reason, "up to 2 channels"));
        CHECK(mentions(sent.reason, "Opus"));
        CHECK_FALSE(mentions(sent.reason, "drop out"));
    }
    SECTION("not set, it is sent PCM with the caveat") {
        c6.opus_fallback = false;
        const auto sent = choose_sink_form(ac3(6), c6, iclforge::hearth::form_policy(c6));
        REQUIRE(sent.form == SinkForm::kPcm);
        CHECK(sent.codec == PcmCodec::kPcm);
        CHECK(mentions(sent.reason, "drop out"));
    }
    SECTION("a sink that does not list Opus is sent PCM whatever was set") {
        c6.codecs = {"pcm"};
        const auto policy = iclforge::hearth::form_policy(c6);
        CHECK_FALSE(policy.opus);
        const auto sent = choose_sink_form(ac3(6), c6, policy);
        REQUIRE(sent.form == SinkForm::kPcm);
        CHECK(sent.codec == PcmCodec::kPcm);
    }
    SECTION("a stream it takes as it is is sent as it is") {
        const auto sent = choose_sink_form(ac3(2), c6, iclforge::hearth::form_policy(c6));
        CHECK(sent.form == SinkForm::kCoded);
        CHECK(sent.codec == PcmCodec::kPcm);
    }
    SECTION("a stream at another rate is not sent Opus, which is listed at 48 kHz only") {
        c6.pcm_formats.push_back({.channels = 2, .sample_rate = 44100, .bit_depth = 16});
        const StreamNeeds at_44k{
            .stream = BitstreamFormat::kAc3, .sample_rate = 44100, .coded_channels = 6};
        c6.iclforge_support->sample_rates = {44100};
        const auto sent = choose_sink_form(at_44k, c6, iclforge::hearth::form_policy(c6));
        CHECK(sent.codec == PcmCodec::kPcm);
    }
    SECTION("in a group it shares the 2.0 render the stereo player has") {
        SinkFacts stereo;
        stereo.name = "Stereo";
        stereo.kind = SinkKind::kStandardPlayer;
        stereo.pcm_formats.push_back({.channels = 2, .sample_rate = k48k, .bit_depth = 16});
        const auto master = iclforge::render::OutputLayout::parse("5.1");
        REQUIRE(master.has_value());
        const std::vector<SinkFacts> sinks{c6, stereo};
        const auto plan = plan_group_forms(ac3(6), *master, sinks);
        REQUIRE(plan.members.size() == 2);
        CHECK(plan.members[0].action == MemberAction::kPcm);
        CHECK(plan.members[0].codec == PcmCodec::kOpus);
        CHECK(plan.members[0].channels == 2);
        CHECK(plan.members[0].label == "Opus · 2.0");
        CHECK(plan.members[1].action == MemberAction::kPcm);
        CHECK(plan.members[1].codec == PcmCodec::kPcm);
        CHECK(plan.members[1].label == "PCM · 2.0");
        CHECK(plan.variants.size() == 1);
    }
}

TEST_CASE("sink form: a layout the sink's PCM cannot carry is folded to the widest it can",
          "[hearth][sink_form]") {
    const StreamNeeds ac4{
        .stream = BitstreamFormat::kAc4, .sample_rate = k48k, .coded_channels = 12};
    const auto choose = [&](std::vector<std::uint16_t> pcm, const std::string& layout) {
        SinkFacts sink = hearth_sink({sp::DataType::kAc3}, std::move(pcm));
        configured(sink, layout);
        return choose_sink_form(ac4, sink);
    };

    SECTION("a sink that lists the layout's own width is sent that layout") {
        const auto choice = choose({2, 10}, "5.1.4");
        REQUIRE(choice.form == SinkForm::kPcm);
        CHECK(choice.layout->slots() == 10);
    }
    SECTION("7.1.4 on a sink that lists up to 5.1 is folded to 5.1") {
        const auto choice = choose({2, 6}, "7.1.4");
        REQUIRE(choice.form == SinkForm::kPcm);
        CHECK(choice.layout->slots() == 6);
        CHECK(mentions(choice.reason, "folded to 5.1"));
    }
    SECTION("5.1.4 on a stereo-only sink is folded to 2.0") {
        const auto choice = choose({2}, "5.1.4");
        REQUIRE(choice.form == SinkForm::kPcm);
        CHECK(choice.layout->slots() == 2);
        CHECK(mentions(choice.reason, "folded to 2.0"));
    }
    SECTION("a layout narrower than every PCM format the sink lists has nowhere to go") {
        const auto choice = choose({6}, "2.0");
        CHECK(choice.form == SinkForm::kNone);
        CHECK(mentions(choice.reason, "2.0"));
    }
}

TEST_CASE("sink form: PCM is sent at the stream's own rate, which this app does not resample",
          "[hearth][sink_form]") {
    SinkFacts sink;
    sink.kind = SinkKind::kStandardPlayer;
    sink.pcm_formats.push_back({.channels = 2, .sample_rate = 44100, .bit_depth = 16});
    const auto choice = choose_sink_form(ac3(6), sink);
    CHECK(choice.form == SinkForm::kNone);
    CHECK(mentions(choice.reason, "44100 Hz only"));
    CHECK(mentions(choice.reason, "48000"));
}

TEST_CASE("sink form: a sink that lists no PCM and cannot take the stream is sent nothing",
          "[hearth][sink_form]") {
    SinkFacts sink = hearth_sink({sp::DataType::kAc3}, {});
    const StreamNeeds ac4{
        .stream = BitstreamFormat::kAc4, .sample_rate = k48k, .coded_channels = 2};
    const auto choice = choose_sink_form(ac4, sink);
    CHECK(choice.form == SinkForm::kNone);
    CHECK(mentions(choice.reason, "does not decode AC-4"));
    CHECK(mentions(choice.reason, "no PCM"));
}

// ---- karaoke (planning/karaoke-phase2.md, W7) ---------------------------------------------------
//
// A karaoke AC-3 stream whose reproduction is wanted goes as it is only to a sink that is told how
// to reproduce it: a sink whose decoder is not played the coded channels, and a group would hear
// two things. That sink is sent PCM decoded here, where the reproduction is applied.

namespace {

// A paired sink that takes the karaoke keys (`keys`), and the settings command, as a board of
// today does.
SinkFacts karaoke_sink(std::vector<std::string> keys, std::vector<std::uint16_t> pcm = {2}) {
    SinkFacts sink = hearth_sink({sp::DataType::kAc3, sp::DataType::kEac3}, std::move(pcm));
    sink.iclforge_support->decoder_settings = std::move(keys);
    sp::State state;
    state.supported_commands = {sp::Command::kVolume, sp::Command::kMute, sp::Command::kSettings};
    sink.iclforge_state = state;
    return sink;
}

StreamNeeds karaoke_stream(sp::Karaoke reproduction, sp::KaraokeVocals vocals = sp::KaraokeVocals::kBoth) {
    StreamNeeds needs = ac3(6);
    needs.karaoke = iclforge::hearth::KaraokeChoice{.reproduction = reproduction, .vocals = vocals};
    return needs;
}

}  // namespace

TEST_CASE("sink form: a karaoke stream goes as it is to a sink that takes the karaoke keys",
          "[hearth][sink_form][karaoke]") {
    const SinkFacts sink = karaoke_sink({"karaoke", "karaoke_vocals"});
    for (const sp::Karaoke reproduction : {sp::Karaoke::kAware, sp::Karaoke::kCapable}) {
        const auto choice = choose_sink_form(karaoke_stream(reproduction), sink);
        REQUIRE(choice.form == SinkForm::kCoded);
        CHECK(mentions(choice.reason, "reproduces the karaoke"));
    }
    // A stream that is not karaoke, or is played as coded, is told nothing and is not asked for it.
    CHECK_FALSE(mentions(choose_sink_form(ac3(6), sink).reason, "karaoke"));
}

TEST_CASE("sink form: a sink that does not take the keys is sent a karaoke stream as PCM",
          "[hearth][sink_form][karaoke]") {
    // A board from before the keys: the other decoder settings and not karaoke.
    const SinkFacts old_board = karaoke_sink({"mode", "downmix", "objects"}, {2, 6});
    const auto choice = choose_sink_form(karaoke_stream(sp::Karaoke::kAware), old_board);
    REQUIRE(choice.form == SinkForm::kPcm);
    CHECK(mentions(choice.reason, "does not reproduce karaoke"));
    CHECK(mentions(choice.reason, "decoded here"));
    // And the same board is sent the stream as it is when the singer wants none of it.
    CHECK(choose_sink_form(ac3(6), old_board).form == SinkForm::kCoded);
}

TEST_CASE("sink form: the capable reproduction needs the vocals key too",
          "[hearth][sink_form][karaoke]") {
    const SinkFacts aware_only = karaoke_sink({"karaoke"}, {2, 6});
    CHECK(choose_sink_form(karaoke_stream(sp::Karaoke::kAware), aware_only).form == SinkForm::kCoded);
    const auto capable = choose_sink_form(karaoke_stream(sp::Karaoke::kCapable), aware_only);
    REQUIRE(capable.form == SinkForm::kPcm);
    CHECK(mentions(capable.reason, "karaoke_vocals"));
}

TEST_CASE("sink form: a sink that takes no settings cannot be told, so it is sent PCM",
          "[hearth][sink_form][karaoke]") {
    SinkFacts sink = karaoke_sink({"karaoke", "karaoke_vocals"}, {2, 6});
    sink.iclforge_state->supported_commands = {sp::Command::kVolume, sp::Command::kMute};
    const auto choice = choose_sink_form(karaoke_stream(sp::Karaoke::kAware), sink);
    REQUIRE(choice.form == SinkForm::kPcm);
    CHECK(mentions(choice.reason, "takes no settings"));

    // One that has not said what it supports yet is the same: nothing says it can be told.
    sink.iclforge_state.reset();
    CHECK(choose_sink_form(karaoke_stream(sp::Karaoke::kAware), sink).form == SinkForm::kPcm);
}

TEST_CASE("sink form: where no PCM can be sent, the stream plays as coded and the reason says so",
          "[hearth][sink_form][karaoke]") {
    const SinkFacts old_board = karaoke_sink({"mode"}, {});
    const auto choice = choose_sink_form(karaoke_stream(sp::Karaoke::kAware), old_board);
    // Not "nothing": a person who asked for karaoke still hears the stream.
    REQUIRE(choice.form == SinkForm::kCoded);
    CHECK(mentions(choice.reason, "played as coded"));

    // Nor with the fallback off, which is the person's own "never decode in Hearth".
    const SinkFacts with_pcm = karaoke_sink({"mode"}, {2});
    const auto off = choose_sink_form(karaoke_stream(sp::Karaoke::kAware), with_pcm, {.pcm_fallback = false});
    CHECK(off.form == SinkForm::kCoded);
    CHECK(mentions(off.reason, "played as coded"));
}

// ---- the codec (planning/karaoke-eac3.md, D6) ----------------------------------------------------
//
// A sink that takes the karaoke keys has a path for the codecs it lists: AC-3's alone when it
// lists none, as a board from before E-AC-3's reproduction does. An E-AC-3 karaoke stream sent
// to such a board would be heard as coded beside a member that reproduces it, so it is decoded
// here and sent as PCM.

namespace {

StreamNeeds eac3_karaoke_stream(sp::Karaoke reproduction) {
    StreamNeeds needs = karaoke_stream(reproduction);
    needs.stream = BitstreamFormat::kEac3;
    return needs;
}

}  // namespace

TEST_CASE("sink form: an E-AC-3 karaoke stream goes as it is only to a sink that lists E-AC-3",
          "[hearth][sink_form][karaoke]") {
    // A board that reproduces karaoke and says nothing of codecs: AC-3's alone.
    const SinkFacts before = karaoke_sink({"karaoke", "karaoke_vocals"}, {2, 6});
    const auto sent_pcm = choose_sink_form(eac3_karaoke_stream(sp::Karaoke::kAware), before);
    REQUIRE(sent_pcm.form == SinkForm::kPcm);
    CHECK(mentions(sent_pcm.reason, "reproduces karaoke for AC-3, not for E-AC-3"));
    CHECK(mentions(sent_pcm.reason, "decoded here"));
    // Its AC-3 path is what it has: an AC-3 karaoke stream still goes as it is.
    CHECK(choose_sink_form(karaoke_stream(sp::Karaoke::kAware), before).form == SinkForm::kCoded);

    // The same board once its firmware lists E-AC-3.
    SinkFacts newer = karaoke_sink({"karaoke", "karaoke_vocals"}, {2, 6});
    newer.iclforge_support->karaoke_codecs = {sp::DataType::kAc3, sp::DataType::kEac3};
    for (const sp::Karaoke reproduction : {sp::Karaoke::kAware, sp::Karaoke::kCapable}) {
        const auto coded = choose_sink_form(eac3_karaoke_stream(reproduction), newer);
        REQUIRE(coded.form == SinkForm::kCoded);
        CHECK(mentions(coded.reason, "reproduces the karaoke"));
    }
    CHECK(choose_sink_form(karaoke_stream(sp::Karaoke::kAware), newer).form == SinkForm::kCoded);

    // A list that names E-AC-3 and not AC-3 is what it says.
    SinkFacts eac3_only = karaoke_sink({"karaoke"}, {2, 6});
    eac3_only.iclforge_support->karaoke_codecs = {sp::DataType::kEac3};
    CHECK(choose_sink_form(eac3_karaoke_stream(sp::Karaoke::kAware), eac3_only).form ==
          SinkForm::kCoded);
    const auto ac3_refused = choose_sink_form(karaoke_stream(sp::Karaoke::kAware), eac3_only);
    REQUIRE(ac3_refused.form == SinkForm::kPcm);
    CHECK(mentions(ac3_refused.reason, "reproduces karaoke for E-AC-3, not for AC-3"));

    // An E-AC-3 stream the singer wants as coded asks nothing of the codec list.
    StreamNeeds as_coded = eac3_karaoke_stream(sp::Karaoke::kAware);
    as_coded.karaoke.reset();
    CHECK(choose_sink_form(as_coded, before).form == SinkForm::kCoded);
}

TEST_CASE("sink form: where an E-AC-3 karaoke stream cannot be sent PCM, it plays as coded and says so",
          "[hearth][sink_form][karaoke]") {
    const SinkFacts no_pcm = karaoke_sink({"karaoke", "karaoke_vocals"}, {});
    const auto choice = choose_sink_form(eac3_karaoke_stream(sp::Karaoke::kAware), no_pcm);
    REQUIRE(choice.form == SinkForm::kCoded);
    CHECK(mentions(choice.reason, "played as coded"));
    CHECK(mentions(choice.reason, "not for E-AC-3"));
}

// ---- the part levels (planning/karaoke-phase2.md, W10c) -----------------------------------------
//
// A level that moves the melody or a vocal is for the sink to apply, or for this app to apply in
// the PCM it sends: a sink that took the reproduction and not the level would play the table, and
// a group would be at two levels.

namespace {

StreamNeeds leveled_stream(sp::Karaoke reproduction, sp::KaraokeVocals vocals, double melody_db,
                           double vocal1_db, double vocal2_db) {
    StreamNeeds needs = karaoke_stream(reproduction, vocals);
    needs.karaoke->melody_db = melody_db;
    needs.karaoke->vocal1_db = vocal1_db;
    needs.karaoke->vocal2_db = vocal2_db;
    return needs;
}

}  // namespace

TEST_CASE("sink form: a moved part level needs a sink that takes its key, else PCM",
          "[hearth][sink_form][karaoke]") {
    const std::vector<std::string> reproduces{"karaoke", "karaoke_vocals"};
    std::vector<std::string> with_levels = reproduces;
    with_levels.insert(with_levels.end(),
                       {"karaoke_melody_db", "karaoke_vocal1_db", "karaoke_vocal2_db"});
    const SinkFacts before_levels = karaoke_sink(reproduces, {2, 6});
    const SinkFacts has_levels = karaoke_sink(with_levels, {2, 6});
    const auto moved = leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kBoth, -6.0, 0.0, 0.0);

    // The table (every level 0) asks nothing of either board.
    const auto table = leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kBoth, 0.0, 0.0, 0.0);
    CHECK(choose_sink_form(table, before_levels).form == SinkForm::kCoded);
    // A moved melody goes as it is to a board that takes the key, and as PCM, named, to one that
    // has the reproduction and not the level.
    CHECK(choose_sink_form(moved, has_levels).form == SinkForm::kCoded);
    const auto refused = choose_sink_form(moved, before_levels);
    REQUIRE(refused.form == SinkForm::kPcm);
    CHECK(mentions(refused.reason, "level of the melody"));
    CHECK(mentions(refused.reason, "karaoke_melody_db"));

    // Each part names its own key.
    const auto vocal1 = choose_sink_form(
        leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kBoth, 0.0, 3.0, 0.0), before_levels);
    REQUIRE(vocal1.form == SinkForm::kPcm);
    CHECK(mentions(vocal1.reason, "karaoke_vocal1_db"));
    const auto vocal2 = choose_sink_form(
        leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kBoth, 0.0, 0.0, -24.0), before_levels);
    REQUIRE(vocal2.form == SinkForm::kPcm);
    CHECK(mentions(vocal2.reason, "karaoke_vocal2_db"));

    // A level of a vocal the choice leaves out changes nothing that is heard, so it asks nothing.
    CHECK(choose_sink_form(leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kV1, 0.0, 0.0, -6.0),
                           before_levels)
              .form == SinkForm::kCoded);
    CHECK(choose_sink_form(leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kV2, 0.0, 6.0, 0.0),
                           before_levels)
              .form == SinkForm::kCoded);
    CHECK(choose_sink_form(leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kNone, 0.0, 6.0, -6.0),
                           before_levels)
              .form == SinkForm::kCoded);
    // ... and the aware reproduction has no levels at all, whatever the settings hold.
    CHECK(choose_sink_form(leveled_stream(sp::Karaoke::kAware, sp::KaraokeVocals::kBoth, -6.0, 6.0, -6.0),
                           before_levels)
              .form == SinkForm::kCoded);
    // A value at the very bottom is a level too (off), and one that is not a number is the table.
    CHECK(choose_sink_form(leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kBoth, -90.0, 0.0, 0.0),
                           before_levels)
              .form == SinkForm::kPcm);
    CHECK(choose_sink_form(
              leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kBoth,
                             std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0),
              before_levels)
              .form == SinkForm::kCoded);
}

TEST_CASE("sink form: the levels a choice carries are the ones that are heard, held to the span",
          "[hearth][sink_form][karaoke]") {
    using iclforge::hearth::KaraokeChoice;
    KaraokeChoice choice{.reproduction = sp::Karaoke::kCapable,
                         .vocals = sp::KaraokeVocals::kBoth,
                         .melody_db = 40.0,
                         .vocal1_db = -6.0,
                         .vocal2_db = -90.0};
    CHECK(choice.heard_levels_db() == std::array<double, 3>{12.0, -6.0, -24.0});
    choice.vocals = sp::KaraokeVocals::kV1;
    CHECK(choice.heard_levels_db() == std::array<double, 3>{12.0, -6.0, 0.0});
    choice.vocals = sp::KaraokeVocals::kV2;
    CHECK(choice.heard_levels_db() == std::array<double, 3>{12.0, 0.0, -24.0});
    choice.vocals = sp::KaraokeVocals::kNone;
    CHECK(choice.heard_levels_db() == std::array<double, 3>{12.0, 0.0, 0.0});
    choice.reproduction = sp::Karaoke::kAware;
    choice.vocals = sp::KaraokeVocals::kBoth;
    CHECK(choice.heard_levels_db() == std::array<double, 3>{0.0, 0.0, 0.0});
}

TEST_CASE("sink form: where no PCM can be sent a board without the level key plays the table",
          "[hearth][sink_form][karaoke]") {
    const SinkFacts no_pcm = karaoke_sink({"karaoke", "karaoke_vocals"}, {});
    const auto choice = choose_sink_form(
        leveled_stream(sp::Karaoke::kCapable, sp::KaraokeVocals::kBoth, -6.0, 0.0, 0.0), no_pcm);
    // It still reproduces the karaoke (it takes the keys), so this is not "played as coded".
    REQUIRE(choice.form == SinkForm::kCoded);
    CHECK(mentions(choice.reason, "at the table's levels"));
    CHECK_FALSE(mentions(choice.reason, "played as coded"));
}

TEST_CASE("sink form: a group of a board that takes karaoke and one that does not plays one reproduction",
          "[hearth][sink_form][karaoke]") {
    SinkFacts takes = karaoke_sink({"karaoke", "karaoke_vocals"}, {2});
    takes.id = "takes";
    SinkFacts old_board = karaoke_sink({"mode"}, {2});
    old_board.id = "old";
    const std::vector<SinkFacts> sinks{takes, old_board};
    const auto master = iclforge::render::OutputLayout::stereo();
    const auto plan = plan_group_forms(karaoke_stream(sp::Karaoke::kAware), master, sinks);
    REQUIRE(plan.members.size() == 2);
    // The board that takes it decodes for itself; the other is sent PCM that has it applied.
    CHECK(plan.members[0].action == MemberAction::kCoded);
    CHECK(plan.members[1].action == MemberAction::kPcm);
}
