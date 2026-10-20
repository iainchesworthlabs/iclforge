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

TEST_CASE("now_view: the four views and their words", "[now_view]") {
    CHECK(std::string(nv::view_name(nv::View::kSpeakers)) == "speakers");
    CHECK(std::string(nv::view_name(nv::View::kTitle)) == "title");
    CHECK(std::string(nv::view_name(nv::View::kClock)) == "clock");
    CHECK(std::string(nv::view_name(nv::View::kOff)) == "off");
    for (const nv::View view : {nv::View::kSpeakers, nv::View::kTitle, nv::View::kClock, nv::View::kOff}) {
        const auto parsed = nv::parse_view(nv::view_name(view));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == view);
    }
    CHECK(nv::kDefaultView == nv::View::kSpeakers);
}

TEST_CASE("now_view: a view is read with or without a line ending, and nothing else is a view", "[now_view]") {
    CHECK(nv::parse_view("speakers\n") == nv::View::kSpeakers);
    CHECK(nv::parse_view("  off\r\n") == nv::View::kOff);
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
    CHECK(nv::view_from_stored(0) == nv::View::kSpeakers);
    CHECK(nv::view_from_stored(3) == nv::View::kOff);
    CHECK_FALSE(nv::view_from_stored(-1).has_value());
    CHECK_FALSE(nv::view_from_stored(4).has_value());
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
