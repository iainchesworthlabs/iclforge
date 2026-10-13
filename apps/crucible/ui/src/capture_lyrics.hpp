#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// What `crucible --shot <png> --lyrics Name=<file>[@<time>]` asks for (main.cpp): lyrics imported
// against the application of that name and, with a time, the strip started by hand so that the song
// is at that time when the window is grabbed. Kept apart from main() and Qt-free so that plain
// Catch2 cases hold it (test_capture_lyrics.cpp): which part of the option is the application,
// which the file and which the time, and the clock's start that lands the song on the time.

namespace iclforge::crucible::ui {

struct CaptureLyrics {
    // The name the room lists the application by, matched without regard to case.
    std::string application;
    // A path, or a file: URL, as CrucibleLyrics::importFile() takes it.
    std::string file;
    // Where the song is when the picture is taken, in milliseconds. None: the strip is left to the
    // application's media session, whatever that says.
    std::optional<std::int64_t> at_ms;
};

// `Name=<file>` or `Name=<file>@<time>`, where <time> is seconds (`5.75`) or minutes and seconds
// as an .lrc writes them (`0:05.75`, `00:05.75`). The name is what is before the first `=`; the
// time is what is after the last `@` when that reads as a time, so a path with an `@` in it is
// still a path. None for a spec with no `=`, no name or no file. A fraction is read to the
// millisecond and anything further is dropped.
[[nodiscard]] std::optional<CaptureLyrics> parse_capture_lyrics(std::string_view spec);

// The `now_ms` to give CrucibleLyrics::startAt() so that the song is at `at_ms` when the clock
// reads `now_ms + until_grab_ms`: the position is the clock less the start, so the start is the
// moment the picture is taken, less the time. A start in the future is the song not yet begun,
// which the controller reads as the beginning, so a short time is not a special case.
[[nodiscard]] std::int64_t capture_start_ms(std::int64_t now_ms, std::int64_t at_ms, std::int64_t until_grab_ms);

}  // namespace iclforge::crucible::ui
