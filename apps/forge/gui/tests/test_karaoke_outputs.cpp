// forge_gui::karaoke_outputs() (src/karaoke_authoring.hpp) against what the
// library's decoder actually does. The Karaoke flow's Hear step says which part
// each speaker plays; that sentence is a table in this program, and no outside
// decoder reproduces karaoke, so the only oracle there is is the decoder it
// stands in front of. Every mode of Table C.2.1 is encoded with a tone a part
// (L 440, R 620, M 800, V1 1030, V2 1270 Hz), decoded as coded, as the aware
// decoder reproduces it and as the capable decoder does for each choice of
// vocals, and the tones present on each output are what the table says.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "karaoke_authoring.hpp"

namespace {

using forge_gui::KaraokeFile;
using forge_gui::KaraokeListening;
using forge_gui::KaraokePart;
using forge_gui::KaraokeVocalsChoice;

constexpr double kRate = 48000.0;
constexpr std::size_t kFrames = 24;

double part_hz(const std::string& part) {
    if (part == "L") return 440.0;
    if (part == "R") return 620.0;
    if (part == "M") return 800.0;
    if (part == "V1") return 1030.0;
    return 1270.0;  // V2
}

// The level of a tone in `x`, in dB below full scale, by a Hann-windowed
// Goertzel sum over the second half (the decoder's first blocks are its
// overlap-add warm-up).
double tone_db(const std::vector<float>& x, double hz) {
    const std::size_t begin = x.size() / 2;
    const std::size_t n = x.size() - begin;
    double re = 0.0;
    double im = 0.0;
    double sum_w = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                              static_cast<double>(n - 1));
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(begin + i) / kRate;
        re += w * static_cast<double>(x[begin + i]) * std::cos(phase);
        im += w * static_cast<double>(x[begin + i]) * std::sin(phase);
        sum_w += w;
    }
    const double amplitude = 2.0 * std::sqrt(re * re + im * im) / sum_w;
    return 20.0 * std::log10(amplitude + 1e-12);
}

struct Mode {
    bool melody;
    int vocals;
};

// Encodes the parts of `mode` (a tone each, at 0.1 amplitude) as a karaoke
// stream and returns its frames.
std::vector<std::vector<std::byte>> encode(const Mode& mode, forge_gui::KaraokePlan* plan_out) {
    std::vector<KaraokeFile> files;
    KaraokeFile backing;
    backing.part = KaraokePart::kBacking;
    backing.name = "backing.wav";
    backing.channels = 2;
    backing.sample_rate = 48000;
    backing.seconds = 10;
    files.push_back(backing);
    const auto mono = [](KaraokePart part, const char* name) {
        KaraokeFile f;
        f.part = part;
        f.name = name;
        f.channels = 1;
        f.sample_rate = 48000;
        f.seconds = 10;
        return f;
    };
    if (mode.melody) files.push_back(mono(KaraokePart::kMelody, "melody.wav"));
    if (mode.vocals >= 1) files.push_back(mono(KaraokePart::kVocal1, "lead.wav"));
    if (mode.vocals >= 2) files.push_back(mono(KaraokePart::kVocal2, "harmony.wav"));
    const forge_gui::KaraokePlan plan = forge_gui::plan_karaoke_parts(files);
    REQUIRE(plan.buildable);
    *plan_out = plan;

    iclforge::ac3::EncoderConfig config;
    config.sample_rate = iclforge::ac3::SampleRate::k48000;
    config.bitrate_kbps = 448;
    config.acmod = plan.acmod;
    config.lfe = false;
    config.info.bsmod = iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke;
    iclforge::ac3::FrameEncoder encoder{config};
    REQUIRE(static_cast<std::size_t>(encoder.channel_count()) == plan.channels.size());

    std::vector<std::vector<float>> input;
    for (const auto& channel : plan.channels) {
        std::vector<float> tone(kFrames * iclforge::ac3::kSamplesPerFrame);
        const double hz = part_hz(channel.part);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            tone[i] = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
        }
        input.push_back(std::move(tone));
    }
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t f = 0; f < kFrames; ++f) {
        std::vector<std::span<const float>> views;
        for (const auto& channel : input) {
            views.emplace_back(channel.data() + f * iclforge::ac3::kSamplesPerFrame,
                               iclforge::ac3::kSamplesPerFrame);
        }
        const auto encoded = encoder.encode_frame(views);
        REQUIRE(encoded.has_value());
        frames.emplace_back(encoded->begin(), encoded->end());
    }
    return frames;
}

struct Decoded {
    std::vector<std::vector<float>> channels;  // planar, in the decoder's output order
};

Decoded decode(const std::vector<std::vector<std::byte>>& frames, iclforge::ac3::KaraokeReproduction mode,
               iclforge::ac3::KaraokeVocals vocals,
               iclforge::ac3::DownmixTarget target = iclforge::ac3::DownmixTarget::kAsCoded) {
    iclforge::ac3::DecoderConfig config;
    config.output.karaoke = mode;
    config.output.karaoke_vocals = vocals;
    config.output.target = target;
    iclforge::ac3::FrameDecoder decoder{config};
    Decoded out;
    for (const auto& frame : frames) {
        const auto decoded = decoder.decode_frame(frame);
        REQUIRE(decoded.has_value());
        if (out.channels.empty()) {
            out.channels.resize(decoded->channels.size());
        }
        REQUIRE(out.channels.size() == decoded->channels.size());
        for (std::size_t c = 0; c < decoded->channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), decoded->channels[c].begin(),
                                   decoded->channels[c].end());
        }
    }
    return out;
}

// The parts whose tone is on `x`.
std::string parts_on(const std::vector<float>& x) {
    std::string out;
    for (const char* part : {"L", "R", "M", "V1", "V2"}) {
        const double db = tone_db(x, part_hz(part));
        INFO("part " << part << " at " << db << " dB");
        // A tone that is there is about -20 dBFS less its gain; one that is
        // not is at the encoder's noise floor.
        REQUIRE((db > -45.0 || db < -70.0));
        if (db > -45.0) {
            out += (out.empty() ? "" : " ");
            out += part;
        }
    }
    return out;
}

std::string joined(const std::vector<std::string>& parts) {
    // In the order parts_on() lists them.
    std::string out;
    for (const char* part : {"L", "R", "M", "V1", "V2"}) {
        for (const std::string& p : parts) {
            if (p == part) {
                out += (out.empty() ? "" : " ");
                out += part;
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("karaoke outputs: the table holds against the decoder for every mode and choice",
          "[forge-gui][karaoke][oracle]") {
    const std::array<Mode, 5> modes = {{{true, 0}, {false, 1}, {true, 1}, {false, 2}, {true, 2}}};
    struct Listening {
        const char* name;
        KaraokeListening listening;
        iclforge::ac3::KaraokeReproduction reproduction;
        KaraokeVocalsChoice choice;
        iclforge::ac3::KaraokeVocals vocals;
    };
    const std::array<Listening, 6> listenings = {{
        {"as coded", KaraokeListening::kAsCoded, iclforge::ac3::KaraokeReproduction::kOff,
         KaraokeVocalsChoice::kBoth, iclforge::ac3::KaraokeVocals::kBoth},
        {"aware", KaraokeListening::kAware, iclforge::ac3::KaraokeReproduction::kAware,
         KaraokeVocalsChoice::kBoth, iclforge::ac3::KaraokeVocals::kBoth},
        {"capable none", KaraokeListening::kCapable, iclforge::ac3::KaraokeReproduction::kCapable,
         KaraokeVocalsChoice::kNone, iclforge::ac3::KaraokeVocals::kNone},
        {"capable v1", KaraokeListening::kCapable, iclforge::ac3::KaraokeReproduction::kCapable,
         KaraokeVocalsChoice::kV1, iclforge::ac3::KaraokeVocals::kV1},
        {"capable v2", KaraokeListening::kCapable, iclforge::ac3::KaraokeReproduction::kCapable,
         KaraokeVocalsChoice::kV2, iclforge::ac3::KaraokeVocals::kV2},
        {"capable both", KaraokeListening::kCapable, iclforge::ac3::KaraokeReproduction::kCapable,
         KaraokeVocalsChoice::kBoth, iclforge::ac3::KaraokeVocals::kBoth},
    }};

    for (const Mode& mode : modes) {
        forge_gui::KaraokePlan plan;
        const auto frames = encode(mode, &plan);
        for (const Listening& listening : listenings) {
            INFO("mode " << plan.mode << ", " << listening.name);
            const Decoded decoded = decode(frames, listening.reproduction, listening.vocals);
            const forge_gui::KaraokeOutputs expected =
                forge_gui::karaoke_outputs(mode.melody, mode.vocals, listening.listening, listening.choice);

            if (listening.listening == KaraokeListening::kAsCoded) {
                // The coded channels, one per position of the plan.
                REQUIRE(decoded.channels.size() == plan.channels.size());
                for (std::size_t c = 0; c < plan.channels.size(); ++c) {
                    const std::string& position = plan.channels[c].position;
                    const std::string actual = parts_on(decoded.channels[c]);
                    INFO("position " << position);
                    if (position == "L") {
                        CHECK(actual == joined(expected.l));
                    } else if (position == "R") {
                        CHECK(actual == joined(expected.r));
                    } else if (position == "C") {
                        CHECK(actual == joined(expected.c));
                    } else if (position == "Ls") {
                        CHECK(actual == joined(expected.ls));
                    } else if (position == "Rs") {
                        CHECK(actual == joined(expected.rs));
                    } else {
                        // The one surround of 2/1 and 3/1: a 5.1 room plays it
                        // from both, which is what the table says of each.
                        CHECK(joined(expected.ls) == joined(expected.rs));
                        CHECK(actual == joined(expected.ls));
                    }
                }
            } else {
                // Reproduced: L, C, R.
                REQUIRE(decoded.channels.size() == 3);
                CHECK(parts_on(decoded.channels[0]) == joined(expected.l));
                CHECK(parts_on(decoded.channels[1]) == joined(expected.c));
                CHECK(parts_on(decoded.channels[2]) == joined(expected.r));
                CHECK(expected.ls.empty());
                CHECK(expected.rs.empty());
            }
        }
    }
}

TEST_CASE("karaoke outputs: the Lo/Ro fold's column holds against the decoder too",
          "[forge-gui][karaoke][oracle]") {
    const std::array<Mode, 5> modes = {{{true, 0}, {false, 1}, {true, 1}, {false, 2}, {true, 2}}};
    struct Choice {
        const char* name;
        KaraokeListening listening;
        iclforge::ac3::KaraokeReproduction reproduction;
        KaraokeVocalsChoice choice;
        iclforge::ac3::KaraokeVocals vocals;
    };
    const std::array<Choice, 6> choices = {{
        {"as coded", KaraokeListening::kAsCoded, iclforge::ac3::KaraokeReproduction::kOff,
         KaraokeVocalsChoice::kBoth, iclforge::ac3::KaraokeVocals::kBoth},
        {"aware", KaraokeListening::kAware, iclforge::ac3::KaraokeReproduction::kAware,
         KaraokeVocalsChoice::kBoth, iclforge::ac3::KaraokeVocals::kBoth},
        {"capable none", KaraokeListening::kCapable, iclforge::ac3::KaraokeReproduction::kCapable,
         KaraokeVocalsChoice::kNone, iclforge::ac3::KaraokeVocals::kNone},
        {"capable v1", KaraokeListening::kCapable, iclforge::ac3::KaraokeReproduction::kCapable,
         KaraokeVocalsChoice::kV1, iclforge::ac3::KaraokeVocals::kV1},
        {"capable v2", KaraokeListening::kCapable, iclforge::ac3::KaraokeReproduction::kCapable,
         KaraokeVocalsChoice::kV2, iclforge::ac3::KaraokeVocals::kV2},
        {"capable both", KaraokeListening::kCapable, iclforge::ac3::KaraokeReproduction::kCapable,
         KaraokeVocalsChoice::kBoth, iclforge::ac3::KaraokeVocals::kBoth},
    }};
    for (const Mode& mode : modes) {
        forge_gui::KaraokePlan plan;
        const auto frames = encode(mode, &plan);
        for (const Choice& choice : choices) {
            INFO("mode " << plan.mode << ", " << choice.name << ", folded");
            const Decoded decoded =
                decode(frames, choice.reproduction, choice.vocals, iclforge::ac3::DownmixTarget::kLoRo);
            const forge_gui::KaraokeOutputs expected = forge_gui::karaoke_outputs(
                mode.melody, mode.vocals, choice.listening, choice.choice, /*folded=*/true);
            REQUIRE(decoded.channels.size() == 2);
            CHECK(parts_on(decoded.channels[0]) == joined(expected.l));
            CHECK(parts_on(decoded.channels[1]) == joined(expected.r));
        }
    }
}
