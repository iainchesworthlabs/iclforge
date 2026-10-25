// A listener's level and distance trim, kept by speaker role (render/speaker_trim.hpp) and turned into the
// per-output trims and delays TrimDelay applies.
//
// The numbers a person hears are where a mistake hides: a delay one sample out is 7 mm, a level half a step out
// is 0.5 dB, a role that follows the wrong speaker puts the trim of the left surround on the right. So the tests
// hold the stored role numbers fixed, the delay to the sample, and the whole path from a layout to the samples
// that come out of a TrimDelay.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "iclforge/render/layout.hpp"
#include "iclforge/render/routing.hpp"
#include "iclforge/render/speaker_trim.hpp"
#include "iclforge/render/trim_delay.hpp"

namespace {

using iclforge::render::OutputLayout;
using iclforge::render::Routing;
using iclforge::render::SpeakerTrimTable;
using iclforge::render::TrimDelay;
namespace st = iclforge::render::speaker_trim;
using Catch::Approx;

OutputLayout layout_of(const char* text) {
    const auto layout = OutputLayout::parse(text);
    REQUIRE(layout.has_value());
    return *layout;
}

Routing identity_for(const OutputLayout& layout) {
    const auto routing = Routing::identity(layout.slots(), layout.slots());
    REQUIRE(routing.has_value());
    return *routing;
}

st::RoleCode role(const char* word) {
    const auto parsed = st::parse_role(word);
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_CASE("the stored role numbers are never renumbered", "[render][speaker-trim]") {
    // The numbers are in the settings of boards on shelves. A new role takes the next number; none moves.
    REQUIRE(st::kRoles == 38);
    REQUIRE(st::role_name(0) == "L");
    REQUIRE(st::role_name(1) == "C");
    REQUIRE(st::role_name(2) == "R");
    REQUIRE(st::role_name(3) == "Ls");
    REQUIRE(st::role_name(4) == "Rs");
    REQUIRE(st::role_name(5) == "Lc");
    REQUIRE(st::role_name(6) == "Rc");
    REQUIRE(st::role_name(7) == "Lrs");
    REQUIRE(st::role_name(8) == "Rrs");
    REQUIRE(st::role_name(9) == "Cs");
    REQUIRE(st::role_name(10) == "Ts");
    REQUIRE(st::role_name(11) == "Lsd");
    REQUIRE(st::role_name(12) == "Rsd");
    REQUIRE(st::role_name(13) == "Lw");
    REQUIRE(st::role_name(14) == "Rw");
    REQUIRE(st::role_name(15) == "Vhl");
    REQUIRE(st::role_name(16) == "Vhr");
    REQUIRE(st::role_name(17) == "Vhc");
    REQUIRE(st::role_name(18) == "Lts");
    REQUIRE(st::role_name(19) == "Rts");
    REQUIRE(st::role_name(20) == "LFE2");
    REQUIRE(st::role_name(21) == "LFE");
    REQUIRE(st::role_name(22) == "S1");
    REQUIRE(st::role_name(37) == "S16");
    REQUIRE(st::role_name(38).empty());
    REQUIRE(st::role_label(0) == "Front left");
    REQUIRE(st::role_label(21) == "Subwoofer");
}

TEST_CASE("every location of the standard has its role, and a slot placed by angle has its slot's", "[render][speaker-trim]") {
    using L = iclforge::base::Location;
    // Each location names the role of the same name.
    for (int i = 0; i < iclforge::base::kMaxChannels; ++i) {
        const auto location = static_cast<L>(i);
        const st::RoleCode code = st::role_of_location(location);
        REQUIRE(code < st::kLocationRoles);
        REQUIRE(st::role_name(code) == iclforge::base::name(location));
    }
    const OutputLayout five_one = layout_of("5.1");
    REQUIRE(st::role_name(st::role_of_slot(five_one, 0)) == "L");
    REQUIRE(st::role_name(st::role_of_slot(five_one, 1)) == "C");
    REQUIRE(st::role_name(st::role_of_slot(five_one, 5)) == "LFE");
    REQUIRE(st::role_of_slot(five_one, 6) == st::kNoRole);

    // By angle: the slot's number; a slot nothing is connected to has none.
    const OutputLayout by_angle = layout_of("30/0,-30/0,-,lfe");
    REQUIRE(st::role_name(st::role_of_slot(by_angle, 0)) == "S1");
    REQUIRE(st::role_name(st::role_of_slot(by_angle, 1)) == "S2");
    REQUIRE(st::role_of_slot(by_angle, 2) == st::kNoRole);
    REQUIRE(st::role_name(st::role_of_slot(by_angle, 3)) == "LFE");
}

TEST_CASE("a role is read from its name in any case, and from the plain names", "[render][speaker-trim]") {
    REQUIRE(st::parse_role("L") == 0);
    REQUIRE(st::parse_role("ls") == 3);
    REQUIRE(st::parse_role(" vhl ") == 15);
    REQUIRE(st::parse_role("lfe") == 21);
    REQUIRE(st::parse_role("S16") == 37);
    REQUIRE(st::parse_role("FL") == 0);
    REQUIRE(st::parse_role("fr") == 2);
    REQUIRE(st::parse_role("SL") == 3);
    REQUIRE(st::parse_role("BR") == 8);
    REQUIRE(st::parse_role("sub") == 21);
    REQUIRE_FALSE(st::parse_role("").has_value());
    REQUIRE_FALSE(st::parse_role("S17").has_value());
    REQUIRE_FALSE(st::parse_role("Left").has_value());
}

TEST_CASE("a level is in half decibels inside twelve, and a distance in centimetres inside ten metres", "[render][speaker-trim]") {
    SpeakerTrimTable table;
    REQUIRE(table.neutral());
    REQUIRE(table.set_level_db(0, -1.5));
    REQUIRE(table.get(0).half_db == -3);
    REQUIRE(table.get(0).level_db() == Approx(-1.5));
    REQUIRE(table.set_level_db(0, 12.0));
    REQUIRE(table.set_level_db(0, -12.0));
    // Out of range or off the step: refused, and nothing changes.
    REQUIRE_FALSE(table.set_level_db(0, 12.5));
    REQUIRE_FALSE(table.set_level_db(0, -12.5));
    REQUIRE_FALSE(table.set_level_db(0, 0.3));
    REQUIRE_FALSE(table.set_level_db(0, std::nan("")));
    REQUIRE_FALSE(table.set_level_db(st::kRoles, 1.0));
    REQUIRE(table.get(0).half_db == -24);

    REQUIRE(table.set_distance_m(1, 2.4));
    REQUIRE(table.get(1).cm == 240);
    REQUIRE(table.set_distance_m(1, 0.1));
    REQUIRE(table.get(1).cm == 10);
    REQUIRE(table.set_distance_m(1, 10.0));
    REQUIRE(table.get(1).cm == 1000);
    REQUIRE_FALSE(table.set_distance_m(1, 0.09));
    REQUIRE_FALSE(table.set_distance_m(1, 10.01));
    REQUIRE_FALSE(table.set_distance_m(1, std::nan("")));
    REQUIRE(table.get(1).cm == 1000);
    table.clear_distance(1);
    REQUIRE(table.get(1).cm == 0);
    REQUIRE_FALSE(table.neutral());
    table.clear_all();
    REQUIRE(table.neutral());
}

TEST_CASE("the stored form holds only what is not the default, and a bad one is refused whole", "[render][speaker-trim]") {
    SpeakerTrimTable table;
    std::array<std::uint8_t, SpeakerTrimTable::kBlobBytes> blob{};
    REQUIRE(table.to_blob(blob) == 2);
    REQUIRE(blob[0] == 1);
    REQUIRE(blob[1] == 0);

    REQUIRE(table.set_level_db(role("Ls"), -3.5));
    REQUIRE(table.set_distance_m(role("Ls"), 3.05));
    REQUIRE(table.set_level_db(role("LFE"), 6.0));
    const std::size_t written = table.to_blob(blob);
    REQUIRE(written == 2 + (2 * 4));
    // Pinned bytes: version 1, two entries; role 3 -7 half dB 305 cm; role 21 12 half dB 0 cm.
    const std::array<std::uint8_t, 10> expected = {1, 2, 3, 0xF9, 0x31, 0x01, 21, 12, 0, 0};
    REQUIRE(std::equal(expected.begin(), expected.end(), blob.begin()));

    SpeakerTrimTable read;
    REQUIRE(read.from_blob(std::span<const std::uint8_t>(blob.data(), written)));
    REQUIRE(read == table);

    // The worst case fits the buffer: every role set.
    SpeakerTrimTable full;
    for (st::RoleCode r = 0; r < st::kRoles; ++r) {
        REQUIRE(full.set_level_db(r, 0.5));
    }
    REQUIRE(full.to_blob(blob) == SpeakerTrimTable::kBlobBytes);
    SpeakerTrimTable full_read;
    REQUIRE(full_read.from_blob(std::span<const std::uint8_t>(blob.data(), SpeakerTrimTable::kBlobBytes)));
    REQUIRE(full_read == full);

    // Refused whole, leaving the table as it was.
    const auto refuses = [&](std::vector<std::uint8_t> bad) {
        SpeakerTrimTable target = table;
        const bool ok = target.from_blob(bad);
        return !ok && target == table;
    };
    REQUIRE(refuses({}));
    REQUIRE(refuses({1}));
    REQUIRE(refuses({2, 0}));                       // a version this reader does not know
    REQUIRE(refuses({1, 1, 3, 0, 0}));              // shorter than its count
    REQUIRE(refuses({1, 0, 3, 0, 0, 0}));           // longer than its count
    REQUIRE(refuses({1, 1, 38, 0, 0, 0}));          // a role that does not exist
    REQUIRE(refuses({1, 1, 3, 25, 0, 0}));          // 12.5 dB
    REQUIRE(refuses({1, 1, 3, 0xE7, 0, 0}));        // -12.5 dB
    REQUIRE(refuses({1, 1, 3, 0, 9, 0}));           // 9 cm
    REQUIRE(refuses({1, 1, 3, 0, 0xE9, 0x03}));     // 1001 cm
    REQUIRE(refuses({1, 39}));                      // more entries than roles
}

TEST_CASE("a body of statements changes the table, all or nothing", "[render][speaker-trim]") {
    SpeakerTrimTable table;
    REQUIRE(table.apply_text("L level=-1.5 distance=2.4\nR level=+0.5; Ls distance=1.85m").ok);
    REQUIRE(table.get(role("L")).half_db == -3);
    REQUIRE(table.get(role("L")).cm == 240);
    REQUIRE(table.get(role("R")).half_db == 1);
    REQUIRE(table.get(role("R")).cm == 0);
    REQUIRE(table.get(role("Ls")).cm == 185);

    // Commas for spaces, the dB suffix, the plain names, any case.
    REQUIRE(table.apply_text("fl,level=-2dB,distance=3\r\nSUB level=4.0").ok);
    REQUIRE(table.get(role("L")).half_db == -4);
    REQUIRE(table.get(role("L")).cm == 300);
    REQUIRE(table.get(role("LFE")).half_db == 8);

    // none takes a distance away; reset takes one speaker, then everything.
    REQUIRE(table.apply_text("L distance=none").ok);
    REQUIRE(table.get(role("L")).cm == 0);
    REQUIRE(table.apply_text("R reset").ok);
    REQUIRE(table.get(role("R")).neutral());
    REQUIRE(table.get(role("Ls")).cm == 185);
    REQUIRE(table.apply_text("reset").ok);
    REQUIRE(table.neutral());

    // One bad word anywhere and nothing is changed.
    REQUIRE(table.apply_text("C level=1").ok);
    const SpeakerTrimTable before = table;
    for (const char* bad : {"", "  \n ; ", "C level=1.2", "C level=13", "C level=abc", "C distance=0.05", "C distance=11",
                            "C distance=", "C volume=1", "C", "Left level=1", "L level=1\nR level=99", "reset now",
                            "L level=1 distance=2 level=1 distance=2 level=1 distance=2"}) {
        INFO(bad);
        const auto result = table.apply_text(bad);
        REQUIRE_FALSE(result.ok);
        REQUIRE(std::string(result.error).size() > 0);
        REQUIRE(table == before);
    }
}

TEST_CASE("a delay is the extra distance over the farthest speaker, at the speed of sound, to the sample", "[render][speaker-trim]") {
    // 343 m/s: a centimetre is 48000 / 34300 = 1.3994 samples at 48 kHz.
    REQUIRE(st::delay_samples_for(0, 48000) == 0);
    REQUIRE(st::delay_samples_for(1, 48000) == 1);
    REQUIRE(st::delay_samples_for(100, 48000) == 140);
    REQUIRE(st::delay_samples_for(343, 48000) == 480);
    REQUIRE(st::delay_samples_for(1000, 48000) == 1399);
    REQUIRE(st::delay_samples_for(1000, 44100) == 1286);
    REQUIRE(st::delay_samples_for(1000, 96000) == 2799);
    // Exactly half a sample rounds up: 34300 cm/s at a rate of 686 Hz is 0.02 samples a centimetre; 25 cm is 0.5.
    REQUIRE(st::delay_samples_for(25, 686) == 1);
}

TEST_CASE("a table with nothing set resolves to nothing", "[render][speaker-trim]") {
    const OutputLayout layout = layout_of("7.1.4");
    const SpeakerTrimTable table;
    const auto resolved = st::resolve_trim(layout, identity_for(layout), table, 48000, 1440);
    REQUIRE(resolved.neutral());
    REQUIRE(resolved.clamped == 0);
    REQUIRE(resolved.farthest_cm == 0);
    for (std::size_t o = 0; o < layout.slots(); ++o) {
        REQUIRE(resolved.role[o] == st::role_of_slot(layout, o));
    }
    REQUIRE(resolved.role[12] == st::kNoRole);
}

TEST_CASE("the farthest speaker is not delayed and a nearer one is, to the sample", "[render][speaker-trim]") {
    const OutputLayout layout = layout_of("5.1");  // L C R Ls Rs LFE
    SpeakerTrimTable table;
    REQUIRE(table.apply_text("L distance=3.20; R distance=3.20; C distance=2.80; Ls distance=1.90; Rs distance=2.10; LFE distance=3.60").ok);
    const auto resolved = st::resolve_trim(layout, identity_for(layout), table, 48000, 1440);
    REQUIRE(resolved.farthest_cm == 360);
    // The subwoofer at 3.60 m is the farthest: no delay. The rest by their extra distance.
    REQUIRE(resolved.delay[5] == 0);
    REQUIRE(resolved.delay[0] == st::delay_samples_for(40, 48000));   // 56
    REQUIRE(resolved.delay[2] == st::delay_samples_for(40, 48000));
    REQUIRE(resolved.delay[1] == st::delay_samples_for(80, 48000));   // 112
    REQUIRE(resolved.delay[3] == st::delay_samples_for(170, 48000));  // 238
    REQUIRE(resolved.delay[4] == st::delay_samples_for(150, 48000));  // 210
    REQUIRE(resolved.delay[0] == 56);
    REQUIRE(resolved.delay[3] == 238);
    REQUIRE(resolved.clamped == 0);

    // A speaker with no distance sits with the farthest: no delay for it.
    SpeakerTrimTable partial;
    REQUIRE(partial.apply_text("L distance=2.0").ok);
    const auto only_one = st::resolve_trim(layout, identity_for(layout), partial, 48000, 1440);
    REQUIRE(only_one.farthest_cm == 200);
    REQUIRE(only_one.neutral());
    REQUIRE(partial.apply_text("R distance=2.5").ok);
    const auto two = st::resolve_trim(layout, identity_for(layout), partial, 48000, 1440);
    REQUIRE(two.delay[0] == st::delay_samples_for(50, 48000));
    REQUIRE(two.delay[2] == 0);
    REQUIRE(two.delay[1] == 0);
}

TEST_CASE("a delay past what the build holds is cut to it, and said so; without delay lines there is none", "[render][speaker-trim]") {
    const OutputLayout layout = layout_of("2.0");
    SpeakerTrimTable table;
    REQUIRE(table.apply_text("L distance=1.0; R distance=10.0").ok);
    // 9 m of extra distance is 1260 samples; the build holds 960 (20 ms).
    const auto cut = st::resolve_trim(layout, identity_for(layout), table, 48000, 960);
    REQUIRE(cut.delay[0] == 960);
    REQUIRE(cut.clamped == 1);
    REQUIRE(cut.delay[1] == 0);
    // And a build that holds enough does not.
    const auto whole = st::resolve_trim(layout, identity_for(layout), table, 48000, 1440);
    REQUIRE(whole.delay[0] == st::delay_samples_for(900, 48000));
    REQUIRE(whole.clamped == 0);
    // No delay lines: levels only.
    SpeakerTrimTable both = table;
    REQUIRE(both.apply_text("L level=-2").ok);
    const auto none = st::resolve_trim(layout, identity_for(layout), both, 48000, 0);
    REQUIRE(none.delay[0] == 0);
    REQUIRE(none.level_db[0] == Approx(-2.0));
    REQUIRE(none.clamped == 1);

    const auto caps = st::trim_caps(1440, 48000);
    REQUIRE(caps.delay);
    REQUIRE(caps.max_span_m == Approx(10.29).margin(0.01));
    REQUIRE(caps.max_span_cm == 1029);
    REQUIRE(st::trim_caps(960, 48000).max_span_cm == 686);
    REQUIRE(st::trim_caps(960, 44100).max_span_cm == 746);
    REQUIRE(st::trim_caps(0, 48000).max_span_cm == 0);
    const auto no_caps = st::trim_caps(0, 48000);
    REQUIRE_FALSE(no_caps.delay);
    REQUIRE(no_caps.level);
    REQUIRE(std::string(no_caps.delay_reason).size() > 0);
}

TEST_CASE("the span is how far apart the nearest and the farthest set distances are", "[render][speaker-trim]") {
    SpeakerTrimTable table;
    REQUIRE(table.span_cm() == 0);
    REQUIRE(table.apply_text("L distance=3.0").ok);
    REQUIRE(table.span_cm() == 0);  // one distance is no span
    REQUIRE(table.apply_text("Ls distance=1.85; Vhl distance=4.2").ok);
    REQUIRE(table.span_cm() == 235);
    // Whether or not a layout has the speaker: the span is over every role.
    REQUIRE(table.apply_text("S9 distance=10").ok);
    REQUIRE(table.span_cm() == 815);
    REQUIRE(table.apply_text("S9 distance=none").ok);
    REQUIRE(table.span_cm() == 235);
    // Levels are not distances.
    REQUIRE(table.apply_text("C level=3").ok);
    REQUIRE(table.span_cm() == 235);
}

TEST_CASE("a trim follows its speaker when the layout changes", "[render][speaker-trim]") {
    SpeakerTrimTable table;
    REQUIRE(table.apply_text("Ls level=-3 distance=2.0; Rs level=+3 distance=2.0; L distance=3.0; R distance=3.0; C distance=3.0").ok);

    const OutputLayout five = layout_of("5.1");  // L C R Ls Rs LFE
    const auto a = st::resolve_trim(five, identity_for(five), table, 48000, 1440);
    REQUIRE(a.level_db[3] == Approx(-3.0));
    REQUIRE(a.level_db[4] == Approx(3.0));

    // 7.1.4 puts the surrounds in the same slots, with the rears after them and the heights after those.
    const OutputLayout seven = layout_of("7.1.4");  // L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE
    const auto b = st::resolve_trim(seven, identity_for(seven), table, 48000, 1440);
    REQUIRE(b.level_db[3] == Approx(-3.0));
    REQUIRE(b.level_db[4] == Approx(3.0));
    REQUIRE(b.delay[3] == st::delay_samples_for(100, 48000));
    REQUIRE(b.delay[5] == 0);  // the rears have no distance: with the farthest

    // A list that puts the right surround first: the trim goes with the speaker, not the slot.
    const OutputLayout reordered = layout_of("Rs,Ls,R,C,L");
    const auto c = st::resolve_trim(reordered, identity_for(reordered), table, 48000, 1440);
    REQUIRE(c.level_db[0] == Approx(3.0));
    REQUIRE(c.level_db[1] == Approx(-3.0));
    REQUIRE(c.role[0] == role("Rs"));

    // A patch that sends channels to other outputs: the trim goes to the output the speaker's channel reaches.
    const OutputLayout two = layout_of("2.0");
    SpeakerTrimTable lr;
    REQUIRE(lr.apply_text("L level=-1; R level=+2").ok);
    const std::array<int, 2> swapped = {1, 0};
    const auto routing = Routing::from_outputs(swapped, 2);
    REQUIRE(routing.has_value());
    const auto d = st::resolve_trim(two, *routing, lr, 48000, 0);
    REQUIRE(d.level_db[1] == Approx(-1.0));
    REQUIRE(d.level_db[0] == Approx(2.0));
    REQUIRE(d.role[1] == role("L"));

    // A channel the patch leaves out has no output, and costs nothing.
    const std::array<int, 2> left_out = {0, Routing::kUnassigned};
    const auto partial = Routing::from_outputs(left_out, 2);
    REQUIRE(partial.has_value());
    const auto e = st::resolve_trim(two, *partial, lr, 48000, 0);
    REQUIRE(e.level_db[0] == Approx(-1.0));
    REQUIRE(e.role[1] == st::kNoRole);
}

TEST_CASE("the resolved trim through a delay line puts each speaker its distance behind, across blocks and at two rates", "[render][speaker-trim]") {
    for (const std::uint32_t rate : {48000U, 44100U}) {
        INFO(rate);
        const OutputLayout layout = layout_of("2.0");
        SpeakerTrimTable table;
        REQUIRE(table.apply_text("L distance=2.0; R distance=3.5").ok);
        const std::size_t max_delay = TrimDelay::samples_for_ms(30.0, rate);
        const auto resolved = st::resolve_trim(layout, identity_for(layout), table, rate, max_delay);
        const std::uint32_t wanted = st::delay_samples_for(150, rate);
        REQUIRE(resolved.delay[0] == wanted);
        REQUIRE(resolved.delay[1] == 0);

        std::vector<float> storage(TrimDelay::storage_floats(2, max_delay));
        TrimDelay stage;
        REQUIRE(stage.configure(storage, 2, max_delay));
        for (std::size_t o = 0; o < 2; ++o) {
            REQUIRE(stage.set_trim_db(o, resolved.level_db[o]));
            REQUIRE(stage.set_delay(o, resolved.delay[o]));
        }
        // A ramp through both outputs in blocks of 256, 100 and 7 samples: the farthest speaker is untouched,
        // the nearer one is exactly `wanted` samples later.
        for (const std::size_t block : {256U, 100U, 7U}) {
            stage.reset();
            const std::size_t total = 3000;
            std::vector<float> near_out;
            std::vector<float> far_out;
            std::size_t next = 1;
            for (std::size_t done = 0; done < total; done += block) {
                const std::size_t n = std::min(block, total - done);
                std::vector<float> a(n);
                std::vector<float> b(n);
                for (std::size_t k = 0; k < n; ++k) {
                    a[k] = b[k] = static_cast<float>(next + k);
                }
                next += n;
                const std::array<std::span<float>, 2> planes = {std::span<float>(a), std::span<float>(b)};
                stage.process(planes);
                near_out.insert(near_out.end(), a.begin(), a.end());
                far_out.insert(far_out.end(), b.begin(), b.end());
            }
            for (std::size_t k = 0; k < total; ++k) {
                REQUIRE(far_out[k] == static_cast<float>(k + 1));
                const float expected = k < wanted ? 0.0F : static_cast<float>(k - wanted + 1);
                REQUIRE(near_out[k] == expected);
            }
        }
    }
}

TEST_CASE("a level comes out as exactly the gain it names, and zero changes nothing", "[render][speaker-trim]") {
    const OutputLayout layout = layout_of("2.0");
    SpeakerTrimTable table;
    REQUIRE(table.apply_text("L level=-6.0; R level=+3.5").ok);
    const auto resolved = st::resolve_trim(layout, identity_for(layout), table, 48000, 0);

    std::vector<float> storage;
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 2, 0));
    for (std::size_t o = 0; o < 2; ++o) {
        REQUIRE(stage.set_trim_db(o, resolved.level_db[o]));
    }
    std::vector<float> a(256);
    std::vector<float> b(256);
    for (std::size_t k = 0; k < 256; ++k) {
        a[k] = b[k] = std::sin(static_cast<float>(k) * 0.1F) * 0.5F;
    }
    const std::vector<float> original = a;
    const std::array<std::span<float>, 2> planes = {std::span<float>(a), std::span<float>(b)};
    stage.process(planes);
    // The gain is 10^(dB/20) as a float, and each sample is the original times it.
    const float gain_l = stage.gain(0);
    const float gain_r = stage.gain(1);
    REQUIRE(gain_l == Approx(0.501187).epsilon(1e-6));
    REQUIRE(gain_r == Approx(1.496236).epsilon(1e-6));
    for (std::size_t k = 0; k < 256; ++k) {
        REQUIRE(a[k] == original[k] * gain_l);
        REQUIRE(b[k] == original[k] * gain_r);
    }

    // The default table is the stage doing nothing, bit for bit.
    const SpeakerTrimTable empty;
    const auto none = st::resolve_trim(layout, identity_for(layout), empty, 48000, 1440);
    TrimDelay idle;
    std::vector<float> idle_storage(TrimDelay::storage_floats(2, 1440));
    REQUIRE(idle.configure(idle_storage, 2, 1440));
    for (std::size_t o = 0; o < 2; ++o) {
        REQUIRE(idle.set_trim_db(o, none.level_db[o]));
        REQUIRE(idle.set_delay(o, none.delay[o]));
    }
    REQUIRE_FALSE(idle.active());
    std::vector<float> c = original;
    std::vector<float> d = original;
    const std::array<std::span<float>, 2> idle_planes = {std::span<float>(c), std::span<float>(d)};
    idle.process(idle_planes);
    REQUIRE(c == original);
    REQUIRE(d == original);
}

TEST_CASE("the report is JSON a page can read", "[render][speaker-trim]") {
    const OutputLayout layout = layout_of("2.0");
    SpeakerTrimTable table;
    REQUIRE(table.apply_text("L level=-1.5 distance=2.4; R distance=2.0").ok);
    const auto resolved = st::resolve_trim(layout, identity_for(layout), table, 48000, 1440);
    st::TrimReport report;
    report.layout = layout.text();
    report.caps = st::trim_caps(1440, 48000);
    report.farthest_m = resolved.farthest_cm / 100.0;
    for (std::size_t o = 0; o < 2; ++o) {
        const st::SpeakerTrim trim = table.get(resolved.role[o]);
        report.rows.push_back({.role = resolved.role[o],
                               .output = static_cast<int>(o),
                               .level_db = trim.level_db(),
                               .distance_m = trim.distance_m(),
                               .applied_level_db = resolved.level_db[o],
                               .applied_delay_samples = resolved.delay[o],
                               .delay_clamped = false});
    }
    const std::string json = st::trim_json(report);
    REQUIRE(json.find("\"layout\":\"2.0\"") != std::string::npos);
    REQUIRE(json.find("\"level\":{\"available\":true,\"min_db\":-12.0,\"max_db\":12.0,\"step_db\":0.5}") != std::string::npos);
    REQUIRE(json.find("\"delay\":{\"available\":true,\"min_m\":0.10,\"max_m\":10.00,\"max_span_m\":10.29,\"samples\":1440,\"reason\":\"\"}") != std::string::npos);
    REQUIRE(json.find("\"farthest_m\":2.40") != std::string::npos);
    REQUIRE(json.find("{\"role\":\"L\",\"label\":\"Front left\",\"output\":0,\"level_db\":-1.5,\"distance_m\":2.40,"
                      "\"applied_level_db\":-1.5,\"delay_samples\":0,\"delay_ms\":0.00,\"clamped\":false}") != std::string::npos);
    REQUIRE(json.find("{\"role\":\"R\",\"label\":\"Front right\",\"output\":1,\"level_db\":0.0,\"distance_m\":2.00,"
                      "\"applied_level_db\":0.0,\"delay_samples\":56,\"delay_ms\":1.17,\"clamped\":false}") != std::string::npos);

    // No delay lines: the reason is given, and no range is claimed.
    report.caps = st::trim_caps(0, 48000);
    const std::string without = st::trim_json(report);
    REQUIRE(without.find("\"delay\":{\"available\":false,\"min_m\":0.10,\"max_m\":10.00,\"max_span_m\":0.00,\"samples\":0,"
                         "\"reason\":\"this board has no memory for delay lines\"}") != std::string::npos);
}

TEST_CASE("the editor steps a level a half decibel and stops at the ends", "[speaker-trim][editor]") {
    REQUIRE(*st::step_half_db(0, +1) == 1);
    REQUIRE(*st::step_half_db(0, -1) == -1);
    REQUIRE(*st::step_half_db(23, +1) == 24);
    REQUIRE_FALSE(st::step_half_db(24, +1).has_value());
    REQUIRE(*st::step_half_db(-23, -1) == -24);
    REQUIRE_FALSE(st::step_half_db(-24, -1).has_value());
    REQUIRE_FALSE(st::step_half_db(3, 0).has_value());
    // Every stop a finger reaches is one the table takes, and the other way about.
    SpeakerTrimTable table;
    for (int half = -24; half <= 24; ++half) {
        REQUIRE(table.set_level_db(*st::parse_role("L"), half * 0.5));
    }
    std::int8_t here = -24;
    int stops = 1;
    while (const auto next = st::step_half_db(here, +1)) {
        here = *next;
        ++stops;
    }
    REQUIRE(stops == 49);
    REQUIRE(here == 24);
}

TEST_CASE("the editor steps a distance along its grid, from a seed, to nothing and back", "[speaker-trim][editor]") {
    // Unset: up starts at the seed (held inside the table's range), down does nothing.
    REQUIRE(*st::step_distance_cm(0, +1, 10, 300) == 300);
    REQUIRE(*st::step_distance_cm(0, +1, 100, 5) == st::kDistanceMinCm);
    REQUIRE(*st::step_distance_cm(0, +1, 100, 5000) == st::kDistanceMaxCm);
    REQUIRE_FALSE(st::step_distance_cm(0, -1, 10, 300).has_value());
    // On the grid.
    REQUIRE(*st::step_distance_cm(300, +1, 10, 300) == 310);
    REQUIRE(*st::step_distance_cm(300, -1, 10, 300) == 290);
    REQUIRE(*st::step_distance_cm(300, +1, 100, 300) == 400);
    REQUIRE(*st::step_distance_cm(300, -1, 100, 300) == 200);
    // A step is the step, from wherever the distance is (a PUT stored 2.45 m, and 4.1 m less a metre is 3.1 m).
    REQUIRE(*st::step_distance_cm(245, +1, 10, 300) == 255);
    REQUIRE(*st::step_distance_cm(245, -1, 10, 300) == 235);
    REQUIRE(*st::step_distance_cm(245, +1, 100, 300) == 345);
    REQUIRE(*st::step_distance_cm(410, -1, 100, 300) == 310);
    // The far end: held at 10 m, and nothing past it.
    REQUIRE(*st::step_distance_cm(995, +1, 10, 300) == 1000);
    REQUIRE(*st::step_distance_cm(950, +1, 100, 300) == 1000);
    REQUIRE_FALSE(st::step_distance_cm(1000, +1, 10, 300).has_value());
    // The near end: a step that would pass the nearest distance the table takes stops at it, and from it the speaker goes
    // back to "not set".
    REQUIRE(*st::step_distance_cm(20, -1, 10, 300) == 10);
    REQUIRE(*st::step_distance_cm(10, -1, 10, 300) == 0);
    REQUIRE(*st::step_distance_cm(100, -1, 100, 300) == 10);
    REQUIRE(*st::step_distance_cm(50, -1, 100, 300) == 10);
    REQUIRE(*st::step_distance_cm(10, -1, 100, 300) == 0);
    // No direction and no step are no move.
    REQUIRE_FALSE(st::step_distance_cm(300, 0, 10, 300).has_value());
    REQUIRE_FALSE(st::step_distance_cm(300, +1, 0, 300).has_value());
    // Every stop it reaches from the seed is one the table takes.
    SpeakerTrimTable table;
    const st::RoleCode role = *st::parse_role("C");
    std::uint16_t cm = *st::step_distance_cm(0, +1, 10, 300);
    int stops = 0;
    for (;; ++stops) {
        REQUIRE(table.set_distance_m(role, cm / 100.0));
        const auto next = st::step_distance_cm(cm, +1, 10, 300);
        if (!next) {
            break;
        }
        cm = *next;
    }
    REQUIRE(cm == 1000);
    REQUIRE(stops == 70);
}

TEST_CASE("the editor's words for a level, a distance and a table", "[speaker-trim][editor]") {
    REQUIRE(st::level_text(0) == "0 dB");
    REQUIRE(st::level_text(3) == "+1.5 dB");
    REQUIRE(st::level_text(-3) == "-1.5 dB");
    REQUIRE(st::level_text(2) == "+1 dB");
    REQUIRE(st::level_text(-24) == "-12 dB");
    REQUIRE(st::level_text(24) == "+12 dB");
    REQUIRE(st::level_text(1) == "+0.5 dB");
    REQUIRE(st::level_text(-1) == "-0.5 dB");

    REQUIRE(st::distance_text(0) == "not set");
    REQUIRE(st::distance_text(10) == "0.1 m");
    REQUIRE(st::distance_text(240) == "2.4 m");
    REQUIRE(st::distance_text(245) == "2.45 m");
    REQUIRE(st::distance_text(305) == "3.05 m");
    REQUIRE(st::distance_text(300) == "3 m");
    REQUIRE(st::distance_text(1000) == "10 m");

    SpeakerTrimTable table;
    REQUIRE(st::summary_text(table) == "none set");
    REQUIRE(table.set_count() == 0);
    REQUIRE(table.set_level_db(*st::parse_role("L"), -1.0));
    REQUIRE(st::summary_text(table) == "1 speaker set");
    REQUIRE(table.set_distance_m(*st::parse_role("R"), 2.0));
    REQUIRE(table.set_level_db(*st::parse_role("L"), -2.0));
    REQUIRE(st::summary_text(table) == "2 speakers set");
    // A speaker put back to the default is not counted.
    table.clear(*st::parse_role("L"));
    REQUIRE(st::summary_text(table) == "1 speaker set");
    table.clear_all();
    REQUIRE(st::summary_text(table) == "none set");
}

TEST_CASE("the editor's statements are ones the table's own parser takes, and say what they mean", "[speaker-trim][editor]") {
    for (st::RoleCode role = 0; role < st::kRoles; ++role) {
        INFO(st::role_name(role));
        for (const std::int8_t half : {std::int8_t{-24}, std::int8_t{-3}, std::int8_t{0}, std::int8_t{1}, std::int8_t{24}}) {
            SpeakerTrimTable table;
            const auto result = table.apply_text(st::level_statement(role, half));
            REQUIRE(result.ok);
            REQUIRE(table.get(role).half_db == half);
            REQUIRE(table.get(role).cm == 0);
        }
        for (const std::uint16_t cm : {std::uint16_t{10}, std::uint16_t{245}, std::uint16_t{1000}}) {
            SpeakerTrimTable table;
            REQUIRE(table.apply_text(st::distance_statement(role, cm)).ok);
            REQUIRE(table.get(role).cm == cm);
            REQUIRE(table.get(role).half_db == 0);
            // "none" takes it away again.
            REQUIRE(table.apply_text(st::distance_statement(role, 0)).ok);
            REQUIRE(table.get(role).cm == 0);
        }
        SpeakerTrimTable table;
        REQUIRE(table.apply_text(st::level_statement(role, 5) + ";" + st::distance_statement(role, 300)).ok);
        REQUIRE_FALSE(table.neutral());
        REQUIRE(table.apply_text(st::reset_statement(role)).ok);
        REQUIRE(table.neutral());
    }
    REQUIRE(st::level_statement(*st::parse_role("Ls"), -3) == "Ls level=-1.5");
    REQUIRE(st::distance_statement(*st::parse_role("LFE"), 240) == "LFE distance=2.40");
    REQUIRE(st::distance_statement(*st::parse_role("S7"), 0) == "S7 distance=none");
    REQUIRE(st::reset_statement(*st::parse_role("C")) == "C reset");
}
