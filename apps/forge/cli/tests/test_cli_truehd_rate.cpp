#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/truehd/encoder/layout.hpp"
#include "iclforge/truehd/encoder/ratectl.hpp"
#include "platform/process.hpp"

// `forge truehd-atmos` with the rate and layout options, and `forge probe detail=rate`
// (libs/truehd/ERRATA.md entry 71): the real binary, a stream of 12 object channels of a signal that
// is quiet for a second and noisy for a second, and the figures the probe reads back checked against
// the library's own reading of the same bytes.
//
// ICLFORGE_CLI_EXE comes from apps/forge/cli/tests/CMakeLists.txt.

namespace fs = std::filesystem;
namespace th = iclforge::truehd;

namespace {

constexpr int kExitUsage = 1;

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_truehd_rate_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::vector<std::byte> read_stream(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(raw.size());
    std::transform(raw.begin(), raw.end(), out.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    return out;
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

// Twelve object channels, `seconds` long: the first second a quiet tone in each, the rest noise of
// 14 bits (the tone is far cheaper to code than the noise, so the rate changes at the second).
std::vector<std::vector<float>> object_channels(int seconds) {
    const std::size_t frames = static_cast<std::size_t>(seconds) * 48000;
    std::vector<std::vector<float>> channels(12, std::vector<float>(frames));
    std::uint32_t state = 12345;
    for (std::size_t t = 0; t < frames; ++t) {
        for (std::size_t c = 0; c < 12; ++c) {
            state = state * 1664525U + 1013904223U;
            const double noise = static_cast<double>(state >> 8U) / 16777216.0 * 2.0 - 1.0;
            const double tone = std::sin(2.0 * std::numbers::pi * (200.0 + 90.0 * static_cast<double>(c)) *
                                         static_cast<double>(t) / 48000.0);
            channels[c][t] = static_cast<float>(t < 48000 ? 0.002 * tone : noise * 0.001);
        }
    }
    return channels;
}

struct Written {
    fs::path wav;
    fs::path stream;
    fs::path log;
};

Written write_atmos(const std::string& name, const std::string& options, int seconds = 2,
                    int expect_exit = 0) {
    const fs::path dir = scratch_dir();
    Written out{dir / (name + ".wav"), dir / (name + ".thd"), dir / (name + ".log")};
    REQUIRE(iclforge::ac3::io::write_wav_f32(out.wav.string(), object_channels(seconds), 48000)
                .has_value());
    CHECK(run_cli("truehd-atmos " + quoted(out.wav) + " " + quoted(out.stream) + " " + options,
                  out.log) == expect_exit);
    return out;
}

}  // namespace

TEST_CASE("probe detail=rate prints a figure for every restart interval", "[forge-cli][truehd][rate]") {
    const Written w = write_atmos("rate_probe", "truehd-restart=16");
    const fs::path log = w.stream.parent_path() / "rate_probe_probe.log";
    REQUIRE(run_cli("probe " + quoted(w.stream) + " detail=rate", log) == 0);
    const std::string text = read_text(log);

    // 2 seconds are 2400 units, 150 intervals of 16.
    CHECK(text.find("arrival model at 18000 kbit/s over 75 ms: 150 interval(s), mean ") !=
          std::string::npos);
    CHECK(text.find("late unit(s)") != std::string::npos);
    CHECK(text.find("  interval first unit  units       kbit/s  peak kbit/s  headroom ms  late") !=
          std::string::npos);
    // One row a restart interval, numbered from 0, each unit count 16.
    std::istringstream lines(text);
    std::string line;
    bool in_table = false;
    int rows = 0;
    while (std::getline(lines, line)) {
        if (line.starts_with("  interval")) {
            in_table = true;
            continue;
        }
        if (in_table && !line.empty()) {
            std::istringstream row(line);
            long number = -1;
            long first_unit = -1;
            long units = -1;
            row >> number >> first_unit >> units;
            CHECK(number == rows);
            CHECK(first_unit == 16L * rows);
            CHECK(units == 16);
            ++rows;
        }
    }
    CHECK(rows == 150);

    // The same figures every time.
    const fs::path again = w.stream.parent_path() / "rate_probe_probe2.log";
    REQUIRE(run_cli("probe " + quoted(w.stream) + " detail=rate", again) == 0);
    CHECK(read_text(again) == text);

    // Without the detail the report is the one it always was.
    const fs::path plain = w.stream.parent_path() / "rate_probe_plain.log";
    REQUIRE(run_cli("probe " + quoted(w.stream), plain) == 0);
    CHECK(read_text(plain).find("arrival model") == std::string::npos);
}

TEST_CASE("probe detail=rate json holds what the library reads from the same bytes",
          "[forge-cli][truehd][rate]") {
    const Written w = write_atmos("rate_json", "truehd-restart=16");
    const fs::path log = w.stream.parent_path() / "rate_json_probe.log";
    REQUIRE(run_cli("probe " + quoted(w.stream) + " json=1 detail=rate", log) == 0);
    const auto doc = nlohmann::json::parse(read_text(log));
    const auto& rate = doc.at("truehd").at("rate");

    const auto expected = th::analyse_rate(read_stream(w.stream));
    REQUIRE(expected.has_value());
    CHECK(rate.at("model_rate_bps").get<std::uint64_t>() == 18'000'000);
    CHECK(rate.at("latency_ms").get<int>() == 75);
    CHECK(rate.at("access_units").get<std::uint64_t>() == 2400);
    CHECK(rate.at("access_units").get<std::uint64_t>() == expected->units);
    CHECK(rate.at("bytes").get<std::uint64_t>() == expected->bytes);
    CHECK(rate.at("bitrate_bps").get<std::uint64_t>() == expected->bitrate_bps);
    CHECK(rate.at("peak_unit_bps").get<std::uint64_t>() == expected->peak_unit_bps);
    CHECK(rate.at("min_headroom_us").get<std::int64_t>() == expected->min_headroom_us);
    CHECK(rate.at("late_units").get<std::uint64_t>() == 0);
    const auto& intervals = rate.at("intervals");
    REQUIRE(intervals.size() == 150);
    REQUIRE(intervals.size() == expected->intervals.size());
    std::uint64_t units = 0;
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        CHECK(intervals[i].at("first_unit").get<std::uint64_t>() == expected->intervals[i].first_unit);
        CHECK(intervals[i].at("units").get<std::uint64_t>() == 16);
        CHECK(intervals[i].at("bitrate_bps").get<std::uint64_t>() ==
              expected->intervals[i].bitrate_bps);
        CHECK(intervals[i].at("min_headroom_us").get<std::int64_t>() ==
              expected->intervals[i].min_headroom_us);
        units += intervals[i].at("units").get<std::uint64_t>();
    }
    CHECK(units == 2400);
    // The noisy second costs more than the quiet one.
    CHECK(intervals[140].at("bitrate_bps").get<std::uint64_t>() >
          2 * intervals[2].at("bitrate_bps").get<std::uint64_t>());

    // A probe without the detail has no such member.
    const fs::path plain = w.stream.parent_path() / "rate_json_plain.log";
    REQUIRE(run_cli("probe " + quoted(w.stream) + " json=1", plain) == 0);
    CHECK_FALSE(nlohmann::json::parse(read_text(plain)).at("truehd").contains("rate"));
}

TEST_CASE("probe detail=rate runs the model at truehd-bitrate=", "[forge-cli][truehd][rate]") {
    const Written w = write_atmos("rate_model", "truehd-restart=16");
    const fs::path log = w.stream.parent_path() / "rate_model_probe.log";
    // A rate well under what the noisy second needs: some units are late, which the figures say.
    REQUIRE(run_cli("probe " + quoted(w.stream) + " json=1 detail=rate truehd-bitrate=6000", log) == 0);
    const auto doc = nlohmann::json::parse(read_text(log));
    const auto& rate = doc.at("truehd").at("rate");
    CHECK(rate.at("model_rate_bps").get<std::uint64_t>() == 6'000'000);
    CHECK(rate.at("late_units").get<std::uint64_t>() > 0);
    CHECK(rate.at("min_headroom_us").get<std::int64_t>() < 0);
    const auto expected = th::analyse_rate(read_stream(w.stream), 6'000'000);
    REQUIRE(expected.has_value());
    CHECK(rate.at("late_units").get<std::uint64_t>() == expected->late_units);

    // truehd-bitrate= means nothing to a probe without detail=rate: refused, not dropped.
    CHECK(run_cli("probe " + quoted(w.stream) + " truehd-bitrate=6000", log) == kExitUsage);
}

TEST_CASE("truehd-major-sync= puts a major sync every so many access units",
          "[forge-cli][truehd][rate]") {
    const Written w = write_atmos("rate_major", "truehd-restart=16 truehd-major-sync=64");
    const th::LayoutAudit audit = th::audit_layout(read_stream(w.stream));
    CAPTURE(audit.problem);
    CHECK(audit.ok);
    CHECK(audit.units == 2400);
    CHECK(audit.restarts == 150);
    CHECK(audit.major_syncs == 38);  // units 0, 64, ... 2368
    CHECK(audit.segments == 4 * audit.units);
    // It decodes, strictly, every interval's lossless check included.
    const fs::path log = w.stream.parent_path() / "rate_major_decode.log";
    CHECK(run_cli("decode " + quoted(w.stream) + " " +
                      quoted(w.stream.parent_path() / "rate_major_all.wav") +
                      " truehd-presentation=all",
                  log) == 0);
    // A multiple of the restart interval, or the encoder says so.
    const Written bad = write_atmos("rate_major_bad", "truehd-restart=16 truehd-major-sync=40", 1,
                                    kExitUsage);
    CHECK(read_text(bad.log).find("multiple") != std::string::npos);
}

TEST_CASE("truehd-rate-mode=shape fits a bitrate that the lossless stream does not",
          "[forge-cli][truehd][rate]") {
    const Written lossless = write_atmos("rate_shape_base", "truehd-restart=16");
    const auto base = th::analyse_rate(read_stream(lossless.stream));
    REQUIRE(base.has_value());
    std::uint32_t most = 0;
    for (const th::IntervalRate& interval : base->intervals) {
        most = std::max(most, interval.bitrate_bps);
    }
    // 15% under what the noisy second needs, in kbit/s.
    const std::uint32_t kbps = most / 20 * 17 / 1000;
    const std::string rate = "truehd-restart=16 truehd-bitrate=" + std::to_string(kbps);

    // Strict: the stream does not fit, and the encoder says what would help.
    const Written strict = write_atmos("rate_shape_strict", rate, 2, kExitUsage);
    const std::string refusal = read_text(strict.log);
    CHECK(refusal.find("rate exceeded") != std::string::npos);
    CHECK(refusal.find("truehd-rate-mode=shape") != std::string::npos);

    // Shaped: it is written, says what it gave up, and fits.
    const Written shaped = write_atmos("rate_shape", rate + " truehd-rate-mode=shape verify");
    const std::string text = read_text(shaped.log);
    INFO(text);
    CHECK(text.find("rate: held to " + std::to_string(kbps) + " kbit/s over 75 ms") !=
          std::string::npos);
    CHECK(text.find("gave up up to ") != std::string::npos);
    CHECK(text.find("object channels to fit the rate") != std::string::npos);
    CHECK(text.find("verified: 2400 access units") != std::string::npos);
    const auto at_target = th::analyse_rate(read_stream(shaped.stream), std::uint64_t{kbps} * 1000);
    REQUIRE(at_target.has_value());
    CHECK(at_target->late_units == 0);
    CHECK(at_target->min_headroom_us >= 0);
    const fs::path log = shaped.stream.parent_path() / "rate_shape_probe.log";
    REQUIRE(run_cli("probe " + quoted(shaped.stream) + " json=1 detail=rate truehd-bitrate=" +
                        std::to_string(kbps),
                    log) == 0);
    CHECK(nlohmann::json::parse(read_text(log))
              .at("truehd")
              .at("rate")
              .at("late_units")
              .get<std::uint64_t>() == 0);
    // The shaped stream is smaller than the lossless one.
    CHECK(fs::file_size(shaped.stream) < fs::file_size(lossless.stream));
}

TEST_CASE("the rate options are refused where they mean nothing", "[forge-cli][truehd][rate]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "rate_refused.log";
    const fs::path wav = dir / "rate_refused.wav";
    REQUIRE(iclforge::ac3::io::write_wav_f32(wav.string(), object_channels(1), 48000).has_value());
    // truehd-encode has no object channels to shape.
    CHECK(run_cli("truehd-encode " + quoted(wav) + " " + quoted(dir / "x.thd") +
                      " truehd-rate-mode=shape",
                  log) == kExitUsage);
    CHECK(read_text(log).find("only truehd-atmos has") != std::string::npos);
    CHECK(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(dir / "x.thd") +
                      " truehd-shape-bits=17",
                  log) == kExitUsage);
    CHECK(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(dir / "x.thd") +
                      " truehd-bitrate=999",
                  log) == kExitUsage);
    CHECK(run_cli("truehd-atmos " + quoted(wav) + " " + quoted(dir / "x.thd") +
                      " truehd-rate-mode=sometimes",
                  log) == kExitUsage);
}
