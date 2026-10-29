#include "truehd_evolution.hpp"

#include <fmt/base.h>
#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/objects/oamd.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/evolution.hpp"
#include "json_sink.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;
namespace oba = iclforge::objects::oba;

constexpr std::size_t kMaxPrograms = 16;
constexpr std::size_t kLeadBytes = 4;

std::string hex_of(std::span<const std::byte> bytes) {
    std::string out;
    for (const std::byte b : bytes) {
        out += fmt::format("{:02x}", std::to_integer<unsigned>(b));
    }
    return out;
}

void continuation(std::string_view text) {
    fmt::println("{:<16}{}", "", text);
}

}  // namespace

void EvolutionSurvey::add(std::uint64_t unit_index, const th::AccessUnit& unit) {
    if (!unit.extra_data) {
        return;
    }
    const th::ExtraData& extra = *unit.extra_data;
    if (regions_ == 0) {
        first_unit_ = unit_index;
        if (extra.framing_ok) {
            lead_hex_ = hex_of(extra.evolution_payload.first(
                std::min<std::size_t>(extra.evolution_payload.size(), kLeadBytes)));
        }
    } else {
        const std::uint64_t gap = unit_index - last_unit_;
        gap_min_ = regions_ == 1 ? gap : std::min(gap_min_, gap);
        gap_max_ = std::max(gap_max_, gap);
    }
    last_unit_ = unit_index;
    ++regions_;

    const th::EvolutionRead emdf = th::read_evolution(extra, th::EvolutionReading::kEmdf);
    const th::EvolutionRead letter = th::read_evolution(extra, th::EvolutionReading::kSpec36);
    check_ok_ += emdf.check_ok ? 1U : 0U;
    framing_ok_ += emdf.framing_ok ? 1U : 0U;
    parity_ok_ += emdf.parity_ok ? 1U : 0U;
    reserved_zero_ += emdf.reserved_zero ? 1U : 0U;
    length_fits_ += emdf.length_fits ? 1U : 0U;
    const th::EvolutionRead* const reads[2] = {&emdf, &letter};
    const auto clean = [](const th::EvolutionRead& read) {
        return read.container.has_value() && read.container->trailing_zero &&
               !read.container->payloads.empty();
    };
    for (std::size_t i = 0; i < 2; ++i) {
        if (reads[i]->container.has_value()) {
            ++readings_[i].frames;
            readings_[i].clean += clean(*reads[i]) ? 1U : 0U;
        } else {
            ++readings_[i].errors[std::string(th::to_string(reads[i]->container.error().kind))];
        }
    }

    // The frame the EMDF reading read cleanly, else the one the letter of §15.4 read cleanly, else
    // whichever read at all (a stream of random bytes reads as a frame now and then; one with a
    // payload and nothing but zero bits after it does not).
    const th::EvolutionFrame* frame = nullptr;
    const char* reading = "none";
    if (clean(emdf) || (!clean(letter) && emdf.container.has_value())) {
        frame = &*emdf.container;
        reading = "emdf";
    } else if (letter.container.has_value()) {
        frame = &*letter.container;
        reading = "spec36";
    }
    if (detail_enabled_) {
        detail_.add(unit_index, extra, emdf, letter, frame, reading);
    }
    if (frame == nullptr) {
        return;
    }
    ++frames_read_;
    for (const th::EvolutionPayload& payload : frame->payloads) {
        ++payload_ids_[payload.id];
    }
    ++primary_codes_[frame->protection.primary_code];
    ++secondary_codes_[frame->protection.secondary_code];

    if (frame->find(th::kEvolutionPayloadOamd) == nullptr) {
        return;
    }
    const auto bytes = frame->oamd();
    oamd_min_bytes_ =
        oamd_payloads_ == 0 ? bytes.size() : std::min<std::uint64_t>(oamd_min_bytes_, bytes.size());
    oamd_max_bytes_ = std::max<std::uint64_t>(oamd_max_bytes_, bytes.size());
    ++oamd_payloads_;
    const auto decoded = oba::parse_payload(bytes);
    if (!decoded) {
        ++oamd_unread_;
        return;
    }
    const oba::Program& program = decoded->program;
    ProgramTally tally;
    tally.object_count = static_cast<std::uint64_t>(oba::object_count(program));
    tally.dynamic_only = program.dynamic_only;
    tally.lfe = program.lfe;
    tally.bed_assignment = program.bed;
    tally.bed_nonstandard = program.nonstd_bed != 0;
    tally.extra_bed_instances = program.extra_beds.size();
    tally.isf_index = program.isf_idx;
    tally.dynamic_objects = static_cast<std::uint64_t>(program.dynamic_objects);
    tally.update_blocks = decoded->blocks.size();
    if (program.dynamic_only) {
        if (program.lfe) {
            tally.bed_channels.emplace_back("LFE");
        }
    } else if (!tally.bed_nonstandard) {
        for (const oba::BedLabel label : oba::bed_labels(program.bed)) {
            tally.bed_channels.emplace_back(oba::describe(label));
        }
    }
    const auto same = [&tally](const ProgramTally& other) {
        return other.object_count == tally.object_count &&
               other.dynamic_only == tally.dynamic_only && other.lfe == tally.lfe &&
               other.bed_assignment == tally.bed_assignment &&
               other.bed_nonstandard == tally.bed_nonstandard &&
               other.extra_bed_instances == tally.extra_bed_instances &&
               other.isf_index == tally.isf_index &&
               other.dynamic_objects == tally.dynamic_objects &&
               other.update_blocks == tally.update_blocks;
    };
    const auto found = std::ranges::find_if(programs_, same);
    if (found != programs_.end()) {
        ++found->frames;
    } else if (programs_.size() < kMaxPrograms) {
        tally.frames = 1;
        programs_.push_back(std::move(tally));
    } else {
        ++oamd_other_programs_;
    }
}

void EvolutionSurvey::write_json(iclforge::apps::JsonSink& json) const {
    json.begin_object();
    json.member("provisional", true);
    json.member("question", "libs/truehd/ERRATA.md, Atmos spec v3.6, question 20");
    json.member("regions", regions_);
    if (regions_ == 0) {
        if (detail_enabled_) {
            json.key("detail");
            detail_.write_json(json);
        }
        json.end_object();
        return;
    }
    json.member("check_nibble_ok", check_ok_);
    json.member("framing_ok", framing_ok_);
    json.member("parity_ok", parity_ok_);
    json.member("evo_reserved_zero", reserved_zero_);
    json.member("length_fits", length_fits_);
    json.member("frames_read", frames_read_);
    json.key("readings");
    json.begin_object();
    const std::array<std::string_view, 2> names{"emdf", "spec36"};
    for (std::size_t i = 0; i < 2; ++i) {
        json.key(names[i]);
        json.begin_object();
        json.member("frames", readings_[i].frames);
        json.member("clean_frames", readings_[i].clean);
        json.key("errors");
        json.begin_array();
        for (const auto& [kind, count] : readings_[i].errors) {
            json.begin_object();
            json.member("kind", kind);
            json.member("count", count);
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_object();
    json.key("payload_ids");
    json.begin_array();
    for (const auto& [id, count] : payload_ids_) {
        json.begin_object();
        json.member("id", id);
        json.member("frames", count);
        json.end_object();
    }
    json.end_array();
    json.key("protection");
    json.begin_object();
    const auto codes = [&json](std::string_view name,
                               const std::map<std::uint64_t, std::uint64_t>& tally) {
        json.key(name);
        json.begin_array();
        for (const auto& [code, count] : tally) {
            json.begin_object();
            json.member("code", code);
            json.member("frames", count);
            json.end_object();
        }
        json.end_array();
    };
    codes("primary_codes", primary_codes_);
    codes("secondary_codes", secondary_codes_);
    json.member("verified", false);
    json.end_object();
    json.member("signature_verified", false);
    json.key("oamd");
    json.begin_object();
    json.member("payloads", oamd_payloads_);
    json.member("unread", oamd_unread_);
    json.member("min_bytes", oamd_min_bytes_);
    json.member("max_bytes", oamd_max_bytes_);
    json.key("programs");
    json.begin_array();
    for (const ProgramTally& program : programs_) {
        json.begin_object();
        json.member("frames", program.frames);
        json.member("object_count", program.object_count);
        json.member("dynamic_only", program.dynamic_only);
        json.member("lfe", program.lfe);
        json.member("bed_assignment", program.bed_assignment);
        json.member("bed_nonstandard", program.bed_nonstandard);
        json.member("extra_bed_instances", program.extra_bed_instances);
        json.key("bed_channels");
        json.begin_array();
        for (const std::string& channel : program.bed_channels) {
            json.value(channel);
        }
        json.end_array();
        json.member("isf_index", program.isf_index);
        json.member("dynamic_objects", program.dynamic_objects);
        json.member("update_blocks", program.update_blocks);
        json.end_object();
    }
    json.end_array();
    json.member("other_program_frames", oamd_other_programs_);
    json.end_object();
    json.key("cadence");
    json.begin_object();
    json.member("first_unit", first_unit_);
    json.member("last_unit", last_unit_);
    json.member("min_gap_units", gap_min_);
    json.member("max_gap_units", gap_max_);
    json.end_object();
    json.member("lead_hex", lead_hex_);
    if (detail_enabled_) {
        json.key("detail");
        detail_.write_json(json);
    }
    json.end_object();
}

void EvolutionSurvey::print_text() const {
    if (regions_ == 0) {
        return;
    }
    fmt::println(
        "{:<16}{} unit(s) carry extra data, {} read as an evolution frame (provisional, "
        "ERRATA question 20)",
        "evolution", regions_, frames_read_);
    continuation(
        fmt::format("check nibble {}/{}, framing {}/{}, parity {}/{}, evo_reserved 0 in "
                    "{}/{}, length fits {}/{}",
                    check_ok_, regions_, framing_ok_, regions_, parity_ok_, regions_,
                    reserved_zero_, regions_, length_fits_, regions_));
    continuation(fmt::format(
        "read as the EMDF container: {}/{} ({} clean); as the letter of §15.4: {}/{} ({} clean)",
        readings_[0].frames, regions_, readings_[0].clean, readings_[1].frames, regions_,
        readings_[1].clean));
    for (std::size_t i = 0; i < 2; ++i) {
        for (const auto& [kind, count] : readings_[i].errors) {
            continuation(fmt::format("  {} reading failed {} time(s): {}",
                                     i == 0 ? "EMDF" : "§15.4", count, kind));
        }
    }
    std::string ids;
    for (const auto& [id, count] : payload_ids_) {
        ids += fmt::format("{}{} (x{})", ids.empty() ? "" : ", ", id, count);
    }
    continuation(fmt::format("payload ids: {}", ids.empty() ? "none" : ids));
    continuation(fmt::format("frames at units {} to {}, {} to {} unit(s) apart; first bytes {}",
                             first_unit_, last_unit_, gap_min_, gap_max_,
                             lead_hex_.empty() ? "n/a" : lead_hex_));
    if (oamd_payloads_ != 0) {
        continuation(
            fmt::format("object audio metadata: {} payload(s) of {} to {} byte(s), {} not read",
                        oamd_payloads_, oamd_min_bytes_, oamd_max_bytes_, oamd_unread_));
        for (const ProgramTally& program : programs_) {
            std::string beds;
            for (const std::string& channel : program.bed_channels) {
                beds += fmt::format("{}{}", beds.empty() ? "" : " ", channel);
            }
            std::string layout;
            if (program.dynamic_only) {
                layout = program.lfe ? "dynamic-only program with LFE" : "dynamic-only program";
            } else if (program.bed_nonstandard) {
                layout = fmt::format("bed of a non-standard assignment (0x{:05X})",
                                     program.bed_assignment);
            } else {
                layout = fmt::format("bed {} (assignment 0x{:03X})", beds.empty() ? "empty" : beds,
                                     program.bed_assignment);
            }
            if (program.extra_bed_instances != 0) {
                layout += fmt::format(" and {} more bed instance(s)", program.extra_bed_instances);
            }
            continuation(
                fmt::format("  x{}: {} object(s), {}, ISF {}, {} dynamic, {} update block(s)",
                            program.frames, program.object_count, layout,
                            program.isf_index < 0 ? std::string("none")
                                                  : fmt::format("index {}", program.isf_index),
                            program.dynamic_objects, program.update_blocks));
        }
        if (oamd_other_programs_ != 0) {
            continuation(
                fmt::format("  and {} frame(s) of further programs", oamd_other_programs_));
        }
    }
    continuation("not verified: the protection bytes and any signature");
    if (detail_enabled_) {
        detail_.print_text();
    }
}

}  // namespace forge_cli::commands
