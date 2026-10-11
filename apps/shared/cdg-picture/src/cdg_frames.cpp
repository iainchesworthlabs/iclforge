#include "cdg_frames.hpp"

#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "iclforge/lyrics/cdg.hpp"

namespace iclforge::apps {

using iclforge::lyrics::CdgFile;
using iclforge::lyrics::CdgScreen;
using iclforge::lyrics::kCdgHeight;
using iclforge::lyrics::kCdgWidth;

struct CdgFrames::State {
    // The screen holds a pointer to the file, which is never moved once it is in here (the State
    // itself is heap-allocated and only the pointer to it moves), and goes before it.
    std::optional<CdgFile> file;
    std::unique_ptr<CdgScreen> screen;
    // The last image handed out and how many packets it had applied.
    QImage frame;
    std::size_t frame_applied = 0;

    void reset() {
        screen.reset();
        file.reset();
        frame = QImage{};
        frame_applied = 0;
    }

    void adopt(CdgFile disc) {
        reset();
        file.emplace(std::move(disc));
        screen = std::make_unique<CdgScreen>(*file);
    }
};

CdgFrames::CdgFrames() : state_(std::make_unique<State>()) {}

CdgFrames::~CdgFrames() = default;

CdgFrames::CdgFrames(CdgFrames&&) noexcept = default;

CdgFrames& CdgFrames::operator=(CdgFrames&&) noexcept = default;

std::expected<void, std::string> CdgFrames::load(const std::filesystem::path& file) {
    state_->reset();
    auto read = iclforge::lyrics::read_cdg(file);
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    state_->adopt(std::move(*read));
    return {};
}

std::expected<void, std::string> CdgFrames::load_bytes(std::string_view bytes) {
    state_->reset();
    auto read = iclforge::lyrics::parse_cdg(bytes);
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    state_->adopt(std::move(*read));
    return {};
}

void CdgFrames::clear() {
    state_->reset();
}

bool CdgFrames::loaded() const {
    return state_->screen != nullptr;
}

std::int64_t CdgFrames::duration_ms() const {
    return state_->file ? state_->file->duration_ms() : 0;
}

std::optional<std::filesystem::path> find_cdg_beside(const std::string& item_path) {
    return iclforge::lyrics::find_cdg_file(item_path);
}

QImage CdgFrames::frame(std::int64_t position_ms) {
    if (!state_->screen) {
        return {};
    }
    CdgScreen& screen = *state_->screen;
    screen.seek(position_ms);
    if (!state_->frame.isNull() && screen.applied() == state_->frame_applied) {
        return state_->frame;
    }
    const std::vector<std::uint32_t> pixels = screen.pixels();
    QImage image(kCdgWidth, kCdgHeight, QImage::Format_ARGB32);
    for (int y = 0; y < kCdgHeight; ++y) {
        std::memcpy(image.scanLine(y),
                    pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(kCdgWidth),
                    static_cast<std::size_t>(kCdgWidth) * sizeof(std::uint32_t));
    }
    state_->frame = image;
    state_->frame_applied = screen.applied();
    return state_->frame;
}

}  // namespace iclforge::apps
