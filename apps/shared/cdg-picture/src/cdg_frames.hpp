#pragma once

#include <QImage>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// The pictures of one CD+G disc, as images by position (planning/karaoke-phase2.md, W13f and
// decision D11): a `.cdg` file read once, and the screen that follows its packets, so that a
// program with a disc to show asks "what is on screen at this position" and is handed a QImage.
// Hearth's lyric view, Crucible's lyric strip and the Forge GUI's player all show a disc this way,
// and none of them owns the format: that is libs/lyrics' `cdg.hpp`, which this wraps.
//
// Qt Gui only (QImage), no QObject, so that a controller's own tests can hold one without Qt Quick.
// Nothing here owns a clock: the picture is a function of the position it is asked for, so a
// pause and a seek are only another position, and the screen replays from the start when the
// position goes back.
namespace iclforge::apps {

class CdgFrames {
public:
    CdgFrames();
    ~CdgFrames();

    CdgFrames(const CdgFrames&) = delete;
    CdgFrames& operator=(const CdgFrames&) = delete;
    CdgFrames(CdgFrames&&) noexcept;
    CdgFrames& operator=(CdgFrames&&) noexcept;

    // A `.cdg` file, replacing any disc held. The error is the reader's sentence for the page (a
    // file that is not a whole number of packets, one with none, one too large); nothing is held
    // after it.
    [[nodiscard]] std::expected<void, std::string> load(const std::filesystem::path& file);
    // The same from bytes already in hand.
    [[nodiscard]] std::expected<void, std::string> load_bytes(std::string_view bytes);
    // Forgets the disc and the last picture.
    void clear();

    [[nodiscard]] bool loaded() const;
    // How long the disc's packets last, in milliseconds; 0 when nothing is loaded.
    [[nodiscard]] std::int64_t duration_ms() const;

    // The picture at `position_ms`: 300 by 216, the border drawn, 0xAARRGGBB. Every packet the
    // disc has sent by then has been applied, and none after; a position before 0 is the start.
    // A null image when no disc is loaded. The image of the last call is returned again, without
    // a copy of the pixels, while the position has not reached another packet (they come 300 a
    // second and a view asks 60).
    [[nodiscard]] QImage frame(std::int64_t position_ms);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// The `name.cdg` beside a media item (the MP3+G convention: a ripped disc is `name.mp3` and
// `name.cdg`), the extension in any case, or nullopt. `item_path` is the item's path as the
// program holds it. A thin wrapper on the library's find_cdg_file, so that a program with a disc
// to show needs this project and not `lyrics` as well.
[[nodiscard]] std::optional<std::filesystem::path> find_cdg_beside(const std::string& item_path);

}  // namespace iclforge::apps
