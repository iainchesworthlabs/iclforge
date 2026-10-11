#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "platform/process.hpp"

#include "cdg_frames.hpp"

// apps/shared/cdg-picture/src/cdg_frames.cpp: the images a CD+G disc draws, by position. The format
// is held to its arithmetic in libs/lyrics/tests/test_cdg.cpp; here a disc only has to be a disc,
// and what is held is what the class adds: the picture as an image, the same image while no new
// packet has been reached, a replay when the position goes back, and nothing held after a failure.

namespace {

namespace fs = std::filesystem;
using iclforge::apps::CdgFrames;

using Data = std::array<std::uint8_t, 16>;

// One 24-byte packet: command 9, the instruction, two parity bytes, sixteen data bytes, four parity.
std::string packet(int instruction, const Data& data = {}) {
    std::string out(24, static_cast<char>(0));
    out[0] = static_cast<char>(9);
    out[1] = static_cast<char>(instruction);
    for (std::size_t i = 0; i < data.size(); ++i) {
        out[4 + i] = static_cast<char>(data[i]);
    }
    return out;
}

Data first_byte(int value) {
    Data data{};
    data[0] = static_cast<std::uint8_t>(value);
    return data;
}

// A disc whose colour 1 is red and colour 2 green: the screen is `first` from the first packets and
// `second` from the packet at one second (300 packets in): 301 packets, 1003 ms.
std::string disc(int first = 1, int second = 2) {
    Data colours{};
    // Colour 1 is 0xF00 and colour 2 is 0x0F0, as two six-bit bytes each.
    colours[2] = 0x3C;
    colours[3] = 0x00;
    colours[4] = 0x03;
    colours[5] = 0x30;
    std::string bytes = packet(30, colours);
    bytes += packet(1, first_byte(first));
    for (int i = 2; i < 300; ++i) {
        bytes += packet(0);
    }
    bytes += packet(1, first_byte(second));
    return bytes;
}

constexpr std::uint32_t kBlack = 0xFF000000U;
constexpr std::uint32_t kRed = 0xFFFF0000U;
constexpr std::uint32_t kGreen = 0xFF00FF00U;

}  // namespace

TEST_CASE("CdgFrames: nothing loaded is no picture", "[app-cdg-picture][cdg]") {
    CdgFrames frames;
    CHECK_FALSE(frames.loaded());
    CHECK(frames.duration_ms() == 0);
    CHECK(frames.frame(0).isNull());
    CHECK(frames.frame(5000).isNull());
}

TEST_CASE("CdgFrames: a disc is an image of 300 by 216 that follows the position", "[app-cdg-picture][cdg]") {
    CdgFrames frames;
    REQUIRE(frames.load_bytes(disc()).has_value());
    CHECK(frames.loaded());
    CHECK(frames.duration_ms() == 301 * 1000 / 300);

    // Before the first packet is applied the screen is black; by 500 ms the colours are loaded.
    const QImage start = frames.frame(0);
    REQUIRE_FALSE(start.isNull());
    CHECK(start.width() == 300);
    CHECK(start.height() == 216);
    CHECK(start.format() == QImage::Format_ARGB32);
    CHECK(start.pixel(100, 100) == kBlack);
    const QImage red = frames.frame(500);
    CHECK(red.pixel(100, 100) == kRed);

    // A position between two packets is the same image, not a copy of it: no new packet was reached.
    const QImage same = frames.frame(502);
    CHECK(same.constBits() == red.constBits());

    // The packet at one second is applied by 1000 ms and turns it green; a new packet is a new image.
    const QImage green = frames.frame(1100);
    CHECK(green.pixel(100, 100) == kGreen);
    CHECK(green.constBits() != red.constBits());

    // Going back replays from the start, and a position before 0 is the start.
    CHECK(frames.frame(500).pixel(100, 100) == kRed);
    CHECK(frames.frame(-250).pixel(100, 100) == kBlack);
    // Past the end is the last picture.
    CHECK(frames.frame(600000).pixel(100, 100) == kGreen);
}

TEST_CASE("CdgFrames: a second disc is its own picture and clear() takes it away", "[app-cdg-picture][cdg]") {
    CdgFrames frames;
    REQUIRE(frames.load_bytes(disc(1, 2)).has_value());
    CHECK(frames.frame(500).pixel(100, 100) == kRed);

    // The same number of packets applied at the same position, but a different disc: the cached
    // image of the first must not be handed out for it.
    REQUIRE(frames.load_bytes(disc(2, 1)).has_value());
    CHECK(frames.frame(500).pixel(100, 100) == kGreen);

    frames.clear();
    CHECK_FALSE(frames.loaded());
    CHECK(frames.frame(500).isNull());
    CHECK(frames.duration_ms() == 0);
}

TEST_CASE("CdgFrames: a file that is not CD+G says why and leaves nothing held", "[app-cdg-picture][cdg]") {
    CdgFrames frames;
    REQUIRE(frames.load_bytes(disc()).has_value());
    REQUIRE_FALSE(frames.frame(500).isNull());

    // Not a whole number of packets.
    const auto bad = frames.load_bytes(std::string(25, 'x'));
    REQUIRE_FALSE(bad.has_value());
    CHECK_FALSE(bad.error().empty());
    CHECK_FALSE(frames.loaded());
    CHECK(frames.frame(500).isNull());

    // No packets at all.
    CHECK_FALSE(frames.load_bytes(std::string{}).has_value());
    CHECK_FALSE(frames.loaded());
}

TEST_CASE("CdgFrames: a disc is read from a file, and a file that is missing is an error", "[app-cdg-picture][cdg]") {
    const fs::path folder =
        fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("cdg_frames_" + iclforge::test::platform::process_id());
    fs::remove_all(folder);
    fs::create_directories(folder);
    {
        std::ofstream out(folder / "disc.cdg", std::ios::binary);
        const std::string bytes = disc();
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    CdgFrames frames;
    REQUIRE(frames.load(folder / "disc.cdg").has_value());
    CHECK(frames.loaded());
    CHECK(frames.frame(500).pixel(100, 100) == kRed);

    // A file that is not there is an error and the disc held before it is gone.
    const auto missing = frames.load(folder / "none.cdg");
    REQUIRE_FALSE(missing.has_value());
    CHECK_FALSE(missing.error().empty());
    CHECK_FALSE(frames.loaded());

    fs::remove_all(folder);
}

TEST_CASE("find_cdg_beside: the name.cdg beside an item, in any case", "[app-cdg-picture][cdg]") {
    const fs::path folder =
        fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("cdg_beside_" + iclforge::test::platform::process_id());
    fs::remove_all(folder);
    fs::create_directories(folder);
    for (const char* name : {"disc.cdg", "SHOUT.CDG"}) {
        std::ofstream out(folder / name, std::ios::binary);
        out << disc();
    }

    const auto lower = iclforge::apps::find_cdg_beside((folder / "disc.mp3").string());
    REQUIRE(lower.has_value());
    CHECK(lower->filename().string() == "disc.cdg");
    const auto upper = iclforge::apps::find_cdg_beside((folder / "SHOUT.ac3").string());
    REQUIRE(upper.has_value());
    CHECK(upper->filename().string() == "SHOUT.CDG");
    // Nothing beside an item with no disc, and nothing for an empty path.
    CHECK_FALSE(iclforge::apps::find_cdg_beside((folder / "other.mp3").string()).has_value());
    CHECK_FALSE(iclforge::apps::find_cdg_beside("").has_value());

    fs::remove_all(folder);
}

TEST_CASE("CdgFrames: moving one carries the disc and its screen", "[app-cdg-picture][cdg]") {
    CdgFrames first;
    REQUIRE(first.load_bytes(disc()).has_value());
    CHECK(first.frame(500).pixel(100, 100) == kRed);

    CdgFrames second(std::move(first));
    CHECK(second.loaded());
    CHECK(second.frame(1100).pixel(100, 100) == kGreen);

    CdgFrames third;
    third = std::move(second);
    CHECK(third.loaded());
    CHECK(third.frame(500).pixel(100, 100) == kRed);
}
