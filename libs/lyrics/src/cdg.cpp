#include "iclforge/lyrics/cdg.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace iclforge::lyrics {

namespace {

// The packets' command byte is the subcode channel's: 9 is CD+G.
constexpr std::uint8_t kCommandMask = 0x3F;
constexpr std::uint8_t kCdgCommand = 9;
constexpr std::uint8_t kInstructionMask = 0x3F;

constexpr std::uint8_t kMemoryPreset = 1;
constexpr std::uint8_t kBorderPreset = 2;
constexpr std::uint8_t kTileBlock = 6;
constexpr std::uint8_t kScrollPreset = 20;
constexpr std::uint8_t kScrollCopy = 24;
constexpr std::uint8_t kTransparentColour = 28;
constexpr std::uint8_t kLoadLow = 30;
constexpr std::uint8_t kLoadHigh = 31;
constexpr std::uint8_t kTileBlockXor = 38;

constexpr int kTileWidth = 6;
constexpr int kTileHeight = 12;
// The visible picture sits inside a border of one tile across and one tile down.
constexpr int kBorderX = kTileWidth;
constexpr int kBorderY = kTileHeight;

constexpr std::uintmax_t kMaxFileBytes = 64U * 1024U * 1024U;

[[nodiscard]] std::string lowercase(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

std::expected<CdgFile, std::string> parse_cdg(std::string_view bytes) {
    if (bytes.empty()) {
        return std::unexpected(std::string{"The CD+G file is empty."});
    }
    if (bytes.size() % kCdgPacketBytes != 0) {
        return std::unexpected(std::string{"The file is not CD+G: its length is not a whole number of packets."});
    }
    CdgFile file;
    file.packets.resize(bytes.size() / kCdgPacketBytes);
    std::size_t pictures = 0;
    for (std::size_t slot = 0; slot < file.packets.size(); ++slot) {
        const std::size_t at = slot * kCdgPacketBytes;
        const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(bytes[at + i]); };
        // A packet of one of the disc's other subcode channels is not a picture, but it takes its
        // place in time: it stays in the stream as a packet that does nothing (instruction 0), so
        // that a position maps to a packet by the clock alone.
        if ((byte(0) & kCommandMask) != kCdgCommand) {
            continue;
        }
        CdgPacket& packet = file.packets[slot];
        packet.instruction = static_cast<std::uint8_t>(byte(1) & kInstructionMask);
        for (std::size_t i = 0; i < packet.data.size(); ++i) {
            packet.data[i] = byte(4 + i);
        }
        ++pictures;
    }
    if (pictures == 0) {
        return std::unexpected(std::string{"The file has no CD+G packets in it."});
    }
    return file;
}

std::expected<CdgFile, std::string> read_cdg(const std::filesystem::path& file) {
    std::error_code sized;
    const std::uintmax_t size = std::filesystem::file_size(file, sized);
    if (sized) {
        return std::unexpected(std::string{"The CD+G file could not be read: "} + sized.message());
    }
    if (size > kMaxFileBytes) {
        return std::unexpected(std::string{"The CD+G file is too large to be a disc's subcode."});
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(std::string{"The CD+G file could not be opened."});
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty() && !in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(std::string{"The CD+G file could not be read."});
    }
    return parse_cdg(bytes);
}

std::optional<std::filesystem::path> find_cdg_file(const std::string& item_path) {
    const std::filesystem::path media{item_path};
    const std::filesystem::path folder = media.parent_path();
    const std::string stem = media.stem().string();
    if (stem.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    const std::filesystem::path usual = folder / (stem + ".cdg");
    if (std::filesystem::is_regular_file(usual, error) && !error) {
        return usual;
    }
    error.clear();
    std::filesystem::directory_iterator it(folder.empty() ? std::filesystem::path{"."} : folder,
                                           std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    while (!error && it != end) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !entry_error && it->path().stem().string() == stem &&
            lowercase(it->path().extension().string()) == ".cdg") {
            return it->path();
        }
        it.increment(error);
    }
    return std::nullopt;
}

CdgScreen::CdgScreen(const CdgFile& file) : file_(&file) {
    reset();
}

void CdgScreen::reset() {
    screen_.fill(0);
    table_.fill(0xFF000000U);
    border_ = 0;
    fine_x_ = 0;
    fine_y_ = 0;
    transparent_.reset();
    applied_ = 0;
}

void CdgScreen::seek(std::int64_t position_ms) {
    const std::int64_t clamped = std::max<std::int64_t>(position_ms, 0);
    const std::size_t due = std::min<std::size_t>(
        file_->packets.size(), static_cast<std::size_t>(clamped * kCdgPacketsPerSecond / 1000));
    if (due < applied_) {
        reset();
    }
    while (applied_ < due) {
        apply(file_->packets[applied_]);
        ++applied_;
    }
}

void CdgScreen::apply(const CdgPacket& packet) {
    const auto& data = packet.data;
    switch (packet.instruction) {
        case kMemoryPreset:
            screen_.fill(static_cast<std::uint8_t>(data[0] & 0x0F));
            break;
        case kBorderPreset:
            border_ = static_cast<std::uint8_t>(data[0] & 0x0F);
            break;
        case kTileBlock:
            tile(packet, false);
            break;
        case kTileBlockXor:
            tile(packet, true);
            break;
        case kScrollPreset:
            scroll(packet, false);
            break;
        case kScrollCopy:
            scroll(packet, true);
            break;
        case kTransparentColour:
            transparent_ = static_cast<std::uint8_t>(data[0] & 0x0F);
            break;
        case kLoadLow:
        case kLoadHigh: {
            const std::size_t base = packet.instruction == kLoadLow ? 0 : 8;
            for (std::size_t i = 0; i < 8; ++i) {
                // Two bytes of six bits a colour, four bits each of red, green and blue.
                const std::uint32_t high = data[2 * i] & 0x3FU;
                const std::uint32_t low = data[(2 * i) + 1] & 0x3FU;
                const std::uint32_t value = (high << 6U) | low;
                const std::uint32_t red = ((value >> 8U) & 0xFU) * 17U;
                const std::uint32_t green = ((value >> 4U) & 0xFU) * 17U;
                const std::uint32_t blue = (value & 0xFU) * 17U;
                table_[base + i] = 0xFF000000U | (red << 16U) | (green << 8U) | blue;
            }
            break;
        }
        default:
            // An instruction a disc may carry that a screen has no use for.
            break;
    }
}

void CdgScreen::tile(const CdgPacket& packet, bool xor_pixels) {
    const auto& data = packet.data;
    const std::uint8_t colour0 = data[0] & 0x0F;
    const std::uint8_t colour1 = data[1] & 0x0F;
    const int row = data[2] & 0x1F;
    const int column = data[3] & 0x3F;
    const int x0 = column * kTileWidth;
    const int y0 = row * kTileHeight;
    // A tile that would not fit is not drawn: the first row of the screen is row 0 and the last is 17.
    if (x0 + kTileWidth > kCdgWidth || y0 + kTileHeight > kCdgHeight) {
        return;
    }
    for (int line = 0; line < kTileHeight; ++line) {
        const std::uint8_t bits = data[4 + static_cast<std::size_t>(line)] & 0x3F;
        for (int pixel = 0; pixel < kTileWidth; ++pixel) {
            const bool set = ((bits >> (kTileWidth - 1 - pixel)) & 1U) != 0;
            const std::uint8_t colour = set ? colour1 : colour0;
            std::uint8_t& target =
                screen_[static_cast<std::size_t>((y0 + line) * kCdgWidth + x0 + pixel)];
            target = xor_pixels ? static_cast<std::uint8_t>(target ^ colour) : colour;
        }
    }
}

void CdgScreen::scroll(const CdgPacket& packet, bool copy) {
    const auto& data = packet.data;
    const std::uint8_t fill = data[0] & 0x0F;
    const int horizontal = (data[1] >> 4) & 3;
    const int vertical = (data[2] >> 4) & 3;
    // Every scroll command sets the display's fine offset, the low bits of the two direction bytes
    // (also when it moves no tile, which is how a disc scrolls a pixel at a time); the bits a value
    // past the largest offset spends are read as that largest.
    fine_x_ = std::min(data[1] & 0x07, kCdgMaxFineScrollX);
    fine_y_ = std::min(data[2] & 0x0F, kCdgMaxFineScrollY);
    // The command moves the picture by a whole tile: 1 is right or down, 2 is left or up.
    const int dx = horizontal == 1 ? kTileWidth : (horizontal == 2 ? -kTileWidth : 0);
    const int dy = vertical == 1 ? kTileHeight : (vertical == 2 ? -kTileHeight : 0);
    if (dx == 0 && dy == 0) {
        return;
    }
    const auto before = screen_;
    for (int y = 0; y < kCdgHeight; ++y) {
        for (int x = 0; x < kCdgWidth; ++x) {
            int sx = x - dx;
            int sy = y - dy;
            std::uint8_t value = fill;
            if (copy) {
                sx = ((sx % kCdgWidth) + kCdgWidth) % kCdgWidth;
                sy = ((sy % kCdgHeight) + kCdgHeight) % kCdgHeight;
                value = before[static_cast<std::size_t>(sy * kCdgWidth + sx)];
            } else if (sx >= 0 && sx < kCdgWidth && sy >= 0 && sy < kCdgHeight) {
                value = before[static_cast<std::size_t>(sy * kCdgWidth + sx)];
            }
            screen_[static_cast<std::size_t>(y * kCdgWidth + x)] = value;
        }
    }
}

std::uint8_t CdgScreen::index_at(int x, int y) const {
    if (x < 0 || x >= kCdgWidth || y < 0 || y >= kCdgHeight) {
        return 0;
    }
    return screen_[static_cast<std::size_t>(y * kCdgWidth + x)];
}

std::uint32_t CdgScreen::colour(int index) const {
    const std::size_t slot = static_cast<std::size_t>(index & 0x0F);
    const std::uint32_t entry = table_[slot];
    // Transparent keeps the table's red, green and blue and loses the alpha.
    return transparent_.has_value() && static_cast<std::size_t>(*transparent_) == slot ? (entry & 0x00FFFFFFU)
                                                                                         : entry;
}

std::vector<std::uint32_t> CdgScreen::pixels() const {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(kCdgWidth) * kCdgHeight);
    for (int y = 0; y < kCdgHeight; ++y) {
        for (int x = 0; x < kCdgWidth; ++x) {
            const bool in_border = x < kBorderX || x >= kCdgWidth - kBorderX || y < kBorderY ||
                                   y >= kCdgHeight - kBorderY;
            // The border is a solid colour; the picture inside it is shown from the fine offset on,
            // which stays inside the memory (at most 5 across and 11 down, in a border of 6 and 12).
            const std::uint8_t index =
                in_border ? border_
                          : screen_[static_cast<std::size_t>((y + fine_y_) * kCdgWidth + x + fine_x_)];
            out[static_cast<std::size_t>(y * kCdgWidth + x)] = colour(index);
        }
    }
    return out;
}

}  // namespace iclforge::lyrics
