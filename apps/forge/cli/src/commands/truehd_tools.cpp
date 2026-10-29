// `metadata`, `normalize` and `qc` of a TrueHD stream: the channel meaning of TH spec section 14.1
// rewritten in a finished stream without re-encoding it, set from a measurement, and compared with
// one. The stream is the elementary one `metadata` and `normalize` take for E-AC-3
// (commands/stream_tools.cpp); the library does the rewrite
// (iclforge::truehd::edit_channel_meaning).
//
// PROVISIONAL, no oracle until the DEE set (libs/truehd/ERRATA.md, questions 51 to 55): FFmpeg
// writes zeros for these fields, so what the values mean is the specification's alone, and where it
// is silent these commands write and show the numbers as they are.

#include <fmt/base.h>
#include <fmt/format.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "analysis.hpp"
#include "iclforge/ac3/meta/loudness.hpp"
#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "iclforge/truehd/io/metadata_edit.hpp"
#include "truehd.hpp"
#include "truehd_meaning.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;

// What `metadata` applies to a TrueHD stream: the channel meaning's fields.
constexpr std::initializer_list<std::string_view> kMetadataOptions{
    "truehd-dialnorm-2ch=",      "truehd-dialnorm-6ch=",      "truehd-dialnorm-8ch=",
    "truehd-mix-level-2ch=",     "truehd-mix-level-6ch=",     "truehd-mix-level-8ch=",
    "truehd-source-format-6ch=", "truehd-source-format-8ch=", "truehd-control-2ch=",
    "truehd-control-6ch=",       "truehd-control-8ch=",       "truehd-heavy-drc-start-up-gain=",
    "truehd-drc-start-up-gain="};

// A field of the channel meaning, to show what an edit changed.
struct FieldView {
    std::string_view label;
    int (*get)(const th::ChannelMeaning&);
    bool flag;  // a control-enabled bit: shown as on and off
};

constexpr std::array<FieldView, 13> kFields{{
    {"heavy DRC start-up gain",
     [](const th::ChannelMeaning& m) { return int{m.heavy_drc_start_up_gain}; }, false},
    {"DRC start-up gain", [](const th::ChannelMeaning& m) { return int{m.drc_start_up_gain}; },
     false},
    {"2ch control", [](const th::ChannelMeaning& m) { return int{m.twoch_control_enabled}; }, true},
    {"2ch dialogue norm", [](const th::ChannelMeaning& m) { return int{m.twoch_dialogue_norm}; },
     false},
    {"2ch mix level", [](const th::ChannelMeaning& m) { return int{m.twoch_mix_level}; }, false},
    {"6ch control", [](const th::ChannelMeaning& m) { return int{m.sixch_control_enabled}; }, true},
    {"6ch dialogue norm", [](const th::ChannelMeaning& m) { return int{m.sixch_dialogue_norm}; },
     false},
    {"6ch mix level", [](const th::ChannelMeaning& m) { return int{m.sixch_mix_level}; }, false},
    {"6ch source format", [](const th::ChannelMeaning& m) { return int{m.sixch_source_format}; },
     false},
    {"8ch control", [](const th::ChannelMeaning& m) { return int{m.eightch_control_enabled}; },
     true},
    {"8ch dialogue norm", [](const th::ChannelMeaning& m) { return int{m.eightch_dialogue_norm}; },
     false},
    {"8ch mix level", [](const th::ChannelMeaning& m) { return int{m.eightch_mix_level}; }, false},
    {"8ch source format", [](const th::ChannelMeaning& m) { return int{m.eightch_source_format}; },
     false},
}};

std::string show(const FieldView& field, int value) {
    if (field.flag) {
        return value != 0 ? "on" : "off";
    }
    return fmt::to_string(value);
}

// Prints one line for each field `before` and `after` differ in.
void print_changes(FILE* status, const th::ChannelMeaning& before,
                   const th::ChannelMeaning& after) {
    for (const FieldView& field : kFields) {
        const int was = field.get(before);
        const int now = field.get(after);
        if (was != now) {
            status_println(status, "  {:<24}{} -> {}", field.label, show(field, was),
                           show(field, now));
        }
    }
}

// Writes `bytes` to `out_path` through the sink every stream tool writes through.
bool write_stream(std::string_view out_path, std::span<const std::byte> bytes, bool keep_partial) {
    EncodedStreamSink sink;
    if (!sink.open(out_path, keep_partial)) {
        return false;
    }
    if (!sink.push(bytes)) {
        sink.abort();
        return false;
    }
    return sink.close();
}

int report_edit_error(const th::Error& error, std::string_view in_path) {
    switch (error.kind) {
        case th::ErrorKind::kBadParameter:
            fmt::println(stderr, "error: {}", error.detail);
            return kExitUsage;
        case th::ErrorKind::kUnsupported:
            fmt::println(stderr, "error: {}: {}", in_path, error.detail);
            return kExitUsage;
        default:
            break;
    }
    fmt::println(stderr, "error: {}: {}{}{}", in_path, th::to_string(error.kind),
                 error.detail.empty() ? "" : ": ", error.detail);
    return kExitInput;
}

}  // namespace

int run_metadata_truehd(std::vector<std::byte>&& bytes, std::string_view in_path,
                        std::string_view out_path, const Options& meta) {
    // dialnorm=, compr=, bsmod=, insert and the rest are E-AC-3's: refused by name, never dropped.
    if (const int refused = check_options_applied(meta, kMetadataOptions); refused != kExitOk) {
        return refused;
    }
    if (meta.thd_meaning.empty()) {
        fmt::println(
            stderr,
            "error: nothing to change - give at least one of truehd-dialnorm-2ch=, "
            "-6ch=, -8ch=, truehd-mix-level-2ch=, -6ch=, -8ch=, truehd-source-format-6ch=, "
            "-8ch=, truehd-control-2ch=, -6ch=, -8ch=, truehd-heavy-drc-start-up-gain= or "
            "truehd-drc-start-up-gain= ('forge probe' shows the current values)");
        return kExitUsage;
    }
    const auto summary = th::edit_channel_meaning(bytes, meta.thd_meaning);
    if (!summary) {
        return report_edit_error(summary.error(), in_path);
    }
    if (!write_stream(out_path, bytes, meta.keep_partial)) {
        return kExitOutput;
    }
    const auto status = status_stream(out_path);
    status_println(status,
                   "rewrote the channel meaning of {} of {} major syncs ({} access units) -> {} "
                   "(audio untouched)",
                   summary->changed, summary->major_syncs, summary->units, out_path);
    print_changes(status, summary->before, summary->after);
    if (summary->changed == 0) {
        status_println(status, "  the stream already had these values");
    }
    if (summary->distinct_before > 1) {
        status_println(status,
                       "  the major syncs carried {} different channel meanings; the changes are "
                       "those of the first",
                       summary->distinct_before);
    }
    status_println(status,
                   "  provisional, no oracle until the DEE set (libs/truehd/ERRATA.md, questions "
                   "51 to 55)");
    return kExitOk;
}

namespace {

constexpr std::array<MeaningPresentation, 3> kAllPresentations{
    MeaningPresentation::k2ch, MeaningPresentation::k6ch, MeaningPresentation::k8ch};

// The presentations a stream with `substreams` substreams has, in the order 2, 6, 8 channels.
std::vector<MeaningPresentation> presentations_of(int substreams) {
    std::vector<MeaningPresentation> present;
    for (const MeaningPresentation presentation : kAllPresentations) {
        if (stream_has_presentation(presentation, substreams)) {
            present.push_back(presentation);
        }
    }
    return present;
}

// The field of a ChannelMeaningEdit that holds a presentation's dialogue norm.
std::optional<int>& dialogue_norm_of(th::ChannelMeaningEdit& edit,
                                     MeaningPresentation presentation) {
    switch (presentation) {
        case MeaningPresentation::k2ch:
            return edit.twoch_dialogue_norm;
        case MeaningPresentation::k6ch:
            return edit.sixch_dialogue_norm;
        case MeaningPresentation::k8ch:
            break;
    }
    return edit.eightch_dialogue_norm;
}

bool control_of(const th::ChannelMeaning& meaning, MeaningPresentation presentation) {
    switch (presentation) {
        case MeaningPresentation::k2ch:
            return meaning.twoch_control_enabled;
        case MeaningPresentation::k6ch:
            return meaning.sixch_control_enabled;
        case MeaningPresentation::k8ch:
            break;
    }
    return meaning.eightch_control_enabled;
}

int dialogue_norm_field(const th::ChannelMeaning& meaning, MeaningPresentation presentation) {
    switch (presentation) {
        case MeaningPresentation::k2ch:
            return meaning.twoch_dialogue_norm;
        case MeaningPresentation::k6ch:
            return meaning.sixch_dialogue_norm;
        case MeaningPresentation::k8ch:
            break;
    }
    return meaning.eightch_dialogue_norm;
}

}  // namespace

int run_normalize_truehd(std::vector<std::byte>&& bytes, std::string_view in_path,
                         std::string_view out_path, const Options& meta) {
    // E-AC-3's normalize takes no option, and neither does this one.
    if (const int refused = check_options_applied(meta, {}); refused != kExitOk) {
        return refused;
    }
    const auto info = th::probe_stream(bytes);
    if (!info) {
        fmt::println(stderr, "error: {}: {}{}{}", in_path, th::to_string(info.error().kind),
                     info.error().detail.empty() ? "" : ": ", info.error().detail);
        return kExitInput;
    }
    if (info->major_sync.kind != th::StreamKind::kTrueHd) {
        fmt::println(stderr,
                     "error: {} is MLP: the bytes after substream_info that would hold the "
                     "dialogue norm are not described (TH spec §5.2)",
                     in_path);
        return kExitUsage;
    }
    // One measurement of each presentation the stream has: the decode of its substreams, metered
    // as `loudness` meters it. The audio is not changed, so the decodes are thrown away.
    const std::vector<MeaningPresentation> present = presentations_of(info->num_substreams);
    const auto measured = measure_truehd(bytes, in_path, present, /*integrity=*/false,
                                         /*lenient=*/false);
    if (!measured) {
        return kExitInput;
    }
    th::ChannelMeaningEdit edit;
    for (const PresentationLoudness& loudness : measured->presentations) {
        const char* name = presentation_name(loudness.presentation);
        if (loudness.unnamed) {
            fmt::println(stderr,
                         "error: {}: the stream does not name the speakers of its {} presentation, "
                         "so no BS.1770 weights can be chosen",
                         in_path, name);
            return kExitInput;
        }
        if (!loudness.integrated_lkfs) {
            fmt::println(stderr,
                         "error: no audio above the -70 LKFS absolute gate in the {} presentation; "
                         "nothing to normalise against",
                         name);
            return kExitRuntime;
        }
        // ATSC A/85 section 8, as E-AC-3's normalize takes it: dialnorm states where dialogue
        // sits relative to full scale, and the anchor for a programme with no separate dialogue
        // measurement is its own integrated loudness. The same mapping, clamped to 1..31, which
        // the 5 bit fields hold and the 6 bit one never needs (ERRATA.md question 51).
        dialogue_norm_of(edit, loudness.presentation) =
            iclforge::ac3::meta::dialnorm_from_lkfs(*loudness.integrated_lkfs);
    }
    const auto summary = th::edit_channel_meaning(bytes, edit);
    if (!summary) {
        return report_edit_error(summary.error(), in_path);
    }
    if (!write_stream(out_path, bytes, meta.keep_partial)) {
        return kExitOutput;
    }
    const auto status = status_stream(out_path);
    status_println(status, "normalised {} ({} access units, {} major syncs, audio untouched) -> {}",
                   in_path, summary->units, summary->major_syncs, out_path);
    for (const PresentationLoudness& loudness : measured->presentations) {
        const char* name = presentation_name(loudness.presentation);
        status_println(status, "  {}  measured {:+.2f} LKFS (BS.1770-5 Annex 3 weights, {})", name,
                       *loudness.integrated_lkfs, loudness.layout);
        status_println(status, "  {}  dialogue norm {} -> {} (ATSC A/85 section 8)", name,
                       dialogue_norm_field(summary->before, loudness.presentation),
                       dialogue_norm_field(summary->after, loudness.presentation));
    }
    // What the changes in the major sync are, field by field, is above; what is left alone is said
    // once.
    status_println(status,
                   "  no gain is applied (the stream is lossless): only the dialogue norm fields "
                   "change; the mix level, source format, control bits and DRC start-up gains are "
                   "as they were");
    for (const PresentationLoudness& loudness : measured->presentations) {
        if (!control_of(summary->after, loudness.presentation)) {
            status_println(
                status,
                "  note: the {} control-enabled bit is off, so a decoder may not apply "
                "its dialogue norm (set it with metadata truehd-control-{}=on; ERRATA.md "
                "question 54)",
                presentation_name(loudness.presentation), presentation_name(loudness.presentation));
        }
    }
    status_println(status,
                   "  provisional, no oracle until the DEE set (libs/truehd/ERRATA.md, questions "
                   "51 to 55)");
    return kExitOk;
}

// --- qc
// ---------------------------------------------------------------------------------------------

namespace {

// The presentation the gates are judged on: truehd-presentation= (best, the default, is the largest
// the stream has).
std::optional<MeaningPresentation> gate_presentation(const std::string& option, int substreams) {
    const std::vector<MeaningPresentation> present = presentations_of(substreams);
    if (present.empty()) {
        return std::nullopt;
    }
    if (option.empty() || option == "best") {
        return present.back();
    }
    for (const MeaningPresentation presentation : present) {
        if (option == presentation_name(presentation)) {
            return presentation;
        }
    }
    return std::nullopt;
}

}  // namespace

int run_qc_truehd(std::span<const std::byte> stream, std::string_view in_path,
                  const Options& meta) {
    // preset= and the presentation the gates are judged on are applied; layout=, objects=,
    // programme= and the rest describe E-AC-3's or AC-4's soundfield and are refused by name.
    if (const int refused = check_options_applied(meta, {"preset=", "truehd-presentation="});
        refused != kExitOk) {
        return refused;
    }
    if (meta.truehd_presentation == "all") {
        fmt::println(stderr,
                     "error: qc judges one presentation: truehd-presentation= is best, 2ch, 6ch or "
                     "8ch ('all' adds the sixteen-channel substream, which is not a programme "
                     "to measure)");
        return kExitUsage;
    }
    const auto info = th::probe_stream(stream);
    if (!info) {
        fmt::println(stderr, "error: {}: {}{}{}", in_path, th::to_string(info.error().kind),
                     info.error().detail.empty() ? "" : ": ", info.error().detail);
        return kExitInput;
    }
    const std::vector<MeaningPresentation> present = presentations_of(info->num_substreams);
    const auto gated = gate_presentation(meta.truehd_presentation, info->num_substreams);
    if (!gated) {
        fmt::println(stderr, "error: {} has no {} presentation (it has {} substream(s))", in_path,
                     meta.truehd_presentation, info->num_substreams);
        return kExitUsage;
    }
    // The integrity gate is lenient on purpose: every failure is counted and reported, where a
    // strict decode would stop at the first.
    const auto measured = measure_truehd(stream, in_path, present, /*integrity=*/true,
                                         /*lenient=*/true);
    if (!measured || !measured->major_sync) {
        return kExitInput;
    }
    const th::MajorSync& sync = *measured->major_sync;
    const std::uint32_t rate = sync.sample_rate();
    const std::uint64_t per_unit =
        static_cast<std::uint64_t>(th::nominal_samples_per_access_unit(sync.ratebits()));
    const double seconds =
        static_cast<double>(measured->units * per_unit) / static_cast<double>(rate);
    std::string layout;
    for (const PresentationLoudness& loudness : measured->presentations) {
        if (loudness.presentation == *gated) {
            layout = loudness.layout;
        }
    }
    fmt::println("qc: {} ({}, {}, {} Hz, {} access unit(s), {:.2f} s)", in_path,
                 sync.kind == th::StreamKind::kTrueHd ? "TrueHD" : "MLP",
                 layout.empty() ? "speakers not named" : layout, rate, measured->units, seconds);
    fmt::println("  weights  (BS.1770-5 Annex 3, by the speakers the major sync names)");

    // --- the integrity gate
    // -------------------------------------------------------------------------
    const th::DecoderCounters& counters = measured->counters;
    const RateReport& rate_report = measured->rate;
    const bool units_ok = measured->skipped_bytes == 0 && measured->decode_errors == 0;
    const bool checks_ok = counters.check_failures == 0;
    const bool lossless_ok = counters.lossless_check_failures == 0;
    const bool rate_ok = rate_report.violations == 0;
    const bool integrity = units_ok && checks_ok && lossless_ok && rate_ok;
    fmt::println("integrity (TH spec 6.2 parity and CRC, 7.3 lossless check, 18 data rate):");
    fmt::println("  access units         {} read, {} byte(s) skipped, {} unreadable  {}",
                 measured->units, measured->skipped_bytes, measured->decode_errors,
                 units_ok ? "PASS" : "FAIL");
    fmt::println(
        "  checks               {} failed (check nibble, major sync, restart header, parity, "
        "segment CRC)  {}",
        counters.check_failures, checks_ok ? "PASS" : "FAIL");
    fmt::println("  lossless check       {} interval(s) ok, {} failed  {}",
                 counters.lossless_checks_ok, counters.lossless_check_failures,
                 lossless_ok ? "PASS" : "FAIL");
    if (rate_report.declared_bps != 0) {
        fmt::println(
            "  data rate            {:.2f} Mbit/s declared (field {}, TH spec v3.9 section 4.4), "
            "largest unit {:.2f} Mbit/s, slowest delivery {:.1f} ms of {:.0f} ms, {} unit(s) "
            "late  {}",
            static_cast<double>(rate_report.declared_bps) / 1e6, sync.peak_data_rate,
            static_cast<double>(rate_report.max_unit_rate_bps) / 1e6, rate_report.worst_latency_ms,
            rate_report.latency_limit_ms, rate_report.violations, rate_ok ? "PASS" : "FAIL");
    } else {
        fmt::println(
            "  data rate            the peak data rate field is 0: nothing to hold the stream to  "
            "PASS");
    }
    fmt::println("  integrity verdict: {}", integrity ? "PASS" : "FAIL");

    // --- loudness, the embedded metadata and the preset gates
    // ---------------------------------------
    bool gates_pass = true;
    const th::ChannelMeaning& meaning = sync.channel_meaning;
    for (const PresentationLoudness& loudness : measured->presentations) {
        const char* name = presentation_name(loudness.presentation);
        const bool judged = loudness.presentation == *gated;
        fmt::println("{} ({}){}:", name,
                     loudness.layout.empty() ? "speakers not named" : loudness.layout,
                     judged ? ", the presentation the gates are judged on" : "");
        if (loudness.unnamed) {
            fmt::println(
                "  the major sync does not name the speakers of this presentation: no BS.1770 "
                "weights, no measurement");
        } else if (loudness.integrated_lkfs) {
            fmt::println("  measured (BS.1770-5 Annex 3 / EBU Tech 3342 / BS.1770-4 Annex 2):");
            fmt::println("    integrated loudness  {:>+8.2f} LKFS", *loudness.integrated_lkfs);
            fmt::println("    loudness range       {}",
                         loudness.loudness_range_lu
                             ? fmt::format("{:>7.2f} LU", *loudness.loudness_range_lu)
                             : std::string{"n/a"});
            fmt::println("    true peak            {}",
                         loudness.true_peak_dbtp
                             ? fmt::format("{:>+8.2f} dBTP", *loudness.true_peak_dbtp)
                             : std::string{"n/a"});
        } else {
            fmt::println("  measured: no audio above the -70 LKFS absolute gate");
        }
        // The stream's own claim, as the major sync has it. Provisional: the unit is the
        // specification's for the 6 and 8 channel fields and assumed for the 2 channel one, and
        // whether a decoder applies the field at all depends on the control bit (ERRATA.md 51, 54).
        const int claimed = dialogue_norm_field(meaning, loudness.presentation);
        const bool has_meaning = sync.kind == th::StreamKind::kTrueHd;
        if (has_meaning) {
            fmt::println(
                "  embedded metadata (provisional, no oracle until the DEE set; ERRATA.md "
                "questions 51 to 55):");
            fmt::println("    {}", describe_presentation(meaning, loudness.presentation));
        } else {
            fmt::println("  embedded metadata: none described for MLP (TH spec §5.2)");
        }
        if (loudness.integrated_lkfs && has_meaning) {
            const double claimed_lkfs = 0.0 - static_cast<double>(claimed);
            const int implied = iclforge::ac3::meta::dialnorm_from_lkfs(*loudness.integrated_lkfs);
            fmt::println("  dialnorm check:");
            fmt::println("    claimed              {:>+8.2f} LKFS  (from dialogue norm {}{})",
                         claimed_lkfs, claimed,
                         control_of(meaning, loudness.presentation)
                             ? ""
                             : "; the control-enabled bit is off, so a decoder may not apply it");
            fmt::println(
                "    delta                {:>+8.2f} dB    (measured - claimed; positive = "
                "measured is louder)",
                *loudness.integrated_lkfs - claimed_lkfs);
            fmt::println("    measurement-derived dialogue norm would be {}{}", implied,
                         implied == claimed ? " (matches)" : fmt::format(", not {}", claimed));
        }
        if (judged && !report_qc_gates("", loudness.integrated_lkfs, loudness.true_peak_dbtp,
                                       meta.qc_preset)) {
            gates_pass = false;
        }
    }
    return integrity && gates_pass ? kExitOk : kExitQcGate;
}

}  // namespace forge_cli::commands
