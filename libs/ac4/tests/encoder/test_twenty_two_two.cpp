// iclforge::ac4::Encoder's 22.2 channel element end to end (ETSI TS 103 190-2
// V1.3.1 clause 6.2.4.3, behind experimental.twenty_two_two): two LFE tracks
// and eleven channel pairs of Table 21, in the SIMPLE and ASPX codec modes, from
// 24 input channels in Table A.27's order. Each stream reads back with the trace
// the encoder recorded, and decodes in full decoding with each channel's tone
// on its own channel. The signals are half a second long, and a third of a
// second under the sanitizers (tests/support/sanitized.hpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4/core/syntax.hpp"
#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/encoder/encoder.hpp"
#include "iclforge/ac4/io/carriage.hpp"
#include "sanitized.hpp"

namespace {

using iclforge::ac4::CodecMode;
using iclforge::ac4::Speaker;
using iclforge::test::kSanitized;

// The decoder's delay at frame_rate_index 13 (d_pcm, the QMF banks' 577
// samples and six QMF slots) and the encoder's, a frame and a half.
constexpr std::size_t kDecoderDelay = 352 + 577 + 6 * 64;
constexpr std::size_t kLag = 3072 + kDecoderDelay;
constexpr std::size_t kSamples = kSanitized ? 16384 : 24000;
constexpr double kAmplitude = 0.1;  // -20 dBFS

struct Channel {
    Speaker speaker;
    double hz;
};

// The 24 channels in Part 2 Table A.27's order by speaker index, which is the
// encoder's input order and the decoder's output order, each with a tone of its
// own: primes, so that none is on another's harmonic, 60 Hz or more apart, the
// LFEs under the 120 Hz their tracks code to, and every other under the lowest
// crossover the ASPX mode here has (7.5 kHz).
std::vector<Channel> layout() {
    return {
        {Speaker::kLeft, 331.0},         {Speaker::kRight, 457.0},
        {Speaker::kCentre, 613.0},       {Speaker::kLeftSurround, 787.0},
        {Speaker::kRightSurround, 953.0}, {Speaker::kLeftBack, 1777.0},
        {Speaker::kRightBack, 1931.0},   {Speaker::kTopFrontLeft, 1117.0},
        {Speaker::kTopFrontRight, 1289.0}, {Speaker::kTopBackLeft, 1453.0},
        {Speaker::kTopBackRight, 1621.0}, {Speaker::kLfe, 47.0},
        {Speaker::kTopSideLeft, 2131.0}, {Speaker::kTopSideRight, 2287.0},
        {Speaker::kTopFrontCentre, 2459.0}, {Speaker::kTopBackCentre, 2633.0},
        {Speaker::kTopCentre, 2791.0},   {Speaker::kLfe2, 71.0},
        {Speaker::kBottomFrontLeft, 2971.0}, {Speaker::kBottomFrontRight, 3109.0},
        {Speaker::kBottomFrontCentre, 3253.0}, {Speaker::kCentreBack, 3407.0},
        {Speaker::kLeftWide, 3571.0},    {Speaker::kRightWide, 3739.0},
    };
}

std::vector<float> tone(double hz, std::size_t count, double rate = 48000.0) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(
            kAmplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / rate));
    }
    return x;
}

std::vector<std::vector<float>> tones(const std::vector<Channel>& channels, double rate = 48000.0) {
    std::vector<std::vector<float>> input;
    for (const Channel& c : channels) {
        input.push_back(tone(c.hz, kSamples, rate));
    }
    return input;
}

iclforge::ac4::EncoderConfig config_at(int kbps, CodecMode mode = CodecMode::kAuto) {
    iclforge::ac4::EncoderConfig config;
    config.channels = 24;
    config.bitrate_kbps = kbps;
    config.codec_mode = mode;
    config.experimental.twenty_two_two = true;
    return config;
}

struct Encoded {
    std::vector<iclforge::ac4::EncodedFrame> frames;
    std::vector<iclforge::ac4::SyntaxRecord> trace;
    CodecMode mode = CodecMode::kAuto;
};

Encoded encode(const iclforge::ac4::EncoderConfig& base,
               const std::vector<std::vector<float>>& input) {
    Encoded out;
    iclforge::ac4::EncoderConfig config = base;
    const auto sink = [&out](const iclforge::ac4::SyntaxRecord& r) { out.trace.push_back(r); };
    config.trace = sink;
    auto encoder = iclforge::ac4::Encoder::create(config);
    INFO(iclforge::ac4::Encoder::refusal_reason(base));
    REQUIRE(encoder.has_value());
    out.mode = encoder->codec_mode();
    std::vector<std::span<const float>> views;
    for (const auto& channel : input) {
        views.emplace_back(channel);
    }
    auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    out.frames = *frames;
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    out.frames.insert(out.frames.end(), rest->begin(), rest->end());
    return out;
}

struct Decoded {
    std::vector<Speaker> speakers;
    std::vector<std::vector<float>> channels;
};

// A 22.2 presentation has 22 tracks, which no md_compat level below 7
// (unrestricted) holds (Part 2 Table 55), so the decoder claims level 7.
Decoded decode(const std::vector<iclforge::ac4::EncodedFrame>& frames) {
    iclforge::ac4::DecoderConfig config;
    config.level = 7;
    iclforge::ac4::Decoder decoder(config);
    Decoded out;
    for (const iclforge::ac4::EncodedFrame& frame : frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const iclforge::ac4::DecodedFrame& pcm = **decoded;
        out.speakers = pcm.speakers;
        out.channels.resize(pcm.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(),
                                   pcm.channels[c].end());
        }
    }
    return out;
}

// Every substream of every frame reads to its end, and the decoder's trace is
// the encoder's, record for record.
void check_frames_read_back(const Encoded& encoded) {
    std::vector<iclforge::ac4::SyntaxRecord> read;
    const auto sink = [&read](const iclforge::ac4::SyntaxRecord& r) { read.push_back(r); };
    iclforge::ac4::DecoderConfig config;
    config.syntax = sink;
    iclforge::ac4::Decoder decoder(config);
    for (const iclforge::ac4::EncodedFrame& frame : encoded.frames) {
        const auto report = decoder.parse(frame.raw_ac4_frame);
        REQUIRE(report.has_value());
        for (const iclforge::ac4::SubstreamReport& substream : report->substreams) {
            CAPTURE(substream.index, substream.refused_reason);
            REQUIRE_FALSE(substream.refused.has_value());
            CHECK(substream.bits_read == substream.size_bits);
        }
    }
    REQUIRE(read.size() == encoded.trace.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < read.size(); ++i) {
        const bool same = read[i].substream == encoded.trace[i].substream &&
                          read[i].bit_offset == encoded.trace[i].bit_offset &&
                          read[i].bits == encoded.trace[i].bits &&
                          read[i].value == encoded.trace[i].value;
        if (!same && mismatches++ < 5) {
            CAPTURE(i, encoded.trace[i].name, read[i].name, encoded.trace[i].value, read[i].value);
            CHECK(same);
        }
    }
    CHECK(mismatches == 0);
}

std::size_t count_records(const Encoded& encoded, std::string_view name, std::uint64_t value) {
    return static_cast<std::size_t>(std::ranges::count_if(
        encoded.trace,
        [&](const iclforge::ac4::SyntaxRecord& r) { return r.name == name && r.value == value; }));
}

std::size_t count_named(const Encoded& encoded, std::string_view name) {
    return static_cast<std::size_t>(std::ranges::count_if(
        encoded.trace, [&](const iclforge::ac4::SyntaxRecord& r) { return r.name == name; }));
}

// The amplitude of x's component at `hz` over the decoded output after the
// lag and a frame's settling, through a Hann window, as a fraction of the
// source's tone amplitude.
double level(std::span<const float> x, double hz, double rate = 48000.0) {
    const std::size_t first = kLag + 4096;
    const std::size_t count = kSamples - 8192;
    double re = 0.0;
    double im = 0.0;
    double weight = 0.0;
    for (std::size_t n = 0; n < count && first + n < x.size(); ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                              static_cast<double>(count));
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(first + n) / rate;
        re += w * static_cast<double>(x[first + n]) * std::cos(phase);
        im -= w * static_cast<double>(x[first + n]) * std::sin(phase);
        weight += w;
    }
    return 2.0 * std::hypot(re, im) / weight / kAmplitude;
}

double db(double ratio) {
    return 20.0 * std::log10(std::max(ratio, 1e-30));
}

// Each channel's own tone within 0.2 dB of unity, and every other tone 60 dB
// under it there: planning/ac4.md, E3's exit, held to 22.2's 24 channels.
void check_routing(const std::vector<Channel>& channels, const Decoded& decoded,
                   double rate = 48000.0) {
    REQUIRE(decoded.speakers.size() == channels.size());
    for (std::size_t c = 0; c < channels.size(); ++c) {
        CHECK(decoded.speakers[c] == channels[c].speaker);
        CAPTURE(c, channels[c].hz);
        const double gain = level(decoded.channels[c], channels[c].hz, rate);
        CHECK(std::abs(db(gain)) < 0.2);
        for (std::size_t other = 0; other < channels.size(); ++other) {
            if (other != c) {
                CAPTURE(other, channels[other].hz);
                CHECK(db(gain / level(decoded.channels[c], channels[other].hz, rate)) > 60.0);
            }
        }
    }
}

// Gain (dB) and SNR (dB) of `decoded` against `source` delayed by `lag`, over
// the middle of the overlap.
struct Score {
    double gain_db = 0.0;
    double snr_db = 0.0;
};

Score score(std::span<const float> source, std::span<const float> decoded, std::size_t lag) {
    const std::size_t skip = 4096;
    const std::size_t count = std::min(source.size(), decoded.size() - lag);
    double sd = 0.0;
    double ss = 0.0;
    for (std::size_t n = skip; n + skip < count; ++n) {
        sd += static_cast<double>(source[n]) * static_cast<double>(decoded[n + lag]);
        ss += static_cast<double>(source[n]) * static_cast<double>(source[n]);
    }
    const double gain = sd / ss;
    double error = 0.0;
    for (std::size_t n = skip; n + skip < count; ++n) {
        const double e =
            static_cast<double>(decoded[n + lag]) - gain * static_cast<double>(source[n]);
        error += e * e;
    }
    return {20.0 * std::log10(std::abs(gain)), 10.0 * std::log10(gain * gain * ss / error)};
}

// Music-like noise: a spectrum that falls with frequency (three one-pole
// low-passes of equal power, weighted 1, 0.6 and 0.3, and a little white) under
// a swell at 3 Hz, from `seed`; an LFE's is four one-pole stages at 48 Hz,
// under the 120 Hz its track codes to.
std::vector<float> music(std::size_t count, std::uint32_t seed, bool lfe) {
    std::vector<float> x(count);
    const auto white = [&seed]() {
        seed = seed * 1664525U + 1013904223U;
        return static_cast<double>(seed >> 8U) / 16777216.0 - 0.5;
    };
    const std::array<double, 3> poles = {0.99, 0.9, 0.5};
    const std::array<double, 3> weights = {1.0, 0.6, 0.3};
    std::array<double, 3> state{};
    std::array<double, 4> stage{};
    const double lfe_pole = std::exp(-2.0 * std::numbers::pi * 48.0 / 48000.0);
    for (std::size_t n = 0; n < count; ++n) {
        const double w = white();
        double v = 0.1 * w;
        if (lfe) {
            double s = w;
            for (double& y : stage) {
                y = lfe_pole * y + (1.0 - lfe_pole) * s;
                s = y;
            }
            v = 60.0 * s;
        } else {
            for (std::size_t k = 0; k < poles.size(); ++k) {
                state[k] = poles[k] * state[k] + (1.0 - poles[k]) * w;
                v += weights[k] * state[k] / std::sqrt((1.0 - poles[k]) / (1.0 + poles[k]));
            }
        }
        const double swell =
            1.0 + 0.5 * std::sin(2.0 * std::numbers::pi * 3.0 * static_cast<double>(n) / 48000.0);
        x[n] = static_cast<float>(0.025 * swell * v);
    }
    return x;
}

// Table 21's two LFE tracks and eleven pairs, as Table A.27's indices of the
// channels each holds, in the syntax's order.
struct Track {
    std::string name;
    std::array<std::size_t, 2> channels;  // the one channel twice for an LFE
};

std::vector<Track> tracks() {
    return {{"LFE", {11, 11}},     {"LFE2", {17, 17}},    {"[L, R]", {0, 1}},
            {"[C, Tc]", {2, 16}},  {"[Ls, Rs]", {3, 4}},  {"[Lb, Rb]", {5, 6}},
            {"[Tfl, Tfr]", {7, 8}}, {"[Tbl, Tbr]", {9, 10}}, {"[Tsl, Tsr]", {12, 13}},
            {"[Tfc, Tbc]", {14, 15}}, {"[Bfl, Bfr]", {18, 19}}, {"[Bfc, Cb]", {20, 21}},
            {"[Lw, Rw]", {22, 23}}};
}

// Each pair's two channels share part of their power (a common signal and each
// its own), from none in [L, R] to a tenth more in each pair after it, as the two
// channels of a stereo pair of music do.
std::vector<std::vector<float>> correlated_music(std::size_t count) {
    std::vector<std::vector<float>> input(24);
    const std::vector<Track> all = tracks();
    for (std::size_t t = 2; t < all.size(); ++t) {
        const double share = static_cast<double>(t - 2) / 10.0;
        const std::vector<float> common = music(count, 5000U + static_cast<std::uint32_t>(t), false);
        for (const std::size_t c : all[t].channels) {
            const std::vector<float> own = music(count, 9000U + static_cast<std::uint32_t>(c), false);
            input[c].resize(count);
            for (std::size_t n = 0; n < count; ++n) {
                input[c][n] = static_cast<float>(std::sqrt(share) * static_cast<double>(common[n]) +
                                                 std::sqrt(1.0 - share) * static_cast<double>(own[n]));
            }
        }
    }
    for (const std::size_t lfe : {std::size_t{11}, std::size_t{17}}) {
        input[lfe] = music(count, 31U + static_cast<std::uint32_t>(lfe), true);
    }
    return input;
}

}  // namespace

TEST_CASE("22.2 in SIMPLE and ASPX puts each channel's tone on its own channel",
          "[ac4][encoder][twenty-two-two]") {
    // kAuto takes ASPX below 76.8 kbps a full-band channel, as the 5.X element
    // does, which over 22.2's 22 of them is 1 690 kbps, and SIMPLE from there.
    struct Case {
        int kbps;
        CodecMode asked;
        CodecMode mode;
        std::uint64_t code;  // 22_2_codec_mode
    };
    const std::vector<Case> cases = {
        {1536, CodecMode::kSimple, CodecMode::kSimple, 0},
        {1792, CodecMode::kAuto, CodecMode::kSimple, 0},
        {880, CodecMode::kAuto, CodecMode::kAspx, 1},
        {1408, CodecMode::kAspx, CodecMode::kAspx, 1},
    };
    for (const Case& c : cases) {
        if (kSanitized && c.kbps != 1536 && c.kbps != 880) {
            continue;
        }
        CAPTURE(c.kbps, static_cast<int>(c.asked));
        const std::vector<Channel> channels = layout();
        const Encoded encoded = encode(config_at(c.kbps, c.asked), tones(channels));
        CHECK(encoded.mode == c.mode);
        const std::size_t frames = encoded.frames.size();
        std::size_t iframes = 0;
        for (const iclforge::ac4::EncodedFrame& frame : encoded.frames) {
            iframes += frame.iframe ? 1U : 0U;
        }
        CHECK(count_records(encoded, "22_2_codec_mode", c.code) == frames);
        // Eleven two_channel_data() with stereo processing on, in every frame.
        CHECK(count_records(encoded, "b_enable_mdct_stereo_proc", 1) == 11 * frames);
        // aspx_config() in the I-frames of the ASPX mode alone.
        CHECK(count_named(encoded, "aspx_quant_mode_env") == (c.code != 0 ? iframes : 0U));
        // The element sends no companding_control() and no A-CPL data.
        CHECK(count_named(encoded, "b_compand_on") == 0);
        CHECK(count_named(encoded, "acpl_num_param_bands_id") == 0);
        check_frames_read_back(encoded);
        check_routing(channels, decode(encoded.frames));
    }
}

TEST_CASE("22.2 at 25 fps and at 44.1 kHz puts each channel's tone on its own channel",
          "[ac4][encoder][twenty-two-two]") {
    SECTION("25 fps in SIMPLE and 50 fps in ASPX") {
        for (const auto& [index, kbps, mode] :
             {std::tuple{2, 1536, CodecMode::kSimple}, std::tuple{7, 1408, CodecMode::kAspx}}) {
            CAPTURE(index, kbps);
            iclforge::ac4::EncoderConfig config = config_at(kbps, mode);
            config.frame_rate_index = index;
            const std::vector<Channel> channels = layout();
            const Encoded encoded = encode(config, tones(channels));
            check_frames_read_back(encoded);
            check_routing(channels, decode(encoded.frames));
        }
    }
    SECTION("44.1 kHz in SIMPLE and ASPX") {
        for (const auto& [kbps, mode] :
             {std::pair{1536, CodecMode::kSimple}, std::pair{1056, CodecMode::kAspx}}) {
            CAPTURE(kbps);
            iclforge::ac4::EncoderConfig config = config_at(kbps, mode);
            config.sample_rate_hz = 44100;
            const std::vector<Channel> channels = layout();
            const Encoded encoded = encode(config, tones(channels, 44100.0));
            check_frames_read_back(encoded);
            check_routing(channels, decode(encoded.frames), 44100.0);
        }
    }
}

TEST_CASE("22.2's table of contents, level and MP4 description", "[ac4][encoder][twenty-two-two]") {
    for (const auto& [kbps, mode] :
         {std::pair{1536, CodecMode::kSimple}, std::pair{880, CodecMode::kAspx}}) {
        CAPTURE(kbps);
        const iclforge::ac4::EncoderConfig config = config_at(kbps, mode);
        auto encoder = iclforge::ac4::Encoder::create(config);
        INFO(iclforge::ac4::Encoder::refusal_reason(config));
        REQUIRE(encoder.has_value());
        CHECK(encoder->codec_mode() == mode);
        const iclforge::ac4::Toc& toc = encoder->toc();
        const auto& chan = toc.substream_groups.at(0).substreams.at(0).chan;
        REQUIRE(chan.has_value());
        CHECK(chan->ch_mode == 15);
        // Table 56's code for 22.2 names none of the source's channels.
        CHECK_FALSE(chan->original_content.has_value());
        // 22 tracks, the LFEs not counted, are over every level but
        // unrestricted (Part 2 Table 55).
        REQUIRE(toc.presentations_v1.size() == 1);
        CHECK(toc.presentations_v1.front().md_compat == 7);
        CHECK(toc.presentations_v1.front().immersive_audio_indicator == true);
        // The MP4 description: a dac4 the carriage writer accepts, and CMAF's
        // rules hold.
        CHECK(iclforge::ac4::dac4_refusal(toc).empty());
        CHECK_FALSE(iclforge::ac4::build_dac4(toc).empty());
        CHECK(iclforge::ac4::cmaf_refusal(toc).empty());
        CHECK(iclforge::ac4::rfc6381_codec_string(toc) == "ac-4.02.01.07");
    }
}

TEST_CASE("a decoder at level 3 does not select a 22.2 presentation, and one at level 7 does",
          "[ac4][encoder][twenty-two-two]") {
    const std::vector<Channel> channels = layout();
    const Encoded encoded = encode(config_at(880, CodecMode::kAspx), tones(channels));
    REQUIRE_FALSE(encoded.frames.empty());
    iclforge::ac4::Decoder level_three;
    const auto refused = level_three.decode(encoded.frames.front().raw_ac4_frame);
    CHECK_FALSE(refused.has_value());
    CHECK(level_three.refusal_reason().find("select") != std::string_view::npos);
    iclforge::ac4::DecoderConfig config;
    config.level = 7;
    iclforge::ac4::Decoder level_seven(config);
    const auto decoded = level_seven.decode(encoded.frames.front().raw_ac4_frame);
    REQUIRE(decoded.has_value());
}

TEST_CASE("22.2 decodes with an SNR floor on each pair of Table 21",
          "[ac4][encoder][twenty-two-two]") {
    // Music-like noise, each pair's two channels sharing part of their power
    // (none in [L, R], a tenth more in each pair after it, all of it in [Lw,
    // Rw]), and each LFE its own low-passed noise. The rate loop holds each
    // band's noise to its masking threshold, not to an SNR, so a channel of
    // this noise lands near 12 dB in SIMPLE at 70 kbps a channel and near 8 dB
    // in ASPX at 40: where the 5.1 path lands on the same noise (350 kbps
    // SIMPLE: 11.8 to 12.9 dB). Each floor is the lower of its pair's two
    // channels as measured, less a dB, to a whole dB.
    struct Floors {
        std::array<double, 13> simple;  // Track order: LFE, LFE2, then the pairs
        std::array<double, 13> aspx;
    };
    const Floors floors = {
        .simple = {32.0, 32.0, 11.0, 11.0, 11.0, 11.0, 11.0, 12.0, 11.0, 11.0, 11.0, 11.0, 12.0},
        .aspx = {25.0, 26.0, 6.0, 7.0, 7.0, 7.0, 6.0, 7.0, 7.0, 7.0, 6.0, 7.0, 8.0},
    };
    const std::size_t count = kSanitized ? 24000 : 72000;
    const std::vector<std::vector<float>> input = correlated_music(count);
    const std::vector<Track> all = tracks();
    for (const auto& [kbps, mode] :
         {std::pair{1536, CodecMode::kSimple}, std::pair{880, CodecMode::kAspx}}) {
        CAPTURE(kbps);
        const Encoded encoded = encode(config_at(kbps, mode), input);
        check_frames_read_back(encoded);
        const Decoded full = decode(encoded.frames);
        REQUIRE(full.channels.size() == 24);
        const std::array<double, 13>& floor =
            mode == CodecMode::kSimple ? floors.simple : floors.aspx;
        for (std::size_t t = 0; t < all.size(); ++t) {
            CAPTURE(all[t].name);
            for (const std::size_t c : all[t].channels) {
                const Score s = score(input[c], full.channels[c], kLag);
                CAPTURE(c, s.snr_db, s.gain_db);
                CHECK(s.snr_db > floor[t]);
                CHECK(std::abs(s.gain_db) < 1.5);
            }
        }
    }
}

TEST_CASE("the rate must hold 22.2's least frame, and refusal_reason names a rate that does not",
          "[ac4][encoder][twenty-two-two]") {
    // A silent I-frame of the element: sf_info() and chparam_info() for each
    // of eleven pairs and the two LFEs' max_sfb, and in ASPX eleven
    // aspx_data_2ch(), which is what sets the floors: at the native frame rate
    // 17 kbps in SIMPLE and 49 in ASPX, at 120 fps 65 and 224.
    struct Case {
        CodecMode mode;
        int frame_rate_index;
        int floor_kbps;
    };
    const std::vector<Case> cases = {
        {CodecMode::kSimple, 13, 17}, {CodecMode::kAspx, 13, 49},
        {CodecMode::kSimple, 2, 18},  {CodecMode::kAspx, 2, 52},
        {CodecMode::kSimple, 12, 65}, {CodecMode::kAspx, 12, 224},
    };
    for (const Case& c : cases) {
        CAPTURE(static_cast<int>(c.mode), c.frame_rate_index, c.floor_kbps);
        iclforge::ac4::EncoderConfig config = config_at(c.floor_kbps, c.mode);
        config.frame_rate_index = c.frame_rate_index;
        CHECK(iclforge::ac4::Encoder::create(config).has_value());
        config.bitrate_kbps = c.floor_kbps - 1;
        CHECK_FALSE(iclforge::ac4::Encoder::create(config).has_value());
        CHECK(iclforge::ac4::Encoder::refusal_reason(config) ==
              "a rate that cannot hold a substream's least frame");
    }
}

TEST_CASE("the encoder refuses the 22.2 configurations it does not write",
          "[ac4][encoder][twenty-two-two]") {
    using iclforge::ac4::EncoderConfig;
    struct Case {
        const char* name;
        EncoderConfig config;
        std::string_view says;
    };
    EncoderConfig plain = config_at(1536);
    plain.experimental.twenty_two_two = false;
    EncoderConfig without_flag_aspx = plain;
    without_flag_aspx.codec_mode = CodecMode::kAspx;
    EncoderConfig six = config_at(384);
    six.channels = 6;
    EncoderConfig two = config_at(128);
    two.channels = 2;
    EncoderConfig ten = config_at(512);
    ten.channels = 10;
    EncoderConfig a_cpl_2 = config_at(1536, CodecMode::kAspxAcpl2);
    EncoderConfig a_cpl_1 = config_at(1536, CodecMode::kAspxAcpl1);
    a_cpl_1.experimental.acpl = true;
    EncoderConfig a_cpl_3 = config_at(1536, CodecMode::kAspxAcpl3);
    EncoderConfig scpl = config_at(1536, CodecMode::kScpl);
    EncoderConfig aspx_scpl = config_at(1536, CodecMode::kAspxScpl);
    EncoderConfig ajcc = config_at(1536, CodecMode::kAspxAjcc);
    ajcc.experimental.ajcc = true;
    EncoderConfig sample_rate = config_at(1536);
    sample_rate.sample_rate_hz = 32000;
    EncoderConfig frame_rate = config_at(1536);
    frame_rate.sample_rate_hz = 44100;
    frame_rate.frame_rate_index = 2;
    EncoderConfig reserved_rate = config_at(1536);
    reserved_rate.frame_rate_index = 14;
    EncoderConfig too_much = config_at(3001);
    EncoderConfig too_little = config_at(16, CodecMode::kSimple);
    EncoderConfig configs = config_at(1536);
    configs.experimental.coding_configs = true;
    EncoderConfig dialogue = config_at(1536);
    dialogue.dialogue = iclforge::ac4::DialogueConfig{};
    EncoderConfig downmix = config_at(1536);
    downmix.downmix = iclforge::ac4::DownmixConfig{};
    EncoderConfig own_downmix = config_at(1536);
    own_downmix.presentations = {
        {.substreams = {0}, .downmix = iclforge::ac4::DownmixConfig{}}};
    EncoderConfig gains = config_at(1536);
    gains.drc = iclforge::ac4::DrcConfig{.modes = {{.id = 0, .gains_config = 1}}};
    gains.experimental.drc_gains = true;
    const std::vector<Case> cases = {
        {"24 channels without the option", plain, "experimental.twenty_two_two"},
        {"24 channels in ASPX without the option", without_flag_aspx, "experimental.twenty_two_two"},
        {"the option with 6 channels", six, "without 24 channels"},
        {"the option with 2 channels", two, "without 24 channels"},
        {"the option with 10 channels", ten, "without 24 channels"},
        {"ASPX_ACPL_2", a_cpl_2, "22.2 element does not take"},
        {"ASPX_ACPL_1 with experimental.acpl", a_cpl_1, "22.2 element does not take"},
        {"ASPX_ACPL_3", a_cpl_3, "22.2 element does not take"},
        {"SCPL", scpl, "22.2 element does not take"},
        {"ASPX_SCPL", aspx_scpl, "22.2 element does not take"},
        {"ASPX_AJCC", ajcc, "22.2 element does not take"},
        {"a sample rate of 32 kHz", sample_rate, "sample rate"},
        {"a frame rate at 44.1 kHz", frame_rate, "frame_rate_index"},
        {"a reserved frame rate", reserved_rate, "frame_rate_index"},
        {"a rate over 3 000 kbps", too_much, "8 to 3 000 kbps"},
        {"a rate under the least frame", too_little, "least frame"},
        {"the 5.X coding configurations", configs, "coding_configs"},
        {"dialogue enhancement", dialogue, "dialogue enhancement in a 22.2 substream"},
        {"the stream's downmix values", downmix, "no 5.X or 7.X presentation"},
        {"a presentation's own downmix values", own_downmix, "downmix values for a 22.2"},
        {"DRC gains", gains, "DRC gains for a 22.2"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        CHECK_FALSE(iclforge::ac4::Encoder::create(c.config).has_value());
        const std::string_view reason = iclforge::ac4::Encoder::refusal_reason(c.config);
        CAPTURE(reason);
        CHECK(reason.find(c.says) != std::string_view::npos);
    }
}

