#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

// Where in an item the sound is, kept by something that is told where it was and when (planning/
// karaoke-phase2.md, W7d: a sink that runs the lyrics on its own playout clock). The server sends
// anchors, each "the sound is at `position_ms` at `at_us`, and moves at `speed` times real time from
// there": 1 while the item plays, 0 held. The playhead answers a position for any time on the same
// clock, and the lyric cursor (lyrics.hpp) turns that position into a line and a word.
//
// An anchor applies when the clock reaches its time and not before, so a hold the server sends for
// the end of what it has already sent does not stop the words early, and a seek's new anchor does not
// start them before the sound does. Standard library only, no clock of its own and no allocation: the
// caller passes the time, so a test sets it and a board reads its own.

namespace iclforge::lyrics {

class Playhead {
public:
    // The most anchors kept waiting or in force. A server sends one when something changes and one a
    // few seconds to correct drift, and each is applied within a few seconds of arriving, so a handful
    // is plenty; the oldest is dropped past it, which is the one already in force and so is never
    // missed unless the sender has run far ahead of the clock.
    static constexpr std::size_t kMaxAnchors = 16;

    // The sound is at `position_ms` at `at_us`, and moves at `speed` from there. A time before an
    // earlier anchor's goes before it, and one equal to an earlier anchor's replaces it.
    void anchor(std::int64_t position_ms, std::int64_t at_us, double speed) {
        const Anchor added{position_ms, at_us, speed < 0.0 ? 0.0 : speed};
        // Where it goes: after every anchor at or before its time, unless one has exactly its time,
        // which it replaces.
        std::size_t at = 0;
        while (at < size_ && anchors_[at].at_us < added.at_us) {
            ++at;
        }
        if (at < size_ && anchors_[at].at_us == added.at_us) {
            anchors_[at] = added;
            return;
        }
        if (size_ == kMaxAnchors) {
            // Full: drop the oldest, and the position of the new one shifts down with it.
            std::move(anchors_.begin() + 1, anchors_.begin() + static_cast<std::ptrdiff_t>(size_), anchors_.begin());
            --size_;
            at = at > 0 ? at - 1 : 0;
        }
        std::move_backward(anchors_.begin() + static_cast<std::ptrdiff_t>(at),
                           anchors_.begin() + static_cast<std::ptrdiff_t>(size_),
                           anchors_.begin() + static_cast<std::ptrdiff_t>(size_) + 1);
        anchors_[at] = added;
        ++size_;
    }

    // Forgets every anchor: a flush, a seek that restarts the stream, an item that ended.
    void clear() { size_ = 0; }

    [[nodiscard]] bool empty() const { return size_ == 0; }

    // The position at `now_us`, never below 0: that of the last anchor whose time has come, moved on
    // by the time since at its speed. Nothing before the first anchor's time.
    [[nodiscard]] std::optional<std::int64_t> position_ms(std::int64_t now_us) const {
        const Anchor* in_force = nullptr;
        for (std::size_t i = 0; i < size_; ++i) {
            if (anchors_[i].at_us <= now_us) {
                in_force = &anchors_[i];
            }
        }
        if (in_force == nullptr) {
            return std::nullopt;
        }
        const double since_ms = static_cast<double>(now_us - in_force->at_us) / 1000.0;
        const double position = static_cast<double>(in_force->position_ms) + since_ms * in_force->speed;
        return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(position)));
    }

private:
    struct Anchor {
        std::int64_t position_ms = 0;
        std::int64_t at_us = 0;
        double speed = 1.0;
    };

    std::array<Anchor, kMaxAnchors> anchors_{};
    std::size_t size_ = 0;
};

}  // namespace iclforge::lyrics
