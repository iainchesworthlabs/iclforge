#include "truehd_objects.hpp"

#include <fmt/base.h>
#include <fmt/format.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../adm/atmos_adm.hpp"
#include "../exit_codes.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/evolution.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;
namespace oba = iclforge::objects::oba;

constexpr double kFullScale = 8388608.0;  // 2^23: the 24 bit samples as floats in -1..1
// TS 103 420 §5.5.7: the unit of block_offset_factor.
constexpr std::uint64_t kBlockOffsetUnit = 32;

}  // namespace

ObjectPlan plan_object_channels(const oba::Program& program, int channels) {
    ObjectPlan plan;
    const int objects = oba::object_count(program);
    if (objects != channels) {
        plan.problem =
            fmt::format("the programme describes {} objects and substream 3 has {} channels",
                        objects, channels);
    }
    const int beds = oba::bed_channel_count(program);
    const int isf = oba::isf_object_count(program.isf_idx);
    std::vector<oba::BedLabel> labels;
    if (!program.dynamic_only && program.nonstd_bed == 0 && program.extra_beds.empty() &&
        program.extra_bed_channels == 0) {
        labels = oba::bed_labels(program.bed);
    } else if (!program.dynamic_only && plan.problem.empty()) {
        plan.problem = "the programme uses a non-standard bed assignment or several bed instances";
    }
    if (isf > 0 && plan.problem.empty()) {
        plan.problem =
            "the programme uses intermediate spatial format objects, which have no position";
    }
    plan.channels.resize(static_cast<std::size_t>(std::max(channels, 0)));
    for (std::size_t i = 0; i < plan.channels.size(); ++i) {
        ObjectChannel& channel = plan.channels[i];
        const auto index = static_cast<int>(i);
        if (index < beds) {
            if (program.dynamic_only) {  // the LFE of a dynamic-object-only programme
                channel.kind = ObjectChannelKind::kLfe;
                channel.label = oba::BedLabel::kLfe;
            } else if (i < labels.size()) {
                channel.label = labels[i];
                channel.kind = labels[i] == oba::BedLabel::kLfe ? ObjectChannelKind::kLfe
                                                                : ObjectChannelKind::kBed;
            }
        } else if (index >= beds + isf && index < objects) {
            channel.kind = ObjectChannelKind::kDynamic;
            channel.dynamic_index = static_cast<std::size_t>(index - beds - isf);
        }
    }
    return plan;
}

TrueHdObjectSink::TrueHdObjectSink(std::string_view objects_dir, std::string_view adm_out)
    : objects_dir_(objects_dir), adm_out_(adm_out) {}

bool TrueHdObjectSink::open_files(const th::DecodedAccessUnit& unit) {
    opened_ = true;
    sample_rate_ = unit.sample_rate;
    const auto channels = static_cast<std::size_t>(unit.channels);
    adm_.sample_rate = unit.sample_rate;
    adm_.channels.resize(channels);
    if (objects_dir_.empty()) {
        return true;
    }
    std::error_code ec;
    const std::filesystem::path dir{objects_dir_};
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        fmt::println(stderr, "error: cannot create directory {} ({})", objects_dir_, ec.message());
        return false;
    }
    sinks_.resize(channels);
    for (std::size_t i = 0; i < channels; ++i) {
        const auto path = dir / fmt::format("object_{:02}.wav", i);
        if (!sinks_[i].open(path.string(), unit.sample_rate, 1, {})) {
            fmt::println(stderr, "error: cannot open {} for writing", path.string());
            return false;
        }
    }
    return true;
}

// The object audio metadata of the unit's evolution frame, if it carries any: the programme (the
// first one decides the plan) and each dynamic object's update blocks for the ADM master.
void TrueHdObjectSink::read_metadata(const th::DecodedAccessUnit& unit) {
    const auto extra = th::read_extra_data(unit.extra_data);
    if (!extra) {
        return;
    }
    const th::EvolutionRead evolution = th::read_evolution(*extra);
    if (!evolution.container || evolution.container->find(th::kEvolutionPayloadOamd) == nullptr) {
        return;
    }
    ++oamd_payloads_;
    const auto decoded = oba::parse_payload(evolution.container->oamd());
    if (!decoded) {
        ++oamd_unread_;
        return;
    }
    if (!program_) {
        program_ = decoded->program;
        plan_ = plan_object_channels(*program_, unit.channels);
    } else if (oba::object_count(decoded->program) != oba::object_count(*program_) ||
               decoded->program.bed != program_->bed ||
               decoded->program.dynamic_only != program_->dynamic_only) {
        // The channels of the substream are fixed for the stream; a programme that describes them
        // another way is not followed (ERRATA.md entry 61).
        ++program_changes_;
        return;
    }
    const th::EvolutionPayload* const payload =
        evolution.container->find(th::kEvolutionPayloadOamd);
    const std::uint64_t frame_start =
        samples_ + (payload != nullptr ? payload->config.sample_offset.value_or(0) : 0);
    // TS 103 420 §5.5.6: the update's own sample offset, which parse_payload keeps on its first
    // block, then 32 samples to each unit of a block's offset factor.
    const int base =
        decoded->blocks.empty() ? 0 : std::max(decoded->blocks.front().sample_offset, 0);
    for (const oba::UpdateBlock& block : decoded->blocks) {
        const std::uint64_t sample =
            frame_start + static_cast<std::uint64_t>(base) +
            kBlockOffsetUnit * static_cast<std::uint64_t>(std::max(block.block_offset_factor, 0));
        for (std::size_t i = 0; i < plan_.channels.size() && i < adm_.channels.size(); ++i) {
            const ObjectChannel& channel = plan_.channels[i];
            if (channel.kind != ObjectChannelKind::kDynamic ||
                channel.dynamic_index >= block.objects.size()) {
                continue;
            }
            adm_.channels[i].updates.push_back({.sample_offset = sample,
                                                .ramp_duration_samples = block.ramp_duration,
                                                .state = block.objects[channel.dynamic_index]});
        }
    }
}

bool TrueHdObjectSink::add(const th::DecodedAccessUnit& unit) {
    if (unit.channels <= 0 || unit.samples <= 0) {
        return true;
    }
    if (!opened_ && !open_files(unit)) {
        return false;
    }
    if (static_cast<std::size_t>(unit.channels) != adm_.channels.size()) {
        fmt::println(stderr, "error: the channel count of substream 3 changes mid-stream");
        return false;
    }
    read_metadata(unit);

    const auto channels = static_cast<std::size_t>(unit.channels);
    const auto samples = static_cast<std::size_t>(unit.samples);
    std::vector<float> column(samples);
    for (std::size_t ch = 0; ch < channels; ++ch) {
        for (std::size_t i = 0; i < samples; ++i) {
            column[i] =
                static_cast<float>(static_cast<double>(unit.pcm[i * channels + ch]) / kFullScale);
        }
        if (!adm_out_.empty()) {
            adm_.channels[ch].pcm.insert(adm_.channels[ch].pcm.end(), column.begin(), column.end());
        }
        if (!sinks_.empty() && !sinks_[ch].append(0, column)) {
            fmt::println(stderr, "error: cannot write object audio under {}", objects_dir_);
            return false;
        }
    }
    samples_ += samples;
    return true;
}

void TrueHdObjectSink::abort() {
    for (PlanarWavSink& sink : sinks_) {
        sink.abort();
    }
}

int TrueHdObjectSink::finish(FILE* status) {
    int code = kExitOk;
    for (PlanarWavSink& sink : sinks_) {
        if (!sink.close()) {
            fmt::println(stderr, "error: cannot finish the object audio under {}", objects_dir_);
            code = kExitOutput;
        }
    }
    sinks_.clear();
    if (!opened_) {
        return code;
    }
    if (!program_) {
        status_println(status,
                       "  no object audio metadata in this stream's evolution frames (payloads "
                       "read: {}, unread: {}); the channels are written without names",
                       oamd_payloads_, oamd_unread_);
    } else {
        status_println(status, "  object audio metadata: {} payload(s), {} object(s){}",
                       oamd_payloads_, oba::object_count(*program_),
                       oamd_unread_ != 0 ? fmt::format(", {} payload(s) unread", oamd_unread_)
                                         : std::string{});
        if (!plan_.problem.empty()) {
            status_println(status, "  warning: {}", plan_.problem);
        }
        if (program_changes_ != 0) {
            status_println(status,
                           "  warning: {} payload(s) describe another programme than the first and "
                           "are not followed",
                           program_changes_);
        }
    }
    if (!objects_dir_.empty()) {
        status_println(status, "  wrote {} channel(s) as mono WAVs under {}", adm_.channels.size(),
                       objects_dir_);
    }
    if (adm_out_.empty() || code != kExitOk) {
        return code;
    }
    // A channel the plan does not name has nothing the ADM master can place.
    bool placeable = program_.has_value() && plan_.problem.empty();
    for (std::size_t i = 0; placeable && i < plan_.channels.size() && i < adm_.channels.size();
         ++i) {
        const ObjectChannel& channel = plan_.channels[i];
        if (channel.kind == ObjectChannelKind::kUnplaced) {
            placeable = false;
            break;
        }
        AdmMasterChannel& target = adm_.channels[i];
        if (channel.kind == ObjectChannelKind::kDynamic) {
            target.name = fmt::format("Object {}", channel.dynamic_index + 1);
        } else {
            target.bed_label = channel.label;
            target.name = channel.label ? std::string(oba::describe(*channel.label)) : "Bed";
        }
    }
    if (!placeable) {
        fmt::println(stderr, "warning: {} not written: {}", adm_out_,
                     program_
                         ? (plan_.problem.empty() ? "the channels have no names" : plan_.problem)
                         : "the stream carries no object audio metadata");
        return code;
    }
    const auto written = write_adm_atmos_master(adm_out_, adm_);
    if (!written) {
        fmt::println(stderr, "error: {}", written.error());
        return kExitOutput;
    }
    status_println(status, "  wrote ADM master ({} channels) to {}", adm_.channels.size(),
                   adm_out_);
    return code;
}

std::optional<oba::BedLabel> bed_label_of(th::Channel channel) noexcept {
    switch (channel) {
        case th::Channel::kL:
            return oba::BedLabel::kL;
        case th::Channel::kR:
            return oba::BedLabel::kR;
        case th::Channel::kC:
            return oba::BedLabel::kC;
        case th::Channel::kLfe:
            return oba::BedLabel::kLfe;
        case th::Channel::kLs:
            return oba::BedLabel::kLs;
        case th::Channel::kRs:
            return oba::BedLabel::kRs;
        case th::Channel::kLrs:
            return oba::BedLabel::kLb;
        case th::Channel::kRrs:
            return oba::BedLabel::kRb;
        case th::Channel::kLvh:
            return oba::BedLabel::kTfl;
        case th::Channel::kRvh:
            return oba::BedLabel::kTfr;
        case th::Channel::kLw:
            return oba::BedLabel::kLw;
        case th::Channel::kRw:
            return oba::BedLabel::kRw;
        case th::Channel::kLfe2:
            return oba::BedLabel::kLfe2;
        default:
            return std::nullopt;
    }
}

bool TrueHdBedSink::add(const th::DecodedAccessUnit& unit) {
    if (unit.channels <= 0 || unit.samples <= 0) {
        return true;
    }
    const auto channels = static_cast<std::size_t>(unit.channels);
    if (adm_.channels.empty()) {
        if (unit.layout.size() != channels) {
            fmt::println(stderr,
                         "error: the major sync does not name the speakers of this {} channel "
                         "presentation, so an ADM master cannot place them",
                         channels);
            return false;
        }
        adm_.sample_rate = unit.sample_rate;
        adm_.channels.resize(channels);
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const auto label = bed_label_of(unit.layout[ch]);
            if (!label) {
                fmt::println(
                    stderr,
                    "error: the speaker {} has no bed label, so an ADM master cannot place it",
                    th::channel_name(unit.layout[ch]));
                return false;
            }
            adm_.channels[ch].bed_label = label;
            adm_.channels[ch].name = std::string(oba::describe(*label));
        }
    }
    if (channels != adm_.channels.size()) {
        fmt::println(stderr, "error: the channel count changes mid-stream");
        return false;
    }
    const auto samples = static_cast<std::size_t>(unit.samples);
    for (std::size_t ch = 0; ch < channels; ++ch) {
        auto& pcm = adm_.channels[ch].pcm;
        for (std::size_t i = 0; i < samples; ++i) {
            pcm.push_back(
                static_cast<float>(static_cast<double>(unit.pcm[i * channels + ch]) / kFullScale));
        }
    }
    return true;
}

int TrueHdBedSink::finish(FILE* status) {
    if (adm_.channels.empty()) {
        return kExitOk;
    }
    const auto written = write_adm_atmos_master(adm_out_, adm_);
    if (!written) {
        fmt::println(stderr, "error: {}", written.error());
        return kExitOutput;
    }
    status_println(status, "  wrote ADM master (bed only, {} channels) to {}", adm_.channels.size(),
                   adm_out_);
    return kExitOk;
}

}  // namespace forge_cli::commands
