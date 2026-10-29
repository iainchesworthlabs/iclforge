// Which layouts a board can be set to, and why not: the one verdict behind PUT /layout, GET /layout-options and the
// Preferences registry's speaker-layout choices, checked for every board in board_variants.hpp against libs/render's
// own parser and against the sink's slot count (what accept_layout() in firmware/hearth-sink/main/hearth_sink.cpp
// applies through this verdict).

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fake_prefs_backend.hpp"
#include "iclforge/board_variants.hpp"
#include "iclforge/layout_options.hpp"
#include "iclforge/preferences.hpp"
#include "iclforge/render/layout.hpp"

using iclforge::BoardCaps;
using iclforge::evaluate_layout;
using iclforge::kLayoutCatalog;
using iclforge::LayoutFix;
using iclforge::LayoutOption;
using iclforge::LayoutVerdict;
using iclforge::layout_options;
using iclforge::render::OutputLayout;

namespace {

namespace variants = iclforge::variants;

// An ESP32-S3 at a slot width and wiring (the two settings a person can change).
BoardCaps s3(int bits, bool wired, const char* profile = "s3-n16r8") {
    BoardCaps caps = variants::s3_n16r8();
    caps.profile = profile;
    caps.sink.slot_bits = bits;
    caps.sink.second_line = wired;
    return caps;
}

BoardCaps c6() { return variants::c6_16mb(); }
BoardCaps p4_rev1() { return variants::p4_rev1(); }
BoardCaps p4_rev3() { return variants::p4_rev3_kit(); }

// A board with no sink at all, and the two sinks without a TDM frame: the capture sink CI plays onto (twelve slots
// and a width it was built with) and the null sink (every slot a layout can have).
BoardCaps no_sink() { return BoardCaps{}; }

BoardCaps capture12() {
    BoardCaps caps;
    caps.sink.fixed_slots = 12;
    caps.sink.slot_bits = 32;
    return caps;
}

BoardCaps null_sink() {
    BoardCaps caps;
    caps.sink.fixed_slots = 16;
    return caps;
}

struct Case {
    std::string name;
    BoardCaps caps;
};

// Every board the project knows, and the same boards at the settings a person can move.
std::vector<Case> boards() {
    std::vector<Case> out;
    for (const auto& variant : iclforge::known_variants()) {
        out.push_back({variant.name, variant.caps});
    }
    out.push_back({"S3 16-bit, one line", s3(16, false)});
    out.push_back({"S3 32-bit, two lines", s3(32, true)});
    out.push_back({"S3 16-bit, two lines", s3(16, true)});
    out.push_back({"S3 with AC-4", s3(32, false, "s3-n16r8-ac4")});
    out.push_back({"P4 rev3, 16-bit slots", [] {
                       BoardCaps caps = p4_rev3();
                       caps.sink.slot_bits = 16;
                       return caps;
                   }()});
    out.push_back({"no sink", no_sink()});
    out.push_back({"capture sink", capture12()});
    out.push_back({"null sink", null_sink()});
    out.push_back({"a profile nobody has measured", s3(16, false, "s3-scratch")});
    return out;
}

// What PUT /layout does with a text: it parses, and the verdict does not refuse it.
bool accepted(const BoardCaps& caps, std::string_view text) {
    const std::optional<OutputLayout> layout = OutputLayout::parse(text);
    return layout.has_value() && evaluate_layout(caps, text, layout->slots()).accepted();
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

TEST_CASE("a layout is accepted if and only if the sink carries its slots, on every board", "[device][layout_options]") {
    // Every F.L.H the name grammar takes (layout.hpp): the ring, the low-frequency feeds, the heights. The verdict
    // and the sink agree to the slot: a chip that cannot open TDM reports two, and the verdict refuses past two.
    static constexpr int rings[] = {1, 2, 3, 4, 5, 7, 9};
    static constexpr int feeds[] = {0, 1, 2};
    static constexpr int heights[] = {0, 2, 4, 6};
    for (const Case& board : boards()) {
        std::size_t parsed = 0;
        for (const int ring : rings) {
            for (const int feed : feeds) {
                for (const int height : heights) {
                    const std::string name = std::to_string(ring) + "." + std::to_string(feed) +
                                             (height != 0 ? "." + std::to_string(height) : std::string{});
                    const std::optional<OutputLayout> layout = OutputLayout::parse(name);
                    if (!layout) {
                        continue;  // seventeen speakers: 9.2.6 is not a layout
                    }
                    ++parsed;
                    const LayoutOption option = evaluate_layout(board.caps, name, layout->slots());
                    INFO(board.name << " / " << name << ": " << option.reason);
                    CHECK(option.accepted() == (static_cast<int>(layout->slots()) <= board.caps.sink.slots()));
                    CHECK(accepted(board.caps, name) == option.accepted());
                    // A verdict always comes with its sentence, unless it is the plain yes.
                    CHECK((option.verdict == LayoutVerdict::supported) == option.reason.empty());
                    CHECK((option.fix == LayoutFix::none) == option.accepted());
                }
            }
        }
        CHECK(parsed >= 83);  // the grammar's whole product, less a name that expands past sixteen slots
    }
}

TEST_CASE("the listing, the route and the Preferences choices give one verdict for every board and layout", "[device][layout_options]") {
    using namespace iclforge::prefs;
    const Pref* pref = find("speaker-layout");
    REQUIRE(pref != nullptr);
    REQUIRE(pref->choices.size() == kLayoutCatalog.size());
    for (const Case& board : boards()) {
        const auto options = layout_options(board.caps);
        REQUIRE(options.size() == kLayoutCatalog.size());
        testing::FakeBackend backend(board.caps);
        const Entry entry = resolve(*pref, backend, board.caps);
        REQUIRE(entry.choices.size() == options.size());
        for (std::size_t i = 0; i < options.size(); ++i) {
            const LayoutOption& listed = options[i];
            INFO(board.name << " / " << listed.name);
            // The listing is the catalog in order, and each choice is its layout.
            CHECK(entry.choices[i].id == std::string(kLayoutCatalog[i].name));
            // The route: the same verdict over the same name, from the text.
            CHECK(accepted(board.caps, listed.name) == listed.accepted());
            // The registry: available exactly when accepted, with the same sentence.
            CHECK(entry.choices[i].availability.available == listed.accepted());
            CHECK(entry.choices[i].availability.reason == listed.reason);
            // set_value() asks the same verdict before it asks the board.
            const SetResult set = set_value(backend, *pref, listed.name);
            if (!entry.availability.available) {
                CHECK_FALSE(set.ok);
            } else if (listed.accepted()) {
                CHECK(set.ok);
                CHECK(backend.layout() == std::string(listed.name));
            } else {
                CHECK_FALSE(set.ok);
                CHECK(set.message == listed.reason);
            }
        }
    }
}

TEST_CASE("a board with no audio output has the preference greyed, with the sentence", "[device][layout_options]") {
    using namespace iclforge::prefs;
    const Pref* pref = find("speaker-layout");
    REQUIRE(pref != nullptr);
    const BoardCaps caps = no_sink();
    testing::FakeBackend backend(caps);
    const Entry entry = resolve(*pref, backend, caps);
    CHECK_FALSE(entry.availability.available);
    CHECK(entry.availability.reason == "This board has no audio output to set a layout for.");
    for (const auto& choice : entry.choices) {
        CHECK_FALSE(choice.availability.available);
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
    // And a second line wired but not needed does not colour the layouts one line carries.
    CHECK_FALSE(mentions(find(options, "5.1"), "second I2S line"));
}

TEST_CASE("an ESP32-C6 offers what its single 16-bit line carries and says its build decodes stereo", "[device][layout_options]") {
    for (const char* profile : {"c6-16mb", "c6-4mb"}) {
        BoardCaps caps = c6();
        caps.profile = profile;
        const auto options = layout_options(caps);
        CHECK(find(options, "1.0").verdict == LayoutVerdict::supported);
        CHECK(find(options, "2.0").verdict == LayoutVerdict::supported);
        for (const char* name : {"2.1", "5.1", "7.1"}) {
            INFO(profile << " " << name);
            const LayoutOption& option = find(options, name);
            CHECK(option.verdict == LayoutVerdict::with_limits);
            CHECK(option.accepted());
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

TEST_CASE("an ESP32-P4 below v3.0 offers mono and stereo and PUT /layout refuses the rest with the chip as the reason", "[device][layout_options]") {
    for (const char* profile : {"p4-rev1"}) {
        BoardCaps caps = p4_rev1();
        caps.profile = profile;
        const auto options = layout_options(caps);
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
            // The route says the same, because it is the same verdict: 5.1 is refused here, not accepted and failed.
            CHECK_FALSE(accepted(caps, option.name));
        }
        CHECK(caps.sink.slots() == 2);
    }
}

TEST_CASE("an ESP32-P4 of v3.0 or newer, the P4X kit among them, supports every layout up to 9.1.6", "[device][layout_options]") {
    for (const char* name : {"p4-rev3-kit"}) {
        for (const auto& variant : iclforge::known_variants()) {
            if (variant.name != std::string(name)) {
                continue;
            }
            for (const auto& option : layout_options(variant.caps)) {
                INFO(option.name << ": " << option.reason);
                CHECK(option.verdict == LayoutVerdict::supported);
                CHECK(option.verified);
                CHECK(option.reason.empty());
            }
        }
    }
    BoardCaps narrow = p4_rev3();
    narrow.sink.slot_bits = 16;
    for (const auto& option : layout_options(narrow)) {
        CHECK(option.verdict == LayoutVerdict::supported);
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
    // Twelve slots on one line, as if a wider line carried it: isolates the stream evidence from the wiring.
    BoardCaps caps = s3(16, false);
    caps.sink.frame_bit_max = 192;
    const LayoutOption wide = evaluate_layout(caps, "7.1.4", 12);
    CHECK(wide.verdict == LayoutVerdict::with_limits);
    CHECK(mentions(wide, "1.08 of real time"));
    CHECK(mentions(wide, "7.1.4 can break up"));
    // Narrower layouts do not carry the sentence.
    CHECK_FALSE(mentions(evaluate_layout(caps, "5.1.4", 10), "1.08"));
}

TEST_CASE("a speaker list too long to name is called 'This layout' in its sentence", "[device][layout_options]") {
    const LayoutOption option =
        evaluate_layout(c6(), "L,R,C,LFE,Ls,Rs,Lrs,Rrs,Vhl,Vhr,Lts,Rts", 12);
    CHECK(option.verdict == LayoutVerdict::unavailable);
    CHECK(option.reason.starts_with("This layout needs 12 slots"));
}

TEST_CASE("every profile the project defines has an evidence row", "[device][layout_options]") {
    // A new hardware variant is a new profile (firmware/hearth-sink/profiles.json) and a row in
    // kLayoutEvidence; tools/hearth/test_sink_profile.py holds the two lists to each other.
    for (const char* profile :
         {"s3-n16r8", "s3-n16r8-ac4", "c6-16mb", "c6-4mb", "p4-rev1", "p4-rev3"}) {
        bool found = false;
        for (const auto& row : iclforge::kLayoutEvidence) {
            found = found || row.profile == profile;
        }
        CHECK(found);
    }
    CHECK(iclforge::kLayoutEvidence.size() == 6);
}

TEST_CASE("GET /layout-options is the head and one object a layout, in a fixed key order", "[device][layout_options]") {
    const BoardCaps caps = s3(32, false);
    const std::string json = iclforge::layout_options_json(caps, "2.0");
    CHECK(json.starts_with("{\"layout\":\"2.0\",\"slots\":4,\"slot_bits\":32,\"second_line\":false,"
                           "\"profile\":\"s3-n16r8\",\"options\":[{\"layout\":\"1.0\",\"slots\":1,"
                           "\"verdict\":\"supported\",\"fix\":\"none\",\"verified\":true}"));
    CHECK(json.ends_with("]}\n"));
    CHECK(json.find("{\"layout\":\"5.1\",\"slots\":6,\"verdict\":\"unavailable\",\"fix\":\"slot-width\","
                    "\"verified\":false,\"reason\":\"5.1 needs 6 slots; this board's output carries 4 (32-bit "
                    "slots, one line). 16-bit slots would carry 8.\"}") != std::string::npos);
    // A sink that is not an I2S bus has no wiring to report; a part with one controller has no second line.
    const std::string fixed = iclforge::layout_options_head_json(capture12(), "2.0");
    CHECK(fixed.find("second_line") == std::string::npos);
    CHECK(fixed.find("\"slots\":12,\"slot_bits\":32") != std::string::npos);
    CHECK(iclforge::layout_options_head_json(c6(), "2.0").find("second_line") == std::string::npos);
    // The head's slots are what the sink carries: two on a chip that opens no TDM.
    CHECK(iclforge::layout_options_head_json(p4_rev1(), "2.0").starts_with("{\"layout\":\"2.0\",\"slots\":2,"));
    // Quotes in a layout text cannot break the document.
    CHECK(iclforge::layout_options_head_json(c6(), "a\"b").starts_with("{\"layout\":\"a\\\"b\""));
}
