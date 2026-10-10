#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/containers/iamf/iamf.hpp"
#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"
#include "sequence_detail.hpp"

// Scalable channel audio reconstruction: IAMF v2.0.0 section 7.2 (the Gain, De-mixer and Recon
// Gain modules) over the Channel Groups section 3.6.2.2 lays out. The encoder side the
// specification describes in Annex A2 (down-mix and recon gain generation) is informative; this is
// the decoder side, which is normative, written from the section 7.2 formulas.

namespace iclforge::containers::iamf::detail {

namespace {

constexpr std::array<std::uint32_t, 13> kAacSampleRates{
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

[[nodiscard]] std::optional<std::uint32_t> flac_sample_rate(const Bytes& bytes) {
    // 3.13.3: the Metadata Blocks of RFC 9639, so a 4 byte block header then STREAMINFO: block
    // sizes (2 x 16 bits), frame sizes (2 x 24 bits) and then the 20 bit sample rate. A bare
    // STREAMINFO is accepted as well.
    std::size_t start = 0;
    if (bytes.size() >= 4 + 13 && (std::to_integer<unsigned>(bytes[0]) & 0x7FU) == 0) {
        start = 4;
    } else if (bytes.size() < 13) {
        return std::nullopt;
    }
    const auto at = [&](std::size_t i) { return std::to_integer<std::uint32_t>(bytes[start + i]); };
    const std::uint32_t rate = (at(10) << 12) | (at(11) << 4) | (at(12) >> 4);
    return rate == 0 ? std::nullopt : std::optional<std::uint32_t>(rate);
}

// An MPEG-4 Systems expandable size: 7 bits per byte, a set top bit continuing, at most 4 bytes.
[[nodiscard]] std::optional<std::uint32_t> read_expandable(const Bytes& bytes, std::size_t& pos) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= bytes.size()) {
            return std::nullopt;
        }
        const auto b = std::to_integer<std::uint32_t>(bytes[pos++]);
        value = (value << 7) | (b & 0x7FU);
        if ((b & 0x80U) == 0) {
            return value;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> aac_sample_rate(const Bytes& bytes) {
    // 3.13.2: a DecoderConfigDescriptor (tag 0x04) holding objectTypeIndication, streamType,
    // bufferSizeDB, maxBitrate and avgBitrate (13 bytes) and then the DecoderSpecificInfo (tag
    // 0x05) whose AudioSpecificConfig starts with a 5 bit audioObjectType and a 4 bit
    // samplingFrequencyIndex.
    std::size_t pos = 0;
    if (bytes.empty() || std::to_integer<unsigned>(bytes[pos++]) != 0x04U) {
        return std::nullopt;
    }
    if (!read_expandable(bytes, pos).has_value() || pos + 13 > bytes.size()) {
        return std::nullopt;
    }
    pos += 13;
    if (pos >= bytes.size() || std::to_integer<unsigned>(bytes[pos++]) != 0x05U) {
        return std::nullopt;
    }
    if (!read_expandable(bytes, pos).has_value() || pos + 2 > bytes.size()) {
        return std::nullopt;
    }
    const unsigned asc =
        (std::to_integer<unsigned>(bytes[pos]) << 8) | std::to_integer<unsigned>(bytes[pos + 1]);
    if ((asc >> 11) == 31U) {
        return std::nullopt;  // an escaped audioObjectType: not AAC-LC's 2
    }
    const unsigned index = (asc >> 7) & 0xFU;
    if (index >= kAacSampleRates.size()) {
        return std::nullopt;
    }
    return kAacSampleRates[index];
}

}  // namespace

std::optional<std::uint32_t> codec_sample_rate(const CodecConfig& codec) {
    if (codec.codec_id == "ipcm") {
        if (!codec.lpcm.has_value()) {
            return std::nullopt;
        }
        return codec.lpcm->sample_rate;
    }
    if (codec.codec_id == "Opus") {
        return 48000;  // 3.13.1: the sample rate offsets are computed at
    }
    if (codec.codec_id == "mp4a") {
        return aac_sample_rate(codec.decoder_config);
    }
    if (codec.codec_id == "fLaC") {
        return flac_sample_rate(codec.decoder_config);
    }
    return std::nullopt;
}

std::expected<std::vector<std::vector<float>>, Error> decode_ipcm_frame(
    std::span<const std::byte> data, const LpcmConfig& lpcm, std::size_t channels) {
    if (lpcm.sample_size != 16 && lpcm.sample_size != 24 && lpcm.sample_size != 32) {
        return std::unexpected(Error::kBadDescriptor);
    }
    const unsigned bytes_per_sample = lpcm.sample_size / 8U;
    const bool little_endian = (lpcm.sample_format_flags & 1U) != 0;
    const std::size_t frame_bytes = static_cast<std::size_t>(bytes_per_sample) * channels;
    if (frame_bytes == 0 || data.size() % frame_bytes != 0) {
        return std::unexpected(Error::kBadObu);
    }
    const std::size_t samples = data.size() / frame_bytes;
    const double scale = 1.0 / static_cast<double>(std::uint64_t{1} << (lpcm.sample_size - 1));
    std::vector<std::vector<float>> planar(channels);
    for (auto& channel : planar) {
        channel.reserve(samples);
    }
    for (std::size_t n = 0; n < samples; ++n) {
        for (std::size_t c = 0; c < channels; ++c) {
            std::uint32_t raw = 0;
            const std::size_t at = (n * channels + c) * bytes_per_sample;
            for (unsigned b = 0; b < bytes_per_sample; ++b) {
                const unsigned shift = 8U * (little_endian ? b : bytes_per_sample - 1 - b);
                raw |= std::to_integer<std::uint32_t>(data[at + b]) << shift;
            }
            // Sign-extend the sample_size-bit two's complement value.
            const std::int64_t signed_value =
                static_cast<std::int64_t>(raw) -
                ((raw & (std::uint32_t{1} << (lpcm.sample_size - 1))) != 0
                     ? (std::int64_t{1} << lpcm.sample_size)
                     : 0);
            planar[c].push_back(static_cast<float>(static_cast<double>(signed_value) * scale));
        }
    }
    return planar;
}

}  // namespace iclforge::containers::iamf::detail

namespace iclforge::containers::iamf {

namespace {

using Plane = std::vector<double>;
using Planes = std::map<std::string_view, Plane>;

// 3.9.3: the pre-defined combinations of the five demixing parameters.
struct DemixParams {
    double alpha = 1.0;
    double beta = 1.0;
    double gamma = 0.707;
    double delta = 0.707;
    int w_idx_offset = -1;
};

[[nodiscard]] std::optional<DemixParams> demix_params(std::uint8_t dmixp_mode) {
    switch (dmixp_mode) {
        case 0:
            return DemixParams{1.0, 1.0, 0.707, 0.707, -1};
        case 1:
            return DemixParams{0.707, 0.707, 0.707, 0.707, -1};
        case 2:
            return DemixParams{1.0, 0.866, 0.866, 0.866, -1};
        case 4:
            return DemixParams{1.0, 1.0, 0.707, 0.707, 1};
        case 5:
            return DemixParams{0.707, 0.707, 0.707, 0.707, 1};
        case 6:
            return DemixParams{1.0, 0.866, 0.866, 0.866, 1};
        default:
            return std::nullopt;  // 3 and 7 are reserved
    }
}

// 7.2.2: the mapping of wIdx(k) (and default_w) to w(k).
constexpr std::array<double, 11> kTopWeight{0.0,    0.0179, 0.0391, 0.0658, 0.1038, 0.25,
                                            0.3962, 0.4342, 0.4609, 0.4821, 0.5};

// 7.2.3: the moving average length and the overlap recommended per codec.
constexpr double kAverageLength = 7.0;
constexpr std::size_t kOpusOverlap = 60;
constexpr std::size_t kAacOverlap = 64;

// The Xi.Yi.Zi of a layout in the generation rule (3.6.2.1): surround, LFE and height channels,
// with Mono as one surround channel.
struct Shape {
    int surround = 0;
    int lfe = 0;
    int height = 0;
    bool five_one_four = false;  // the back height pair is named Ltr/Rtr, not Ltb/Rtb
};

[[nodiscard]] std::optional<Shape> shape_of(std::uint8_t loudspeaker_layout) {
    switch (loudspeaker_layout) {
        case 0:
            return Shape{1, 0, 0, false};
        case 1:
            return Shape{2, 0, 0, false};
        case 2:
            return Shape{5, 1, 0, false};
        case 3:
            return Shape{5, 1, 2, false};
        case 4:
            return Shape{5, 1, 4, true};
        case 5:
            return Shape{7, 1, 0, false};
        case 6:
            return Shape{7, 1, 2, false};
        case 7:
            return Shape{7, 1, 4, false};
        case 8:
            return Shape{3, 1, 2, false};
        default:
            return std::nullopt;  // binaural, reserved and expanded values do not scale
    }
}

// The channels the substreams of one Channel Group carry, in substream order.
struct GroupLayout {
    std::vector<SubstreamChannels> substreams;
    std::uint8_t coupled = 0;
};

// 3.6.2.2 and 3.6.2.3: Channel Group #i (i > 1) holds the channels CL #i adds to CL #i-1 that the
// de-mixer cannot derive: L2 when Mono grows to Stereo, the centre, L5 and R5, Lss and Rss, the LFE
// and the new top channels. Coupled pairs come first (surround before top), then the centre, the
// LFE and the L channel.
[[nodiscard]] GroupLayout dependent_group(const Shape& prev, const Shape& now) {
    GroupLayout group;
    if (prev.surround < 5 && now.surround >= 5) {
        group.substreams.push_back({"L", "R"});
    }
    if (prev.surround < 7 && now.surround >= 7) {
        group.substreams.push_back({"Lss", "Rss"});
    }
    if (now.height > prev.height) {
        group.substreams.push_back({"Ltf", "Rtf"});
        if (prev.height == 0 && now.height == 4) {
            group.substreams.push_back(now.five_one_four ? SubstreamChannels{"Ltr", "Rtr"}
                                                         : SubstreamChannels{"Ltb", "Rtb"});
        }
    }
    group.coupled = static_cast<std::uint8_t>(group.substreams.size());
    if (prev.surround < 3 && now.surround >= 3) {
        group.substreams.push_back({"C", ""});
    }
    if (now.lfe > prev.lfe) {
        group.substreams.push_back({"LFE", ""});
    }
    if (prev.surround < 2 && now.surround >= 2) {
        group.substreams.push_back({"L2", ""});
    }
    return group;
}

// 7.2.2: Channel Group #i's channels, joined with what CL #i-1 reconstructed, into CL #i.
// `cur` and the result are named by position: L R C Ls Rs / Lss Rss Lrs Rrs, LFE, Ltf Rtf and the
// back pair.
[[nodiscard]] Planes demix_step(const Planes& cur, const Planes& group, const Shape& prev,
                                const Shape& now, const DemixParams& p, double w, std::size_t n) {
    const Plane zero(n, 0.0);
    const auto get = [&zero](const Planes& planes, std::string_view name) -> const Plane& {
        const auto it = planes.find(name);
        return it != planes.end() ? it->second : zero;
    };
    Planes next;

    if (now.surround == prev.surround) {
        for (const std::string_view name :
             {"L", "R", "C", "Ls", "Rs", "Lss", "Rss", "Lrs", "Rrs"}) {
            if (const auto it = cur.find(name); it != cur.end()) {
                next[name] = it->second;
            }
        }
    } else {
        Plane l;
        Plane r;
        Plane c;
        Plane ls5;
        Plane rs5;
        int stage = prev.surround;
        if (stage == 1) {
            // S1to2: R2 = 2 * Mono - L2
            const Plane& mono = get(cur, "C");
            l = get(group, "L2");
            r.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                r[i] = 2.0 * mono[i] - l[i];
            }
            stage = 2;
        } else {
            l = get(cur, "L");
            r = get(cur, "R");
            if (stage >= 3) {
                c = get(cur, "C");
            }
        }
        if (stage == 2 && now.surround >= 3) {
            // S2to3: L3 = L2 - 0.707 * C
            c = get(group, "C");
            for (std::size_t i = 0; i < n; ++i) {
                l[i] -= 0.707 * c[i];
                r[i] -= 0.707 * c[i];
            }
            stage = 3;
        }
        if (stage == 3 && now.surround >= 5) {
            // S3to5: Ls = (L3 - L5) / delta
            const Plane& l5 = get(group, "L");
            const Plane& r5 = get(group, "R");
            ls5.resize(n);
            rs5.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                ls5[i] = (l[i] - l5[i]) / p.delta;
                rs5[i] = (r[i] - r5[i]) / p.delta;
            }
            l = l5;
            r = r5;
            stage = 5;
        } else if (stage == 5) {
            ls5 = get(cur, "Ls");
            rs5 = get(cur, "Rs");
        }
        next["L"] = l;
        next["R"] = r;
        if (now.surround >= 3) {
            next["C"] = c;
        }
        if (now.surround == 5) {
            next["Ls"] = ls5;
            next["Rs"] = rs5;
        } else if (now.surround == 7) {
            // S5to7: Lrs = (Ls - alpha * Lss) / beta
            const Plane& lss = get(group, "Lss");
            const Plane& rss = get(group, "Rss");
            Plane lrs(n);
            Plane rrs(n);
            for (std::size_t i = 0; i < n; ++i) {
                lrs[i] = (ls5[i] - p.alpha * lss[i]) / p.beta;
                rrs[i] = (rs5[i] - p.alpha * rss[i]) / p.beta;
            }
            next["Lss"] = lss;
            next["Rss"] = rss;
            next["Lrs"] = std::move(lrs);
            next["Rrs"] = std::move(rrs);
        }
    }

    if (now.lfe > prev.lfe) {
        next["LFE"] = get(group, "LFE");
    } else if (prev.lfe > 0) {
        next["LFE"] = get(cur, "LFE");
    }

    // The top channels. A 3.1.2 layer's tops (Ltf3, Rtf3) carry w * delta * Ls: once the layer
    // after it supplies L5 and R5, TF2toT2 takes it out again.
    const bool from_three_one_two = prev.surround == 3 && prev.height == 2;
    const auto top_front_two = [&](Plane& ltf, Plane& rtf) {
        ltf = get(cur, "Ltf");
        rtf = get(cur, "Rtf");
        if (from_three_one_two && now.surround != 3) {
            const Plane& l3 = get(cur, "L");
            const Plane& r3 = get(cur, "R");
            const Plane& l5 = get(group, "L");
            const Plane& r5 = get(group, "R");
            for (std::size_t i = 0; i < n; ++i) {
                ltf[i] -= w * (l3[i] - l5[i]);
                rtf[i] -= w * (r3[i] - r5[i]);
            }
        }
    };
    const std::string_view back_l = now.five_one_four ? "Ltr" : "Ltb";
    const std::string_view back_r = now.five_one_four ? "Rtr" : "Rtb";
    if (now.height == 2) {
        if (prev.height == 0) {
            next["Ltf"] = get(group, "Ltf");
            next["Rtf"] = get(group, "Rtf");
        } else {
            Plane ltf;
            Plane rtf;
            top_front_two(ltf, rtf);
            next["Ltf"] = std::move(ltf);
            next["Rtf"] = std::move(rtf);
        }
    } else if (now.height == 4) {
        if (prev.height == 0) {
            next["Ltf"] = get(group, "Ltf");
            next["Rtf"] = get(group, "Rtf");
            next[back_l] = get(group, back_l);
            next[back_r] = get(group, back_r);
        } else if (prev.height == 2) {
            // T2to4: Ltb = (Ltf2 - Ltf4) / gamma
            Plane ltf2;
            Plane rtf2;
            top_front_two(ltf2, rtf2);
            const Plane& ltf4 = get(group, "Ltf");
            const Plane& rtf4 = get(group, "Rtf");
            Plane ltb(n);
            Plane rtb(n);
            for (std::size_t i = 0; i < n; ++i) {
                ltb[i] = (ltf2[i] - ltf4[i]) / p.gamma;
                rtb[i] = (rtf2[i] - rtf4[i]) / p.gamma;
            }
            next["Ltf"] = ltf4;
            next["Rtf"] = rtf4;
            next[back_l] = std::move(ltb);
            next[back_r] = std::move(rtb);
        } else {
            const std::string_view was_l = prev.five_one_four ? "Ltr" : "Ltb";
            const std::string_view was_r = prev.five_one_four ? "Rtr" : "Rtb";
            next["Ltf"] = get(cur, "Ltf");
            next["Rtf"] = get(cur, "Rtf");
            next[back_l] = get(cur, was_l);
            next[back_r] = get(cur, was_r);
        }
    }
    return next;
}

// recon_gain_flags bit positions (3.9.4) and the channel names they refer to; a layout names its
// left and right surrounds Ls / Rs (5.1) or Lss / Rss (7.1) and the back height pair Ltr / Rtr or
// Ltb / Rtb.
struct ReconChannel {
    std::string_view name;
    std::string_view alternative;
};
constexpr std::array<ReconChannel, 12> kReconChannels{{{"L", ""},
                                                       {"C", ""},
                                                       {"R", ""},
                                                       {"Ls", "Lss"},
                                                       {"Rs", "Rss"},
                                                       {"Ltf", ""},
                                                       {"Rtf", ""},
                                                       {"Lrs", ""},
                                                       {"Rrs", ""},
                                                       {"Ltb", "Ltr"},
                                                       {"Rtb", "Rtr"},
                                                       {"LFE", ""}}};

// 3.6.1: output_gain_flags names the mixed channels the gain is applied to, MSB first.
constexpr std::array<std::string_view, 6> kOutputGainChannels{"Rtf", "Ltf", "Rs",
                                                              "Ls",  "R",   "L"};  // bit 0 .. 5

[[nodiscard]] Plane* find_plane(Planes& planes, const ReconChannel& channel) {
    if (const auto it = planes.find(channel.name); it != planes.end()) {
        return &it->second;
    }
    if (!channel.alternative.empty()) {
        if (const auto it = planes.find(channel.alternative); it != planes.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

[[nodiscard]] const ParameterBlock* find_block(const TemporalUnit& unit,
                                               std::uint32_t parameter_id) {
    for (const ParameterBlock& block : unit.parameter_blocks) {
        if (block.parameter_id == parameter_id) {
            return &block;
        }
    }
    return nullptr;
}

}  // namespace

std::expected<DecodedElement, Error> reconstruct_channels(const Sequence& sequence,
                                                          std::uint32_t audio_element_id,
                                                          std::span<const SubstreamPcm> decoded,
                                                          const DecodeOptions& options) {
    const AudioElement* element = nullptr;
    for (const auto& candidate : sequence.audio_elements) {
        if (candidate.audio_element_id == audio_element_id) {
            element = &candidate;
        }
    }
    if (element == nullptr) {
        return std::unexpected(Error::kBadDescriptor);
    }
    if (element->type != ElementType::kChannelBased) {
        return std::unexpected(Error::kUnsupported);
    }
    const CodecConfig* codec = nullptr;
    for (const auto& candidate : sequence.codec_configs) {
        if (candidate.codec_config_id == element->codec_config_id) {
            codec = &candidate;
        }
    }
    if (codec == nullptr || codec->num_samples_per_frame == 0) {
        return std::unexpected(Error::kBadDescriptor);
    }
    const auto sample_rate = detail::codec_sample_rate(*codec);
    if (!sample_rate.has_value()) {
        return std::unexpected(Error::kUnsupported);
    }
    const std::vector<ChannelLayer>& layers = element->layers;
    if (layers.empty() || layers.size() > 6) {
        return std::unexpected(Error::kBadDescriptor);
    }
    const std::size_t target = options.layer.value_or(layers.size() - 1);
    if (target >= layers.size()) {
        return std::unexpected(Error::kInvalidArgument);
    }

    // The substream count of every layer must account for the element's substreams exactly.
    std::size_t total_substreams = 0;
    for (const ChannelLayer& layer : layers) {
        total_substreams += layer.substream_count;
    }
    if (total_substreams != element->audio_substream_ids.size()) {
        return std::unexpected(Error::kBadDescriptor);
    }

    // The Channel Groups up to the target: their channels and the shape of each layer's layout.
    std::vector<GroupLayout> groups;
    std::vector<Shape> shapes;
    std::optional<LayoutInfo> output_layout;
    for (std::size_t j = 0; j <= target; ++j) {
        const ChannelLayer& layer = layers[j];
        GroupLayout group;
        if (j == 0 && layer.loudspeaker_layout == 15) {
            if (layers.size() != 1 || !layer.expanded_loudspeaker_layout.has_value()) {
                return std::unexpected(Error::kUnsupported);
            }
            const auto info = expanded_layout_info(*layer.expanded_loudspeaker_layout);
            if (!info.has_value()) {
                return std::unexpected(Error::kUnsupported);
            }
            group = {info->substreams, info->coupled_substream_count};
            output_layout = info;
            shapes.push_back({});
        } else {
            const auto shape = shape_of(layer.loudspeaker_layout);
            if (!shape.has_value()) {
                if (layers.size() == 1) {
                    const auto single = layout_info(layer.loudspeaker_layout);
                    if (!single.has_value()) {
                        return std::unexpected(Error::kUnsupported);
                    }
                    group = {single->substreams, single->coupled_substream_count};
                    output_layout = single;
                    shapes.push_back({});
                } else {
                    return std::unexpected(Error::kUnsupported);
                }
            } else {
                const Shape& now = *shape;
                if (j == 0) {
                    const auto info = layout_info(layer.loudspeaker_layout);
                    group = {info->substreams, info->coupled_substream_count};
                } else {
                    const Shape& prev = shapes[j - 1];
                    // 3.6.2.1: no channel count shrinks along the layers, and each layer adds
                    // something.
                    if (now.surround < prev.surround || now.lfe < prev.lfe ||
                        now.height < prev.height ||
                        (now.surround == prev.surround && now.lfe == prev.lfe &&
                         now.height == prev.height)) {
                        return std::unexpected(Error::kBadDescriptor);
                    }
                    group = dependent_group(prev, now);
                }
                shapes.push_back(now);
                if (j == target) {
                    output_layout = layout_info(layer.loudspeaker_layout);
                }
            }
        }
        if (group.substreams.size() != layer.substream_count ||
            group.coupled != layer.coupled_substream_count) {
            return std::unexpected(Error::kBadDescriptor);
        }
        groups.push_back(std::move(group));
    }

    // Find each used substream's decoded PCM.
    const std::size_t frame_samples = codec->num_samples_per_frame;
    std::vector<std::vector<const SubstreamPcm*>> group_pcm(groups.size());
    std::size_t offset = 0;
    for (std::size_t j = 0; j < groups.size(); ++j) {
        for (std::size_t s = 0; s < groups[j].substreams.size(); ++s) {
            const std::uint32_t id = element->audio_substream_ids[offset + s];
            const SubstreamPcm* found = nullptr;
            for (const SubstreamPcm& candidate : decoded) {
                if (candidate.audio_substream_id == id) {
                    found = &candidate;
                }
            }
            const std::size_t channels = s < groups[j].coupled ? 2 : 1;
            if (found == nullptr || found->channels.size() != channels) {
                return std::unexpected(Error::kInvalidArgument);
            }
            group_pcm[j].push_back(found);
        }
        offset += layers[j].substream_count;
    }

    // The Temporal Units that hold this element's Audio Frames, one frame each.
    const auto belongs = [&](const AudioFrame& frame) {
        return std::find(element->audio_substream_ids.begin(), element->audio_substream_ids.end(),
                         frame.audio_substream_id) != element->audio_substream_ids.end();
    };
    std::vector<const TemporalUnit*> units;
    for (const TemporalUnit& unit : sequence.temporal_units) {
        if (std::any_of(unit.audio_frames.begin(), unit.audio_frames.end(), belongs)) {
            units.push_back(&unit);
        }
    }
    const std::size_t total_samples = units.size() * frame_samples;
    for (const auto& pcm_list : group_pcm) {
        for (const SubstreamPcm* pcm : pcm_list) {
            for (const auto& channel : pcm->channels) {
                if (channel.size() != total_samples) {
                    return std::unexpected(Error::kInvalidArgument);
                }
            }
        }
    }

    // Output gain factors, per Channel Group (7.2.1): output_gain / (20 * 256) as a power of ten.
    const auto output_gain_factor = [&](std::size_t j) {
        return std::pow(10.0, static_cast<double>(layers[j].output_gain) / (20.0 * 256.0));
    };

    // Which channels of the target layout ever carry a recon gain, so the smoothing runs on those
    // only.
    const std::uint32_t demix_id =
        element->demixing.has_value() ? element->demixing->definition.parameter_id : 0;
    const std::uint32_t recon_id =
        element->recon_gain.has_value() ? element->recon_gain->definition.parameter_id : 0;
    const bool recon_possible = options.apply_recon_gain && target > 0 &&
                                layers[target].recon_gain_is_present &&
                                element->recon_gain.has_value();
    std::uint32_t active_flags = 0;
    if (recon_possible) {
        for (const TemporalUnit* unit : units) {
            const ParameterBlock* block = find_block(*unit, recon_id);
            if (block != nullptr && !block->subblocks.empty() &&
                target < block->subblocks[0].recon_layers.size()) {
                active_flags |= block->subblocks[0].recon_layers[target].recon_gain_flags & 0xFFFU;
            }
        }
    }

    // The smoothing windows (7.2.3): the second half of a Hann window of 2 * olen samples falls out
    // of the previous frame's gain as the first half rises into this one.
    // Past the overlap the previous frame's share is 0 and this one's 1, so only the overlap itself
    // is tabulated (and the table never scales with the file's num_samples_per_frame).
    const std::size_t full = codec->codec_id == "Opus" ? kOpusOverlap : kAacOverlap;
    const std::size_t overlap = std::min(full, frame_samples);
    std::vector<double> fall(overlap);
    std::vector<double> rise(overlap);
    for (std::size_t i = 0; i < overlap; ++i) {
        const auto hann = [full](std::size_t n) {
            return 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                        static_cast<double>(2 * full - 1));
        };
        rise[i] = hann(i);
        fall[i] = hann(full + i);
    }
    std::array<double, 12> average;
    average.fill(1.0);

    DecodedElement result;
    result.sample_rate = *sample_rate;
    for (const SubstreamChannels& s : output_layout->substreams) {
        result.channel_names.emplace_back(s.first);
        if (!s.second.empty()) {
            result.channel_names.emplace_back(s.second);
        }
    }
    result.channels.assign(result.channel_names.size(), {});
    for (auto& channel : result.channels) {
        channel.reserve(total_samples);
    }

    int w_index = 0;
    const std::uint8_t default_mode =
        element->demixing.has_value() ? element->demixing->default_dmixp_mode : 0;
    const std::uint8_t default_w =
        element->demixing.has_value() ? element->demixing->default_w : static_cast<std::uint8_t>(0);

    for (std::size_t k = 0; k < units.size(); ++k) {
        const TemporalUnit& unit = *units[k];
        const std::size_t base = k * frame_samples;

        // This frame's channels, group by group, with the output gain undone on the mixed ones.
        std::vector<Planes> frame_groups(groups.size());
        for (std::size_t j = 0; j < groups.size(); ++j) {
            Planes& planes = frame_groups[j];
            for (std::size_t s = 0; s < groups[j].substreams.size(); ++s) {
                const SubstreamChannels& names = groups[j].substreams[s];
                const SubstreamPcm& pcm = *group_pcm[j][s];
                Plane first(frame_samples);
                for (std::size_t i = 0; i < frame_samples; ++i) {
                    first[i] = static_cast<double>(pcm.channels[0][base + i]);
                }
                planes[names.first] = std::move(first);
                if (!names.second.empty()) {
                    Plane second(frame_samples);
                    for (std::size_t i = 0; i < frame_samples; ++i) {
                        second[i] = static_cast<double>(pcm.channels[1][base + i]);
                    }
                    planes[names.second] = std::move(second);
                }
            }
            if (layers[j].output_gain_is_present) {
                const double factor = output_gain_factor(j);
                for (std::size_t bit = 0; bit < kOutputGainChannels.size(); ++bit) {
                    if (((layers[j].output_gain_flags >> bit) & 1U) == 0) {
                        continue;
                    }
                    auto it = planes.find(kOutputGainChannels[bit]);
                    if (it == planes.end() && bit == 5) {
                        // The L channel of the group: L1 of a Mono layer, L2 of Mono growing to
                        // Stereo.
                        it = planes.find(j == 0 && shapes[0].surround == 1 ? "C" : "L2");
                    }
                    if (it != planes.end()) {
                        for (double& sample : it->second) {
                            sample *= factor;
                        }
                    }
                }
            }
        }

        // 7.2.2: the demixing parameters of this frame. A Parameter Block moves w(k) by
        // w_idx_offset; without one the definition's defaults hold, default_w being w itself.
        DemixParams params = demix_params(default_mode).value_or(DemixParams{});
        double w = kTopWeight[std::min<std::size_t>(default_w, kTopWeight.size() - 1)];
        if (element->demixing.has_value()) {
            const ParameterBlock* block = find_block(unit, demix_id);
            if (block != nullptr && !block->subblocks.empty()) {
                if (const auto mode = demix_params(block->subblocks[0].dmixp_mode);
                    mode.has_value()) {
                    params = *mode;
                    w_index = std::clamp(w_index + mode->w_idx_offset, 0, 10);
                    w = kTopWeight[static_cast<std::size_t>(w_index)];
                }
            }
        }

        Planes current = frame_groups[0];
        for (std::size_t j = 1; j <= target; ++j) {
            current = demix_step(current, frame_groups[j], shapes[j - 1], shapes[j], params, w,
                                 frame_samples);
        }

        // 7.2.3: the recon gain of this frame, smoothed against the previous frame's.
        if (active_flags != 0) {
            std::array<double, 12> gain;
            gain.fill(1.0);
            const ParameterBlock* block = find_block(unit, recon_id);
            if (block != nullptr && !block->subblocks.empty() &&
                target < block->subblocks[0].recon_layers.size()) {
                const ReconLayerData& data = block->subblocks[0].recon_layers[target];
                std::size_t next_gain = 0;
                for (std::size_t bit = 0; bit < gain.size(); ++bit) {
                    if (((data.recon_gain_flags >> bit) & 1U) != 0 &&
                        next_gain < data.recon_gains.size()) {
                        gain[bit] = static_cast<double>(data.recon_gains[next_gain++]) / 255.0;
                    }
                }
            }
            for (std::size_t bit = 0; bit < gain.size(); ++bit) {
                if (((active_flags >> bit) & 1U) == 0) {
                    continue;
                }
                const double previous = average[bit];
                const double weight = 2.0 / (kAverageLength + 1.0);
                average[bit] = weight * gain[bit] + (1.0 - weight) * previous;
                if (Plane* plane = find_plane(current, kReconChannels[bit]); plane != nullptr) {
                    for (std::size_t i = 0; i < overlap; ++i) {
                        (*plane)[i] *= previous * fall[i] + average[bit] * rise[i];
                    }
                    for (std::size_t i = overlap; i < frame_samples; ++i) {
                        (*plane)[i] *= average[bit];
                    }
                }
            }
        }

        // The trimming of this Temporal Unit's frames, then the output in layout order.
        std::size_t trim_start = 0;
        std::size_t trim_end = 0;
        for (const AudioFrame& frame : unit.audio_frames) {
            if (belongs(frame)) {
                trim_start =
                    std::min<std::size_t>(frame.num_samples_to_trim_at_start, frame_samples);
                trim_end = std::min<std::size_t>(frame.num_samples_to_trim_at_end,
                                                 frame_samples - trim_start);
                break;
            }
        }
        std::size_t out = 0;
        for (const SubstreamChannels& s : output_layout->substreams) {
            for (const std::string_view name : {s.first, s.second}) {
                if (name.empty()) {
                    continue;
                }
                const auto it = current.find(name);
                if (it == current.end()) {
                    return std::unexpected(Error::kBadDescriptor);
                }
                for (std::size_t i = trim_start; i < frame_samples - trim_end; ++i) {
                    result.channels[out].push_back(static_cast<float>(it->second[i]));
                }
                ++out;
            }
        }
    }
    return result;
}

}  // namespace iclforge::containers::iamf
