#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// CD+G, the picture of a karaoke disc (planning/karaoke-phase2.md, W13f and decision D11): a `.cdg`
// file is the subcode channel of a CD ripped on its own, a stream of 24-byte packets, 300 of them
// a second, that draw a 300 by 216 picture in 16 colours. It carries NO TEXT: the words are pixels,
// and what a player can do is show the picture against the audio's position. That is all this does.
// Standard library only, like the rest of this library.
//
// The packets (a command byte of 9 in its low six bits, an instruction byte, two parity bytes,
// sixteen data bytes, four parity bytes) are the ones a karaoke player implements:
//
//   1  memory preset      fill the screen with a colour
//   2  border preset      the border (the outer 6 by 12 pixels) is a colour
//   6  tile block         a 6 by 12 pixel tile in two colours, from 12 rows of 6 bits
//  38  tile block XOR     the same, XORed onto what is there
//  20  scroll preset      scroll by a tile, filling the gap with a colour
//  24  scroll copy        scroll by a tile, the part that left coming back on the other side
//  28  transparent colour one of the 16 colours is left out of the picture, for video to show through
//  30  load colours 0-7   eight 12-bit colours      31  load colours 8-15
//
// Both scrolls also carry a fine offset in the low bits of their two direction bytes: the picture is
// shown shifted left by up to 5 pixels and up by up to 11, which is how a disc scrolls smoothly (a
// run of single-pixel offsets, then the whole-tile scroll and the offset back to 0). The offset is a
// state of the display, set by every scroll command and moving nothing in the screen memory.
//
// Parity is not checked (a rip has already been through the drive's error correction), a packet whose
// command is not 9 is a packet of another subcode channel and is skipped, and an instruction that is
// not listed is skipped, so a file that has a few of those is still a picture.

namespace iclforge::lyrics {

inline constexpr int kCdgWidth = 300;
inline constexpr int kCdgHeight = 216;
// The subcode runs at 75 sectors a second, each of four groups of packets: 300 packets a second.
inline constexpr int kCdgPacketsPerSecond = 300;
inline constexpr std::size_t kCdgPacketBytes = 24;
// The most a scroll command's fine offset moves the picture: 5 pixels left (a tile is 6 wide) and
// 11 up (a tile is 12 high). A larger value in the data is read as this, which keeps the shifted
// picture inside the screen memory.
inline constexpr int kCdgMaxFineScrollX = 5;
inline constexpr int kCdgMaxFineScrollY = 11;

struct CdgPacket {
    std::uint8_t instruction = 0;
    std::array<std::uint8_t, 16> data{};
};

// The packets of a file, in order, a packet of another subcode channel being one that does nothing
// (instruction 0): the length is what the picture lasts, and a packet's place is its time.
struct CdgFile {
    std::vector<CdgPacket> packets;

    [[nodiscard]] std::int64_t duration_ms() const {
        return static_cast<std::int64_t>(packets.size()) * 1000 / kCdgPacketsPerSecond;
    }
};

// A `.cdg` file's bytes. The error is a sentence for the page: a file that is not a whole number
// of packets, or has none, is refused.
[[nodiscard]] std::expected<CdgFile, std::string> parse_cdg(std::string_view bytes);

// A file's picture. A file over 64 MiB is refused, which no CD's subcode is (a disc is 80 minutes:
// 80 * 60 * 300 * 24 bytes is under 35 MiB).
[[nodiscard]] std::expected<CdgFile, std::string> read_cdg(const std::filesystem::path& file);

// The `name.cdg` beside a media item, the extension in any case (the MP3+G convention: a ripped
// disc is `name.mp3` and `name.cdg`). `item_path` is the item's path as the queue holds it.
[[nodiscard]] std::optional<std::filesystem::path> find_cdg_file(const std::string& item_path);

// The screen a stream of packets draws: indices into a table of 16 colours, 300 by 216, and the
// border's. It follows the packets up to a time; going back replays from the start, which is
// fast enough that nothing is kept (a whole disc is a few hundred thousand packets).
class CdgScreen {
public:
    // The screen is a view of `file`, which has to outlive it.
    explicit CdgScreen(const CdgFile& file);

    // Where the picture is at `position_ms`: every packet the disc has sent by then has been
    // applied, and none after. Nothing before 0 and nothing past the last packet.
    void seek(std::int64_t position_ms);

    // The pixels as 0xAARRGGBB, 300 by 216, row by row, as the disc's display shows them: the border
    // drawn in its colour, the picture inside it shifted by the fine scroll offset, and a pixel of
    // the transparent colour with an alpha of 0 (its red, green and blue are still the table's).
    [[nodiscard]] std::vector<std::uint32_t> pixels() const;
    // One pixel's colour index, 0 to 15, in the screen memory (not the border, and before the fine
    // scroll offset is applied: the offset is the display's, not the memory's).
    [[nodiscard]] std::uint8_t index_at(int x, int y) const;
    // A colour table entry as 0xAARRGGBB, an alpha of 0 for the transparent colour.
    [[nodiscard]] std::uint32_t colour(int index) const;
    [[nodiscard]] std::uint8_t border() const { return border_; }
    // The fine scroll offset the last scroll command set: the picture is shown that many pixels to
    // the left (0 to 5) and up (0 to 11) of the memory's.
    [[nodiscard]] int fine_scroll_x() const { return fine_x_; }
    [[nodiscard]] int fine_scroll_y() const { return fine_y_; }
    // The colour index instruction 28 made transparent, none until it is sent.
    [[nodiscard]] std::optional<std::uint8_t> transparent() const { return transparent_; }
    // How many packets have been applied.
    [[nodiscard]] std::size_t applied() const { return applied_; }

private:
    void apply(const CdgPacket& packet);
    void reset();
    void tile(const CdgPacket& packet, bool xor_pixels);
    void scroll(const CdgPacket& packet, bool copy);

    const CdgFile* file_;
    std::array<std::uint8_t, static_cast<std::size_t>(kCdgWidth) * kCdgHeight> screen_{};
    std::array<std::uint32_t, 16> table_{};
    std::uint8_t border_ = 0;
    int fine_x_ = 0;
    int fine_y_ = 0;
    std::optional<std::uint8_t> transparent_;
    std::size_t applied_ = 0;
};

}  // namespace iclforge::lyrics
