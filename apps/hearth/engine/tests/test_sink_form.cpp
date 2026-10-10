#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
using iclforge::hearth::PairState;
using iclforge::hearth::PcmFormat;
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
