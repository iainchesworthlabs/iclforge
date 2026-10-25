// Which layouts a board can be set to, and why not: the verdicts behind GET /layout-options, checked
// for every kind of board the project builds for against libs/render's own parser and the rule
// accept_layout() (firmware/hearth-sink/main/hearth_sink.cpp) applies: a layout that parses and has
// no more slots than the sink has now.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/layout_options.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/sink_plan.hpp"

using iclforge::evaluate_layout;
using iclforge::kLayoutCatalog;
using iclforge::LayoutFacts;
using iclforge::LayoutFix;
using iclforge::LayoutOption;
using iclforge::LayoutVerdict;
using iclforge::layout_options;
using iclforge::render::OutputLayout;

namespace {

constexpr int kS3C6FrameBits = 128;
constexpr int kP4FrameBits = 512;

// The facts player::sink_*() and the settings give on each kind of board, as hearth_sink.cpp fills
// them. The ceilings come from sink_plan.hpp, the function the sinks call.
LayoutFacts s3(int bits, bool wired, std::string_view profile = "s3-n16r8") {
    LayoutFacts f;
    f.profile = profile;
    f.slots_now = static_cast<int>(iclforge::sink_ceiling(bits, wired, kS3C6FrameBits));
    f.slot_bits = bits;
    f.slots_max = 16;
    f.slots_max_bits = 16;
    f.second_line_possible = true;
    f.second_line_wired = wired;
    return f;
}

// An ESP32-C6: one controller, 16-bit slots by sdkconfig.sendspin-c6, and a build that decodes two
// coded channels at most.
LayoutFacts c6(std::string_view profile = "c6-16mb") {
    LayoutFacts f;
    f.profile = profile;
    f.slots_now = static_cast<int>(iclforge::sink_ceiling(16, false, kS3C6FrameBits));
    f.slot_bits = 16;
    f.slots_max = 8;
    f.slots_max_bits = 16;
    f.coded_channels_max = 2;
    return f;
}

LayoutFacts p4(int bits, std::string_view profile, std::string_view blocked = {}) {
    LayoutFacts f;
    f.profile = profile;
    f.slots_now = static_cast<int>(iclforge::sink_ceiling(bits, false, kP4FrameBits));
    f.slot_bits = bits;
    f.slots_max = 32;
    f.slots_max_bits = 16;
    f.tdm_blocked = blocked;
    return f;
}

constexpr std::string_view kV13 = "This chip (v1.3) has no clock for multi-channel output";

LayoutFacts p4_rev1() { return p4(32, "p4-rev1", kV13); }
LayoutFacts p4_rev3() { return p4(32, "p4-rev3"); }

// A board with no sink at all, and the two sinks without hardware: the capture sink CI plays onto
// (twelve slots, no width) and the null sink (every slot a layout can have).
LayoutFacts no_sink() { return LayoutFacts{}; }

LayoutFacts capture12() {
    LayoutFacts f;
    f.slots_now = 12;
    f.slot_bits = 32;  // the width it was built for: sink_slot_bits() answers one, and nothing moves it
    f.slots_max = 12;
    return f;
}

LayoutFacts null_sink() {
    LayoutFacts f;
    f.slots_now = 16;
    f.slots_max = 16;
    return f;
}

struct Variant {
    const char* name;
    LayoutFacts facts;
};

std::vector<Variant> variants() {
    return {
        {"S3 N16R8, 32-bit, one line", s3(32, false)},
        {"S3 N16R8, 16-bit, one line", s3(16, false)},
        {"S3 N16R8, 32-bit, two lines", s3(32, true)},
        {"S3 N16R8, 16-bit, two lines", s3(16, true)},
        {"S3 N16R8 with AC-4", s3(32, false, "s3-n16r8-ac4")},
        {"C6 16 MB", c6()},
        {"C6 4 MB", c6("c6-4mb")},
        {"P4 rev1 (v1.3)", p4_rev1()},
        {"P4 rev1 with AC-4", p4(32, "p4-rev1-ac4", kV13)},
        {"P4 rev3, 32-bit", p4_rev3()},
        {"P4 rev3, 16-bit", p4(16, "p4-rev3")},
        {"P4X kit (rev3)", p4_rev3()},
        {"no sink", no_sink()},
        {"capture sink", capture12()},
        {"null sink", null_sink()},
        {"a profile nobody has measured", s3(16, false, "s3-scratch")},
    };
}

// accept_layout()'s decision, minus the queue: parses, and fits the sink.
bool accepted(const LayoutFacts& facts, std::string_view text) {
    const std::optional<OutputLayout> layout = OutputLayout::parse(text);
    return layout.has_value() && iclforge::layout_fits_sink(layout->slots(), facts.slots_now);
}

const LayoutOption& find(const std::vector<LayoutOption>& options, std::string_view name) {
    for (const LayoutOption& option : options) {
        if (option.name == name) {
            return option;
        }
    }
    FAIL("no such layout in the catalog: " << name);
    return options.front();
}

bool mentions(const LayoutOption& option, std::string_view text) {
    return option.reason.find(text) != std::string::npos;
}

}  // namespace

TEST_CASE("catalog: every name parses and its slot count is the parser's", "[device][layout_options]") {
    std::size_t last = 0;
    for (const auto& entry : kLayoutCatalog) {
        const std::optional<OutputLayout> layout = OutputLayout::parse(entry.name);
        INFO(entry.name);
        REQUIRE(layout.has_value());
        CHECK(layout->slots() == entry.slots);
        CHECK(entry.slots >= last);
        last = entry.slots;
    }
}

TEST_CASE("catalog: every layout the grammar reads is judged exactly as PUT /layout would", "[device][layout_options]") {
    // Every F.L.H the name grammar takes (layout.hpp): the ring, the low-frequency feeds, the heights.
    static constexpr int rings[] = {1, 2, 3, 4, 5, 7, 9};
    static constexpr int feeds[] = {0, 1, 2};
    static constexpr int heights[] = {0, 2, 4, 6};
    for (const Variant& variant : variants()) {
        std::size_t parsed = 0;
        for (const int ring : rings) {
            for (const int feed : feeds) {
                for (const int height : heights) {
                    const std::string name = std::to_string(ring) + "." + std::to_string(feed) +
                                             (height != 0 ? "." + std::to_string(height) : std::string{});
                    const std::optional<OutputLayout> layout = OutputLayout::parse(name);
                    if (!layout) {
                        continue;
                    }
                    ++parsed;
                    const LayoutOption option = evaluate_layout(variant.facts, name, layout->slots());
                    INFO(variant.name << " / " << name << ": " << option.reason);
                    if (accepted(variant.facts, name)) {
                        // Offered only when the route takes it. The one layout the route takes and the
                        // listing refuses is a chip that cannot open TDM, above stereo.
                        if (option.verdict == LayoutVerdict::unavailable) {
                            CHECK_FALSE(variant.facts.tdm_blocked.empty());
                            CHECK(layout->slots() > 2);
                        }
                    } else {
                        CHECK(option.verdict == LayoutVerdict::unavailable);
                    }
                    // A verdict always comes with its sentence, unless it is the plain yes.
                    CHECK((option.verdict == LayoutVerdict::supported) == option.reason.empty());
                }
            }
        }
        // The grammar's whole product. (9.2.6 is seventeen speakers and the parser keeps sixteen of
        // them: it is judged by the slots it parses to, as PUT /layout would.)
        CHECK(parsed == 84);
    }
}

TEST_CASE("catalog: a layout the route refuses is never offered, on any board, at any setting", "[device][layout_options]") {
    for (const int bits : {16, 32}) {
        for (const bool wired : {false, true}) {
            const LayoutFacts facts = s3(bits, wired);
            for (const LayoutOption& option : layout_options(facts)) {
                INFO(bits << "-bit, second line " << wired << ": " << option.name);
                CHECK((option.verdict != LayoutVerdict::unavailable) == accepted(facts, option.name));
            }
        }
    }
}

TEST_CASE("an ESP32-S3 at its shipped setting offers stereo and says what the rest needs", "[device][layout_options]") {
    const auto options = layout_options(s3(32, false));
    CHECK(find(options, "1.0").verdict == LayoutVerdict::supported);
    CHECK(find(options, "2.0").verdict == LayoutVerdict::supported);
    CHECK(find(options, "2.0").verified);
    CHECK(find(options, "2.0").reason.empty());

    // Three slots fit the four of one 32-bit line; nobody has measured this board past stereo.
    const LayoutOption& two_one = find(options, "2.1");
    CHECK(two_one.verdict == LayoutVerdict::with_limits);
    CHECK_FALSE(two_one.verified);
    CHECK(mentions(two_one, "Not verified"));

    // 5.1 and 7.1 need the eight slots of 16-bit slots on one line.
    for (const char* name : {"5.0", "5.1", "5.1.2", "7.1"}) {
        INFO(name);
        const LayoutOption& option = find(options, name);
        CHECK(option.verdict == LayoutVerdict::unavailable);
        CHECK(option.fix == LayoutFix::slot_width);
        CHECK(mentions(option, "this board's output carries 4 (32-bit slots, one line)"));
        CHECK(mentions(option, "16-bit slots would carry 8"));
    }
    // Ten or more need both.
    for (const char* name : {"5.1.4", "7.1.2", "7.1.4", "9.1.2", "9.1.4", "9.1.6"}) {
        INFO(name);
        const LayoutOption& option = find(options, name);
        CHECK(option.verdict == LayoutVerdict::unavailable);
        CHECK(option.fix == LayoutFix::slot_width_and_second_line);
        CHECK(mentions(option, "16-bit slots and a second line would carry 16"));
    }
    CHECK(find(options, "7.1.4").reason.starts_with("7.1.4 needs 12 slots; this board's output carries 4"));
}

TEST_CASE("an ESP32-S3 at 16-bit slots on one line carries 7.1 and wants a second line for more", "[device][layout_options]") {
    const auto options = layout_options(s3(16, false));
    const LayoutOption& five_one = find(options, "5.1");
    CHECK(five_one.verdict == LayoutVerdict::with_limits);
    CHECK(mentions(five_one, "widest layout measured on this kind of board is 2 slots"));
    CHECK(find(options, "7.1").verdict == LayoutVerdict::with_limits);

    const LayoutOption& height = find(options, "5.1.4");
    CHECK(height.verdict == LayoutVerdict::unavailable);
    CHECK(height.fix == LayoutFix::second_line);
    CHECK(mentions(height, "A second line wired would carry 16."));
}

TEST_CASE("an ESP32-S3 with a second line wired reaches 9.1.6, with the second line unverified", "[device][layout_options]") {
    const auto options = layout_options(s3(16, true));
    for (const auto& option : options) {
        INFO(option.name << ": " << option.reason);
        CHECK(option.verdict != LayoutVerdict::unavailable);
    }
    // Eight slots fit the first line; past that the second is carrying audio.
    CHECK(mentions(find(options, "7.1"), "Not verified"));
    const LayoutOption& wide = find(options, "7.1.4");
    CHECK(wide.verdict == LayoutVerdict::with_limits);
    CHECK_FALSE(wide.verified);
    CHECK(mentions(wide, "7.1.4 needs the second I2S line"));
    CHECK(mentions(wide, "never checked with a DAC on both lines"));
    CHECK(mentions(find(options, "9.1.6"), "second I2S line"));
}

TEST_CASE("a second line wired but not needed does not colour the layouts one line carries", "[device][layout_options]") {
    // 5.1 is six slots: the first line's eight carry it, so the second line is not in the sentence.
    const LayoutOption five_one = find(layout_options(s3(16, true)), "5.1");
    CHECK_FALSE(mentions(five_one, "second I2S line"));
}

TEST_CASE("an ESP32-C6 offers what its single 16-bit line carries and says its build decodes stereo", "[device][layout_options]") {
    for (const char* profile : {"c6-16mb", "c6-4mb"}) {
        const auto options = layout_options(c6(profile));
        CHECK(find(options, "1.0").verdict == LayoutVerdict::supported);
        CHECK(find(options, "2.0").verdict == LayoutVerdict::supported);
        for (const char* name : {"2.1", "5.1", "7.1"}) {
            INFO(profile << " " << name);
            const LayoutOption& option = find(options, name);
            CHECK(option.verdict == LayoutVerdict::with_limits);
            CHECK(mentions(option, "decodes streams of at most 2 channels"));
        }
        const LayoutOption& wide = find(options, "7.1.4");
        CHECK(wide.verdict == LayoutVerdict::unavailable);
        CHECK(wide.fix == LayoutFix::hardware);
        CHECK(wide.reason ==
              "7.1.4 needs 12 slots; this board's output carries 8 (16-bit slots, one line). The most this "
              "board can carry is 8.");
    }
}

TEST_CASE("an ESP32-P4 below v3.0 offers mono and stereo and says the chip is why", "[device][layout_options]") {
    for (const char* profile : {"p4-rev1", "p4-rev1-ac4"}) {
        LayoutFacts facts = p4_rev1();
        facts.profile = profile;
        const auto options = layout_options(facts);
        CHECK(find(options, "1.0").verdict == LayoutVerdict::supported);
        CHECK(find(options, "2.0").verdict == LayoutVerdict::supported);
        for (const auto& option : options) {
            if (option.slots <= 2) {
                continue;
            }
            INFO(profile << " " << option.name);
            CHECK(option.verdict == LayoutVerdict::unavailable);
            CHECK(option.fix == LayoutFix::hardware);
            CHECK(option.reason == "This chip (v1.3) has no clock for multi-channel output, so only 1.0 and "
                                   "2.0 can play.");
        }
        // The route would take 5.1 here (the fold is tested through it) and the listing does not offer it.
        CHECK(accepted(facts, "5.1"));
    }
}

TEST_CASE("an ESP32-P4 of v3.0 or newer, the P4X kit among them, supports every layout up to 9.1.6", "[device][layout_options]") {
    for (const LayoutFacts& facts : {p4_rev3(), p4(16, "p4-rev3")}) {
        for (const auto& option : layout_options(facts)) {
            INFO(option.name << ": " << option.reason);
            CHECK(option.verdict == LayoutVerdict::supported);
            CHECK(option.verified);
            CHECK(option.reason.empty());
        }
    }
}

TEST_CASE("a board with no sink has no layout to offer", "[device][layout_options]") {
    for (const auto& option : layout_options(no_sink())) {
        INFO(option.name);
        CHECK(option.verdict == LayoutVerdict::unavailable);
        CHECK(option.fix == LayoutFix::hardware);
        CHECK(option.reason == "This board has no audio output to set a layout for.");
    }
    CHECK_FALSE(accepted(no_sink(), "2.0"));
}

TEST_CASE("a sink with no slot width states no setting that would help", "[device][layout_options]") {
    const auto options = layout_options(capture12());
    CHECK(find(options, "7.1.4").verdict != LayoutVerdict::unavailable);
    const LayoutOption& wide = find(options, "9.1.4");
    CHECK(wide.verdict == LayoutVerdict::unavailable);
    CHECK(wide.fix == LayoutFix::hardware);
    CHECK(wide.reason == "9.1.4 needs 14 slots; this board's output carries 12. The most this board can "
                         "carry is 12.");
    // The null sink carries every slot a layout can have.
    for (const auto& option : layout_options(null_sink())) {
        CHECK(option.verdict != LayoutVerdict::unavailable);
    }
}

TEST_CASE("a profile with no evidence says not verified rather than inventing a limit", "[device][layout_options]") {
    const auto options = layout_options(s3(16, false, "s3-n16r8+scratch"));
    CHECK(find(options, "2.0").verdict == LayoutVerdict::supported);
    const LayoutOption& five_one = find(options, "5.1");
    CHECK(five_one.verdict == LayoutVerdict::with_limits);
    CHECK_FALSE(five_one.verified);
    CHECK(five_one.reason == "Not verified: the widest layout measured on this kind of board is 2 slots.");
}

TEST_CASE("the S3 records a stream measured falling behind", "[device][layout_options]") {
    // Two lines at 16 bits carry 12 slots; the 7.1.4 Atmos stream took 1.08 of real time with Wi-Fi up.
    LayoutFacts facts = s3(16, false);
    facts.slots_now = 12;  // as if a line carried it: isolates the stream evidence from the wiring
    facts.second_line_wired = false;
    const LayoutOption wide = evaluate_layout(facts, "7.1.4", 12);
    CHECK(wide.verdict == LayoutVerdict::with_limits);
    CHECK(mentions(wide, "1.08 of real time"));
    CHECK(mentions(wide, "7.1.4 can break up"));
    // Narrower layouts do not carry the sentence.
    CHECK_FALSE(mentions(evaluate_layout(facts, "5.1.4", 10), "1.08"));
}

TEST_CASE("every profile the project defines has an evidence row", "[device][layout_options]") {
    // A new hardware variant is a new profile (firmware/hearth-sink/profiles.json) and a row in
    // kLayoutEvidence; tools/hearth/test_sink_profile.py holds the two lists to each other.
    for (const char* profile :
         {"s3-n16r8", "s3-n16r8-ac4", "c6-16mb", "c6-4mb", "p4-rev1", "p4-rev1-ac4", "p4-rev3"}) {
        bool found = false;
        for (const auto& row : iclforge::kLayoutEvidence) {
            found = found || row.profile == profile;
        }
        CHECK(found);
    }
    CHECK(iclforge::kLayoutEvidence.size() == 7);
}
