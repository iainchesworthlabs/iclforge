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
// report gained when it moved to apps/shared/media.
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

// --- detail=matrix ------------------------------------------------------------------------------
// Every matrix section and output_shift of the block headers, in all four substreams and with no
// cap, so that a substream with none reads as none.

namespace {

fs::path handbuilt(const std::string& name) {
    return fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / "handbuilt" / (name + ".thd");
}

// The `matrix` member of `probe detail=matrix json=1`.
nlohmann::json matrix_json(const fs::path& file, const fs::path& dir) {
    return probe_json(file, dir, "detail=matrix")["truehd"]["matrix"];
}

}  // namespace

TEST_CASE("probe detail=matrix lists the matrices of a stream this encoder wrote",
          "[forge-cli][truehd][probe][matrix]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "matrix_encode.log";
    const fs::path source = dir / "matrix_51.wav";
    const fs::path stream = dir / "matrix_own.thd";
    REQUIRE(run_cli("decode " + arg(fixture("truehd_m51_s24.thd")) + arg(source), log) == 0);
    // The standard downmix puts the 2.0 matrices in substream 1 and sends them at each restart.
    REQUIRE(run_cli("truehd-encode " + arg(source) + arg(stream) + "truehd-downmix=standard", log) ==
            0);

    const auto matrix = matrix_json(stream, dir);
    CHECK(matrix["capped"] == false);
    REQUIRE(matrix["substreams"].size() == 4);
    CHECK(matrix["substreams"][0]["matrix_sets"] == 0);
    CHECK(matrix["substreams"][1]["matrix_sets"] == 3);
    CHECK(matrix["substreams"][1]["matrices"] == 6);
    CHECK(matrix["substreams"][2]["matrix_sets"] == 0);
    // No object substream: it reads as none, not as cut off.
    CHECK(matrix["substreams"][3]["matrix_sets"] == 0);
    CHECK(matrix["substreams"][3]["matrices"] == 0);
    REQUIRE(matrix["headers"].size() == 3);
    const auto& first = matrix["headers"][0];
    CHECK(first["unit"] == 0);
    CHECK(first["substream"] == 1);
    CHECK(first["restart"] == true);
    CHECK(first["matrix_channels"] == 6);
    REQUIRE(first["matrices"].size() == 2);
    CHECK(first["matrices"][0]["out"] == 0);
    CHECK(first["matrices"][0]["frac_bits"] == 14);
    CHECK(first["matrices"][0]["coeff"]["m0"] == 16384);
    CHECK(first["matrices"][0]["coeff"]["m2"] == -11585);
    CHECK(matrix["headers"][2]["unit"] == 256);

    // The text form says the same, and the plain report is untouched.
    REQUIRE(run_cli("probe " + arg(stream) + "detail=matrix", log) == 0);
    const std::string text = read_file(log);
    CHECK(text.find("3 block header(s) with matrices or output shifts, all substreams, no cap") !=
          std::string::npos);
    CHECK(text.find("substream 3: 0 matrix set(s) with 0 matrices") != std::string::npos);
    CHECK(text.find("unit 128 sub 1 block 0 sample 0 restart yes matrix_channels 6 count 2") !=
          std::string::npos);
    CHECK(text.find("coeff m0:16384 m2:-11585 m4:-11585") != std::string::npos);
    const auto plain = probe_json(stream, dir)["truehd"];
    auto detailed = probe_json(stream, dir, "detail=matrix")["truehd"];
    CHECK_FALSE(plain.contains("matrix"));
    detailed.erase("matrix");
    CHECK(detailed == plain);
    REQUIRE(run_cli("probe " + arg(stream), log) == 0);
    CHECK(read_file(log).find("matrix_set") == std::string::npos);
}

TEST_CASE("probe detail=matrix reads the matrices of the hand-built streams, substream 3 among them",
          "[forge-cli][truehd][probe][matrix]") {
    const fs::path dir = scratch_dir();

    SECTION("hb_matrices: three primitive matrices of the bed, coefficients as sent") {
        const auto matrix = matrix_json(handbuilt("hb_matrices"), dir);
        CHECK(matrix["capped"] == false);
        CHECK(matrix["substreams"][0]["matrix_sets"] == 3);
        CHECK(matrix["substreams"][0]["matrices"] == 9);
        CHECK(matrix["substreams"][3]["matrix_sets"] == 0);
        const auto& header = matrix["headers"][0];
        REQUIRE(header["matrices"].size() == 3);
        CHECK(header["matrices"][0]["out"] == 4);
        CHECK(header["matrices"][0]["frac_bits"] == 8);
        CHECK(header["matrices"][0]["coeff"] ==
              nlohmann::json({{"m2", 41}, {"m3", -23}, {"m4", 256}}));
        CHECK(header["matrices"][1]["coeff"] == nlohmann::json({{"m0", 90}, {"m5", 256}}));
        CHECK(header["matrices"][0]["slope"].empty());
    }

    SECTION("hb_substream3_object_extension: the object substream's matrices, slopes and fields") {
        const auto matrix = matrix_json(handbuilt("hb_substream3_object_extension"), dir);
        // The matrices are all in substream 3; the three bed substreams send none.
        for (std::size_t s = 0; s < 3; ++s) {
            CHECK(matrix["substreams"][s]["matrix_sets"] == 0);
        }
        CHECK(matrix["substreams"][3]["matrix_sets"] == 12);
        CHECK(matrix["substreams"][3]["matrices"] == 48);
        const nlohmann::json* object_header = nullptr;
        for (const auto& header : matrix["headers"]) {
            if (header["substream"] == 3 && header.contains("matrices")) {
                object_header = &header;
                break;
            }
        }
        REQUIRE(object_header != nullptr);
        CHECK((*object_header)["extension"] == true);
        CHECK((*object_header)["coeffs_sent"] == true);
        CHECK((*object_header)["deltas_sent"] == true);
        REQUIRE((*object_header)["matrices"].size() == 4);
        const auto& m0 = (*object_header)["matrices"][0];
        CHECK(m0["out"] == 8);
        CHECK(m0["frac_bits"] == 12);
        CHECK(m0["field_m4"] == 2);
        CHECK(m0["ext_width"] == 3);
        CHECK(m0["ext_field2"] == 1);
        CHECK(m0["coeff"] == nlohmann::json({{"m1", 777}, {"m3", -1500}, {"m4", 0}, {"m8", 4096}}));
        CHECK(m0["slope"] == nlohmann::json({{"m1", -5}, {"m3", 7}, {"m4", 6}, {"m8", 0}}));
        const auto& m1 = (*object_header)["matrices"][1];
        CHECK(m1["f_shift"] == 1);
        CHECK(m1["frac_bits"] == 13);
        // A matrix without a width has no slope.
        CHECK((*object_header)["matrices"][3]["slope"].empty());
    }
}

TEST_CASE("probe detail=matrix prints output_shift by matrix channel, with the output it goes to",
          "[forge-cli][truehd][probe][matrix]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "matrix_shift.log";

    // hb_output_shift_negative: ch_assign [2, 0, 1] and shifts -1, -3, 0, sent at both restarts.
    const auto matrix = matrix_json(handbuilt("hb_output_shift_negative"), dir);
    CHECK(matrix["substreams"][0]["output_shift_headers"] == 2);
    CHECK(matrix["substreams"][0]["output_shift_headers_nonzero"] == 2);
    CHECK(matrix["substreams"][0]["matrix_sets"] == 0);
    REQUIRE(matrix["headers"].size() == 2);
    const auto expected = nlohmann::json::parse(
        R"([{"matrix_channel":0,"output_channel":2,"shift":-1},)"
        R"({"matrix_channel":1,"output_channel":0,"shift":-3},)"
        R"({"matrix_channel":2,"output_channel":1,"shift":0}])");
    CHECK(matrix["headers"][0]["output_shift"] == expected);
    CHECK_FALSE(matrix["headers"][0].contains("matrices"));

    REQUIRE(run_cli("probe " + arg(handbuilt("hb_output_shift_negative")) + "detail=matrix", log) ==
            0);
    CHECK(read_file(log).find("unit 16 sub 0 block 0 sample 0 restart yes shifts m0->out2:-1 "
                              "m1->out0:-3 m2->out1:0") != std::string::npos);

    // A permuted assignment with positive shifts: the shift stays with the matrix channel.
    const auto permuted = matrix_json(handbuilt("hb_output_shift_permuted"), dir);
    const auto& shifts = permuted["headers"][0]["output_shift"];
    REQUIRE(shifts.size() == 3);
    CHECK(shifts[0]["shift"] == 1);
    CHECK(shifts[1]["shift"] == 2);
    CHECK(shifts[2]["shift"] == 3);
}

TEST_CASE("probe detail=matrix is refused for a stream that is not TrueHD or MLP",
          "[forge-cli][truehd][probe][matrix]") {
    const fs::path dir = scratch_dir();
    const fs::path ec3 =
        fs::path{ICLFORGE_GOLDEN_AUDIO_DIR} / "reference_51_eac3_448k_cplbndstrce0.ec3";
    const fs::path log = dir / "matrix_ec3.log";
    CHECK(run_cli("probe " + arg(ec3) + "detail=matrix", log) == 1);
}
