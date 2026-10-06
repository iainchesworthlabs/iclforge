// The decoder on streams of the channel elements DEE's streams do not reach
// (constructed.hpp): the 3.0 element, the 5.X element's
// coding_configs, 2ch_modes and matrices, and the 7.X element in its three
// channel modes, in the SIMPLE and ASPX codec modes; and the channel pair,
// 5.X and 7.X elements in the A-CPL modes. Each stream reads with the
// writer's trace, record for record; each channel's tone comes back on its
// own channel, and the channels A-CPL leaves silent stay so; A-SPX fills the
// channels of the aspx_data element that asks for it; companding changes the
// channel companding_control() names.
//
// The streams under tests/golden/ac4/constructed/ are the committed cases,
// byte for byte, and tests/golden/ac4/ holds
// tools/references/ac4_syntax.py's digests of them, which
// test_syntax.cpp holds the decoder to. With AC4_DECODER_WRITE_CONSTRUCTED
// set to a directory, this writes the committed cases there instead of
// comparing them, to commit after a change to the builder.

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4/decoder/decoder.hpp"
#include "constructed.hpp"
#include "core/dsp/qmf.hpp"
#include "sanitized.hpp"

namespace {

namespace fs = std::filesystem;
using iclforge::test::kSanitized;
using iclforge::ac4::Speaker;
using ac4_decoder_test::BuiltStream;
using ac4_decoder_test::ElementCase;

// Under the sanitizers ten frames, six of them steady: over those,
// tone_amplitude()'s window still holds a tone 126 Hz away 90 dB down.
constexpr int kFrames = kSanitized ? 10 : 16;
// The first frames hold the decoder's delay and the transform's start.
constexpr std::size_t kSkippedFrames = 4;
constexpr double kAmplitude = 0.1;

struct Decoded {
    std::vector<Speaker> speakers;
    std::vector<std::vector<float>> channels;
};

// Every frame decoded, with the decoder's records the writer's: the same
// substreams, offsets, widths and values, in the same order.
Decoded decode_checked(const BuiltStream& stream,
                       iclforge::ac4::DecodingMode decoding = iclforge::ac4::DecodingMode::kFull,
                       double dialogue_enhancement_db = 0.0) {
    std::vector<iclforge::ac4::SyntaxRecord> read;
    const auto keep = [&read](const iclforge::ac4::SyntaxRecord& record) {
        read.push_back(record);
    };
    iclforge::ac4::DecoderConfig config;
    config.syntax = keep;
    config.decoding = decoding;
    config.output.dialogue_enhancement_db = dialogue_enhancement_db;
    iclforge::ac4::Decoder decoder(config);
    Decoded out;
    for (std::size_t f = 0; f < stream.frames.size(); ++f) {
        read.clear();
        const auto decoded = decoder.decode(stream.frames[f]);
        INFO("frame " << f << ": " << decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const std::vector<iclforge::ac4::SyntaxRecord>& written = stream.traces[f];
        REQUIRE(read.size() == written.size());
        for (std::size_t i = 0; i < read.size(); ++i) {
            const bool same = read[i].substream == written[i].substream &&
                              read[i].bit_offset == written[i].bit_offset && read[i].bits == written[i].bits &&
                              read[i].value == written[i].value;
            if (!same) {
                CAPTURE(i, written[i].name, read[i].name, written[i].bit_offset, read[i].bit_offset,
                        written[i].value, read[i].value);
                REQUIRE(same);
            }
        }
        const iclforge::ac4::DecodedFrame& pcm = **decoded;
        if (out.channels.empty()) {
            out.speakers = pcm.speakers;
            out.channels.resize(pcm.channels.size());
        }
        REQUIRE(pcm.channels.size() == out.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
    }
    return out;
}

// The amplitude of `samples`' component at `hz`, through a Hann window, whose
// sidelobes leave another tone 126 Hz away far below 60 dB.
double tone_amplitude(std::span<const float> samples, double hz) {
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
    const auto n = static_cast<double>(samples.size());
    std::complex<double> sum{};
    double weights = 0.0;
    for (std::size_t k = 0; k < samples.size(); ++k) {
        const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) / (n - 1.0));
        sum += window * static_cast<double>(samples[k]) * std::polar(1.0, -w * static_cast<double>(k));
        weights += window;
    }
    return 2.0 * std::abs(sum) / weights;
}

std::span<const float> steady(const Decoded& decoded, std::size_t c) {
    const std::size_t skip = kSkippedFrames * 2048;
    return std::span<const float>(decoded.channels[c]).subspan(skip, decoded.channels[c].size() - skip);
}

// Each channel's tone on that channel at -20 dBFS within 0.3 dB, and every
// other channel's tone at least 60 dB under it there; a channel with no tone
// (tone_hz 0, left silent by A-CPL) has every tone 60 dB under -20 dBFS.
void check_routing(const BuiltStream& stream, const Decoded& decoded) {
    REQUIRE(decoded.speakers == stream.speakers);
    for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
        CAPTURE(c, iclforge::ac4::describe(decoded.speakers[c]));
        double own = kAmplitude;
        if (stream.tone_hz[c] > 0.0) {
            own = tone_amplitude(steady(decoded, c), stream.tone_hz[c]);
            CHECK(std::abs(20.0 * std::log10(own / kAmplitude)) < 0.3);
        }
        for (std::size_t other = 0; other < decoded.channels.size(); ++other) {
            if (other != c && stream.tone_hz[other] > 0.0) {
                CAPTURE(other);
                CHECK(tone_amplitude(steady(decoded, c), stream.tone_hz[other]) < own * 1e-3);
            }
        }
    }
}

// Mean energy of QMF subbands [first, last) over the steady frames.
double band_energy(std::span<const float> samples, std::size_t first, std::size_t last) {
    iclforge::ac4::detail::dsp::QmfAnalysis<double> analysis;
    const std::size_t slots = samples.size() / 64;
    std::vector<double> pcm(slots * 64);
    for (std::size_t n = 0; n < pcm.size(); ++n) {
        pcm[n] = static_cast<double>(samples[n]);
    }
    std::vector<iclforge::ac4::detail::dsp::Complex<double>> q(pcm.size());
    analysis.process(pcm, q);
    double sum = 0.0;
    for (std::size_t ts = 0; ts < slots; ++ts) {
        for (std::size_t sb = first; sb < last; ++sb) {
            sum += norm(q[ts * 64 + sb]);
        }
    }
    return sum / static_cast<double>(slots * (last - first)) + 1e-30;
}

std::string name_of(const ElementCase& c) {
    return c.name.empty() ? "ch_mode " + std::to_string(c.ch_mode) + " config " + std::to_string(c.coding_config)
                          : c.name;
}

// Core decoding of the immersive element (Part 2 clause 4.7): each tone on the
// core channel of its pair (Ls and Lb on Ls, Tfl and Tbl on Tsl, and alike),
// L, R, C and the LFE as they are and the others 3 dB down (Tables 24 and 45),
// and every other tone 60 dB under it there.
void check_core_routing(const BuiltStream& stream, const Decoded& decoded) {
    using S = Speaker;
    const auto core_of = [](Speaker s) {
        switch (s) {
            case S::kLeftBack:
                return S::kLeftSurround;
            case S::kRightBack:
                return S::kRightSurround;
            case S::kTopFrontLeft:
            case S::kTopBackLeft:
                return S::kTopSideLeft;
            case S::kTopFrontRight:
            case S::kTopBackRight:
                return S::kTopSideRight;
            default:
                return s;
        }
    };
    std::vector<S> core_speakers;
    for (const S s : stream.speakers) {
        if (std::ranges::find(core_speakers, core_of(s)) == core_speakers.end()) {
            core_speakers.push_back(core_of(s));
        }
    }
    REQUIRE(decoded.speakers == core_speakers);
    const double down = std::sqrt(0.5);
    for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
        CAPTURE(c, iclforge::ac4::describe(decoded.speakers[c]));
        for (std::size_t s = 0; s < stream.speakers.size(); ++s) {
            if (stream.tone_hz[s] <= 0.0) {
                continue;
            }
            CAPTURE(iclforge::ac4::describe(stream.speakers[s]));
            const double level = tone_amplitude(steady(decoded, c), stream.tone_hz[s]);
            if (core_of(stream.speakers[s]) != decoded.speakers[c]) {
                CHECK(level < kAmplitude * 1e-3);
                continue;
            }
            const bool coupled = stream.speakers[s] != decoded.speakers[c] ||
                                 stream.speakers[s] == S::kLeftSurround ||
                                 stream.speakers[s] == S::kRightSurround;
            CHECK(std::abs(20.0 * std::log10(level / (kAmplitude * (coupled ? down : 1.0)))) < 0.3);
        }
    }
}

// Core decoding of the 9.X.4 modes' immersive element (Part 2 clauses 4.7 and 5.3.3.2): the core
// is 5.X.2, whose L, R, C, Ls, Rs, Tsl and Tsr come, by codec mode, as follows. Each entry is a
// core channel, the tone that is on it and that tone's amplitude as a multiple of the source's
// -20 dBFS; every other tone must be 60 dB under -20 dBFS there.
//
//   SCPL, ASPX_SCPL and ASPX_ACPL_1: A'' to G'' at c_gain (Table 24), or with A-SPX at 2 and
//   with A-CPL's replacement gain of 2 (clause 4.8.3.14): L is (L + Lscr), R (R + Rscr), C C, Ls
//   (Ls + Lb) / sqrt 2, Rs (Rs + Rb) / sqrt 2, Tsl (Tfl + Tbl) / sqrt 2, Tsr (Tfr + Tbr) / sqrt 2;
//   ASPX_ACPL_2: each core channel the one of its pair that the module sends the downmix to, L
//   the L or, with acpl_second, the Lscr tone at 1, Ls the Ls or Lb tone at 1 / sqrt 2, and alike;
//   ASPX_AJCC: Pseudocode 13's modules by the route (below).
struct CoreTone {
    Speaker core;
    Speaker source;
    double gain;
};

std::vector<CoreTone> core_tones_of_fronts(const ElementCase& c) {
    using S = Speaker;
    const double down = std::sqrt(0.5);
    std::vector<CoreTone> out = {{S::kCentre, S::kCentre, 1.0}};
    const auto sides = [&](auto&& side) {
        side(true, S::kLeft, S::kLeftScreen, S::kLeftSurround, S::kLeftBack, S::kTopFrontLeft,
             S::kTopBackLeft, S::kTopSideLeft);
        side(false, S::kRight, S::kRightScreen, S::kRightSurround, S::kRightBack, S::kTopFrontRight,
             S::kTopBackRight, S::kTopSideRight);
    };
    if (c.immersive == 4) {
        // Pseudocode 13 at wet 0: dry1f = dry1b = 1 on route 0, dry2f = dry2b = 1 on route 1,
        // neither on route 2. L = (1 - dry2f) x0in, Ls = (dry1b + dry2b) x1in, Tsl = dry2f x0in +
        // (1 - dry1b - dry2b) x1in, where x0in is the tone of the front's output by the route
        // (L, Tfl or Lscr; Tfl, which Pseudocode 8 scales, at 1 / sqrt 2) and x1in that of the
        // back's (Ls, Lb or Tbl, at 1 / sqrt 2).
        sides([&](bool, S l, S scr, S ls, S lb, S tfl, S tbl, S tsl) {
            switch (c.ajcc_route) {
                case 0:
                    out.push_back({l, l, 1.0});
                    out.push_back({ls, ls, down});
                    break;
                case 1:
                    out.push_back({ls, lb, down});
                    out.push_back({tsl, tfl, down});
                    break;
                default:
                    out.push_back({l, scr, 1.0});
                    out.push_back({tsl, tbl, down});
                    break;
            }
        });
        return out;
    }
    sides([&](bool, S l, S scr, S ls, S lb, S tfl, S tbl, S tsl) {
        if (c.immersive == 3) {
            out.push_back({l, c.acpl_second ? scr : l, 1.0});
            out.push_back({ls, c.acpl_second ? lb : ls, down});
            out.push_back({tsl, c.acpl_second ? tbl : tfl, down});
            return;
        }
        out.push_back({l, l, 1.0});
        out.push_back({l, scr, 1.0});
        out.push_back({ls, ls, down});
        out.push_back({ls, lb, down});
        out.push_back({tsl, tfl, down});
        out.push_back({tsl, tbl, down});
    });
    return out;
}

void check_core_routing_fronts(const ElementCase& c, const BuiltStream& stream,
                               const Decoded& decoded) {
    using S = Speaker;
    const std::vector<S> core =
        stream.speakers.size() == 14
            ? std::vector<S>{S::kLeft,         S::kRight,         S::kCentre,      S::kLfe,
                             S::kLeftSurround, S::kRightSurround, S::kTopSideLeft, S::kTopSideRight}
            : std::vector<S>{S::kLeft,          S::kRight,       S::kCentre,      S::kLeftSurround,
                             S::kRightSurround, S::kTopSideLeft, S::kTopSideRight};
    REQUIRE(decoded.speakers == core);
    const std::vector<CoreTone> tones = core_tones_of_fronts(c);
    for (std::size_t ch = 0; ch < decoded.channels.size(); ++ch) {
        CAPTURE(ch, iclforge::ac4::describe(decoded.speakers[ch]));
        for (std::size_t s = 0; s < stream.speakers.size(); ++s) {
            if (stream.tone_hz[s] <= 0.0 || stream.speakers[s] == S::kLfe) {
                continue;
            }
            CAPTURE(iclforge::ac4::describe(stream.speakers[s]));
            const double level = tone_amplitude(steady(decoded, ch), stream.tone_hz[s]);
            const auto wanted = std::ranges::find_if(tones, [&](const CoreTone& t) {
                return t.core == decoded.speakers[ch] && t.source == stream.speakers[s];
            });
            if (wanted == tones.end()) {
                CHECK(level < kAmplitude * 1e-3);
            } else {
                CHECK(std::abs(20.0 * std::log10(level / (kAmplitude * wanted->gain))) < 0.3);
            }
        }
    }
    // The LFE is its own, untouched.
    if (stream.speakers.size() == 14) {
        const auto lfe = std::ranges::find(decoded.speakers, S::kLfe) - decoded.speakers.begin();
        const auto at = static_cast<std::size_t>(lfe);
        CHECK(std::abs(20.0 * std::log10(tone_amplitude(steady(decoded, at), 47.0) / kAmplitude)) <
              0.3);
    }
}

void check_case(const ElementCase& c) {
    INFO(name_of(c) << (c.aspx ? " ASPX" : " SIMPLE") << ", chel_matsel " << c.chel_matsel
                    << ", sap_mode " << c.sap_mode << ", 2ch_mode " << c.two_ch_mode
                    << ", stereo processing " << c.stereo_proc << ", b_use_sap_add_ch "
                    << c.use_sap_add_ch << ", A-CPL mode " << c.acpl << ", second " << c.acpl_second
                    << ", add_ch_base " << c.add_ch_base << ", immersive mode " << c.immersive
                    << ", a' alpha_q " << c.prediction_alpha_q << ", A-JCC core mode "
                    << c.ajcc_core_mode << " route " << c.ajcc_route);
    const BuiltStream stream = ac4_decoder_test::build_stream(c, kFrames);
    check_routing(stream, decode_checked(stream));
    if (c.immersive >= 0 && c.ch_mode >= 13 && c.ch_mode <= 14) {
        check_core_routing_fronts(c, stream,
                                  decode_checked(stream, iclforge::ac4::DecodingMode::kCore));
    } else if (c.immersive >= 0) {
        check_core_routing(stream, decode_checked(stream, iclforge::ac4::DecodingMode::kCore));
    }
}

// Under the sanitizers a test takes every `stride`-th of its cases from the
// `first`, which its comment says covers what the test covers; a normal build
// takes every case.
class Stride {
   public:
    Stride(std::size_t stride, std::size_t first) : stride_(stride), first_(first) {}

    // Whether the next case runs.
    [[nodiscard]] bool take() {
        const std::size_t index = next_++;
        return !kSanitized || (index >= first_ && (index - first_) % stride_ == 0);
    }

   private:
    std::size_t stride_;
    std::size_t first_;
    std::size_t next_ = 0;
};

void check_case(const ElementCase& c, Stride& stride) {
    if (stride.take()) {
        check_case(c);
    }
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

}  // namespace

TEST_CASE("the 3.0 element's two coding_configs put each tone on its channel", "[ac4][decoder][constructed]") {
    // Under the sanitizers every fifth case: each coding_config in SIMPLE and
    // ASPX, stereo processing on and off, and four of the matrices.
    Stride stride(5, 0);
    for (const bool aspx : {false, true}) {
        for (const bool proc : {false, true}) {
            check_case({.ch_mode = 2, .aspx = aspx, .coding_config = 0, .sap_mode = 2, .stereo_proc = proc},
                       stride);
        }
        for (int matsel = 0; matsel < 12; ++matsel) {
            check_case({.ch_mode = 2, .aspx = aspx, .coding_config = 1, .chel_matsel = matsel, .sap_mode = 2},
                       stride);
        }
    }
}

TEST_CASE("Table 180's coding_configs and 2ch_modes put each 5.X tone on its channel", "[ac4][decoder][constructed]") {
    // Under the sanitizers each 2ch_mode with stereo processing one way, and
    // every fifth matrix, in SIMPLE and ASPX for coding_config 3.
    for (const int ch_mode : {3, 4}) {
        for (const bool two : {false, true}) {
            for (const bool proc : {false, true}) {
                if (kSanitized && two != proc) {
                    continue;
                }
                check_case({.ch_mode = ch_mode, .coding_config = 0, .two_ch_mode = two, .sap_mode = 2,
                            .stereo_proc = proc});
            }
        }
        check_case({.ch_mode = ch_mode, .coding_config = 2, .sap_mode = 2});
        check_case({.ch_mode = ch_mode, .aspx = true, .coding_config = 2, .sap_mode = 0});
    }
    Stride stride(5, 0);
    for (int matsel = 0; matsel < 12; ++matsel) {
        if (!stride.take()) {
            continue;
        }
        check_case({.ch_mode = 4, .coding_config = 1, .chel_matsel = matsel, .sap_mode = 2});
        check_case({.ch_mode = 4, .aspx = matsel % 2 == 1, .coding_config = 3, .chel_matsel = matsel, .sap_mode = 2});
    }
}

TEST_CASE("Tables 182 and 183 put each 7.X tone on its channel in the three modes", "[ac4][decoder][constructed]") {
    // Under the sanitizers every seventh case: each ch_mode and
    // coding_config, SIMPLE and ASPX, both 2ch_modes and b_use_sap_add_ch.
    Stride stride(7, 0);
    for (int ch_mode = 5; ch_mode <= 10; ++ch_mode) {
        for (int config = 0; config < 4; ++config) {
            for (const bool sap : {false, true}) {
                check_case({.ch_mode = ch_mode, .aspx = config % 2 == 1, .coding_config = config,
                            .two_ch_mode = ch_mode % 2 == 0, .chel_matsel = (ch_mode + config) % 12, .sap_mode = 2,
                            .use_sap_add_ch = sap},
                           stride);
            }
        }
    }
}

TEST_CASE("the channel pair's A-CPL modes put each tone on its channel", "[ac4][decoder][constructed][acpl]") {
    // ASPX_ACPL_1: mid and side below acpl_qmf_band, with and without stereo
    // processing; ASPX_ACPL_2: one track, sent to L or to R.
    for (const bool proc : {false, true}) {
        for (const int sap : {0, 2}) {
            check_case({.ch_mode = 1, .sap_mode = sap, .stereo_proc = proc, .acpl = 2});
        }
    }
    // b_dual_maxsfb with the side in fewer bands than the mid, 16 of 22 (to
    // 984 Hz, above both tones): the two tracks' lines do not line up until
    // the decoder lays them out alike.
    check_case({.ch_mode = 1, .sap_mode = 0, .stereo_proc = true, .acpl = 2, .side_bands = 16});
    for (const bool second : {false, true}) {
        for (int id = 0; id < 4; ++id) {
            check_case({.ch_mode = 1, .acpl = 3, .acpl_second = second, .acpl_bands_id = id, .acpl_quant = id % 2});
        }
    }
}

TEST_CASE("the 5.X element's A-CPL modes put each tone on its channel", "[ac4][decoder][constructed][acpl]") {
    // Under the sanitizers each ch_mode takes one coding_config, 3/2/0 the
    // first and 3/2/0 with the LFE the second, and a case of each A-CPL mode,
    // with sap_add_mode, acpl_second, acpl_quant and stereo processing each
    // way between the two.
    for (const int ch_mode : {3, 4}) {
        for (int config = 0; config < 2; ++config) {
            if (kSanitized && config != ch_mode - 3) {
                continue;
            }
            for (const int sap : {0, 2}) {
                if (kSanitized && sap != 2 * config) {
                    continue;
                }
                check_case({.ch_mode = ch_mode, .coding_config = config, .chel_matsel = 5 + config, .sap_mode = 2,
                            .sap_add_mode = sap, .acpl = 2, .acpl_bands_id = config});
            }
            for (const bool second : {false, true}) {
                if (kSanitized && second != (config == 1)) {
                    continue;
                }
                check_case({.ch_mode = ch_mode, .coding_config = config, .chel_matsel = 9, .sap_mode = 2, .acpl = 3,
                            .acpl_second = second, .acpl_quant = config});
            }
        }
        for (const bool second : {false, true}) {
            for (const bool proc : {false, true}) {
                if (kSanitized && (second != proc || second != (ch_mode == 4))) {
                    continue;
                }
                check_case({.ch_mode = ch_mode, .sap_mode = 2, .stereo_proc = proc, .acpl = 4, .acpl_second = second,
                            .acpl_quant = proc ? 1 : 0});
            }
        }
    }
}

TEST_CASE("the 7.X element's A-CPL modes put each tone on its channel by Table 202", "[ac4][decoder][constructed][acpl]") {
    // Under the sanitizers every twentieth case from the second: each
    // ch_mode, coding_config and 2ch_mode, add_ch_base both ways, and each
    // A-CPL mode and acpl_second.
    Stride stride(20, 1);
    for (int ch_mode = 5; ch_mode <= 10; ++ch_mode) {
        for (int config = 0; config < 4; ++config) {
            for (const bool base : {false, true}) {
                if (base && ch_mode <= 6) {
                    continue;  // 3/4/0 sends no add_ch_base
                }
                ElementCase common;
                common.ch_mode = ch_mode;
                common.coding_config = config;
                common.two_ch_mode = ch_mode % 2 == 0;
                common.chel_matsel = (ch_mode + config) % 12;
                common.sap_mode = 2;
                common.sap_add_mode = config % 2 == 0 ? 0 : 2;
                common.add_ch_base = base;
                common.acpl_bands_id = config;
                ElementCase residual = common;
                residual.acpl = 2;
                check_case(residual, stride);
                for (const bool second : {false, true}) {
                    ElementCase full = common;
                    full.acpl = 3;
                    full.acpl_second = second;
                    check_case(full, stride);
                }
            }
        }
    }
}

TEST_CASE("Part 2 Table 19, step 4 and Table 20 put each 7.X.4 tone on its channel, full and core",
          "[ac4][decoder][constructed][immersive]") {
    // SCPL, ASPX_SCPL and ASPX_ACPL_1, which code all eleven signals: every
    // core_5ch_grouping and 2ch_mode, step 4 at identity, M/S or absent, and
    // Table 20 predicting at 0.5 or not at all. Under the sanitizers four
    // cases: SCPL in grouping 1, ASPX_SCPL in 2 and 3 with b_use_sap_add_ch,
    // and ASPX_ACPL_1 in 0, which take each mode and grouping, both 2ch_modes
    // and stereo processing both ways.
    for (const int mode : {0, 1, 2}) {
        for (int grouping = 0; grouping < 4; ++grouping) {
            for (const bool sap : {false, true}) {
                if (kSanitized && (sap != (mode == 1) ||
                                   (mode == 1 ? grouping < 2 : grouping != (mode == 0 ? 1 : 0)))) {
                    continue;
                }
                ElementCase c;
                c.ch_mode = grouping % 2 == 0 ? 12 : 11;
                c.immersive = mode;
                c.coding_config = grouping;
                c.two_ch_mode = (grouping + mode) % 2 == 1;
                c.chel_matsel = (4 * mode + grouping) % 12;
                c.sap_mode = 2;
                c.stereo_proc = mode != 1;
                c.use_sap_add_ch = sap;
                c.sap_add_mode = grouping % 2 == 0 ? 2 : 0;
                c.prediction_alpha_q = sap ? 5 : 0;
                c.acpl_bands_id = grouping;
                c.loud_unit = mode == 1 ? grouping : -1;
                check_case(c);
            }
        }
    }
}

TEST_CASE("the immersive element's ASPX_ACPL_2 makes each coupled pair by A-CPL",
          "[ac4][decoder][constructed][immersive]") {
    // Under the sanitizers the first case and the last, which differ in
    // every field.
    Stride stride(3, 0);
    for (const bool second : {false, true}) {
        for (const bool sap : {false, true}) {
            ElementCase c;
            c.ch_mode = second ? 11 : 12;
            c.immersive = 3;
            c.coding_config = sap ? 3 : 0;
            c.chel_matsel = 2;
            c.sap_mode = 2;
            c.use_sap_add_ch = sap;
            c.acpl_second = second;
            c.acpl_quant = second ? 1 : 0;
            c.acpl_bands_id = sap ? 1 : 3;
            check_case(c, stride);
        }
    }
}

TEST_CASE("A-JCC makes the 7.X.4 channels of its five, full and core, by each core mode",
          "[ac4][decoder][constructed][immersive][ajcc]") {
    // Each route sends a module's A'' and D'' whole to two of its five
    // outputs; the other three stay silent. Under the sanitizers every other
    // case: each route, and each core mode.
    Stride stride(2, 0);
    for (const int core_mode : {0, 1}) {
        for (int route = 0; route < 3; ++route) {
            ElementCase c;
            c.ch_mode = route == 1 ? 11 : 12;
            c.immersive = 4;
            c.coding_config = (core_mode + route) % 4;
            c.two_ch_mode = route == 2;
            c.chel_matsel = 6 + route;
            c.sap_mode = 2;
            c.ajcc_core_mode = core_mode;
            c.ajcc_route = route;
            c.acpl_quant = route % 2;
            c.acpl_bands_id = route;
            check_case(c, stride);
        }
    }
}

TEST_CASE("A-SPX fills the immersive element's channels by Part 2 Table 8, full and core",
          "[ac4][decoder][constructed][immersive]") {
    // The loud aspx_data element's channels, and no other. A-CPL at alpha 1
    // and A-JCC's first route leave each channel A-SPX made where it was, and
    // what they make of it silent. Core decoding in ASPX_SCPL takes the first
    // channel of a coupled pair alone (Table 8's square brackets): the
    // second, Lb, Rb, Tbl or Tbr, or with b_5fronts (the 9.X.4 modes, where
    // the pairs are (L, Lscr) and (R, Rscr)) Lscr or Rscr, core decoding does
    // not have.
    using S = Speaker;
    const auto core_of = [](S s) {
        switch (s) {
            case S::kTopFrontLeft:
                return S::kTopSideLeft;
            case S::kTopFrontRight:
                return S::kTopSideRight;
            default:
                return s;
        }
    };
    // Under the sanitizers every other element, from the first in ASPX_SCPL
    // and A-JCC and from the second in ASPX_ACPL_2.
    for (const int ch_mode : {12, 14}) {
        for (const int mode : {1, 3, 4}) {
            const auto elements = ac4_decoder_test::aspx_elements(ch_mode, mode);
            for (std::size_t loud = 0; loud < elements.size(); ++loud) {
                if (kSanitized && (loud + (mode == 3 ? 1U : 0U)) % 2 != 0) {
                    continue;
                }
                CAPTURE(ch_mode, mode, loud);
                ElementCase c;
                c.ch_mode = ch_mode;
                c.immersive = mode;
                c.sap_mode = 2;
                c.loud_unit = static_cast<int>(loud);
                const BuiltStream stream = ac4_decoder_test::build_stream(c, kFrames);
                for (const iclforge::ac4::DecodingMode decoding :
                     {iclforge::ac4::DecodingMode::kFull, iclforge::ac4::DecodingMode::kCore}) {
                    const bool core = decoding == iclforge::ac4::DecodingMode::kCore;
                    CAPTURE(core);
                    const Decoded decoded = decode_checked(stream, decoding);
                    std::vector<S> filled;
                    for (const S s : elements[loud]) {
                        const bool second = s == S::kLeftBack || s == S::kRightBack ||
                                            s == S::kTopBackLeft || s == S::kTopBackRight ||
                                            s == S::kLeftScreen || s == S::kRightScreen;
                        if (!core || !second) {
                            filled.push_back(core ? core_of(s) : s);
                        }
                    }
                    double quietest_loud = 1e300;
                    double loudest_other = 0.0;
                    std::string levels;
                    for (std::size_t ch = 0; ch < decoded.channels.size(); ++ch) {
                        const double high = band_energy(steady(decoded, ch), 32, 48);
                        levels += std::string{iclforge::ac4::describe(decoded.speakers[ch])} + " " +
                                  std::to_string(10.0 * std::log10(high)) + "; ";
                        if (std::ranges::find(filled, decoded.speakers[ch]) != filled.end()) {
                            quietest_loud = std::min(quietest_loud, high);
                        } else {
                            loudest_other = std::max(loudest_other, high);
                        }
                    }
                    CAPTURE(levels);
                    CAPTURE(10.0 * std::log10(quietest_loud),
                            10.0 * std::log10(loudest_other + 1e-30));
                    CHECK(quietest_loud > 1e3 * loudest_other);
                }
            }
        }
    }
}

TEST_CASE(
    "Part 2 Table 19 with b_5fronts, Tables 20, 23 and 25 put each 9.X.4 tone on its channel, full "
    "and core",
    "[ac4][decoder][constructed][immersive][fronts]") {
    // SCPL, ASPX_SCPL and ASPX_ACPL_1, which code all thirteen signals: every core_5ch_grouping and
    // 2ch_mode, step 4 at identity, M/S or absent, and Table 20's six parameters predicting or not.
    // Under the sanitizers three cases, one of each mode.
    for (const int mode : {0, 1, 2}) {
        for (int grouping = 0; grouping < 4; ++grouping) {
            for (const bool sap : {false, true}) {
                if (kSanitized && (sap != (mode == 1) || grouping != mode + 1)) {
                    continue;
                }
                ElementCase c;
                c.ch_mode = grouping % 2 == 0 ? 14 : 13;
                c.immersive = mode;
                c.coding_config = grouping;
                c.two_ch_mode = (grouping + mode) % 2 == 1;
                c.chel_matsel = (4 * mode + grouping) % 12;
                c.sap_mode = 2;
                c.stereo_proc = mode != 1;
                c.use_sap_add_ch = sap;
                c.sap_add_mode = grouping % 2 == 0 ? 2 : 0;
                c.prediction_alpha_q = sap ? 5 : (mode == 2 ? -3 : 0);
                c.acpl_bands_id = grouping;
                c.loud_unit = mode == 1 ? grouping : -1;
                check_case(c);
            }
        }
    }
}

TEST_CASE("the 9.X.4 modes' ASPX_ACPL_2 makes each coupled pair and each front pair by A-CPL",
          "[ac4][decoder][constructed][immersive][fronts]") {
    // Pseudocode 2 with b_5fronts: six modules, the last two on (L, Lscr) and (R, Rscr), sending
    // the downmix to the first output (alpha 1) or the second (alpha -1). Under the sanitizers the
    // first case and the last, which differ in every field.
    Stride stride(3, 0);
    for (const bool second : {false, true}) {
        for (const bool sap : {false, true}) {
            ElementCase c;
            c.ch_mode = second ? 13 : 14;
            c.immersive = 3;
            c.coding_config = sap ? 3 : 0;
            c.chel_matsel = 2;
            c.sap_mode = 2;
            c.use_sap_add_ch = sap;
            c.acpl_second = second;
            c.acpl_quant = second ? 1 : 0;
            c.acpl_bands_id = sap ? 1 : 3;
            check_case(c, stride);
        }
    }
}

TEST_CASE(
    "A-JCC makes the 9.X.4 channels of its five by its four modules, full and core, by each route",
    "[ac4][decoder][constructed][immersive][ajcc][fronts]") {
    // Each route sends a module's input whole to one of its three outputs (the front modules' to
    // L, Tfl or Lscr, the back modules' to Ls, Lb or Tbl, and alike on the right). Under the
    // sanitizers every other case.
    Stride stride(2, 0);
    for (int route = 0; route < 3; ++route) {
        for (const bool steep : {false}) {
            ElementCase c;
            c.ch_mode = route == 1 ? 13 : 14;
            c.immersive = 4;
            c.coding_config = (route + 1) % 4;
            c.two_ch_mode = route == 2;
            c.chel_matsel = 6 + route;
            c.sap_mode = 2;
            c.ajcc_route = route;
            c.acpl_quant = route % 2;
            c.acpl_bands_id = route;
            (void)steep;
            check_case(c, stride);
        }
    }
}

// The amplitude of source tone `s` of `stream` on the decoded channel `ch`.
double tone_level(const BuiltStream& stream, const Decoded& decoded, std::size_t ch,
                  std::size_t s) {
    return tone_amplitude(steady(decoded, ch), stream.tone_hz[s]);
}

// Dialogue enhancement at its 9 dB cap, with every band's parameter 10 (Table 209's 1.0), raises a
// channel it applies to by 1 + (10^(9/20) - 1) x 1.0, 9 dB, and by 1 + C x that where the core tool
// of Part 2 clauses 5.8.2.1 and 5.8.2.2 weighs the channel's input by C.
const double kDeGain = 9.0;

// Returns how many of the (channel, tone) pairs it checked `weight` raised.
std::size_t check_de_levels(const ElementCase& c, const BuiltStream& stream, const Decoded& plain,
                            const Decoded& enhanced,
                            const std::function<double(Speaker, Speaker)>& weight) {
    // `weight` is the share of the 9 dB each (decoded channel, source tone) pair gets, 0 to 1.
    REQUIRE(plain.speakers == enhanced.speakers);
    const double g = std::pow(10.0, kDeGain / 20.0) - 1.0;
    std::size_t checked = 0;
    std::size_t raised = 0;
    for (std::size_t ch = 0; ch < plain.channels.size(); ++ch) {
        for (std::size_t s = 0; s < stream.speakers.size(); ++s) {
            if (stream.tone_hz[s] <= 0.0 || stream.speakers[s] == Speaker::kLfe) {
                continue;
            }
            const double before = tone_level(stream, plain, ch, s);
            if (before < kAmplitude * 0.1) {
                continue;  // not a tone this channel carries
            }
            CAPTURE(iclforge::ac4::describe(plain.speakers[ch]),
                    iclforge::ac4::describe(stream.speakers[s]), c.de_channel_config, c.ajcc_route,
                    c.acpl_second);
            const double expected = 1.0 + weight(plain.speakers[ch], stream.speakers[s]) * g;
            CHECK(std::abs(20.0 * std::log10(tone_level(stream, enhanced, ch, s) / before /
                                             expected)) < 0.3);
            ++checked;
            raised += weight(plain.speakers[ch], stream.speakers[s]) > 0.0 ? 1U : 0U;
        }
    }
    CHECK(checked >= 5);
    return raised;
}

TEST_CASE("dialogue enhancement raises 9.X.4's Lscr, Rscr and C, not L and R (Part 2 Table 15)",
          "[ac4][decoder][constructed][immersive][fronts][de]") {
    using S = Speaker;
    // de_channel_config's bits are the first, second and third channels of Table 15: 7 is all
    // three, 4 the first alone and 3 the second and the third.
    for (const int config : {7, 4, 3}) {
        for (const int ch_mode : {13, 14}) {
            ElementCase c;
            c.ch_mode = ch_mode;
            c.immersive = 0;
            c.coding_config = config % 4;
            c.sap_mode = 2;
            c.de_channel_config = config;
            c.de_par = 10;
            const BuiltStream stream = ac4_decoder_test::build_stream(c, kFrames);
            ElementCase off = c;
            off.de_channel_config = 0;
            const BuiltStream plain_stream = ac4_decoder_test::build_stream(off, kFrames);
            const Decoded plain = decode_checked(plain_stream);
            const Decoded enhanced =
                decode_checked(stream, iclforge::ac4::DecodingMode::kFull, kDeGain);
            const std::size_t raised =
                check_de_levels(c, stream, plain, enhanced, [&](Speaker out, Speaker source) {
                    if (out != source) {
                        return 0.0;
                    }
                    const bool first = out == S::kLeftScreen;
                    const bool second = out == S::kRightScreen;
                    const bool third = out == S::kCentre;
                    return ((first && (config & 4) != 0) || (second && (config & 2) != 0) ||
                            (third && (config & 1) != 0))
                               ? 1.0
                               : 0.0;
                });
            CHECK(raised == static_cast<std::size_t>(std::popcount(static_cast<unsigned>(config))));
            // Asked for 0 dB, nothing changes.
            const Decoded bypass = decode_checked(stream, iclforge::ac4::DecodingMode::kFull, 0.0);
            check_de_levels(c, stream, plain, bypass, [](Speaker, Speaker) { return 0.0; });
        }
    }
}

TEST_CASE(
    "core decoding's dialogue enhancement weighs Lscr and Rscr by the coefficient the A-JCC or "
    "A-CPL data gives",
    "[ac4][decoder][constructed][immersive][fronts][de]") {
    using S = Speaker;
    // Clauses 5.8.2.1 and 5.8.2.2: C is enhanced whole; the core's L and R by C_L and C_R, 1 where
    // the module sent the input to Lscr (A-CPL's second output, A-JCC's route 2), 0 where it sent
    // it to L (or, route 1, Tfl).
    struct Mode {
        int immersive;
        int route;
        bool second;
        double front;  // C_L and C_R
    };
    for (const Mode mode : {Mode{3, 0, false, 0.0}, Mode{3, 0, true, 1.0}, Mode{4, 0, false, 0.0},
                            Mode{4, 1, false, 0.0}, Mode{4, 2, false, 1.0}}) {
        ElementCase c;
        c.ch_mode = mode.immersive == 4 && mode.route == 1 ? 13 : 14;
        c.immersive = mode.immersive;
        c.coding_config = 1;
        c.chel_matsel = 2;
        c.sap_mode = 2;
        c.acpl_second = mode.second;
        c.ajcc_route = mode.route;
        c.de_channel_config = 7;
        c.de_par = 10;
        const BuiltStream stream = ac4_decoder_test::build_stream(c, kFrames);
        ElementCase off = c;
        off.de_channel_config = 0;
        const BuiltStream plain_stream = ac4_decoder_test::build_stream(off, kFrames);
        const Decoded plain = decode_checked(plain_stream, iclforge::ac4::DecodingMode::kCore);
        const Decoded enhanced =
            decode_checked(stream, iclforge::ac4::DecodingMode::kCore, kDeGain);
        const std::size_t raised =
            check_de_levels(c, stream, plain, enhanced, [&](Speaker out, Speaker) {
                if (out == S::kCentre) {
                    return 1.0;
                }
                return out == S::kLeft || out == S::kRight ? mode.front : 0.0;
            });
        CHECK(raised == (mode.front > 0.0 ? 3U : 1U));
        // The core takes b_de_simulcast's second set where there is one: 0 there leaves the core
        // alone while the full decoding is enhanced as before.
        ElementCase simulcast = c;
        simulcast.de_core_par = 0;
        const BuiltStream simulcast_stream = ac4_decoder_test::build_stream(simulcast, kFrames);
        const Decoded core_zero =
            decode_checked(simulcast_stream, iclforge::ac4::DecodingMode::kCore, kDeGain);
        check_de_levels(c, stream, plain, core_zero, [](Speaker, Speaker) { return 0.0; });
        const Decoded full_plain = decode_checked(plain_stream);
        const Decoded full_simulcast =
            decode_checked(simulcast_stream, iclforge::ac4::DecodingMode::kFull, kDeGain);
        check_de_levels(c, stream, full_plain, full_simulcast, [](Speaker out, Speaker source) {
            return out == source &&
                           (out == S::kLeftScreen || out == S::kRightScreen || out == S::kCentre)
                       ? 1.0
                       : 0.0;
        });
    }
}

TEST_CASE("Table 21 puts each 22.2 tone on its channel, in Table A.27's order",
          "[ac4][decoder][constructed][22_2]") {
    // 24 tones, one on each channel, the two LFEs' among them: every pair
    // processed (M/S and L/R), none, and the pairs alternating, in SIMPLE and
    // ASPX. A pair coded in another's place, or an LFE in a pair's, puts a tone
    // on a channel whose own is another.
    Stride stride(2, 0);
    for (const bool aspx : {false, true}) {
        check_case({.ch_mode = 15, .aspx = aspx, .sap_mode = 2}, stride);
        check_case({.ch_mode = 15, .aspx = aspx, .sap_mode = 0, .stereo_proc = false}, stride);
        check_case({.ch_mode = 15, .aspx = aspx, .sap_mode = 2, .stereo_proc_alternates = true},
                   stride);
    }
    // The output is 24 channels in Table A.27's order: L R C Ls Rs Lb Rb Tfl
    // Tfr Tbl Tbr LFE Tsl Tsr Tfc Tbc Tc LFE2 Bfl Bfr Bfc Cb Lw Rw.
    const BuiltStream stream = ac4_decoder_test::build_stream({.ch_mode = 15, .sap_mode = 2}, 6);
    REQUIRE(stream.speakers.size() == 24);
    const Decoded decoded = decode_checked(stream);
    std::string names;
    for (const Speaker speaker : decoded.speakers) {
        names += std::string{iclforge::ac4::describe(speaker)} + " ";
    }
    CHECK(names ==
          "L R C Ls Rs Lb Rb Tfl Tfr Tbl Tbr LFE Tsl Tsr Tfc Tbc Tc LFE2 Bfl Bfr Bfc Cb Lw Rw ");
}

TEST_CASE("A-SPX fills the 22.2 element's channels by the pair of Part 2 Table 8 that asks for it",
          "[ac4][decoder][constructed][22_2]") {
    // Crossover at QMF subband 28 (10.5 kHz): the tones are below it, and only
    // the loud aspx_data_2ch()'s two channels have anything from 12 to 18 kHz;
    // the LFEs, which have no aspx_data, and every other pair have nothing.
    const auto elements = ac4_decoder_test::aspx_elements(15);
    REQUIRE(elements.size() == 11);
    Stride stride(2, 0);
    for (std::size_t loud = 0; loud < elements.size(); ++loud) {
        if (!stride.take()) {
            continue;
        }
        CAPTURE(loud);
        const BuiltStream stream = ac4_decoder_test::build_stream(
            {.ch_mode = 15, .aspx = true, .sap_mode = 2, .loud_unit = static_cast<int>(loud)},
            kFrames);
        const Decoded decoded = decode_checked(stream);
        double quietest_loud = 1e300;
        double loudest_other = 0.0;
        for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
            const double high = band_energy(steady(decoded, c), 32, 48);
            const bool carried =
                std::ranges::find(elements[loud], decoded.speakers[c]) != elements[loud].end();
            if (carried) {
                quietest_loud = std::min(quietest_loud, high);
            } else {
                loudest_other = std::max(loudest_other, high);
            }
        }
        CAPTURE(10.0 * std::log10(quietest_loud), 10.0 * std::log10(loudest_other + 1e-30));
        CHECK(quietest_loud > 1e3 * loudest_other);
    }
}

TEST_CASE("a 22.2 stream decodes as coded, 24 channels, and nothing else",
          "[ac4][decoder][constructed][22_2]") {
    // Part 2 Table 8: only full decoding. Tables 35 to 43 have no 22.2 input, so
    // every target but as coded is refused, by name, and so is core decoding.
    for (const bool aspx : {false, true}) {
        const BuiltStream stream =
            ac4_decoder_test::build_stream({.ch_mode = 15, .aspx = aspx, .sap_mode = 2}, 3);
        CAPTURE(aspx);
        {
            iclforge::ac4::Decoder decoder;
            for (const std::vector<std::byte>& frame : stream.frames) {
                const auto decoded = decoder.decode(frame);
                REQUIRE(decoded.has_value());
                REQUIRE(decoded->has_value());
                CHECK((**decoded).channels.size() == 24);
                CHECK((**decoded).speakers == stream.speakers);
            }
            REQUIRE(decoder.presentations().size() == 1);
            const iclforge::ac4::PresentationInfo& presentation = decoder.presentations().front();
            CHECK(presentation.decodable);
            CHECK(presentation.selectable);
            CHECK(presentation.speakers == stream.speakers);
        }
        {
            iclforge::ac4::Decoder decoder(
                iclforge::ac4::DecoderConfig{.decoding = iclforge::ac4::DecodingMode::kCore});
            const auto decoded = decoder.decode(stream.frames.front());
            REQUIRE_FALSE(decoded.has_value());
            CHECK(decoded.error() == iclforge::ac4::DecodeError::kUnsupported);
            CHECK(decoder.refusal_reason().find("22_2_channel_element()") !=
                  std::string_view::npos);
            CHECK(decoder.refusal_reason().find("core decoding") != std::string_view::npos);
        }
        using iclforge::ac4::DownmixTarget;
        for (const DownmixTarget target :
             {DownmixTarget::k5X, DownmixTarget::kStereo, DownmixTarget::kLoRo,
              DownmixTarget::kLtRt, DownmixTarget::kMono, DownmixTarget::k7X4, DownmixTarget::k7X2,
              DownmixTarget::k7X0, DownmixTarget::k5X4, DownmixTarget::k5X2}) {
            CAPTURE(iclforge::ac4::describe(target));
            iclforge::ac4::DecoderConfig config;
            config.output.downmix = target;
            iclforge::ac4::Decoder decoder(config);
            const auto decoded = decoder.decode(stream.frames.front());
            REQUIRE_FALSE(decoded.has_value());
            CHECK(decoded.error() == iclforge::ac4::DecodeError::kUnsupported);
            CHECK(decoder.refusal_reason().find("22.2") != std::string_view::npos);
        }
    }
}

TEST_CASE("A-CPL's decorrelated part cancels in the sum of its two outputs", "[ac4][decoder][constructed][acpl]") {
    // The channel pair in ASPX_ACPL_2 with alpha 0 and beta_q 4 (1.4 at ibeta
    // 0, Table 204): L = x0 + 0.7 y and R = x0 - 0.7 y, with y the ducked
    // decorrelator output of 2 x0. A steady tone passes the all-pass filters
    // and the ducker whole, so (L + R) / 2 is the coded tone and (L - R) / 2
    // the tone 1.4 times over, phase-shifted.
    const BuiltStream stream = ac4_decoder_test::build_stream({.ch_mode = 1, .acpl = 3, .acpl_beta_q = 4}, kFrames);
    const Decoded decoded = decode_checked(stream);
    const std::span<const float> l = steady(decoded, 0);
    const std::span<const float> r = steady(decoded, 1);
    std::vector<float> sum(l.size());
    std::vector<float> difference(l.size());
    for (std::size_t n = 0; n < l.size(); ++n) {
        sum[n] = 0.5F * (l[n] + r[n]);
        difference[n] = 0.5F * (l[n] - r[n]);
    }
    const double hz = stream.tone_hz[0];
    CHECK(std::abs(20.0 * std::log10(tone_amplitude(sum, hz) / kAmplitude)) < 0.1);
    CHECK(std::abs(20.0 * std::log10(tone_amplitude(difference, hz) / (1.4 * kAmplitude))) < 0.3);
}

TEST_CASE("A-SPX fills the channels of the aspx_data element Table 213 gives them", "[ac4][decoder][constructed]") {
    // Crossover at QMF subband 28 (10.5 kHz): the tones are below it, and
    // only the loud element's channels have anything from 12 to 18 kHz. Under
    // the sanitizers every other element, from the first and the second by
    // turns.
    std::size_t turn = 0;
    for (const int ch_mode : {2, 4, 5, 7, 9}) {
        const auto elements = ac4_decoder_test::aspx_elements(ch_mode);
        const std::size_t first = turn++ % 2;
        for (std::size_t loud = 0; loud < elements.size(); ++loud) {
            if (kSanitized && (loud + first) % 2 != 0) {
                continue;
            }
            CAPTURE(ch_mode, loud);
            const BuiltStream stream = ac4_decoder_test::build_stream(
                {.ch_mode = ch_mode, .aspx = true, .coding_config = 0, .sap_mode = 2,
                 .loud_unit = static_cast<int>(loud)},
                kFrames);
            const Decoded decoded = decode_checked(stream);
            double quietest_loud = 1e300;
            double loudest_other = 0.0;
            for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
                const double high = band_energy(steady(decoded, c), 32, 48);
                const bool carried = std::ranges::find(elements[loud], decoded.speakers[c]) != elements[loud].end();
                if (carried) {
                    quietest_loud = std::min(quietest_loud, high);
                } else {
                    loudest_other = std::max(loudest_other, high);
                }
            }
            CAPTURE(10.0 * std::log10(quietest_loud), 10.0 * std::log10(loudest_other + 1e-30));
            CHECK(quietest_loud > 1e3 * loudest_other);
        }
    }
}

TEST_CASE("companding changes only the channel companding_control() names", "[ac4][decoder][constructed]") {
    // Table 212: L, R and C for 3.0; L, R, C, Ls and Rs for 5.X. With
    // b_compand_on, the expander scales that channel's low band by its level
    // over full scale to the 0.54th power times 2^(1/0.65), which moves each
    // tone here by 0.9 dB or more, by where it falls among the subbands.
    struct Mode {
        int ch_mode;
        std::vector<Speaker> order;
    };
    const std::vector<Mode> modes = {
        {2, {Speaker::kLeft, Speaker::kRight, Speaker::kCentre}},
        {4, {Speaker::kLeft, Speaker::kRight, Speaker::kCentre, Speaker::kLeftSurround, Speaker::kRightSurround}},
    };
    for (const Mode& mode : modes) {
        const BuiltStream plain_stream =
            ac4_decoder_test::build_stream({.ch_mode = mode.ch_mode, .aspx = true, .coding_config = 0}, kFrames);
        const Decoded plain = decode_checked(plain_stream);
        for (std::size_t k = 0; k < mode.order.size(); ++k) {
            CAPTURE(mode.ch_mode, k);
            const BuiltStream stream = ac4_decoder_test::build_stream(
                {.ch_mode = mode.ch_mode, .aspx = true, .coding_config = 0, .companded = static_cast<int>(k)},
                kFrames);
            const Decoded decoded = decode_checked(stream);
            for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
                CAPTURE(iclforge::ac4::describe(decoded.speakers[c]));
                const double before = tone_amplitude(steady(plain, c), stream.tone_hz[c]);
                const double after = tone_amplitude(steady(decoded, c), stream.tone_hz[c]);
                const double change = std::abs(20.0 * std::log10(after / before));
                if (decoded.speakers[c] == mode.order[k]) {
                    CHECK(change > 0.5);
                } else {
                    CHECK(change < 0.01);
                }
            }
        }
    }
}

TEST_CASE("the committed constructed streams are the builder's", "[ac4][decoder][constructed]") {
    const fs::path committed = fs::path{AC4_GOLDEN_DIR} / "constructed";
    const char* write_to = std::getenv("AC4_DECODER_WRITE_CONSTRUCTED");
    for (const ElementCase& c : ac4_decoder_test::committed_cases()) {
        CAPTURE(c.name);
        const BuiltStream stream = ac4_decoder_test::build_stream(c, ac4_decoder_test::kCommittedFrames);
        (void)decode_checked(stream);
        const std::vector<std::byte> bytes = ac4_decoder_test::sync_framed(stream);
        if (write_to != nullptr) {
            fs::create_directories(write_to);
            std::ofstream out(fs::path{write_to} / (c.name + ".ac4"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(out.good());
            continue;
        }
        CHECK(read_file(committed / (c.name + ".ac4")) == bytes);
    }
}
