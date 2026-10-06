// The decoder on streams at 96 and 192 kHz (ac4dec_hsf.hpp): ETSI TS 103 190-1 V1.4.1 clause 5.4,
// the HSF extension substream's lines in transforms twice and four times as long (Tables 99 to
// 108), their band tables (Annex B, Tables B.2 to B.7) and what clause 6.2.5.2 leaves them
// of the output stages. No real stream at these rates exists to hold the decoder to; the streams
// here carry a steady tone, above 24 kHz where the base rate has none, and each is held to its
// tone's frequency and level in the decoder's output and to its waveform.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec_hsf.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "pcm/downmix.hpp"
#include "pcm/drc.hpp"
#include "pcm/substream_pcm.hpp"
#include "sanitized.hpp"

namespace {

using ac4dec_test::HsfCase;
using ac4dec_test::HsfChannel;
using ac4dec_test::HsfStream;
using iclforge::ac4::DecodedFrame;
using iclforge::ac4::DecodeError;
using iclforge::ac4::Decoder;
using iclforge::ac4::DecoderConfig;
using iclforge::test::kSanitized;

constexpr int kFrames = kSanitized ? 8 : 12;
constexpr std::size_t kSkipped = 3;  // the transform's start and the first frame's overlap

struct Decoded {
    std::vector<std::vector<float>> channels;
    std::vector<std::size_t> lengths;
    std::vector<int> rates;
    std::vector<iclforge::ac4::Speaker> speakers;
};

Decoded decode_stream(const HsfStream& stream, const DecoderConfig& config = {}) {
    Decoder decoder(config);
    Decoded out;
    for (std::size_t f = 0; f < stream.frames.size(); ++f) {
        const auto decoded = decoder.decode(stream.frames[f]);
        INFO("frame " << f << ": " << decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const DecodedFrame& pcm = **decoded;
        if (out.channels.empty()) {
            out.channels.resize(pcm.channels.size());
            out.speakers = pcm.speakers;
        }
        REQUIRE(pcm.channels.size() == out.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(),
                                   pcm.channels[c].end());
        }
        out.lengths.push_back(pcm.samples);
        out.rates.push_back(pcm.sample_rate_hz);
    }
    return out;
}

// |S| of the Hann-windowed samples at `hz`, by Goertzel's recurrence, as the amplitude of the sine
// it would be the transform of: a sine of amplitude A at `hz` gives A.
double amplitude_at(std::span<const float> samples, double hz, double rate) {
    const std::size_t n = samples.size();
    const double w = 2.0 * std::numbers::pi * hz / rate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    double window_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double hann =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * (static_cast<double>(i) + 0.5) /
                                 static_cast<double>(n));
        window_sum += hann;
        const double s0 = hann * static_cast<double>(samples[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt(std::max(power, 0.0)) / window_sum;
}

// The frequency of the strongest component within `span_hz` of `guess`, to half a hertz, and its
// amplitude: a tone that is not where it was made to be, by more than a few hertz, has its peak at
// the edge of the span or its amplitude gone.
struct Peak {
    double hz = 0.0;
    double amplitude = 0.0;
};

Peak find_peak(std::span<const float> samples, double guess, double rate, double span_hz = 4.0) {
    Peak best;
    for (double hz = guess - span_hz; hz <= guess + span_hz; hz += 0.5) {
        const double a = amplitude_at(samples, hz, rate);
        if (a > best.amplitude) {
            best = {hz, a};
        }
    }
    return best;
}

double db(double ratio) {
    return 20.0 * std::log10(ratio);
}

// The decoded channel's steady part: kSkipped frames in, 16 384 samples.
std::span<const float> steady(const std::vector<float>& channel, std::size_t frame_length) {
    const std::size_t first = kSkipped * frame_length;
    REQUIRE(channel.size() >= first + 16384);
    return std::span<const float>(channel).subspan(first, 16384);
}

// The decoded channel against the tone the stream was made from, over the steady part:
// the largest difference, as a fraction of the tone's amplitude.
double waveform_error(const HsfCase& c, const HsfStream& stream, const Decoded& d,
                      std::size_t channel) {
    const auto delay =
        static_cast<long>(352 * stream.multiplier);  // Table 188's d_pcm at frame_rate_index 13
    const std::size_t first = kSkipped * static_cast<std::size_t>(stream.frame_length);
    const std::size_t count = 8192;
    const std::vector<float> expected = ac4dec_test::hsf_tone(
        c, stream, channel, stream.origin + static_cast<long>(first) - delay, count);
    double worst = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        worst = std::max(
            worst, static_cast<double>(std::abs(d.channels[channel][first + i] - expected[i])));
    }
    return worst / c.channels[channel].amplitude;
}

HsfCase mono_case(int sf_multiplier, double hz) {
    HsfCase c;
    c.name = "mono";
    c.ch_mode = 0;
    c.sf_multiplier = sf_multiplier;
    c.channels = {HsfChannel{.hz = hz, .amplitude = 0.1}};
    return c;
}

}  // namespace

TEST_CASE("a tone above 24 kHz decodes at 96 kHz at its frequency and level", "[ac4dec][hsf]") {
    // 30 006.25 Hz is the centre of line 2 400 of the 4 096-line transform (12.5 Hz a line), past
    // the 2 048 lines the base rate has: only the HSF extension carries it.
    const HsfCase c = mono_case(0, 30006.25);
    const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
    CHECK(stream.sample_rate_hz == 96000.0);
    const Decoded d = decode_stream(stream);
    REQUIRE(d.channels.size() == 1);
    for (const std::size_t length : d.lengths) {
        CHECK(length == 4096);  // Table 83: 2 048 at 48 kHz, 4 096 at 96
    }
    for (const int rate : d.rates) {
        CHECK(rate == 96000);
    }
    const Peak peak = find_peak(steady(d.channels[0], 4096), 30006.25, 96000.0);
    CHECK(std::abs(peak.hz - 30006.25) <= 1.0);
    CHECK(std::abs(db(peak.amplitude / 0.1)) < 0.1);
    CHECK(waveform_error(c, stream, d, 0) < 0.02);
}

TEST_CASE("a tone above 48 kHz decodes at 192 kHz at its frequency and level", "[ac4dec][hsf]") {
    // Line 5 600 of the 8 192-line transform (11.72 Hz a line) is at 65 625 Hz, past the 4 096
    // lines of 96 kHz as well.
    const double hz = 65625.0 + 0.5 * 24000.0 / 2048.0;
    const HsfCase c = mono_case(1, hz);
    const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
    CHECK(stream.sample_rate_hz == 192000.0);
    const Decoded d = decode_stream(stream);
    for (const std::size_t length : d.lengths) {
        CHECK(length == 8192);
    }
    for (const int rate : d.rates) {
        CHECK(rate == 192000);
    }
    const Peak peak = find_peak(steady(d.channels[0], 8192), hz, 192000.0);
    CHECK(std::abs(peak.hz - hz) <= 1.0);
    CHECK(std::abs(db(peak.amplitude / 0.1)) < 0.1);
    CHECK(waveform_error(c, stream, d, 0) < 0.02);
}

namespace {

HsfCase stereo_case(int sf_multiplier, double left_hz, double right_hz) {
    HsfCase c;
    c.name = "stereo";
    c.ch_mode = 1;
    c.sf_multiplier = sf_multiplier;
    c.channels = {HsfChannel{.hz = left_hz, .amplitude = 0.1},
                  HsfChannel{.hz = right_hz, .amplitude = 0.05}};
    return c;
}

// Each of the case's channels carries its tone and, past `leak_db` below it, none of the others'.
void check_tones(const HsfCase& c, const HsfStream& stream, const Decoded& d,
                 double leak_db = -60.0) {
    REQUIRE(d.channels.size() == c.channels.size());
    for (std::size_t ch = 0; ch < c.channels.size(); ++ch) {
        CAPTURE(ch);
        const auto samples = steady(d.channels[ch], static_cast<std::size_t>(stream.frame_length));
        const HsfChannel& own = c.channels[ch];
        if (own.hz == 0.0) {
            continue;
        }
        const Peak peak = find_peak(samples, own.hz, stream.sample_rate_hz);
        CHECK(std::abs(peak.hz - own.hz) <= 1.0);
        CHECK(std::abs(db(peak.amplitude / own.amplitude)) < 0.15);
        CHECK(waveform_error(c, stream, d, ch) < 0.03);
        for (std::size_t o = 0; o < c.channels.size(); ++o) {
            const HsfChannel& other = c.channels[o];
            if (o != ch && other.hz != 0.0 && std::abs(other.hz - own.hz) > 1000.0) {
                CAPTURE(o);
                CHECK(db(amplitude_at(samples, other.hz, stream.sample_rate_hz) / own.amplitude) <
                      leak_db);
            }
        }
    }
}

}  // namespace

TEST_CASE(
    "a pair's tones, one in the core's bands and one in the extension's, stay in their channels",
    "[ac4dec][hsf]") {
    for (const int sf_multiplier : {0, 1}) {
        CAPTURE(sf_multiplier);
        const double rate = 96000.0 * (sf_multiplier + 1);
        // The left channel's tone is below 24 kHz, in the bands the base rate has; the right's is
        // above it.
        const HsfCase c =
            stereo_case(sf_multiplier, 9812.5, sf_multiplier == 0 ? 37506.25 : 71881.0);
        const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
        CHECK(stream.sample_rate_hz == rate);
        const Decoded d = decode_stream(stream);
        REQUIRE(d.channels.size() == 2);
        CHECK(d.speakers == std::vector<iclforge::ac4::Speaker>{iclforge::ac4::Speaker::kLeft,
                                                                iclforge::ac4::Speaker::kRight});
        check_tones(c, stream, d);
    }
}

TEST_CASE(
    "a pair's stereo processing reaches the extension's bands as clause 5.3 and Table 114 say",
    "[ac4dec][hsf]") {
    // sap_mode 0: left and right. 1: M/S where ms_used says, which covers the core's bands, so the
    // extension's are left and right. 2: M/S in all scale factor bands, the extension's included.
    for (const int sap_mode : {0, 1, 2}) {
        CAPTURE(sap_mode);
        HsfCase c = stereo_case(0, 9812.5, 37506.25);
        c.stereo_proc = true;
        c.sap_mode = sap_mode;
        const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
        check_tones(c, stream, decode_stream(stream));
    }
    // Both channels in the extension at once, M/S coding them into two tracks.
    HsfCase c = stereo_case(0, 33006.25, 41506.25);
    c.stereo_proc = true;
    c.sap_mode = 2;
    const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
    check_tones(c, stream, decode_stream(stream));
}

TEST_CASE("noise fill's escape codes in a stream at 96 kHz leave the tone alone", "[ac4dec][hsf]") {
    HsfCase c = mono_case(0, 30006.25);
    c.noise_fill = true;
    const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
    const Decoded d = decode_stream(stream);
    CHECK(waveform_error(c, stream, d, 0) < 0.02);
    // The same stream without the flag decodes to the same samples.
    c.noise_fill = false;
    const Decoded plain = decode_stream(ac4dec_test::build_hsf_stream(c, kFrames));
    CHECK(d.channels[0] == plain.channels[0]);
}

TEST_CASE("an extension with no band of lines and one with all of them decode", "[ac4dec][hsf]") {
    // No tone past the core's bands: the extension is empty, but its header is read and the
    // frame is 96 kHz, with the core's tone alone.
    HsfCase c = mono_case(0, 5000.5);
    c.ext_bands = 0;
    HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
    check_tones(c, stream, decode_stream(stream));
    // The most bands max_sfb_ext_hsf can name, 16 (96 kHz at 4 096 lines has 79 bands, 63 the base
    // rate's).
    c = mono_case(0, 30006.25);
    c.ext_bands = 16;
    stream = ac4dec_test::build_hsf_stream(c, kFrames);
    check_tones(c, stream, decode_stream(stream));
}

TEST_CASE("a stream at 96 kHz at another frame rate converts as Table 83 says", "[ac4dec][hsf]") {
    struct Leg {
        int frame_rate_index;
        int base;  // frame_len_base
        int up;
        int down;
    };
    // The decoder resampling ratio of Table 83 is the same at 96 kHz as at 48; at 48 kHz these
    // frames give base * up / down samples, and at 96 kHz twice that.
    for (const Leg leg : {Leg{1, 1920, 25, 24}, Leg{0, 1920, 1001, 960}, Leg{6, 960, 25, 24},
                          Leg{7, 1024, 15, 16}, Leg{10, 512, 15, 16}}) {
        CAPTURE(leg.frame_rate_index);
        HsfCase c = mono_case(0, 30006.25);
        c.frame_rate_index = leg.frame_rate_index;
        // Enough frames for the steady part to be 16 384 samples after the first few.
        const int frames = static_cast<int>(kSkipped) + 16384 / (leg.base * leg.up / leg.down) + 3;
        const HsfStream stream = ac4dec_test::build_hsf_stream(c, frames);
        CHECK(stream.frame_length == 2 * leg.base);
        const Decoded d = decode_stream(stream);
        for (std::size_t f = 0; f < d.lengths.size(); ++f) {
            CHECK(d.lengths[f] == static_cast<std::size_t>(2 * leg.base * leg.up / leg.down));
            CHECK(d.rates[f] == 96000);
        }
        // The converter stretches the signal by up / down: the tone, a fixed number of cycles to
        // the internal sample, comes out at down / up of its frequency in cycles to the output
        // sample.
        const double expected = 30006.25 * leg.down / leg.up;
        const Peak peak =
            find_peak(steady(d.channels[0], d.lengths.front()), expected, 96000.0, 6.0);
        CHECK(std::abs(peak.hz - expected) <= 1.5);
        CHECK(std::abs(db(peak.amplitude / 0.1)) < 0.3);
    }
}

TEST_CASE("block switching at 96 and 192 kHz: groups of windows of every transform length",
          "[ac4dec][hsf]") {
    struct Leg {
        int t0;
        int t1;
        int group_size;
    };
    // Each half of the frame in blocks of transf_length 0 to 3, windows grouped in twos, in ones
    // and all together, and the halves differently framed (b_different_framing: two max_sfb, two
    // max_sfb_ext_hsf, groups within each half).
    for (const int sf_multiplier : {0, 1}) {
        for (const Leg leg :
             {Leg{1, 1, 0}, Leg{1, 1, 2}, Leg{1, 1, 1}, Leg{0, 0, 0}, Leg{0, 0, 4}, Leg{2, 2, 1},
              Leg{3, 3, 0}, Leg{3, 1, 0}, Leg{1, 3, 2}, Leg{2, 0, 1}, Leg{0, 3, 0}}) {
            CAPTURE(sf_multiplier, leg.t0, leg.t1, leg.group_size);
            HsfCase c = mono_case(sf_multiplier, sf_multiplier == 0 ? 30010.0 : 65000.0);
            c.transf_length0 = leg.t0;
            c.transf_length1 = leg.t1;
            c.group_size = leg.group_size;
            const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
            const Decoded d = decode_stream(stream);
            const auto samples =
                steady(d.channels[0], static_cast<std::size_t>(stream.frame_length));
            const Peak peak = find_peak(samples, c.channels[0].hz, stream.sample_rate_hz);
            CHECK(std::abs(peak.hz - c.channels[0].hz) <= 1.0);
            CHECK(std::abs(db(peak.amplitude / 0.1)) < 0.25);
            CHECK(waveform_error(c, stream, d, 0) < 0.04);
        }
    }
}

namespace {

// A tone for each channel of a mode, in the decoder's order, at frequencies a channel's own: below
// and above the base rate's 24 kHz, and at 192 kHz above 48 kHz too.
HsfCase multichannel_case(int ch_mode, int sf_multiplier) {
    HsfCase c;
    c.ch_mode = ch_mode;
    c.sf_multiplier = sf_multiplier;
    const std::vector<double> hz = {5006.25, 17512.5,  31018.75, 3750.0,
                                    26025.0, 38531.25, 12506.25};
    const std::vector<double> hz192 = {5006.25, 17512.5,  31018.75, 3750.0,
                                       55025.0, 71531.25, 12506.25};
    const std::size_t count =
        std::vector<std::size_t>{1, 2, 3, 5, 6, 7}[static_cast<std::size_t>(ch_mode)];
    for (std::size_t i = 0; i < count; ++i) {
        double tone = (sf_multiplier == 0 ? hz : hz192)[i];
        if (ch_mode == 4 && i == 3) {
            tone = 87.5;  // the LFE, whose bands stop under 250 Hz
        }
        c.channels.push_back(
            HsfChannel{.hz = tone, .amplitude = 0.04 + 0.01 * static_cast<double>(i)});
    }
    return c;
}

}  // namespace

TEST_CASE("the 3.0 element's channels at 96 and 192 kHz, by coding_config and chel_matsel",
          "[ac4dec][hsf]") {
    for (const int sf_multiplier : {0, 1}) {
        for (const int sap_mode : {0, 2}) {
            // Table 178's twelve matrices: each puts the tracks on L, R and C another way, which
            // the extension's lines must follow as the core's do.
            for (int matsel = 0; matsel < 12; ++matsel) {
                CAPTURE(sf_multiplier, sap_mode, matsel);
                HsfCase c = multichannel_case(2, sf_multiplier);
                c.coding_config = 1;
                c.chel_matsel = matsel;
                c.sap_mode = sap_mode;
                const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
                check_tones(c, stream, decode_stream(stream));
            }
        }
        HsfCase c = multichannel_case(2, sf_multiplier);
        c.coding_config = 0;
        c.stereo_proc = true;
        c.sap_mode = 2;
        const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
        check_tones(c, stream, decode_stream(stream));
    }
}

TEST_CASE("the 5.X element's channels at 96 and 192 kHz, with and without an LFE",
          "[ac4dec][hsf]") {
    for (const int ch_mode : {3, 4}) {
        for (const int sf_multiplier : {0, 1}) {
            for (const int sap_mode : {0, 2}) {
                // Table 179's twelve matrices of five_channel_data(); every third at 192 kHz.
                for (int matsel = 0; matsel < 12; matsel += sf_multiplier == 0 ? 1 : 3) {
                    CAPTURE(ch_mode, sf_multiplier, sap_mode, matsel);
                    HsfCase c = multichannel_case(ch_mode, sf_multiplier);
                    c.coding_config = 3;
                    c.chel_matsel = matsel;
                    c.sap_mode = sap_mode;
                    const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
                    check_tones(c, stream, decode_stream(stream));
                }
            }
            // coding_config 0: two pairs and a mono track, with stereo processing on and off.
            for (const bool proc : {false, true}) {
                CAPTURE(ch_mode, sf_multiplier, proc);
                HsfCase c = multichannel_case(ch_mode, sf_multiplier);
                c.coding_config = 0;
                c.stereo_proc = proc;
                c.sap_mode = 2;
                const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
                check_tones(c, stream, decode_stream(stream));
            }
        }
    }
}

TEST_CASE("the 7.X element's additional channels at 96 and 192 kHz", "[ac4dec][hsf]") {
    for (const int sf_multiplier : {0, 1}) {
        // Without b_use_sap_add_ch, and with Table 183's two steps in each sap_mode.
        for (const int step : {-1, 0, 1, 2}) {
            for (const bool proc : {false, true}) {
                CAPTURE(sf_multiplier, step, proc);
                HsfCase c = multichannel_case(5, sf_multiplier);
                c.use_sap_add_ch = step >= 0;
                c.sap_add_mode = std::max(step, 0);
                c.stereo_proc = proc;
                c.sap_mode = 2;
                const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
                check_tones(c, stream, decode_stream(stream));
            }
        }
    }
}

namespace {

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = static_cast<std::byte>(raw[i]);
    }
    return out;
}

}  // namespace

TEST_CASE("the committed streams at 96 and 192 kHz are the builder's", "[ac4dec][hsf]") {
    // tests/golden/ac4-hsf/*.ac4 are these cases' streams byte for byte, and
    // tools/references/ac4_syntax.py's digests of them, tests/golden/ac4dec/hsf-*.tsv, are what
    // test_ac4dec_syntax.cpp holds the decoder's trace to. They are not under tests/golden/ac4dec/
    // with the other constructed streams, which the tests and checks that play every committed
    // stream at 48 kHz take in. With AC4DEC_WRITE_HSF set to a directory the streams are written
    // there instead of compared, to commit after a change to the builder.
    const std::filesystem::path committed =
        std::filesystem::path{AC4DEC_GOLDEN_DIR} / ".." / "ac4-hsf";
    const char* write_to = std::getenv("AC4DEC_WRITE_HSF");
    for (const HsfCase& c : ac4dec_test::committed_hsf_cases()) {
        CAPTURE(c.name);
        const HsfStream stream = ac4dec_test::build_hsf_stream(c, ac4dec_test::kHsfCommittedFrames);
        const std::vector<std::byte> bytes = ac4dec_test::hsf_sync_framed(stream);
        if (write_to != nullptr) {
            std::filesystem::create_directories(write_to);
            std::ofstream out(std::filesystem::path{write_to} / (c.name + ".ac4"),
                              std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            REQUIRE(out.good());
            continue;
        }
        CHECK(read_file(committed / (c.name + ".ac4")) == bytes);
    }
}

TEST_CASE("the output level gain applies at 96 kHz as clause 5.7.9.3.3 gives it", "[ac4dec][hsf]") {
    // dialnorm_bits 20 is -5 dBFS; to -23 dBFS is 2^((-23 + 5) / 6) = 1/8, by DrcMode::kOff or by
    // the default mode of a stream with no compression to apply.
    const HsfCase c = mono_case(0, 30006.25);
    const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
    for (const iclforge::ac4::DrcMode mode :
         {iclforge::ac4::DrcMode::kOff, iclforge::ac4::DrcMode::kDefault}) {
        DecoderConfig config;
        config.output.output_level_dbfs = -23.0;
        config.output.drc = mode;
        const Decoded d = decode_stream(stream, config);
        const Peak peak = find_peak(steady(d.channels[0], 4096), 30006.25, 96000.0);
        CHECK(std::abs(db(peak.amplitude / (0.1 * 0.125))) < 0.15);
    }
    // At its own dialnorm the level is the coded one.
    DecoderConfig config;
    config.output.output_level_dbfs = -5.0;
    const Decoded d = decode_stream(stream, config);
    CHECK(std::abs(db(find_peak(steady(d.channels[0], 4096), 30006.25, 96000.0).amplitude / 0.1)) <
          0.15);
}

TEST_CASE("the downmix is the matrix of clause 6.2.17 on the samples at 96 kHz", "[ac4dec][hsf]") {
    // Decoded as coded and then downmixed, a stream's channels are what the downmix's matrix
    // makes of them: the same DownmixStage matrix, applied to the samples.
    namespace detail = iclforge::ac4::detail;
    struct Leg {
        int ch_mode;
        iclforge::ac4::DownmixTarget target;
        bool mix_lfe;
    };
    for (const Leg leg : {Leg{4, iclforge::ac4::DownmixTarget::kLoRo, true},
                          Leg{4, iclforge::ac4::DownmixTarget::kLtRt, false},
                          Leg{4, iclforge::ac4::DownmixTarget::kMono, true},
                          Leg{1, iclforge::ac4::DownmixTarget::kMono, true},
                          Leg{0, iclforge::ac4::DownmixTarget::kStereo, true}}) {
        CAPTURE(leg.ch_mode, static_cast<int>(leg.target));
        HsfCase c = multichannel_case(leg.ch_mode, 0);
        c.coding_config = 3;
        const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
        const Decoded coded = decode_stream(stream);
        DecoderConfig config;
        config.output.downmix = leg.target;
        config.output.mix_lfe = leg.mix_lfe;
        const Decoded mixed = decode_stream(stream, config);

        detail::DownmixStage stage;
        stage.configure(coded.speakers, false, leg.target, leg.mix_lfe);
        std::vector<std::vector<detail::QmfValue>> sink;
        stage.update(detail::DownmixValues{});
        const auto& matrix = stage.matrix();
        REQUIRE(mixed.channels.size() == matrix.size());
        for (std::size_t o = 0; o < matrix.size(); ++o) {
            for (std::size_t n = 0; n < mixed.channels[o].size(); ++n) {
                double expected = 0.0;
                for (std::size_t ch = 0; ch < coded.channels.size(); ++ch) {
                    expected += matrix[o][ch] * static_cast<double>(coded.channels[ch][n]);
                }
                REQUIRE(std::abs(static_cast<double>(mixed.channels[o][n]) - expected) < 1e-4);
            }
        }
        // The downmix is what comes out: its channels are the target's.
        CHECK(mixed.speakers.size() == matrix.size());
    }
}

TEST_CASE("a lost frame is concealed at 96 kHz as it is at 48", "[ac4dec][hsf]") {
    const HsfCase c = mono_case(0, 30006.25);
    const HsfStream stream = ac4dec_test::build_hsf_stream(c, kFrames);
    DecoderConfig config;
    config.concealment = iclforge::ac4::ConcealmentPolicy::kRepeatFade;
    Decoder decoder(config);
    std::vector<float> out;
    for (std::size_t f = 0; f < stream.frames.size(); ++f) {
        std::vector<std::byte> frame = stream.frames[f];
        if (f == 6) {
            frame.resize(frame.size() / 3);  // cut inside the table of contents' substreams
        }
        const auto decoded = decoder.decode(frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const DecodedFrame& pcm = **decoded;
        CHECK(pcm.sample_rate_hz == 96000);
        CHECK(pcm.samples == 4096);
        CHECK(pcm.concealed.has_value() == (f == 6));
        out.insert(out.end(), pcm.channels[0].begin(), pcm.channels[0].end());
    }
    // The concealed frame repeats the last good one, faded, with no break in the tone: its level is
    // below the frame before's and above silence.
    const auto level = [&](std::size_t frame) {
        return find_peak(std::span<const float>(out).subspan(frame * 4096, 4096), 30006.25, 96000.0,
                         8.0)
            .amplitude;
    };
    CHECK(level(6) < level(5));
    CHECK(level(6) > 0.01);
}

TEST_CASE("decode_by_block hands a stream at 96 and 192 kHz over at its rate", "[ac4dec][hsf]") {
    for (const int sf_multiplier : {0, 1}) {
        const HsfCase c = mono_case(sf_multiplier, sf_multiplier == 0 ? 30006.25 : 65625.0);
        const HsfStream stream = ac4dec_test::build_hsf_stream(c, 6);
        Decoder decoder;
        std::size_t blocks = 0;
        for (const auto& frame : stream.frames) {
            std::size_t frame_blocks = 0;
            const auto info =
                decoder.decode_by_block(frame, [&](const iclforge::ac4::PcmBlock& block) {
                    CHECK(block.sample_rate_hz == 96000 * (sf_multiplier + 1));
                    CHECK(block.speakers.size() == 1);
                    ++frame_blocks;
                });
            REQUIRE(info.has_value());
            REQUIRE(info->has_value());
            CHECK((*info)->sample_rate_hz == 96000 * (sf_multiplier + 1));
            CHECK((*info)->samples == static_cast<std::size_t>(stream.frame_length));
            blocks += frame_blocks;
        }
        // 256 samples a block, whatever the rate: a frame of 4 096 is sixteen.
        CHECK(blocks ==
              (6U * static_cast<std::size_t>(stream.frame_length)) / iclforge::ac4::kBlockSamples);
    }
}

TEST_CASE("a presentation reports the rate it decodes at", "[ac4dec][hsf]") {
    for (const int sf_multiplier : {0, 1}) {
        const HsfCase c = mono_case(sf_multiplier, 30006.25);
        const HsfStream stream = ac4dec_test::build_hsf_stream(c, 2);
        Decoder decoder;
        const auto report = decoder.parse(stream.frames[0]);
        REQUIRE(report.has_value());
        for (const iclforge::ac4::SubstreamReport& substream : report->substreams) {
            INFO(substream.refused_reason);
            CHECK_FALSE(substream.refused.has_value());
            CHECK(substream.bits_read == substream.size_bits);
        }
        const auto presentations = decoder.presentations();
        REQUIRE(presentations.size() == 1);
        CHECK(presentations[0].decodable);
        CHECK(presentations[0].selectable);
        CHECK(presentations[0].sample_rate_hz == 96000 * (sf_multiplier + 1));
        REQUIRE(presentations[0].members.size() == 1);
        CHECK(presentations[0].members[0].sample_rate_hz == 96000 * (sf_multiplier + 1));
    }
}

TEST_CASE(
    "what Part 1 clause 5.4 and 6.2.5.2 do not give a stream at 96 or 192 kHz is refused by name",
    "[ac4dec][hsf]") {
    namespace detail = iclforge::ac4::detail;
    detail::SubstreamContext ctx;
    ctx.sf_multiplier = 0;
    ctx.frame_len_base = 2048;
    ctx.ch_mode = detail::ch_mode::kStereo;
    detail::FrameInputs inputs;
    std::vector<std::vector<float>> channels;
    std::vector<iclforge::ac4::Speaker> speakers;
    const auto refusal = [&](const detail::AudioSubstream& substream, const detail::FrameInputs& in,
                             const detail::SubstreamContext& context) -> std::string {
        detail::SubstreamPcm pcm;
        const detail::ParseResult result = pcm.decode(context, substream, in, channels, speakers);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kUnsupported);
        return std::string{result.error().reason};
    };
    detail::AudioSubstream simple;
    simple.element.kind = detail::ElementKind::kPair;
    simple.element.codec_mode = detail::codec_mode::kSimple;

    SECTION("object audio") {
        detail::SubstreamContext objects = ctx;
        objects.coding = detail::AudioCoding::kObjects;
        CHECK(refusal(simple, inputs, objects).find("object audio") == 0);
    }
    SECTION("the immersive and 22.2 elements") {
        for (const detail::ElementKind kind :
             {detail::ElementKind::kImmersive, detail::ElementKind::k22_2}) {
            detail::AudioSubstream other = simple;
            other.element.kind = kind;
            CHECK(refusal(other, inputs, ctx).find("the immersive and 22.2 channel elements") == 0);
        }
    }
    SECTION("A-SPX and A-CPL, the QMF domain tools") {
        for (const int mode : {detail::codec_mode::kAspx, detail::codec_mode::kAspxAcpl1,
                               detail::codec_mode::kAspxAcpl2, detail::codec_mode::kAspxAcpl3}) {
            detail::AudioSubstream other = simple;
            other.element.codec_mode = mode;
            CHECK(refusal(other, inputs, ctx).find("A-SPX and A-CPL at 96 or 192 kHz") == 0);
        }
    }
    SECTION("the speech spectral frontend") {
        detail::AudioSubstream other = simple;
        other.element.infos.resize(1);
        other.element.infos[0].spec_frontend = 1;
        other.element.tracks.resize(1);
        other.element.tracks[0].info = 0;
        CHECK(refusal(other, inputs, ctx).find("the speech spectral frontend at 96 or 192 kHz") ==
              0);
    }
    SECTION("mixing a presentation's substreams") {
        detail::FrameInputs mixing = inputs;
        mixing.qmf_only = true;
        CHECK(refusal(simple, mixing, ctx)
                  .find("mixing a presentation's substreams at 96 or 192 kHz") == 0);
        mixing = inputs;
        mixing.mix.active = true;
        CHECK(refusal(simple, mixing, ctx)
                  .find("mixing a presentation's substreams at 96 or 192 kHz") == 0);
    }
    SECTION("dialogue enhancement, where the stream sends it and the system asks for it") {
        detail::FrameInputs de = inputs;
        de.de.active = true;
        de.de.max_gain_db = 6.0;
        de.output.dialogue_enhancement_db = 3.0;
        CHECK(refusal(simple, de, ctx).find("dialogue enhancement at 96 or 192 kHz") == 0);
    }
    SECTION("the compression of DRC") {
        detail::FrameInputs drc = inputs;
        drc.drc.curve = detail::drc_default_curve(1);
        CHECK(refusal(simple, drc, ctx).find("dynamic range compression at 96 or 192 kHz") == 0);
    }
}

TEST_CASE("Table 106 for the base length is Tables 107 and 108 for the longer one",
          "[ac4dec][hsf]") {
    // Clause 4.3.6.2.1: with b_hsf_ext the n_msfb_bits and n_msfbl_bits of the high sampling
    // frequency's transform length are taken from Table 107 (96 kHz) or 108 (192 kHz). The decoder
    // reads them by the base length's, Table 106: the same widths, for every length
    // (src/ac4dec/ERRATA.md, "The widths of max_sfb at 96 and 192 kHz"). Tables 106 to 108 as
    // printed (n_msfb_bits, then n_msfbl_bits where there is one, 0 where the table says N/A):
    struct Row {
        int length;
        int msfb;
        int msfbl;
    };
    constexpr std::array<Row, 15> k48 = {{{2048, 6, 3},
                                          {1920, 6, 3},
                                          {1536, 6, 3},
                                          {1024, 6, 2},
                                          {960, 6, 2},
                                          {768, 6, 2},
                                          {512, 6, 2},
                                          {480, 6, 0},
                                          {384, 6, 2},
                                          {256, 5, 0},
                                          {240, 5, 0},
                                          {192, 5, 0},
                                          {128, 4, 0},
                                          {120, 4, 0},
                                          {96, 4, 0}}};
    constexpr std::array<Row, 15> k96 = {{{4096, 6, 3},
                                          {3840, 6, 3},
                                          {3072, 6, 3},
                                          {2048, 6, 2},
                                          {1920, 6, 2},
                                          {1536, 6, 2},
                                          {1024, 6, 2},
                                          {960, 6, 0},
                                          {768, 6, 2},
                                          {512, 5, 0},
                                          {480, 5, 0},
                                          {384, 5, 0},
                                          {256, 4, 0},
                                          {240, 4, 0},
                                          {192, 4, 0}}};
    constexpr std::array<Row, 15> k192 = {{{8192, 6, 3},
                                           {7680, 6, 3},
                                           {6144, 6, 3},
                                           {4096, 6, 2},
                                           {3840, 6, 2},
                                           {3072, 6, 2},
                                           {2048, 6, 2},
                                           {1920, 6, 0},
                                           {1536, 6, 2},
                                           {1024, 5, 0},
                                           {960, 5, 0},
                                           {768, 5, 0},
                                           {512, 4, 0},
                                           {480, 4, 0},
                                           {384, 4, 0}}};
    for (std::size_t i = 0; i < k48.size(); ++i) {
        CAPTURE(k48[i].length);
        CHECK(k96[i].length == 2 * k48[i].length);
        CHECK(k192[i].length == 4 * k48[i].length);
        CHECK(k96[i].msfb == k48[i].msfb);
        CHECK(k192[i].msfb == k48[i].msfb);
        CHECK(k96[i].msfbl == k48[i].msfbl);
        CHECK(k192[i].msfbl == k48[i].msfbl);
    }
}
