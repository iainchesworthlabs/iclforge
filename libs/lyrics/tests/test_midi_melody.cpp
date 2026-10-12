#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/lyrics/midi_melody.hpp"

// libs/lyrics/src/midi_melody.cpp: the notes of a MIDI karaoke file, and which track is the melody
// (planning/karaoke-microphone.md, D9; findings F4). The files are built here, byte by byte, so
// that what is in them is what the test says.

namespace {

using iclforge::lyrics::read_midi_melody;

constexpr std::uint32_t kDivision =
    480;  // ticks to a quarter note; at 120 bpm 480 ticks are 500 ms

struct Event {
    std::uint64_t tick = 0;
    int order = 0;      // note-offs before note-ons at one tick
    std::string bytes;  // the event after its delta time
};

void vlq(std::string& out, std::uint32_t value) {
    std::string tmp;
    tmp.push_back(static_cast<char>(value & 0x7FU));
    value >>= 7;
    while (value != 0) {
        tmp.push_back(static_cast<char>(0x80U | (value & 0x7FU)));
        value >>= 7;
    }
    out.append(tmp.rbegin(), tmp.rend());
}

void be16(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void be32(std::string& out, std::uint32_t v) {
    be16(out, v >> 16);
    be16(out, v & 0xFFFF);
}

Event meta(std::uint64_t tick, std::uint8_t type, const std::string& data) {
    std::string b;
    b.push_back(static_cast<char>(0xFF));
    b.push_back(static_cast<char>(type));
    vlq(b, static_cast<std::uint32_t>(data.size()));
    b += data;
    return {tick, 1, b};
}

Event tempo(std::uint64_t tick, std::uint32_t micros) {
    std::string data;
    data.push_back(static_cast<char>((micros >> 16) & 0xFF));
    data.push_back(static_cast<char>((micros >> 8) & 0xFF));
    data.push_back(static_cast<char>(micros & 0xFF));
    return meta(tick, 0x51, data);
}

Event lyric(std::uint64_t tick, const std::string& text) {
    return meta(tick, 0x05, text);
}

Event text(std::uint64_t tick, const std::string& s) {
    return meta(tick, 0x01, s);
}

std::vector<Event> note(std::uint64_t tick, int channel, int pitch, std::uint64_t length,
                        int velocity = 90) {
    std::string on;
    on.push_back(static_cast<char>(0x90 | channel));
    on.push_back(static_cast<char>(pitch));
    on.push_back(static_cast<char>(velocity));
    std::string off;
    off.push_back(static_cast<char>(0x80 | channel));
    off.push_back(static_cast<char>(pitch));
    off.push_back(static_cast<char>(0));
    return {{tick, 2, on}, {tick + length, 0, off}};
}

std::string track(std::vector<Event> events) {
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    std::string body;
    std::uint64_t at = 0;
    for (const Event& e : events) {
        vlq(body, static_cast<std::uint32_t>(e.tick - at));
        at = e.tick;
        body += e.bytes;
    }
    body += std::string("\x00\xFF\x2F\x00", 4);
    std::string chunk = "MTrk";
    be32(chunk, static_cast<std::uint32_t>(body.size()));
    return chunk + body;
}

std::string file(const std::vector<std::string>& tracks, std::uint32_t division = kDivision) {
    std::string out = "MThd";
    be32(out, 6);
    be16(out, 1);
    be16(out, static_cast<std::uint32_t>(tracks.size()));
    be16(out, division);
    for (const std::string& t : tracks) {
        out += t;
    }
    return out;
}

void append(std::vector<Event>& to, const std::vector<Event>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

// Twelve syllables a beat apart, a melody that sings on them (a few ticks early or late, as
// hand-timed files are), a chord track that lands on every second beat, and a drum track on every
// beat.
struct Song {
    std::string bytes;
};

Song song(bool with_tempo_change = false, bool use_text_events = false) {
    std::vector<Event> conductor = {tempo(0, 500000), meta(0, 0x03, "Conductor")};
    if (with_tempo_change) {
        conductor.push_back(tempo(2400, 250000));  // twice as fast from beat 5
    }
    std::vector<Event> words;
    if (use_text_events) {
        words.push_back(text(0, "@KMIDI KARAOKE FILE"));
        words.push_back(text(0, "@T Test song"));
    }
    std::vector<Event> melody;
    std::vector<Event> chords;
    std::vector<Event> drums;
    const int pitches[] = {60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65};
    for (int i = 0; i < 12; ++i) {
        const std::uint64_t beat = static_cast<std::uint64_t>(i) * kDivision;
        const std::string syllable = "la";
        words.push_back(use_text_events ? text(beat, syllable) : lyric(beat, syllable));
        const std::uint64_t jitter = static_cast<std::uint64_t>((i % 3) * 4);  // up to 8 ticks late
        append(melody, note(beat + jitter, 0, pitches[i], 400));
        append(drums, note(beat, 9, 36, 60));
        if (i % 2 == 0) {
            for (const int p : {48, 52, 55}) {
                append(chords, note(beat, 1, p, 900));
            }
            append(chords, note(beat, 1, 43, 900));
            append(chords, note(beat + 200, 1, 50, 100));
        }
    }
    Song s;
    s.bytes = file({track(conductor), track(words), track(chords), track(melody), track(drums)});
    return s;
}

}  // namespace

TEST_CASE("midi melody: the track whose notes sit on the words is the melody",
          "[lyrics][midi-melody]") {
    const auto melody = read_midi_melody(song().bytes);
    REQUIRE(melody.has_value());
    CHECK(melody->track == 3);
    CHECK(melody->channel == 0);
    CHECK(melody->syllables == 12);
    CHECK(melody->match == Catch::Approx(1.0));
    REQUIRE(melody->notes.size() == 12);
    CHECK(melody->notes[0].pitch == 60);
    CHECK(melody->notes[5].pitch == 69);
    // Beat 1 is 500 ms at 120 bpm; the note is 8 ticks late (8.3 ms) and 400 ticks long (416.7 ms).
    CHECK(melody->notes[1].start_ms == 500 + 4);
    CHECK(melody->notes[1].end_ms - melody->notes[1].start_ms == 417);
    for (std::size_t i = 1; i < melody->notes.size(); ++i) {
        CHECK(melody->notes[i].start_ms >= melody->notes[i - 1].start_ms);
    }
}

TEST_CASE("midi melody: a tempo change moves every later note", "[lyrics][midi-melody]") {
    const auto melody = read_midi_melody(song(true).bytes);
    REQUIRE(melody.has_value());
    REQUIRE(melody->notes.size() == 12);
    // Beat 5 is at tick 2400, which is 5 x 500 ms; from there a beat is 250 ms. Note 4 is at tick
    // 1920 plus 4 of jitter, before the change; note 6 is at tick 2880 exactly, 480 ticks (250 ms)
    // after the change.
    CHECK(melody->notes[4].start_ms == 2004);
    CHECK(melody->notes[6].start_ms == 2750);
    CHECK(melody->match == Catch::Approx(1.0));
}

TEST_CASE(
    "midi melody: text events are the words when there are no lyric events, minus the @ lines",
    "[lyrics][midi-melody]") {
    const auto melody = read_midi_melody(song(false, true).bytes);
    REQUIRE(melody.has_value());
    CHECK(melody->syllables == 12);  // the two @ lines are not syllables
    CHECK(melody->track == 3);
}

TEST_CASE("midi melody: running status and a note-on of velocity 0 as a note-off are read",
          "[lyrics][midi-melody]") {
    // Eight notes on one channel, each a note-on with the status byte once and velocity-0 note-offs
    // under running status.
    std::vector<Event> words;
    std::string body;
    std::uint64_t at = 0;
    auto delta = [&](std::uint64_t tick) {
        vlq(body, static_cast<std::uint32_t>(tick - at));
        at = tick;
    };
    for (int i = 0; i < 8; ++i) {
        words.push_back(lyric(static_cast<std::uint64_t>(i) * 480, "la"));
    }
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t t = static_cast<std::uint64_t>(i) * 480;
        delta(t);
        if (i == 0) {
            body.push_back(static_cast<char>(0x90));  // the only status byte
        }
        body.push_back(static_cast<char>(60 + i));
        body.push_back(static_cast<char>(80));
        delta(t + 300);
        body.push_back(static_cast<char>(60 + i));
        body.push_back(static_cast<char>(0));  // note-on, velocity 0: a note-off
    }
    body += std::string("\x00\xFF\x2F\x00", 4);
    std::string melody_track = "MTrk";
    be32(melody_track, static_cast<std::uint32_t>(body.size()));
    melody_track += body;

    const auto melody =
        read_midi_melody(file({track({tempo(0, 500000)}), track(words), melody_track}));
    REQUIRE(melody.has_value());
    REQUIRE(melody->notes.size() == 8);
    CHECK(melody->notes[3].pitch == 63);
    CHECK(melody->notes[3].start_ms == 1500);
    CHECK(melody->notes[3].end_ms - melody->notes[3].start_ms == 313);  // 300 ticks
}

TEST_CASE("midi melody: of two tracks that match the words, the lead line with fewer notes wins",
          "[lyrics][midi-melody]") {
    std::vector<Event> words;
    std::vector<Event> lead;
    std::vector<Event> pad;
    for (int i = 0; i < 10; ++i) {
        const std::uint64_t beat = static_cast<std::uint64_t>(i) * kDivision;
        words.push_back(lyric(beat, "la"));
        append(lead, note(beat, 0, 60 + i, 400));
        append(pad, note(beat, 1, 40, 400));
        append(pad, note(beat, 1, 47, 400));  // two notes a beat on the same starts
    }
    const auto melody =
        read_midi_melody(file({track({tempo(0, 500000)}), track(words), track(pad), track(lead)}));
    REQUIRE(melody.has_value());
    CHECK(melody->track == 3);
    CHECK(melody->notes.size() == 10);
}

TEST_CASE("midi melody: a note never ended runs to the end of its track", "[lyrics][midi-melody]") {
    std::vector<Event> words;
    std::vector<Event> notes;
    for (int i = 0; i < 9; ++i) {
        const std::uint64_t beat = static_cast<std::uint64_t>(i) * kDivision;
        words.push_back(lyric(beat, "la"));
        if (i < 8) {
            append(notes, note(beat, 0, 60 + i, 400));
        } else {
            // Only the note-on of the last note.
            std::string on;
            on.push_back(static_cast<char>(0x90));
            on.push_back(static_cast<char>(70));
            on.push_back(static_cast<char>(90));
            notes.push_back({beat, 2, on});
        }
    }
    notes.push_back(meta(9 * kDivision + 480, 0x06, "end"));
    const auto melody =
        read_midi_melody(file({track({tempo(0, 500000)}), track(words), track(notes)}));
    REQUIRE(melody.has_value());
    REQUIRE(melody->notes.size() == 9);
    CHECK(melody->notes[8].end_ms > melody->notes[8].start_ms);
}

TEST_CASE("midi melody: a file that cannot give a melody says why", "[lyrics][midi-melody]") {
    SECTION("not a MIDI file") {
        const auto r = read_midi_melody("RIFF....WAVEfmt ");
        REQUIRE(!r.has_value());
        CHECK(r.error().find("not a MIDI file") != std::string::npos);
    }
    SECTION("SMPTE time") {
        const auto r = read_midi_melody(file({track({tempo(0, 500000)})}, 0xE728));
        REQUIRE(!r.has_value());
        CHECK(r.error().find("SMPTE") != std::string::npos);
    }
    SECTION("cut short") {
        std::string bytes = song().bytes;
        bytes.resize(bytes.size() / 2);
        const auto r = read_midi_melody(bytes);
        CHECK(!r.has_value());
    }
    SECTION("no words") {
        std::vector<Event> notes;
        for (int i = 0; i < 10; ++i) {
            append(notes, note(static_cast<std::uint64_t>(i) * kDivision, 0, 60 + i, 400));
        }
        const auto r = read_midi_melody(file({track({tempo(0, 500000)}), track(notes)}));
        REQUIRE(!r.has_value());
        CHECK(r.error().find("no words") != std::string::npos);
    }
    SECTION("no track has its notes where the words are") {
        std::vector<Event> words;
        std::vector<Event> notes;
        for (int i = 0; i < 10; ++i) {
            words.push_back(lyric(static_cast<std::uint64_t>(i) * kDivision, "la"));
            // Notes a quarter of a second off every word: nowhere near within 60 ms.
            append(notes, note(static_cast<std::uint64_t>(i) * kDivision + 240, 0, 60 + i, 100));
        }
        const auto r =
            read_midi_melody(file({track({tempo(0, 500000)}), track(words), track(notes)}));
        REQUIRE(!r.has_value());
        CHECK(r.error().find("no melody") != std::string::npos);
    }
    SECTION("only percussion") {
        std::vector<Event> words;
        std::vector<Event> drums;
        for (int i = 0; i < 10; ++i) {
            words.push_back(lyric(static_cast<std::uint64_t>(i) * kDivision, "la"));
            append(drums, note(static_cast<std::uint64_t>(i) * kDivision, 9, 36, 60));
        }
        const auto r =
            read_midi_melody(file({track({tempo(0, 500000)}), track(words), track(drums)}));
        CHECK(!r.has_value());
    }
}
