#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lyric_osc.hpp"

// apps/crucible/ui/src/lyric_osc.cpp: the wire form of an OSC 1.0 message, and which messages a
// change in one application's place in its lyrics makes. The address layout is in the header; the
// socket and the timer are the controller's and are held by the QML suite and by hand.

namespace {

using iclforge::crucible::ui::encode_osc;
using iclforge::crucible::ui::kOscMaxStringBytes;
using iclforge::crucible::ui::LyricEventOptions;
using iclforge::crucible::ui::LyricEventTracker;
using iclforge::crucible::ui::LyricSnapshot;
using iclforge::crucible::ui::osc_string;
using iclforge::crucible::ui::OscArg;
using iclforge::crucible::ui::OscMessage;

// The bytes as a string, for a comparison that reads: NULs are shown as '.'.
std::string shown(const std::vector<std::byte>& bytes) {
    std::string out;
    for (const std::byte b : bytes) {
        const auto c = static_cast<unsigned char>(b);
        out.push_back(c == 0 ? '.' : (c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '#'));
    }
    return out;
}

LyricSnapshot playing(int key, int line, int word = -1) {
    LyricSnapshot s;
    s.key = key;
    s.app = "Spotify";
    s.following = true;
    s.line = line;
    s.line_text = "Line " + std::to_string(line);
    s.line_start_ms = 1000 * line;
    s.line_end_ms = 1000 * line + 900;
    s.word = word;
    s.word_text = "w" + std::to_string(word);
    s.word_start_ms = 1000 * line + 100 * word;
    s.word_end_ms = 1000 * line + 100 * word + 90;
    s.position_ms = 1000 * line + 50;
    return s;
}

std::vector<std::string> addresses(const std::vector<OscMessage>& messages) {
    std::vector<std::string> out;
    for (const OscMessage& m : messages) {
        out.push_back(m.address);
    }
    return out;
}

}  // namespace

TEST_CASE("lyric osc: a message is the address, the type tags and the arguments, each padded to four bytes",
          "[crucible][lyric-osc]") {
    OscMessage m;
    m.address = "/crucible/lyrics/position";
    m.args = {OscArg::of(std::string("Spotify")), OscArg::of(std::int32_t{1250})};
    const std::vector<std::byte> bytes = encode_osc(m);
    // "/crucible/lyrics/position" is 25 bytes: a NUL and 2 more make 28; ",si" is 3: a NUL makes 4;
    // "Spotify" is 7: a NUL makes 8; and the int is 4.
    REQUIRE(bytes.size() == 28 + 4 + 8 + 4);
    CHECK(shown(bytes).substr(0, 40) == "/crucible/lyrics/position...,si.Spotify.");
    // 1250 is 0x000004E2, big-endian.
    CHECK(static_cast<unsigned>(bytes[40]) == 0x00);
    CHECK(static_cast<unsigned>(bytes[41]) == 0x00);
    CHECK(static_cast<unsigned>(bytes[42]) == 0x04);
    CHECK(static_cast<unsigned>(bytes[43]) == 0xE2);
}

TEST_CASE("lyric osc: a float is its IEEE bits, big-endian, and a negative integer is two's complement",
          "[crucible][lyric-osc]") {
    OscMessage m;
    m.address = "/x";
    m.args = {OscArg::of(1.0F), OscArg::of(std::int32_t{-1})};
    const std::vector<std::byte> bytes = encode_osc(m);
    // "/x" + NUL + 1 pad = 4; ",fi" + NUL = 4; then 4 + 4.
    REQUIRE(bytes.size() == 16);
    CHECK(shown(bytes).substr(0, 8) == "/x..,fi.");
    CHECK(static_cast<unsigned>(bytes[8]) == 0x3F);
    CHECK(static_cast<unsigned>(bytes[9]) == 0x80);
    CHECK(static_cast<unsigned>(bytes[10]) == 0x00);
    CHECK(static_cast<unsigned>(bytes[11]) == 0x00);
    for (std::size_t i = 12; i < 16; ++i) {
        CHECK(static_cast<unsigned>(bytes[i]) == 0xFF);
    }
}

TEST_CASE("lyric osc: a string that is a multiple of four bytes still gets a NUL and a whole padding word",
          "[crucible][lyric-osc]") {
    OscMessage m;
    m.address = "/abc";  // 4 bytes: NUL + 3 pad is 8
    m.args = {OscArg::of(std::string("abcd"))};
    const std::vector<std::byte> bytes = encode_osc(m);
    // "/abc" + 4 NUL = 8; ",s" + 2 NUL = 4; "abcd" + 4 NUL = 8.
    REQUIRE(bytes.size() == 20);
    CHECK(shown(bytes) == "/abc....,s..abcd....");
}

TEST_CASE("lyric osc: a string is cut at a character boundary and loses its NULs", "[crucible][lyric-osc]") {
    CHECK(osc_string(std::string("a\0b", 3)) == "ab");
    CHECK(osc_string("short") == "short");
    // 479 ASCII bytes and a two-byte character: 481 bytes; cut at 480 would split the character.
    const std::string text(kOscMaxStringBytes - 1, 'x');
    const std::string cut = osc_string(text + "\xC3\xA9");
    CHECK(cut == text);
    // A three-byte character straddling the limit goes whole.
    const std::string three = osc_string(std::string(kOscMaxStringBytes - 2, 'x') + "\xE2\x80\x9C");
    CHECK(three.size() == kOscMaxStringBytes - 2);
    // Exactly the limit is kept.
    CHECK(osc_string(std::string(kOscMaxStringBytes, 'y')).size() == kOscMaxStringBytes);
}

TEST_CASE("lyric osc: following is said when it starts and when it stops, and a first reading is not a change",
          "[crucible][lyric-osc]") {
    LyricEventTracker tracker;
    LyricSnapshot idle;
    idle.key = 7;
    idle.app = "Spotify";
    // An application with lyrics that is not followed says nothing at first.
    CHECK(tracker.update({idle}, 0).empty());

    LyricSnapshot on = playing(7, -1);
    on.line_text.clear();
    const auto started = tracker.update({on}, 100);
    REQUIRE(started.size() >= 1);
    CHECK(started[0].address == "/crucible/lyrics/state");
    CHECK(started[0].args[0].text == "Spotify");
    CHECK(started[0].args[1].text == "following");

    const auto stopped = tracker.update({idle}, 200);
    REQUIRE(stopped.size() == 1);
    CHECK(stopped[0].address == "/crucible/lyrics/state");
    CHECK(stopped[0].args[1].text == "stopped");
    // And nothing again while it stays stopped.
    CHECK(tracker.update({idle}, 300).empty());
}

TEST_CASE("lyric osc: a line is said when it changes and not while it stays", "[crucible][lyric-osc]") {
    LyricEventTracker tracker;
    const auto first = tracker.update({playing(7, 0)}, 0);
    // state, then the line.
    REQUIRE(addresses(first) == std::vector<std::string>{"/crucible/lyrics/state", "/crucible/lyrics/line"});
    const OscMessage& line = first[1];
    REQUIRE(line.args.size() == 5);
    CHECK(line.args[0].text == "Spotify");
    CHECK(line.args[1].integer == 0);
    CHECK(line.args[2].text == "Line 0");
    CHECK(line.args[3].integer == 0);
    CHECK(line.args[4].integer == 900);

    // The same line a moment later: nothing.
    CHECK(tracker.update({playing(7, 0)}, 50).empty());
    // The next line.
    const auto next = tracker.update({playing(7, 1)}, 1000);
    REQUIRE(addresses(next) == std::vector<std::string>{"/crucible/lyrics/line"});
    CHECK(next[0].args[1].integer == 1);
    CHECK(next[0].args[2].text == "Line 1");
    // Between lines: -1 and no text, so a rig can clear what it shows.
    LyricSnapshot gap = playing(7, -1);
    gap.line_text = "ignored";
    const auto between = tracker.update({gap}, 2000);
    REQUIRE(addresses(between) == std::vector<std::string>{"/crucible/lyrics/line"});
    CHECK(between[0].args[1].integer == -1);
    CHECK(between[0].args[2].text.empty());
}

TEST_CASE("lyric osc: a word is said when another is being sung, with its line", "[crucible][lyric-osc]") {
    LyricEventTracker tracker;
    (void)tracker.update({playing(7, 2, 0)}, 0);
    CHECK(tracker.update({playing(7, 2, 0)}, 10).empty());
    const auto next = tracker.update({playing(7, 2, 1)}, 100);
    REQUIRE(addresses(next) == std::vector<std::string>{"/crucible/lyrics/word"});
    const OscMessage& word = next[0];
    REQUIRE(word.args.size() == 6);
    CHECK(word.args[1].integer == 2);
    CHECK(word.args[2].integer == 1);
    CHECK(word.args[3].text == "w1");
    CHECK(word.args[4].integer == 2100);
    CHECK(word.args[5].integer == 2190);
    // Leaving the word for none is not said: the line's message says the line is over.
    CHECK(tracker.update({playing(7, 2, -1)}, 200).empty());
    // The same word number in the next line is a different word and is said.
    const auto again = tracker.update({playing(7, 3, 1)}, 300);
    CHECK(addresses(again) == std::vector<std::string>{"/crucible/lyrics/line", "/crucible/lyrics/word"});
}

TEST_CASE("lyric osc: the options choose which events are sent", "[crucible][lyric-osc]") {
    LyricEventOptions options;
    options.state = false;
    options.words = false;
    LyricEventTracker tracker(options);
    CHECK(addresses(tracker.update({playing(7, 0, 0)}, 0)) == std::vector<std::string>{"/crucible/lyrics/line"});
    // Words only.
    options.lines = false;
    options.words = true;
    tracker.set_options(options);
    CHECK(addresses(tracker.update({playing(7, 0, 1)}, 100)) == std::vector<std::string>{"/crucible/lyrics/word"});
}

TEST_CASE("lyric osc: the position is sent at its own period while it is followed", "[crucible][lyric-osc]") {
    LyricEventOptions options;
    options.state = false;
    options.lines = false;
    options.words = false;
    options.position = true;
    options.position_period_ms = 100;
    LyricEventTracker tracker(options);

    const auto at = [&](std::int64_t position, std::int64_t now) {
        LyricSnapshot s = playing(7, 0);
        s.position_ms = position;
        return tracker.update({s}, now);
    };
    const auto first = at(250, 1000);
    REQUIRE(first.size() == 1);
    CHECK(first[0].address == "/crucible/lyrics/position");
    CHECK(first[0].args[1].integer == 250);
    // 40 ms later is not due; 100 ms later is.
    CHECK(at(290, 1040).empty());
    const auto due = at(350, 1100);
    REQUIRE(due.size() == 1);
    CHECK(due[0].args[1].integer == 350);

    // Not followed: nothing, and a follow that begins again is said at once.
    LyricSnapshot idle;
    idle.key = 7;
    idle.app = "Spotify";
    CHECK(tracker.update({idle}, 1150).empty());
    CHECK(at(0, 1160).size() == 1);
}

TEST_CASE("lyric osc: an application that has gone is said to be cleared once", "[crucible][lyric-osc]") {
    LyricEventTracker tracker;
    (void)tracker.update({playing(7, 0), playing(8, 0)}, 0);
    LyricSnapshot other = playing(8, 0);
    other.app = "VLC";
    const auto gone = tracker.update({other}, 100);
    REQUIRE(gone.size() == 1);
    CHECK(gone[0].address == "/crucible/lyrics/state");
    CHECK(gone[0].args[0].text == "Spotify");
    CHECK(gone[0].args[1].text == "cleared");
    // Said once.
    CHECK(tracker.update({other}, 200).empty());
}

TEST_CASE("lyric osc: reset says everything again", "[crucible][lyric-osc]") {
    LyricEventTracker tracker;
    CHECK_FALSE(tracker.update({playing(7, 0)}, 0).empty());
    CHECK(tracker.update({playing(7, 0)}, 10).empty());
    tracker.reset();
    CHECK_FALSE(tracker.update({playing(7, 0)}, 20).empty());
}
