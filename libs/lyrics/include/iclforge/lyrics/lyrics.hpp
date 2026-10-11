#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Lyrics for karaoke playback (planning/karaoke.md, requirements L1 to L6): a
// `.lrc` file read into lines of timed words, and a stateless cursor that says
// where in them a playing position is. No Qt in it, and nothing but the
// standard library, so that Hearth's window, Crucible, the sink firmware and
// the browser can all link it (planning/karaoke-phase2.md, W13a); a controller
// reads a file with read_lyrics() and a view asks lyric_position() on every
// frame.
//
// The format is LRC, which is informal, so what is read is stated here:
//
//   [mm:ss.xx]text        a line, shown when its time is reached; `.xx` is
//                         hundredths, `.x` tenths and `.xxx` milliseconds, and
//                         `:xx` is read as `.xx`; seconds are 00 to 59
//   [t1][t2]text          the same text at several times (a repeated chorus)
//   <mm:ss.xx>word        the enhanced form: a word starts at its tag. A word
//                         runs to the next word's tag; a tag with no text after
//                         it ends the word before it. A line's tags are never
//                         shown as text
//   [offset:+/-ms]        shifts every time: a positive value makes the lyrics
//                         appear sooner, as the format has always meant it
//   [ti:] [ar:] [al:] [by:] [length:mm:ss]   kept for display; other id tags
//                         are read and ignored
//   V1:text  V2:text  V1+V2:text
//                         a duet's singer for the line (planning/karaoke-phase2.md,
//                         W11): the marker is the first thing after the line's time
//                         tags (or after its first word tag), in either case, with
//                         spaces round the `+` and after the colon, and is not
//                         part of the line's text. V1 and V2 are the two vocals of
//                         an Annex C karaoke stream. A line with no marker is for
//                         nobody in particular. Anything else before a colon is
//                         text: "V3:" is a word
//   [v1:name] [v2:name]   what to call the two singers; a person's typed names
//                         come first
//
// A line with time tags and no word tags is word-timed nowhere: it is a line
// with times, and no word times are invented for it. A file with no time tags
// at all is plain text, `timed` false, and is not followed. In a timed file a
// line with no time tag is not shown (credits and notes often look like that).
// The text is UTF-8, with or without a byte-order mark, or UTF-16 with a
// byte-order mark in either order (what an editor saves as "Unicode", and so
// what a Hebrew or Yiddish file often is); anything else - Latin-1, Windows-
// 1255, UTF-32 - is an error with a reason, not a guess. What is returned is
// always UTF-8.
//
// A word's text keeps the spacing written after it, so the words of a line
// joined are the line's text.

namespace iclforge::lyrics {

struct LyricWord {
    // Milliseconds from the start of the item, as the file says after its
    // `[offset:]`, and at least the line's start.
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string text{};
};

// Which singer of a duet sings a line: the first vocal (V1), the second (V2), or both of
// them together. kUnmarked is a line that says nothing, which a solo has all of.
enum class Singer : std::uint8_t { kUnmarked, kFirst, kSecond, kBoth };

struct LyricLine {
    std::int64_t start_ms = 0;
    // The next line's start, or for the last line the item's duration when it
    // is known, its `[length:]`, or five seconds after it starts.
    std::int64_t end_ms = 0;
    // The line without its tags. Empty for a line that only marks a time (an
    // instrumental gap, or the end of the last line).
    std::string text{};
    // Empty unless the line carried word tags.
    std::vector<LyricWord> words{};
    // From a `V1:`, `V2:` or `V1+V2:` marker at the start of the line.
    Singer singer = Singer::kUnmarked;

    [[nodiscard]] bool word_timed() const { return !words.empty(); }
};

struct Lyrics {
    std::string title{};
    std::string artist{};
    std::string album{};
    std::string author{};
    // The `[v1:]` and `[v2:]` id tags: what the file calls its two singers.
    std::string first_singer_name{};
    std::string second_singer_name{};
    std::optional<std::int64_t> length_ms{};
    // The file's `[offset:]`, already applied to every time below.
    std::int64_t offset_ms = 0;
    // False for plain text: `lines` are then text only and not followed.
    bool timed = false;
    // In time order, stable among equal times.
    std::vector<LyricLine> lines{};
};

// `text` as the file holds it (its bytes, UTF-8 or UTF-16). `duration_ms` is
// the item's length, for the end of the last line. The error is a sentence for
// the page: the file is empty, is not UTF-8 or UTF-16, or has no lyrics in it.
[[nodiscard]] std::expected<Lyrics, std::string> parse_lyrics(
    std::string_view text, std::optional<std::int64_t> duration_ms = std::nullopt);

// A file's lyrics. A file over four MiB is refused, which no lyric file is.
[[nodiscard]] std::expected<Lyrics, std::string> read_lyrics(
    const std::filesystem::path& file, std::optional<std::int64_t> duration_ms = std::nullopt);

// The lyrics file of a media item (L1): `name.lrc` in the item's folder, the
// extension in any case. `item_path` is the item's path as the queue holds it.
[[nodiscard]] std::optional<std::filesystem::path> find_lyrics_file(const std::string& item_path);

// Whether any line has a singer: the file is a duet and a view shows who sings what.
[[nodiscard]] bool has_singers(const Lyrics& lyrics);

// The marker a line of that singer is written with, "V1:", "V2:" or "V1+V2:"; empty for
// kUnmarked.
[[nodiscard]] std::string_view singer_marker(Singer singer);

enum class WordState : std::uint8_t { kSung, kBeingSung, kNotYetSung };

// Where a position is in a timed file. Stateless: a seek, a pause and a stop
// are only a different position.
struct LyricPosition {
    // The line most recently started, or nothing before the first.
    std::optional<std::size_t> line{};
    // Whether the position is before that line's end: false in the stretch
    // after the last line.
    bool in_line = false;
    // The word being sung, when that line is word-timed and one word spans the
    // position; nothing between two words.
    std::optional<std::size_t> word{};
    // How far through that word, from 0 up to but not including 1, by time.
    double fraction = 0.0;
    // The position the answer is for: the one asked, less the delay. A caller
    // draws the other words of the line against it with word_state().
    std::int64_t position_ms = 0;
};

// `delay_ms` is the singer's nudge (planning/karaoke.md, K5): positive shows
// the lyrics later.
[[nodiscard]] LyricPosition lyric_position(const Lyrics& lyrics, std::int64_t position_ms,
                                           std::int64_t delay_ms = 0);

// A word against a position: sung from its end, being sung from its start, not
// yet sung before that. A word that starts and ends together is sung from there.
[[nodiscard]] WordState word_state(const LyricWord& word, std::int64_t position_ms);

}  // namespace iclforge::lyrics
