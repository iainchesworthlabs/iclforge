#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include "iclforge/sendspin/codec.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "take_recorder.hpp"

// apps/hearth/engine/src/take_recorder.*: a take kept as lossless FLAC with a description beside it
// (planning/karaoke-microphone.md, M6; D11, D13).

namespace {

namespace fs = std::filesystem;

using iclforge::hearth::TakeMetadata;
using iclforge::hearth::TakeRecorder;

constexpr std::uint32_t kRate = 48000;

// A folder of this process's own under the system's temporary one, removed with it.
class Folder {
   public:
    Folder() {
        static int n = 0;
        path_ = fs::temp_directory_path() /
                ("iclforge-take-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(n++));
        fs::create_directories(path_);
    }
    ~Folder() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

   private:
    fs::path path_;
};

std::vector<float> tone(double hz, double seconds, double amplitude = 0.5) {
    std::vector<float> x(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    return x;
}

std::vector<std::uint8_t> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string read_text(const fs::path& path) {
    const auto bytes = read_all(path);
    return std::string(bytes.begin(), bytes.end());
}

std::int32_t quantised(float v) {
    const double clamped = std::clamp(static_cast<double>(v), -1.0, 1.0);
    return static_cast<std::int32_t>(std::lround(clamped * 8388607.0));
}

}  // namespace

TEST_CASE("take file names: nothing a file system refuses, and a sensible length", "[take]") {
    using iclforge::hearth::take_file_stem;
    CHECK(take_file_stem("Auld Lang Syne", "2026-10-12 211500") ==
          "Auld Lang Syne - 2026-10-12 211500");
    CHECK(take_file_stem("A/B: C?*\"<>|", "t") == "A_B_ C______ - t");
    CHECK(take_file_stem("", "t") == "Take - t");
    CHECK(take_file_stem("...  ", "t") == "Take - t");
    CHECK(take_file_stem("Name. ", "t") == "Name - t");
    // 90 ASCII letters are cut to 80.
    CHECK(take_file_stem(std::string(90, 'x'), "t") == std::string(80, 'x') + " - t");
    // Never through the middle of a two-byte letter: 79 ASCII letters then "e" with an acute (2
    // bytes).
    const std::string cut = take_file_stem(std::string(79, 'x') + "\xC3\xA9\xC3\xA9", "t");
    CHECK(cut == std::string(79, 'x') + " - t");
}

TEST_CASE("take recorder: a take is lossless FLAC with its length in the header", "[take]") {
    Folder folder;
    const fs::path file = folder.path() / "one.flac";
    TakeRecorder recorder;
    REQUIRE(recorder.begin(file, kRate).has_value());
    CHECK(recorder.recording());

    const std::vector<float> samples = tone(440.0, 2.0);
    // In blocks of 480, as a microphone delivers them.
    for (std::size_t at = 0; at < samples.size(); at += 480) {
        const std::size_t n = std::min<std::size_t>(480, samples.size() - at);
        REQUIRE(recorder.write(std::span<const float>(samples).subspan(at, n)));
    }
    CHECK(recorder.frames() == samples.size());

    TakeMetadata metadata;
    metadata.item_path = "C:/songs/Auld Lang Syne.ac3";
    metadata.item_title = "Auld Lang Syne";
    metadata.item_start_ms = 1500;
    metadata.input_delay_ms = 130;
    metadata.offset_ms = -20;
    metadata.reference = "the melody guide (M)";
    metadata.headphones = true;
    const auto info = recorder.finish(metadata);
    REQUIRE(info.has_value());
    CHECK(!recorder.recording());
    CHECK(info->frames == samples.size());
    CHECK(info->seconds == Catch::Approx(2.0));
    CHECK(info->sample_rate == kRate);

    // The file: the marker, a STREAMINFO block, and the length in it.
    const auto bytes = read_all(file);
    REQUIRE(bytes.size() > 42);
    CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "fLaC");
    std::uint64_t packed = 0;
    for (std::size_t i = 18; i < 26; ++i) {
        packed = (packed << 8) | bytes[i];
    }
    CHECK((packed & ((std::uint64_t{1} << 36) - 1)) == samples.size());
    // The rate, one channel and 24 bits in the same word.
    CHECK(((packed >> 44) & 0xFFFFF) == kRate);
    CHECK(((packed >> 41) & 0x7) == 0);
    CHECK(((packed >> 36) & 0x1F) == 23);

    // Lossless: decoded again, it is what went in, to the 24-bit quantisation.
    iclforge::sendspin::messages::PlayerStream stream;
    stream.format.codec = iclforge::sendspin::messages::Codec::kFlac;
    stream.format.channels = 1;
    stream.format.sample_rate = static_cast<std::int32_t>(kRate);
    stream.format.bit_depth = 24;
    stream.codec_header.assign(bytes.begin(), bytes.begin() + 42);
    auto decoder = iclforge::sendspin::codec::make_decoder(stream);
    REQUIRE(decoder != nullptr);
    const std::vector<std::uint8_t> frames(bytes.begin() + 42, bytes.end());
    const auto decoded = decoder->decode(frames);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == samples.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        differing += (*decoded)[i] != quantised(samples[i]) ? 1U : 0U;
    }
    CHECK(differing == 0);
}

TEST_CASE("take recorder: the description says where the take starts and what it was made with",
          "[take]") {
    Folder folder;
    TakeRecorder recorder;
    REQUIRE(recorder.begin(folder.path() / "two.flac", kRate).has_value());
    REQUIRE(recorder.write(tone(220.0, 0.5)));
    TakeMetadata metadata;
    metadata.item_title = "A \"quoted\" song\\with\ttabs";
    metadata.item_path = "D:/songs/a.ac3";
    metadata.item_start_ms = 2750;
    metadata.input_delay_ms = 130;
    metadata.timing_estimated = true;
    metadata.reference = "vocal 1 (V1)";
    const auto info = recorder.finish(metadata);
    REQUIRE(info.has_value());
    CHECK(info->sidecar.filename().string() == "two.take.json");
    const std::string json = read_text(info->sidecar);
    CHECK(json.find("\"format\": \"iclforge-take-1\"") != std::string::npos);
    CHECK(json.find("\"audio\": \"two.flac\"") != std::string::npos);
    CHECK(json.find("\"dry_voice\": true") != std::string::npos);
    CHECK(json.find("\"sample_rate\": 48000") != std::string::npos);
    CHECK(json.find("\"item_start_ms\": 2750") != std::string::npos);
    CHECK(json.find("\"input_delay_ms\": 130") != std::string::npos);
    CHECK(json.find("\"timing_estimated\": true") != std::string::npos);
    CHECK(json.find("\"reference\": \"vocal 1 (V1)\"") != std::string::npos);
    CHECK(json.find("\"headphones\": false") != std::string::npos);
    // The title's quotes, backslash and tab are escaped.
    CHECK(json.find("A \\\"quoted\\\" song\\\\with\\ttabs") != std::string::npos);
}

TEST_CASE("take recorder: a start that is not known is null, not zero", "[take]") {
    Folder folder;
    TakeRecorder recorder;
    REQUIRE(recorder.begin(folder.path() / "three.flac", kRate).has_value());
    REQUIRE(recorder.write(tone(330.0, 0.2)));
    const auto info = recorder.finish({});
    REQUIRE(info.has_value());
    CHECK(read_text(info->sidecar).find("\"item_start_ms\": null") != std::string::npos);
}

TEST_CASE("take recorder: the folder is made, and a discarded take leaves nothing", "[take]") {
    Folder folder;
    const fs::path file = folder.path() / "a" / "b" / "four.flac";
    TakeRecorder recorder;
    REQUIRE(recorder.begin(file, kRate).has_value());
    CHECK(fs::exists(file));
    REQUIRE(recorder.write(tone(440.0, 0.3)));
    recorder.discard();
    CHECK(!recorder.recording());
    CHECK(!fs::exists(file));
    // And it can begin again.
    REQUIRE(recorder.begin(file, kRate).has_value());
    CHECK(recorder.finish({}).has_value());
    CHECK(fs::exists(file));
}

TEST_CASE("take recorder: what cannot be done is said", "[take]") {
    Folder folder;
    TakeRecorder recorder;
    // A path that is a folder.
    const auto refused = recorder.begin(folder.path(), kRate);
    REQUIRE(!refused.has_value());
    CHECK(!refused.error().empty());
    CHECK(!recorder.recording());
    // Nothing recording: nothing to write or finish.
    const std::vector<float> one(10, 0.1F);
    CHECK(!recorder.write(one));
    CHECK(!recorder.finish({}).has_value());
    // A second begin while one runs.
    REQUIRE(recorder.begin(folder.path() / "five.flac", kRate).has_value());
    const auto again = recorder.begin(folder.path() / "six.flac", kRate);
    REQUIRE(!again.has_value());
    CHECK(again.error().find("already") != std::string::npos);
}

TEST_CASE("take recorder: samples beyond full scale are clipped, not wrapped", "[take]") {
    Folder folder;
    const fs::path file = folder.path() / "seven.flac";
    TakeRecorder recorder;
    REQUIRE(recorder.begin(file, kRate).has_value());
    std::vector<float> loud(4800, 3.0F);
    for (std::size_t i = 0; i < loud.size(); i += 2) {
        loud[i] = -3.0F;
    }
    REQUIRE(recorder.write(loud));
    REQUIRE(recorder.finish({}).has_value());

    const auto bytes = read_all(file);
    iclforge::sendspin::messages::PlayerStream stream;
    stream.format.codec = iclforge::sendspin::messages::Codec::kFlac;
    stream.format.channels = 1;
    stream.format.sample_rate = static_cast<std::int32_t>(kRate);
    stream.format.bit_depth = 24;
    stream.codec_header.assign(bytes.begin(), bytes.begin() + 42);
    auto decoder = iclforge::sendspin::codec::make_decoder(stream);
    REQUIRE(decoder != nullptr);
    const auto decoded =
        decoder->decode(std::vector<std::uint8_t>(bytes.begin() + 42, bytes.end()));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == loud.size());
    CHECK((*decoded)[0] == -8388607);
    CHECK((*decoded)[1] == 8388607);
}
