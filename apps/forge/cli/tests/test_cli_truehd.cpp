#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/loudness.hpp"
#include "iclforge/base/speaker.hpp"
#include "multi_track_files.hpp"
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

// The manifest's fixtures, by value: a range-for over `manifest()["fixtures"]` iterates a reference
// into a temporary that is already gone when the first pass starts.
nlohmann::json fixture_list() {
    return manifest()["fixtures"];
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
    for (const auto& item : fixture_list()) {
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

TEST_CASE("forge probe json=1 of a TrueHD or MLP stream is valid JSON in every container",
          "[forge-cli][truehd]") {
    // The TrueHD report wrote the key "container" itself and then called the writer that writes it
    // again, so a stream in Matroska, MP4 or a transport stream gave a document no parser reads.
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "container_json.log";
    for (const auto& item : fixture_list()) {
        const std::string name = item["name"];
        const std::string input = fixture(item["file"]).string();
        DYNAMIC_SECTION(name + " bare") {
            REQUIRE(run_cli("probe \"" + input + "\" json=1", log) == 0);
            const auto document = nlohmann::json::parse(read_text(log));
            CHECK(document["container"].is_null());
            CHECK(document["truehd"]["codec"] == item["codec"]);
        }
        for (const std::string container : {"mkv", "mp4", "ts"}) {
            DYNAMIC_SECTION(name + " in " + container) {
                const fs::path file = dir / (name + "_json." + container);
                REQUIRE(run_cli(container + " \"" + input + "\" \"" + file.string() + "\"", log) ==
                        0);
                REQUIRE(run_cli("probe \"" + file.string() + "\" json=1", log) == 0);
                const auto document = nlohmann::json::parse(read_text(log));
                CHECK(document["schema"] == "iclforge.probe/1");
                REQUIRE(document["container"].is_object());
                CHECK(document["container"]["format"].is_string());
                CHECK(document["truehd"]["codec"] == item["codec"]);
                CHECK(document["truehd"]["access_units"] == item["access_units"]);
                CHECK(document["truehd"]["sample_rate"] == item["sample_rate"]);
            }
        }
    }
}

// --- truehd-encode -----------------------------------------------------------------------------
// The encoder is held to FFmpeg's strict decode in libs/truehd/tests and
// tools/ci/run_codec_matrix.sh; what is held here is the command: the WAV it reads (its channel
// mask above all), its options, its containers and its refusals, through the real binary.

namespace {

void put_le(std::vector<std::byte>& out, std::uint32_t value, int width) {
    for (int i = 0; i < width; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU));
    }
}

// A 24-bit WAVE_FORMAT_EXTENSIBLE file with `mask` as its dwChannelMask, as `forge decode` writes.
void write_wav(const fs::path& path, std::uint32_t rate, std::uint32_t channels, std::uint32_t mask,
               const std::vector<std::int32_t>& samples) {
    std::vector<std::byte> out;
    const auto text = [&out](const char* s) {
        for (; *s != '\0'; ++s) {
            out.push_back(static_cast<std::byte>(*s));
        }
    };
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 3);
    text("RIFF");
    put_le(out, 60 + data_bytes, 4);
    text("WAVEfmt ");
    put_le(out, 40, 4);
    put_le(out, 0xFFFE, 2);
    put_le(out, channels, 2);
    put_le(out, rate, 4);
    put_le(out, rate * channels * 3, 4);
    put_le(out, channels * 3, 2);
    put_le(out, 24, 2);
    put_le(out, 22, 2);
    put_le(out, 24, 2);
    put_le(out, mask, 4);
    constexpr std::array<std::uint8_t, 16> kPcmSubformat{0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                         0x10, 0x00, 0x80, 0x00, 0x00, 0xAA,
                                                         0x00, 0x38, 0x9B, 0x71};
    for (const std::uint8_t b : kPcmSubformat) {
        out.push_back(static_cast<std::byte>(b));
    }
    text("data");
    put_le(out, data_bytes, 4);
    for (const std::int32_t sample : samples) {
        put_le(out, static_cast<std::uint32_t>(sample) & 0xFFFFFFU, 3);
    }
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

// The WAV at `path` with its channel mask replaced.
void patch_mask(const fs::path& path, std::uint32_t mask) {
    std::vector<std::byte> bytes = read_bytes(path);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[40 + i] = static_cast<std::byte>((mask >> (8U * i)) & 0xFFU);
    }
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::string digest_of(const Wav& wav) {
    return hex(iclforge::base::crypto::sha256(wav.s32le));
}

}  // namespace

TEST_CASE("a TrueHD stream decoded, encoded and decoded again gives the same PCM",
          "[forge-cli][truehd][truehd-encode]") {
    const fs::path dir = scratch_dir();
    for (const auto& item : fixture_list()) {
        if (item["codec"] != "truehd") {
            continue;  // the encoder writes TrueHD, not MLP
        }
        const std::string name = item["name"];
        DYNAMIC_SECTION(name) {
            const fs::path first = dir / (name + "_first.wav");
            const fs::path stream = dir / (name + "_loop.thd");
            const fs::path second = dir / (name + "_second.wav");
            const fs::path log = dir / (name + "_loop.log");
            REQUIRE(run_cli("decode \"" + fixture(item["file"]).string() + "\" \"" +
                                first.string() + "\"",
                            log) == 0);
            REQUIRE(run_cli("truehd-encode \"" + first.string() + "\" \"" + stream.string() +
                                "\" verify",
                            log) == 0);
            CHECK(read_text(log).find("verified: ") != std::string::npos);
            REQUIRE(run_cli("decode \"" + stream.string() + "\" \"" + second.string() + "\"",
                            log) == 0);
            const Wav wav = read_wav(second);
            CHECK(wav.channels == item["channels"].get<std::uint32_t>());
            CHECK(wav.rate == item["sample_rate"].get<std::uint32_t>());
            CHECK(wav.mask == read_wav(first).mask);
            CHECK(digest_of(wav) == item["pcm_sha256"].get<std::string>());
        }
    }
}

TEST_CASE("truehd-encode reads the speakers of the WAV's channel mask",
          "[forge-cli][truehd][truehd-encode]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "mask.log";
    const fs::path source = dir / "mask_51.wav";
    REQUIRE(run_cli("decode \"" + fixture("truehd_m51_s24.thd").string() + "\" \"" +
                        source.string() + "\"",
                    log) == 0);
    const std::string expected = digest_of(read_wav(source));

    SECTION("a 5.1 file whose surrounds are the back pair is coded as L R C LFE Ls Rs") {
        patch_mask(source, 0x3F);  // FL FR FC LFE BL BR
        const fs::path stream = dir / "mask_51_back.thd";
        const fs::path out = dir / "mask_51_back.wav";
        REQUIRE(run_cli("truehd-encode \"" + source.string() + "\" \"" + stream.string() + "\"",
                        log) == 0);
        REQUIRE(run_cli("decode \"" + stream.string() + "\" \"" + out.string() + "\"", log) == 0);
        CHECK(read_text(log).find("(L R C LFE Ls Rs)") != std::string::npos);
        CHECK(digest_of(read_wav(out)) == expected);
    }

    SECTION("a mask that names a different number of speakers than the file has is a warning") {
        patch_mask(source, 0x7);
        const fs::path stream = dir / "mask_51_warn.thd";
        REQUIRE(run_cli("truehd-encode \"" + source.string() + "\" \"" + stream.string() + "\"",
                        log) == 0);
        CHECK(read_text(log).find(
                  "warning: the WAV's channel mask names 3 speakers for 6 channels") !=
              std::string::npos);
    }

    SECTION("a speaker TrueHD has no channel for is refused") {
        patch_mask(source, 0x1F | (1U << 15));  // top back left in place of the right surround
        const fs::path stream = dir / "mask_51_top.thd";
        CHECK(run_cli("truehd-encode \"" + source.string() + "\" \"" + stream.string() + "\"",
                      log) == kExitInput);
        CHECK(read_text(log).find("has no channel for") != std::string::npos);
    }

    SECTION(
        "a 7.1 file in WAV order (back pair, then side pair) is coded with the sides as Ls Rs") {
        constexpr std::size_t kFrames = 4800;
        std::vector<std::int32_t> samples;
        std::uint32_t state = 12345;
        for (std::size_t n = 0; n < kFrames; ++n) {
            for (std::int32_t c = 0; c < 8; ++c) {
                state = state * 1664525U + 1013904223U;
                samples.push_back(static_cast<std::int32_t>(state >> 9U) - (1 << 22) + c * 1000);
            }
        }
        const fs::path wav = dir / "mask_71.wav";
        write_wav(wav, 48000, 8, 0x63F, samples);  // FL FR FC LFE BL BR SL SR
        const fs::path stream = dir / "mask_71.thd";
        const fs::path out = dir / "mask_71_out.wav";
        REQUIRE(run_cli("truehd-encode \"" + wav.string() + "\" \"" + stream.string() + "\" verify",
                        log) == 0);
        REQUIRE(run_cli("decode \"" + stream.string() + "\" \"" + out.string() + "\"", log) == 0);
        CHECK(read_text(log).find("(L R C LFE Ls Rs Lrs Rrs)") != std::string::npos);
        // decode writes the stream's order, L R C LFE Ls Rs Lrs Rrs: the WAV's with its back pair
        // (columns 4 and 5) and its side pair (6 and 7) swapped.
        const Wav decoded = read_wav(out);
        REQUIRE(decoded.channels == 8);
        REQUIRE(decoded.s32le.size() == samples.size() * 4);
        constexpr std::array<std::size_t, 8> kWavOfStream{0, 1, 2, 3, 6, 7, 4, 5};
        bool equal = true;
        for (std::size_t n = 0; n < kFrames && equal; ++n) {
            for (std::size_t c = 0; c < 8 && equal; ++c) {
                const auto got =
                    static_cast<std::int32_t>(le(decoded.s32le, (n * 8 + c) * 4, 4)) >> 8;
                equal = got == samples[n * 8 + kWavOfStream[c]];
            }
        }
        CHECK(equal);
    }
}

TEST_CASE("truehd-encode options, containers and refusals", "[forge-cli][truehd][truehd-encode]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "options.log";
    const fs::path source = dir / "options_51.wav";
    REQUIRE(run_cli("decode \"" + fixture("truehd_m51_s24.thd").string() + "\" \"" +
                        source.string() + "\"",
                    log) == 0);
    const std::string expected = digest_of(read_wav(source));
    const std::string in = "\"" + source.string() + "\" ";

    SECTION("every option still gives the samples back") {
        const fs::path stream = dir / "options.thd";
        const fs::path out = dir / "options_out.wav";
        REQUIRE(run_cli("truehd-encode " + in + "\"" + stream.string() +
                            "\" truehd-downmix=standard truehd-downmix-gains=-6,-6,off,-6 "
                            "truehd-restart=8 truehd-fir-order=2 truehd-matrix=off "
                            "truehd-stereo-mode=ltrt truehd-peak-rate=18000 "
                            "truehd-rate-check=lenient verify",
                        log) == 0);
        REQUIRE(run_cli("decode \"" + stream.string() + "\" \"" + out.string() + "\"", log) == 0);
        CHECK(digest_of(read_wav(out)) == expected);
        REQUIRE(run_cli("decode \"" + stream.string() + "\" \"" + out.string() +
                            "\" truehd-presentation=2ch",
                        log) == 0);
        CHECK(read_wav(out).channels == 2);
    }

    SECTION("Matroska, MP4 and a Blu-ray transport stream carry the stream") {
        for (const std::string container : {"mkv", "mp4", "ts"}) {
            const fs::path file = dir / ("options." + container);
            const fs::path out = dir / ("options_" + container + ".wav");
            REQUIRE(run_cli("truehd-encode " + in + "\"" + file.string() + "\"", log) == 0);
            REQUIRE(run_cli("decode \"" + file.string() + "\" \"" + out.string() + "\"", log) == 0);
            CHECK(digest_of(read_wav(out)) == expected);
        }
    }

    SECTION("the bare stream on stdout is the file") {
        const fs::path stream = dir / "options_file.thd";
        const fs::path piped = dir / "options_stdout.thd";
        REQUIRE(run_cli("truehd-encode " + in + "\"" + stream.string() + "\"", log) == 0);
        const std::string command = "\"" + std::string(ICLFORGE_CLI_EXE) + "\" truehd-encode " +
                                    in + "- quiet > \"" + piped.string() + "\"";
        REQUIRE(iclforge::test::platform::run_shell(command) == 0);
        CHECK(read_bytes(piped) == read_bytes(stream));
    }

    SECTION("a name, an option or a rate the encoder cannot write is a usage error") {
        CHECK(run_cli("truehd-encode " + in + "\"" + (dir / "refused.mlp").string() + "\"", log) ==
              kExitUsage);
        CHECK(read_text(log).find("TrueHD, not MLP") != std::string::npos);
        CHECK(run_cli("truehd-encode " + in + "\"" + (dir / "refused.wav").string() + "\"", log) ==
              kExitUsage);
        const std::string out = "\"" + (dir / "refused.thd").string() + "\" ";
        CHECK(run_cli("truehd-encode " + in + out + "truehd-restart=7", log) == kExitUsage);
        CHECK(run_cli("truehd-encode " + in + out + "truehd-downmix=full", log) == kExitUsage);
        CHECK(run_cli("truehd-encode " + in + out + "truehd-downmix-gains=-3,-3,off", log) ==
              kExitUsage);
        CHECK(run_cli("truehd-encode " + in + out + "truehd-fir-order=9", log) == kExitUsage);
        CHECK(run_cli("decode " + in + out + "truehd-restart=8", log) == kExitUsage);
    }
}

// A Blu-ray remux has its TrueHD track first. The commands that read TrueHD take it, as the first
// track of the file; the commands that do not (qc, transcode and the rest) take the AC-3 or E-AC-3
// track after it, where they played it before TrueHD tracks were read, and refuse a file with
// TrueHD alone by name.
TEST_CASE("a command that does not read TrueHD takes the track after a TrueHD one",
          "[forge-cli][truehd]") {
    namespace mt = iclforge::test::multi_track;
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "remux.log";
    const std::vector<std::byte> truehd = read_bytes(fixture("truehd_m51_s24.thd"));
    const std::vector<std::byte> eac3 = read_bytes(
        fs::path{ICLFORGE_GOLDEN_AUDIO_DIR} / "reference_51_eac3_448k_cplbndstrce0.ec3");
    const auto scanned = iclforge::ac3::io::scan(eac3);
    REQUIRE(scanned.has_value());
    const auto truehd_units = mt::split_truehd(truehd);
    REQUIRE(truehd_units.size() > 3);

    const auto write = [&dir](const std::string& name, const std::vector<std::byte>& bytes) {
        const fs::path path = dir / name;
        std::ofstream out{path, std::ios::binary};
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
        return path;
    };
    const std::array<mt::Track, 2> both{
        mt::Track{.codec = mt::Codec::kTruehd, .units = truehd_units},
        mt::Track{.codec = mt::Codec::kEac3, .units = scanned->access_units}};
    const std::array<mt::Track, 1> alone{
        mt::Track{.codec = mt::Codec::kTruehd, .units = truehd_units}};
    const fs::path remux = write("remux.mkv", mt::matroska_file(both));
    const fs::path truehd_only = write("truehd_only.mkv", mt::matroska_file(alone));

    SECTION("probe reads the first track, TrueHD") {
        REQUIRE(run_cli("probe \"" + remux.string() + "\"", log) == 0);
        const std::string text = read_text(log);
        CHECK(text.find("codec           TrueHD") != std::string::npos);
        CHECK(text.find("other tracks    audio track 2 'A_EAC3'") != std::string::npos);
    }

    SECTION("qc measures the E-AC-3 track") {
        REQUIRE(run_cli("qc \"" + remux.string() + "\"", log) == 0);
        const std::string text = read_text(log);
        CHECK(text.find("(E-AC-3,") != std::string::npos);
        CHECK(text.find("does not read") == std::string::npos);
    }

    SECTION("transcode reads the E-AC-3 track") {
        const fs::path out = dir / "transcoded.ac3";
        REQUIRE(run_cli("transcode \"" + remux.string() + "\" \"" + out.string() + "\"", log) == 0);
        const std::vector<std::byte> written = read_bytes(out);
        REQUIRE(written.size() > 2);
        CHECK(written[0] == std::byte{0x0B});
        CHECK(written[1] == std::byte{0x77});
    }

    SECTION("a file with TrueHD alone is refused by name") {
        CHECK(run_cli("qc \"" + truehd_only.string() + "\"", log) == kExitUsage);
        CHECK(read_text(log).find("which 'qc' does not read") != std::string::npos);
        CHECK(run_cli("transcode \"" + truehd_only.string() + "\" \"" +
                          (dir / "refused.ac3").string() + "\"",
                      log) == kExitUsage);
        CHECK(read_text(log).find("which 'transcode' does not read") != std::string::npos);
    }
}

// --- the commands around a stream ---------------------------------------------------------------

namespace {

std::string cli_arg(const fs::path& path) {
    return "\"" + path.string() + "\" ";
}

// The report of a run without its first line, which names the file.
std::string after_first_line(const std::string& text) {
    return text.substr(text.find('\n') + 1);
}

}  // namespace

TEST_CASE("forge probe reads a TrueHD or MLP stream from stdin", "[forge-cli][truehd]") {
    // The TrueHD test opened the file by name, which a pipe has not got, and a pipe skipped it.
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "stdin_probe.log";
    for (const auto& item : fixture_list()) {
        const std::string name = item["name"];
        DYNAMIC_SECTION(name) {
            const std::string input = cli_arg(fixture(item["file"]));
            REQUIRE(run_cli("probe - < " + input, log) == 0);
            const std::string piped = read_text(log);
            REQUIRE(run_cli("probe " + input, log) == 0);
            const std::string named = read_text(log);
            CHECK(after_first_line(piped) == after_first_line(named));
            CHECK(piped.find("codec") != std::string::npos);

            REQUIRE(run_cli("probe - json=1 < " + input, log) == 0);
            const auto document = nlohmann::json::parse(read_text(log));
            CHECK(document["truehd"]["access_units"] == item["access_units"]);
            CHECK(document["file"] == "-");
        }
    }
    SECTION("an AC-3 family stream on stdin still reads as it did") {
        const fs::path ec3 = fixture("../audio/reference_51_eac3_448k_cplbndstrce0.ec3");
        REQUIRE(run_cli("probe - < " + cli_arg(ec3), log) == 0);
        const std::string piped = read_text(log);
        REQUIRE(run_cli("probe " + cli_arg(ec3), log) == 0);
        CHECK(after_first_line(piped) == after_first_line(read_text(log)));
        CHECK(piped.find("E-AC-3") != std::string::npos);
    }
    SECTION("an empty pipe is an error, not a TrueHD stream") {
        const fs::path empty = dir / "empty.bin";
        std::ofstream{empty, std::ios::binary};
        CHECK(run_cli("probe - < " + cli_arg(empty), log) == kExitUsage);
    }
}

namespace {

constexpr std::size_t kToneFrames = 48000;  // a second: loudness needs more than one 400 ms block
// Each channel of the tone is a 1 kHz sine at this share of full scale. 48 samples make a cycle and
// sample 12 is its peak, so every channel's peak, and its RMS (the peak less 3.01 dB), are known
// without measuring them.
constexpr std::array<double, 6> kToneShare{0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625};
constexpr double kPi = 3.14159265358979323846;

std::vector<std::int32_t> tone_samples() {
    std::vector<std::int32_t> samples;
    for (std::size_t n = 0; n < kToneFrames; ++n) {
        for (const double share : kToneShare) {
            samples.push_back(static_cast<std::int32_t>(std::lround(
                share * 8388608.0 * std::sin(2.0 * kPi * static_cast<double>(n % 48) / 48.0))));
        }
    }
    return samples;
}

struct ToneFiles {
    fs::path dir;
    fs::path wav;  // L R C LFE Ls Rs, with the mask `decode` writes
    fs::path thd;  // truehd-encode of it
};

// The tone as a WAV and as a TrueHD stream, made once.
const ToneFiles& tone_files() {
    static const ToneFiles files = [] {
        ToneFiles made;
        made.dir = scratch_dir() / "commands";
        fs::create_directories(made.dir);
        made.wav = made.dir / "tone.wav";
        made.thd = made.dir / "tone.thd";
        write_wav(made.wav, 48000, 6, 0x60F, tone_samples());
        REQUIRE(run_cli("truehd-encode \"" + made.wav.string() + "\" \"" + made.thd.string() +
                            "\" verify",
                        made.dir / "make.log") == 0);
        return made;
    }();
    return files;
}

}  // namespace

TEST_CASE("an option a TrueHD run does not apply is refused by name, never dropped",
          "[forge-cli][truehd][options]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "refused_options.log";
    const fs::path out_wav = tone.dir / "refused_options.wav";
    const std::string in = cli_arg(tone.thd);
    const std::string out = cli_arg(out_wav);

    SECTION("decode") {
        // Each of these was accepted, exit 0, and left the output as it would have been.
        for (const std::string option :
             {"channels=2", "downmix=loro", "drc=0.5", "drcmode=rf", "karaoke", "conceal=mute",
              "mix-lfe", "programme=3", "bed-only", "verify-objects", "gate-objects",
              "dialnorm=27", "syntax-trace=trace.txt", "verify-content", "check-content",
              "heavy"}) {
            DYNAMIC_SECTION(option) {
                fs::remove(out_wav);
                CHECK(run_cli("decode " + in + out + "\"" + option + "\"", log) == kExitUsage);
                const std::string text = read_text(log);
                CHECK(text.find("does not apply") != std::string::npos);
                CHECK(text.find(option) != std::string::npos);
                CHECK(text.find("refused, not dropped") != std::string::npos);
                CHECK_FALSE(fs::exists(out_wav));
            }
        }
        SECTION("a verification flag says that nothing was verified") {
            CHECK(run_cli("decode " + in + out + "verify-content", log) == kExitUsage);
            CHECK(read_text(log).find("Nothing was verified") != std::string::npos);
        }
        SECTION("every option the line carries is named") {
            CHECK(run_cli("decode " + in + out + "drc=0.5 truehd-check=lenient channels=2", log) ==
                  kExitUsage);
            const std::string text = read_text(log);
            CHECK(text.find("drc=0.5, channels=2") != std::string::npos);
            CHECK(text.find("truehd-check=lenient, ") == std::string::npos);
        }
        SECTION("the objects directory and the ADM path are not dropped either") {
            const fs::path objects = tone.dir / "objects_dir";
            CHECK(run_cli("decode " + in + out + cli_arg(objects), log) == kExitUsage);
            CHECK(read_text(log).find("writes no objects") != std::string::npos);
            CHECK_FALSE(fs::exists(out_wav));
        }
        SECTION("the options it does apply, and a transform choice it warns about") {
            CHECK(run_cli("decode " + in + out + "truehd-presentation=2ch truehd-check=strict quiet",
                          log) == 0);
            CHECK(run_cli("decode " + in + out + "verbose", log) == 0);
            REQUIRE(run_cli("decode " + in + out + "fast-imdct", log) == 0);
            const std::string text = read_text(log);
            CHECK(text.find("warning: fast-imdct has no effect on TrueHD or MLP") !=
                  std::string::npos);
        }
    }
    SECTION("truehd-encode") {
        const fs::path stream = tone.dir / "refused_options.thd";
        const fs::path other = tone.dir / "refused_other.wav";
        fs::copy_file(tone.wav, other, fs::copy_options::overwrite_existing);
        const std::vector<std::string> options{
            "src=" + other.string(), "map=0:0", "offset=0:5", "layout=stereo", "dialnorm=27",
            "drc=film-standard", "bsmod=0", "programme2=" + other.string(), "lfemix=off",
            "heavy", "couple", "annexd"};
        for (const std::string& option : options) {
            DYNAMIC_SECTION(option) {
                fs::remove(stream);
                CHECK(run_cli("truehd-encode " + cli_arg(tone.wav) + cli_arg(stream) + "\"" +
                                  option + "\"",
                              log) == kExitUsage);
                const std::string text = read_text(log);
                CHECK(text.find("does not apply") != std::string::npos);
                CHECK(text.find(option) != std::string::npos);
                CHECK_FALSE(fs::exists(stream));
            }
        }
        SECTION("its own options, quiet and a transform choice") {
            REQUIRE(run_cli("truehd-encode " + cli_arg(tone.wav) + cli_arg(stream) +
                                "truehd-restart=8 truehd-matrix=off verify quiet fast-mdct",
                            log) == 0);
            CHECK(read_text(log).find("warning: fast-mdct has no effect") != std::string::npos);
            CHECK(fs::exists(stream));
        }
    }
    SECTION("mkv, mp4, ts and spdif") {
        // mainid=, asvc= and programme= describe a service of the AC-3 family.
        const std::array<std::pair<std::string, std::string>, 4> commands{
            std::pair{"mkv", "refused_options.mkv"}, std::pair{"mp4", "refused_options.mp4"},
            std::pair{"ts", "refused_options.ts"}, std::pair{"spdif", "refused_options_mat.wav"}};
        for (const auto& [command, file] : commands) {
            for (const std::string option : {"mainid=1", "asvc=1", "programme=1", "dialnorm=27"}) {
                DYNAMIC_SECTION(command + " " + option) {
                    const fs::path target = tone.dir / file;
                    fs::remove(target);
                    CHECK(run_cli(command + " " + in + cli_arg(target) + option, log) == kExitUsage);
                    CHECK(read_text(log).find("does not apply " + option) != std::string::npos);
                    CHECK_FALSE(fs::exists(target));
                }
            }
        }
        SECTION("remux names itself") {
            const fs::path mkv = tone.dir / "refused_remux_in.mkv";
            REQUIRE(run_cli("mkv " + in + cli_arg(mkv), log) == 0);
            CHECK(run_cli("remux " + cli_arg(mkv) + cli_arg(tone.dir / "refused_remux.ts") +
                              "mainid=1",
                          log) == kExitUsage);
            CHECK(read_text(log).find("remux does not apply mainid=1") != std::string::npos);
        }
    }
    SECTION("probe") {
        CHECK(run_cli("probe " + in + "detail=frames", log) == kExitUsage);
        CHECK(read_text(log).find("probe does not apply detail=frames") != std::string::npos);
        CHECK(run_cli("probe " + in + "json=1", log) == 0);
    }
}

TEST_CASE("levels and loudness take truehd-presentation= and truehd-check=",
          "[forge-cli][truehd][levels][loudness]") {
    // Both accepted truehd-presentation= for decode alone, and refused it as an unknown option.
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "presentation_options.log";
    const std::string in = cli_arg(tone.thd);

    SECTION("a 5.1 stream's 2ch presentation is a stereo measurement") {
        // The tone is coded with the default downmix=none: substream 0 is its first two channels.
        REQUIRE(run_cli("levels " + in + "truehd-presentation=2ch", log) == 0);
        CHECK(read_text(log).find("TrueHD/MLP, 2 channel(s), 48000 Hz") != std::string::npos);
        REQUIRE(run_cli("loudness " + in + "truehd-presentation=2ch", log) == 0);
        CHECK(read_text(log).find("L R (48000 Hz)") != std::string::npos);
        REQUIRE(run_cli("levels " + in + "truehd-presentation=best", log) == 0);
        CHECK(read_text(log).find("6 channel(s)") != std::string::npos);
    }
    SECTION("truehd-check=lenient is taken, and a bad value is a usage error") {
        CHECK(run_cli("levels " + in + "truehd-check=lenient", log) == 0);
        CHECK(run_cli("loudness " + in + "truehd-check=lenient", log) == 0);
        CHECK(run_cli("levels " + in + "truehd-presentation=7ch", log) == kExitUsage);
        CHECK(run_cli("loudness " + in + "truehd-check=sometimes", log) == kExitUsage);
    }
    SECTION("a lenient levels counts a damaged stream where a strict one stops") {
        const fs::path bad = damaged_copy("truehd_st48_s24.thd", tone.dir);
        CHECK(run_cli("levels " + cli_arg(bad), log) == kExitInput);
        CHECK(run_cli("levels " + cli_arg(bad) + "truehd-check=lenient", log) == 0);
    }
    SECTION("anything else is refused by name") {
        for (const std::string command : {"levels", "loudness"}) {
            CHECK(run_cli(command + " " + in + "programme=1", log) == kExitUsage);
            CHECK(read_text(log).find(command + " does not apply programme=1") !=
                  std::string::npos);
        }
    }
    SECTION("on a stream that is not TrueHD they are warned about and change nothing") {
        REQUIRE(run_cli("levels " + cli_arg(tone.wav) + "truehd-presentation=2ch", log) == 0);
        const std::string text = read_text(log);
        CHECK(text.find("warning:") != std::string::npos);
        CHECK(text.find("is not TrueHD or MLP") != std::string::npos);
    }
}

TEST_CASE("cut, cat, metadata and normalize refuse a TrueHD stream by name, and say why",
          "[forge-cli][truehd][refusals]") {
    // They read it as AC-3 and stopped with an AC-3 frame-sync error, exit 2, that said nothing of
    // the codec.
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "tools.log";
    const fs::path out = tone.dir / "tools_out.thd";
    const std::string in = cli_arg(tone.thd);
    struct Refusal {
        std::string command;
        std::string args;
        std::string why;
    };
    const std::vector<Refusal> refusals{
        {"cut", in + cli_arg(out) + "0.05 0.1", "major sync"},
        {"cat", cli_arg(out) + in + in, "major sync"},
        {"metadata", in + cli_arg(out) + "dialnorm=27", "CRC"},
        {"normalize", in + cli_arg(out), "dialogue norm"},
    };
    for (const Refusal& item : refusals) {
        DYNAMIC_SECTION(item.command) {
            fs::remove(out);
            CHECK(run_cli(item.command + " " + item.args, log) == kExitUsage);
            const std::string text = read_text(log);
            CHECK(text.find("is TrueHD or MLP") != std::string::npos);
            CHECK(text.find("'" + item.command + "' does not read") != std::string::npos);
            CHECK(text.find(item.why) != std::string::npos);
            CHECK(text.find("lost sync") == std::string::npos);
            CHECK_FALSE(fs::exists(out));
        }
    }
    SECTION("a TrueHD stream anywhere in cat's inputs") {
        const fs::path ec3 = fixture("../audio/reference_51_eac3_448k_cplbndstrce0.ec3");
        REQUIRE(fs::exists(ec3));
        CHECK(run_cli("cat " + cli_arg(out) + cli_arg(ec3) + in, log) == kExitUsage);
        CHECK(read_text(log).find("is TrueHD or MLP") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
}

TEST_CASE("a TrueHD transport stream is Blu-ray's, and remux defaults to it",
          "[forge-cli][truehd][containers]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "profile.log";
    const std::vector<std::byte> stream = read_bytes(tone.thd);
    const fs::path mkv = tone.dir / "profile.mkv";
    REQUIRE(run_cli("mkv " + cli_arg(tone.thd) + cli_arg(mkv), log) == 0);

    SECTION("ts takes bluray or no profile") {
        const fs::path plain = tone.dir / "profile_plain.ts";
        const fs::path named = tone.dir / "profile_named.ts";
        REQUIRE(run_cli("ts " + cli_arg(tone.thd) + cli_arg(plain), log) == 0);
        CHECK(read_text(log).find("Blu-ray profile") != std::string::npos);
        REQUIRE(run_cli("ts " + cli_arg(tone.thd) + cli_arg(named) + "bluray", log) == 0);
        CHECK(read_bytes(plain) == read_bytes(named));
    }
    SECTION("remux to a transport stream without a profile writes the Blu-ray one") {
        // remux used to pass dvb when no profile was named, and ts refuses dvb for TrueHD with
        // advice (leave the profile out) that remux could not follow.
        const fs::path out = tone.dir / "profile_remux.ts";
        const fs::path back = tone.dir / "profile_remux.thd";
        REQUIRE(run_cli("remux " + cli_arg(mkv) + cli_arg(out), log) == 0);
        REQUIRE(run_cli("demux " + cli_arg(out) + cli_arg(back), log) == 0);
        CHECK(read_bytes(back) == stream);
        const fs::path named = tone.dir / "profile_remux_named.ts";
        REQUIRE(run_cli("remux " + cli_arg(mkv) + cli_arg(named) + "bluray", log) == 0);
        CHECK(read_bytes(named) == read_bytes(out));
    }
    SECTION("dvb and atsc are refused, naming Blu-ray") {
        for (const std::string profile : {"dvb", "atsc"}) {
            DYNAMIC_SECTION(profile) {
                const fs::path out = tone.dir / ("profile_" + profile + ".ts");
                fs::remove(out);
                CHECK(run_cli("ts " + cli_arg(tone.thd) + cli_arg(out) + profile, log) == kExitUsage);
                CHECK(read_text(log).find("leave the profile out or use bluray") !=
                      std::string::npos);
                CHECK(run_cli("remux " + cli_arg(mkv) + cli_arg(out) + profile, log) == kExitUsage);
                CHECK(read_text(log).find("leave the profile out or use bluray") !=
                      std::string::npos);
                CHECK_FALSE(fs::exists(out));
            }
        }
    }
}

TEST_CASE("an MLP stream in MP4 reports its own rate, not 192 kHz", "[forge-cli][truehd][containers]") {
    // dmlp's word is the major sync's, which for MLP opens with the bit depths: the depth code 2
    // (24 bit) is the code for 192 kHz in TrueHD's layout, and probe and demux said 192000.
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "mlp_rate.log";
    for (const auto& item : fixture_list()) {
        if (item["codec"] != "mlp") {
            continue;
        }
        const std::string name = item["name"];
        const std::string rate = std::to_string(item["sample_rate"].get<int>());
        DYNAMIC_SECTION(name) {
            const fs::path file = dir / (name + "_rate.mp4");
            const fs::path back = dir / (name + "_rate.mlp");
            REQUIRE(run_cli("mp4 " + cli_arg(fixture(item["file"])) + cli_arg(file), log) == 0);
            REQUIRE(run_cli("probe " + cli_arg(file), log) == 0);
            std::string text = read_text(log);
            CHECK(text.find(fmt::format("'mlpa', {} samples, {} Hz",
                                        item["access_units"].get<int>(), rate)) !=
                  std::string::npos);
            CHECK(text.find("sample rate     " + rate + " Hz") != std::string::npos);
            REQUIRE(run_cli("probe " + cli_arg(file) + "json=1", log) == 0);
            const auto document = nlohmann::json::parse(read_text(log));
            CHECK(document["container"]["sample_rate_hz"] == item["sample_rate"]);
            REQUIRE(run_cli("demux " + cli_arg(file) + cli_arg(back), log) == 0);
            text = read_text(log);
            CHECK(text.find("(mlpa, " + rate + " Hz,") != std::string::npos);
            CHECK(read_bytes(back) == read_bytes(fixture(item["file"])));
        }
    }
}

TEST_CASE("forge help names TrueHD and prints the options of the commands that take it",
          "[forge-cli][truehd][help]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "help.log";
    SECTION("truehd-encode prints its options") {
        REQUIRE(run_cli("help truehd-encode", log) == 0);
        const std::string text = read_text(log);
        for (const std::string option :
             {"truehd-downmix=", "truehd-downmix-gains=", "truehd-restart=", "truehd-peak-rate=",
              "truehd-rate-check=", "truehd-fir-order=", "truehd-matrix=", "truehd-stereo-mode=",
              "verify"}) {
            CHECK(text.find(option) != std::string::npos);
        }
    }
    SECTION("decode, levels and loudness print truehd-presentation= and truehd-check=") {
        for (const std::string command : {"decode", "levels", "loudness"}) {
            DYNAMIC_SECTION(command) {
                REQUIRE(run_cli("help " + command, log) == 0);
                const std::string text = read_text(log);
                CHECK(text.find("truehd-presentation=") != std::string::npos);
                CHECK(text.find("truehd-check=") != std::string::npos);
            }
        }
    }
    SECTION("probe's row takes .thd and .mlp") {
        REQUIRE(run_cli("help probe", log) == 0);
        const std::string text = read_text(log);
        CHECK(text.find("in.thd|in.mlp") != std::string::npos);
    }
    SECTION("the banner, the man page and the exit codes name TrueHD") {
        REQUIRE(run_cli("help", log) == 0);
        const std::string banner = read_text(log);
        CHECK(banner.substr(0, banner.find('\n')).find("TrueHD") != std::string::npos);
        REQUIRE(run_cli("man", log) == 0);
        CHECK(read_text(log).find("TrueHD") != std::string::npos);
        REQUIRE(run_cli("help exit-codes", log) == 0);
        CHECK(read_text(log).find("TrueHD") != std::string::npos);
    }
    SECTION("remux lists bluray") {
        REQUIRE(run_cli("help remux", log) == 0);
        CHECK(read_text(log).find("[dvb|atsc|bluray]") != std::string::npos);
    }
}

// --- the containers, MAT, levels and loudness of a TrueHD stream ---------------------------------
// One case a command (or a group of them), over a stream that truehd-encode makes here from a tone,
// so that none of it needs FFmpeg. The containers and MAT are held to FFmpeg's reading in
// tools/ci/run_codec_matrix.sh; what is held here is the command: that it runs, gives the stream
// back, and reports what is in it.

TEST_CASE("mkv, mp4 and ts wrap a TrueHD stream, and demux and remux give it back",
          "[forge-cli][truehd][containers]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "wrap.log";
    const std::vector<std::byte> stream = read_bytes(tone.thd);
    const std::array<std::string, 3> kinds{"mkv", "mp4", "ts"};
    for (const std::string& kind : kinds) {
        DYNAMIC_SECTION(kind) {
            const fs::path file = tone.dir / ("wrap." + kind);
            REQUIRE(run_cli(kind + " " + cli_arg(tone.thd) + cli_arg(file), log) == 0);
            CHECK(read_text(log).find("TrueHD access units") != std::string::npos);

            // truehd-encode writes the same container in one step.
            const fs::path direct = tone.dir / ("direct." + kind);
            REQUIRE(run_cli("truehd-encode " + cli_arg(tone.wav) + cli_arg(direct), log) == 0);
            CHECK(read_bytes(direct) == read_bytes(file));

            const fs::path back = tone.dir / ("back_" + kind + ".thd");
            REQUIRE(run_cli("demux " + cli_arg(file) + cli_arg(back), log) == 0);
            CHECK(read_bytes(back) == stream);

            // ... and every other container, one remux away.
            for (const std::string& other : kinds) {
                if (other == kind) {
                    continue;
                }
                const fs::path moved = tone.dir / ("moved_" + kind + "_to." + other);
                const fs::path unwrapped = tone.dir / ("moved_" + kind + "_to_" + other + ".thd");
                REQUIRE(run_cli("remux " + cli_arg(file) + cli_arg(moved), log) == 0);
                REQUIRE(run_cli("demux " + cli_arg(moved) + cli_arg(unwrapped), log) == 0);
                CHECK(read_bytes(unwrapped) == stream);
            }

            // A decode of the container is the decode of the bare stream.
            const fs::path bare_wav = tone.dir / "bare_decode.wav";
            const fs::path wrapped_wav = tone.dir / ("decode_" + kind + ".wav");
            REQUIRE(run_cli("decode " + cli_arg(tone.thd) + cli_arg(bare_wav), log) == 0);
            REQUIRE(run_cli("decode " + cli_arg(file) + cli_arg(wrapped_wav), log) == 0);
            CHECK(read_bytes(wrapped_wav) == read_bytes(bare_wav));
        }
    }
}

TEST_CASE("spdif wraps a TrueHD stream in MAT bursts and unspdif gives it back",
          "[forge-cli][truehd][containers]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "mat.log";
    const std::vector<std::byte> stream = read_bytes(tone.thd);

    SECTION("an eight-channel PCM16 WAV") {
        const fs::path wav = tone.dir / "mat.wav";
        const fs::path back = tone.dir / "mat_wav.thd";
        REQUIRE(run_cli("spdif " + cli_arg(tone.thd) + cli_arg(wav), log) == 0);
        CHECK(read_text(log).find("MAT burst(s)") != std::string::npos);
        const std::vector<std::byte> bytes = read_bytes(wav);
        REQUIRE(bytes.size() > 44);
        CHECK(le(bytes, 22, 2) == 8);        // channels
        CHECK(le(bytes, 24, 4) == 192000);   // four times the 48 kHz family
        CHECK(le(bytes, 34, 2) == 16);       // bits
        REQUIRE(run_cli("unspdif " + cli_arg(wav) + cli_arg(back), log) == 0);
        CHECK(read_bytes(back) == stream);
    }
    SECTION("raw bursts when the output is not a WAV") {
        const fs::path raw = tone.dir / "mat.raw";
        const fs::path back = tone.dir / "mat_raw.thd";
        REQUIRE(run_cli("spdif " + cli_arg(tone.thd) + cli_arg(raw), log) == 0);
        const std::vector<std::byte> bursts = read_bytes(raw);
        CHECK(!bursts.empty());
        CHECK(bursts.size() % 61440 == 0);  // a MAT burst is 61,440 bytes
        REQUIRE(run_cli("unspdif " + cli_arg(raw) + cli_arg(back), log) == 0);
        CHECK(read_bytes(back) == stream);
    }
}

TEST_CASE("levels reports each channel of a TrueHD stream, bare or in a container",
          "[forge-cli][truehd][levels]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "levels.log";
    REQUIRE(run_cli("levels " + cli_arg(tone.thd), log) == 0);
    const std::string report = read_text(log);
    CHECK(report.find("TrueHD/MLP, 6 channel(s), 48000 Hz, 1.00 s") != std::string::npos);
    // The tone's channels are at 1/2, 1/4, ... of full scale: -6.02, -12.04, ... dBFS peak and
    // 3.01 dB less RMS, under the names the stream gives them.
    const std::array<std::string, 6> names{"L", "R", "C", "LFE", "Ls", "Rs"};
    for (std::size_t c = 0; c < names.size(); ++c) {
        const double peak = 20.0 * std::log10(kToneShare[c]);
        const std::string line = fmt::format("  {:<6} {:>10.2f} {:>10.2f}", names[c], peak,
                                             peak - 3.0103);
        CHECK(report.find(line) != std::string::npos);
    }
    for (const std::string kind : {"mkv", "mp4", "ts"}) {
        DYNAMIC_SECTION(kind) {
            const fs::path file = tone.dir / ("levels." + kind);
            REQUIRE(run_cli(kind + " " + cli_arg(tone.thd) + cli_arg(file), log) == 0);
            REQUIRE(run_cli("levels " + cli_arg(file), log) == 0);
            CHECK(after_first_line(read_text(log)) == after_first_line(report));
        }
    }
}

TEST_CASE("loudness measures a TrueHD stream as the meter measures its samples",
          "[forge-cli][truehd][loudness]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "loudness.log";

    // What the library's meter says of the tone itself, with the speakers the stream names.
    using iclforge::base::Speaker;
    const std::array<Speaker, 6> speakers{Speaker::kLeft,         Speaker::kRight,
                                          Speaker::kCentre,       Speaker::kLfe,
                                          Speaker::kLeftSurround, Speaker::kRightSurround};
    iclforge::base::LoudnessMeter meter(48000, speakers);
    const std::vector<std::int32_t> samples = tone_samples();
    constexpr std::size_t kBlock = 480;
    for (std::size_t at = 0; at < kToneFrames; at += kBlock) {
        std::vector<std::vector<float>> planes(6, std::vector<float>(kBlock));
        for (std::size_t n = 0; n < kBlock; ++n) {
            for (std::size_t c = 0; c < 6; ++c) {
                planes[c][n] = static_cast<float>(samples[((at + n) * 6) + c]) / 8388608.0F;
            }
        }
        const std::array<std::span<const float>, 6> views{planes[0], planes[1], planes[2],
                                                          planes[3], planes[4], planes[5]};
        meter.push(views);
    }
    const auto expected_lkfs = meter.integrated_lkfs();
    const auto expected_peak = meter.true_peak_dbtp();
    REQUIRE(expected_lkfs.has_value());
    REQUIRE(expected_peak.has_value());

    REQUIRE(run_cli("loudness " + cli_arg(tone.thd), log) == 0);
    const std::string report = read_text(log);
    CHECK(report.find("L R C LFE Ls Rs (48000 Hz)") != std::string::npos);
    CHECK(report.find(fmt::format("integrated loudness {:.1f} LKFS", *expected_lkfs)) !=
          std::string::npos);
    CHECK(report.find(fmt::format("true peak {:.1f} dBTP", *expected_peak)) != std::string::npos);

    for (const std::string kind : {"mkv", "mp4", "ts"}) {
        DYNAMIC_SECTION(kind) {
            const fs::path file = tone.dir / ("loudness." + kind);
            REQUIRE(run_cli(kind + " " + cli_arg(tone.thd) + cli_arg(file), log) == 0);
            REQUIRE(run_cli("loudness " + cli_arg(file), log) == 0);
            CHECK(after_first_line(read_text(log)) == after_first_line(report));
        }
    }
}

TEST_CASE("qc, transcode and fmp4 refuse a TrueHD stream by name", "[forge-cli][truehd][refusals]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "refusals.log";
    const fs::path out = tone.dir / "refused_out";
    struct Refusal {
        std::string command;
        std::string args;
    };
    const std::vector<Refusal> refusals{
        {"qc", cli_arg(tone.thd)},
        {"transcode", cli_arg(tone.thd) + cli_arg(out.string() + ".ac3")},
        {"fmp4", cli_arg(tone.thd) + cli_arg(out)},
    };
    for (const Refusal& item : refusals) {
        DYNAMIC_SECTION(item.command) {
            CHECK(run_cli(item.command + " " + item.args, log) == kExitUsage);
            const std::string text = read_text(log);
            CHECK(text.find("is TrueHD or MLP") != std::string::npos);
            CHECK(text.find(item.command == "fmp4" ? "CMAF, HLS or DASH"
                                                   : "'" + item.command + "' does not read") !=
                  std::string::npos);
            CHECK(text.find("lost sync") == std::string::npos);
        }
    }
}

// --- a write that fails ------------------------------------------------------------------------
// The streams are buffered, and a TrueHD file this small is still in the buffer when the last write
// call returns: the kernel sees it only when the stream is flushed or closed. A command that checks
// the write calls and not the flush reports success for a file that was never written.

TEST_CASE("truehd-encode reports an output that could not be written", "[forge-cli][truehd][truehd-encode]") {
    std::error_code ec;
    if (!fs::is_character_file("/dev/full", ec)) {
        SKIP("no /dev/full character device here");
    }
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "full.log";
    const fs::path source = dir / "full_source.wav";
    std::vector<std::int32_t> samples;
    for (std::int32_t i = 0; i < 2 * 2400; ++i) {
        samples.push_back((i * 37) % 64 - 32);
    }
    write_wav(source, 48000, 2, 0x3, samples);

    // The same encode to a file of its own: small enough for one buffer, and a good one.
    const fs::path good = dir / "full_good.thd";
    REQUIRE(run_cli("truehd-encode \"" + source.string() + "\" \"" + good.string() + "\"", log) == 0);
    REQUIRE(fs::file_size(good) < 4096);

    // A link of this test's own to /dev/full for each name the encode writes: the open succeeds and
    // every write the kernel sees fails with ENOSPC.
    for (const std::string container : {"thd", "mkv", "mp4", "ts"}) {
        CAPTURE(container);
        const fs::path link = dir / ("full_device." + container);
        fs::remove(link, ec);
        fs::create_symlink("/dev/full", link, ec);
        if (ec) {
            SKIP("cannot create a symlink here");
        }
        CHECK(run_cli("truehd-encode \"" + source.string() + "\" \"" + link.string() + "\"", log) !=
              0);
        CHECK(read_text(log).find("write failed") != std::string::npos);
        fs::remove(link, ec);
    }

    // The container commands, from the stream that was just written.
    for (const std::string command : {"mkv", "mp4", "ts"}) {
        CAPTURE(command);
        const fs::path link = dir / ("full_convert." + command);
        fs::remove(link, ec);
        fs::create_symlink("/dev/full", link, ec);
        if (ec) {
            SKIP("cannot create a symlink here");
        }
        CHECK(run_cli(command + " \"" + good.string() + "\" \"" + link.string() + "\"", log) != 0);
        CHECK(read_text(log).find("write failed") != std::string::npos);
        fs::remove(link, ec);
    }
}
