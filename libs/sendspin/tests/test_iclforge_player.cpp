#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/frames.hpp"
#include "iclforge/sendspin/json.hpp"

// _iclforge_player@v1's objects (planning/hearth-sendspin-extension.md, The role
// _iclforge_player@v1): each writer's text byte for byte against the page's tables, each reader
// against what it must refuse and what it must let through, and the settings a sink refuses with
// the revision it names.

namespace {

namespace ac = iclforge::sendspin::player;
namespace json = iclforge::sendspin::json;

// A parsed object whose values stay valid while the Parsed lives.
struct Parsed {
    std::string text;
    std::vector<json::Token> tokens;
    json::Document document;

    explicit Parsed(std::string source) : text(std::move(source)) {
        const bool parsed = static_cast<bool>(document.parse(text, tokens, 4096));
        CHECK(parsed);
    }
    // The document points into `text` and `tokens`.
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    Parsed(Parsed&&) = delete;
    Parsed& operator=(Parsed&&) = delete;
    ~Parsed() = default;

    [[nodiscard]] json::Value root() const { return document.root(); }
};

template <class T, class Write>
[[nodiscard]] std::string written(const T& value, Write write) {
    std::string out;
    json::Writer w(out);
    write(w, value);
    return out;
}

[[nodiscard]] ac::Support board_support() {
    ac::Support support;
    support.data_types = {ac::DataType::kAc3, ac::DataType::kEac3};
    support.sample_rates = {48000};
    support.outputs.count = 6;
    support.outputs.bit_depth = 32;
    support.outputs.bit_depths = {16, 24, 32};
    support.layout_grammar = 1;
    support.management.routing = true;
    support.management.trim_db = {-12.0, 12.0};
    support.management.max_delay_ms = 50.0;
    support.management.crossover_hz = {40.0, 250.0};
    support.management.identify = true;
    support.decoder_settings.assign(ac::kDecoderSettingNames.begin(), ac::kDecoderSettingNames.end());
    support.buffer_capacity = 1048576;
    return support;
}

[[nodiscard]] ac::CommandMessage full_settings() {
    ac::CommandMessage command;
    command.command = ac::Command::kSettings;
    ac::Settings& settings = command.settings;
    settings.revision = 7;
    settings.layout = "L:small,C,R:small,Ls,Rs,LFE";
    settings.routing = "0,1,2,3,4,5";
    settings.trim_db = std::vector<double>{0.0, -1.5, 0.0, -3.0, -3.0, 2.0};
    settings.delay_ms = std::vector<double>{0.0, 0.0, 0.0, 12.5, 12.5, 0.0};
    settings.crossover_hz = 80.0;
    ac::DecoderSettings& decoder = settings.decoder;
    decoder.mode = ac::DecoderMode::kRf;
    decoder.drc_cut = 0.25;
    decoder.drc_boost = 1.0;
    decoder.heavy_compression = false;
    decoder.dialnorm = true;
    decoder.downmix = ac::Downmix::kLtRt;
    decoder.ltrt_phase_shift = true;
    decoder.mix_lfe = false;
    decoder.has_programme = true;
    decoder.objects = ac::ObjectsPolicy::kNever;
    decoder.concealment = ac::Concealment::kRepeatFade;
    decoder.karaoke = ac::Karaoke::kCapable;
    decoder.karaoke_vocals = ac::KaraokeVocals::kV2;
    decoder.karaoke_melody_db = -6.0;
    decoder.karaoke_vocal1_db = 3.0;
    decoder.karaoke_vocal2_db = -24.0;
    return command;
}

}  // namespace

TEST_CASE("iclforge_player: names and IDs", "[sendspin][iclforge]") {
    CHECK(ac::kRole == "_iclforge_player@v1");
    CHECK(ac::kSupportKey == "_iclforge_player@v1_support");
    CHECK(ac::kObjectKey == "_iclforge_player");
    CHECK(iclforge::sendspin::message_id::kIclforgeBurst == 192);
    CHECK(ac::data_type_name(ac::DataType::kAc3) == "ac3");
    CHECK(ac::data_type_name(ac::DataType::kEac3) == "eac3");
    CHECK(ac::data_type_name(ac::DataType::kAc4) == "ac4");
    CHECK(ac::burst_data_type(ac::DataType::kAc3) == iclforge::sendspin::BurstDataType::kAc3);
    CHECK(ac::burst_data_type(ac::DataType::kEac3) == iclforge::sendspin::BurstDataType::kEac3);
    CHECK(ac::burst_data_type(ac::DataType::kAc4) == iclforge::sendspin::BurstDataType::kAc4);
}

TEST_CASE("iclforge_player: an AC-4 stream carries any of IEC 61937-14's four burst types",
          "[sendspin][iclforge][ac4]") {
    using iclforge::sendspin::BurstDataType;
    for (const BurstDataType burst : {BurstDataType::kAc4, BurstDataType::kAc4Hbr4,
                                      BurstDataType::kAc4Hbr16, BurstDataType::kAc4Ld}) {
        CHECK(ac::carries(ac::DataType::kAc4, burst));
        CHECK_FALSE(ac::carries(ac::DataType::kEac3, burst));
    }
    CHECK_FALSE(ac::carries(ac::DataType::kAc4, BurstDataType::kAc3));
    CHECK(ac::carries(ac::DataType::kAc3, BurstDataType::kAc3));
    CHECK_FALSE(ac::carries(ac::DataType::kAc3, BurstDataType::kEac3));
    CHECK(ac::carries(ac::DataType::kEac3, BurstDataType::kEac3));
}

TEST_CASE("iclforge_player: the support object", "[sendspin][iclforge]") {
    const std::string text = written(board_support(), ac::write_support);
    CHECK(text == R"({"data_types":["ac3","eac3"],"sample_rates":[48000],)"
                  R"("outputs":{"count":6,"bit_depth":32,"bit_depths":[16,24,32]},"layout_grammar":1,)"
                  R"("management":{"routing":true,"trim_db":[-12,12],"delay_ms":[0,50],"crossover_hz":[40,250],"identify":true},)"
                  R"("decoder_settings":["mode","drc_cut","drc_boost","heavy_compression","dialnorm","downmix",)"
                  R"("ltrt_phase_shift","mix_lfe","programme","objects","concealment","karaoke","karaoke_vocals",)"
                  R"("karaoke_melody_db","karaoke_vocal1_db","karaoke_vocal2_db"],)"
                  R"("buffer_capacity":1048576})");

    const Parsed parsed(text);
    const std::optional<ac::Support> read = ac::read_support(parsed.root());
    REQUIRE(read.has_value());
    CHECK(read->data_types == board_support().data_types);
    CHECK(read->sample_rates == std::vector<std::int32_t>{48000});
    CHECK(read->outputs.count == 6);
    CHECK(read->outputs.bit_depth == 32);
    CHECK(read->outputs.bit_depths == std::vector<std::int32_t>{16, 24, 32});
    CHECK(read->management.routing);
    CHECK(read->management.trim_db[0] == -12.0);
    CHECK(read->management.max_delay_ms == 50.0);
    CHECK(read->management.crossover_hz[1] == 250.0);
    CHECK(read->decoder_settings.size() == ac::kDecoderSettingNames.size());
    CHECK(read->buffer_capacity == 1048576);

    const auto read_text = [](std::string source) { return ac::read_support(Parsed(std::move(source)).root()); };
    const std::string outputs = R"("outputs":{"count":2,"bit_depth":16,"bit_depths":[16]},"layout_grammar":1,)";
    const std::string management =
        R"("management":{"routing":false,"trim_db":[0,0],"delay_ms":[0,0],"crossover_hz":[80,80],"identify":false},)";
    const std::string tail = R"("decoder_settings":[],"buffer_capacity":65536})";
    // A sink of a later day with a data type this reader does not know still offers E-AC-3: the
    // unknown type is left out.
    const std::optional<ac::Support> later = read_text(
        R"({"data_types":["mpegh","eac3"],"sample_rates":[48000],)" + outputs + management + tail);
    REQUIRE(later.has_value());
    CHECK(later->data_types == std::vector<ac::DataType>{ac::DataType::kEac3});
    CHECK(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],"future":{},)" + outputs + management + tail));
    // AC-4 (planning/ac4.md, D11).
    const std::optional<ac::Support> with_ac4 =
        read_text(R"({"data_types":["ac3","eac3","ac4"],"sample_rates":[48000],)" + outputs +
                  management + tail);
    REQUIRE(with_ac4.has_value());
    CHECK(with_ac4->data_types ==
          std::vector<ac::DataType>{ac::DataType::kAc3, ac::DataType::kEac3, ac::DataType::kAc4});

    CHECK_FALSE(read_text(R"({"data_types":["mpegh"],"sample_rates":[48000],)" + outputs +
                          management + tail));
    CHECK_FALSE(read_text(R"({"data_types":[],"sample_rates":[48000],)" + outputs + management + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3",3],"sample_rates":[48000],)" + outputs + management + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[0],)" + outputs + management + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + management + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + outputs + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + outputs +
                          R"("management":{"routing":false,"trim_db":[6,-6],"delay_ms":[0,0],"crossover_hz":[80,80],"identify":false},)" +
                          tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + outputs + management +
                          R"("decoder_settings":[],"buffer_capacity":-1})"));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + outputs + management +
                          R"("buffer_capacity":65536})"));
    CHECK_FALSE(ac::read_support(Parsed(R"(["eac3"])").root()));
}

TEST_CASE("iclforge_player: the state object", "[sendspin][iclforge]") {
    ac::State state;
    state.volume = 70;
    state.muted = false;
    state.output_delay_ms = 20;
    state.required_lead_time_ms = 250;
    state.min_buffer_ms = 200;
    state.supported_commands = {ac::Command::kVolume, ac::Command::kMute, ac::Command::kSetOutputDelay,
                                ac::Command::kSettings, ac::Command::kIdentify};
    state.settings_revision = 3;
    state.settings_error = ac::SettingsError{.revision = 4, .why = "trim_db needs one value per output"};
    state.decoder = ac::DecoderReport{.data_type = ac::DataType::kEac3,
                                      .acmod = 7,
                                      .lfe = true,
                                      .substreams = 2,
                                      .objects = 11,
                                      .objects_placed = true,
                                      .dialnorm = -31.0};
    state.levels = std::vector<ac::Level>{
        ac::Level{.output = 0, .peak_db = -6.04, .rms_db = -20.0},
        ac::Level{.output = 1, .peak_db = -std::numeric_limits<double>::infinity(), .rms_db = -200.0},
    };
    state.counters.bursts_played = 1000;
    state.counters.underruns = 1;
    state.counters.late_chunks = 2;
    state.counters.dropped_chunks = 3;
    state.counters.invalid_chunks = 4;

    const std::string text = written(state, ac::write_state);
    CHECK(text == R"({"volume":70,"muted":false,"output_delay_ms":20,"required_lead_time_ms":250,"min_buffer_ms":200,)"
                  R"("supported_commands":["volume","mute","set_output_delay","settings","identify"],"settings_revision":3,)"
                  R"("settings_error":{"revision":4,"why":"trim_db needs one value per output"},)"
                  R"("decoder":{"data_type":"eac3","acmod":7,"lfe":true,"substreams":2,"objects":11,"objects_placed":true,"dialnorm":-31},)"
                  R"("levels":[{"output":0,"peak_db":-6,"rms_db":-20},{"output":1,"peak_db":-120,"rms_db":-120}],)"
                  R"("counters":{"bursts_played":1000,"underruns":1,"late_chunks":2,"dropped_chunks":3,"invalid_chunks":4}})");

    const Parsed parsed(text);
    const std::optional<ac::State> read = ac::read_state(parsed.root());
    REQUIRE(read.has_value());
    CHECK(read->volume == 70);
    CHECK(read->muted == false);
    CHECK(read->output_delay_ms == 20);
    CHECK(read->required_lead_time_ms == 250);
    CHECK(read->min_buffer_ms == 200);
    CHECK(read->supported_commands == state.supported_commands);
    CHECK(read->settings_revision == 3);
    REQUIRE(read->settings_error.has_value());
    CHECK(read->settings_error->revision == 4);
    REQUIRE(read->decoder.has_value());
    CHECK(read->decoder->acmod == 7);
    CHECK(read->decoder->objects == 11);
    CHECK(read->decoder->dialnorm == -31.0);
    REQUIRE(read->levels.has_value());
    REQUIRE(read->levels->size() == 2);
    CHECK((*read->levels)[1].peak_db == ac::kSilenceDb);
    CHECK(read->counters.invalid_chunks == 4);
    CHECK_FALSE(read->why.has_value());

    const auto read_text = [](const std::string& fields) {
        return ac::read_state(Parsed("{" + fields + "}").root());
    };
    const std::string counters =
        R"("counters":{"bursts_played":0,"underruns":0,"late_chunks":0,"dropped_chunks":0,"invalid_chunks":0})";
    const std::string timing = R"("output_delay_ms":0,"required_lead_time_ms":0,"min_buffer_ms":0,)";
    const std::string idle = timing + R"("supported_commands":["settings","louder"],"settings_revision":0,)";
    const std::optional<ac::State> quiet = read_text(idle + counters + R"(,"why":"The output went away.")");
    REQUIRE(quiet.has_value());
    // A command this reader does not know is left out.
    CHECK(quiet->supported_commands == std::vector<ac::Command>{ac::Command::kSettings});
    CHECK(quiet->why == "The output went away.");
    CHECK_FALSE(quiet->decoder.has_value());

    CHECK_FALSE(read_text(idle + R"("why":"No counters.")"));
    CHECK_FALSE(read_text(R"("output_delay_ms":5001,"required_lead_time_ms":0,"min_buffer_ms":0,)"
                          R"("supported_commands":[],"settings_revision":0,)" + counters));
    CHECK_FALSE(read_text(R"("volume":101,)" + idle + counters));
    CHECK_FALSE(read_text(idle + counters + R"(,"settings_error":{"revision":1})"));
    CHECK_FALSE(read_text(idle + counters + R"(,"levels":[{"output":0,"peak_db":null,"rms_db":-20}])"));
    CHECK_FALSE(read_text(idle + counters +
                          R"(,"decoder":{"data_type":"eac3","acmod":8,"lfe":true,"substreams":1,"objects":0,"objects_placed":false,"dialnorm":-31})"));
    CHECK_FALSE(read_text(timing + R"("supported_commands":"settings","settings_revision":0,)" + counters));
}

TEST_CASE("iclforge_player: the chunk limit a sink states", "[sendspin][iclforge]") {
    // Written only when it is stated, so a sink that does not say writes what it always did.
    ac::Support support = board_support();
    CHECK(written(support, ac::write_support).find("max_chunk_bytes") == std::string::npos);

    support.max_chunk_bytes = 4096;
    const std::string text = written(support, ac::write_support);
    CHECK(text.ends_with(R"("buffer_capacity":1048576,"max_chunk_bytes":4096})"));
    const std::optional<ac::Support> read = ac::read_support(Parsed(text).root());
    REQUIRE(read.has_value());
    CHECK(read->max_chunk_bytes == 4096);
    CHECK(read->buffer_capacity == 1048576);

    // A sink from before the key sends none, and that reads as not stated.
    const std::optional<ac::Support> before =
        ac::read_support(Parsed(written(board_support(), ac::write_support)).root());
    REQUIRE(before.has_value());
    CHECK(before->max_chunk_bytes == 0);

    // A limit that is not a size is refused with the object, as a bad buffer_capacity is.
    const auto with_limit = [&](const std::string& value) {
        std::string source = written(board_support(), ac::write_support);
        source.pop_back();  // the closing brace
        return ac::read_support(Parsed(source + R"(,"max_chunk_bytes":)" + value + "}").root());
    };
    CHECK(with_limit("1").has_value());
    CHECK(with_limit("1048576").has_value());
    CHECK_FALSE(with_limit("0"));
    CHECK_FALSE(with_limit("-4096"));
    CHECK_FALSE(with_limit("1048577"));
    CHECK_FALSE(with_limit("40.5"));
    CHECK_FALSE(with_limit(R"("4096")"));
    CHECK_FALSE(with_limit("null"));
}

TEST_CASE("iclforge_player: the channel limit a sink states per data type",
          "[sendspin][iclforge]") {
    // Written only when some type has one, so a sink that states none writes what it always did.
    ac::Support support = board_support();
    CHECK(written(support, ac::write_support).find("max_coded_channels") == std::string::npos);

    support.max_coded_channels[static_cast<std::size_t>(ac::DataType::kAc3)] = 2;
    support.max_coded_channels[static_cast<std::size_t>(ac::DataType::kEac3)] = 2;
    const std::string text = written(support, ac::write_support);
    CHECK(text.ends_with(R"("buffer_capacity":1048576,"max_coded_channels":{"ac3":2,"eac3":2}})"));
    const std::optional<ac::Support> read = ac::read_support(Parsed(text).root());
    REQUIRE(read.has_value());
    CHECK(read->max_coded_channels_of(ac::DataType::kAc3) == 2);
    CHECK(read->max_coded_channels_of(ac::DataType::kEac3) == 2);
    // A type the object does not name has no stated limit, which is not a limit of none.
    CHECK(read->max_coded_channels_of(ac::DataType::kAc4) == 0);

    // A sink from before the key sends none, and every type reads as not stated.
    const std::optional<ac::Support> before =
        ac::read_support(Parsed(written(board_support(), ac::write_support)).root());
    REQUIRE(before.has_value());
    for (const ac::DataType type : {ac::DataType::kAc3, ac::DataType::kEac3, ac::DataType::kAc4}) {
        CHECK(before->max_coded_channels_of(type) == 0);
    }

    const auto with_limits = [&](const std::string& value) {
        std::string source = written(board_support(), ac::write_support);
        source.pop_back();  // the closing brace
        return ac::read_support(Parsed(source + R"(,"max_coded_channels":)" + value + "}").root());
    };
    // A name this reader does not know is skipped, as an unknown key is.
    const std::optional<ac::Support> future = with_limits(R"({"ac3":6,"dts":8})");
    REQUIRE(future.has_value());
    CHECK(future->max_coded_channels_of(ac::DataType::kAc3) == 6);
    CHECK(with_limits("{}").has_value());
    CHECK(with_limits(R"({"ac4":64})").has_value());
    // A count that is not a count, or a limit that is not an object, is refused with the object.
    CHECK_FALSE(with_limits(R"({"ac3":0})"));
    CHECK_FALSE(with_limits(R"({"ac3":65})"));
    CHECK_FALSE(with_limits(R"({"ac3":-2})"));
    CHECK_FALSE(with_limits(R"({"ac3":2.5})"));
    CHECK_FALSE(with_limits(R"({"ac3":"2"})"));
    CHECK_FALSE(with_limits("2"));
    CHECK_FALSE(with_limits("[2,2]"));
    CHECK_FALSE(with_limits("null"));
}

TEST_CASE("iclforge_player: the object limit a sink states per data type", "[sendspin][iclforge]") {
    // Written only when some type has one, so a sink that states none writes what it always did.
    ac::Support support = board_support();
    CHECK(written(support, ac::write_support).find("max_objects") == std::string::npos);

    // 0 is a statement (the bed alone), where a type the object does not name is none.
    support.max_objects[static_cast<std::size_t>(ac::DataType::kEac3)] = 0;
    support.max_objects[static_cast<std::size_t>(ac::DataType::kAc4)] = 4;
    const std::string text = written(support, ac::write_support);
    CHECK(text.ends_with(R"("buffer_capacity":1048576,"max_objects":{"eac3":0,"ac4":4}})"));
    const std::optional<ac::Support> read = ac::read_support(Parsed(text).root());
    REQUIRE(read.has_value());
    REQUIRE(read->max_objects_of(ac::DataType::kEac3).has_value());
    CHECK(*read->max_objects_of(ac::DataType::kEac3) == 0);
    CHECK(read->max_objects_of(ac::DataType::kAc4) == 4);
    CHECK_FALSE(read->max_objects_of(ac::DataType::kAc3).has_value());

    // A sink from before the key sends none, and every type reads as stating none.
    const std::optional<ac::Support> before =
        ac::read_support(Parsed(written(board_support(), ac::write_support)).root());
    REQUIRE(before.has_value());
    for (const ac::DataType type : {ac::DataType::kAc3, ac::DataType::kEac3, ac::DataType::kAc4}) {
        CHECK_FALSE(before->max_objects_of(type).has_value());
    }

    const auto with_limits = [&](const std::string& value) {
        std::string source = written(board_support(), ac::write_support);
        source.pop_back();  // the closing brace
        return ac::read_support(Parsed(source + R"(,"max_objects":)" + value + "}").root());
    };
    // A name this reader does not know is skipped, as an unknown key is.
    const std::optional<ac::Support> future = with_limits(R"({"eac3":15,"dts":8})");
    REQUIRE(future.has_value());
    CHECK(future->max_objects_of(ac::DataType::kEac3) == 15);
    CHECK(with_limits("{}").has_value());
    CHECK(with_limits(R"({"ac4":255})").has_value());
    // A count that is not a count, or a limit that is not an object, is refused with the object.
    CHECK_FALSE(with_limits(R"({"eac3":-1})"));
    CHECK_FALSE(with_limits(R"({"eac3":256})"));
    CHECK_FALSE(with_limits(R"({"eac3":2.5})"));
    CHECK_FALSE(with_limits(R"({"eac3":"4"})"));
    CHECK_FALSE(with_limits("4"));
    CHECK_FALSE(with_limits("[4]"));
    CHECK_FALSE(with_limits("null"));
}

TEST_CASE("iclforge_player: the frame rate a sink states per data type", "[sendspin][iclforge]") {
    // Written only when some type has one, so a sink that states none writes what it always did.
    ac::Support support = board_support();
    CHECK(written(support, ac::write_support).find("max_frame_rate") == std::string::npos);

    support.max_frame_rate[static_cast<std::size_t>(ac::DataType::kAc4)] = 25;
    const std::string text = written(support, ac::write_support);
    CHECK(text.ends_with(R"("buffer_capacity":1048576,"max_frame_rate":{"ac4":25}})"));
    const std::optional<ac::Support> read = ac::read_support(Parsed(text).root());
    REQUIRE(read.has_value());
    CHECK(read->max_frame_rate_of(ac::DataType::kAc4) == 25);
    // A type the object does not name states none: 0, which a server reads as no limit.
    CHECK(read->max_frame_rate_of(ac::DataType::kAc3) == 0);
    CHECK(read->max_frame_rate_of(ac::DataType::kEac3) == 0);

    // A sink from before the key sends none, and every type reads as stating none.
    const std::optional<ac::Support> before =
        ac::read_support(Parsed(written(board_support(), ac::write_support)).root());
    REQUIRE(before.has_value());
    for (const ac::DataType type : {ac::DataType::kAc3, ac::DataType::kEac3, ac::DataType::kAc4}) {
        CHECK(before->max_frame_rate_of(type) == 0);
    }

    const auto with_limits = [&](const std::string& value) {
        std::string source = written(board_support(), ac::write_support);
        source.pop_back();  // the closing brace
        return ac::read_support(Parsed(source + R"(,"max_frame_rate":)" + value + "}").root());
    };
    // A name this reader does not know is skipped, as an unknown key is.
    const std::optional<ac::Support> future = with_limits(R"({"ac4":30,"dts":60})");
    REQUIRE(future.has_value());
    CHECK(future->max_frame_rate_of(ac::DataType::kAc4) == 30);
    CHECK(with_limits("{}").has_value());
    CHECK(with_limits(R"({"ac4":1000})").has_value());
    // A rate that is not a rate, or a limit that is not an object, is refused with the object.
    CHECK_FALSE(with_limits(R"({"ac4":0})"));
    CHECK_FALSE(with_limits(R"({"ac4":1001})"));
    CHECK_FALSE(with_limits(R"({"ac4":-25})"));
    CHECK_FALSE(with_limits(R"({"ac4":29.97})"));
    CHECK_FALSE(with_limits(R"({"ac4":"25"})"));
    CHECK_FALSE(with_limits("25"));
    CHECK_FALSE(with_limits("[25]"));
    CHECK_FALSE(with_limits("null"));
}

TEST_CASE("iclforge_player: the layout a sink states", "[sendspin][iclforge]") {
    ac::State state;
    state.supported_commands = {ac::Command::kVolume};
    CHECK(written(state, ac::write_state).find("\"layout\"") == std::string::npos);

    state.layout = "5.1";
    const std::string text = written(state, ac::write_state);
    CHECK(text.ends_with(
        R"("counters":{"bursts_played":0,"underruns":0,"late_chunks":0,"dropped_chunks":0,"invalid_chunks":0},"layout":"5.1"})"));
    const std::optional<ac::State> read = ac::read_state(Parsed(text).root());
    REQUIRE(read.has_value());
    CHECK(read->layout == "5.1");

    // A sink that does not say has none, which is not the same as an empty layout.
    state.layout.reset();
    const std::optional<ac::State> silent =
        ac::read_state(Parsed(written(state, ac::write_state)).root());
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->layout.has_value());

    const auto with_layout = [&](const std::string& value) {
        std::string source = written(state, ac::write_state);
        source.pop_back();
        return ac::read_state(Parsed(source + R"(,"layout":)" + value + "}").root());
    };
    CHECK(with_layout(R"("L,R,C,LFE,Ls,Rs")").has_value());
    CHECK(with_layout(std::string("\"") + std::string(512, 'x') + "\"").has_value());
    CHECK_FALSE(with_layout(std::string("\"") + std::string(513, 'x') + "\""));
    CHECK_FALSE(with_layout("7"));
    CHECK_FALSE(with_layout("null"));
}

TEST_CASE("iclforge_player: how a sink's stream leaves it", "[sendspin][iclforge]") {
    ac::State state;
    state.supported_commands = {ac::Command::kVolume};
    // A sink from before the key, or one with no stream, says nothing: the key is not written.
    CHECK(written(state, ac::write_state).find("\"output\"") == std::string::npos);

    const auto round_trip = [&](ac::Output output, std::string_view name) {
        state.output = output;
        const std::string text = written(state, ac::write_state);
        CHECK(text.ends_with(R"(,"output":")" + std::string(name) + R"("})"));
        const std::optional<ac::State> read = ac::read_state(Parsed(text).root());
        REQUIRE(read.has_value());
        REQUIRE(read->output.has_value());
        CHECK(*read->output == output);
        CHECK(ac::output_name(output) == name);
    };
    round_trip(ac::Output::kPassthrough, "passthrough");
    round_trip(ac::Output::kPcm, "pcm");

    // The key sits beside the others and does not disturb them.
    state.layout = "5.1";
    state.lyrics = ac::LyricsReport{.item = 4, .line = 2};
    state.output = ac::Output::kPassthrough;
    const std::optional<ac::State> both = ac::read_state(Parsed(written(state, ac::write_state)).root());
    REQUIRE(both.has_value());
    CHECK(both->layout == "5.1");
    CHECK(both->lyrics == ac::LyricsReport{.item = 4, .line = 2});
    CHECK(both->output == ac::Output::kPassthrough);

    // Absent reads as "not said", which is neither value.
    state.output.reset();
    const std::optional<ac::State> silent =
        ac::read_state(Parsed(written(state, ac::write_state)).root());
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->output.has_value());

    const auto with_output = [&](const std::string& value) {
        ac::State quiet;
        quiet.supported_commands = {ac::Command::kVolume};
        std::string source = written(quiet, ac::write_state);
        source.pop_back();
        return ac::read_state(Parsed(source + R"(,"output":)" + value + "}").root());
    };
    const std::optional<ac::State> named = with_output(R"("passthrough")");
    REQUIRE(named.has_value());
    CHECK(named->output == ac::Output::kPassthrough);
    // A value a later revision adds is read as the sink not saying, and the state is still read.
    const std::optional<ac::State> later = with_output(R"("decoded")");
    REQUIRE(later.has_value());
    CHECK_FALSE(later->output.has_value());
    CHECK(later->supported_commands.size() == 1);
    // A value of the wrong type is a malformed state, as for every other key.
    CHECK_FALSE(with_output("7"));
    CHECK_FALSE(with_output("null"));
    CHECK_FALSE(with_output("true"));
}

TEST_CASE("iclforge_player: the stream/start object", "[sendspin][iclforge]") {
    const std::string text =
        written(ac::StreamStart{.data_type = ac::DataType::kEac3, .sample_rate = 48000}, ac::write_stream_start);
    CHECK(text == R"({"data_type":"eac3","sample_rate":48000})");
    const Parsed parsed(text);
    const std::optional<ac::StreamStart> read = ac::read_stream_start(parsed.root());
    REQUIRE(read.has_value());
    CHECK(read->data_type == ac::DataType::kEac3);
    CHECK(read->sample_rate == 48000);

    CHECK(ac::read_stream_start(Parsed(R"({"data_type":"ac3","sample_rate":44100,"future":true})").root()));
    const std::optional<ac::StreamStart> ac4 =
        ac::read_stream_start(Parsed(R"({"data_type":"ac4","sample_rate":48000})").root());
    REQUIRE(ac4.has_value());
    CHECK(ac4->data_type == ac::DataType::kAc4);
    CHECK_FALSE(
        ac::read_stream_start(Parsed(R"({"data_type":"mpegh","sample_rate":48000})").root()));
    CHECK_FALSE(ac::read_stream_start(Parsed(R"({"data_type":"eac3"})").root()));
    CHECK_FALSE(ac::read_stream_start(Parsed(R"({"sample_rate":48000})").root()));
}

TEST_CASE("iclforge_player: the player@v1 commands and identify", "[sendspin][iclforge]") {
    const auto round_trip = [](const ac::CommandMessage& command, std::string_view expected) {
        const std::string text = written(command, ac::write_command);
        CHECK(text == expected);
        const Parsed parsed(text);
        auto read = ac::read_command(parsed.root());
        REQUIRE(read.has_value());
        CHECK(read->command == command.command);
        return *read;
    };
    ac::CommandMessage volume;
    volume.command = ac::Command::kVolume;
    volume.volume = 40;
    CHECK(round_trip(volume, R"({"command":"volume","volume":40})").volume == 40);

    ac::CommandMessage mute;
    mute.command = ac::Command::kMute;
    mute.mute = true;
    CHECK(round_trip(mute, R"({"command":"mute","mute":true})").mute);

    ac::CommandMessage delay;
    delay.command = ac::Command::kSetOutputDelay;
    delay.output_delay_ms = 120;
    CHECK(round_trip(delay, R"({"command":"set_output_delay","output_delay_ms":120})").output_delay_ms == 120);

    ac::CommandMessage identify;
    identify.command = ac::Command::kIdentify;
    identify.identify = ac::Identify{.output = 3, .level_db = -30.0};
    const ac::CommandMessage tone = round_trip(identify, R"({"command":"identify","identify":{"output":3,"level_db":-30}})");
    REQUIRE(tone.identify.has_value());
    CHECK(tone.identify->output == 3);
    CHECK(tone.identify->level_db == -30.0);

    identify.identify.reset();
    CHECK_FALSE(round_trip(identify, R"({"command":"identify","identify":null})").identify.has_value());

    const auto failure = [](std::string text) {
        const auto read = ac::read_command(Parsed(std::move(text)).root());
        REQUIRE_FALSE(read.has_value());
        return read.error().error;
    };
    CHECK(failure(R"({"command":"louder"})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"volume","volume":101})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"mute"})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"set_output_delay","output_delay_ms":5001})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"identify"})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"identify","identify":{"output":0,"level_db":-6}})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"identify","identify":{"output":0,"level_db":-61}})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"identify","identify":{"level_db":-30}})") == ac::CommandError::kMalformed);
}

TEST_CASE("iclforge_player: settings", "[sendspin][iclforge]") {
    const std::string text = written(full_settings(), ac::write_command);
    CHECK(text == R"({"command":"settings","settings":{"revision":7,"layout":"L:small,C,R:small,Ls,Rs,LFE",)"
                  R"("routing":"0,1,2,3,4,5","trim_db":[0,-1.5,0,-3,-3,2],"delay_ms":[0,0,0,12.5,12.5,0],"crossover_hz":80,)"
                  R"("decoder":{"mode":"rf","drc_cut":0.25,"drc_boost":1,"heavy_compression":false,"dialnorm":true,)"
                  R"("downmix":"ltrt","ltrt_phase_shift":true,"mix_lfe":false,"programme":null,"objects":"never",)"
                  R"("concealment":"repeat_fade","karaoke":"capable","karaoke_vocals":"v2",)"
                  R"("karaoke_melody_db":-6,"karaoke_vocal1_db":3,"karaoke_vocal2_db":-24}}})");

    const Parsed parsed(text);
    const auto read = ac::read_command(parsed.root());
    REQUIRE(read.has_value());
    const ac::Settings& settings = read->settings;
    CHECK(settings.revision == 7);
    CHECK(settings.layout == "L:small,C,R:small,Ls,Rs,LFE");
    CHECK(settings.routing == "0,1,2,3,4,5");
    CHECK(settings.trim_db == full_settings().settings.trim_db);
    CHECK(settings.delay_ms == full_settings().settings.delay_ms);
    CHECK(settings.crossover_hz == 80.0);
    CHECK(settings.decoder.mode == ac::DecoderMode::kRf);
    CHECK(settings.decoder.drc_cut == 0.25);
    CHECK(settings.decoder.drc_boost == 1.0);
    CHECK(settings.decoder.heavy_compression == false);
    CHECK(settings.decoder.dialnorm == true);
    CHECK(settings.decoder.downmix == ac::Downmix::kLtRt);
    CHECK(settings.decoder.ltrt_phase_shift == true);
    CHECK(settings.decoder.mix_lfe == false);
    CHECK(settings.decoder.has_programme);
    CHECK_FALSE(settings.decoder.programme.has_value());
    CHECK(settings.decoder.objects == ac::ObjectsPolicy::kNever);
    CHECK(settings.decoder.concealment == ac::Concealment::kRepeatFade);
    CHECK(settings.decoder.karaoke == ac::Karaoke::kCapable);
    CHECK(settings.decoder.karaoke_vocals == ac::KaraokeVocals::kV2);
    CHECK(settings.decoder.karaoke_melody_db == -6.0);
    CHECK(settings.decoder.karaoke_vocal1_db == 3.0);
    CHECK(settings.decoder.karaoke_vocal2_db == -24.0);

    const auto read_settings = [](const std::string& object) {
        return ac::read_command(Parsed(R"({"command":"settings","settings":)" + object + "}").root());
    };
    // Absent keys stay absent, and unknown keys are ignored.
    const auto sparse = read_settings(R"({"revision":2,"future":"x","decoder":{"programme":1,"future":1}})");
    REQUIRE(sparse.has_value());
    CHECK_FALSE(sparse->settings.layout.has_value());
    CHECK_FALSE(sparse->settings.trim_db.has_value());
    CHECK_FALSE(sparse->settings.decoder.mode.has_value());
    // A sink is told nothing about karaoke unless it is told, and then takes its own default.
    CHECK_FALSE(sparse->settings.decoder.karaoke.has_value());
    CHECK_FALSE(sparse->settings.decoder.karaoke_vocals.has_value());
    CHECK_FALSE(sparse->settings.decoder.karaoke_melody_db.has_value());
    CHECK_FALSE(sparse->settings.decoder.karaoke_vocal1_db.has_value());
    CHECK_FALSE(sparse->settings.decoder.karaoke_vocal2_db.has_value());
    CHECK(sparse->settings.decoder.has_programme);
    CHECK(sparse->settings.decoder.programme == 1);
    const auto bare = read_settings(R"({"revision":3})");
    REQUIRE(bare.has_value());
    CHECK_FALSE(bare->settings.decoder.has_programme);

    const auto refused = [&](const std::string& object) {
        const auto result = read_settings(object);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == ac::CommandError::kSettingsRefused);
        CHECK_FALSE(result.error().settings.why.empty());
        return result.error().settings.revision;
    };
    CHECK(refused(R"({"revision":9,"decoder":{"drc_cut":1.5}})") == 9);
    CHECK(refused(R"({"revision":10,"decoder":{"mode":"loud"}})") == 10);
    CHECK(refused(R"({"revision":11,"decoder":{"programme":"first"}})") == 11);
    CHECK(refused(R"({"revision":12,"decoder":{"dialnorm":1}})") == 12);
    CHECK(refused(R"({"revision":13,"delay_ms":[0,-1]})") == 13);
    CHECK(refused(R"({"revision":14,"trim_db":"flat"})") == 14);
    CHECK(refused(R"({"revision":15,"crossover_hz":0})") == 15);
    CHECK(refused(R"({"revision":16,"layout":51})") == 16);
    CHECK(refused(R"({"revision":17,"decoder":[]})") == 17);
    CHECK(refused(R"({"revision":18,"decoder":{"objects":"sometimes"}})") == 18);
    CHECK(refused(R"({"revision":22,"decoder":{"karaoke":"on"}})") == 22);
    CHECK(refused(R"({"revision":23,"decoder":{"karaoke":true}})") == 23);
    CHECK(refused(R"({"revision":24,"decoder":{"karaoke_vocals":"all"}})") == 24);
    CHECK(refused(R"({"revision":25,"decoder":{"karaoke":"aware","karaoke_vocals":1}})") == 25);
    // The levels are numbers from -24 to 12 dB, and each of the three is held to it.
    CHECK(refused(R"({"revision":26,"decoder":{"karaoke_melody_db":12.5}})") == 26);
    CHECK(refused(R"({"revision":27,"decoder":{"karaoke_vocal1_db":-24.5}})") == 27);
    CHECK(refused(R"({"revision":28,"decoder":{"karaoke_vocal2_db":"loud"}})") == 28);
    CHECK(refused(R"({"revision":29,"decoder":{"karaoke_melody_db":null}})") == 29);
    const auto edges = read_settings(
        R"({"revision":30,"decoder":{"karaoke_melody_db":12,"karaoke_vocal1_db":-24,"karaoke_vocal2_db":0}})");
    REQUIRE(edges.has_value());
    CHECK(edges->settings.decoder.karaoke_melody_db == 12.0);
    CHECK(edges->settings.decoder.karaoke_vocal1_db == -24.0);
    CHECK(edges->settings.decoder.karaoke_vocal2_db == 0.0);
    // A number the writer could not write back, and a crossover that its tenth of a hertz rounds to
    // nothing (libs/sendspin/fuzz/regressions/fuzz_sendspin_messages).
    CHECK(refused(R"({"revision":19,"trim_db":[7.7e62,0]})") == 19);
    CHECK(refused(R"({"revision":20,"crossover_hz":0.04})") == 20);
    CHECK(read_settings(R"({"revision":21,"crossover_hz":0.05})").has_value());

    const auto malformed = read_settings(R"({"layout":"5.1"})");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().error == ac::CommandError::kMalformed);
    CHECK_FALSE(ac::read_command(Parsed(R"({"command":"settings"})").root()).has_value());
}

TEST_CASE("iclforge_player: settings checked against the support object", "[sendspin][iclforge]") {
    const ac::Support support = board_support();
    CHECK_FALSE(ac::check_settings(full_settings().settings, support).has_value());
    CHECK_FALSE(ac::check_settings(ac::Settings{}, support).has_value());

    const auto why = [&](void (*change)(ac::Settings&), ac::Support with = board_support()) {
        ac::Settings settings = full_settings().settings;
        change(settings);
        return ac::check_settings(settings, with).value_or("");
    };
    CHECK(why([](ac::Settings& s) { s.trim_db->pop_back(); }) == "trim_db needs one value per output");
    CHECK(why([](ac::Settings& s) { (*s.trim_db)[0] = 12.5; }) == "a trim_db value is outside management.trim_db");
    CHECK(why([](ac::Settings& s) { s.delay_ms->push_back(0.0); }) == "delay_ms needs one value per output");
    CHECK(why([](ac::Settings& s) { (*s.delay_ms)[5] = 50.5; }) == "a delay_ms value is outside management.delay_ms");
    CHECK(why([](ac::Settings& s) { s.crossover_hz = 30.0; }) == "crossover_hz is outside management.crossover_hz");

    ac::Support fixed = board_support();
    fixed.management.routing = false;
    CHECK(why([](ac::Settings&) {}, fixed) == "this sink does not take routing");
    CHECK(why([](ac::Settings& s) { s.routing.reset(); }, fixed).empty());

    ac::Support simple = board_support();
    simple.decoder_settings = {"mode", "drc_cut", "drc_boost", "heavy_compression", "dialnorm", "downmix",
                               "ltrt_phase_shift", "mix_lfe", "objects", "concealment", "karaoke",
                               "karaoke_vocals", "karaoke_melody_db", "karaoke_vocal1_db",
                               "karaoke_vocal2_db"};
    CHECK(why([](ac::Settings&) {}, simple) == "decoder.programme is not a setting this sink takes");
    CHECK(why([](ac::Settings& s) { s.decoder.has_programme = false; }, simple).empty());

    // A sink from before karaoke lists the other keys and not these: asking it for the
    // reproduction is refused by name, which is how a server learns to decode for it instead.
    ac::Support before_karaoke = board_support();
    before_karaoke.decoder_settings.erase(
        std::remove(before_karaoke.decoder_settings.begin(), before_karaoke.decoder_settings.end(), "karaoke"),
        before_karaoke.decoder_settings.end());
    CHECK(why([](ac::Settings&) {}, before_karaoke) == "decoder.karaoke is not a setting this sink takes");
    before_karaoke.decoder_settings.erase(std::remove(before_karaoke.decoder_settings.begin(),
                                                      before_karaoke.decoder_settings.end(), "karaoke_vocals"),
                                          before_karaoke.decoder_settings.end());
    CHECK(why([](ac::Settings& s) { s.decoder.karaoke.reset(); }, before_karaoke) ==
          "decoder.karaoke_vocals is not a setting this sink takes");
    CHECK(why([](ac::Settings& s) {
               s.decoder.karaoke.reset();
               s.decoder.karaoke_vocals.reset();
           },
              before_karaoke)
              .empty());

    // A sink that has the reproduction and not the levels (a board from before them) is refused
    // each level by name, which is how a server learns to decode for it when one is moved.
    ac::Support before_levels = board_support();
    for (const std::string_view key : {"karaoke_melody_db", "karaoke_vocal1_db", "karaoke_vocal2_db"}) {
        before_levels.decoder_settings.erase(std::remove(before_levels.decoder_settings.begin(),
                                                         before_levels.decoder_settings.end(), key),
                                             before_levels.decoder_settings.end());
    }
    CHECK(why([](ac::Settings&) {}, before_levels) ==
          "decoder.karaoke_melody_db is not a setting this sink takes");
    CHECK(why([](ac::Settings& s) { s.decoder.karaoke_melody_db.reset(); }, before_levels) ==
          "decoder.karaoke_vocal1_db is not a setting this sink takes");
    CHECK(why([](ac::Settings& s) { s.decoder.karaoke_vocal1_db.reset(); s.decoder.karaoke_melody_db.reset(); },
              before_levels) == "decoder.karaoke_vocal2_db is not a setting this sink takes");
    CHECK(why([](ac::Settings& s) {
               s.decoder.karaoke_melody_db.reset();
               s.decoder.karaoke_vocal1_db.reset();
               s.decoder.karaoke_vocal2_db.reset();
           },
              before_levels)
              .empty());
}

TEST_CASE("iclforge_player: the lyric document, as a command", "[sendspin][iclforge][lyrics]") {
    ac::CommandMessage document;
    document.command = ac::Command::kLyrics;
    document.lyrics = ac::LyricsDocument{.item = 7, .text = "[00:01.00]Should auld\n[00:03.50]\"Be\" forgot\\\n"};
    const std::string text = written(document, ac::write_command);
    CHECK(text ==
          R"({"command":"lyrics","lyrics":{"item":7,"text":"[00:01.00]Should auld\n[00:03.50]\"Be\" forgot\\\n"}})");
    const Parsed parsed(text);
    const auto read = ac::read_command(parsed.root());
    REQUIRE(read.has_value());
    CHECK(read->command == ac::Command::kLyrics);
    CHECK(read->lyrics.item == 7);
    CHECK(read->lyrics.text == document.lyrics.text);

    // An empty text takes the item's lyrics away, and is a document like any other.
    document.lyrics.text.clear();
    const auto cleared = ac::read_command(Parsed(written(document, ac::write_command)).root());
    REQUIRE(cleared.has_value());
    CHECK(cleared->lyrics.text.empty());

    // The text fits one message, or the command is refused.
    document.lyrics.text.assign(ac::kMaxLyricsBytes, 'x');
    CHECK(ac::read_command(Parsed(written(document, ac::write_command)).root()).has_value());
    document.lyrics.text.assign(ac::kMaxLyricsBytes + 1, 'x');
    CHECK_FALSE(ac::read_command(Parsed(written(document, ac::write_command)).root()).has_value());

    const auto refused = [](std::string source) {
        return !ac::read_command(Parsed(std::move(source)).root()).has_value();
    };
    CHECK(refused(R"({"command":"lyrics"})"));
    CHECK(refused(R"({"command":"lyrics","lyrics":{"text":"x"}})"));
    CHECK(refused(R"({"command":"lyrics","lyrics":{"item":-1,"text":"x"}})"));
    CHECK(refused(R"({"command":"lyrics","lyrics":{"item":1}})"));
    CHECK(refused(R"({"command":"lyrics","lyrics":{"item":1,"text":4}})"));
}

TEST_CASE("iclforge_player: where the sound is in the lyrics", "[sendspin][iclforge][lyrics]") {
    ac::CommandMessage position;
    position.command = ac::Command::kLyricsPosition;
    position.lyrics_position =
        ac::LyricsAnchor{.item = 7, .position_ms = 12500, .at_us = 9000000123, .speed = 1.0};
    const std::string text = written(position, ac::write_command);
    CHECK(text == R"({"command":"lyrics_position","lyrics_position":{"item":7,"position_ms":12500,)"
                  R"("at_us":9000000123,"speed":1}})");
    const auto read = ac::read_command(Parsed(text).root());
    REQUIRE(read.has_value());
    CHECK(read->command == ac::Command::kLyricsPosition);
    CHECK(read->lyrics_position.item == 7);
    CHECK(read->lyrics_position.position_ms == 12500);
    CHECK(read->lyrics_position.at_us == 9000000123);
    CHECK(read->lyrics_position.speed == 1.0);

    // Held, as at a pause.
    position.lyrics_position.speed = 0.0;
    const auto held = ac::read_command(Parsed(written(position, ac::write_command)).root());
    REQUIRE(held.has_value());
    CHECK(held->lyrics_position.speed == 0.0);

    const auto refused = [](std::string source) {
        return !ac::read_command(Parsed(std::move(source)).root()).has_value();
    };
    const std::string good = R"({"command":"lyrics_position","lyrics_position":{"item":7,"position_ms":0,)"
                             R"("at_us":1,"speed":1}})";
    CHECK_FALSE(refused(good));
    CHECK(refused(R"({"command":"lyrics_position"})"));
    CHECK(refused(R"({"command":"lyrics_position","lyrics_position":{"item":7,"position_ms":-1,"at_us":1,"speed":1}})"));
    CHECK(refused(R"({"command":"lyrics_position","lyrics_position":{"item":7,"position_ms":0,"at_us":-1,"speed":1}})"));
    CHECK(refused(R"({"command":"lyrics_position","lyrics_position":{"item":-1,"position_ms":0,"at_us":1,"speed":1}})"));
    CHECK(refused(R"({"command":"lyrics_position","lyrics_position":{"item":7,"position_ms":0,"at_us":1,"speed":-0.5}})"));
    CHECK(refused(R"({"command":"lyrics_position","lyrics_position":{"item":7,"position_ms":0,"at_us":1,"speed":4.5}})"));
    CHECK(refused(R"({"command":"lyrics_position","lyrics_position":{"item":7,"position_ms":4233600001,"at_us":1,"speed":1}})"));
    CHECK(refused(R"({"command":"lyrics_position","lyrics_position":{"item":7,"position_ms":0,"at_us":1}})"));
    CHECK(refused(R"({"command":"lyrics_position","lyrics_position":{"position_ms":0,"at_us":1,"speed":1}})"));
}

TEST_CASE("iclforge_player: the codecs a sink reproduces karaoke for", "[sendspin][iclforge][karaoke]") {
    // Written only when the sink lists some, so a sink that states none writes what it always did.
    ac::Support support = board_support();
    CHECK(written(support, ac::write_support).find("karaoke_codecs") == std::string::npos);

    // A sink from before the key reproduces AC-3's karaoke and nothing else.
    const std::optional<ac::Support> before =
        ac::read_support(Parsed(written(support, ac::write_support)).root());
    REQUIRE(before.has_value());
    CHECK(before->karaoke_codecs.empty());
    CHECK(before->reproduces_karaoke(ac::DataType::kAc3));
    CHECK_FALSE(before->reproduces_karaoke(ac::DataType::kEac3));
    CHECK_FALSE(before->reproduces_karaoke(ac::DataType::kAc4));

    // A sink that lists E-AC-3 states it, and each of the types it lists reads back.
    support.karaoke_codecs = {ac::DataType::kAc3, ac::DataType::kEac3};
    const std::string text = written(support, ac::write_support);
    CHECK(text.ends_with(R"("buffer_capacity":1048576,"karaoke_codecs":["ac3","eac3"]})"));
    const std::optional<ac::Support> read = ac::read_support(Parsed(text).root());
    REQUIRE(read.has_value());
    CHECK(read->reproduces_karaoke(ac::DataType::kAc3));
    CHECK(read->reproduces_karaoke(ac::DataType::kEac3));
    CHECK_FALSE(read->reproduces_karaoke(ac::DataType::kAc4));

    // A list that names E-AC-3 alone is what it says: AC-3's is not assumed once a sink lists.
    support.karaoke_codecs = {ac::DataType::kEac3};
    const std::optional<ac::Support> only =
        ac::read_support(Parsed(written(support, ac::write_support)).root());
    REQUIRE(only.has_value());
    CHECK_FALSE(only->reproduces_karaoke(ac::DataType::kAc3));
    CHECK(only->reproduces_karaoke(ac::DataType::kEac3));

    const auto with_codecs = [&](const std::string& value) {
        std::string source = written(board_support(), ac::write_support);
        source.pop_back();  // the closing brace
        return ac::read_support(Parsed(source + R"(,"karaoke_codecs":)" + value + "}").root());
    };
    // A name this reader does not know is skipped, as one in data_types is.
    const std::optional<ac::Support> future = with_codecs(R"(["eac3","dts"])");
    REQUIRE(future.has_value());
    CHECK(future->karaoke_codecs == std::vector<ac::DataType>{ac::DataType::kEac3});
    CHECK(with_codecs("[]").has_value());
    // An element that is not a string, or a list that is not an array, is refused with the object.
    CHECK_FALSE(with_codecs(R"(["eac3",3])"));
    CHECK_FALSE(with_codecs(R"("eac3")"));
    CHECK_FALSE(with_codecs("null"));
}

TEST_CASE("iclforge_player: what a sink says of the lyrics it holds", "[sendspin][iclforge][lyrics]") {
    // The size the sink holds, in the support object, after the limits the other keys state.
    ac::Support support = board_support();
    CHECK(written(support, ac::write_support).find("\"lyrics\"") == std::string::npos);
    support.lyrics_max_bytes = 16384;
    const std::string text = written(support, ac::write_support);
    CHECK(text.find(R"("lyrics":{"max_bytes":16384})") != std::string::npos);
    const auto read = ac::read_support(Parsed(text).root());
    REQUIRE(read.has_value());
    CHECK(read->lyrics_max_bytes == 16384);
    // A sink from before the key has none, which a server reads as "takes no lyrics".
    const auto silent = ac::read_support(Parsed(written(board_support(), ac::write_support)).root());
    REQUIRE(silent.has_value());
    CHECK(silent->lyrics_max_bytes == 0);

    const auto with_lyrics = [&](const std::string& value) {
        std::string source = written(board_support(), ac::write_support);
        source.pop_back();
        return ac::read_support(Parsed(source + R"(,"lyrics":)" + value + "}").root());
    };
    CHECK(with_lyrics(R"({"max_bytes":1})").has_value());
    CHECK(with_lyrics(R"({"max_bytes":60000})").has_value());
    CHECK_FALSE(with_lyrics(R"({"max_bytes":0})"));
    CHECK_FALSE(with_lyrics(R"({"max_bytes":60001})"));
    CHECK_FALSE(with_lyrics(R"({})"));
    CHECK_FALSE(with_lyrics("7"));

    // The state: the item and the line, absent while the sink holds none.
    ac::State state;
    state.supported_commands = {ac::Command::kLyrics, ac::Command::kLyricsPosition};
    CHECK(written(state, ac::write_state).find(R"("lyrics":{)") == std::string::npos);
    CHECK(written(state, ac::write_state).find(R"("supported_commands":["lyrics","lyrics_position"])") !=
          std::string::npos);
    state.lyrics = ac::LyricsReport{.item = 7, .line = 3};
    const std::string reported = written(state, ac::write_state);
    CHECK(reported.ends_with(R"(,"lyrics":{"item":7,"line":3}})"));
    const auto back = ac::read_state(Parsed(reported).root());
    REQUIRE(back.has_value());
    REQUIRE(back->lyrics.has_value());
    CHECK(*back->lyrics == ac::LyricsReport{.item = 7, .line = 3});
    CHECK(back->supported_commands.size() == 2);

    // Before the first line is -1, and nothing below that is a line.
    state.lyrics = ac::LyricsReport{.item = 7, .line = -1};
    const auto before = ac::read_state(Parsed(written(state, ac::write_state)).root());
    REQUIRE(before.has_value());
    CHECK(before->lyrics->line == -1);
    const auto with_state_lyrics = [&](const std::string& value) {
        ac::State quiet;
        quiet.supported_commands = {ac::Command::kLyrics};
        std::string source = written(quiet, ac::write_state);
        source.pop_back();
        return ac::read_state(Parsed(source + R"(,"lyrics":)" + value + "}").root());
    };
    CHECK(with_state_lyrics(R"({"item":0,"line":0})").has_value());
    CHECK_FALSE(with_state_lyrics(R"({"item":-1,"line":0})"));
    CHECK_FALSE(with_state_lyrics(R"({"item":1,"line":-2})"));
    CHECK_FALSE(with_state_lyrics(R"({"item":1})"));
}

TEST_CASE("iclforge_player: a sink's request for the karaoke choice", "[sendspin][iclforge][request]") {
    ac::Request request;
    request.decoder.karaoke = ac::Karaoke::kCapable;
    request.decoder.karaoke_vocals = ac::KaraokeVocals::kV2;
    request.decoder.karaoke_melody_db = -6.0;
    request.decoder.karaoke_vocal1_db = 0.0;
    request.decoder.karaoke_vocal2_db = 12.0;
    const std::string text = written(request, ac::write_request);
    CHECK(text ==
          R"({"command":"decoder","decoder":{"karaoke":"capable","karaoke_vocals":"v2","karaoke_melody_db":-6,)"
          R"("karaoke_vocal1_db":0,"karaoke_vocal2_db":12}})");
    const Parsed parsed(text);
    const std::optional<ac::Request> read = ac::read_request(parsed.root());
    REQUIRE(read.has_value());
    CHECK(read->decoder.karaoke == ac::Karaoke::kCapable);
    CHECK(read->decoder.karaoke_vocals == ac::KaraokeVocals::kV2);
    CHECK(read->decoder.karaoke_melody_db == -6.0);
    CHECK(read->decoder.karaoke_vocal1_db == 0.0);
    CHECK(read->decoder.karaoke_vocal2_db == 12.0);

    // Any one key is a request, and only the keys present are written.
    ac::Request vocals;
    vocals.decoder.karaoke_vocals = ac::KaraokeVocals::kNone;
    CHECK(written(vocals, ac::write_request) == R"({"command":"decoder","decoder":{"karaoke_vocals":"none"}})");

    const auto read_text = [](const std::string& source) { return ac::read_request(Parsed(source).root()); };
    // A key that is not requestable is ignored as every unknown key is; alone it is no request.
    const auto mixed = read_text(R"({"command":"decoder","decoder":{"karaoke_vocals":"v1","drc_cut":0.5,"x":1}})");
    REQUIRE(mixed.has_value());
    CHECK(mixed->decoder.karaoke_vocals == ac::KaraokeVocals::kV1);
    CHECK_FALSE(mixed->decoder.drc_cut.has_value());
    CHECK_FALSE(read_text(R"({"command":"decoder","decoder":{"drc_cut":0.5}})"));
    // Refused whole: the wrong command, no decoder object, nothing in it, a name or a level out of range.
    CHECK_FALSE(read_text(R"({"command":"settings","decoder":{"karaoke_vocals":"v1"}})"));
    CHECK_FALSE(read_text(R"({"decoder":{"karaoke_vocals":"v1"}})"));
    CHECK_FALSE(read_text(R"({"command":"decoder"})"));
    CHECK_FALSE(read_text(R"({"command":"decoder","decoder":[]})"));
    CHECK_FALSE(read_text(R"({"command":"decoder","decoder":{}})"));
    CHECK_FALSE(read_text(R"({"command":"decoder","decoder":{"karaoke":"always"}})"));
    CHECK_FALSE(read_text(R"({"command":"decoder","decoder":{"karaoke_vocals":"v3"}})"));
    CHECK_FALSE(read_text(R"({"command":"decoder","decoder":{"karaoke_melody_db":-24.1}})"));
    CHECK_FALSE(read_text(R"({"command":"decoder","decoder":{"karaoke_vocal2_db":12.1}})"));
    CHECK_FALSE(read_text(R"({"command":"decoder","decoder":{"karaoke_vocal1_db":"loud"}})"));
    // The ends are in range, and one wrong key spoils the others that are right.
    CHECK(read_text(R"({"command":"decoder","decoder":{"karaoke_melody_db":-24.0,"karaoke_vocal1_db":12.0}})"));
    CHECK_FALSE(read_text(R"({"command":"decoder","decoder":{"karaoke_vocals":"v1","karaoke_vocal1_db":99}})"));
}

TEST_CASE("iclforge_player: a server acts on the keys a sink listed as requestable", "[sendspin][iclforge][request]") {
    ac::Request request;
    request.decoder.karaoke = ac::Karaoke::kAware;
    request.decoder.karaoke_vocals = ac::KaraokeVocals::kV1;
    request.decoder.karaoke_vocal2_db = -3.0;

    ac::Support support = board_support();
    // A sink from before the key lists none, and nothing is acted on.
    CHECK_FALSE(ac::accept_request(request, support).has_value());

    support.requests = {"karaoke_vocals", "karaoke_vocal1_db"};
    const std::optional<ac::Request> cut = ac::accept_request(request, support);
    REQUIRE(cut.has_value());
    CHECK(cut->decoder.karaoke_vocals == ac::KaraokeVocals::kV1);
    CHECK_FALSE(cut->decoder.karaoke.has_value());
    CHECK_FALSE(cut->decoder.karaoke_vocal2_db.has_value());

    // None of its keys listed: nothing left.
    support.requests = {"karaoke_vocal1_db"};
    CHECK_FALSE(ac::accept_request(request, support).has_value());

    // The support object: written only when it lists some, read with names that are not requestable
    // skipped and a repeat collapsed, and refused for a value that is not a list of strings.
    ac::Support listing = board_support();
    CHECK(written(listing, ac::write_support).find("requests") == std::string::npos);
    listing.requests = {"karaoke", "karaoke_vocals"};
    const std::string text = written(listing, ac::write_support);
    CHECK(text.find(R"("requests":["karaoke","karaoke_vocals"])") != std::string::npos);
    const auto back = ac::read_support(Parsed(text).root());
    REQUIRE(back.has_value());
    CHECK(back->requests == listing.requests);
    const auto silent = ac::read_support(Parsed(written(board_support(), ac::write_support)).root());
    REQUIRE(silent.has_value());
    CHECK(silent->requests.empty());

    const auto with_requests = [&](const std::string& value) {
        std::string source = written(board_support(), ac::write_support);
        source.pop_back();
        return ac::read_support(Parsed(source + R"(,"requests":)" + value + "}").root());
    };
    const auto skipped = with_requests(R"(["karaoke_vocals","drc_cut","karaoke_vocals","future"])");
    REQUIRE(skipped.has_value());
    CHECK(skipped->requests == std::vector<std::string>{"karaoke_vocals"});
    CHECK_FALSE(with_requests("7"));
    CHECK_FALSE(with_requests("[1]"));
    CHECK_FALSE(with_requests("null"));
}

TEST_CASE("iclforge_player: a sink lists TrueHD and states its limits", "[sendspin][iclforge][truehd]") {
    const auto read_text = [](std::string source) { return ac::read_support(Parsed(std::move(source)).root()); };
    const std::string outputs = R"("outputs":{"count":2,"bit_depth":16,"bit_depths":[16]},"layout_grammar":1,)";
    const std::string management =
        R"("management":{"routing":false,"trim_db":[0,0],"delay_ms":[0,0],"crossover_hz":[80,80],"identify":false},)";
    const std::string tail = R"("decoder_settings":[],"buffer_capacity":262144})";

    const std::optional<ac::Support> support = read_text(
        R"({"data_types":["ac3","eac3","truehd"],"sample_rates":[48000,96000],)"
        R"("max_coded_channels":{"truehd":6},"max_frame_rate":{"truehd":50},)"
        R"("max_data_rate":{"truehd":12000},"max_objects":{"truehd":0},)" +
        outputs + management + tail);
    REQUIRE(support.has_value());
    CHECK(support->data_types ==
          std::vector<ac::DataType>{ac::DataType::kAc3, ac::DataType::kEac3, ac::DataType::kTruehd});
    CHECK(support->max_coded_channels_of(ac::DataType::kTruehd) == 6);
    CHECK(support->max_frame_rate_of(ac::DataType::kTruehd) == 50);
    CHECK(support->max_data_rate_of(ac::DataType::kTruehd) == 12000);
    CHECK(support->max_objects_of(ac::DataType::kTruehd) == 0);
    // Nothing is stated for the types the object does not name.
    CHECK(support->max_data_rate_of(ac::DataType::kEac3) == 0);
    CHECK(support->max_coded_channels_of(ac::DataType::kAc3) == 0);

    // And it writes what it read, the limit with the type's name.
    const std::string text = written(*support, ac::write_support);
    CHECK(text.find(R"("data_types":["ac3","eac3","truehd"])") != std::string::npos);
    CHECK(text.find(R"("max_data_rate":{"truehd":12000})") != std::string::npos);
    const std::optional<ac::Support> again = read_text(text);
    REQUIRE(again.has_value());
    CHECK(again->max_data_rate_of(ac::DataType::kTruehd) == 12000);

    // A sink from before the type has no TrueHD in it, and an older reader of this object would
    // skip the name: the role still has its other data types.
    const std::optional<ac::Support> before = read_text(
        R"({"data_types":["ac3","eac3"],"sample_rates":[48000],)" + outputs + management + tail);
    REQUIRE(before.has_value());
    CHECK(std::ranges::find(before->data_types, ac::DataType::kTruehd) == before->data_types.end());
    // A data rate that is not a rate refuses the object, as the frame rate does.
    CHECK_FALSE(read_text(R"({"data_types":["truehd"],"sample_rates":[48000],"max_data_rate":{"truehd":0},)" +
                          outputs + management + tail));
    CHECK_FALSE(read_text(R"({"data_types":["truehd"],"sample_rates":[48000],"max_data_rate":[],)" + outputs +
                          management + tail));
    // The burst type a stream of the type is sent as.
    CHECK(ac::burst_data_type(ac::DataType::kTruehd) == iclforge::sendspin::BurstDataType::kTruehd);
    CHECK(ac::carries(ac::DataType::kTruehd, iclforge::sendspin::BurstDataType::kTruehd));
    CHECK_FALSE(ac::carries(ac::DataType::kTruehd, iclforge::sendspin::BurstDataType::kEac3));
    CHECK(ac::data_type_name(ac::DataType::kTruehd) == "truehd");
}
