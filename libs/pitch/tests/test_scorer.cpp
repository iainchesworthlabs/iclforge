#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "iclforge/pitch/reference.hpp"
#include "iclforge/pitch/scorer.hpp"
#include "iclforge/pitch/synth.hpp"
#include "iclforge/pitch/tracker.hpp"

// libs/pitch/src/scorer.cpp: the pitch score of a singer's contour against a reference
// (planning/karaoke-microphone.md, D10).

namespace {

using iclforge::pitch::Contour;
using iclforge::pitch::hz_from_midi;
using iclforge::pitch::Note;
using iclforge::pitch::ScoreOptions;
using iclforge::pitch::Scorer;

// Three notes of a second each with a gap of half a second between them: 0-1000, 1500-2500,
// 3000-4000.
std::vector<Note> melody() {
    return {{0, 1000, 60.0}, {1500, 2500, 64.0}, {3000, 4000, 67.0}};
}

Contour reference() {
    const auto notes = melody();
    return iclforge::pitch::contour_of_notes(notes, 10, 4200);
}

// The singer follows the reference `shift_cents` away and `late_ms` behind it, every grid point.
void sing(Scorer& scorer, const Contour& ref, double shift_cents, std::int64_t late_ms = 0) {
    for (std::size_t i = 0; i < ref.hz.size(); ++i) {
        const double hz = static_cast<double>(ref.hz[i]) * std::exp2(shift_cents / 1200.0);
        scorer.add(static_cast<std::int64_t>(i) * 10 + late_ms,
                   ref.hz[i] > 0.0F ? static_cast<float>(hz) : 0.0F);
    }
}

}  // namespace

TEST_CASE("pitch score: a singer on the reference scores everything", "[pitch][scorer]") {
    const Contour ref = reference();
    Scorer scorer(ref);
    sing(scorer, ref, 0.0);
    const auto total = scorer.total();
    CHECK(total.counted == 300);
    CHECK(total.sung == 300);
    CHECK(total.hit == 300);
    CHECK(total.fraction() == Catch::Approx(1.0));
    CHECK(total.coverage() == Catch::Approx(1.0));
    CHECK(std::fabs(total.mean_cents) < 0.5);
}

TEST_CASE("pitch score: octaves are ignored, unless they are asked for", "[pitch][scorer]") {
    const Contour ref = reference();
    {
        Scorer scorer(ref);
        sing(scorer, ref, 1200.0);
        CHECK(scorer.total().fraction() == Catch::Approx(1.0));
    }
    {
        Scorer scorer(ref);
        sing(scorer, ref, -2400.0);
        CHECK(scorer.total().fraction() == Catch::Approx(1.0));
    }
    {
        ScoreOptions strict;
        strict.fold_octaves = false;
        Scorer scorer(ref, strict);
        sing(scorer, ref, 1200.0);
        CHECK(scorer.total().hit == 0);
    }
}

TEST_CASE("pitch score: the tolerance decides, and the deviation is signed", "[pitch][scorer]") {
    const Contour ref = reference();
    {
        Scorer scorer(ref);  // 100 cents
        sing(scorer, ref, 150.0);
        const auto total = scorer.total();
        CHECK(total.hit == 0);
        CHECK(total.sung == 300);
        CHECK(total.mean_cents == Catch::Approx(150.0).margin(0.5));
    }
    {
        ScoreOptions loose;
        loose.tolerance_cents = 200.0;
        Scorer scorer(ref, loose);
        sing(scorer, ref, -150.0);
        const auto total = scorer.total();
        CHECK(total.fraction() == Catch::Approx(1.0));
        CHECK(total.mean_cents == Catch::Approx(-150.0).margin(0.5));
    }
    {
        // Just inside and just outside 100 cents.
        Scorer scorer(ref);
        sing(scorer, ref, 99.0);
        CHECK(scorer.total().fraction() == Catch::Approx(1.0));
        scorer.clear();
        sing(scorer, ref, 101.0);
        CHECK(scorer.total().hit == 0);
    }
}

TEST_CASE("pitch score: a key shift is allowed when it is asked for, and can be estimated",
          "[pitch][scorer]") {
    const Contour ref = reference();
    Scorer scorer(ref);
    sing(scorer, ref, 300.0);
    CHECK(scorer.total().hit == 0);

    const auto offset = scorer.key_offset_cents();
    REQUIRE(offset.has_value());
    CHECK(*offset == Catch::Approx(300.0).margin(0.5));

    ScoreOptions keyed;
    keyed.key_shift_cents = *offset;
    scorer.set_options(keyed);
    CHECK(scorer.total().fraction() == Catch::Approx(1.0));
}

TEST_CASE("pitch score: a silent singer scores nothing and is not scored as wrong everywhere",
          "[pitch][scorer]") {
    const Contour ref = reference();
    Scorer scorer(ref);
    const auto before = scorer.total();
    CHECK(before.counted == 300);
    CHECK(before.sung == 0);
    CHECK(before.hit == 0);
    CHECK(before.fraction() == 0.0);

    // An explicit "not voiced" answer is the same count as no answer.
    for (std::int64_t t = 0; t < 4200; t += 10) {
        scorer.add(t, 0.0F);
    }
    const auto after = scorer.total();
    CHECK(after.counted == 300);
    CHECK(after.sung == 0);
    CHECK(after.coverage() == 0.0);
    CHECK(!scorer.best_lag_ms().has_value());
    CHECK(!scorer.key_offset_cents().has_value());
}

TEST_CASE("pitch score: singing where the reference is silent costs nothing", "[pitch][scorer]") {
    const Contour ref = reference();
    Scorer scorer(ref);
    sing(scorer, ref, 0.0);
    // Add a held note across the gap between the first two.
    for (std::int64_t t = 1000; t < 1500; t += 10) {
        scorer.add(t, 523.0F);
    }
    const auto total = scorer.total();
    CHECK(total.counted == 300);
    CHECK(total.hit == 300);
    // The gap itself has no reference note, so it has no score.
    CHECK(!scorer.tally(1000, 1500).scored());
}

TEST_CASE("pitch score: each line has its own tally", "[pitch][scorer]") {
    const Contour ref = reference();
    Scorer scorer(ref);
    // The first note on pitch, the second 300 cents sharp, the third not sung.
    for (std::size_t i = 0; i < ref.hz.size(); ++i) {
        const std::int64_t t = static_cast<std::int64_t>(i) * 10;
        if (ref.hz[i] <= 0.0F) {
            continue;
        }
        if (t < 1000) {
            scorer.add(t, ref.hz[i]);
        } else if (t < 2500) {
            scorer.add(t, ref.hz[i] * static_cast<float>(std::exp2(300.0 / 1200.0)));
        }
    }
    const auto first = scorer.tally(0, 1250);
    const auto second = scorer.tally(1250, 2750);
    const auto third = scorer.tally(2750, 4200);
    CHECK(first.counted == 100);
    CHECK(first.fraction() == Catch::Approx(1.0));
    CHECK(second.counted == 100);
    CHECK(second.sung == 100);
    CHECK(second.hit == 0);
    CHECK(second.mean_cents == Catch::Approx(300.0).margin(0.5));
    CHECK(third.counted == 100);
    CHECK(third.sung == 0);
    CHECK(scorer.total().hit == 100);
}

TEST_CASE("pitch score: a later answer replaces an earlier one and positions outside are ignored",
          "[pitch][scorer]") {
    const Contour ref = reference();
    Scorer scorer(ref);
    scorer.add(500, 1000.0F);                                 // wrong
    scorer.add(500, static_cast<float>(hz_from_midi(60.0)));  // right, replaces it
    scorer.add(-30, 440.0F);
    scorer.add(10'000'000, 440.0F);
    scorer.add(500, -1.0F);  // nonsense is ignored: the right answer stays
    const auto t = scorer.tally(490, 510);
    CHECK(t.counted == 2);
    CHECK(t.sung == 1);
    CHECK(t.hit == 1);
}

TEST_CASE("pitch score: how early or late the singer is is found", "[pitch][scorer]") {
    const Contour ref = reference();
    {
        Scorer scorer(ref);
        sing(scorer, ref, 0.0, 200);
        const auto lag = scorer.best_lag_ms();
        REQUIRE(lag.has_value());
        CHECK(*lag == 200);
    }
    {
        Scorer scorer(ref);
        sing(scorer, ref, 0.0, -120);
        // A negative position before 0 is dropped, which costs the first few points only.
        const auto lag = scorer.best_lag_ms();
        REQUIRE(lag.has_value());
        CHECK(*lag == -120);
    }
    {
        Scorer scorer(ref);
        sing(scorer, ref, 0.0);
        const auto lag = scorer.best_lag_ms();
        REQUIRE(lag.has_value());
        CHECK(*lag == 0);
    }
}

TEST_CASE("pitch score: a lag beyond the search is not found, and nothing is said on too little",
          "[pitch][scorer]") {
    const Contour ref = reference();
    Scorer scorer(ref);
    sing(scorer, ref, 0.0, 600);
    const auto lag = scorer.best_lag_ms(300);
    // Nothing within +-300 ms fits the contour, so whatever is reported is not 600.
    if (lag.has_value()) {
        CHECK(std::abs(*lag) <= 300);
    }
    Scorer short_ref(iclforge::pitch::contour_of_notes(std::vector<Note>{{0, 100, 60.0}}, 10, 100));
    CHECK(!short_ref.best_lag_ms().has_value());
}

TEST_CASE("pitch score: a tracked voice on the melody, flat, an octave down and wrong",
          "[pitch][scorer][voice]") {
    const auto notes = melody();
    const Contour ref = iclforge::pitch::contour_of_notes(notes, 10, 4200);
    std::vector<iclforge::pitch::VoiceNote> voice_notes;
    for (const Note& n : notes) {
        voice_notes.push_back({static_cast<double>(n.start_ms) / 1000.0,
                               static_cast<double>(n.end_ms - n.start_ms) / 1000.0, n.midi});
    }
    auto score = [&](double shift_semitones) {
        std::vector<iclforge::pitch::VoiceNote> shifted = voice_notes;
        for (auto& n : shifted) {
            n.midi += shift_semitones;
        }
        iclforge::pitch::VoiceOptions options;
        options.lead_s = 0.0;
        options.seed = 11;
        options.snr_db = 20.0;
        const auto voice = iclforge::pitch::synthesize_voice(shifted, options);
        const auto tracked = iclforge::pitch::contour_of_signal(voice.samples, 16000);
        Scorer scorer(ref);
        for (std::size_t i = 0; i < tracked.hz.size(); ++i) {
            scorer.add(static_cast<std::int64_t>(i) * 10, tracked.hz[i]);
        }
        return scorer.total();
    };
    CHECK(score(0.0).fraction() > 0.85);
    CHECK(score(-0.3).fraction() > 0.85);   // 30 cents flat
    CHECK(score(-12.0).fraction() > 0.85);  // an octave down
    CHECK(score(2.5).fraction() < 0.1);     // two and a half semitones sharp
}
