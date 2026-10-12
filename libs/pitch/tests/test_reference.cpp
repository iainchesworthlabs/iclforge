#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "iclforge/pitch/reference.hpp"
#include "iclforge/pitch/synth.hpp"
#include "iclforge/pitch/tracker.hpp"

// libs/pitch/src/reference.cpp: a reference melody as a contour on a grid, and as notes
// (planning/karaoke-microphone.md, D9).

namespace {

using iclforge::pitch::Contour;
using iclforge::pitch::hz_from_midi;
using iclforge::pitch::Note;

Contour contour_with(std::initializer_list<float> hz) {
    Contour c;
    c.hop_ms = 10;
    c.hz = hz;
    return c;
}

std::vector<float> repeat(float value, std::size_t n) {
    return std::vector<float>(n, value);
}

}  // namespace

TEST_CASE("pitch reference: a contour of audio places each note at its time",
          "[pitch][reference]") {
    const std::vector<iclforge::pitch::VoiceNote> melody = {{0.0, 0.6, 60.0}, {0.9, 0.6, 67.0}};
    iclforge::pitch::VoiceOptions options;
    options.lead_s = 0.0;
    options.vibrato_cents = 10.0;  // placement is what is tested here; accuracy is test_tracker's
    const auto voice = iclforge::pitch::synthesize_voice(melody, options);
    const Contour contour = iclforge::pitch::contour_of_signal(voice.samples, 16000);

    REQUIRE(contour.hop_ms == 10);
    REQUIRE(contour.duration_ms() >= 1500);
    // Mid-note: the note's pitch within 25 cents.
    for (const auto& [ms, midi] :
         {std::pair<int, double>{300, 60.0}, std::pair<int, double>{1200, 67.0}}) {
        const float hz = contour.hz[static_cast<std::size_t>(ms / 10)];
        INFO("at " << ms << " ms");
        REQUIRE(hz > 0.0F);
        CHECK(std::fabs(iclforge::pitch::midi_from_hz(static_cast<double>(hz)) - midi) < 0.25);
    }
    // In the gap between the notes: nothing.
    CHECK(contour.hz[75] == 0.0F);
    CHECK(contour.voiced_frames() > 80);
}

TEST_CASE("pitch reference: a contour of audio at 48 kHz is the same place and pitch",
          "[pitch][reference]") {
    const std::vector<iclforge::pitch::VoiceNote> melody = {{0.0, 0.8, 64.0}};
    iclforge::pitch::VoiceOptions options;
    options.lead_s = 0.0;
    options.vibrato_cents = 10.0;
    options.sample_rate = 48000;
    const auto voice = iclforge::pitch::synthesize_voice(melody, options);
    const Contour contour = iclforge::pitch::contour_of_signal(voice.samples, 48000);
    REQUIRE(contour.hz.size() > 60);
    const float hz = contour.hz[40];
    REQUIRE(hz > 0.0F);
    CHECK(std::fabs(iclforge::pitch::midi_from_hz(static_cast<double>(hz)) - 64.0) < 0.25);
}

TEST_CASE("pitch reference: notes are rendered to a contour", "[pitch][reference]") {
    const std::vector<Note> notes = {{100, 300, 69.0}, {300, 500, 72.0}, {800, 900, 60.0}};
    const Contour c = iclforge::pitch::contour_of_notes(notes, 10, 1000);
    REQUIRE(c.hop_ms == 10);
    REQUIRE(c.hz.size() == 101);
    CHECK(c.hz[5] == 0.0F);
    CHECK(c.hz[10] == Catch::Approx(440.0F));
    CHECK(c.hz[29] == Catch::Approx(440.0F));
    CHECK(c.hz[30] == Catch::Approx(static_cast<float>(hz_from_midi(72.0))));
    CHECK(c.hz[49] == Catch::Approx(static_cast<float>(hz_from_midi(72.0))));
    CHECK(c.hz[50] == 0.0F);
    CHECK(c.hz[80] == Catch::Approx(static_cast<float>(hz_from_midi(60.0))));
    CHECK(c.hz[90] == 0.0F);
    CHECK(c.voiced_frames() == 20 + 20 + 10);
}

TEST_CASE("pitch reference: a later note replaces an overlapped one from its own start",
          "[pitch][reference]") {
    const std::vector<Note> notes = {{0, 500, 60.0}, {200, 300, 72.0}};
    const Contour c = iclforge::pitch::contour_of_notes(notes);
    CHECK(c.hz[10] == Catch::Approx(static_cast<float>(hz_from_midi(60.0))));
    CHECK(c.hz[25] == Catch::Approx(static_cast<float>(hz_from_midi(72.0))));
    CHECK(c.hz[35] == Catch::Approx(static_cast<float>(hz_from_midi(60.0))));
}

TEST_CASE("pitch reference: segmentation finds the notes of a melody", "[pitch][reference]") {
    const std::vector<iclforge::pitch::VoiceNote> melody = {
        {0.0, 0.5, 60.0}, {0.5, 0.5, 64.0}, {1.2, 0.4, 67.0}, {1.8, 0.6, 72.0}};
    iclforge::pitch::VoiceOptions options;
    options.lead_s = 0.0;
    options.vibrato_cents = 25.0;
    const auto voice = iclforge::pitch::synthesize_voice(melody, options);
    const Contour contour = iclforge::pitch::contour_of_signal(voice.samples, 16000);
    const auto notes = iclforge::pitch::segment_notes(contour);

    REQUIRE(notes.size() == melody.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        INFO("note " << i);
        CHECK(std::fabs(notes[i].midi - melody[i].midi) < 0.3);
        CHECK(std::llabs(notes[i].start_ms -
                         static_cast<std::int64_t>(melody[i].start_s * 1000.0)) <= 70);
        CHECK(std::llabs(notes[i].end_ms -
                         static_cast<std::int64_t>((melody[i].start_s + melody[i].duration_s) *
                                                   1000.0)) <= 70);
    }
}

TEST_CASE("pitch reference: a short unvoiced gap does not end a note, a flicker is not one",
          "[pitch][reference]") {
    // 10 voiced points, 2 unvoiced, 10 voiced at the same pitch: one note of 220 ms.
    Contour c;
    c.hop_ms = 10;
    auto append = [&](float hz, std::size_t n) { c.hz.insert(c.hz.end(), n, hz); };
    append(440.0F, 10);
    append(0.0F, 2);
    append(440.0F, 10);
    append(0.0F, 20);
    append(330.0F, 3);  // a 30 ms flicker: dropped
    append(0.0F, 20);
    const auto notes = iclforge::pitch::segment_notes(c);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].start_ms == 0);
    CHECK(notes[0].end_ms == 220);
    CHECK(notes[0].midi == Catch::Approx(69.0));
}

TEST_CASE("pitch reference: a jump of a tone or more starts a new note", "[pitch][reference]") {
    Contour c;
    c.hop_ms = 10;
    c.hz = repeat(static_cast<float>(hz_from_midi(60.0)), 20);
    const auto up = repeat(static_cast<float>(hz_from_midi(62.0)), 20);
    c.hz.insert(c.hz.end(), up.begin(), up.end());
    const auto notes = iclforge::pitch::segment_notes(c);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].end_ms == 200);
    CHECK(notes[1].start_ms == 200);
    CHECK(notes[1].midi == Catch::Approx(62.0));
}

TEST_CASE("pitch reference: empty input gives empty output", "[pitch][reference]") {
    CHECK(iclforge::pitch::contour_of_signal({}, 16000).hz.empty());
    CHECK(iclforge::pitch::contour_of_signal(std::vector<float>(100, 0.0F), 0).hz.empty());
    CHECK(iclforge::pitch::contour_of_notes({}).hz.empty());
    CHECK(iclforge::pitch::segment_notes(Contour{}).empty());
    CHECK(contour_with({}).duration_ms() == 0);
}
