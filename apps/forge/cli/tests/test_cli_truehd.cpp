#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/thrift_binary.hpp"
#include "iclforge/base/loudness.hpp"
#include "iclforge/base/speaker.hpp"
#include "iclforge/objects/emdf.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
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

// The log of a run, with each line end as LF: a run that writes stdout text on Windows writes CR LF,
// and one that reads stdin writes LF (platform/windows/stdio_binary.cpp).
std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::string raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::string text;
    text.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') {
            continue;
        }
        text.push_back(raw[i]);
    }
    return text;
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

TEST_CASE("bytes ahead of the first access unit are a warning, not an error",
          "[forge-cli][truehd]") {
    // A packet some muxers put in front of the stream (a timecode, say): forge skips it to the
    // first access unit with a major sync that reads and is followed by one that reads, says how
    // many bytes it passed, and the PCM is the stream's.
    const fs::path dir = scratch_dir();
    const std::vector<std::byte> stream = read_bytes(fixture("truehd_st48_s24.thd"));
    const fs::path clean_wav = dir / "junk_clean.wav";
    REQUIRE(run_cli("decode \"" + fixture("truehd_st48_s24.thd").string() + "\" \"" +
                        clean_wav.string() + "\"",
                    dir / "junk_clean.log") == 0);
    const Wav clean = read_wav(clean_wav);

    for (const std::size_t junk : {1U, 28U, 4096U}) {
        for (const unsigned fill : {0x00U, 0x5AU, 0xF8U}) {
            CAPTURE(junk, fill);
            std::vector<std::byte> bytes(junk, static_cast<std::byte>(fill));
            bytes.insert(bytes.end(), stream.begin(), stream.end());
            const fs::path path = dir / ("junk_" + std::to_string(junk) + ".thd");
            {
                std::ofstream out{path, std::ios::binary};
                out.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
            }
            const fs::path wav_path = dir / ("junk_" + std::to_string(junk) + ".wav");
            const fs::path log = dir / ("junk_" + std::to_string(junk) + ".log");
            REQUIRE(run_cli("decode \"" + path.string() + "\" \"" + wav_path.string() + "\"",
                            log) == 0);
            const std::string text = read_text(log);
            CHECK(text.find("warning: ") != std::string::npos);
            CHECK(text.find(std::to_string(junk) +
                            " byte(s) ahead of the first access unit are not part of the stream") !=
                  std::string::npos);
            CHECK(text.find("error") == std::string::npos);
            CHECK(read_wav(wav_path).s32le == clean.s32le);

            const fs::path probe_log = dir / ("junk_probe_" + std::to_string(junk) + ".json");
            REQUIRE(run_cli("probe \"" + path.string() + "\" json=1", probe_log) == 0);
            // The warning is on stderr, in the same file: the JSON follows it.
            const std::string probe_text = read_text(probe_log);
            const auto document = nlohmann::json::parse(probe_text.substr(probe_text.find('{')));
            CHECK(document["truehd"]["leading_bytes"] == junk);
            CHECK(document["truehd"]["skipped_bytes"] == 0);
            CHECK(document["truehd"]["access_units"] == 300);
        }
    }

    // Bytes that only look like a stream are still not one.
    std::vector<std::byte> noise(5000, std::byte{0x5A});
    const fs::path noise_path = dir / "junk_noise.bin";
    {
        std::ofstream out{noise_path, std::ios::binary};
        out.write(reinterpret_cast<const char*>(noise.data()),
                  static_cast<std::streamsize>(noise.size()));
    }
    CHECK(run_cli("decode \"" + noise_path.string() + "\" \"" + (dir / "noise.wav").string() + "\"",
                  dir / "noise.log") != 0);
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
    // TH spec §4.4 (v3.9): the peak data rate is (raw * rate + 8) >> 4 bits per second.
    CHECK(text.find("peak data rate 9597000 bit/s (field 3199)") != std::string::npos);

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
    CHECK(stream["peak_data_rate"] == 1599);
    CHECK(stream["peak_data_rate_bps"] == 9594000);
    CHECK(stream["segment_failures"] == 0);
    CHECK(stream["skipped_bytes"] == 0);
}

// --- the evolution frame in probe --------------------------------------------------------------
// PROVISIONAL, owner to confirm (libs/truehd/ERRATA.md, "Atmos, spec v3.6", question 22): no real
// stream carries an evolution frame, so the stream here is the encoder's, with the extra data
// framing it writes around the E-AC-3 container's body holding an OAMD payload from
// iclforge::objects.

namespace {

// The access units that carry extra data in the streams below: six of twelve, at gaps of 2, 1, 3, 2
// and 2 units.
bool carries_extra_data(std::uint64_t index) {
    return index == 0 || index == 2 || index == 3 || index == 6 || index == 8 || index == 10;
}

// Writes the stream a payload source makes: twelve access units of stereo whose extra data is
// `payload(index)` in the units that `carries_extra_data` names.
template <typename Payload>
fs::path write_stream_with(const fs::path& dir, const std::string& name, Payload&& payload) {
    iclforge::truehd::EncoderConfig config;
    config.restart_interval_units = 8;
    config.extra_data = [&payload](std::uint64_t index) {
        return carries_extra_data(index) ? payload(index) : std::vector<std::byte>{};
    };
    auto encoder = iclforge::truehd::Encoder::create(config);
    REQUIRE(encoder.has_value());
    std::vector<std::int32_t> pcm(40 * 12 * 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int32_t>((i * 7919U) % 20000U) - 10000;
    }
    std::vector<std::byte> stream;
    auto units = encoder->encode(pcm);
    REQUIRE(units.has_value());
    auto tail = encoder->flush();
    REQUIRE(tail.has_value());
    for (const auto* batch : {&*units, &*tail}) {
        for (const auto& unit : *batch) {
            stream.insert(stream.end(), unit.bytes.begin(), unit.bytes.end());
        }
    }
    const fs::path path = dir / name;
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(stream.data()),
              static_cast<std::streamsize>(stream.size()));
    return path;
}

// Six access units carrying an OAMD payload of three dynamic objects in the EMDF container's body.
fs::path write_atmos_like_stream(const fs::path& dir) {
    namespace oba = iclforge::objects::oba;
    namespace emdf = iclforge::objects::emdf;
    oba::Program program;
    program.dynamic_only = true;
    program.lfe = true;
    program.dynamic_objects = 3;
    const std::vector<oba::DynamicObject> objects(3);
    const std::vector<std::byte> oamd = oba::build_payload(program, objects);
    const std::array<emdf::Payload, 1> payloads{{{.id = emdf::kPayloadIdOamd, .bytes = oamd}}};
    const std::vector<std::byte> container = emdf::build_container(payloads, 0);
    const std::vector<std::byte> body(container.begin() + 4, container.end());

    return write_stream_with(dir, "atmos_like.thd", [&body](std::uint64_t) {
        return std::vector<std::byte>(body.begin(), body.end());
    });
}

}  // namespace

TEST_CASE("forge probe reports the evolution frames and the object metadata in them",
          "[forge-cli][truehd][evolution]") {
    const fs::path dir = scratch_dir();
    const fs::path stream = write_atmos_like_stream(dir);
    const fs::path log = dir / "probe_evolution.log";

    SECTION("text") {
        REQUIRE(run_cli("probe \"" + stream.string() + "\"", log) == 0);
        const std::string text = read_text(log);
        CHECK(
            text.find("evolution       6 unit(s) carry extra data, 6 read as an evolution frame") !=
            std::string::npos);
        CHECK(text.find("frames at units 0 to 10, 1 to 3 unit(s) apart") != std::string::npos);
        CHECK(text.find("check nibble 6/6, framing 6/6, parity 6/6") != std::string::npos);
        CHECK(text.find("read as the EMDF container: 6/6 (6 clean)") != std::string::npos);
        CHECK(text.find("payload ids: 11 (x6)") != std::string::npos);
        CHECK(text.find("4 object(s), dynamic-only program with LFE") != std::string::npos);
        CHECK(text.find("3 dynamic") != std::string::npos);
        CHECK(text.find("not verified: the protection bytes and any signature") !=
              std::string::npos);
    }
    SECTION("json") {
        REQUIRE(run_cli("probe \"" + stream.string() + "\" json=1", log) == 0);
        const auto document = nlohmann::json::parse(read_text(log));
        const auto& evolution = document["truehd"]["evolution"];
        CHECK(evolution["provisional"] == true);
        CHECK(evolution["regions"] == 6);
        CHECK(evolution["check_nibble_ok"] == 6);
        CHECK(evolution["framing_ok"] == 6);
        CHECK(evolution["parity_ok"] == 6);
        CHECK(evolution["frames_read"] == 6);
        CHECK(evolution["readings"]["emdf"]["frames"] == 6);
        CHECK(evolution["readings"]["emdf"]["clean_frames"] == 6);
        CHECK(evolution["payload_ids"].size() == 1);
        CHECK(evolution["payload_ids"][0]["id"] == 11);
        CHECK(evolution["payload_ids"][0]["frames"] == 6);
        CHECK(evolution["protection"]["verified"] == false);
        CHECK(evolution["signature_verified"] == false);
        CHECK(evolution["oamd"]["payloads"] == 6);
        CHECK(evolution["oamd"]["unread"] == 0);
        REQUIRE(evolution["oamd"]["programs"].size() == 1);
        const auto& program = evolution["oamd"]["programs"][0];
        CHECK(program["frames"] == 6);
        CHECK(program["object_count"] == 4);
        CHECK(program["dynamic_only"] == true);
        CHECK(program["dynamic_objects"] == 3);
        CHECK(program["bed_channels"][0] == "LFE");
        CHECK(evolution["cadence"]["first_unit"] == 0);
        CHECK(evolution["cadence"]["last_unit"] == 10);
        CHECK(evolution["cadence"]["min_gap_units"] == 1);
        CHECK(evolution["cadence"]["max_gap_units"] == 3);
    }
}

TEST_CASE("forge probe counts a wrong check nibble and a wrong parity byte",
          "[forge-cli][truehd][evolution]") {
    const fs::path dir = scratch_dir();
    const fs::path stream = write_atmos_like_stream(dir);
    std::vector<std::byte> bytes = read_bytes(stream);
    // Unit 0's extra data starts with the check nibble; unit 3's ends the unit with the parity
    // byte.
    iclforge::truehd::ElementaryReader reader(bytes);
    int index = 0;
    while (const auto unit = reader.next()) {
        const auto parsed = iclforge::truehd::parse_access_unit(*unit, 1, true);
        REQUIRE(parsed.has_value());
        if (index == 0) {
            REQUIRE(parsed->extra_data.has_value());
            bytes[static_cast<std::size_t>(parsed->extra_data->bytes.data() - bytes.data())] ^=
                std::byte{0x10};
        }
        if (index == 3) {
            REQUIRE(parsed->extra_data.has_value());
            bytes[static_cast<std::size_t>(parsed->extra_data->bytes.data() - bytes.data()) +
                  parsed->extra_data->bytes.size() - 1] ^= std::byte{0x5A};
        }
        ++index;
    }
    const fs::path damaged = dir / "atmos_like_damaged.thd";
    {
        std::ofstream out{damaged, std::ios::binary};
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    const fs::path log = dir / "probe_damaged_evolution.log";
    REQUIRE(run_cli("probe \"" + damaged.string() + "\" json=1", log) == 0);
    const auto document = nlohmann::json::parse(read_text(log));
    const auto& evolution = document["truehd"]["evolution"];
    CHECK(evolution["regions"] == 6);
    CHECK(evolution["check_nibble_ok"] == 5);
    CHECK(evolution["parity_ok"] == 5);
    CHECK(evolution["framing_ok"] == 6);
    // The checks are reported, not enforced: every frame still reads and the exit status is 0.
    CHECK(evolution["frames_read"] == 6);
}

TEST_CASE("forge probe tells the two readings of the container apart",
          "[forge-cli][truehd][evolution]") {
    // A frame written in the coding of the letter of TH spec v3.6 section 15.4
    // (tools/references/truehd_evolution.py): the EMDF coding does not read it cleanly, the other
    // does.
    const fs::path dir = scratch_dir();
    std::ifstream vectors_file{fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / "evolution" /
                               "vectors.json"};
    REQUIRE(vectors_file.good());
    const auto vectors = nlohmann::json::parse(vectors_file);
    std::vector<std::byte> payload;
    for (const auto& vector : vectors["vectors"]) {
        if (vector["name"] == "spec36_one_oamd_payload") {
            const std::string text = vector["extra_data"];
            std::vector<std::byte> region;
            for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
                region.push_back(
                    static_cast<std::byte>(std::stoul(text.substr(i, 2), nullptr, 16)));
            }
            const auto extra = iclforge::truehd::read_extra_data(region);
            REQUIRE(extra.has_value());
            REQUIRE(extra->framing_ok);
            payload.assign(extra->evolution_payload.begin(), extra->evolution_payload.end());
        }
    }
    REQUIRE_FALSE(payload.empty());
    const fs::path stream =
        write_stream_with(dir, "letter_of_15_4.thd", [&payload](std::uint64_t) { return payload; });
    const fs::path log = dir / "probe_letter.log";
    REQUIRE(run_cli("probe \"" + stream.string() + "\" json=1", log) == 0);
    const auto document = nlohmann::json::parse(read_text(log));
    const auto& evolution = document["truehd"]["evolution"];
    CHECK(evolution["regions"] == 6);
    CHECK(evolution["readings"]["spec36"]["clean_frames"] == 6);
    CHECK(evolution["readings"]["emdf"]["clean_frames"] == 0);
    REQUIRE(evolution["payload_ids"].size() == 1);
    CHECK(evolution["payload_ids"][0]["id"] == 11);
    CHECK(evolution["payload_ids"][0]["frames"] == 6);
    // 23 bytes that are not object audio metadata: counted, not read.
    CHECK(evolution["oamd"]["payloads"] == 6);
    CHECK(evolution["oamd"]["unread"] == 6);
}

TEST_CASE("forge probe says so when there is no extra data and when it is not a frame",
          "[forge-cli][truehd][evolution]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "probe_no_evolution.log";
    REQUIRE(run_cli("probe \"" + fixture("truehd_m51_s24.thd").string() + "\" json=1", log) == 0);
    const auto none = nlohmann::json::parse(read_text(log));
    CHECK(none["truehd"]["evolution"]["regions"] == 0);
    CHECK(none["truehd"]["evolution"]["provisional"] == true);
    REQUIRE(run_cli("probe \"" + fixture("truehd_m51_s24.thd").string() + "\"", log) == 0);
    CHECK(read_text(log).find("evolution") == std::string::npos);

    // Opaque bytes in the extra data (the hand-built hb_extra_data): framed and checked, no frame.
    const fs::path opaque =
        fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / "handbuilt" / "hb_extra_data.thd";
    REQUIRE(run_cli("probe \"" + opaque.string() + "\" json=1", log) == 0);
    const auto document = nlohmann::json::parse(read_text(log));
    const auto& evolution = document["truehd"]["evolution"];
    CHECK(evolution["regions"] == 20);
    CHECK(evolution["check_nibble_ok"] == 20);
    CHECK(evolution["parity_ok"] == 20);
    CHECK(evolution["readings"]["emdf"]["clean_frames"] == 0);
    CHECK(evolution["oamd"]["payloads"] == 0);
    CHECK(evolution["signature_verified"] == false);
}

// --- detail=evolution, detail=structure, detail=format -----------------------------------------
// The owner's check on a disc's stream (libs/truehd/ERRATA.md, "Atmos, spec v3.6"): what the
// evolution frames are made of, shape by shape, with any Thrift payload walked by field id, type
// and length.

namespace {

// A Thrift Binary struct shaped like a signature envelope, with values that must never be printed:
// field 1 a struct { 1: list<byte> (32), 2: string, 3: i32, 4: list<struct> (2) }, field 2 a struct
// { 1: map<string, list<byte>> (1) }.
constexpr std::string_view kSecretString = "do-not-print-this-string";

std::vector<std::byte> secret_thrift() {
    namespace thrift = iclforge::base::crypto::thrift;
    thrift::Writer w;
    w.field_struct(1);
    const std::vector<std::byte> digest(32, std::byte{0xC3});
    w.field_byte_list(1, digest);
    w.field_string(2, kSecretString);
    w.field_i32(3, 0x5EC12E7);
    w.field(thrift::Type::kList, 4);
    w.list_header(thrift::Type::kStruct, 2);
    for (int i = 0; i < 2; ++i) {
        w.field_i32(1, i);
        w.stop();
    }
    w.stop();
    w.field_struct(2);
    w.field(thrift::Type::kMap, 1);
    w.map_header(thrift::Type::kString, thrift::Type::kList, 1);
    w.string("r");
    w.byte_list(digest);
    w.stop();
    w.stop();
    return w.take();
}

// Six access units: the first carries an OAMD payload (id 11) and a second payload (id 15) that is
// the Thrift struct above; the others carry the OAMD payload alone.
fs::path write_signed_like_stream(const fs::path& dir) {
    namespace oba = iclforge::objects::oba;
    namespace emdf = iclforge::objects::emdf;
    oba::Program program;
    program.dynamic_only = true;
    program.lfe = true;
    program.dynamic_objects = 3;
    const std::vector<oba::DynamicObject> objects(3);
    const std::vector<std::byte> oamd = oba::build_payload(program, objects);
    const std::vector<std::byte> thrift = secret_thrift();
    const std::array<emdf::Payload, 2> both{
        {{.id = emdf::kPayloadIdOamd, .bytes = oamd}, {.id = 15, .bytes = thrift}}};
    const std::array<emdf::Payload, 1> alone{{{.id = emdf::kPayloadIdOamd, .bytes = oamd}}};
    const std::vector<std::byte> first = emdf::build_container(both, 0);
    const std::vector<std::byte> rest = emdf::build_container(alone, 0);
    // Every other frame after the first carries one object more, so a shape's payloads differ in
    // size.
    oba::Program bigger = program;
    bigger.dynamic_objects = 4;
    const std::vector<oba::DynamicObject> more_objects(4);
    const std::vector<std::byte> larger_oamd = oba::build_payload(bigger, more_objects);
    const std::array<emdf::Payload, 1> alone_larger{
        {{.id = emdf::kPayloadIdOamd, .bytes = larger_oamd}}};
    const std::vector<std::byte> rest_larger = emdf::build_container(alone_larger, 0);
    return write_stream_with(dir, "signed_like.thd", [&](std::uint64_t index) {
        const auto& container = index == 0 ? first : (index % 3 == 0 ? rest_larger : rest);
        return std::vector<std::byte>(container.begin() + 4, container.end());
    });
}

}  // namespace

TEST_CASE("forge probe detail=evolution walks the evolution frames and the Thrift payload",
          "[forge-cli][truehd][evolution]") {
    const fs::path dir = scratch_dir();
    const fs::path stream = write_signed_like_stream(dir);
    const fs::path log = dir / "probe_detail_evolution.log";

    SECTION("json") {
        REQUIRE(run_cli("probe \"" + stream.string() + "\" detail=evolution json=1", log) == 0);
        const std::string text = read_text(log);
        // Structure only: not one value of the Thrift struct, the digest or the string is printed.
        CHECK(text.find("do-not-print") == std::string::npos);
        CHECK(text.find("c3c3c3") == std::string::npos);
        CHECK(text.find("5ec12e7") == std::string::npos);
        const auto document = nlohmann::json::parse(text);
        const auto& detail = document["truehd"]["evolution"]["detail"];
        CHECK(detail["frames_total"] == 6);
        REQUIRE(detail["frames"].size() == 6);
        const auto& first = detail["frames"][0];
        CHECK(first["unit"] == 0);
        CHECK(first["check_nibble_ok"] == true);
        CHECK(first["parity_ok"] == true);
        CHECK(first["reading"] == "emdf");
        CHECK(first["version"] == 0);
        CHECK(first["key_id"] == 0);
        REQUIRE(first["payloads"].size() == 2);
        CHECK(first["payloads"][0]["id"] == 11);
        CHECK(first["payloads"][1]["id"] == 15);
        CHECK(first["payloads"][1]["config"]["frame_aligned"] == true);
        CHECK(first["payloads"][1]["config"]["group_id"] == 0);
        CHECK(first["primary_protection_code"] == 2);
        CHECK(first["secondary_protection_code"] == 1);
        CHECK(detail["frames"][1]["payloads"].size() == 1);

        // Two shapes: the first frame (a signature once), and the other five (OAMD alone).
        REQUIRE(detail["shapes"].size() == 2);
        const auto& once = detail["shapes"][0];
        CHECK(once["frames"] == 1);
        CHECK(once["payload_ids"] == nlohmann::json::array({11, 15}));
        const auto& alone = detail["shapes"][1];
        CHECK(alone["frames"] == 5);
        CHECK(alone["payload_ids"] == nlohmann::json::array({11}));
        CHECK(alone["thrift_walks"].empty());
        CHECK(alone["payload_bytes"][0]["min"].get<int>() <
              alone["payload_bytes"][0]["max"].get<int>());

        REQUIRE(once["thrift_walks"].size() == 1);
        const auto& walk = once["thrift_walks"][0];
        CHECK(walk["payload_id"] == 15);
        CHECK(walk["walk"]["found"] == true);
        CHECK(walk["walk"]["offset"] == 0);
        CHECK(walk["walk"]["consumed"] == walk["walk"]["payload_bytes"]);
        const auto& fields = walk["walk"]["fields"];
        REQUIRE(fields.size() == 2);
        CHECK(fields[0]["id"] == 1);
        CHECK(fields[0]["type"] == "struct");
        const auto& inner = fields[0]["fields"];
        REQUIRE(inner.size() == 4);
        CHECK(inner[0]["type"] == "list");
        CHECK(inner[0]["element"] == "byte");
        CHECK(inner[0]["count"] == 32);
        CHECK(inner[1]["type"] == "string");
        CHECK(inner[1]["count"] == kSecretString.size());
        CHECK(inner[2]["type"] == "i32");
        CHECK(inner[2].contains("count") == false);
        CHECK(inner[3]["element"] == "struct");
        CHECK(inner[3]["count"] == 2);
        CHECK(fields[1]["fields"][0]["type"] == "map");
        CHECK(fields[1]["fields"][0]["key"] == "string");
        CHECK(fields[1]["fields"][0]["count"] == 1);
        // detail=evolution adds no structure section.
        CHECK_FALSE(document["truehd"].contains("structure"));
    }
    SECTION("text") {
        REQUIRE(run_cli("probe \"" + stream.string() + "\" detail=evolution", log) == 0);
        const std::string text = read_text(log);
        CHECK(text.find("frames in detail (the first 6 of 6)") != std::string::npos);
        CHECK(text.find("shape 1: 1 frame(s), units 0 to 0, emdf, payload ids [11,15]") !=
              std::string::npos);
        CHECK(text.find("shape 2: 5 frame(s)") != std::string::npos);
        CHECK(text.find("a Thrift struct reads at offset 0") != std::string::npos);
        CHECK(text.find("field 4 list of struct x2") != std::string::npos);
        CHECK(text.find("do-not-print") == std::string::npos);
        CHECK(text.find("shape 1") != std::string::npos);
        CHECK(text.find("stream syntax") == std::string::npos);
    }
}

TEST_CASE("forge probe detail=structure reports the headers, the decode check and the tails",
          "[forge-cli][truehd][evolution]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "probe_detail_structure.log";
    REQUIRE(
        run_cli("probe \"" + fixture("truehd_m51_s24.thd").string() + "\" detail=structure json=1",
                log) == 0);
    const auto document = nlohmann::json::parse(read_text(log));
    const auto& structure = document["truehd"]["structure"];
    REQUIRE(structure["major_syncs"].size() >= 1);
    CHECK(structure["major_syncs"][0]["size_bytes"] == 28);
    CHECK(structure["major_syncs"][0]["substreams"] == 2);
    CHECK(structure["major_syncs"][0]["extra_channel_meaning"] == false);
    REQUIRE(structure["restart_headers"].size() == 2);
    CHECK(structure["restart_headers"][0]["substream"] == 0);
    CHECK(structure["restart_headers"][0]["syncs"][0]["sync"] == "0x31EA");
    CHECK(structure["restart_headers"][0]["tail_16_bits"][0]["value"] == "0x0000");
    CHECK(structure["restart_headers"][0]["failures"].empty());
    const auto& decode = structure["decode"];
    CHECK(decode["units"] == 300);
    CHECK(decode["lossless_check_failures"] == 0);
    CHECK(decode["lossless_checks_ok"].get<int>() >= 1);
    CHECK(decode["errors"].empty());
    CHECK(decode["matrices"]["matrices_with_lsb_bypass"] == 0);
    REQUIRE(decode["segment_tails"].size() == 2);
    CHECK(decode["segment_tails"][0]["segments"] == 300);
    CHECK(decode["segment_tails"][0]["terminator_b_at_once"] == 0);
    CHECK_FALSE(document["truehd"]["evolution"].contains("detail"));

    // The hand-built 16-channel stream: terminatorB after substream 3, restart sync 0x31EC.
    const fs::path sixteen =
        fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / "handbuilt" / "hb_sixteen_channels.thd";
    REQUIRE(run_cli("probe \"" + sixteen.string() + "\" detail=structure json=1", log) == 0);
    const auto wide = nlohmann::json::parse(read_text(log))["truehd"]["structure"];
    REQUIRE(wide["restart_headers"].size() == 4);
    CHECK(wide["restart_headers"][3]["syncs"][0]["sync"] == "0x31EC");
    const auto& tails = wide["decode"]["segment_tails"];
    REQUIRE(tails.size() == 4);
    CHECK(tails[3]["terminator_b_at_once"] == tails[3]["segments"]);
    CHECK(tails[0]["terminator_b_at_once"] == 0);

    // The hand-built stream with LSB bypass, and the text form.
    const fs::path bypass =
        fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / "handbuilt" / "hb_lsb_bypass.thd";
    REQUIRE(run_cli("probe \"" + bypass.string() + "\" detail=structure", log) == 0);
    const std::string text = read_text(log);
    CHECK(text.find("stream syntax") != std::string::npos);
    CHECK(text.find("matrices with lsb_bypass: ") != std::string::npos);
    CHECK(text.find("matrices with lsb_bypass: 0") == std::string::npos);
    CHECK(text.find("lossless check ") != std::string::npos);
    CHECK(text.find("terminatorB at once 0") != std::string::npos);
}

TEST_CASE("forge probe detail=format is both, and the TrueHD details are refused for other streams",
          "[forge-cli][truehd][evolution]") {
    const fs::path dir = scratch_dir();
    const fs::path stream = write_signed_like_stream(dir);
    const fs::path log = dir / "probe_detail_format.log";
    REQUIRE(run_cli("probe \"" + stream.string() + "\" detail=format json=1", log) == 0);
    const auto document = nlohmann::json::parse(read_text(log));
    CHECK(document["truehd"]["evolution"].contains("detail"));
    CHECK(document["truehd"].contains("structure"));
    REQUIRE(run_cli("probe \"" + stream.string() + "\" detail=format", log) == 0);
    const std::string text = read_text(log);
    CHECK(text.find("frames in detail") != std::string::npos);
    CHECK(text.find("stream syntax") != std::string::npos);

    // Not a TrueHD stream: refused by name, exit 1. And a value probe does not know.
    const fs::path ec3 =
        fs::path{ICLFORGE_GOLDEN_AUDIO_DIR} / "reference_51_eac3_448k_cplbndstrce0.ec3";
    CHECK(run_cli("probe \"" + ec3.string() + "\" detail=evolution", log) == 1);
    CHECK(read_text(log).find("describes TrueHD and MLP streams") != std::string::npos);
    CHECK(run_cli("probe \"" + stream.string() + "\" detail=everything", log) == 1);
    CHECK(read_text(log).find("detail must be frames, blocks, evolution, structure or format") !=
          std::string::npos);
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

namespace {

// Whether any object of the document names a key twice. nlohmann::json keeps the last of two and
// says nothing, so the parser's callback watches the keys of each object as they arrive.
bool repeats_a_key(const std::string& text) {
    std::vector<std::set<std::string>> keys;
    bool repeated = false;
    const nlohmann::json::parser_callback_t callback =
        [&keys, &repeated](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
            switch (event) {
                case nlohmann::json::parse_event_t::object_start:
                    keys.emplace_back();
                    break;
                case nlohmann::json::parse_event_t::key:
                    repeated = !keys.back().insert(parsed.get<std::string>()).second || repeated;
                    break;
                case nlohmann::json::parse_event_t::object_end:
                    keys.pop_back();
                    break;
                default:
                    break;
            }
            return true;
        };
    const nlohmann::json parsed = nlohmann::json::parse(text, callback);
    return repeated || parsed.is_discarded();
}

}  // namespace

TEST_CASE("forge probe json=1 of a stream with evolution frames is valid JSON in every container",
          "[forge-cli][truehd][evolution]") {
    // The container member is written by the probe's container writer and the evolution report by
    // the TrueHD report; detail=evolution, structure and format add members of their own. Each
    // document has to parse, name no key twice, and carry both objects.
    REQUIRE(repeats_a_key(R"({"container": null, "container": {"format": "mp4"}})"));
    REQUIRE_FALSE(repeats_a_key(R"({"a": {"a": 1}, "b": [{"a": 1}, {"a": 2}], "c": {"a": 3}})"));
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "container_evolution_json.log";
    const std::array<std::pair<std::string, fs::path>, 2> streams{
        {{"atmos_like", write_atmos_like_stream(dir)},
         {"signed_like", write_signed_like_stream(dir)}}};
    for (const auto& [name, stream] : streams) {
        for (const std::string container : {"thd", "mkv", "mp4", "ts"}) {
            fs::path input = stream;
            if (container != "thd") {
                input = dir / (name + "_evolution." + container);
                REQUIRE(run_cli(container + " \"" + stream.string() + "\" \"" + input.string() +
                                    "\"",
                                log) == 0);
            }
            for (const std::string detail : {"", " detail=evolution", " detail=structure",
                                             " detail=format"}) {
                DYNAMIC_SECTION(name + " in " + container + detail) {
                    REQUIRE(run_cli("probe \"" + input.string() + "\" json=1" + detail, log) == 0);
                    const std::string text = read_text(log);
                    CHECK_FALSE(repeats_a_key(text));
                    const auto document = nlohmann::json::parse(text);
                    CHECK(document["container"].is_object() == (container != "thd"));
                    REQUIRE(document["truehd"]["evolution"].is_object());
                    CHECK(document["truehd"]["evolution"]["regions"] == 6);
                    CHECK(document["truehd"]["evolution"]["provisional"] == true);
                }
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

    SECTION("a file with TrueHD alone is checked by qc and refused by name by transcode") {
        CHECK(run_cli("qc \"" + truehd_only.string() + "\"", log) == 0);
        CHECK(read_text(log).find("(TrueHD,") != std::string::npos);
        CHECK(read_text(log).find("integrity verdict: PASS") != std::string::npos);
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
        SECTION("the objects directory is not dropped either") {
            const fs::path objects = tone.dir / "objects_dir";
            // Objects are the channels of substream 3, which truehd-presentation=all decodes.
            CHECK(run_cli("decode " + in + out + cli_arg(objects), log) == kExitUsage);
            CHECK(read_text(log).find("only truehd-presentation=all decodes") !=
                  std::string::npos);
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

TEST_CASE("cut and cat refuse a TrueHD stream by name, and say why",
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

namespace {

// The BS.1770 integrated loudness of `samples` (interleaved, `channels` of them, 48 kHz, 24 bit)
// with the given speakers, through the library's meter.
double meter_lkfs(const std::vector<std::int32_t>& samples,
                  std::span<const iclforge::base::Speaker> speakers, std::size_t channels) {
    iclforge::base::LoudnessMeter meter(48000, speakers);
    constexpr std::size_t kBlock = 480;
    const std::size_t frames = samples.size() / channels;
    for (std::size_t at = 0; at + kBlock <= frames; at += kBlock) {
        std::vector<std::vector<float>> planes(channels, std::vector<float>(kBlock));
        std::vector<std::span<const float>> views;
        for (std::size_t c = 0; c < channels; ++c) {
            for (std::size_t n = 0; n < kBlock; ++n) {
                planes[c][n] = static_cast<float>(samples[((at + n) * channels) + c]) / 8388608.0F;
            }
            views.emplace_back(planes[c]);
        }
        meter.push(views);
    }
    const auto lkfs = meter.integrated_lkfs();
    REQUIRE(lkfs.has_value());
    return *lkfs;
}

// The major syncs' channel meanings of a stream, each read with its CRC checked.
std::vector<iclforge::truehd::ChannelMeaning> channel_meanings(const fs::path& stream) {
    const std::vector<std::byte> bytes = read_bytes(stream);
    std::vector<iclforge::truehd::ChannelMeaning> out;
    iclforge::truehd::ElementaryReader reader(bytes);
    while (const auto unit = reader.next()) {
        if (unit->size() >= 8 && iclforge::truehd::starts_with_major_sync(unit->subspan(4))) {
            const auto sync = iclforge::truehd::parse_major_sync(unit->subspan(4), true);
            REQUIRE(sync.has_value());
            out.push_back(sync->channel_meaning);
        }
    }
    return out;
}

// What ATSC A/85 section 8 makes of a loudness, as E-AC-3's normalize writes it: -LKFS rounded, 1
// to 31.
int dialnorm_of(double lkfs) {
    return std::clamp(static_cast<int>(std::lround(-lkfs)), 1, 31);
}

}  // namespace

TEST_CASE("normalize measures each presentation of a TrueHD stream and writes its dialogue norm",
          "[forge-cli][truehd][normalize]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "normalize.log";
    const fs::path out = tone.dir / "normalized.thd";
    using iclforge::base::Speaker;
    // The first tone channel (a 1 kHz sine at 0.5 of full scale) halved and repeated in all six
    // channels: the 2 channel presentation (the first two, the stream is coded with the default
    // downmix=none) and the 6 channel one (all of them, the surrounds weighted +1.5 dB) measure
    // differently, so the two fields are told apart.
    const std::vector<std::int32_t> tone_all = tone_samples();
    std::vector<std::int32_t> samples;
    std::vector<std::int32_t> stereo;
    for (std::size_t n = 0; n < kToneFrames; ++n) {
        const std::int32_t value = tone_all[n * 6] / 2;
        for (std::size_t c = 0; c < 6; ++c) {
            samples.push_back(value);
        }
        stereo.push_back(value);
        stereo.push_back(value);
    }
    const fs::path wav = tone.dir / "normalize_levels.wav";
    const fs::path thd = tone.dir / "normalize_levels.thd";
    write_wav(wav, 48000, 6, 0x60F, samples);
    REQUIRE(run_cli("truehd-encode " + cli_arg(wav) + cli_arg(thd), log) == 0);
    const std::array<Speaker, 6> bed{Speaker::kLeft,         Speaker::kRight,
                                     Speaker::kCentre,       Speaker::kLfe,
                                     Speaker::kLeftSurround, Speaker::kRightSurround};
    const std::array<Speaker, 2> pair{Speaker::kLeft, Speaker::kRight};
    const double lkfs_2ch = meter_lkfs(stereo, pair, 2);
    const double lkfs_6ch = meter_lkfs(samples, bed, 6);
    REQUIRE(dialnorm_of(lkfs_2ch) != dialnorm_of(lkfs_6ch));  // the two fields are told apart

    fs::remove(out);
    REQUIRE(run_cli("normalize " + cli_arg(thd) + cli_arg(out), log) == 0);
    const std::string text = read_text(log);
    CHECK(text.find("normalised ") != std::string::npos);
    CHECK(text.find("audio untouched") != std::string::npos);
    CHECK(text.find(fmt::format("2ch  measured {:+.2f} LKFS", lkfs_2ch)) != std::string::npos);
    CHECK(text.find(fmt::format("6ch  measured {:+.2f} LKFS", lkfs_6ch)) != std::string::npos);
    CHECK(text.find(fmt::format("2ch  dialogue norm 0 -> {} (ATSC A/85 section 8)",
                                dialnorm_of(lkfs_2ch))) != std::string::npos);
    CHECK(text.find(fmt::format("6ch  dialogue norm 0 -> {} (ATSC A/85 section 8)",
                                dialnorm_of(lkfs_6ch))) != std::string::npos);
    CHECK(text.find("no gain is applied") != std::string::npos);
    CHECK(text.find("control-enabled bit is off") != std::string::npos);
    CHECK(text.find("provisional, no oracle until the DEE set") != std::string::npos);

    // Every major sync carries the two dialogue norms, with a good CRC, and nothing else is set.
    const auto meanings = channel_meanings(out);
    REQUIRE_FALSE(meanings.empty());
    iclforge::truehd::ChannelMeaning expected;
    expected.twoch_dialogue_norm = static_cast<std::uint8_t>(dialnorm_of(lkfs_2ch));
    expected.sixch_dialogue_norm = static_cast<std::uint8_t>(dialnorm_of(lkfs_6ch));
    for (const auto& meaning : meanings) {
        CHECK(meaning == expected);  // no 8 channel presentation: its field is left alone
    }
    // The audio is the same, sample for sample.
    const fs::path a = tone.dir / "normalize_a.wav";
    const fs::path b = tone.dir / "normalize_b.wav";
    REQUIRE(run_cli("decode " + cli_arg(thd) + cli_arg(a), log) == 0);
    REQUIRE(run_cli("decode " + cli_arg(out) + cli_arg(b), log) == 0);
    CHECK(read_bytes(a) == read_bytes(b));
    // The probe gives them back, and the stream normalised again does not change.
    REQUIRE(run_cli("probe " + cli_arg(out) + "json=1", log) == 0);
    const auto presentations =
        nlohmann::json::parse(read_text(log))["truehd"]["channel_meaning"]["presentations"];
    CHECK(presentations["2ch"]["dialogue_norm"] == dialnorm_of(lkfs_2ch));
    CHECK(presentations["6ch"]["dialogue_norm"] == dialnorm_of(lkfs_6ch));
    const fs::path again = tone.dir / "normalized_again.thd";
    REQUIRE(run_cli("normalize " + cli_arg(out) + cli_arg(again), log) == 0);
    CHECK(read_bytes(again) == read_bytes(out));
    CHECK(read_text(log).find("6ch  dialogue norm " + std::to_string(dialnorm_of(lkfs_6ch)) +
                              " -> " + std::to_string(dialnorm_of(lkfs_6ch))) != std::string::npos);
}

TEST_CASE(
    "normalize keeps the mix level and the rest, clamps to 1..31 and refuses what it cannot do",
    "[forge-cli][truehd][normalize]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "normalize_more.log";

    SECTION(
        "only the dialogue norms change: the mix level, source format, controls and gains stay") {
        const fs::path set = tone.dir / "normalize_set.thd";
        const fs::path out = tone.dir / "normalize_set_out.thd";
        REQUIRE(
            run_cli("metadata " + cli_arg(tone.thd) + cli_arg(set) +
                        "truehd-mix-level-6ch=17 truehd-source-format-6ch=5 truehd-control-6ch=on "
                        "truehd-control-2ch=on truehd-drc-start-up-gain=21 "
                        "truehd-heavy-drc-start-up-gain=-9 truehd-dialnorm-6ch=31",
                    log) == 0);
        REQUIRE(run_cli("normalize " + cli_arg(set) + cli_arg(out), log) == 0);
        CHECK(read_text(log).find("control-enabled bit is off") == std::string::npos);
        const auto before = channel_meanings(set);
        const auto after = channel_meanings(out);
        REQUIRE(after.size() == before.size());
        for (std::size_t i = 0; i < after.size(); ++i) {
            iclforge::truehd::ChannelMeaning expected = before[i];
            expected.twoch_dialogue_norm = after[i].twoch_dialogue_norm;
            expected.sixch_dialogue_norm = after[i].sixch_dialogue_norm;
            CHECK(after[i] == expected);
            CHECK(after[i].sixch_dialogue_norm != 31);
            CHECK(after[i].sixch_mix_level == 17);
            CHECK(after[i].sixch_control_enabled);
        }
    }
    SECTION("a quiet stream is clamped to 31, and a silent one has nothing to normalise against") {
        std::vector<std::int32_t> quiet;
        for (const std::int32_t sample : tone_samples()) {
            quiet.push_back(sample / 1024);  // 60 dB down
        }
        const fs::path wav = tone.dir / "normalize_quiet.wav";
        const fs::path thd = tone.dir / "normalize_quiet.thd";
        const fs::path out = tone.dir / "normalize_quiet_out.thd";
        write_wav(wav, 48000, 6, 0x60F, quiet);
        REQUIRE(run_cli("truehd-encode " + cli_arg(wav) + cli_arg(thd), log) == 0);
        REQUIRE(run_cli("normalize " + cli_arg(thd) + cli_arg(out), log) == 0);
        for (const auto& meaning : channel_meanings(out)) {
            CHECK(meaning.sixch_dialogue_norm == 31);
            CHECK(meaning.twoch_dialogue_norm == 31);
        }

        const fs::path silent_wav = tone.dir / "normalize_silent.wav";
        const fs::path silent = tone.dir / "normalize_silent.thd";
        const fs::path silent_out = tone.dir / "normalize_silent_out.thd";
        write_wav(silent_wav, 48000, 6, 0x60F, std::vector<std::int32_t>(6 * kToneFrames, 0));
        REQUIRE(run_cli("truehd-encode " + cli_arg(silent_wav) + cli_arg(silent), log) == 0);
        fs::remove(silent_out);
        CHECK(run_cli("normalize " + cli_arg(silent) + cli_arg(silent_out), log) == 5);
        CHECK(read_text(log).find("nothing to normalise against") != std::string::npos);
        CHECK_FALSE(fs::exists(silent_out));
    }
    SECTION("a mono stream has only a 2 channel presentation, an MLP stream is refused") {
        const fs::path out = tone.dir / "normalize_mono_out.thd";
        const fs::path mono_wav = tone.dir / "normalize_mono.wav";
        const fs::path mono = tone.dir / "normalize_mono.thd";
        const std::vector<std::int32_t> all_channels = tone_samples();
        std::vector<std::int32_t> first_channel;
        for (std::size_t n = 0; n < kToneFrames; ++n) {
            first_channel.push_back(all_channels[n * 6]);
        }
        write_wav(mono_wav, 48000, 1, 0x4, first_channel);
        REQUIRE(run_cli("truehd-encode " + cli_arg(mono_wav) + cli_arg(mono), log) == 0);
        REQUIRE(run_cli("normalize " + cli_arg(mono) + cli_arg(out), log) == 0);
        CHECK(read_text(log).find("2ch  measured") != std::string::npos);
        CHECK(read_text(log).find("6ch") == std::string::npos);
        CHECK(channel_meanings(out).front().sixch_dialogue_norm == 0);
        fs::remove(out);
        CHECK(run_cli("normalize " + cli_arg(fixture("mlp_st48_s24.mlp")) + cli_arg(out), log) ==
              kExitUsage);
        CHECK(read_text(log).find("MLP") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("options it does not apply are refused by name; E-AC-3 still normalises") {
        const fs::path out = tone.dir / "normalize_refused.thd";
        CHECK(run_cli("normalize " + cli_arg(tone.thd) + cli_arg(out) + "dialnorm=27", log) ==
              kExitUsage);
        CHECK(read_text(log).find("normalize does not apply dialnorm=27") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
        CHECK(run_cli("normalize " + cli_arg(tone.thd) + cli_arg(out) + "truehd-dialnorm-6ch=3",
                      log) == kExitUsage);
        CHECK_FALSE(fs::exists(out));
        const fs::path ec3 = fixture("../audio/reference_51_eac3_448k_cplbndstrce0.ec3");
        REQUIRE(run_cli("normalize " + cli_arg(ec3) + cli_arg(tone.dir / "normalized.ec3"), log) ==
                0);
        CHECK(read_text(log).find("normalised ") != std::string::npos);
    }
    SECTION("a damaged stream fails the measurement, strict, and writes nothing") {
        const fs::path bad = damaged_copy("truehd_st48_s24.thd", tone.dir);
        const fs::path out = tone.dir / "normalize_damaged_out.thd";
        fs::remove(out);
        CHECK(run_cli("normalize " + cli_arg(bad) + cli_arg(out), log) == kExitInput);
        CHECK_FALSE(fs::exists(out));
    }
}

namespace {

// The tone scaled so that its 6 channel integrated loudness is `target` LKFS, as 24-bit samples.
std::vector<std::int32_t> tone_at(double target) {
    using iclforge::base::Speaker;
    const std::array<Speaker, 6> bed{Speaker::kLeft,         Speaker::kRight,
                                     Speaker::kCentre,       Speaker::kLfe,
                                     Speaker::kLeftSurround, Speaker::kRightSurround};
    const std::vector<std::int32_t> tone = tone_samples();
    const double gain = std::pow(10.0, (target - meter_lkfs(tone, bed, 6)) / 20.0);
    std::vector<std::int32_t> scaled;
    for (const std::int32_t sample : tone) {
        scaled.push_back(
            static_cast<std::int32_t>(std::lround(static_cast<double>(sample) * gain)));
    }
    return scaled;
}

// White noise at full scale, six channels, a second of it: more bits than a rate of a few Mbit/s.
std::vector<std::int32_t> noise_samples() {
    std::vector<std::int32_t> samples;
    std::uint32_t state = 12345;
    for (std::size_t i = 0; i < kToneFrames * 6; ++i) {
        state = (state * 1664525U) + 1013904223U;
        samples.push_back(static_cast<std::int32_t>(state >> 8U) - 8388608);
    }
    return samples;
}

}  // namespace

TEST_CASE(
    "qc of a TrueHD stream holds its integrity, measures each presentation and compares the "
    "dialogue norm",
    "[forge-cli][truehd][qc]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "qc.log";
    using iclforge::base::Speaker;
    const std::array<Speaker, 6> bed{Speaker::kLeft,         Speaker::kRight,
                                     Speaker::kCentre,       Speaker::kLfe,
                                     Speaker::kLeftSurround, Speaker::kRightSurround};
    const double lkfs_6ch = meter_lkfs(tone_samples(), bed, 6);

    SECTION("a sound stream passes, and the report names every check and presentation") {
        REQUIRE(run_cli("qc " + cli_arg(tone.thd), log) == 0);
        const std::string text = read_text(log);
        CHECK(text.find("qc: ") != std::string::npos);
        CHECK(text.find("(TrueHD, L R C LFE Ls Rs, 48000 Hz, 1200 access unit(s)") !=
              std::string::npos);
        CHECK(text.find("integrity (TH spec 6.2 parity and CRC, 7.3 lossless check, 18 data "
                        "rate):") != std::string::npos);
        CHECK(text.find("access units         1200 read, 0 byte(s) skipped, 0 unreadable  PASS") !=
              std::string::npos);
        CHECK(text.find(" 0 failed (check nibble, major sync, restart header, parity, segment "
                        "CRC)  PASS") != std::string::npos);
        CHECK(text.find("interval(s) ok, 0 failed  PASS") != std::string::npos);
        CHECK(text.find("data rate            18.00 Mbit/s declared") != std::string::npos);
        CHECK(text.find("provisional") != std::string::npos);
        CHECK(text.find("integrity verdict: PASS") != std::string::npos);
        CHECK(text.find("2ch (L R):") != std::string::npos);
        CHECK(text.find("6ch (L R C LFE Ls Rs), the presentation the gates are judged on:") !=
              std::string::npos);
        CHECK(text.find(fmt::format("integrated loudness  {:>+8.2f} LKFS", lkfs_6ch)) !=
              std::string::npos);
        // Nothing is gated without a preset.
        CHECK(text.find("gates:") == std::string::npos);
        // A container carries the same stream and gives the same report.
        const fs::path mkv = tone.dir / "qc_tone.mkv";
        REQUIRE(run_cli("mkv " + cli_arg(tone.thd) + cli_arg(mkv), log) == 0);
        REQUIRE(run_cli("qc " + cli_arg(mkv), log) == 0);
        CHECK(after_first_line(read_text(log)) == after_first_line(text));
    }
    SECTION("the dialogue norm it carries is compared with the measurement") {
        // The stream as it was written: no norm, so the claim is far from the measurement.
        REQUIRE(run_cli("qc " + cli_arg(tone.thd), log) == 0);
        const std::string before = read_text(log);
        CHECK(before.find("measurement-derived dialogue norm would be " +
                          std::to_string(dialnorm_of(lkfs_6ch)) + ", not 0") != std::string::npos);
        CHECK(before.find("control-enabled bit is off, so a decoder may not apply it") !=
              std::string::npos);
        CHECK(before.find("embedded metadata (provisional, no oracle until the DEE set") !=
              std::string::npos);
        // Normalised, with the control bit on: the same value, and the claim matches.
        const fs::path set = tone.dir / "qc_normalized.thd";
        REQUIRE(run_cli("normalize " + cli_arg(tone.thd) + cli_arg(set), log) == 0);
        const fs::path on = tone.dir / "qc_normalized_on.thd";
        REQUIRE(run_cli("metadata " + cli_arg(set) + cli_arg(on) + "truehd-control-6ch=on", log) ==
                0);
        REQUIRE(run_cli("qc " + cli_arg(on), log) == 0);
        const std::string after = read_text(log);
        CHECK(after.find("(matches)") != std::string::npos);
        CHECK(after.find("control on, dialogue norm " + std::to_string(dialnorm_of(lkfs_6ch))) !=
              std::string::npos);
        CHECK(after.find("claimed              " +
                         fmt::format("{:>+8.2f}", 0.0 - dialnorm_of(lkfs_6ch))) !=
              std::string::npos);
        // The delta is the measurement less the claim.
        CHECK(after.find(fmt::format("delta                {:>+8.2f} dB",
                                     lkfs_6ch + dialnorm_of(lkfs_6ch))) != std::string::npos);
    }
    SECTION(
        "the preset gates judge the largest presentation, or the one truehd-presentation= "
        "names") {
        // The tone is far above -23 LKFS: EBU R 128 s2 fails and the exit status says a gate
        // failed.
        CHECK(run_cli("qc " + cli_arg(tone.thd) + "preset=ebu-r128-s2", log) == 6);
        const std::string failing = read_text(log);
        CHECK(failing.find("gates:") != std::string::npos);
        CHECK(failing.find("ebu-r128-s2") != std::string::npos);
        CHECK(failing.find("verdict: FAIL") != std::string::npos);
        CHECK(failing.find("integrity verdict: PASS") != std::string::npos);
        // Scaled to -23.0 LKFS and clear of -1 dBTP, the 6 channel presentation passes.
        const fs::path wav = tone.dir / "qc_minus23.wav";
        const fs::path thd = tone.dir / "qc_minus23.thd";
        write_wav(wav, 48000, 6, 0x60F, tone_at(-23.0));
        REQUIRE(run_cli("truehd-encode " + cli_arg(wav) + cli_arg(thd), log) == 0);
        CHECK(run_cli("qc " + cli_arg(thd) + "preset=ebu-r128-s2", log) == 0);
        CHECK(read_text(log).find("verdict: PASS") != std::string::npos);
        // The 2 channel presentation of that stream measures differently (its first two channels
        // only), and the same preset judged on it is a different verdict.
        run_cli("qc " + cli_arg(thd) + "preset=ebu-r128-s2 truehd-presentation=2ch", log);
        const std::string two = read_text(log);
        CHECK(two.find("2ch (L R), the presentation the gates are judged on:") !=
              std::string::npos);
        CHECK(two.find("6ch (L R C LFE Ls Rs):") != std::string::npos);
    }
    SECTION("what it is not asked to judge, or cannot, is refused by name") {
        for (const std::string option : {"layout=rendered", "objects=514", "programme=1",
                                         "truehd-check=lenient", "dialnorm=27"}) {
            DYNAMIC_SECTION(option) {
                CHECK(run_cli("qc " + cli_arg(tone.thd) + option, log) == kExitUsage);
                CHECK(read_text(log).find("qc does not apply " + option) != std::string::npos);
            }
        }
        CHECK(run_cli("qc " + cli_arg(tone.thd) + "verify-content", log) == kExitUsage);
        CHECK(run_cli("qc " + cli_arg(tone.thd) + "truehd-presentation=all", log) == kExitUsage);
        CHECK(run_cli("qc " + cli_arg(tone.thd) + "truehd-presentation=8ch", log) == kExitUsage);
        CHECK(read_text(log).find("has no 8ch presentation") != std::string::npos);
    }
    SECTION("E-AC-3 and AC-3 are judged as before, and the TrueHD option is warned about there") {
        const fs::path ec3 = fixture("../audio/reference_51_eac3_448k_cplbndstrce0.ec3");
        REQUIRE(run_cli("qc " + cli_arg(ec3) + "truehd-presentation=2ch", log) == 0);
        CHECK(read_text(log).find("is not TrueHD or MLP") != std::string::npos);
        CHECK(read_text(log).find("dialnorm check:") != std::string::npos);
    }
    SECTION("an MLP stream is checked, and says it has no channel meaning") {
        REQUIRE(run_cli("qc " + cli_arg(fixture("mlp_st48_s24.mlp")), log) == 0);
        const std::string text = read_text(log);
        CHECK(text.find("(MLP, ") != std::string::npos);
        CHECK(text.find("integrity verdict: PASS") != std::string::npos);
        CHECK(text.find("embedded metadata: none described for MLP") != std::string::npos);
    }
}

TEST_CASE("qc of a TrueHD stream fails, exit 6, on a damaged stream, a join, and a rate it breaks",
          "[forge-cli][truehd][qc]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "qc_fail.log";

    SECTION("a flipped bit is counted, not a reason to stop") {
        const fs::path bad = damaged_copy("truehd_st48_s24.thd", tone.dir);
        CHECK(run_cli("qc " + cli_arg(bad), log) == 6);
        const std::string text = read_text(log);
        CHECK(text.find("integrity verdict: FAIL") != std::string::npos);
        CHECK(text.find("(check nibble, major sync, restart header, parity, segment CRC)  FAIL") !=
              std::string::npos);
    }
    SECTION("bytes between units that are not a unit are counted") {
        std::vector<std::byte> bytes = read_bytes(fixture("truehd_st48_s24.thd"));
        std::size_t offset = 0;
        for (int unit = 0; unit < 10; ++unit) {
            offset += (((std::to_integer<std::size_t>(bytes[offset]) & 0xFU) << 8U) |
                       std::to_integer<std::size_t>(bytes[offset + 1])) *
                      2;
        }
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 9, std::byte{0x5A});
        const fs::path junk = tone.dir / "qc_junk.thd";
        {
            std::ofstream out{junk, std::ios::binary};
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        CHECK(run_cli("qc " + cli_arg(junk), log) == 6);
        CHECK(read_text(log).find("byte(s) skipped") != std::string::npos);
        CHECK(read_text(log).find("0 byte(s) skipped") == std::string::npos);
    }
    SECTION("two streams joined end to end fail the lossless check at the join") {
        // ERRATA.md question 20: the second stream's first restart header checks nothing.
        CHECK(run_cli("qc " + cli_arg(fixture("truehd_cat_st48_s24.thd")), log) == 6);
        CHECK(read_text(log).find("interval(s) ok, 1 failed  FAIL") != std::string::npos);
    }
    SECTION("a stream that cannot be sent within its declared peak rate") {
        const fs::path wav = tone.dir / "qc_noise.wav";
        const fs::path fits = tone.dir / "qc_noise_fits.thd";
        const fs::path late = tone.dir / "qc_noise_late.thd";
        write_wav(wav, 48000, 6, 0x60F, noise_samples());
        REQUIRE(run_cli("truehd-encode " + cli_arg(wav) + cli_arg(fits), log) == 0);
        REQUIRE(run_cli("truehd-encode " + cli_arg(wav) + cli_arg(late) +
                            "truehd-peak-rate=2000 truehd-rate-check=lenient",
                        log) == 0);
        CHECK(run_cli("qc " + cli_arg(fits), log) == 0);
        CHECK(run_cli("qc " + cli_arg(late), log) == 6);
        const std::string text = read_text(log);
        CHECK(text.find("data rate            2.00 Mbit/s declared") != std::string::npos);
        CHECK(text.find("unit(s) late  FAIL") != std::string::npos);
        CHECK(text.find(" 0 unit(s) late") == std::string::npos);
        // Everything else about that stream is sound.
        CHECK(text.find("interval(s) ok, 0 failed  PASS") != std::string::npos);
    }
    SECTION("nothing to read is an input error, not a failed gate") {
        const fs::path empty = tone.dir / "qc_empty.thd";
        { std::ofstream{empty, std::ios::binary}; }
        const int status = run_cli("qc " + cli_arg(empty), log);
        CHECK(status != 0);
        CHECK(status != 6);
    }
}

TEST_CASE("transcode and fmp4 refuse a TrueHD stream by name", "[forge-cli][truehd][refusals]") {
    const ToneFiles& tone = tone_files();
    const fs::path log = tone.dir / "refusals.log";
    const fs::path out = tone.dir / "refused_out";
    struct Refusal {
        std::string command;
        std::string args;
    };
    const std::vector<Refusal> refusals{
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
