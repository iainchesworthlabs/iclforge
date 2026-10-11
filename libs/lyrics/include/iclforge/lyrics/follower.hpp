#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/playhead.hpp"

// What a sink with no lyric file of its own holds to follow a song's words (planning/karaoke-phase2.md,
// W7d): the item's document, as the server sent it, and a playhead moved by the server's anchors. It
// answers the line and the word the sound is in at a time on the clock the anchors are in, which a
// screen on the sink draws, and which a sink without one reports as the line.
//
// Not thread-safe: the owner keeps it behind its own lock, as a board must (the session's task sets
// it and the poll reads it). Nothing here keeps a clock; the caller passes the time.

namespace iclforge::lyrics {

class Follower {
public:
    // The document of item `item`, an LRC text as a server writes it (UTF-8). It replaces what was
    // held: another item's lyrics and anchors go with it. An empty text, or one with no lyrics in it,
    // leaves the sink holding none (still for `item`, so that the anchors that follow are the item's
    // and are not mistaken for another's). True when lyrics are held; false with the reader's reason
    // otherwise, which is no fault when the text is empty.
    bool set_document(std::int64_t item, std::string_view text, std::size_t max_bytes = 0) {
        item_ = item;
        lyrics_.reset();
        playhead_.clear();
        if (text.empty() || (max_bytes != 0 && text.size() > max_bytes)) {
            return false;
        }
        auto parsed = parse_lyrics(text);
        if (!parsed) {
            return false;
        }
        lyrics_ = std::move(*parsed);
        return true;
    }

    // Where the sound is, for item `item`: ignored for any other (an anchor of the item that has
    // just ended, arriving after the next item's document, is not this one's). See Playhead::anchor.
    void anchor(std::int64_t item, std::int64_t position_ms, std::int64_t at_us, double speed) {
        if (item_ && *item_ == item) {
            playhead_.anchor(position_ms, at_us, speed);
        }
    }

    // A flush or an item that ended: the document stays and the position goes.
    void clear_position() { playhead_.clear(); }
    // Everything: nothing playing.
    void reset() {
        item_.reset();
        lyrics_.reset();
        playhead_.clear();
    }

    // The item the sink holds a document for, held or not.
    [[nodiscard]] std::optional<std::int64_t> item() const { return item_; }
    [[nodiscard]] bool has_lyrics() const { return lyrics_.has_value(); }
    [[nodiscard]] const Lyrics* lyrics() const { return lyrics_ ? &*lyrics_ : nullptr; }

    // Where in the item the sound is at `now_us`, once an anchor has come due.
    [[nodiscard]] std::optional<std::int64_t> position_ms(std::int64_t now_us) const {
        return playhead_.position_ms(now_us);
    }

    // The line and word at `now_us`: nothing while no lyrics are held, or before the first anchor
    // has come due. A file with no times has a position and no line.
    [[nodiscard]] std::optional<LyricPosition> cursor(std::int64_t now_us) const {
        if (!lyrics_) {
            return std::nullopt;
        }
        const std::optional<std::int64_t> position = playhead_.position_ms(now_us);
        if (!position) {
            return std::nullopt;
        }
        return lyric_position(*lyrics_, *position);
    }

    // What a sink reports: -1 before the first line (and for a file with no times), else the line's
    // index. Nothing while no lyrics are held.
    [[nodiscard]] std::optional<std::int32_t> line(std::int64_t now_us) const {
        if (!lyrics_) {
            return std::nullopt;
        }
        const std::optional<LyricPosition> at = cursor(now_us);
        if (!at || !at->line) {
            return -1;
        }
        return static_cast<std::int32_t>(*at->line);
    }

private:
    std::optional<std::int64_t> item_{};
    std::optional<Lyrics> lyrics_{};
    Playhead playhead_{};
};

}  // namespace iclforge::lyrics
