#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/containers/iamf/iamf.hpp"
#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"

// Scalable channel audio (IAMF v2.0.0 section 7.2): the tests below play the encoder. They down-mix
// a programme with the mechanism Annex A2.2 describes, group the results the way 3.6.2.2 lays out,
// write the Channel Groups as `ipcm` Audio Substreams with the demixing (and recon gain) Parameter
// Blocks, and check that decode_pcm() rebuilds each layer's down-mix. The channel groups of every
// chain are written out literally below, from the specification's own wording, so the decoder's
// composition rules are checked against something other than themselves.

namespace iamf = iclforge::containers::iamf;

namespace {

using Signal = std::vector<double>;
using Channels = std::map<std::string, Signal>;

constexpr std::size_t kFrameSamples = 192;  // above the 64 sample recon gain overlap
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kDemixId = 10;
constexpr std::uint32_t kReconId = 11;

struct Mode {
    double alpha;
    double beta;
    double gamma;
    double delta;
    int offset;
};

// 3.9.3, as the Annex A2.2 down-mixer uses it.
[[nodiscard]] Mode mode_of(std::uint8_t dmixp_mode) {
    switch (dmixp_mode) {
        case 0:
            return {1.0, 1.0, 0.707, 0.707, -1};
        case 1:
            return {0.707, 0.707, 0.707, 0.707, -1};
        case 2:
            return {1.0, 0.866, 0.866, 0.866, -1};
        case 4:
            return {1.0, 1.0, 0.707, 0.707, 1};
        case 5:
            return {0.707, 0.707, 0.707, 0.707, 1};
        default:
            return {1.0, 0.866, 0.866, 0.866, 1};
    }
}

constexpr std::array<double, 11> kWeights{0.0,    0.0179, 0.0391, 0.0658, 0.1038, 0.25,
                                          0.3962, 0.4342, 0.4609, 0.4821, 0.5};

[[nodiscard]] Signal combine(const Signal& a, double ka, const Signal& b, double kb) {
    Signal out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i] = ka * a[i] + kb * b[i];
    }
    return out;
}

// The channels of one frame of layout `layout` down-mixed from the source layout's channels
// (Annex A2.2: S7to5, S5to3, S3to2, S2to1, T4to2, T2toTF2).
[[nodiscard]] Channels downmix(std::uint8_t layout, std::uint8_t source, const Channels& s,
                               const Mode& m, double w) {
    const bool source_seven = source == 5 || source == 6 || source == 7;
    const Signal ls5 =
        source_seven ? combine(s.at("Lss"), m.alpha, s.at("Lrs"), m.beta) : s.at("Ls");
    const Signal rs5 =
        source_seven ? combine(s.at("Rss"), m.alpha, s.at("Rrs"), m.beta) : s.at("Rs");
    const bool source_four = source == 4 || source == 7;
    const bool five_one_four = source == 4;
    Signal ltf2;
    Signal rtf2;
    if (source_four) {
        ltf2 = combine(s.at("Ltf"), 1.0, s.at(five_one_four ? "Ltr" : "Ltb"), m.gamma);
        rtf2 = combine(s.at("Rtf"), 1.0, s.at(five_one_four ? "Rtr" : "Rtb"), m.gamma);
    } else if (s.contains("Ltf")) {
        ltf2 = s.at("Ltf");
        rtf2 = s.at("Rtf");
    }
    const Signal l3 = combine(s.at("L"), 1.0, ls5, m.delta);
    const Signal r3 = combine(s.at("R"), 1.0, rs5, m.delta);
    const Signal l2 = combine(l3, 1.0, s.at("C"), 0.707);
    const Signal r2 = combine(r3, 1.0, s.at("C"), 0.707);

    Channels out;
    switch (layout) {
        case 0:
            out["C"] = combine(l2, 0.5, r2, 0.5);
            break;
        case 1:
            out["L"] = l2;
            out["R"] = r2;
            break;
        case 8:
            out["L"] = l3;
            out["R"] = r3;
            out["C"] = s.at("C");
            out["LFE"] = s.at("LFE");
            out["Ltf"] = combine(ltf2, 1.0, ls5, w * m.delta);
            out["Rtf"] = combine(rtf2, 1.0, rs5, w * m.delta);
            break;
        case 2:
        case 3:
        case 4:
            out["L"] = s.at("L");
            out["R"] = s.at("R");
            out["Ls"] = ls5;
            out["Rs"] = rs5;
            out["C"] = s.at("C");
            out["LFE"] = s.at("LFE");
            if (layout == 3) {
                out["Ltf"] = ltf2;
                out["Rtf"] = rtf2;
            } else if (layout == 4) {
                out["Ltf"] = s.at("Ltf");
                out["Rtf"] = s.at("Rtf");
                out["Ltr"] = s.at("Ltr");
                out["Rtr"] = s.at("Rtr");
            }
            break;
        default:  // 5, 6, 7: the 7.1 family
            for (const char* name : {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs"}) {
                out[name] = s.at(name);
            }
            if (layout == 6) {
                out["Ltf"] = ltf2;
                out["Rtf"] = rtf2;
            } else if (layout == 7) {
                for (const char* name : {"Ltf", "Rtf", "Ltb", "Rtb"}) {
                    out[name] = s.at(name);
                }
            }
            break;
    }
    return out;
}

// The channel names of a layout, in the order of its substreams.
[[nodiscard]] std::vector<std::string> names_of(std::uint8_t layout) {
    std::vector<std::string> names;
    for (const auto& s : iamf::layout_info(layout)->substreams) {
        names.emplace_back(s.first);
        if (!s.second.empty()) {
            names.emplace_back(s.second);
        }
    }
    return names;
}

// One Channel Group: its substreams, each one name (mono) or two (coupled).
using Group = std::vector<std::vector<std::string>>;

struct Chain {
    std::vector<std::uint8_t> layouts;
    std::uint8_t source;
    std::vector<Group> groups;  // 3.6.2.2 and 3.6.2.3, written out
};

// Output gain: the flags a Channel Group names and the dB the encoder took off them.
struct Attenuation {
    std::uint8_t flags = 0;
    double db = 0.0;
};

struct Recon {
    std::uint32_t flags = 0;          // recon_gain_flags of the last layer
    std::vector<std::uint8_t> gains;  // one per set flag, in bit order
};

struct Built {
    iamf::Sequence sequence;
    std::vector<Channels> truth;  // per layer: the down-mix of the whole programme
    Channels source;              // the input channels, whole programme
};

class Noise {
   public:
    explicit Noise(std::uint32_t seed) : state_(seed) {}
    double next() {
        state_ = state_ * 1664525U + 1013904223U;
        return (static_cast<double>(state_ >> 8) / 8388608.0 - 1.0) * 0.12;
    }

   private:
    std::uint32_t state_;
};

void put_pcm24(iamf::Bytes& out, double value) {
    const auto scaled =
        static_cast<std::int32_t>(std::lround(std::clamp(value, -1.0, 0.999999) * 8388608.0));
    const auto bits = static_cast<std::uint32_t>(scaled);
    for (int b = 0; b < 3; ++b) {
        out.push_back(static_cast<std::byte>((bits >> (8 * b)) & 0xFFU));
    }
}

[[nodiscard]] Built build(const Chain& chain, std::size_t frames,
                          const std::vector<std::uint8_t>& modes,
                          const std::map<std::size_t, Attenuation>& attenuation = {},
                          const Recon* recon = nullptr, const std::string& codec_id = "ipcm") {
    Built built;
    // The programme: noise on every channel of the source layout.
    Noise noise(0x1A2B3C4DU);
    for (const std::string& name : names_of(chain.source)) {
        Signal& channel = built.source[name];
        channel.resize(frames * kFrameSamples);
        for (double& v : channel) {
            v = noise.next();
        }
    }
    const std::size_t layer_count = chain.layouts.size();
    built.truth.resize(layer_count);

    // A template: the 7.1.4 programme's codec config, Mix Presentation and parameter ids.
    iamf::AudioTrack track;
    track.sample_rate = kSampleRate;
    track.bit_depth = 24;
    track.samples_per_frame = static_cast<std::uint32_t>(kFrameSamples);
    iamf::Frame silent;
    for (auto& channel : silent.channels) {
        channel.assign(kFrameSamples, 0.0F);
    }
    auto base = iamf::build_sequence(track, std::span(&silent, 1));
    REQUIRE(base.has_value());
    built.sequence = std::move(*base);
    built.sequence.temporal_units.clear();
    if (codec_id != "ipcm") {
        iamf::CodecConfig& codec = built.sequence.codec_configs[0];
        codec.codec_id = codec_id;
        codec.lpcm.reset();
        codec.audio_roll_distance = -1;
        if (codec_id == "Opus") {
            // 3.13.1: version 1, 2 channels, pre-skip, 48000 Hz, gain 0, mapping family 0 -
            // big-endian.
            codec.decoder_config = {std::byte{1}, std::byte{2}, std::byte{0},    std::byte{0},
                                    std::byte{0}, std::byte{0}, std::byte{0xBB}, std::byte{0x80},
                                    std::byte{0}, std::byte{0}, std::byte{0}};
        }
    }

    iamf::AudioElement element;
    element.audio_element_id = 0;
    element.type = iamf::ElementType::kChannelBased;
    element.codec_config_id = 0;
    std::uint32_t substream = 0;
    std::vector<std::vector<std::uint32_t>> group_ids;
    for (std::size_t i = 0; i < layer_count; ++i) {
        iamf::ChannelLayer layer;
        layer.loudspeaker_layout = chain.layouts[i];
        layer.substream_count = static_cast<std::uint8_t>(chain.groups[i].size());
        for (const auto& names : chain.groups[i]) {
            if (names.size() == 2) {
                ++layer.coupled_substream_count;
            }
        }
        if (const auto it = attenuation.find(i); it != attenuation.end()) {
            layer.output_gain_is_present = true;
            layer.output_gain_flags = it->second.flags;
            layer.output_gain = iamf::double_to_q7_8(it->second.db);
        }
        if (recon != nullptr && i + 1 == layer_count) {
            layer.recon_gain_is_present = true;
        }
        element.layers.push_back(layer);
        group_ids.emplace_back();
        for (std::size_t s = 0; s < chain.groups[i].size(); ++s) {
            element.audio_substream_ids.push_back(substream);
            group_ids.back().push_back(substream++);
        }
    }
    iamf::DemixingParamDefinition demixing;
    demixing.definition.parameter_id = kDemixId;
    demixing.definition.parameter_rate = kSampleRate;
    demixing.definition.duration = kFrameSamples;
    demixing.definition.constant_subblock_duration = kFrameSamples;
    element.demixing = demixing;
    if (recon != nullptr) {
        iamf::ReconGainParamDefinition definition;
        definition.definition = demixing.definition;
        definition.definition.parameter_id = kReconId;
        element.recon_gain = definition;
    }
    built.sequence.audio_elements[0] = element;

    int w_index = 0;
    for (std::size_t k = 0; k < frames; ++k) {
        const Mode mode = mode_of(modes[k % modes.size()]);
        w_index = std::clamp(w_index + mode.offset, 0, 10);
        const double w = kWeights[static_cast<std::size_t>(w_index)];

        Channels slice;
        for (const auto& [name, signal] : built.source) {
            slice[name].assign(
                signal.begin() + static_cast<std::ptrdiff_t>(k * kFrameSamples),
                signal.begin() + static_cast<std::ptrdiff_t>((k + 1) * kFrameSamples));
        }
        std::vector<Channels> layers;
        for (const std::uint8_t layout : chain.layouts) {
            layers.push_back(downmix(layout, chain.source, slice, mode, w));
        }
        for (std::size_t i = 0; i < layer_count; ++i) {
            for (const auto& [name, signal] : layers[i]) {
                Signal& whole = built.truth[i][name];
                whole.insert(whole.end(), signal.begin(), signal.end());
            }
        }
        const Channels stereo = downmix(1, chain.source, slice, mode, w);

        iamf::TemporalUnit unit;
        iamf::ParameterBlock demix;
        demix.parameter_id = kDemixId;
        iamf::ParameterSubblock sub;
        sub.dmixp_mode = modes[k % modes.size()];
        demix.subblocks.push_back(sub);
        unit.parameter_blocks.push_back(demix);
        if (recon != nullptr) {
            iamf::ParameterBlock block;
            block.parameter_id = kReconId;
            iamf::ParameterSubblock recon_sub;
            recon_sub.recon_layers.resize(layer_count);
            recon_sub.recon_layers.back().recon_gain_flags = recon->flags;
            recon_sub.recon_layers.back().recon_gains = recon->gains;
            block.subblocks.push_back(recon_sub);
            unit.parameter_blocks.push_back(block);
        }
        for (std::size_t i = 0; i < layer_count; ++i) {
            const auto found = attenuation.find(i);
            for (std::size_t s = 0; s < chain.groups[i].size(); ++s) {
                const auto& names = chain.groups[i][s];
                std::vector<Signal> values;
                for (const std::string& name : names) {
                    Signal v = name == "L2" ? stereo.at("L") : layers[i].at(name);
                    if (found != attenuation.end()) {
                        // Output gain flags, MSB first: L, R, Ls, Rs, Ltf, Rtf.
                        constexpr std::array<const char*, 6> kNames{"Rtf", "Ltf", "Rs",
                                                                    "Ls",  "R",   "L"};
                        for (std::size_t bit = 0; bit < kNames.size(); ++bit) {
                            if (((found->second.flags >> bit) & 1U) != 0 && name == kNames[bit]) {
                                const double factor = std::pow(10.0, -found->second.db / 20.0);
                                for (double& x : v) {
                                    x *= factor;
                                }
                            }
                        }
                    }
                    values.push_back(std::move(v));
                }
                iamf::AudioFrame frame;
                frame.audio_substream_id = group_ids[i][s];
                for (std::size_t n = 0; n < kFrameSamples; ++n) {
                    for (const Signal& v : values) {
                        put_pcm24(frame.data, v[n]);
                    }
                }
                unit.audio_frames.push_back(std::move(frame));
            }
        }
        built.sequence.temporal_units.push_back(std::move(unit));
    }
    return built;
}

// The sequence as a file would hold it: through the OBU writer and reader.
[[nodiscard]] iamf::Sequence round_trip(const iamf::Sequence& sequence) {
    auto bytes = iamf::write_sequence(sequence);
    REQUIRE(bytes.has_value());
    auto parsed = iamf::read_sequence(*bytes);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

void expect_layer(const iamf::DecodedElement& decoded, std::uint8_t layout, const Channels& truth,
                  double tolerance = 1e-5) {
    REQUIRE(decoded.channel_names == names_of(layout));
    for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
        const Signal& expected = truth.at(decoded.channel_names[c]);
        REQUIRE(decoded.channels[c].size() == expected.size());
        double worst = 0.0;
        for (std::size_t n = 0; n < expected.size(); ++n) {
            worst = std::max(worst,
                             std::abs(static_cast<double>(decoded.channels[c][n]) - expected[n]));
        }
        INFO("layout " << int(layout) << " channel " << decoded.channel_names[c]);
        CHECK(worst < tolerance);
    }
}

// The chains of the tests. The groups follow 3.6.2.2: Channel Group #1 is CL #1 itself; Channel
// Group #i adds L2 when Mono grows to Stereo, the centre when 3 is reached, L5/R5 at 5, Lss/Rss at
// 7, the LFE when it appears and the top channels (all of them from none, the front pair from 2 to
// 4). Within a group the coupled pairs come first, surround before top, then C, LFE and L.
[[nodiscard]] std::vector<std::pair<std::string, Chain>> chains() {
    return {
        {"2ch / 3.1.2 / 5.1.2 / 7.1.4",
         {{1, 8, 3, 7},
          7,
          {{{"L", "R"}},
           {{"Ltf", "Rtf"}, {"C"}, {"LFE"}},
           {{"L", "R"}},
           {{"Lss", "Rss"}, {"Ltf", "Rtf"}}}}},
        {"Mono / 2ch / 7.1.4",
         {{0, 1, 7},
          7,
          {{{"C"}},
           {{"L2"}},
           {{"L", "R"}, {"Lss", "Rss"}, {"Ltf", "Rtf"}, {"Ltb", "Rtb"}, {"C"}, {"LFE"}}}}},
        {"5.1.2 / 7.1.4",
         {{3, 7},
          7,
          {{{"L", "R"}, {"Ls", "Rs"}, {"Ltf", "Rtf"}, {"C"}, {"LFE"}},
           {{"Lss", "Rss"}, {"Ltf", "Rtf"}}}}},
        {"5.1 / 7.1", {{2, 5}, 7, {{{"L", "R"}, {"Ls", "Rs"}, {"C"}, {"LFE"}}, {{"Lss", "Rss"}}}}},
        {"3.1.2 / 5.1.4",
         {{8, 4}, 4, {{{"L", "R"}, {"Ltf", "Rtf"}, {"C"}, {"LFE"}}, {{"L", "R"}, {"Ltf", "Rtf"}}}}},
        {"7.1 / 7.1.4",
         {{5, 7},
          7,
          {{{"L", "R"}, {"Lss", "Rss"}, {"Lrs", "Rrs"}, {"C"}, {"LFE"}},
           {{"Ltf", "Rtf"}, {"Ltb", "Rtb"}}}}},
        {"2ch / 5.1 / 5.1.2",
         {{1, 2, 3}, 3, {{{"L", "R"}}, {{"L", "R"}, {"C"}, {"LFE"}}, {{"Ltf", "Rtf"}}}}},
        {"Mono / 5.1", {{0, 2}, 2, {{{"C"}}, {{"L", "R"}, {"C"}, {"LFE"}, {"L2"}}}}},
        {"3.1.2 / 5.1.2",
         {{8, 3}, 3, {{{"L", "R"}, {"Ltf", "Rtf"}, {"C"}, {"LFE"}}, {{"L", "R"}}}}},
        {"2ch / 3.1.2 / 7.1.4",
         {{1, 8, 7},
          7,
          {{{"L", "R"}},
           {{"Ltf", "Rtf"}, {"C"}, {"LFE"}},
           {{"L", "R"}, {"Lss", "Rss"}, {"Ltf", "Rtf"}}}}},
    };
}

}  // namespace

TEST_CASE("IAMF scalable channel audio reconstructs every layer of each chain",
          "[iamf][scalable]") {
    for (const auto& [title, chain] : chains()) {
        DYNAMIC_SECTION(title) {
            const Built built = build(chain, 5, {4, 4, 4, 5, 0});
            const iamf::Sequence sequence = round_trip(built.sequence);
            for (std::size_t layer = 0; layer < chain.layouts.size(); ++layer) {
                auto decoded = iamf::decode_pcm(sequence, 0, {.layer = layer});
                REQUIRE(decoded.has_value());
                CHECK(decoded->sample_rate == kSampleRate);
                expect_layer(*decoded, chain.layouts[layer], built.truth[layer]);
            }
            // With no layer asked for it is the last one.
            auto full = iamf::decode_pcm(sequence, 0);
            REQUIRE(full.has_value());
            expect_layer(*full, chain.layouts.back(), built.truth.back());
        }
    }
}

TEST_CASE("IAMF scalable reconstruction follows each frame's demixing mode", "[iamf][scalable]") {
    // Every mode the table lists, one per frame, so alpha, beta, gamma, delta and w(k) all change.
    const auto all = chains();
    const Chain& chain = all[0].second;
    const Built built = build(chain, 6, {0, 1, 2, 4, 5, 6});
    const iamf::Sequence sequence = round_trip(built.sequence);
    auto decoded = iamf::decode_pcm(sequence, 0);
    REQUIRE(decoded.has_value());
    expect_layer(*decoded, 7, built.truth.back());
}

TEST_CASE("IAMF scalable reconstruction uses the defaults when no demixing Parameter Block is sent",
          "[iamf][scalable]") {
    // The encoder used mode 2 (alpha 1, beta, gamma and delta 0.866, w_idx_offset -1), which holds
    // wIdx(k) at 0 so w(k) is 0 throughout. The Parameter Blocks are dropped and the definition's
    // defaults, default_dmixp_mode 2 and default_w 0, have to stand in for them.
    const auto all = chains();
    const Chain& chain = all[0].second;
    Built built = build(chain, 3, {2});
    built.sequence.audio_elements[0].demixing->default_dmixp_mode = 2;
    built.sequence.audio_elements[0].demixing->default_w = 0;
    for (auto& unit : built.sequence.temporal_units) {
        unit.parameter_blocks.clear();
    }
    const iamf::Sequence sequence = round_trip(built.sequence);
    auto decoded = iamf::decode_pcm(sequence, 0);
    REQUIRE(decoded.has_value());
    expect_layer(*decoded, 7, built.truth.back());
}

TEST_CASE("IAMF scalable reconstruction undoes the output gain on the mixed channels",
          "[iamf][scalable]") {
    const auto all = chains();
    const Chain& chain = all[0].second;
    // Group 1 (L2, R2): both mixed, 4.5 dB off. Group 2 (Ltf3, Rtf3): 3 dB off.
    const std::map<std::size_t, Attenuation> attenuation{{0, {0x30, 4.5}}, {1, {0x03, 3.0}}};
    const Built built = build(chain, 4, {4, 5, 0, 4}, attenuation);
    const iamf::Sequence sequence = round_trip(built.sequence);
    for (std::size_t layer = 0; layer < chain.layouts.size(); ++layer) {
        auto decoded = iamf::decode_pcm(sequence, 0, {.layer = layer});
        REQUIRE(decoded.has_value());
        expect_layer(*decoded, chain.layouts[layer], built.truth[layer]);
    }
}

TEST_CASE("IAMF scalable reconstruction smooths the recon gain over the frames",
          "[iamf][scalable]") {
    const auto all = chains();
    const Chain& chain = all[0].second;
    // The channels the last layer demixes: Lrs (b7), Rrs (b8), Ltb (b9), Rtb (b10).
    const Recon recon{(1U << 7) | (1U << 8) | (1U << 9) | (1U << 10), {200, 128, 255, 100}};
    const Built built = build(chain, 3, {4}, {}, &recon);
    const iamf::Sequence sequence = round_trip(built.sequence);

    auto plain = iamf::decode_pcm(sequence, 0, {.apply_recon_gain = false});
    REQUIRE(plain.has_value());
    expect_layer(*plain, 7, built.truth.back());
    auto gained = iamf::decode_pcm(sequence, 0);
    REQUIRE(gained.has_value());
    REQUIRE(gained->channel_names == plain->channel_names);

    const auto channel_index = [&](const std::string& name) {
        return static_cast<std::size_t>(
            std::find(gained->channel_names.begin(), gained->channel_names.end(), name) -
            gained->channel_names.begin());
    };
    // Channels with no recon gain are untouched, the flagged ones scaled.
    for (const char* name : {"L", "R", "C", "Lss", "Rss", "Ltf", "Rtf", "LFE"}) {
        CHECK(gained->channels[channel_index(name)] == plain->channels[channel_index(name)]);
    }
    // MA_gain(k) = 0.25 * gain + 0.75 * MA_gain(k - 1) with MA_gain(0) = 1, and past the 64 sample
    // overlap the frame holds MA_gain(k).
    const auto ratio_at = [&](const char* name, std::size_t frame, std::size_t n) {
        const std::size_t i = channel_index(name);
        const std::size_t at = frame * kFrameSamples + n;
        return static_cast<double>(gained->channels[i][at]) /
               static_cast<double>(plain->channels[i][at]);
    };
    // Lrs: gain 200 / 255.
    const double g_lrs = 200.0 / 255.0;
    const double ma1 = 0.25 * g_lrs + 0.75;
    const double ma2 = 0.25 * g_lrs + 0.75 * ma1;
    CHECK(ratio_at("Lrs", 0, 100) == Catch::Approx(ma1).margin(1e-5));
    CHECK(ratio_at("Lrs", 1, 100) == Catch::Approx(ma2).margin(1e-5));
    // Ltb has gain 255, which is unity, whatever the frame.
    CHECK(ratio_at("Ltb", 2, 150) == Catch::Approx(1.0).margin(1e-5));
    // Rtb: 100 / 255 settles lower than Lrs.
    CHECK(ratio_at("Rtb", 2, 150) < ratio_at("Lrs", 2, 150));
    // Inside the overlap the previous frame's average falls out as the new one rises in: the first
    // sample of frame 1 is (almost) MA_gain(1), the last of the 64 is (almost) MA_gain(2).
    const double first = ratio_at("Lrs", 1, 0);
    CHECK(first == Catch::Approx(ma1).margin(2e-3));
    const double last_in_overlap = ratio_at("Lrs", 1, 63);
    CHECK(last_in_overlap == Catch::Approx(ma2).margin(2e-3));
}

TEST_CASE("IAMF reconstruct_channels takes externally decoded substreams", "[iamf][scalable]") {
    const auto all = chains();
    const Chain& chain = all[0].second;
    const Built built = build(chain, 4, {4, 5, 0, 4});

    // The caller's decoder returns each substream's channels: here the encoder's own signals.
    std::vector<iamf::SubstreamPcm> decoded;
    std::uint32_t id = 0;
    const Built reference = built;
    for (std::size_t layer = 0; layer < chain.layouts.size(); ++layer) {
        for (const auto& names : chain.groups[layer]) {
            // Read the substream from the file bytes instead of the encoder's variables, so the
            // codec stage is what a decoder would hand back: 24 bit PCM as float.
            iamf::SubstreamPcm pcm;
            pcm.audio_substream_id = id++;
            pcm.channels.resize(names.size());
            decoded.push_back(std::move(pcm));
        }
    }
    for (const iamf::TemporalUnit& unit : built.sequence.temporal_units) {
        for (const iamf::AudioFrame& frame : unit.audio_frames) {
            const std::size_t channels = decoded[frame.audio_substream_id].channels.size();
            for (std::size_t n = 0; n < kFrameSamples; ++n) {
                for (std::size_t c = 0; c < channels; ++c) {
                    const std::size_t at = (n * channels + c) * 3;
                    std::int32_t raw = static_cast<std::int32_t>(
                        std::to_integer<std::uint32_t>(frame.data[at]) |
                        (std::to_integer<std::uint32_t>(frame.data[at + 1]) << 8) |
                        (std::to_integer<std::uint32_t>(frame.data[at + 2]) << 16));
                    if ((raw & 0x800000) != 0) {
                        raw -= 0x1000000;
                    }
                    decoded[frame.audio_substream_id].channels[c].push_back(
                        static_cast<float>(raw) / 8388608.0F);
                }
            }
        }
    }
    auto from_pcm = iamf::reconstruct_channels(built.sequence, 0, decoded);
    REQUIRE(from_pcm.has_value());
    expect_layer(*from_pcm, 7, reference.truth.back());
    auto from_obus = iamf::decode_pcm(built.sequence, 0);
    REQUIRE(from_obus.has_value());
    CHECK(from_pcm->channels == from_obus->channels);

    SECTION("a substream the caller left out") {
        decoded.pop_back();
        auto result = iamf::reconstruct_channels(built.sequence, 0, decoded);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::Error::kInvalidArgument);
    }
    SECTION("a substream of the wrong length") {
        decoded.back().channels[0].pop_back();
        auto result = iamf::reconstruct_channels(built.sequence, 0, decoded);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::Error::kInvalidArgument);
    }
}

TEST_CASE("IAMF scalable reconstruction applies the trimming of the Temporal Units",
          "[iamf][scalable]") {
    const auto all = chains();
    const Chain& chain = all[2].second;  // 5.1.2 / 7.1.4
    Built built = build(chain, 4, {4});
    for (auto& frame : built.sequence.temporal_units.front().audio_frames) {
        frame.has_trimming = true;
        frame.num_samples_to_trim_at_start = 50;
    }
    for (auto& frame : built.sequence.temporal_units.back().audio_frames) {
        frame.has_trimming = true;
        frame.num_samples_to_trim_at_end = 30;
    }
    const iamf::Sequence sequence = round_trip(built.sequence);
    auto decoded = iamf::decode_pcm(sequence, 0);
    REQUIRE(decoded.has_value());
    const std::size_t expected_length = 4 * kFrameSamples - 50 - 30;
    for (std::size_t c = 0; c < decoded->channels.size(); ++c) {
        REQUIRE(decoded->channels[c].size() == expected_length);
        const Signal& truth = built.truth.back().at(decoded->channel_names[c]);
        for (std::size_t n = 0; n < expected_length; n += 97) {
            CHECK(std::abs(static_cast<double>(decoded->channels[c][n]) - truth[50 + n]) < 1e-5);
        }
    }
}

TEST_CASE("IAMF scalable reconstruction uses the Opus overlap for an Opus Codec Config",
          "[iamf][scalable]") {
    const auto all = chains();
    const Chain& chain = all[0].second;
    const Recon recon{(1U << 7), {128}};
    Built built = build(chain, 3, {4}, {}, &recon, "Opus");
    // The substreams are read from the PCM the encoder wrote, as an Opus decoder would return them.
    std::vector<iamf::SubstreamPcm> decoded;
    std::uint32_t id = 0;
    for (std::size_t layer = 0; layer < chain.layouts.size(); ++layer) {
        for (const auto& names : chain.groups[layer]) {
            iamf::SubstreamPcm pcm;
            pcm.audio_substream_id = id++;
            pcm.channels.resize(names.size());
            decoded.push_back(std::move(pcm));
        }
    }
    for (const iamf::TemporalUnit& unit : built.sequence.temporal_units) {
        for (const iamf::AudioFrame& frame : unit.audio_frames) {
            auto& pcm = decoded[frame.audio_substream_id];
            const std::size_t channels = pcm.channels.size();
            for (std::size_t n = 0; n < kFrameSamples; ++n) {
                for (std::size_t c = 0; c < channels; ++c) {
                    const std::size_t at = (n * channels + c) * 3;
                    std::int32_t raw = static_cast<std::int32_t>(
                        std::to_integer<std::uint32_t>(frame.data[at]) |
                        (std::to_integer<std::uint32_t>(frame.data[at + 1]) << 8) |
                        (std::to_integer<std::uint32_t>(frame.data[at + 2]) << 16));
                    if ((raw & 0x800000) != 0) {
                        raw -= 0x1000000;
                    }
                    pcm.channels[c].push_back(static_cast<float>(raw) / 8388608.0F);
                }
            }
        }
    }
    auto plain =
        iamf::reconstruct_channels(built.sequence, 0, decoded, {.apply_recon_gain = false});
    auto gained = iamf::reconstruct_channels(built.sequence, 0, decoded);
    REQUIRE(plain.has_value());
    REQUIRE(gained.has_value());
    CHECK(gained->sample_rate == 48000);  // Opus: offsets at 48 kHz

    const std::size_t lrs = static_cast<std::size_t>(
        std::find(gained->channel_names.begin(), gained->channel_names.end(), "Lrs") -
        gained->channel_names.begin());
    // Frame 1, sample 0: MA(1) * hann(60) + MA(2) * hann(0), with a 60 sample overlap.
    const double g = 128.0 / 255.0;
    const double ma1 = 0.25 * g + 0.75;
    const double ma2 = 0.25 * g + 0.75 * ma1;
    const double hann_at_overlap =
        0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * 60.0 / 119.0);
    const double expected = ma1 * hann_at_overlap;  // hann(0) is 0
    const std::size_t at = kFrameSamples;
    const double ratio = static_cast<double>(gained->channels[lrs][at]) /
                         static_cast<double>(plain->channels[lrs][at]);
    CHECK(ratio == Catch::Approx(expected).margin(1e-4));
    // Sample 60 is past the overlap, so it holds MA(2) exactly.
    const double later = static_cast<double>(gained->channels[lrs][at + 60]) /
                         static_cast<double>(plain->channels[lrs][at + 60]);
    CHECK(later == Catch::Approx(ma2).margin(1e-4));
}

TEST_CASE("IAMF scalable reconstruction refuses what it cannot rebuild", "[iamf][scalable]") {
    const auto all = chains();
    const Chain& chain = all[0].second;
    Built built = build(chain, 2, {4});

    SECTION("a layer past the last") {
        auto decoded = iamf::decode_pcm(built.sequence, 0, {.layer = 4});
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == iamf::Error::kInvalidArgument);
    }
    SECTION("layers that shrink") {
        // 3.1.2 followed by 5.1: the height channels would have to disappear (3.6.2.1).
        built.sequence.audio_elements[0].layers[2].loudspeaker_layout = 2;
        auto decoded = iamf::decode_pcm(built.sequence, 0);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == iamf::Error::kBadDescriptor);
    }
    SECTION("a substream count the layers do not account for") {
        built.sequence.audio_elements[0].layers[1].substream_count = 2;
        auto decoded = iamf::decode_pcm(built.sequence, 0);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == iamf::Error::kBadDescriptor);
    }
    SECTION("a layout that cannot be one of several layers") {
        built.sequence.audio_elements[0].layers[1].loudspeaker_layout = 9;  // binaural
        auto decoded = iamf::decode_pcm(built.sequence, 0);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == iamf::Error::kUnsupported);
    }
    SECTION("a frame that is not the Codec Config's length") {
        built.sequence.codec_configs[0].num_samples_per_frame = 100;
        auto decoded = iamf::decode_pcm(built.sequence, 0);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == iamf::Error::kBadObu);
    }
}

TEST_CASE("IAMF decode_pcm reads the expanded loudspeaker layouts", "[iamf][scalable]") {
    // The channel counts and coupled pairs 3.6.1's table gives, expanded_loudspeaker_layout 0
    // to 19.
    struct Expected {
        std::uint8_t id;
        std::uint8_t channels;
        std::uint8_t coupled;
    };
    constexpr std::array<Expected, 20> kTable{
        {{0, 1, 0},  {1, 2, 1},  {2, 2, 1},   {3, 2, 1},  {4, 2, 1},  {5, 2, 1},  {6, 4, 2},
         {7, 3, 1},  {8, 16, 7}, {9, 2, 1},   {10, 2, 1}, {11, 2, 1}, {12, 6, 3}, {13, 24, 8},
         {14, 2, 0}, {15, 3, 1}, {16, 17, 7}, {17, 4, 2}, {18, 1, 0}, {19, 5, 2}}};
    for (const Expected& e : kTable) {
        const auto info = iamf::expanded_layout_info(e.id);
        REQUIRE(info.has_value());
        INFO(info->name);
        CHECK(info->channel_count == e.channels);
        CHECK(info->coupled_substream_count == e.coupled);
    }
    CHECK_FALSE(iamf::expanded_layout_info(20).has_value());

    // 7.1.5.4ch: ten substreams in the order 3.6.2.3 gives as its example.
    const auto info = *iamf::expanded_layout_info(16);
    REQUIRE(info.substreams.size() == 10);
    CHECK(info.substreams[0].first == "L");
    CHECK(info.substreams[5].first == "BtFL");
    CHECK(info.substreams[7].first == "C");
    CHECK(info.substreams[8].first == "TpC");
    CHECK(info.substreams[9].first == "LFE");

    // A programme in 7.1.5.4ch (and one in Top-4ch) round trips through decode_pcm().
    for (const std::uint8_t expanded : {std::uint8_t{16}, std::uint8_t{6}}) {
        const auto layout = *iamf::expanded_layout_info(expanded);
        iamf::AudioTrack track;
        track.samples_per_frame = 64;
        iamf::Frame silent;
        for (auto& channel : silent.channels) {
            channel.assign(64, 0.0F);
        }
        auto base = iamf::build_sequence(track, std::span(&silent, 1));
        REQUIRE(base.has_value());
        iamf::Sequence sequence = std::move(*base);
        iamf::AudioElement element;
        element.audio_element_id = 0;
        element.type = iamf::ElementType::kChannelBased;
        element.codec_config_id = 0;
        iamf::ChannelLayer layer;
        layer.loudspeaker_layout = 15;
        layer.expanded_loudspeaker_layout = expanded;
        layer.substream_count = static_cast<std::uint8_t>(layout.substreams.size());
        layer.coupled_substream_count = layout.coupled_substream_count;
        element.layers.push_back(layer);
        iamf::TemporalUnit unit;
        for (std::uint32_t s = 0; s < layout.substreams.size(); ++s) {
            element.audio_substream_ids.push_back(s);
            iamf::AudioFrame frame;
            frame.audio_substream_id = s;
            const std::size_t channels = layout.substreams[s].second.empty() ? 1 : 2;
            for (std::size_t n = 0; n < 64; ++n) {
                for (std::size_t c = 0; c < channels; ++c) {
                    // A value that names the substream and channel: s * 2 + c, in 1/64ths.
                    put_pcm24(frame.data, static_cast<double>(s * 2 + c + 1) / 64.0);
                }
            }
            unit.audio_frames.push_back(std::move(frame));
        }
        sequence.audio_elements[0] = element;
        sequence.temporal_units = {unit};
        auto decoded = iamf::decode_pcm(round_trip(sequence), 0);
        REQUIRE(decoded.has_value());
        std::vector<std::string> names;
        for (const auto& s : layout.substreams) {
            names.emplace_back(s.first);
            if (!s.second.empty()) {
                names.emplace_back(s.second);
            }
        }
        CHECK(decoded->channel_names == names);
        REQUIRE(decoded->channels.size() == layout.channel_count);
        std::size_t index = 0;
        for (std::uint32_t s = 0; s < layout.substreams.size(); ++s) {
            const std::size_t channels = layout.substreams[s].second.empty() ? 1 : 2;
            for (std::size_t c = 0; c < channels; ++c, ++index) {
                REQUIRE(decoded->channels[index].size() == 64);
                CHECK(decoded->channels[index][10] ==
                      Catch::Approx(static_cast<double>(s * 2 + c + 1) / 64.0).margin(1e-6));
            }
        }
    }
}
