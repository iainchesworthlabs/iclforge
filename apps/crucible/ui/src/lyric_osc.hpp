#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Events out (planning/karaoke-phase2.md, D9's rejected alternative, built on the owner's ask): what
// the lyric strip follows, said over OSC 1.0 on UDP, so that a visual rig, a show-control desk or a
// lighting console can follow the same line and word the strip does. Off until a person turns it
// on and says where to send.
//
// Everything here is Qt-free and has no clock of its own, so it is held by plain Catch2 cases
// (test_lyric_osc.cpp): the wire form of a message, and which messages one application's change
// from one reading to the next makes. The socket is iclforge::audio::UdpSender; the timer and the
// settings are the controller's.
//
// The address layout (every message begins with the application's name, so a rig can tell which of
// the room's applications a line is from):
//
//   /crucible/lyrics/state     ,ss     app, state            "following" or "stopped"; "cleared" when
//                                                            its lyrics were dropped
//   /crucible/lyrics/line      ,sisii  app, line, text,      the line being sung changed; line -1 and
//                                      startMs, endMs        no text when the song is between lines
//   /crucible/lyrics/word      ,siisii app, line, word,      the word being sung changed (a word-timed
//                                      text, startMs, endMs  line only)
//   /crucible/lyrics/position  ,si     app, positionMs       the song's position, ten times a second,
//                                                            while it is followed (off by default)
//
// Times are milliseconds from the start of the song, integers; a string is UTF-8 (OSC 1.0 says
// ASCII, and the desks that matter carry UTF-8), cut at 480 bytes on a character boundary so that a
// datagram stays under an Ethernet frame.

namespace iclforge::crucible::ui {

// One OSC argument: an int32 (type tag `i`), a float32 (`f`) or a string (`s`).
struct OscArg {
    enum class Type : std::uint8_t { kInt, kFloat, kString };

    Type type = Type::kInt;
    std::int32_t integer = 0;
    float real = 0.0F;
    std::string text;

    [[nodiscard]] static OscArg of(std::int32_t value) {
        OscArg out;
        out.type = Type::kInt;
        out.integer = value;
        return out;
    }
    [[nodiscard]] static OscArg of(float value) {
        OscArg out;
        out.type = Type::kFloat;
        out.real = value;
        return out;
    }
    [[nodiscard]] static OscArg of(std::string value) {
        OscArg out;
        out.type = Type::kString;
        out.text = std::move(value);
        return out;
    }
};

struct OscMessage {
    std::string address;
    std::vector<OscArg> args;
};

// The longest a string argument is, in bytes, before it is cut.
inline constexpr std::size_t kOscMaxStringBytes = 480;

// `text` cut to kOscMaxStringBytes at a UTF-8 character boundary, with any NUL removed (a NUL would
// end the string on the wire).
[[nodiscard]] std::string osc_string(std::string_view text);

// The OSC 1.0 form of one message: the address and the type tag string (",sisii"), each a
// NUL-terminated string padded to a multiple of four bytes, then the arguments in big-endian order
// with strings padded the same way.
[[nodiscard]] std::vector<std::byte> encode_osc(const OscMessage& message);

// Which events are sent. State and line are the useful default; the word and the position are for a
// rig that animates.
struct LyricEventOptions {
    bool state = true;
    bool lines = true;
    bool words = true;
    // The position message, at `position_period_ms`.
    bool position = false;
    std::int64_t position_period_ms = 100;
};

// One application's place in its lyrics at one instant, as the controller reads it. `app` is what
// the room calls it; `key` tells one application from another.
struct LyricSnapshot {
    int key = 0;
    std::string app;
    // Whether the strip is following it (a media session or Start) and the lyrics are timed.
    bool following = false;
    // -1 before the first line and between lines.
    int line = -1;
    std::string line_text;
    std::int64_t line_start_ms = 0;
    std::int64_t line_end_ms = 0;
    // -1 for no word (a line-timed line has none).
    int word = -1;
    std::string word_text;
    std::int64_t word_start_ms = 0;
    std::int64_t word_end_ms = 0;
    // The song's position, -1 when it is not followed.
    std::int64_t position_ms = -1;
};

// Turns readings into messages: each application's reading is compared to its last, and what
// changed is said once. Applications that are not in a reading have gone (their lyrics were
// cleared, or the application left), and are said to be "cleared". Time is an argument.
class LyricEventTracker {
public:
    explicit LyricEventTracker(LyricEventOptions options = {}) : options_(options) {}

    void set_options(const LyricEventOptions& options) { options_ = options; }
    [[nodiscard]] const LyricEventOptions& options() const { return options_; }

    // The messages that this reading of every application's snapshot makes at `now_ms`.
    [[nodiscard]] std::vector<OscMessage> update(const std::vector<LyricSnapshot>& readings, std::int64_t now_ms);

    // Forgets what was said, so that the next reading is said in full again (a new destination).
    void reset() { last_.clear(); }

private:
    struct Last {
        LyricSnapshot snapshot;
        std::int64_t position_sent_ms = 0;
        bool position_sent = false;
    };

    LyricEventOptions options_;
    std::map<int, Last> last_;
};

}  // namespace iclforge::crucible::ui
