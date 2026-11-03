#include <fmt/base.h>
#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "truehd.hpp"

// `cut` and `cat` for TrueHD and MLP elementary streams. The bytes of an access unit are never
// changed: a cut is a run of whole units that starts at one carrying a major sync (the start of a
// restart interval, where the decoder holds no state from before), and a join is two runs of whole
// units back to back. Nothing is re-encoded, so the lossless result of a decode of the output is
// the decode of the input, sliced or joined.
//
// What a join leaves at its seam is the second stream's first unit: it opens with a major sync and
// restart headers, so a decoder starts afresh there, and its `input_timing` word (TH spec §3.4) is
// the second stream's own counter, not a continuation of the first's. The words are carried as they
// are (libs/truehd/ERRATA.md, entry 80; planning/truehd-parity-forge.md, question 14).

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;

struct Unit {
    std::size_t offset = 0;
    std::size_t length = 0;
    bool major_sync = false;
};

struct Walk {
    th::StreamInfo info;
    std::vector<Unit> units;
};

// The shape of a stream: what a decoder cannot follow a change of without a new stream.
struct Shape {
    th::StreamKind kind = th::StreamKind::kTrueHd;
    std::uint32_t format_info = 0;
    std::uint32_t sample_rate = 0;
    int substreams = 0;
    std::uint8_t substream_info = 0;
    std::array<int, th::kMaxSubstreams> channels{};
    bool object_substream = false;
};

Shape shape_of(const th::StreamInfo& info) {
    return Shape{.kind = info.major_sync.kind,
                 .format_info = info.major_sync.format_info,
                 .sample_rate = info.sample_rate,
                 .substreams = info.num_substreams,
                 .substream_info = info.major_sync.substream_info,
                 .channels = info.channels,
                 .object_substream = info.has_object_substream};
}

// What differs between two shapes, in words; empty when nothing does.
std::string_view difference(const Shape& a, const Shape& b) {
    if (a.kind != b.kind) {
        return "stream type (TrueHD or MLP)";
    }
    if (a.sample_rate != b.sample_rate) {
        return "sample rate";
    }
    if (a.substreams != b.substreams) {
        return "substream count";
    }
    if (a.channels != b.channels) {
        return "channels per substream";
    }
    if (a.format_info != b.format_info) {
        return "channel assignment";
    }
    if (a.substream_info != b.substream_info || a.object_substream != b.object_substream) {
        return "substream information";
    }
    return {};
}

// Every access unit of `stream`, each of which must read. A stream with a damaged unit is refused:
// a cut or a join of one would carry the damage into a new file.
std::expected<Walk, int> walk_units(std::span<const std::byte> stream, std::string_view path) {
    const auto info = th::probe_stream(stream);
    if (!info) {
        fmt::println(stderr, "error: {}: {}", path,
                     info.error().detail.empty() ? std::string{th::to_string(info.error().kind)}
                                                 : info.error().detail);
        return std::unexpected(kExitInput);
    }
    Walk walk{.info = *info, .units = {}};
    th::ElementaryReader reader(stream);
    int substreams = 0;
    while (const auto unit = reader.next()) {
        const auto parsed = th::parse_access_unit(*unit, substreams, true);
        if (!parsed) {
            fmt::println(stderr,
                         "error: {}: access unit {} does not read; run 'forge probe' on it first",
                         path, walk.units.size());
            return std::unexpected(kExitInput);
        }
        if (parsed->major_sync) {
            substreams = parsed->major_sync->num_substreams;
        }
        walk.units.push_back(Unit{.offset = static_cast<std::size_t>(unit->data() - stream.data()),
                                  .length = unit->size(),
                                  .major_sync = parsed->major_sync.has_value()});
    }
    if (reader.skipped_bytes() - reader.leading_bytes() != 0) {
        fmt::println(stderr,
                     "error: {}: {} byte(s) between access units do not read as one; run 'forge "
                     "probe' on it first",
                     path, reader.skipped_bytes() - reader.leading_bytes());
        return std::unexpected(kExitInput);
    }
    if (reader.leading_bytes() != 0) {
        fmt::println(stderr,
                     "warning: {}: {} byte(s) ahead of the first access unit are not part of the "
                     "stream and are not copied",
                     path, reader.leading_bytes());
    }
    return walk;
}

double unit_seconds(const Walk& walk, std::size_t index) {
    return static_cast<double>(index) * walk.info.samples_per_access_unit /
           static_cast<double>(walk.info.sample_rate);
}

const char* codec_text(const th::StreamInfo& info) {
    return info.major_sync.kind == th::StreamKind::kTrueHd ? "TrueHD" : "MLP";
}

int total_channels(const th::StreamInfo& info) {
    int most = 0;
    for (int i = 0; i < info.num_substreams; ++i) {
        most = std::max(most, info.channels[static_cast<std::size_t>(i)]);
    }
    return most;
}

}  // namespace

int run_cut_truehd(std::span<const std::byte> stream, std::string_view in_path,
                   std::string_view out_path, std::string_view start_seconds,
                   std::string_view duration_seconds, const Options& meta) {
    if (const int refused = check_options_applied(meta, {}); refused != kExitOk) {
        return refused;
    }
    const auto walk = walk_units(stream, in_path);
    if (!walk) {
        return walk.error();
    }
    const double start = start_seconds.empty() ? 0.0 : parse_seconds_or(start_seconds, 0.0);
    if (start < 0.0) {
        fmt::println(stderr, "error: start must not be negative");
        return kExitUsage;
    }
    const double unit_length = unit_seconds(*walk, 1);
    const std::size_t total = walk->units.size();

    // The first unit at or after the requested time that carries a major sync. The small
    // allowance keeps a time that is a whole number of units from landing one unit late.
    const auto wanted = static_cast<std::size_t>(std::ceil(start / unit_length - 1e-9));
    std::size_t first = wanted;
    while (first < total && !walk->units[first].major_sync) {
        ++first;
    }
    if (first >= total) {
        if (wanted >= total) {
            fmt::println(stderr, "error: start {:.3f} s is past the end of {} ({:.3f} s)", start,
                         in_path, unit_seconds(*walk, total));
        } else {
            fmt::println(stderr,
                         "error: no access unit with a major sync at or after {:.3f} s in {}: a "
                         "cut can start only at one (the last is at {:.3f} s)",
                         start, in_path, unit_seconds(*walk, [&] {
                             std::size_t last_sync = 0;
                             for (std::size_t i = 0; i < total; ++i) {
                                 if (walk->units[i].major_sync) {
                                     last_sync = i;
                                 }
                             }
                             return last_sync;
                         }()));
        }
        return kExitUsage;
    }

    std::size_t last = total;  // exclusive
    if (!duration_seconds.empty()) {
        const double duration = parse_seconds_or(duration_seconds, 0.0);
        if (duration <= 0.0) {
            fmt::println(stderr, "error: duration must be positive");
            return kExitUsage;
        }
        const auto count = static_cast<std::size_t>(std::ceil(duration / unit_length - 1e-9));
        last = std::min(total, first + std::max<std::size_t>(count, 1));
    }

    EncodedStreamSink sink;
    if (!sink.open(out_path, false)) {
        return kExitOutput;
    }
    for (std::size_t i = first; i < last; ++i) {
        const Unit& unit = walk->units[i];
        if (!sink.push(stream.subspan(unit.offset, unit.length))) {
            sink.abort();
            return kExitOutput;
        }
    }
    if (!sink.close()) {
        return kExitOutput;
    }

    const auto status = status_stream(out_path);
    status_println(status, "cut {} access units of {} from {} -> {}", last - first, total, in_path,
                   out_path);
    status_println(status, "  {}, {} Hz, {} channel(s), {:.3f} s from {:.3f} s", codec_text(walk->info),
                   walk->info.sample_rate, total_channels(walk->info),
                   unit_seconds(*walk, last - first), unit_seconds(*walk, first));
    if (first != wanted) {
        status_println(status,
                       "  started at the first major sync at or after {:.3f} s: unit {} at {:.3f} s",
                       start, first, unit_seconds(*walk, first));
    } else {
        status_println(status, "  started at unit {}, which carries a major sync", first);
    }
    return kExitOk;
}

int run_cat_truehd(std::string_view out_path, std::span<const std::string_view> in_paths,
                   const Options& meta) {
    if (const int refused = check_options_applied(meta, {}); refused != kExitOk) {
        return refused;
    }
    // As the AC-3 family's join: the output is opened before the inputs are read, one at a time,
    // so an input named as the output would be truncated first. Refused, whether or not the
    // output exists yet.
    if (!is_stdio_path(out_path)) {
        const auto same_file = [](const std::filesystem::path& a, const std::filesystem::path& b) {
            std::error_code ec;
            if (std::filesystem::equivalent(a, b, ec)) {
                return true;
            }
            const auto a_norm = std::filesystem::weakly_canonical(a, ec);
            if (ec) {
                return false;
            }
            const auto b_norm = std::filesystem::weakly_canonical(b, ec);
            return !ec && a_norm == b_norm;
        };
        for (const auto path : in_paths) {
            if (!is_stdio_path(path) && same_file(std::filesystem::path{std::string{out_path}},
                                                  std::filesystem::path{std::string{path}})) {
                fmt::println(stderr, "error: {} is both an input and the output", path);
                return kExitUsage;
            }
        }
    }
    EncodedStreamSink sink;
    if (!sink.open(out_path, false)) {
        return kExitOutput;
    }
    std::optional<Shape> reference;
    std::string_view reference_path;
    std::optional<th::ChannelMeaning> reference_meaning;
    th::StreamInfo reference_info;
    std::size_t units = 0;
    std::uint64_t samples = 0;
    bool meaning_differs = false;
    for (const auto path : in_paths) {
        const auto bytes = read_all(path);
        if (bytes.empty()) {
            fmt::println(stderr, "error: cannot read {}", path);
            sink.abort();
            return kExitInput;
        }
        if (!is_truehd_stream(bytes)) {
            fmt::println(stderr,
                         "error: {} is not a TrueHD or MLP stream, and a join takes streams of one "
                         "codec ({} is)",
                         path, in_paths.front());
            sink.abort();
            return kExitUsage;
        }
        const auto walk = walk_units(bytes, path);
        if (!walk) {
            sink.abort();
            return walk.error();
        }
        // The join is at a restart: the second stream's first unit has to be one that opens with a
        // major sync, and so does every stream's, because the bytes of a stream that starts
        // inside a restart interval cannot be decoded until the next one. A file with bytes ahead
        // of its first major sync does not open with one.
        if (walk->units.empty() || !walk->units.front().major_sync ||
            walk->units.front().offset != 0) {
            fmt::println(stderr,
                         "error: {} does not open with an access unit that carries a major sync "
                         "(at byte 0): a join can be made only at a restart",
                         path);
            sink.abort();
            return kExitUsage;
        }
        const Shape shape = shape_of(walk->info);
        if (!reference) {
            reference = shape;
            reference_path = path;
            reference_info = walk->info;
            reference_meaning = walk->info.major_sync.channel_meaning;
        } else {
            if (const auto what = difference(*reference, shape); !what.empty()) {
                fmt::println(stderr,
                             "error: {} differs from {} in {} - a decoder cannot follow that "
                             "across a join",
                             path, reference_path, what);
                sink.abort();
                return kExitUsage;
            }
            meaning_differs =
                meaning_differs || walk->info.major_sync.channel_meaning != *reference_meaning;
        }
        for (const Unit& unit : walk->units) {
            if (!sink.push(std::span<const std::byte>(bytes).subspan(unit.offset, unit.length))) {
                sink.abort();
                return kExitOutput;
            }
        }
        units += walk->units.size();
        samples += static_cast<std::uint64_t>(walk->units.size()) *
                   static_cast<std::uint64_t>(walk->info.samples_per_access_unit);
    }
    if (!sink.close()) {
        return kExitOutput;
    }
    const auto status = status_stream(out_path);
    status_println(status, "joined {} files, {} {} access units -> {}", in_paths.size(), units,
                   codec_text(reference_info), out_path);
    status_println(status, "  {:.3f} s, {} Hz, {} channel(s)",
                   static_cast<double>(samples) / static_cast<double>(reference_info.sample_rate),
                   reference_info.sample_rate, total_channels(reference_info));
    // forge's own strict decode reads the second stream's first lossless check as the first's last
    // interval's and stops there (libs/truehd/ERRATA.md, question 20).
    status_println(status,
                   "  a decode with the default truehd-check=strict stops at the join "
                   "(ERRATA question 20); truehd-check=lenient goes on");
    if (meaning_differs) {
        status_println(status,
                       "  the inputs' channel meanings (dialogue norm, mix level) differ; each "
                       "input's major syncs keep their own");
    }
    return kExitOk;
}

}  // namespace forge_cli::commands
