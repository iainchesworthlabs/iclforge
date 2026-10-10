// iclforge::ac4::Encoder's 9.X.4 element end to end (planning/ac4.md, "Never written" list of
// phase E8): 9.0.4 and 9.1.4, thirteen and fourteen input channels, in the immersive element of
// ETSI TS 103 190-2 V1.3.1 clause 6.2.4.1 with b_5fronts 1, which adds the screen pair Lscr and
// Rscr to the 7.X.4 channels (experimental.nine_x_4). Each stream reads back with the trace the
// encoder recorded and decodes, in full decoding, with each channel's tone on its own channel at
// unity; the decoder's renderer then folds it to 7.X.4 and 5.X by Tables 38 to 43's 9.X rows. The
// tests are tagged [nine-x-four]. The signals are half a second long, and a third of a second
// under the sanitizers (tests/support/sanitized.hpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4/io/carriage.hpp"
#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/core/syntax.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/encoder/encoder.hpp"
#include "sanitized.hpp"

namespace {

using iclforge::test::kSanitized;
using iclforge::ac4::CodecMode;
using iclforge::ac4::Speaker;

// The decoder's delay at frame_rate_index 13 (d_pcm, the QMF banks' 577 samples and six QMF
// slots) and the encoder's, a frame and a half.
constexpr std::size_t kLag = 3072 + 352 + 577 + 6 * 64;
constexpr std::size_t kSamples = kSanitized ? 16384 : 24000;
constexpr double kAmplitude = 0.1;  // -20 dBFS

struct Channel {
    Speaker speaker;
    double hz;
};

// The 9.X.4 channels in the decoder's order, which is the encoder's input order (Part 2 Table
// A.27): L R C Ls Rs Lb Rb Tfl Tfr Tbl Tbr, the LFE of 9.1.4, then Lscr and Rscr. `waveform` tones
// are 150 Hz or more apart, for the modes that code the waveform; the others sit mid-subband
// (375 Hz wide at 48 kHz), the two of an A-CPL module's pair, (Ls, Lb), (Rs, Rb), (Tfl, Tbl),
// (Tfr, Tbr), (L, Lscr) and (R, Rscr), in different parameter bands (Part 1 Table 197: subbands
// 0 to 8 a band each, 9 and 10 one, 11 to 13 one and 14 to 17 one), which is where A-CPL parts
// them; C is coded as it is, above everything else.
std::vector<Channel> layout(bool lfe, bool waveform) {
    std::vector<Channel> out;
    if (waveform) {
        out = {{Speaker::kLeft, 331.0},         {Speaker::kRight, 457.0},
               {Speaker::kCentre, 613.0},       {Speaker::kLeftSurround, 787.0},
               {Speaker::kRightSurround, 953.0}, {Speaker::kLeftBack, 1777.0},
               {Speaker::kRightBack, 1931.0},   {Speaker::kTopFrontLeft, 1117.0},
               {Speaker::kTopFrontRight, 1289.0}, {Speaker::kTopBackLeft, 1453.0},
               {Speaker::kTopBackRight, 1621.0}};
    } else {
        out = {{Speaker::kLeft, 560.0},          {Speaker::kRight, 4300.0},
               {Speaker::kCentre, 4500.0},       {Speaker::kLeftSurround, 940.0},
               {Speaker::kRightSurround, 2440.0}, {Speaker::kLeftBack, 2810.0},
               {Speaker::kRightBack, 3750.0},    {Speaker::kTopFrontLeft, 1310.0},
               {Speaker::kTopFrontRight, 1690.0}, {Speaker::kTopBackLeft, 2060.0},
               {Speaker::kTopBackRight, 3190.0}};
    }
    if (lfe) {
        out.push_back({Speaker::kLfe, 47.0});
    }
    if (waveform) {
        out.insert(out.end(), {{Speaker::kLeftScreen, 2161.0}, {Speaker::kRightScreen, 2347.0}});
    } else {
        out.insert(out.end(), {{Speaker::kLeftScreen, 190.0}, {Speaker::kRightScreen, 5500.0}});
    }
    return out;
}

std::vector<float> tone(double hz, std::size_t count) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(
            kAmplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0));
    }
    return x;
}

std::vector<std::vector<float>> tones(const std::vector<Channel>& channels) {
    std::vector<std::vector<float>> input;
    for (const Channel& c : channels) {
        input.push_back(tone(c.hz, kSamples));
    }
    return input;
}

struct Encoded {
    std::vector<iclforge::ac4::EncodedFrame> frames;
    std::vector<iclforge::ac4::SyntaxRecord> trace;
    iclforge::ac4::CodecMode mode = CodecMode::kAuto;
    iclforge::ac4::Toc toc;
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
    out.toc = encoder->toc();
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

Decoded decode(const std::vector<iclforge::ac4::EncodedFrame>& frames,
               iclforge::ac4::DecodingMode mode = iclforge::ac4::DecodingMode::kFull,
               iclforge::ac4::DownmixTarget target = iclforge::ac4::DownmixTarget::kAsCoded) {
    iclforge::ac4::DecoderConfig config;
    config.decoding = mode;
    config.output.downmix = target;
    // Thirteen tracks are above Table 55's level 3, the decoder's default: md_compat 7,
    // "unrestricted", which a decoder takes only when told its level is 7 (libs/ac4/ERRATA.md,
    // "Which presentations can be selected").
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

// Every substream of every frame reads to its end, and the decoder's trace is the encoder's,
// record for record.
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

std::size_t count_records(const Encoded& encoded, std::string_view name) {
    return static_cast<std::size_t>(std::ranges::count_if(
        encoded.trace, [&](const iclforge::ac4::SyntaxRecord& r) { return r.name == name; }));
}

std::size_t count_records(const Encoded& encoded, std::string_view name, std::uint64_t value) {
    return static_cast<std::size_t>(std::ranges::count_if(
        encoded.trace,
        [&](const iclforge::ac4::SyntaxRecord& r) { return r.name == name && r.value == value; }));
}

// The amplitude of x's component at `hz` over the decoded output after the lag and a frame's
// settling, through a Hann window, as a fraction of the source's tone amplitude.
double level(std::span<const float> x, double hz) {
    const std::size_t first = kLag + 4096;
    const std::size_t count = kSamples - 8192;
    double re = 0.0;
    double im = 0.0;
    double weight = 0.0;
    for (std::size_t n = 0; n < count && first + n < x.size(); ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                              static_cast<double>(count));
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(first + n) / 48000.0;
        re += w * static_cast<double>(x[first + n]) * std::cos(phase);
        im -= w * static_cast<double>(x[first + n]) * std::sin(phase);
        weight += w;
    }
    return 2.0 * std::hypot(re, im) / weight / kAmplitude;
}

double db(double ratio) {
    return 20.0 * std::log10(std::max(ratio, 1e-30));
}

std::size_t index_of(const Decoded& d, Speaker speaker) {
    const auto it = std::ranges::find(d.speakers, speaker);
    REQUIRE(it != d.speakers.end());
    return static_cast<std::size_t>(it - d.speakers.begin());
}

// Each channel's own tone within 0.2 dB of unity and every other tone 60 dB under it there:
// planning/ac4.md, E3's exit, held to the 9.X.4 layouts. `skip` names channels held to other
// checks.
void check_routing(const std::vector<Channel>& channels, const Decoded& decoded,
                   std::span<const Speaker> skip = {}) {
    REQUIRE(decoded.speakers.size() == channels.size());
    for (std::size_t c = 0; c < channels.size(); ++c) {
        CHECK(decoded.speakers[c] == channels[c].speaker);  // Table A.27's order
    }
    for (const Channel& own : channels) {
        if (std::ranges::find(skip, own.speaker) != skip.end()) {
            continue;
        }
        const std::size_t c = index_of(decoded, own.speaker);
        CAPTURE(own.hz);
        const double gain = level(decoded.channels[c], own.hz);
        CHECK(std::abs(db(gain)) < 0.2);
        for (const Channel& other : channels) {
            if (other.speaker != own.speaker) {
                CAPTURE(other.hz);
                CHECK(db(gain / level(decoded.channels[c], other.hz)) > 60.0);
            }
        }
    }
}

struct Mode {
    int kbps;
    CodecMode mode;
    std::uint64_t code;  // Table 73's immersive_codec_mode
};

constexpr std::array<Mode, 3> kModes = {Mode{1024, CodecMode::kScpl, 0},
                                        Mode{800, CodecMode::kAspxScpl, 1},
                                        Mode{448, CodecMode::kAspxAcpl2, 3}};

}  // namespace

TEST_CASE("9.1.4 and 9.0.4 in SCPL, ASPX_SCPL and ASPX_ACPL_2 put each tone on its own channel",
          "[ac4][encoder][immersive][nine-x-four]") {
    for (const bool lfe : {true, false}) {
        for (const Mode m : kModes) {
            const bool parametric = m.mode == CodecMode::kAspxAcpl2;
            CAPTURE(lfe, m.kbps);
            const std::vector<Channel> channels = layout(lfe, !parametric);
            const Encoded encoded =
                encode({.channels = lfe ? 14 : 13,
                        .bitrate_kbps = m.kbps,
                        .codec_mode = m.mode,
                        .experimental = {.nine_x_4 = true}},
                       tones(channels));
            CHECK(encoded.mode == m.mode);
            const std::size_t frames = encoded.frames.size();
            REQUIRE(frames > 0);
            CHECK(count_records(encoded, "immersive_codec_mode_code", m.code) == frames);
            // DEE's form of the 7.X.4 element, with the 5-fronts data after it.
            CHECK(count_records(encoded, "core_5ch_grouping", 0) == frames);
            CHECK(count_records(encoded, "2ch_mode", 0) == frames);
            CHECK(count_records(encoded, "b_use_sap_add_ch", 0) == frames);
            if (m.mode == CodecMode::kAspxAcpl2) {
                // Six acpl_data_1ch(), Pseudocode 2's four and the two on (L, Lscr) and
                // (R, Rscr).
                CHECK(count_records(encoded, "acpl_interpolation_type") == 6 * frames);
            } else {
                // The pairs (A'', B''), (D'', E''), (F'', G''), (H'', I''), (J'', K'') and
                // (L'', M'') and Table 20's six, a'_0 to a'_5.
                CHECK(count_records(encoded, "sap_mode") == 12 * frames);
            }
            if (m.mode == CodecMode::kAspxScpl) {
                // Table 8 with b_5fronts: six aspx_data_2ch() and the one aspx_data_1ch(),
                // where the 7.X.4 element has five and the one.
                CHECK(count_records(encoded, "aspx_balance") == 6 * frames);
            }
            check_frames_read_back(encoded);
            const Decoded full = decode(encoded.frames);
            if (parametric) {
                // A-CPL makes each pair of its sum, which it keeps exactly, and puts each
                // tone on its own channel of the pair, band by band.
                const std::array<std::pair<Speaker, Speaker>, 6> pairs = {
                    std::pair{Speaker::kLeftSurround, Speaker::kLeftBack},
                    std::pair{Speaker::kRightSurround, Speaker::kRightBack},
                    std::pair{Speaker::kTopFrontLeft, Speaker::kTopBackLeft},
                    std::pair{Speaker::kTopFrontRight, Speaker::kTopBackRight},
                    std::pair{Speaker::kLeft, Speaker::kLeftScreen},
                    std::pair{Speaker::kRight, Speaker::kRightScreen}};
                std::vector<Speaker> paired;
                for (const auto& [a, b] : pairs) {
                    paired.push_back(a);
                    paired.push_back(b);
                }
                check_routing(channels, full, paired);
                for (const auto& [a, b] : pairs) {
                    const auto hz_of = [&](Speaker s) {
                        const auto it = std::ranges::find_if(
                            channels, [&](const Channel& c) { return c.speaker == s; });
                        REQUIRE(it != channels.end());
                        return it->hz;
                    };
                    std::vector<float> sum = full.channels[index_of(full, a)];
                    const std::vector<float>& second = full.channels[index_of(full, b)];
                    for (std::size_t n = 0; n < sum.size(); ++n) {
                        sum[n] += second[n];
                    }
                    for (const Speaker s : {a, b}) {
                        const double hz = hz_of(s);
                        const Speaker other = s == a ? b : a;
                        CAPTURE(hz);
                        CHECK(std::abs(db(level(sum, hz))) < 0.2);
                        const auto own = full.channels[index_of(full, s)];
                        CHECK(std::abs(db(level(own, hz))) < 0.5);
                        CHECK(db(level(own, hz) / level(full.channels[index_of(full, other)], hz)) >
                              20.0);
                    }
                }
            } else {
                check_routing(channels, full);
            }
        }
    }
}

TEST_CASE("9.1.4 and 9.0.4 in ASPX_ACPL_1, experimental, put each tone on its own channel",
          "[ac4][encoder][immersive][nine-x-four]") {
    // The residuals below acpl_qmf_band, 3 kHz, above every tone: the pairs come back as simple
    // coupling would make them, the six modules' (L'', M'') among them.
    for (const bool lfe : {true, false}) {
        CAPTURE(lfe);
        const std::vector<Channel> channels = layout(lfe, true);
        const Encoded encoded = encode({.channels = lfe ? 14 : 13,
                                        .bitrate_kbps = 640,
                                        .codec_mode = CodecMode::kAspxAcpl1,
                                        .experimental = {.acpl = true, .nine_x_4 = true}},
                                       tones(channels));
        CHECK(encoded.mode == CodecMode::kAspxAcpl1);
        CHECK(count_records(encoded, "immersive_codec_mode_code", 2) == encoded.frames.size());
        CHECK(count_records(encoded, "acpl_interpolation_type") == 6 * encoded.frames.size());
        check_frames_read_back(encoded);
        check_routing(channels, decode(encoded.frames));
    }
}

TEST_CASE("kAuto takes SCPL, ASPX_SCPL and ASPX_ACPL_2 for 9.X.4 by the rate a channel",
          "[ac4][encoder][immersive][nine-x-four]") {
    // The rates the 5.1.4 layout changes mode at, a full-band channel (the LFE not counted): below
    // 480 / 9 kbps ASPX_ACPL_2, below 640 / 9 ASPX_SCPL, SCPL above, over thirteen of them.
    struct Case {
        int channels;
        int kbps;
        CodecMode mode;
    };
    for (const Case c : {Case{14, 512, CodecMode::kAspxAcpl2}, Case{13, 512, CodecMode::kAspxAcpl2},
                         Case{14, 800, CodecMode::kAspxScpl}, Case{13, 800, CodecMode::kAspxScpl},
                         Case{14, 1100, CodecMode::kScpl}, Case{13, 1100, CodecMode::kScpl}}) {
        CAPTURE(c.channels, c.kbps);
        const iclforge::ac4::EncoderConfig config{.channels = c.channels,
                                                  .bitrate_kbps = c.kbps,
                                                  .experimental = {.nine_x_4 = true}};
        auto encoder = iclforge::ac4::Encoder::create(config);
        INFO(iclforge::ac4::Encoder::refusal_reason(config));
        REQUIRE(encoder.has_value());
        CHECK(encoder->codec_mode() == c.mode);
    }
}

TEST_CASE("the decoder's renderer folds 9.1.4 to 7.X.4 and 5.X by Tables 38 to 43's 9.X rows",
          "[ac4][encoder][immersive][nine-x-four]") {
    const std::vector<Channel> channels = layout(true, true);
    const Encoded encoded = encode({.channels = 14,
                                    .bitrate_kbps = 1024,
                                    .codec_mode = CodecMode::kScpl,
                                    .experimental = {.nine_x_4 = true}},
                                   tones(channels));
    check_frames_read_back(encoded);
    struct Part {
        Speaker source;
        double gain_db;
    };
    // The output speaker `speaker` holds exactly the tones of `parts`, each at its gain within 0.3
    // dB, and every other source's tone at least 50 dB under unity.
    const auto expect = [&](const Decoded& rendered, Speaker speaker, std::vector<Part> parts) {
        const std::size_t c = index_of(rendered, speaker);
        for (const Channel& source : channels) {
            const auto part = std::ranges::find_if(
                parts, [&](const Part& p) { return p.source == source.speaker; });
            const double measured = db(level(rendered.channels[c], source.hz));
            CAPTURE(source.hz, measured);
            if (part != parts.end()) {
                CHECK(std::abs(measured - part->gain_db) < 0.3);
            } else {
                CHECK(measured < -50.0);
            }
        }
    };
    constexpr double kM3 = -3.0103;
    SECTION("7.X.4: the screen pair adds to L and R at 0 dB, Table 38's r0,22 and r1,23") {
        const Decoded out = decode(encoded.frames, iclforge::ac4::DecodingMode::kFull,
                                   iclforge::ac4::DownmixTarget::k7X4);
        REQUIRE(out.speakers.size() == 12);
        expect(out, Speaker::kLeft, {{Speaker::kLeft, 0.0}, {Speaker::kLeftScreen, 0.0}});
        expect(out, Speaker::kRight, {{Speaker::kRight, 0.0}, {Speaker::kRightScreen, 0.0}});
        for (const Speaker s :
             {Speaker::kCentre, Speaker::kLfe, Speaker::kLeftSurround, Speaker::kRightSurround,
              Speaker::kLeftBack, Speaker::kRightBack, Speaker::kTopFrontLeft,
              Speaker::kTopFrontRight, Speaker::kTopBackLeft, Speaker::kTopBackRight}) {
            expect(out, s, {{s, 0.0}});
        }
    }
    SECTION("5.X.4: the backs fold into the surrounds, both at -3 dB, and the tops stay") {
        const Decoded out = decode(encoded.frames, iclforge::ac4::DecodingMode::kFull,
                                   iclforge::ac4::DownmixTarget::k5X4);
        REQUIRE(out.speakers.size() == 10);
        expect(out, Speaker::kLeft, {{Speaker::kLeft, 0.0}, {Speaker::kLeftScreen, 0.0}});
        expect(out, Speaker::kRight, {{Speaker::kRight, 0.0}, {Speaker::kRightScreen, 0.0}});
        // Tables 41 and 42 print Ls' = gain_b (Ls + Lb): the surround takes the same gain.
        expect(out, Speaker::kLeftSurround,
               {{Speaker::kLeftSurround, kM3}, {Speaker::kLeftBack, kM3}});
        expect(out, Speaker::kRightSurround,
               {{Speaker::kRightSurround, kM3}, {Speaker::kRightBack, kM3}});
        for (const Speaker s : {Speaker::kCentre, Speaker::kLfe, Speaker::kTopFrontLeft,
                                Speaker::kTopFrontRight, Speaker::kTopBackLeft,
                                Speaker::kTopBackRight}) {
            expect(out, s, {{s, 0.0}});
        }
    }
    SECTION("5.X: the backs and the top channels fold into the surrounds at -3 dB") {
        const Decoded out = decode(encoded.frames, iclforge::ac4::DecodingMode::kFull,
                                   iclforge::ac4::DownmixTarget::k5X);
        REQUIRE(out.speakers.size() == 6);
        expect(out, Speaker::kLeft, {{Speaker::kLeft, 0.0}, {Speaker::kLeftScreen, 0.0}});
        expect(out, Speaker::kRight, {{Speaker::kRight, 0.0}, {Speaker::kRightScreen, 0.0}});
        expect(out, Speaker::kLeftSurround,
               {{Speaker::kLeftSurround, kM3},
                {Speaker::kLeftBack, kM3},
                {Speaker::kTopFrontLeft, kM3},
                {Speaker::kTopBackLeft, kM3}});
        expect(out, Speaker::kRightSurround,
               {{Speaker::kRightSurround, kM3},
                {Speaker::kRightBack, kM3},
                {Speaker::kTopFrontRight, kM3},
                {Speaker::kTopBackRight, kM3}});
        for (const Speaker s : {Speaker::kCentre, Speaker::kLfe}) {
            expect(out, s, {{s, 0.0}});
        }
    }
}

TEST_CASE("the 9.X.4 layouts' table of contents and MP4 description",
          "[ac4][encoder][immersive][nine-x-four]") {
    struct Case {
        int channels;
        int kbps;
        int ch_mode;
        CodecMode mode;
    };
    for (const Case c : {Case{14, 800, 14, CodecMode::kAspxScpl},
                         Case{13, 800, 13, CodecMode::kAspxScpl},
                         Case{14, 512, 14, CodecMode::kAspxAcpl2},
                         Case{13, 1100, 13, CodecMode::kScpl}}) {
        CAPTURE(c.channels, c.kbps);
        const iclforge::ac4::EncoderConfig config{.channels = c.channels,
                                                  .bitrate_kbps = c.kbps,
                                                  .experimental = {.nine_x_4 = true}};
        auto encoder = iclforge::ac4::Encoder::create(config);
        INFO(iclforge::ac4::Encoder::refusal_reason(config));
        REQUIRE(encoder.has_value());
        CHECK(encoder->codec_mode() == c.mode);
        const iclforge::ac4::Toc& toc = encoder->toc();
        const auto& chan = toc.substream_groups.at(0).substreams.at(0).chan;
        REQUIRE(chan.has_value());
        CHECK(chan->ch_mode == c.ch_mode);
        // Part 2 Tables 57 to 59: a 9.X.4 source has the back pair, the centre and both top
        // pairs.
        REQUIRE(chan->original_content.has_value());
        CHECK(chan->original_content->b_4_back_channels_present);
        CHECK(chan->original_content->b_centre_present);
        CHECK(chan->original_content->top_channels_present == 3);
        // md_compat counts the tracks but the LFE (Part 2 Table 55): thirteen, so unrestricted.
        REQUIRE(toc.presentations_v1.size() == 1);
        CHECK(toc.presentations_v1.front().md_compat == 7);
        CHECK(toc.presentations_v1.front().immersive_audio_indicator == true);
        // The MP4 sample entry's dac4 and the CMAF rules take it whole: Annex E.10's
        // ac4_presentation_v1_dsi() and E.11's group DSI hold channel modes 13 and 14.
        CHECK_FALSE(iclforge::ac4::build_dac4(toc).empty());
        CHECK(iclforge::ac4::dac4_refusal(toc).empty());
        CHECK(iclforge::ac4::cmaf_refusal(toc).empty());
    }
}

TEST_CASE("the encoder refuses the 9.X.4 configurations it does not write",
          "[ac4][encoder][immersive][nine-x-four]") {
    struct Case {
        const char* name;
        iclforge::ac4::EncoderConfig config;
        std::string_view says;
    };
    const std::vector<Case> cases = {
        {"fourteen channels without the option",
         {.channels = 14, .bitrate_kbps = 800},
         "experimental.nine_x_4"},
        {"thirteen channels without the option",
         {.channels = 13, .bitrate_kbps = 800},
         "experimental.nine_x_4"},
        {"nine_x_4 for twelve channels",
         {.channels = 12,
          .bitrate_kbps = 768,
          .experimental = {.back_pair = true, .nine_x_4 = true}},
         "experimental.nine_x_4 without thirteen or fourteen channels"},
        {"nine_x_4 for 5.1",
         {.channels = 6, .bitrate_kbps = 384, .experimental = {.nine_x_4 = true}},
         "experimental.nine_x_4 without thirteen or fourteen channels"},
        {"nine_x_4 for stereo",
         {.channels = 2, .bitrate_kbps = 128, .experimental = {.nine_x_4 = true}},
         "experimental.nine_x_4 without thirteen or fourteen channels"},
        {"ASPX_AJCC for 9.1.4",
         {.channels = 14,
          .bitrate_kbps = 512,
          .codec_mode = CodecMode::kAspxAjcc,
          .experimental = {.ajcc = true, .nine_x_4 = true}},
         "ASPX_AJCC for 9.0.4 or 9.1.4"},
        {"ASPX_ACPL_1 for 9.1.4 without experimental.acpl",
         {.channels = 14,
          .bitrate_kbps = 512,
          .codec_mode = CodecMode::kAspxAcpl1,
          .experimental = {.nine_x_4 = true}},
         "experimental.acpl"},
        {"ASPX_ACPL_3 for 9.0.4",
         {.channels = 13,
          .bitrate_kbps = 512,
          .codec_mode = CodecMode::kAspxAcpl3,
          .experimental = {.nine_x_4 = true}},
         "immersive layouts do not take"},
        {"ASPX for 9.0.4",
         {.channels = 13,
          .bitrate_kbps = 512,
          .codec_mode = CodecMode::kAspx,
          .experimental = {.nine_x_4 = true}},
         "immersive layouts do not take"},
        {"dialogue enhancement for 9.1.4",
         {.channels = 14,
          .bitrate_kbps = 800,
          .dialogue = iclforge::ac4::DialogueConfig{.left = true, .right = true, .centre = true},
          .experimental = {.nine_x_4 = true}},
         "dialogue enhancement for 9.0.4 or 9.1.4"},
        {"a height downmix for 9.1.4",
         {.channels = 14,
          .bitrate_kbps = 800,
          .downmix = iclforge::ac4::DownmixConfig{.height = iclforge::ac4::HeightDownmix::kFront},
          .experimental = {.nine_x_4 = true}},
         "height downmix"},
        {"a sample rate other than 48 or 44.1 kHz",
         {.channels = 14,
          .sample_rate_hz = 32000,
          .bitrate_kbps = 800,
          .experimental = {.nine_x_4 = true}},
         "sample rate"},
        {"44.1 kHz at a frame rate Table 83 does not give there",
         {.channels = 14,
          .sample_rate_hz = 44100,
          .frame_rate_index = 2,
          .bitrate_kbps = 800,
          .experimental = {.nine_x_4 = true}},
         "frame_rate_index"},
        {"a frame_rate_index above 13",
         {.channels = 14,
          .frame_rate_index = 14,
          .bitrate_kbps = 800,
          .experimental = {.nine_x_4 = true}},
         "frame_rate_index"},
        {"the coding configurations for 9.1.4",
         {.channels = 14,
          .bitrate_kbps = 800,
          .experimental = {.coding_configs = true, .nine_x_4 = true}},
         "coding_configs"},
        {"DRC gains per group for 9.1.4",
         {.channels = 14,
          .bitrate_kbps = 800,
          .drc = iclforge::ac4::DrcConfig{.modes = {{.id = 0, .gains_config = 1}}},
          .experimental = {.nine_x_4 = true, .drc_gains = true}},
         "immersive presentation"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        CHECK_FALSE(iclforge::ac4::Encoder::create(c.config).has_value());
        const std::string_view reason = iclforge::ac4::Encoder::refusal_reason(c.config);
        CAPTURE(reason);
        CHECK(reason.find(c.says) != std::string_view::npos);
    }
    // And what it takes besides 48 kHz at the 2 048-sample frame: 44.1 kHz at frame_rate_index 13,
    // and 48 kHz at the other rates, as the 5.1.4 layout does.
    for (const iclforge::ac4::EncoderConfig& config :
         {iclforge::ac4::EncoderConfig{.channels = 14,
                                       .sample_rate_hz = 44100,
                                       .frame_rate_index = 13,
                                       .bitrate_kbps = 800,
                                       .experimental = {.nine_x_4 = true}},
          iclforge::ac4::EncoderConfig{.channels = 13,
                                       .frame_rate_index = 2,
                                       .bitrate_kbps = 800,
                                       .experimental = {.nine_x_4 = true}}}) {
        INFO(iclforge::ac4::Encoder::refusal_reason(config));
        CHECK(iclforge::ac4::Encoder::create(config).has_value());
    }
}
