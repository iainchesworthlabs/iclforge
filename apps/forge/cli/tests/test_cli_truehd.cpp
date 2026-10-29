#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "iclforge/base/crypto/sha256.hpp"
#include "platform/process.hpp"

// `forge decode` and `forge probe` on TrueHD and MLP streams: the real binary, run as a subprocess,
// over the FFmpeg fixtures of testdata/truehd/ (tools/generators/gen_truehd_ffmpeg.py). What is
// held here is what a library test cannot see: the WAV the command writes, the exit codes, and the
// status and JSON text. The decoder itself is held to FFmpeg in libs/truehd/tests/decoder.
//
// ICLFORGE_CLI_EXE and ICLFORGE_TRUEHD_FIXTURE_DIR come from apps/forge/cli/tests/CMakeLists.txt.

namespace fs = std::filesystem;

namespace {

constexpr int kExitUsage = 1;
constexpr int kExitInput = 2;

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_truehd_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

fs::path fixture(const std::string& file) {
    return fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / file;
}

std::string hex(const std::array<std::byte, 32>& digest) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : digest) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4U]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 15U]);
    }
    return out;
}

std::uint32_t le(const std::vector<std::byte>& bytes, std::size_t at, int width) {
    std::uint32_t value = 0;
    for (int i = width - 1; i >= 0; --i) {
        value =
            (value << 8U) | std::to_integer<std::uint32_t>(bytes[at + static_cast<std::size_t>(i)]);
    }
    return value;
}

// What the 68-byte header of forge's WAV says, and the samples as FFmpeg's `-f s32le` gives them.
struct Wav {
    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
    std::uint32_t bits = 0;
    std::uint32_t mask = 0;
    std::uint32_t data_bytes = 0;
    std::vector<std::byte> s32le;
};

Wav read_wav(const fs::path& path) {
    const std::vector<std::byte> bytes = read_bytes(path);
    REQUIRE(bytes.size() >= 68);
    CHECK(le(bytes, 20, 2) == 0xFFFE);
    Wav wav;
    wav.channels = le(bytes, 22, 2);
    wav.rate = le(bytes, 24, 4);
    wav.bits = le(bytes, 34, 2);
    wav.mask = le(bytes, 40, 4);
    wav.data_bytes = le(bytes, 64, 4);
    CHECK(le(bytes, 4, 4) == bytes.size() - 8);
    CHECK(wav.data_bytes == bytes.size() - 68);
    REQUIRE(wav.bits == 24);
    for (std::size_t at = 68; at + 3 <= bytes.size(); at += 3) {
        wav.s32le.push_back(std::byte{0});
        wav.s32le.insert(wav.s32le.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at),
                         bytes.begin() + static_cast<std::ptrdiff_t>(at + 3));
    }
    return wav;
}

nlohmann::json manifest() {
    std::ifstream in{fixture("manifest.json")};
    return nlohmann::json::parse(in);
}

// The fixture with one byte of its eleventh access unit's payload flipped.
fs::path damaged_copy(const std::string& file, const fs::path& dir) {
    std::vector<std::byte> bytes = read_bytes(fixture(file));
    std::size_t offset = 0;
    for (int unit = 0; unit < 10; ++unit) {
        // The length is the low 12 bits of the first two bytes, big-endian, in 16-bit words.
        offset += (((std::to_integer<std::size_t>(bytes[offset]) & 0xFU) << 8U) |
                   std::to_integer<std::size_t>(bytes[offset + 1])) *
                  2;
    }
    bytes[offset + 60] ^= std::byte{0x04};
    const fs::path path = dir / ("damaged_" + file);
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

}  // namespace

TEST_CASE("forge decode gives every fixture's PCM as a 24-bit WAV", "[forge-cli][truehd]") {
    const fs::path dir = scratch_dir();
    for (const auto& item : manifest()["fixtures"]) {
        const std::string name = item["name"];
        DYNAMIC_SECTION(name) {
            const fs::path wav_path = dir / (name + ".wav");
            const fs::path log = dir / (name + ".log");
            const std::string input = fixture(item["file"]).string();
            REQUIRE(run_cli("decode \"" + input + "\" \"" + wav_path.string() + "\"", log) == 0);
            const Wav wav = read_wav(wav_path);
            CHECK(wav.channels == item["channels"].get<std::uint32_t>());
            CHECK(wav.rate == item["sample_rate"].get<std::uint32_t>());
            CHECK(hex(iclforge::base::crypto::sha256(wav.s32le)) ==
                  item["pcm_sha256"].get<std::string>());
            const std::string status = read_text(log);
            CHECK(status.find("failed: 0") != std::string::npos);
            CHECK(status.find("lossless check") != std::string::npos);
        }
    }
}

TEST_CASE("forge decode names the 5.1 layout in the WAV and the status", "[forge-cli][truehd]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "layout.log";
    const fs::path out = dir / "layout.wav";
    REQUIRE(run_cli("decode \"" + fixture("truehd_m51_s24.thd").string() + "\" \"" + out.string() +
                        "\"",
                    log) == 0);
    const Wav wav = read_wav(out);
    CHECK(wav.mask ==
          0x60F);  // L R C LFE Ls Rs as FL FR FC LFE SL SR, in the order the stream has them
    const std::string status = read_text(log);
    CHECK(status.find("(L R C LFE Ls Rs)") != std::string::npos);
    CHECK(status.find("substream 1") != std::string::npos);
}

TEST_CASE("truehd-presentation= picks the substreams read", "[forge-cli][truehd]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "presentation.log";
    const fs::path out = dir / "presentation.wav";
    REQUIRE(run_cli("decode \"" + fixture("truehd_m51_s24.thd").string() + "\" \"" + out.string() +
                        "\" truehd-presentation=2ch",
                    log) == 0);
    const Wav stereo = read_wav(out);
    CHECK(stereo.channels == 2);
    CHECK(stereo.mask == 0x3);
    CHECK(stereo.data_bytes == 12000 * 2 * 3);
    CHECK(read_text(log).find("substream 0") != std::string::npos);

    CHECK(run_cli("decode \"" + fixture("truehd_m51_s24.thd").string() + "\" \"" + out.string() +
                      "\" truehd-presentation=7ch",
                  log) == kExitUsage);
}

TEST_CASE("a damaged stream fails a strict decode and is counted by a lenient one",
          "[forge-cli][truehd]") {
    const fs::path dir = scratch_dir();
    const fs::path bad = damaged_copy("truehd_st48_s24.thd", dir);
    const fs::path log = dir / "damaged.log";
    const fs::path out = dir / "damaged.wav";
    CHECK(run_cli("decode \"" + bad.string() + "\" \"" + out.string() + "\"", log) == kExitInput);
    const std::string strict = read_text(log);
    CHECK(strict.find("error:") != std::string::npos);
    CHECK(strict.find("access unit 10") != std::string::npos);

    REQUIRE(run_cli("decode \"" + bad.string() + "\" \"" + out.string() + "\" truehd-check=lenient",
                    log) == 0);
    const std::string lenient = read_text(log);
    CHECK(lenient.find("(access unit, CRC, parity) failed: 0") == std::string::npos);
    CHECK(read_wav(out).channels == 2);
}

TEST_CASE("forge probe describes a TrueHD and an MLP stream", "[forge-cli][truehd]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "probe.log";
    REQUIRE(run_cli("probe \"" + fixture("truehd_m51_s24.thd").string() + "\"", log) == 0);
    const std::string text = read_text(log);
    CHECK(text.find("TrueHD") != std::string::npos);
    CHECK(text.find("300 (40 sample(s) each)") != std::string::npos);
    CHECK(text.find("48000 Hz") != std::string::npos);
    CHECK(text.find("2, 6 channel(s)") != std::string::npos);

    REQUIRE(run_cli("probe \"" + fixture("mlp_st96_s24.mlp").string() + "\" json=1", log) == 0);
    const auto document = nlohmann::json::parse(read_text(log));
    CHECK(document["schema"] == "iclforge.probe/1");
    const auto& stream = document["truehd"];
    CHECK(stream["codec"] == "mlp");
    CHECK(stream["sample_rate"] == 96000);
    CHECK(stream["substreams"] == 1);
    CHECK(stream["channels"][0] == 2);
    CHECK(stream["access_units"] == 300);
    CHECK(stream["samples_per_access_unit"] == 80);
    CHECK(stream["segment_failures"] == 0);
    CHECK(stream["skipped_bytes"] == 0);
}

TEST_CASE("forge probe exits 1 on a damaged TrueHD stream", "[forge-cli][truehd]") {
    const fs::path dir = scratch_dir();
    const fs::path bad = damaged_copy("truehd_m51_s16.thd", dir);
    const fs::path log = dir / "probe_damaged.log";
    CHECK(run_cli("probe \"" + bad.string() + "\" json=1", log) == 1);
    const auto document = nlohmann::json::parse(read_text(log));
    CHECK(document["truehd"]["segment_failures"].get<int>() >= 1);
}
