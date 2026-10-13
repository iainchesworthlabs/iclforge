#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "capture_lyrics.hpp"

// apps/crucible/ui/src/capture_lyrics.cpp: the option `crucible --shot --lyrics` takes, as a
// picture of the lyric strip is asked for (docs/crucible/room.md, "06 Lyrics"). What main.cpp does
// with it (find the application, select it, import the file, start the strip) needs the window and
// the engine, and is held by the QML suite's selection case and by the capture itself.

namespace {

using iclforge::crucible::ui::capture_start_ms;
using iclforge::crucible::ui::parse_capture_lyrics;

}  // namespace

TEST_CASE("[capture-lyrics] a name and a file, and no time, leave the strip to the media session", "[crucible][capture]") {
    const auto request = parse_capture_lyrics("Hearth=examples/karaoke/auld-lang-syne/auld-lang-syne.lrc");
    REQUIRE(request.has_value());
    CHECK(request->application == "Hearth");
    CHECK(request->file == "examples/karaoke/auld-lang-syne/auld-lang-syne.lrc");
    CHECK_FALSE(request->at_ms.has_value());
}

TEST_CASE("[capture-lyrics] a time is seconds or an .lrc's minutes and seconds", "[crucible][capture]") {
    const auto at = [](const std::string& tail) {
        const auto request = parse_capture_lyrics("Hearth=song.lrc@" + tail);
        REQUIRE(request.has_value());
        return request->at_ms;
    };
    CHECK(at("5") == 5000);
    CHECK(at("5.75") == 5750);
    CHECK(at("5.5") == 5500);
    CHECK(at("0:05") == 5000);
    CHECK(at("00:05.75") == 5750);
    CHECK(at("1:02.5") == 62500);
    // A fraction is read to the millisecond; what is further is dropped, not rounded.
    CHECK(at("1.23456") == 1234);
    CHECK(at("0") == 0);
}

TEST_CASE("[capture-lyrics] what is after the last @ is a time only when it reads as one", "[crucible][capture]") {
    // The file keeps its @.
    const auto path = parse_capture_lyrics(R"(Hearth=C:\users\a@b\song.lrc)");
    REQUIRE(path.has_value());
    CHECK(path->file == R"(C:\users\a@b\song.lrc)");
    CHECK_FALSE(path->at_ms.has_value());

    const auto both = parse_capture_lyrics(R"(Hearth=C:\users\a@b\song.lrc@2.5)");
    REQUIRE(both.has_value());
    CHECK(both->file == R"(C:\users\a@b\song.lrc)");
    CHECK(both->at_ms == 2500);

    // Not a time (seconds of a minute cannot be sixty, a comma is not a point): a file name, which
    // the import then says it could not open, and not a picture of the wrong place in the song.
    for (const char* tail : {"0:75", "5,5", "x", "", "5.", ".5", "-1", "1e3", "90000000"}) {
        const auto request = parse_capture_lyrics(std::string("Hearth=song.lrc@") + tail);
        REQUIRE(request.has_value());
        CHECK_FALSE(request->at_ms.has_value());
        CHECK(request->file == std::string("song.lrc@") + tail);
    }
}

TEST_CASE("[capture-lyrics] the name is before the first =, and the application's own are kept", "[crucible][capture]") {
    const auto request = parse_capture_lyrics("Microsoft Edge = C:/media/a=b.lrc @ 3");
    REQUIRE(request.has_value());
    CHECK(request->application == "Microsoft Edge");
    CHECK(request->file == "C:/media/a=b.lrc");
    CHECK(request->at_ms == 3000);
}

TEST_CASE("[capture-lyrics] a spec with no name or no file is refused", "[crucible][capture]") {
    CHECK_FALSE(parse_capture_lyrics("").has_value());
    CHECK_FALSE(parse_capture_lyrics("song.lrc").has_value());
    CHECK_FALSE(parse_capture_lyrics("=song.lrc").has_value());
    CHECK_FALSE(parse_capture_lyrics("Hearth=").has_value());
    CHECK_FALSE(parse_capture_lyrics("Hearth=  ").has_value());
    // A time with no file before it is no file.
    CHECK_FALSE(parse_capture_lyrics("Hearth=@5").has_value());
}

TEST_CASE("[capture-lyrics] the start lands the song on the time at the moment of the grab", "[crucible][capture]") {
    constexpr std::int64_t now = 1'000'000;
    constexpr std::int64_t settle = 1500;
    const std::int64_t start = capture_start_ms(now, 5750, settle);
    // The clock reads now + settle at the grab, and the position is the clock less the start.
    CHECK(now + settle - start == 5750);
    // No settle, as for a run with no picture: the song is at the time now.
    CHECK(now - capture_start_ms(now, 5750, 0) == 5750);
    // A time earlier than the settle puts the start in the future: the song has not yet begun, and
    // is at the time when the picture is taken.
    const std::int64_t late = capture_start_ms(now, 500, settle);
    CHECK(late > now);
    CHECK(now + settle - late == 500);
}
