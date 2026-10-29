#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"

// The encoder (planning/truehd.md, phase T2a), held to this library's decoder: the codec is
// lossless, so whatever goes in comes back sample for sample. The check against FFmpeg's strict
// decode is tools/checks/verify_truehd_encode_interop.py; these are the cases that do not need it.

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

TEST_CASE("every rate, channel count and width comes back sample for sample",
          "[truehd][encoder][roundtrip]") {
    struct Case {
        std::uint32_t rate;
        int channels;
        int bits;
    };
    const std::array<Case, 14> cases{{
        {48000, 1, 24},
        {48000, 2, 24},
        {48000, 2, 16},
        {48000, 2, 20},
        {44100, 2, 24},
        {96000, 2, 24},
        {88200, 2, 16},
        {192000, 2, 24},
        {176400, 2, 24},
        {48000, 3, 24},
        {48000, 6, 24},
        {96000, 6, 16},
        {48000, 7, 24},
        {48000, 8, 24},
    }};
    for (const Case& c : cases) {
        CAPTURE(c.rate, c.channels, c.bits);
        EncoderConfig config;
        config.sample_rate = c.rate;
        config.channels = c.channels;
        config.bits = c.bits;
        config.restart_interval_units = 8;  // several restart intervals in a short stream
        config.verify = true;
        const int frames = static_cast<int>(c.rate / 8);  // 125 ms: a dozen intervals or more
        const auto pcm = make_pcm(c.channels, frames, c.bits,
                                  static_cast<std::uint32_t>(c.channels * 7 + c.bits));
        const Encoded encoded = encode_all(config, pcm);
        REQUIRE(encoded.stats.access_units == encoded.units.size());
        REQUIRE(encoded.stats.restart_intervals >= 3);
        REQUIRE(encoded.stats.verified_units == encoded.units.size());
        int padding = 0;
        auto decoded = decode_all(encoded.units, c.bits, c.channels, Presentation::kBest, &padding);
        REQUIRE(padding >= 0);
        REQUIRE(decoded.size() == pcm.size());
        REQUIRE(decoded == pcm);
    }
}

TEST_CASE("the output does not depend on how the input is chunked",
          "[truehd][encoder][roundtrip]") {
    EncoderConfig config;
    config.channels = 6;
    config.restart_interval_units = 8;
    const auto pcm = make_pcm(6, 4000, 24, 3);
    const Encoded whole = encode_all(config, pcm);
    for (const std::size_t chunk : {1UL, 7UL, 333UL, 1000UL}) {
        const Encoded chunked = encode_all(config, pcm, chunk);
        REQUIRE(chunked.units.size() == whole.units.size());
        for (std::size_t i = 0; i < whole.units.size(); ++i) {
            REQUIRE(chunked.units[i].bytes == whole.units[i].bytes);
        }
    }
}

TEST_CASE("a stream that does not end on a unit boundary ends with the marker and loses no sample",
          "[truehd][encoder][roundtrip]") {
    for (const int frames : {1, 39, 41, 1000, 4001}) {
        CAPTURE(frames);
        EncoderConfig config;
        config.restart_interval_units = 8;
        const auto pcm = make_pcm(2, frames, 24, 11);
        const Encoded encoded = encode_all(config, pcm);
        REQUIRE(!encoded.units.empty());
        const int nominal = 40;
        const int units = (frames + nominal - 1) / nominal;
        REQUIRE(static_cast<int>(encoded.units.size()) == units);
        REQUIRE(encoded.units.back().padding_samples == units * nominal - frames);
        int padding = 0;
        const auto decoded = decode_all(encoded.units, 24, 2, Presentation::kBest, &padding);
        REQUIRE(padding == units * nominal - frames);
        REQUIRE(decoded.size() == pcm.size());
        REQUIRE(decoded == pcm);
    }
}

TEST_CASE("a major sync and a restart header open every restart interval and no other unit",
          "[truehd][encoder][stream]") {
    for (const int interval : {1, 8, 9, 50}) {
        CAPTURE(interval);
        EncoderConfig config;
        config.channels = 6;
        config.restart_interval_units = interval;
        const auto pcm = make_pcm(6, 40 * 130, 24, 5);
        const Encoded encoded = encode_all(config, pcm);
        REQUIRE(encoded.units.size() == 130);
        for (std::size_t i = 0; i < encoded.units.size(); ++i) {
            const bool expected = i % static_cast<std::size_t>(interval) == 0;
            REQUIRE(encoded.units[i].has_major_sync == expected);
            auto parsed = parse_access_unit(encoded.units[i].bytes, 2, true);
            REQUIRE(parsed.has_value());
            REQUIRE(parsed->major_sync.has_value() == expected);
            // TH spec §6.1, ERRATA.md entry 18: a unit with a major sync restarts every substream.
            for (int s = 0; s < parsed->num_substreams; ++s) {
                REQUIRE(parsed->entries[static_cast<std::size_t>(s)].restart_nonexistent ==
                        !expected);
                REQUIRE(parsed->entries[static_cast<std::size_t>(s)].crc_present);
            }
        }
    }
}

TEST_CASE("a restart gap of 2 to 7 units is refused", "[truehd][encoder][config]") {
    for (const int gap : {0, 2, 3, 7, -1}) {
        EncoderConfig config;
        config.restart_interval_units = gap;
        REQUIRE(!Encoder::create(config).has_value());
    }
    for (const int gap : {1, 8, 128}) {
        EncoderConfig config;
        config.restart_interval_units = gap;
        REQUIRE(Encoder::create(config).has_value());
    }
}

TEST_CASE("configurations the encoder does not write are refused with a reason",
          "[truehd][encoder][config]") {
    const auto refused = [](auto change) {
        EncoderConfig config;
        change(config);
        auto encoder = Encoder::create(config);
        REQUIRE(!encoder.has_value());
        REQUIRE(!encoder.error().detail.empty());
        REQUIRE(!Encoder::refusal_reason(config).empty());
    };
    refused([](EncoderConfig& c) { c.channels = 0; });
    refused([](EncoderConfig& c) { c.channels = 9; });
    refused([](EncoderConfig& c) { c.sample_rate = 32000; });
    refused([](EncoderConfig& c) { c.bits = 18; });
    refused([](EncoderConfig& c) { c.kind = StreamKind::kMlp; });
    refused([](EncoderConfig& c) {
        c.channels = 7;
        c.sample_rate = 192000;
    });
    refused([](EncoderConfig& c) {
        c.channels = 2;
        c.channel_assignment = 0xF;
    });
    refused([](EncoderConfig& c) { c.max_fir_order = 9; });
    refused([](EncoderConfig& c) { c.peak_data_rate_bps = 10; });
}

TEST_CASE("PCM outside the declared width or not whole frames is refused",
          "[truehd][encoder][config]") {
    EncoderConfig config;
    config.bits = 16;
    auto encoder = Encoder::create(config);
    REQUIRE(encoder.has_value());
    const std::array<std::int32_t, 2> wide{40000, 0};
    REQUIRE(!encoder->encode(wide).has_value());
    const std::array<std::int32_t, 3> odd{1, 2, 3};
    REQUIRE(!encoder->encode(odd).has_value());
}

TEST_CASE("the low bits every sample leaves clear are sent as the output shift and quantiser step",
          "[truehd][encoder][stream]") {
    struct Case {
        int bits;
        int shift;
        int quant;
    };
    for (const Case c : {Case{16, 7, 1}, Case{20, 4, 0}, Case{24, 0, 0}}) {
        CAPTURE(c.bits);
        EncoderConfig config;
        config.bits = c.bits;
        config.restart_interval_units = 8;
        // Full-scale noise so that no more than the declared width's low bits are clear.
        std::vector<std::int32_t> pcm(2 * 800);
        std::uint32_t state = 99;
        for (auto& v : pcm) {
            state = state * 1664525U + 1013904223U;
            v = (static_cast<std::int32_t>(state >> 8U) - (1 << 23)) >> (24 - c.bits);
        }
        const Encoded encoded = encode_all(config, pcm);
        auto parsed = parse_access_unit(encoded.units.front().bytes, 1, true);
        REQUIRE(parsed.has_value());
        const auto payload = parsed->segments[0];
        iclforge::BitReader reader(payload);
        REQUIRE(reader.read(1) == 1);  // block_header_exists
        REQUIRE(reader.read(1) == 1);  // restart_header_exists
        auto restart = read_restart_header(reader, payload, true);
        REQUIRE(restart.has_value());
        REQUIRE(restart->max_shift == c.shift);
        BlockContext context;
        context.max_chan = restart->max_chan;
        context.max_matrix_chan = restart->max_matrix_chan;
        context.noise_type = restart->noise_type();
        auto block = read_block_header(reader, context);
        REQUIRE(block.has_value());
        for (int ch = 0; ch < 2; ++ch) {
            const auto at = static_cast<std::size_t>(ch);
            const int shift = block->output_shift ? (*block->output_shift)[at] : 0;
            const int quant = block->quant_step ? (*block->quant_step)[at] : 0;
            REQUIRE(shift == c.shift);
            REQUIRE(quant == c.quant);
        }
    }
}

TEST_CASE("a restart header carries the lossless check of the interval before it",
          "[truehd][encoder][stream]") {
    EncoderConfig config;
    config.restart_interval_units = 8;
    const auto pcm = make_pcm(2, 40 * 40, 24, 21);
    Encoded encoded = encode_all(config, pcm);

    // Change the samples a unit decodes to while keeping its parity and CRC valid, so that only the
    // lossless check can notice: add one to the first residual of a block in the second interval's
    // middle. The next restart header's check no longer matches.
    DecoderConfig strict;
    strict.strictness = Strictness::kStrict;
    {
        Decoder decoder(strict);
        for (const auto& unit : encoded.units) {
            REQUIRE(decoder.decode(unit.bytes).has_value());
        }
        REQUIRE(decoder.counters().lossless_checks_ok >= 3);
        REQUIRE(decoder.counters().lossless_check_failures == 0);
    }
    // Flip a bit in the data of unit 12 (interval 1) and fix the checks.
    std::vector<std::byte>& bytes = encoded.units[12].bytes;
    auto parsed = parse_access_unit(bytes, 1, true);
    REQUIRE(parsed.has_value());
    const std::size_t segment_at =
        static_cast<std::size_t>(parsed->segments[0].data() - bytes.data());
    const std::size_t payload_size = parsed->segments[0].size() - 2;
    bytes[segment_at + payload_size / 2] ^= std::byte{0x04};
    const auto payload = std::span<const std::byte>(bytes).subspan(segment_at, payload_size);
    bytes[segment_at + payload_size] = static_cast<std::byte>(substream_parity(payload));
    bytes[segment_at + payload_size + 1] = static_cast<std::byte>(substream_crc(payload));

    Decoder decoder(strict);
    bool failed = false;
    for (const auto& unit : encoded.units) {
        auto decoded = decoder.decode(unit.bytes);
        if (!decoded) {
            failed = true;
            break;
        }
    }
    // A flipped residual bit either breaks the block data or changes the samples; either way the
    // strict decoder must not accept the stream.
    REQUIRE(failed);
}

TEST_CASE("the declared peak data rate is kept or the encoder says it cannot",
          "[truehd][encoder][rate]") {
    // White noise at 192 kHz in 6 channels needs about 27 Mbps, more than 18; the lag behind real
    // time passes the 75 ms of TH spec §18 after about 150 ms of it.
    EncoderConfig config;
    config.sample_rate = 192000;
    config.channels = 6;
    config.restart_interval_units = 8;
    std::vector<std::int32_t> pcm(6 * 57600);
    std::uint32_t state = 17;
    for (auto& v : pcm) {
        state = state * 1664525U + 1013904223U;
        v = static_cast<std::int32_t>(state >> 8U) - (1 << 23);
    }
    {
        auto encoder = Encoder::create(config);
        REQUIRE(encoder.has_value());
        std::optional<EncodeErrorKind> failure;
        if (auto units = encoder->encode(pcm); !units.has_value()) {
            failure = units.error().kind;
        } else if (auto tail = encoder->flush(); !tail.has_value()) {
            failure = tail.error().kind;
        }
        REQUIRE(failure.has_value());
        REQUIRE(*failure == EncodeErrorKind::kRateExceeded);
    }
    config.allow_rate_violations = true;
    const Encoded encoded = encode_all(config, pcm);
    REQUIRE(encoded.stats.rate_violations > 0);

    // Quiet material fits, and `input_timing` then advances by one unit per unit (TH spec §3.4,
    // ERRATA.md entry 37).
    EncoderConfig quiet;
    quiet.restart_interval_units = 8;
    const Encoded steady = encode_all(quiet, make_pcm(2, 40 * 60, 24, 4));
    REQUIRE(steady.stats.rate_violations == 0);
    std::uint16_t previous = 0;
    for (std::size_t i = 0; i < steady.units.size(); ++i) {
        auto parsed = parse_access_unit(steady.units[i].bytes, 1, true);
        REQUIRE(parsed.has_value());
        if (i > 0) {
            REQUIRE(static_cast<std::uint16_t>(parsed->input_timing - previous) == 40);
        }
        previous = parsed->input_timing;
    }
}

TEST_CASE("a payload handed to the encoder is framed as extra data and comes back",
          "[truehd][encoder][extra]") {
    EncoderConfig config;
    config.restart_interval_units = 8;
    std::vector<std::vector<std::byte>> payloads(20);
    config.extra_data = [&payloads](std::uint64_t index) {
        if (index % 3 == 1) {
            return std::vector<std::byte>{};
        }
        std::vector<std::byte> payload(static_cast<std::size_t>(5 + index));
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::byte>((index * 31 + i * 7) & 0xFF);
        }
        if (index < payloads.size()) {
            payloads[index] = payload;
        }
        return payload;
    };
    const Encoded encoded = encode_all(config, make_pcm(2, 40 * 20, 24, 8));
    Decoder decoder;
    for (std::size_t i = 0; i < encoded.units.size(); ++i) {
        auto parsed = parse_access_unit(encoded.units[i].bytes, 1, true);
        REQUIRE(parsed.has_value());
        if (i % 3 == 1) {
            REQUIRE(!parsed->extra_data.has_value());
        } else {
            REQUIRE(parsed->extra_data.has_value());
            REQUIRE(parsed->extra_data->check_ok);
            REQUIRE(parsed->extra_data->framing_ok);
            REQUIRE(parsed->extra_data->parity_ok);
            REQUIRE(std::equal(parsed->extra_data->evolution_payload.begin(),
                               parsed->extra_data->evolution_payload.end(), payloads[i].begin(),
                               payloads[i].end()));
        }
        // The audio decodes the same with or without it.
        auto decoded = decoder.decode(encoded.units[i].bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
    }
}

TEST_CASE("the stream reads back through the elementary reader and probe",
          "[truehd][encoder][stream]") {
    EncoderConfig config;
    config.channels = 6;
    config.sample_rate = 96000;
    config.restart_interval_units = 8;
    const Encoded encoded = encode_all(config, make_pcm(6, 80 * 40, 24, 2));
    std::vector<std::byte> stream;
    for (const auto& unit : encoded.units) {
        stream.insert(stream.end(), unit.bytes.begin(), unit.bytes.end());
    }
    auto info = probe_stream(stream);
    REQUIRE(info.has_value());
    REQUIRE(info->sample_rate == 96000);
    REQUIRE(info->num_substreams == 2);
    REQUIRE(info->channels[0] == 2);
    REQUIRE(info->channels[1] == 6);
    REQUIRE(info->samples_per_access_unit == 80);
    ElementaryReader reader(stream);
    std::size_t count = 0;
    while (reader.next()) {
        ++count;
    }
    REQUIRE(count == encoded.units.size());
    REQUIRE(reader.skipped_bytes() == 0);
}

TEST_CASE("the 2 and 6 channel decodes of a spread stream give the first channels unchanged",
          "[truehd][encoder][roundtrip]") {
    EncoderConfig config;
    config.channels = 8;
    config.restart_interval_units = 8;
    const auto pcm = make_pcm(8, 40 * 40, 24, 13);
    const Encoded encoded = encode_all(config, pcm);
    REQUIRE(Encoder::create(config)->substreams() == 3);
    for (const auto& [presentation, channels] :
         {std::pair{Presentation::k2ch, 2}, std::pair{Presentation::k6ch, 6},
          std::pair{Presentation::k8ch, 8}}) {
        const auto decoded = decode_all(encoded.units, 24, channels, presentation);
        REQUIRE(decoded.size() == static_cast<std::size_t>(40 * 40 * channels));
        for (std::size_t t = 0; t < 40 * 40; ++t) {
            for (int c = 0; c < channels; ++c) {
                REQUIRE(
                    decoded[t * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] ==
                    pcm[t * 8 + static_cast<std::size_t>(c)]);
            }
        }
    }
}

TEST_CASE("silence costs almost nothing and a pair of equal channels costs one",
          "[truehd][encoder][size]") {
    EncoderConfig config;
    config.restart_interval_units = 128;
    const std::vector<std::int32_t> silence(2 * 48000, 0);
    const Encoded quiet = encode_all(config, silence);
    REQUIRE(quiet.stats.bytes <= quiet.stats.access_units * 16);  // the unit overhead and no more

    std::vector<std::int32_t> twin(2 * 48000);
    const auto mono = make_pcm(1, 48000, 24, 6);
    for (std::size_t t = 0; t < 48000; ++t) {
        twin[2 * t] = mono[t];
        twin[2 * t + 1] = mono[t];
    }
    const Encoded paired = encode_all(config, twin);
    config.channels = 1;
    const Encoded single = encode_all(config, mono);
    REQUIRE(paired.stats.bytes < single.stats.bytes + single.stats.bytes / 8);
}
