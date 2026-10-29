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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "iclforge/truehd/core/error.hpp"
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

}  // namespace forge_cli::commands
