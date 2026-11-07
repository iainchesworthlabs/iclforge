#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/metadata_edit.hpp"
#include "truehd_encode_core.hpp"

// The options of the Forge window's Encode TrueHD dialog are the truehd- options of
// `forge truehd-encode`, read by one set of rules (apps/shared/media/src/truehd_encode_core.hpp):
// what each takes, the sentence that says so, and the encoder fields they set.

namespace core = iclforge::apps::truehd_encode;
namespace th = iclforge::truehd;

namespace {

bool takes(std::string_view name, std::string_view value) {
    core::Settings settings;
    return core::set_option(settings, name, value).has_value();
}

}  // namespace

TEST_CASE("each option takes the values the command line takes and refuses the rest",
          "[truehd][encode-settings]") {
    CHECK(takes("downmix", "standard"));
    CHECK(takes("downmix", "none"));
    CHECK_FALSE(takes("downmix", "stereo"));

    CHECK(takes("restart", "1"));
    CHECK(takes("restart", "8"));
    CHECK(takes("restart", "4096"));
    CHECK_FALSE(takes("restart", "0"));
    CHECK_FALSE(takes("restart", "4097"));
    CHECK_FALSE(takes("restart", "-8"));
    CHECK_FALSE(takes("restart", "8k"));
    CHECK_FALSE(takes("restart", ""));

    CHECK(takes("peak-rate", "1"));
    CHECK(takes("peak-rate", "100000"));
    CHECK_FALSE(takes("peak-rate", "0"));
    CHECK_FALSE(takes("peak-rate", "100001"));

    CHECK(takes("bitrate", "1000"));
    CHECK(takes("bitrate", "100000"));
    CHECK_FALSE(takes("bitrate", "999"));
    CHECK_FALSE(takes("bitrate", "100001"));

    CHECK(takes("major-sync", "0"));
    CHECK(takes("major-sync", "65536"));
    CHECK_FALSE(takes("major-sync", "65537"));
    CHECK_FALSE(takes("major-sync", "x"));

    CHECK(takes("fir-order", "0"));
    CHECK(takes("fir-order", "8"));
    CHECK_FALSE(takes("fir-order", "9"));
    CHECK_FALSE(takes("fir-order", "10"));
    CHECK_FALSE(takes("fir-order", ""));

    CHECK(takes("rate-check", "strict"));
    CHECK(takes("rate-check", "lenient"));
    CHECK_FALSE(takes("rate-check", "off"));
    CHECK(takes("matrix", "on"));
    CHECK(takes("matrix", "off"));
    CHECK_FALSE(takes("matrix", "lenient"));
    CHECK(takes("stereo-mode", "stereo"));
    CHECK(takes("stereo-mode", "ltrt"));
    CHECK(takes("stereo-mode", "headphone"));
    CHECK_FALSE(takes("stereo-mode", "surround"));

    CHECK(takes("downmix-gains", "-3,-3,off,-3"));
    CHECK(takes("downmix-gains", "0,0,0,0"));
    CHECK(takes("downmix-gains", "-60,-60,-60,-60"));
    CHECK_FALSE(takes("downmix-gains", "-3,-3,off"));
    CHECK_FALSE(takes("downmix-gains", "-3,-3,off,-3,-3"));
    CHECK_FALSE(takes("downmix-gains", "3,-3,off,-3"));
    CHECK_FALSE(takes("downmix-gains", "-61,-3,off,-3"));
    CHECK_FALSE(takes("downmix-gains", "-3,-3,nan,-3"));

    CHECK_FALSE(takes("rate-mode", "shape"));  // truehd-atmos only: not one of these
    CHECK_FALSE(takes("not-an-option", "1"));
}

TEST_CASE("a refusal says what the option takes and names it", "[truehd][encode-settings]") {
    core::Settings settings;
    const auto restart = core::set_option(settings, "restart", "0");
    REQUIRE_FALSE(restart.has_value());
    CHECK_THAT(restart.error(), Catch::Matchers::StartsWith("truehd-restart is the access units"));
    CHECK_THAT(restart.error(), Catch::Matchers::ContainsSubstring("4096"));
    const auto gains = core::set_option(settings, "downmix-gains", "x");
    REQUIRE_FALSE(gains.has_value());
    CHECK_THAT(gains.error(), Catch::Matchers::ContainsSubstring("-3,-3,off,-3"));
    // A refusal leaves what was set before it.
    CHECK(core::set_option(settings, "restart", "64").has_value());
    CHECK_FALSE(core::set_option(settings, "restart", "0").has_value());
    CHECK(settings.restart_units == 64);
}

TEST_CASE("the settings reach the encoder's configuration", "[truehd][encode-settings]") {
    core::Settings settings;
    for (const auto& [name, value] : {std::pair<std::string_view, std::string_view>{"downmix", "standard"},
                                      {"restart", "64"},
                                      {"major-sync", "128"},
                                      {"peak-rate", "12000"},
                                      {"rate-check", "lenient"},
                                      {"bitrate", "9000"},
                                      {"fir-order", "4"},
                                      {"matrix", "off"},
                                      {"stereo-mode", "headphone"},
                                      {"downmix-gains", "-6,-3,off,0"}}) {
        REQUIRE(core::set_option(settings, name, value).has_value());
    }
    settings.verify = true;

    th::EncoderConfig config;
    REQUIRE(core::apply(settings, config).has_value());
    CHECK(config.downmix == th::DownmixMode::kStandard);
    CHECK(config.restart_interval_units == 64);
    CHECK(config.major_sync_interval_units == 128);
    CHECK(config.peak_data_rate_bps == 12'000'000U);
    CHECK(config.allow_rate_violations);
    CHECK(config.rate_control.target_bitrate_bps == 9'000'000U);
    CHECK(config.max_fir_order == 4);
    CHECK_FALSE(config.matrix_search);
    CHECK(config.stereo_mode == 2U);
    CHECK(config.verify);
    CHECK(config.downmix_gains.centre == Catch::Approx(std::pow(10.0, -6.0 / 20.0)));
    CHECK(config.downmix_gains.surround == Catch::Approx(std::pow(10.0, -3.0 / 20.0)));
    CHECK(config.downmix_gains.lfe == 0.0);
    CHECK(config.downmix_gains.rear_to_surround == Catch::Approx(1.0));
}

TEST_CASE("settings left alone leave the encoder's defaults", "[truehd][encode-settings]") {
    th::EncoderConfig defaults;
    th::EncoderConfig config;
    REQUIRE(core::apply(core::Settings{}, config).has_value());
    CHECK(config.downmix == th::DownmixMode::kNone);
    CHECK(config.restart_interval_units == defaults.restart_interval_units);
    CHECK(config.major_sync_interval_units == defaults.major_sync_interval_units);
    CHECK(config.peak_data_rate_bps == defaults.peak_data_rate_bps);
    CHECK(config.allow_rate_violations == defaults.allow_rate_violations);
    CHECK(config.max_fir_order == defaults.max_fir_order);
    CHECK(config.matrix_search == defaults.matrix_search);
    CHECK(config.stereo_mode == defaults.stereo_mode);
    CHECK_FALSE(config.verify);
    CHECK(config.channel_meaning == defaults.channel_meaning);
}

// ---- the channel meaning -------------------------------------------------------------------------

namespace {

bool takes_meaning(std::string_view name, std::string_view value) {
    th::ChannelMeaningEdit edit;
    return core::set_meaning_option(edit, name, value).has_value();
}

}  // namespace

TEST_CASE("the channel meaning options take the widths of the fields and nothing else",
          "[truehd][encode-settings][meaning]") {
    CHECK(takes_meaning("dialnorm-2ch", "0"));
    CHECK(takes_meaning("dialnorm-2ch", "63"));
    CHECK_FALSE(takes_meaning("dialnorm-2ch", "64"));
    CHECK_FALSE(takes_meaning("dialnorm-2ch", "-1"));
    CHECK(takes_meaning("dialnorm-6ch", "31"));
    CHECK_FALSE(takes_meaning("dialnorm-6ch", "32"));
    CHECK(takes_meaning("dialnorm-8ch", "27"));
    CHECK_FALSE(takes_meaning("dialnorm-8ch", "32"));
    for (const char* name : {"mix-level-2ch", "mix-level-6ch", "mix-level-8ch"}) {
        CHECK(takes_meaning(name, "63"));
        CHECK_FALSE(takes_meaning(name, "64"));
    }
    CHECK(takes_meaning("source-format-6ch", "31"));
    CHECK_FALSE(takes_meaning("source-format-6ch", "32"));
    CHECK(takes_meaning("source-format-8ch", "63"));
    CHECK_FALSE(takes_meaning("source-format-8ch", "64"));
    CHECK_FALSE(takes_meaning("source-format-2ch", "1"));  // the 2 channel presentation has none
    CHECK(takes_meaning("heavy-drc-start-up-gain", "-32"));
    CHECK(takes_meaning("heavy-drc-start-up-gain", "31"));
    CHECK_FALSE(takes_meaning("heavy-drc-start-up-gain", "-33"));
    CHECK_FALSE(takes_meaning("heavy-drc-start-up-gain", "32"));
    CHECK(takes_meaning("drc-start-up-gain", "-64"));
    CHECK(takes_meaning("drc-start-up-gain", "63"));
    CHECK_FALSE(takes_meaning("drc-start-up-gain", "64"));
    CHECK_FALSE(takes_meaning("drc-start-up-gain", "+5"));
    CHECK_FALSE(takes_meaning("drc-start-up-gain", "5 "));
    CHECK_FALSE(takes_meaning("drc-start-up-gain", "1.5"));
    CHECK_FALSE(takes_meaning("drc-start-up-gain", ""));
    for (const char* name : {"control-2ch", "control-6ch", "control-8ch"}) {
        CHECK(takes_meaning(name, "on"));
        CHECK(takes_meaning(name, "off"));
        CHECK_FALSE(takes_meaning(name, "1"));
        CHECK_FALSE(takes_meaning(name, "On"));
    }
    CHECK_FALSE(takes_meaning("restart", "64"));  // a setting of the stream, not of its meaning
}

TEST_CASE("the channel meaning options are named once and the settings read them",
          "[truehd][encode-settings][meaning]") {
    CHECK(core::meaning_option_names().size() == 13);
    for (const std::string_view name : core::meaning_option_names()) {
        CAPTURE(name);
        CHECK(core::is_meaning_option(name));
        // Every name the list gives is one of the settings, with a value of its own kind.
        core::Settings settings;
        CHECK(core::set_option(settings, name, name.starts_with("control") ? "on" : "1")
                  .has_value());
        CHECK_FALSE(settings.meaning.empty());
    }
    CHECK_FALSE(core::is_meaning_option("restart"));
    CHECK_FALSE(core::is_meaning_option("truehd-dialnorm-2ch"));
    CHECK_FALSE(core::is_meaning_option(""));
}

TEST_CASE("a refusal of a channel meaning option says the range and names the option",
          "[truehd][encode-settings][meaning]") {
    core::Settings settings;
    const auto dialnorm = core::set_option(settings, "dialnorm-6ch", "40");
    REQUIRE_FALSE(dialnorm.has_value());
    CHECK(dialnorm.error() == "truehd-dialnorm-6ch is a whole number from 0 to 31");
    const auto gain = core::set_option(settings, "heavy-drc-start-up-gain", "x");
    REQUIRE_FALSE(gain.has_value());
    CHECK(gain.error() == "truehd-heavy-drc-start-up-gain is a whole number from -32 to 31");
    const auto control = core::set_option(settings, "control-8ch", "yes");
    REQUIRE_FALSE(control.has_value());
    CHECK(control.error() == "truehd-control-8ch is on or off");
    // Nothing was set by a refusal.
    CHECK(settings.meaning.empty());
    // A later good value is kept, and a later refusal leaves it.
    CHECK(core::set_option(settings, "dialnorm-6ch", "27").has_value());
    CHECK_FALSE(core::set_option(settings, "dialnorm-6ch", "40").has_value());
    CHECK(settings.meaning.sixch_dialogue_norm == 27);
}

TEST_CASE("the channel meaning reaches the encoder's configuration and only what was named",
          "[truehd][encode-settings][meaning]") {
    core::Settings settings;
    for (const auto& [name, value] :
         {std::pair<std::string_view, std::string_view>{"dialnorm-2ch", "40"},
          {"dialnorm-6ch", "27"},
          {"dialnorm-8ch", "20"},
          {"mix-level-2ch", "10"},
          {"mix-level-6ch", "11"},
          {"mix-level-8ch", "12"},
          {"source-format-6ch", "5"},
          {"source-format-8ch", "6"},
          {"control-2ch", "on"},
          {"control-8ch", "on"},
          {"heavy-drc-start-up-gain", "-9"},
          {"drc-start-up-gain", "21"}}) {
        REQUIRE(core::set_option(settings, name, value).has_value());
    }
    th::EncoderConfig config;
    REQUIRE(core::apply(settings, config).has_value());
    const th::ChannelMeaning& m = config.channel_meaning;
    CHECK(m.twoch_dialogue_norm == 40);
    CHECK(m.sixch_dialogue_norm == 27);
    CHECK(m.eightch_dialogue_norm == 20);
    CHECK(m.twoch_mix_level == 10);
    CHECK(m.sixch_mix_level == 11);
    CHECK(m.eightch_mix_level == 12);
    CHECK(m.sixch_source_format == 5);
    CHECK(m.eightch_source_format == 6);
    CHECK(m.twoch_control_enabled);
    CHECK_FALSE(m.sixch_control_enabled);  // not named: left as the encoder has it
    CHECK(m.eightch_control_enabled);
    CHECK(m.heavy_drc_start_up_gain == -9);
    CHECK(m.drc_start_up_gain == 21);

    // A value the settings were filled with by hand and the field cannot hold is the field's sentence.
    core::Settings bad;
    bad.meaning.sixch_dialogue_norm = 99;
    th::EncoderConfig other;
    const auto refused = core::apply(bad, other);
    REQUIRE_FALSE(refused.has_value());
    CHECK_THAT(refused.error(), Catch::Matchers::ContainsSubstring("dialogue"));
}

TEST_CASE("the channel meaning is in the major sync of the stream the encode writes",
          "[truehd][encode-settings][meaning]") {
    iclforge::ac3::io::WavData wav;
    wav.sample_rate = 48000;
    wav.channels.assign(2, std::vector<float>(48000 / 4));
    for (std::size_t i = 0; i < wav.channels[0].size(); ++i) {
        wav.channels[0][i] = 0.25F * std::sin(0.05F * static_cast<float>(i));
        wav.channels[1][i] = 0.25F * std::sin(0.07F * static_cast<float>(i));
    }
    const auto layout = core::layout_of(wav);
    REQUIRE(layout.has_value());

    const auto meaning_of = [&](const core::Settings& settings) {
        th::EncoderConfig config;
        REQUIRE(core::apply(settings, config).has_value());
        auto outcome = core::encode_wav(wav, *layout, config);
        REQUIRE(outcome.has_value());
        // An edit with nothing set changes no byte and reports the first major sync's meaning.
        const auto summary = th::edit_channel_meaning(outcome->stream, th::ChannelMeaningEdit{});
        REQUIRE(summary.has_value());
        return summary->before;
    };

    const th::ChannelMeaning plain = meaning_of(core::Settings{});
    CHECK(plain == th::ChannelMeaning{});  // the default is what FFmpeg's streams have: zeros

    core::Settings settings;
    REQUIRE(core::set_option(settings, "dialnorm-2ch", "31").has_value());
    REQUIRE(core::set_option(settings, "mix-level-2ch", "17").has_value());
    REQUIRE(core::set_option(settings, "control-2ch", "on").has_value());
    REQUIRE(core::set_option(settings, "drc-start-up-gain", "-12").has_value());
    const th::ChannelMeaning written = meaning_of(settings);
    CHECK(written.twoch_dialogue_norm == 31);
    CHECK(written.twoch_mix_level == 17);
    CHECK(written.twoch_control_enabled);
    CHECK(written.drc_start_up_gain == -12);
    CHECK(written.heavy_drc_start_up_gain == 0);
    CHECK(written.sixch_dialogue_norm == 0);
}
