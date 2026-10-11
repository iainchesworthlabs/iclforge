#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "platform/process.hpp"

#include "iclforge/lyrics/cdg.hpp"

// libs/lyrics/src/cdg.cpp: the picture of a CD+G file (planning/karaoke-phase2.md, W13f). Files are
// built here by hand, one 24-byte packet at a time, so that every instruction is held to the
// arithmetic of the format and not to a file that was only ever read.

namespace {

namespace fs = std::filesystem;
using iclforge::lyrics::CdgFile;
using iclforge::lyrics::CdgScreen;

using Data = std::array<std::uint8_t, 16>;

std::string packet(int instruction, const Data& data, int command = 9) {
    std::string out(24, static_cast<char>(0));
    out[0] = static_cast<char>(command);
    out[1] = static_cast<char>(instruction);
    for (std::size_t i = 0; i < data.size(); ++i) {
        out[4 + i] = static_cast<char>(data[i]);
    }
    return out;
}

// A colour table entry: 4 bits each of red, green and blue as the two six-bit bytes of the format.
void colour_bytes(Data& data, std::size_t slot, int red, int green, int blue) {
    const int value = (red << 8) | (green << 4) | blue;
    data[2 * slot] = static_cast<std::uint8_t>((value >> 6) & 0x3F);
    data[(2 * slot) + 1] = static_cast<std::uint8_t>(value & 0x3F);
}

std::string preset(int colour) {
    Data d{};
    d[0] = static_cast<std::uint8_t>(colour);
    return packet(1, d);
}

std::string border(int colour) {
    Data d{};
    d[0] = static_cast<std::uint8_t>(colour);
    return packet(2, d);
}

// A tile at (row, column): six-bit rows, the leftmost pixel being bit 5.
std::string tile(int colour0, int colour1, int row, int column, const std::array<std::uint8_t, 12>& rows,
                 bool xor_tile = false) {
    Data d{};
    d[0] = static_cast<std::uint8_t>(colour0);
    d[1] = static_cast<std::uint8_t>(colour1);
    d[2] = static_cast<std::uint8_t>(row);
    d[3] = static_cast<std::uint8_t>(column);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        d[4 + i] = rows[i];
    }
    return packet(xor_tile ? 38 : 6, d);
}

std::string scroll(bool copy, int fill, int horizontal, int vertical) {
    Data d{};
    d[0] = static_cast<std::uint8_t>(fill);
    d[1] = static_cast<std::uint8_t>(horizontal << 4);
    d[2] = static_cast<std::uint8_t>(vertical << 4);
    return packet(copy ? 24 : 20, d);
}

CdgFile parsed(const std::string& bytes) {
    const auto file = iclforge::lyrics::parse_cdg(bytes);
    REQUIRE(file.has_value());
    return *file;
}

// The sentence a file is refused with, empty when it was not refused.
std::string refusal(const std::string& bytes) {
    const auto file = iclforge::lyrics::parse_cdg(bytes);
    return file.has_value() ? std::string{} : file.error();
}

constexpr std::array<std::uint8_t, 12> kStripes = {0b100000, 0b010000, 0b001000, 0b000100, 0b000010, 0b000001,
                                                   0b100000, 0b010000, 0b001000, 0b000100, 0b000010, 0b000001};

}  // namespace

TEST_CASE("cdg: a file is whole packets, and another channel's packets keep their place as nothing",
          "[lyrics][cdg]") {
    CHECK(refusal("") == "The CD+G file is empty.");
    CHECK(refusal(std::string(25, 'x')) == "The file is not CD+G: its length is not a whole number of packets.");
    // A whole number of packets and not one is CD+G.
    CHECK(refusal(std::string(48, 'x')) == "The file has no CD+G packets in it.");

    // Command 9 in the low six bits (the top two are the subcode's own), instruction in the low six.
    const std::string bytes = packet(1, Data{}, 9 | 0x40) + packet(1, Data{}, 0) + packet(2, Data{}, 9) +
                              packet(0x40 | 6, Data{}, 9);
    const CdgFile file = parsed(bytes);
    REQUIRE(file.packets.size() == 4);
    CHECK(file.packets[0].instruction == 1);
    // A packet of another channel is a packet that does nothing, in its own place in time.
    CHECK(file.packets[1].instruction == 0);
    CHECK(file.packets[2].instruction == 2);
    CHECK(file.packets[3].instruction == 6);
    // 300 packets a second.
    CHECK(file.duration_ms() == 13);
}

TEST_CASE("cdg: a colour table of 12-bit colours, a preset, and the border", "[lyrics][cdg]") {
    Data low{};
    colour_bytes(low, 0, 0, 0, 0);
    colour_bytes(low, 1, 15, 0, 0);
    colour_bytes(low, 2, 0, 15, 0);
    colour_bytes(low, 3, 5, 10, 15);
    Data high{};
    colour_bytes(high, 1, 15, 15, 15);  // colour 9
    const CdgFile file = parsed(packet(30, low) + packet(31, high) + preset(1) + border(2));
    CdgScreen screen(file);
    screen.seek(1000);
    CHECK(screen.applied() == 4);
    CHECK(screen.colour(0) == 0xFF000000U);
    CHECK(screen.colour(1) == 0xFFFF0000U);
    CHECK(screen.colour(2) == 0xFF00FF00U);
    // Each nibble is multiplied by 17, so that 15 is full.
    CHECK(screen.colour(3) == 0xFF55AAFFU);
    CHECK(screen.colour(9) == 0xFFFFFFFFU);

    CHECK(screen.index_at(150, 100) == 1);
    CHECK(screen.border() == 2);
    const auto pixels = screen.pixels();
    REQUIRE(pixels.size() == 300U * 216U);
    // The picture is red and the border green, and the border is the outer tile all round.
    CHECK(pixels[100 * 300 + 150] == 0xFFFF0000U);
    CHECK(pixels[0] == 0xFF00FF00U);
    CHECK(pixels[5] == 0xFF00FF00U);
    CHECK(pixels[6 + 12 * 300] == 0xFFFF0000U);
    CHECK(pixels[215 * 300 + 299] == 0xFF00FF00U);
    CHECK(pixels[(203 * 300) + 293] == 0xFFFF0000U);
    CHECK(pixels[(204 * 300) + 150] == 0xFF00FF00U);
}

TEST_CASE("cdg: a tile is two colours in six by twelve pixels, and its XOR flips what is there",
          "[lyrics][cdg]") {
    const CdgFile file = parsed(preset(2) + tile(3, 4, 2, 5, kStripes));
    CdgScreen screen(file);
    screen.seek(1000);
    // Row 2 and column 5 are at y = 24 and x = 30; the leftmost pixel of a row is its bit 5, so the
    // diagonal runs left to right and down.
    for (int line = 0; line < 12; ++line) {
        const int set = line % 6;
        for (int pixel = 0; pixel < 6; ++pixel) {
            INFO("line " << line << " pixel " << pixel);
            CHECK(screen.index_at(30 + pixel, 24 + line) == (pixel == set ? 4 : 3));
        }
    }
    // Its neighbours are as they were.
    CHECK(screen.index_at(29, 24) == 2);
    CHECK(screen.index_at(36, 24) == 2);
    CHECK(screen.index_at(30, 23) == 2);
    CHECK(screen.index_at(30, 36) == 2);

    // XOR: colour 1 where the bit is set and colour 0 where it is clear, onto what was there.
    const CdgFile twice = parsed(preset(2) + tile(0, 5, 0, 0, kStripes, true) + tile(0, 5, 0, 0, kStripes, true));
    CdgScreen flipped(twice);
    flipped.seek(1000);
    // Two of the same XOR leave the screen as it was.
    CHECK(flipped.index_at(0, 0) == 2);
    CHECK(flipped.index_at(5, 5) == 2);
    const CdgFile once = parsed(preset(2) + tile(0, 5, 0, 0, kStripes, true));
    CdgScreen one(once);
    one.seek(1000);
    CHECK(one.index_at(0, 0) == (2 ^ 5));
    CHECK(one.index_at(1, 0) == 2);

    // A tile that would not fit is not drawn (row 18 starts at y = 216, column 50 at x = 300).
    const CdgFile off = parsed(preset(2) + tile(3, 4, 18, 0, kStripes) + tile(3, 4, 0, 50, kStripes));
    CdgScreen outside(off);
    outside.seek(1000);
    CHECK(outside.index_at(0, 0) == 2);
    CHECK(outside.index_at(299, 215) == 2);
}

TEST_CASE("cdg: only a field's own bits count, and the last tile is the corner", "[lyrics][cdg]") {
    // The data bytes carry six bits: a colour is the low four, a row the low five.
    Data d{};
    d[0] = 0x30 | 5;
    const std::string high_preset = packet(1, d);
    d[0] = 0x30 | 10;
    const std::string high_border = packet(2, d);
    // Row 17, column 49 is the last tile, at x 294 to 299 and y 204 to 215; its row byte also has a
    // bit above the five, and colours 3 and 4 have theirs.
    const std::string corner = tile(0x30 | 3, 0x20 | 4, 0x20 | 17, 49, kStripes);
    const CdgFile file = parsed(high_preset + high_border + corner);
    CdgScreen screen(file);
    screen.seek(1000);
    CHECK(screen.index_at(100, 100) == 5);
    CHECK(screen.border() == 10);
    CHECK(screen.index_at(294, 204) == 4);
    CHECK(screen.index_at(295, 204) == 3);
    CHECK(screen.index_at(299, 215) == 4);
    CHECK(screen.index_at(293, 204) == 5);
    CHECK(screen.index_at(294, 203) == 5);
}

TEST_CASE("cdg: a scroll moves the picture by a tile, and a copy brings back what left", "[lyrics][cdg]") {
    const std::string drawn = preset(0) + tile(0, 7, 3, 0, kStripes);  // a tile at the left edge, x 0 to 5
    // Preset scroll, one tile to the right: the tile is at x 6 to 11, and the gap is the fill colour.
    const CdgFile right = parsed(drawn + scroll(false, 9, 1, 0));
    CdgScreen moved(right);
    moved.seek(1000);
    CHECK(moved.index_at(6, 36) == 7);
    CHECK(moved.index_at(0, 36) == 9);
    CHECK(moved.index_at(0, 0) == 9);

    // Copy scroll to the left: what leaves at the left edge comes back at the right.
    const CdgFile left = parsed(drawn + scroll(true, 9, 2, 0));
    CdgScreen wrapped(left);
    wrapped.seek(1000);
    CHECK(wrapped.index_at(294, 36) == 7);
    CHECK(wrapped.index_at(0, 36) == 0);

    // Down by a tile, with a preset: the tile at y 36 is at y 48 and the top row is the fill.
    const CdgFile down = parsed(drawn + scroll(false, 9, 0, 1));
    CdgScreen lower(down);
    lower.seek(1000);
    CHECK(lower.index_at(0, 48) == 7);
    CHECK(lower.index_at(0, 0) == 9);
    CHECK(lower.index_at(0, 11) == 9);

    // Up by a tile, copy: a tile at the top row wraps to the bottom.
    const CdgFile up = parsed(preset(0) + tile(0, 7, 0, 0, kStripes) + scroll(true, 0, 0, 2));
    CdgScreen top(up);
    top.seek(1000);
    CHECK(top.index_at(0, 204) == 7);
    CHECK(top.index_at(0, 0) == 0);
}

TEST_CASE("cdg: the picture follows the position, forward and back, 300 packets a second", "[lyrics][cdg]") {
    // Packet 0 presets colour 1, packet 300 (at one second) colour 2, packet 600 colour 3.
    std::string bytes;
    for (int i = 0; i < 601; ++i) {
        if (i == 0) {
            bytes += preset(1);
        } else if (i == 300) {
            bytes += preset(2);
        } else if (i == 600) {
            bytes += preset(3);
        } else {
            bytes += packet(0, Data{});  // nothing
        }
    }
    const CdgFile file = parsed(bytes);
    CHECK(file.duration_ms() == 2003);
    CdgScreen screen(file);
    CHECK(screen.index_at(100, 100) == 0);
    // Before the first packet's time, nothing has been sent; at 4 ms the first has.
    screen.seek(0);
    CHECK(screen.applied() == 0);
    screen.seek(4);
    CHECK(screen.applied() == 1);
    CHECK(screen.index_at(100, 100) == 1);
    // The packet at 300 is sent at one second, and applied by 1004 ms.
    screen.seek(999);
    CHECK(screen.index_at(100, 100) == 1);
    screen.seek(1004);
    CHECK(screen.index_at(100, 100) == 2);
    // A seek backwards replays from the start and lands where a seek from the start does.
    screen.seek(500);
    CHECK(screen.index_at(100, 100) == 1);
    screen.seek(1999);
    CHECK(screen.index_at(100, 100) == 2);
    screen.seek(2004);
    CHECK(screen.index_at(100, 100) == 3);
    // Before zero is zero, and past the end is the last packet.
    screen.seek(-5);
    CHECK(screen.applied() == 0);
    screen.seek(100000000);
    CHECK(screen.applied() == 601);
    CHECK(screen.index_at(100, 100) == 3);
}

TEST_CASE("cdg: the file beside an item, in any case, and read by its path", "[lyrics][cdg]") {
    const fs::path folder = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("cdg_" + iclforge::test::platform::process_id());
    fs::remove_all(folder);
    fs::create_directories(folder);
    const std::string bytes = preset(1) + preset(2);
    {
        std::ofstream out(folder / "song.mp3", std::ios::binary);
        out << "x";
        std::ofstream cdg(folder / "song.CDG", std::ios::binary);
        cdg.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const auto found = iclforge::lyrics::find_cdg_file((folder / "song.mp3").string());
    REQUIRE(found.has_value());
    std::string extension = found->extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(extension == ".cdg");
    const auto read = iclforge::lyrics::read_cdg(*found);
    REQUIRE(read.has_value());
    CHECK(read->packets.size() == 2);
    // An item with no file beside it has none, and a missing file says it could not be read.
    CHECK_FALSE(iclforge::lyrics::find_cdg_file((folder / "other.mp3").string()).has_value());
    CHECK_FALSE(iclforge::lyrics::read_cdg(folder / "missing.cdg").has_value());
    fs::remove_all(folder);
}
