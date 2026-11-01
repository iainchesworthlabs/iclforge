#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <vector>

#include "platform/process.hpp"

// What `forge probe json=1` says of every TrueHD and MLP stream in testdata/truehd (the FFmpeg
// fixtures and the hand-built ones), held against a golden document for each, and the fields the
// report gained when it moved to apps/shared/media (planning/truehd-parity-forge.md, row M1).
//
// A golden is the document without its `generator` and `file` members, which name the build and the
// checkout. To rewrite them after a change that is meant (and reviewed), create
// a file named UPDATE_GOLDEN in testdata/truehd/golden (do not commit it).
//
// ICLFORGE_CLI_EXE and ICLFORGE_TRUEHD_FIXTURE_DIR come from apps/forge/cli/tests/CMakeLists.txt.

namespace fs = std::filesystem;

namespace {

constexpr int kExitDamaged = 1;

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_truehd_probe_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string arg(const fs::path& path) {
    return "\"" + path.string() + "\" ";
}

std::string read_file(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

fs::path fixture(const std::string& file) {
    return fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / file;
}

// The document `forge probe file json=1` writes, as the stdout-only log of a run that has no
// stderr: the run's own log is the document.
nlohmann::json probe_json(const fs::path& file, const fs::path& dir, const std::string& extra = {},
                          int expected_exit = 0) {
    const fs::path log = dir / "probe.json";
    REQUIRE(run_cli("probe " + arg(file) + "json=1 " + extra, log) == expected_exit);
    auto document = nlohmann::json::parse(read_file(log));
    return document;
}

nlohmann::json without_build(nlohmann::json document) {
    document.erase("generator");
    document.erase("file");
    return document;
}

}  // namespace

TEST_CASE("probe json=1 of every TrueHD and MLP fixture is its golden document",
          "[forge-cli][truehd][probe][golden]") {
    const fs::path dir = scratch_dir();
    const bool update = fs::exists(fixture("golden/UPDATE_GOLDEN"));
    std::vector<std::pair<fs::path, std::string>> inputs;  // path, golden name
    for (const auto& entry : fs::directory_iterator(fixture(""))) {
        const auto ext = entry.path().extension().string();
        if (entry.is_regular_file() && (ext == ".thd" || ext == ".mlp")) {
            inputs.emplace_back(entry.path(), entry.path().filename().string());
        }
    }
    for (const auto& entry : fs::directory_iterator(fixture("handbuilt"))) {
        if (entry.is_regular_file() && entry.path().extension() == ".thd") {
            inputs.emplace_back(entry.path(), "handbuilt_" + entry.path().filename().string());
        }
    }
    std::ranges::sort(inputs);
    REQUIRE(inputs.size() >= 29);
    for (const auto& [path, name] : inputs) {
        DYNAMIC_SECTION(name) {
            const fs::path golden = fixture("golden/" + name + ".probe.json");
            const fs::path log = dir / "probe.json";
            // A hand-built stream may be damaged on purpose, and then probe exits 1.
            const int code = run_cli("probe " + arg(path) + "json=1", log);
            REQUIRE((code == 0 || code == kExitDamaged));
            const auto document = without_build(nlohmann::json::parse(read_file(log)));
            if (update) {
                fs::create_directories(golden.parent_path());
                std::ofstream out{golden, std::ios::binary};
                out << document.dump(2) << "\n";
            }
            REQUIRE(fs::exists(golden));
            CHECK(document == nlohmann::json::parse(read_file(golden)));
        }
    }
}

TEST_CASE("probe json=1 gives the duration, the rates, the presentations and the restart spacing",
          "[forge-cli][truehd][probe]") {
    const fs::path dir = scratch_dir();
    const auto document = probe_json(fixture("truehd_m51_s24.thd"), dir);
    const auto& truehd = document.at("truehd");
    CHECK(truehd.at("access_units") == 300);
    CHECK(truehd.at("duration_seconds").get<double>() == 0.25);
    CHECK(truehd.at("bytes").get<std::uint64_t>() > 100000);
    CHECK(truehd.at("average_bit_rate_bps").get<double>() ==
          static_cast<double>(truehd.at("bytes").get<std::uint64_t>()) * 8.0 / 0.25);
    // Two substreams: the 2 channel presentation, then the 6 channel one with its speakers named.
    const auto& presentations = truehd.at("presentations");
    REQUIRE(presentations.size() == 2);
    CHECK(presentations[0].at("name") == "2ch");
    CHECK(presentations[0].at("layout") == nlohmann::json::array({"L", "R"}));
    CHECK(presentations[1].at("name") == "6ch");
    CHECK(presentations[1].at("channels") == 6);
    CHECK(presentations[1].at("layout") ==
          nlohmann::json::array({"L", "R", "C", "LFE", "Ls", "Rs"}));
    // 19 major syncs in 300 units, a restart interval of at most 16 units here.
    const auto& restarts = truehd.at("restart_spacing");
    CHECK(restarts.at("major_syncs") == 19);
    CHECK(restarts.at("min_units").get<int>() >= 1);
    CHECK(restarts.at("max_units").get<int>() >= restarts.at("min_units").get<int>());
    const auto& model = truehd.at("rate_model");
    CHECK(model.at("declared_bps") == truehd.at("peak_data_rate_bps"));
    CHECK(model.at("violations") == 0);
    CHECK(truehd.at("word_length").at("decoded_bits") == 24);
    CHECK(truehd.at("lossless").is_null());
}

TEST_CASE("probe says the word length of an MLP stream", "[forge-cli][truehd][probe]") {
    const fs::path dir = scratch_dir();
    const auto document = probe_json(fixture("mlp_st48_s24.mlp"), dir);
    const auto& word = document.at("truehd").at("word_length");
    CHECK(word.at("group1_bits") == 24);
    CHECK(document.at("truehd").at("channel_meaning").is_null());
}

TEST_CASE("probe detail=checks decodes for the lossless check, and exits 1 when one fails",
          "[forge-cli][truehd][probe]") {
    const fs::path dir = scratch_dir();
    SECTION("a stream that checks") {
        const auto document = probe_json(fixture("truehd_m51_s24.thd"), dir, "detail=checks");
        const auto& lossless = document.at("truehd").at("lossless");
        CHECK(lossless.at("intervals_ok").get<int>() > 0);
        CHECK(lossless.at("intervals_failed") == 0);
        CHECK(lossless.at("decode_errors") == 0);
    }
    SECTION("two streams joined end to end fail at the join (ERRATA question 20)") {
        const fs::path joined = fixture("truehd_cat_st48_s24.thd");
        // The headers and CRCs of a join are fine, so the plain report passes ...
        const auto plain = probe_json(joined, dir);
        CHECK(plain.at("truehd").at("lossless").is_null());
        // ... and the decode finds the interval the join breaks.
        const auto checked = probe_json(joined, dir, "detail=checks", kExitDamaged);
        CHECK(checked.at("truehd").at("lossless").at("intervals_failed").get<int>() >= 1);
    }
    SECTION("text") {
        const fs::path log = dir / "checks.log";
        REQUIRE(run_cli("probe " + arg(fixture("truehd_m51_s24.thd")) + "detail=checks", log) == 0);
        const std::string text = read_file(log);
        CHECK(text.find("lossless check") != std::string::npos);
        CHECK(text.find("duration") != std::string::npos);
        CHECK(text.find("word length") != std::string::npos);
    }
    SECTION("an AC-3 family stream refuses it") {
        const fs::path log = dir / "refused.log";
        const fs::path ec3 = fixture("../audio/reference_51_eac3_448k_cplbndstrce0.ec3");
        CHECK(run_cli("probe " + arg(ec3) + "detail=checks", log) == 1);
        CHECK(read_file(log).find("describes TrueHD and MLP streams") != std::string::npos);
    }
}

TEST_CASE("probe detail=frames lists every access unit", "[forge-cli][truehd][probe]") {
    const fs::path dir = scratch_dir();
    const fs::path stream = fixture("truehd_m51_s24.thd");
    SECTION("json") {
        const auto document = probe_json(stream, dir, "detail=frames");
        const auto& truehd = document.at("truehd");
        const auto& frames = truehd.at("frames");
        REQUIRE(frames.size() == 300);
        std::uint64_t bytes = 0;
        int major_syncs = 0;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            CHECK(frames[i].at("index") == i);
            CHECK(frames[i].at("checks_ok") == true);
            bytes += frames[i].at("bytes").get<std::uint64_t>();
            major_syncs += frames[i].at("major_sync").get<bool>() ? 1 : 0;
        }
        CHECK(bytes == truehd.at("bytes").get<std::uint64_t>());
        CHECK(major_syncs == truehd.at("restart_spacing").at("major_syncs").get<int>());
        // Each unit starts where the one before it ended.
        CHECK(frames[0].at("offset") == 0);
        CHECK(frames[1].at("offset") == frames[0].at("bytes"));
        CHECK(frames[299].at("time_seconds").get<double>() > 0.24);
    }
    SECTION("without it, no frames member") {
        CHECK_FALSE(probe_json(stream, dir).at("truehd").contains("frames"));
    }
    SECTION("text") {
        const fs::path log = dir / "frames.log";
        REQUIRE(run_cli("probe " + arg(stream) + "detail=frames", log) == 0);
        const std::string text = read_file(log);
        CHECK(text.find("access unit 0 @ 0 (") != std::string::npos);
        CHECK(text.find("access unit 299 @ ") != std::string::npos);
        CHECK(text.find(", major sync, input timing") != std::string::npos);
    }
    SECTION("a damaged unit says so") {
        std::string bytes = read_file(stream);
        // Unit 10's payload, as the damaged-copy test of test_cli_truehd.cpp flips it.
        std::size_t offset = 0;
        for (int unit = 0; unit < 10; ++unit) {
            offset += ((static_cast<std::size_t>(static_cast<unsigned char>(bytes[offset]) & 0xFU) << 8U) |
                       static_cast<unsigned char>(bytes[offset + 1])) *
                      2;
        }
        bytes[offset + 60] = static_cast<char>(bytes[offset + 60] ^ 0x04);
        const fs::path damaged = dir / "damaged_frames.thd";
        {
            std::ofstream out{damaged, std::ios::binary};
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        const fs::path log = dir / "damaged.log";
        CHECK(run_cli("probe " + arg(damaged) + "detail=frames", log) == kExitDamaged);
        CHECK(read_file(log).find("access unit 10 @ ") != std::string::npos);
        CHECK(read_file(log).find("CHECK FAILED") != std::string::npos);
    }
}
