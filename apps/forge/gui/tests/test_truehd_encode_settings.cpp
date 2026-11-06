#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include "iclforge/truehd/encoder/encoder.hpp"
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
    CHECK_FALSE(takes("dialnorm-2ch", "-27"));
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
    core::apply(settings, config);
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
    core::apply(core::Settings{}, config);
    CHECK(config.downmix == th::DownmixMode::kNone);
    CHECK(config.restart_interval_units == defaults.restart_interval_units);
    CHECK(config.major_sync_interval_units == defaults.major_sync_interval_units);
    CHECK(config.peak_data_rate_bps == defaults.peak_data_rate_bps);
    CHECK(config.allow_rate_violations == defaults.allow_rate_violations);
    CHECK(config.max_fir_order == defaults.max_fir_order);
    CHECK(config.matrix_search == defaults.matrix_search);
    CHECK(config.stereo_mode == defaults.stereo_mode);
    CHECK_FALSE(config.verify);
}
