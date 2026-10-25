#pragma once

// The screen a panel shows while a song with no lyrics plays (planning/p4x-now-playing.md): which
// view the owner chose, when it dims, and the words it says about the stream. Standard library only, so
// that the host tests it (libs/device/tests/test_now_view.cpp): the words are what PUT /now-playing and
// PUT /dim-after take and GET /status's screen reports, and what NVS holds is a number that must never
// be renumbered.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iclforge::nowview {

// --- the view -------------------------------------------------------------------------------------

// The stored values are these numbers: never renumbered, never reused.
enum class View : std::uint8_t {
    kSpeakers = 0,  // the layout seen from above, each speaker glowing with its level
    kTitle = 1,     // title, artist, album and a progress line (needs the server to send them)
    kClock = 2,     // the time (needs a clock the board has to be given)
    kOff = 3,       // the screen this replaced: one sentence
    kDark = 4,      // nothing lit: the backlight off while a song has no lyrics, a touch lights the speakers for a while
};

inline constexpr View kDefaultView = View::kSpeakers;

[[nodiscard]] constexpr const char* view_name(View view) {
    switch (view) {
        case View::kTitle:
            return "title";
        case View::kClock:
            return "clock";
        case View::kOff:
            return "off";
        case View::kDark:
            return "dark";
        case View::kSpeakers:
            break;
    }
    return "speakers";
}

namespace detail {

[[nodiscard]] constexpr std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace detail

// One of the five words, with or without a line ending after it.
[[nodiscard]] constexpr std::optional<View> parse_view(std::string_view text) {
    text = detail::trimmed(text);
    for (const View view : {View::kSpeakers, View::kTitle, View::kClock, View::kOff, View::kDark}) {
        if (text == view_name(view)) {
            return view;
        }
    }
    return std::nullopt;
}

// A stored number, or nothing for one this build does not know (a partition written by another build).
[[nodiscard]] constexpr std::optional<View> view_from_stored(std::int32_t value) {
    if (value >= static_cast<std::int32_t>(View::kSpeakers) && value <= static_cast<std::int32_t>(View::kDark)) {
        return static_cast<View>(value);
    }
    return std::nullopt;
}

// --- the speakers' style --------------------------------------------------------------------------------

// How the speakers view shows a level (planning/p4x-speakers-options.md). The stored values are these
// numbers: never renumbered, never reused.
enum class SpeakersStyle : std::uint8_t {
    kGlow = 0,   // each speaker glows with its level: the fill and a halo in steps
    kPulse = 1,  // the same, and the filled circle grows and shrinks a little with the level
};

inline constexpr SpeakersStyle kDefaultSpeakersStyle = SpeakersStyle::kGlow;

[[nodiscard]] constexpr const char* speakers_style_name(SpeakersStyle style) {
    switch (style) {
        case SpeakersStyle::kPulse:
            return "pulse";
        case SpeakersStyle::kGlow:
            break;
    }
    return "glow";
}

// One of the two words, with or without a line ending after it.
[[nodiscard]] constexpr std::optional<SpeakersStyle> parse_speakers_style(std::string_view text) {
    text = detail::trimmed(text);
    for (const SpeakersStyle style : {SpeakersStyle::kGlow, SpeakersStyle::kPulse}) {
        if (text == speakers_style_name(style)) {
            return style;
        }
    }
    return std::nullopt;
}

// A stored number, or nothing for one this build does not know (a partition written by another build).
[[nodiscard]] constexpr std::optional<SpeakersStyle> speakers_style_from_stored(std::int32_t value) {
    if (value >= static_cast<std::int32_t>(SpeakersStyle::kGlow) && value <= static_cast<std::int32_t>(SpeakersStyle::kPulse)) {
        return static_cast<SpeakersStyle>(value);
    }
    return std::nullopt;
}

// --- dimming ----------------------------------------------------------------------------------------

inline constexpr int kDimNever = 0;
inline constexpr int kDimMaxMinutes = 240;
inline constexpr int kDefaultDimMinutes = 5;

// "never", or a whole number of minutes from 1 to kDimMaxMinutes (0 is spelled "never").
[[nodiscard]] constexpr std::optional<int> parse_dim_after(std::string_view text) {
    text = detail::trimmed(text);
    if (text == "never") {
        return kDimNever;
    }
    if (text.empty() || text.size() > 3) {
        return std::nullopt;
    }
    int minutes = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        minutes = minutes * 10 + (c - '0');
    }
    if (minutes < 1 || minutes > kDimMaxMinutes) {
        return std::nullopt;
    }
    return minutes;
}

[[nodiscard]] inline std::string dim_after_text(int minutes) {
    return minutes <= kDimNever ? std::string("never") : std::to_string(minutes);
}

// Whether the screen has dimmed: the no-lyrics view is the one showing, a time is set, and nothing has
// touched the glass for that long. A touch ends it, by being activity.
[[nodiscard]] constexpr bool dimmed(bool no_lyrics_view_showing, int minutes, std::uint32_t inactive_ms) {
    return no_lyrics_view_showing && minutes > kDimNever &&
           static_cast<std::uint64_t>(inactive_ms) >= static_cast<std::uint64_t>(minutes) * 60'000U;
}

// --- the dark view ----------------------------------------------------------------------------------------

// How long a touch lights the dark view's screen, with nothing touched since, before it goes dark again.
inline constexpr std::uint32_t kDarkAwakeMs = 30'000;

// What the no-lyrics screen is doing with the glass.
enum class Dim : std::uint8_t {
    kNone,    // lit
    kDimmed,  // the speakers view, left alone for the dim time: a black cover, the backlight at its floor
    kDark,    // the dark view, not touched lately: a black cover, the backlight off
};

// What the screen does now. `showing` is that the no-lyrics view is what the panel is on (the speakers
// built, or the dark view chosen while a song has no lyrics); `inactive_ms` is the time since anything
// touched the glass, and `showing_ms` the time since the view began to show.
//
// The speakers view dims after `minutes` of both. The dark view is dark from the moment it begins to
// show, whatever touched the glass before; a touch after that lights it, for kDarkAwakeMs from the last
// touch. A song that begins on a panel nobody has touched for hours is dark at once in the dark view and
// is not dimmed at once in the speakers view, which counts from the song too.
[[nodiscard]] constexpr Dim dim_state(View view, bool showing, int minutes, std::uint32_t inactive_ms,
                                      std::uint32_t showing_ms) {
    if (!showing) {
        return Dim::kNone;
    }
    if (view == View::kDark) {
        const bool touched_since_it_began = inactive_ms < showing_ms;
        return touched_since_it_began && inactive_ms < kDarkAwakeMs ? Dim::kNone : Dim::kDark;
    }
    const std::uint32_t quiet = inactive_ms < showing_ms ? inactive_ms : showing_ms;
    return dimmed(true, minutes, quiet) ? Dim::kDimmed : Dim::kNone;
}

// --- what the stream is called ------------------------------------------------------------------------

// kUnknown is a coded stream before its first unit has decoded.
enum class Codec : std::uint8_t { kPcm, kAc3, kEac3, kAc4, kUnknown };

// What the decoder found in the stream, as the player reports it (the sink's DecoderReport).
struct StreamFacts {
    Codec codec = Codec::kPcm;
    // The decoder has decoded a unit and says what it found: PCM has no coded layout, and a stream
    // before its first unit has not been looked at.
    bool have_decoder = false;
    // The coded audio coding mode, 0 to 7 (A/52 Table 5.8).
    int acmod = 0;
    bool lfe = false;
    // Objects the stream carries.
    int objects = 0;

    friend constexpr bool operator==(const StreamFacts&, const StreamFacts&) = default;
};

[[nodiscard]] constexpr const char* codec_name(Codec codec) {
    switch (codec) {
        case Codec::kAc3:
            return "Dolby Digital";
        case Codec::kEac3:
            return "Dolby Digital Plus";
        case Codec::kAc4:
            return "Dolby AC-4";
        case Codec::kUnknown:
            return "Coded audio";
        case Codec::kPcm:
            break;
    }
    return "PCM";
}

// The full-bandwidth channels of an audio coding mode: 1+1, 1/0, 2/0, 3/0, 2/1, 3/1, 2/2 and 3/2.
[[nodiscard]] constexpr int acmod_channels(int acmod) {
    constexpr int kChannels[8] = {2, 1, 2, 3, 3, 4, 4, 5};
    return acmod >= 0 && acmod < 8 ? kChannels[acmod] : 0;
}

// "Dolby Digital Plus 5.1", "Dolby Digital Plus 5.1 + 15 objects", "Dolby Digital 2.0", "PCM". The coded
// layout of the stream, which is not the room's: heading() says both.
[[nodiscard]] inline std::string stream_name(const StreamFacts& facts) {
    std::string out = codec_name(facts.codec);
    if (!facts.have_decoder || facts.codec == Codec::kPcm) {
        return out;
    }
    if (facts.acmod == 0) {
        out += " dual mono";
    } else if (const int channels = acmod_channels(facts.acmod); channels > 0) {
        out += " " + std::to_string(channels) + (facts.lfe ? ".1" : ".0");
    }
    if (facts.objects > 0) {
        out += " + " + std::to_string(facts.objects) + (facts.objects == 1 ? " object" : " objects");
    }
    return out;
}

// The room's name: a layout written as a name ("5.1", "7.1.4", "7.1.4:top") as it is, and anything else (a
// list of speakers, which is too long to say) as a count of outputs.
[[nodiscard]] inline std::string room_name(std::string_view layout, std::size_t outputs) {
    layout = detail::trimmed(layout);
    bool named = !layout.empty() && layout.size() <= 12 && layout.front() >= '0' && layout.front() <= '9';
    for (const char c : layout) {
        named = named && ((c >= '0' && c <= '9') || c == '.' || c == ':' || (c >= 'a' && c <= 'z'));
    }
    if (named) {
        return std::string(layout);
    }
    return outputs == 0 ? std::string() : std::to_string(outputs) + (outputs == 1 ? " output" : " outputs");
}

// The line over the speakers: what is arriving and what it is played on.
[[nodiscard]] inline std::string heading(const StreamFacts& facts, std::string_view layout, std::size_t outputs) {
    const std::string stream = stream_name(facts);
    const std::string room = room_name(layout, outputs);
    return room.empty() ? stream : stream + " to " + room;
}

// --- who sends it, how fast, and where it is decoded -----------------------------------------------------

// The bitrate of the coded stream being played, in kb/s: the bytes of coded audio the player has played
// (a total that only grows within a stream) over the time they took, smoothed. Not the figure in a header:
// what actually arrived. Fed a few times a second; it measures over windows of at least kWindowMs and
// moves a third of the way to each one, so a variable-rate stream settles and a burst of network does not
// show. A total that goes down is a new stream and starts the meter again.
class BitrateMeter {
   public:
    static constexpr std::uint32_t kWindowMs = 1000;

    void observe(std::uint64_t total_bytes, std::uint32_t now_ms) {
        if (!started_ || total_bytes < last_bytes_ || now_ms < last_ms_) {
            started_ = true;
            last_bytes_ = total_bytes;
            last_ms_ = now_ms;
            kbps_ = 0.0;
            have_ = false;
            return;
        }
        const std::uint32_t elapsed = now_ms - last_ms_;
        if (elapsed < kWindowMs) {
            return;
        }
        const std::uint64_t bytes = total_bytes - last_bytes_;
        last_bytes_ = total_bytes;
        last_ms_ = now_ms;
        if (bytes == 0) {
            // Nothing played in the window (paused, or between songs): the figure is not kept, it is unknown.
            kbps_ = 0.0;
            have_ = false;
            return;
        }
        // bits per millisecond is kb/s.
        const double window = static_cast<double>(bytes) * 8.0 / static_cast<double>(elapsed);
        kbps_ = have_ ? kbps_ + (window - kbps_) / 3.0 : window;
        have_ = true;
    }

    // 0 until a window has closed with audio in it.
    [[nodiscard]] int kbps() const { return have_ ? static_cast<int>(kbps_ + 0.5) : 0; }

    void reset() { *this = BitrateMeter(); }

   private:
    bool started_ = false;
    bool have_ = false;
    std::uint64_t last_bytes_ = 0;
    std::uint32_t last_ms_ = 0;
    double kbps_ = 0.0;
};

// What a server calls itself in /status is "Music Assistant (d5369777-music-assistant)": the name, and the
// id it connects with in brackets. The screen says the name; the developer view keeps the whole string.
[[nodiscard]] inline std::string server_label(std::string_view server) {
    server = detail::trimmed(server);
    if (!server.empty() && server.back() == ')') {
        const std::size_t open = server.rfind(" (");
        if (open != std::string_view::npos && open > 0) {
            return std::string(detail::trimmed(server.substr(0, open)));
        }
    }
    return std::string(server);
}

// The line the footer says about a stream, from the most to the least it can say; the screen takes the
// first that fits its width. A coded stream is decoded on this board (the server sent it undecoded), and a
// PCM one was decoded by the server.
//   "Decoded on this board - 768 kb/s - from Music Assistant", "Decoded on this board - 768 kb/s",
//   "Decoded here - 768 kb/s - from Music Assistant", "Decoded here - 768 kb/s", "768 kb/s"; "PCM from Music
//   Assistant", "PCM".
[[nodiscard]] inline std::vector<std::string> stream_info_options(const StreamFacts& facts, int kbps,
                                                                  std::string_view server) {
    const std::string who = server_label(server);
    std::vector<std::string> out;
    if (facts.codec == Codec::kPcm) {
        out.push_back(who.empty() ? std::string("PCM") : "PCM from " + who);
        out.push_back("PCM");
        return out;
    }
    const std::string rate = kbps > 0 ? std::to_string(kbps) + " kb/s" : std::string();
    const auto join = [&](const char* head) {
        std::string line = head;
        if (!rate.empty()) {
            line += " - " + rate;
        }
        return line;
    };
    if (!who.empty()) {
        out.push_back(join("Decoded on this board") + " - from " + who);
        out.push_back(join("Decoded here") + " - from " + who);
    }
    out.push_back(join("Decoded on this board"));
    out.push_back(join("Decoded here"));
    if (!rate.empty()) {
        out.push_back(rate);
    }
    return out;
}

}  // namespace iclforge::nowview
