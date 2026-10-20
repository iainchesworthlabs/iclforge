// The no-lyrics screen's setting, its dimming and its wording (iclforge/now_view.hpp), on the host.
//
// What is held here is what GET and PUT /now-playing and /dim-after take, what NVS may hold, and the
// words the screen says about a stream: a stored number that is renumbered turns every board's screen
// into another one, and a heading that names a 5.1 stream a 2.0 one is the screen lying.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

#include "iclforge/now_view.hpp"

namespace nv = iclforge::nowview;

TEST_CASE("now_view: the five views and their words", "[now_view]") {
    CHECK(std::string(nv::view_name(nv::View::kSpeakers)) == "speakers");
    CHECK(std::string(nv::view_name(nv::View::kTitle)) == "title");
    CHECK(std::string(nv::view_name(nv::View::kClock)) == "clock");
    CHECK(std::string(nv::view_name(nv::View::kOff)) == "off");
    CHECK(std::string(nv::view_name(nv::View::kDark)) == "dark");
    for (const nv::View view :
         {nv::View::kSpeakers, nv::View::kTitle, nv::View::kClock, nv::View::kOff, nv::View::kDark}) {
        const auto parsed = nv::parse_view(nv::view_name(view));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == view);
    }
    CHECK(nv::kDefaultView == nv::View::kSpeakers);
}

TEST_CASE("now_view: a view is read with or without a line ending, and nothing else is a view", "[now_view]") {
    CHECK(nv::parse_view("speakers\n") == nv::View::kSpeakers);
    CHECK(nv::parse_view("  off\r\n") == nv::View::kOff);
    CHECK(nv::parse_view("dark\n") == nv::View::kDark);
    CHECK_FALSE(nv::parse_view("Dark").has_value());
    CHECK_FALSE(nv::parse_view("dark screen").has_value());
    CHECK_FALSE(nv::parse_view("").has_value());
    CHECK_FALSE(nv::parse_view("Speakers").has_value());
    CHECK_FALSE(nv::parse_view("speaker").has_value());
    CHECK_FALSE(nv::parse_view("off now").has_value());
    CHECK_FALSE(nv::parse_view("1").has_value());
}

TEST_CASE("now_view: the stored numbers are the ones written down, and a number nobody knows is refused", "[now_view]") {
    // Never renumbered: a partition written by an earlier build is read by a later one.
    CHECK(static_cast<int>(nv::View::kSpeakers) == 0);
    CHECK(static_cast<int>(nv::View::kTitle) == 1);
    CHECK(static_cast<int>(nv::View::kClock) == 2);
    CHECK(static_cast<int>(nv::View::kOff) == 3);
    CHECK(static_cast<int>(nv::View::kDark) == 4);
    CHECK(nv::view_from_stored(0) == nv::View::kSpeakers);
    CHECK(nv::view_from_stored(3) == nv::View::kOff);
    CHECK(nv::view_from_stored(4) == nv::View::kDark);
    CHECK_FALSE(nv::view_from_stored(-1).has_value());
    CHECK_FALSE(nv::view_from_stored(5).has_value());
}

TEST_CASE("now_view: dim-after is never or whole minutes up to four hours", "[now_view]") {
    CHECK(nv::parse_dim_after("never") == 0);
    CHECK(nv::parse_dim_after("never\n") == 0);
    CHECK(nv::parse_dim_after("5") == 5);
    CHECK(nv::parse_dim_after(" 30 \r\n") == 30);
    CHECK(nv::parse_dim_after("240") == 240);
    CHECK(nv::parse_dim_after("05") == 5);
    // 0 is spelled never, and nothing past the ceiling or short of a minute is a time.
    CHECK_FALSE(nv::parse_dim_after("0").has_value());
    CHECK_FALSE(nv::parse_dim_after("241").has_value());
    CHECK_FALSE(nv::parse_dim_after("-5").has_value());
    CHECK_FALSE(nv::parse_dim_after("").has_value());
    CHECK_FALSE(nv::parse_dim_after("5 min").has_value());
    CHECK_FALSE(nv::parse_dim_after("1000").has_value());
    CHECK_FALSE(nv::parse_dim_after("Never").has_value());
}

TEST_CASE("now_view: dim-after reads back as the words it was set with", "[now_view]") {
    CHECK(nv::dim_after_text(0) == "never");
    CHECK(nv::dim_after_text(5) == "5");
    CHECK(nv::dim_after_text(240) == "240");
    for (const int minutes : {0, 1, 5, 30, 240}) {
        CHECK(nv::parse_dim_after(nv::dim_after_text(minutes)) == minutes);
    }
}

TEST_CASE("now_view: the screen dims after the time set with nothing touched, and only on its own view", "[now_view]") {
    constexpr std::uint32_t kMinute = 60'000;
    // On the view, five minutes set: not a millisecond early, and from the minute on.
    CHECK_FALSE(nv::dimmed(true, 5, 5 * kMinute - 1));
    CHECK(nv::dimmed(true, 5, 5 * kMinute));
    CHECK(nv::dimmed(true, 5, 60 * kMinute));
    // Never is never.
    CHECK_FALSE(nv::dimmed(true, nv::kDimNever, 0xFFFFFFFFU));
    // A lyric screen, the idle screen, a song with the view off: nothing to dim.
    CHECK_FALSE(nv::dimmed(false, 5, 600 * kMinute));
    // The ceiling does not overflow the arithmetic.
    CHECK_FALSE(nv::dimmed(true, nv::kDimMaxMinutes, 240 * kMinute - 1));
    CHECK(nv::dimmed(true, nv::kDimMaxMinutes, 240 * kMinute));
}

TEST_CASE("now_view: the dark view is dark from the moment it shows, and a touch lights it for a while", "[now_view]") {
    using nv::Dim;
    constexpr std::uint32_t kSecond = 1000;
    constexpr nv::View kDark = nv::View::kDark;
    // The song has shown for 10 s and nothing has touched the glass since before it began (it has been
    // idle for an hour): dark, however long that was.
    CHECK(nv::dim_state(kDark, true, 5, 3'600 * kSecond, 10 * kSecond) == Dim::kDark);
    // At the first instant it shows, too.
    CHECK(nv::dim_state(kDark, true, 5, 0, 0) == Dim::kDark);
    // A touch 2 s ago, after the song began (it has shown for 10 s): lit.
    CHECK(nv::dim_state(kDark, true, 5, 2 * kSecond, 10 * kSecond) == Dim::kNone);
    // Lit for exactly kDarkAwakeMs from the last touch, and not a millisecond longer.
    CHECK(nv::dim_state(kDark, true, 5, nv::kDarkAwakeMs - 1, 600 * kSecond) == Dim::kNone);
    CHECK(nv::dim_state(kDark, true, 5, nv::kDarkAwakeMs, 600 * kSecond) == Dim::kDark);
    CHECK(nv::kDarkAwakeMs == 30 * kSecond);
    // Another touch lights it for another 30 s: the time counts from the last.
    CHECK(nv::dim_state(kDark, true, 5, 1 * kSecond, 600 * kSecond) == Dim::kNone);
    // A touch just before the song began is not a touch of this song: the screen goes dark with it.
    CHECK(nv::dim_state(kDark, true, 5, 4 * kSecond, 2 * kSecond) == Dim::kDark);
    // The dim time of the speakers view means nothing here, even "never".
    CHECK(nv::dim_state(kDark, true, nv::kDimNever, 3'600 * kSecond, 10 * kSecond) == Dim::kDark);
    // Not on the no-lyrics view (a lyric screen, the idle screen): lit.
    CHECK(nv::dim_state(kDark, false, 5, 3'600 * kSecond, 3'600 * kSecond) == Dim::kNone);
}

TEST_CASE("now_view: the speakers view dims as it always did, and the dark view does not change it", "[now_view]") {
    using nv::Dim;
    constexpr std::uint32_t kMinute = 60'000;
    // Five minutes set, the song has shown for ten: the glass untouched for five minutes dims it, a
    // millisecond short does not, and a touch lights it.
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 5 * kMinute - 1, 10 * kMinute) == Dim::kNone);
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 5 * kMinute, 10 * kMinute) == Dim::kDimmed);
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 100, 10 * kMinute) == Dim::kNone);
    // The time counts from the song as well: it began a minute ago on a panel nobody touched for hours.
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 600 * kMinute, 1 * kMinute) == Dim::kNone);
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 600 * kMinute, 5 * kMinute) == Dim::kDimmed);
    // Never is never; and a view that is not showing is not dimmed.
    CHECK(nv::dim_state(nv::View::kSpeakers, true, nv::kDimNever, 600 * kMinute, 600 * kMinute) == Dim::kNone);
    CHECK(nv::dim_state(nv::View::kSpeakers, false, 5, 600 * kMinute, 600 * kMinute) == Dim::kNone);
    // It agrees with dimmed() on every input it takes, for the speakers view.
    for (const std::uint32_t inactive : {0U, 1U, 4 * kMinute, 5 * kMinute, 600 * kMinute}) {
        for (const std::uint32_t showing_ms : {0U, 3 * kMinute, 5 * kMinute, 600 * kMinute}) {
            const std::uint32_t quiet = inactive < showing_ms ? inactive : showing_ms;
            CHECK((nv::dim_state(nv::View::kSpeakers, true, 5, inactive, showing_ms) == Dim::kDimmed) ==
                  nv::dimmed(true, 5, quiet));
        }
    }
}

TEST_CASE("now_view: a stream is named for its codec and the layout it was coded in", "[now_view]") {
    using nv::Codec;
    CHECK(nv::stream_name({.codec = Codec::kPcm}) == "PCM");
    // PCM has no coded layout whatever the fields say.
    CHECK(nv::stream_name({.codec = Codec::kPcm, .have_decoder = true, .acmod = 7, .lfe = true}) == "PCM");
    // Before the decoder has looked at a unit, only the codec is known.
    CHECK(nv::stream_name({.codec = Codec::kEac3}) == "Dolby Digital Plus");
    CHECK(nv::stream_name({.codec = Codec::kUnknown}) == "Coded audio");
    CHECK(nv::stream_name({.codec = Codec::kAc3, .have_decoder = true, .acmod = 7, .lfe = true}) == "Dolby Digital 5.1");
    CHECK(nv::stream_name({.codec = Codec::kAc3, .have_decoder = true, .acmod = 2, .lfe = false}) ==
          "Dolby Digital 2.0");
    CHECK(nv::stream_name({.codec = Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true}) ==
          "Dolby Digital Plus 5.1");
    CHECK(nv::stream_name({.codec = Codec::kAc4, .have_decoder = true, .acmod = 7, .lfe = true}) ==
          "Dolby AC-4 5.1");
}

TEST_CASE("now_view: every audio coding mode has its channel count", "[now_view]") {
    // A/52 Table 5.8: 1+1, 1/0, 2/0, 3/0, 2/1, 3/1, 2/2, 3/2.
    const int expected[8] = {2, 1, 2, 3, 3, 4, 4, 5};
    for (int acmod = 0; acmod < 8; ++acmod) {
        CHECK(nv::acmod_channels(acmod) == expected[acmod]);
    }
    CHECK(nv::acmod_channels(-1) == 0);
    CHECK(nv::acmod_channels(8) == 0);
    CHECK(nv::stream_name({.codec = nv::Codec::kAc3, .have_decoder = true, .acmod = 0}) == "Dolby Digital dual mono");
    CHECK(nv::stream_name({.codec = nv::Codec::kAc3, .have_decoder = true, .acmod = 6, .lfe = true}) == "Dolby Digital 4.1");
}

TEST_CASE("now_view: objects are said once the stream carries any", "[now_view]") {
    using nv::Codec;
    CHECK(nv::stream_name({.codec = Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true, .objects = 15}) ==
          "Dolby Digital Plus 5.1 + 15 objects");
    CHECK(nv::stream_name({.codec = Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true, .objects = 1}) ==
          "Dolby Digital Plus 5.1 + 1 object");
}

TEST_CASE("now_view: a room is named by its name, or by how many outputs it has", "[now_view]") {
    CHECK(nv::room_name("5.1", 6) == "5.1");
    CHECK(nv::room_name("7.1.4", 12) == "7.1.4");
    CHECK(nv::room_name("7.1.4:top", 12) == "7.1.4:top");
    CHECK(nv::room_name("2.0\n", 2) == "2.0");
    // A list of speakers is too long to say, and starts with a letter or an angle.
    CHECK(nv::room_name("L:small,R:small,C,LFE,Ls,Rs", 6) == "6 outputs");
    CHECK(nv::room_name("30/0,-30/0,lfe", 3) == "3 outputs");
    CHECK(nv::room_name("", 2) == "2 outputs");
    CHECK(nv::room_name("", 1) == "1 output");
    CHECK(nv::room_name("", 0).empty());
}

TEST_CASE("now_view: the heading says what arrives and what it is played on", "[now_view]") {
    using nv::Codec;
    CHECK(nv::heading({.codec = Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true}, "7.1.4", 12) ==
          "Dolby Digital Plus 5.1 to 7.1.4");
    CHECK(nv::heading({.codec = Codec::kPcm}, "2.0", 2) == "PCM to 2.0");
    // No room known: the stream alone.
    CHECK(nv::heading({.codec = Codec::kPcm}, "", 0) == "PCM");
}
