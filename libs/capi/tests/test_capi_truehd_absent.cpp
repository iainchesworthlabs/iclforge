// The TrueHD C entry points of a library built with ICLFORGE_BUILD_TRUEHD off
// (src/truehd_absent.cpp): the same symbols, every fallible one answering
// ICLFORGE_ERROR_UNSUPPORTED and the rest a NULL, 0 or
// "". test_capi_truehd.cpp is the other build's.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "iclforge_c/iclforge.h"
#include "iclforge_c/version.h"

// The text a macro expands to, or the macro's own name when it is not defined (see
// test_capi_truehd.cpp).
#define ICLFORGE_TEST_STRINGIFY_VALUE(x) #x
#define ICLFORGE_TEST_STRINGIFY(x) ICLFORGE_TEST_STRINGIFY_VALUE(x)

TEST_CASE("version.h says the TrueHD surface is not built into this library", "[capi][truehd]") {
    CHECK(std::string_view(ICLFORGE_TEST_STRINGIFY(ICLFORGE_HAS_TRUEHD)) == "ICLFORGE_HAS_TRUEHD");
}

TEST_CASE("without ICLFORGE_BUILD_TRUEHD every TrueHD call is unsupported", "[capi][truehd]") {
    const std::array<std::uint8_t, 16> bytes{};
    iclforge_truehd_decoder_config_t config;
    iclforge_truehd_decoder_config_init(&config);
    CHECK(config.presentation == ICLFORGE_TRUEHD_PRESENTATION_BEST);
    CHECK(config.lenient == 0);

    iclforge_truehd_stream_info_t info;
    CHECK(iclforge_truehd_probe(bytes.data(), bytes.size(), &info) == ICLFORGE_ERROR_UNSUPPORTED);
    iclforge_truehd_units_t* units = nullptr;
    CHECK(iclforge_truehd_scan(bytes.data(), bytes.size(), &units) == ICLFORGE_ERROR_UNSUPPORTED);
    CHECK(units == nullptr);
    CHECK(iclforge_truehd_units_count(units) == 0);

    iclforge_truehd_decoder_t* decoder = nullptr;
    CHECK(iclforge_truehd_decoder_create(&config, &decoder) == ICLFORGE_ERROR_UNSUPPORTED);
    CHECK(decoder == nullptr);
    iclforge_truehd_decoded_t* decoded = nullptr;
    CHECK(iclforge_truehd_decoder_decode(decoder, bytes.data(), bytes.size(), &decoded) ==
          ICLFORGE_ERROR_UNSUPPORTED);
    CHECK(std::string_view(iclforge_truehd_decoder_last_error_detail(decoder)).empty());
    CHECK(iclforge_truehd_decoded_pcm(decoded) == nullptr);
    CHECK(iclforge_truehd_decoded_channels(decoded) == 0);
    CHECK(std::string_view(iclforge_truehd_decoded_channel_name(decoded, 0)).empty());
    iclforge_truehd_decoder_lost_data(decoder);  // a no-op, like reset and destroy
    iclforge_truehd_decoder_destroy(decoder);
    iclforge_truehd_decoded_destroy(decoded);
    iclforge_truehd_units_destroy(units);
}

TEST_CASE("without ICLFORGE_BUILD_TRUEHD the TrueHD encoder is unsupported too", "[capi][truehd]") {
    iclforge_truehd_encoder_config_t config;
    iclforge_truehd_encoder_config_init(&config);
    CHECK(config.sample_rate == 48000);
    CHECK(config.channels == 2);
    CHECK(config.bits == 24);
    CHECK(config.restart_interval_units == 128);
    CHECK(config.peak_data_rate_bps == 18'000'000);
    CHECK(config.downmix == ICLFORGE_TRUEHD_DOWNMIX_NONE);
    CHECK(config.downmix_lfe == 0.0);
    CHECK(config.max_fir_order == 8);
    CHECK(config.matrix_search == 1);

    iclforge_truehd_encoder_t* encoder = nullptr;
    CHECK(iclforge_truehd_encoder_create(&config, &encoder) == ICLFORGE_ERROR_UNSUPPORTED);
    CHECK(encoder == nullptr);
    CHECK(iclforge_truehd_default_channel_assignment(2) == 0);
    CHECK(std::string_view(iclforge_truehd_encoder_refusal_reason(&config)).empty());
    const std::array<std::int32_t, 4> pcm{};
    iclforge_truehd_encoded_t* encoded = nullptr;
    CHECK(iclforge_truehd_encoder_encode(encoder, pcm.data(), 2, &encoded) ==
          ICLFORGE_ERROR_UNSUPPORTED);
    CHECK(iclforge_truehd_encoder_flush(encoder, &encoded) == ICLFORGE_ERROR_UNSUPPORTED);
    CHECK(encoded == nullptr);
    CHECK(iclforge_truehd_encoder_substreams(encoder) == 0);
    CHECK(iclforge_truehd_encoded_count(encoded) == 0);
    CHECK(iclforge_truehd_encoded_data(encoded, 0) == nullptr);
    iclforge_truehd_encoder_destroy(encoder);
    iclforge_truehd_encoded_destroy(encoded);
}
