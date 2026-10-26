// Choosing a speaker layout by saying what the room has (iclforge/layout_builder.hpp): the name a pick makes, the words for
// it, the buttons that are greyed on a board and why. Checked for every board in board_variants.hpp (and the S3 at the
// settings a person can move) against libs/render's own parser and against the one verdict (layout_options.hpp), so that
// the sheet that is drawn from it can offer nothing the route refuses and withhold nothing the route takes.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "fake_prefs_backend.hpp"
#include "iclforge/board_variants.hpp"
#include "iclforge/layout_builder.hpp"
#include "iclforge/layout_options.hpp"
#include "iclforge/render/layout.hpp"

using iclforge::BoardCaps;
using iclforge::HeightMount;
using iclforge::LayoutPick;
using iclforge::PickField;
using iclforge::render::OutputLayout;

namespace {

namespace variants = iclforge::variants;

struct Case {
    std::string name;
    BoardCaps caps;
};

std::vector<Case> boards() {
    std::vector<Case> out;
    for (const auto& variant : iclforge::known_variants()) {
        out.push_back({variant.name, variant.caps});
    }
    for (const int bits : {16, 32}) {
        for (const bool wired : {false, true}) {
            BoardCaps caps = variants::s3_n16r8();
            caps.sink.slot_bits = bits;
            caps.sink.second_line = wired;
            out.push_back({"S3 " + std::to_string(bits) + "-bit, " + (wired ? "two lines" : "one line"), caps});
        }
    }
    out.push_back({"no sink", BoardCaps{}});
    BoardCaps capture;
    capture.sink.fixed_slots = 12;
    capture.sink.slot_bits = 32;
    out.push_back({"capture sink of 12", capture});
    BoardCaps null_sink;
    null_sink.sink.fixed_slots = 16;
    out.push_back({"null sink of 16", null_sink});
    return out;
}

// Every pick the sheet can make.
std::vector<LayoutPick> all_picks() {
    std::vector<LayoutPick> out;
    for (const int around : iclforge::kAroundSteps) {
        for (const int subs : iclforge::kSubwooferSteps) {
            for (const int heights : iclforge::kHeightSteps) {
                out.push_back({around, subs, heights, HeightMount::kHigh});
                if (heights > 0) {
                    out.push_back({around, subs, heights, HeightMount::kCeiling});
                }
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("a pick is a name, and the name is what libs/render reads", "[layout-builder]") {
    for (const LayoutPick& pick : all_picks()) {
        const std::string name = iclforge::pick_name(pick);
        const std::optional<OutputLayout> parsed = OutputLayout::parse(name);
        INFO(name);
        if (iclforge::pick_slots(pick) > iclforge::kMaxLayoutOutputs) {
            // 9.2.6 is seventeen: the grammar refuses it, as the sheet does.
            REQUIRE_FALSE(parsed.has_value());
            continue;
        }
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->slots() == iclforge::pick_slots(pick));
        REQUIRE(iclforge::pick_of(name) == pick);
    }
}

TEST_CASE("the sheet's fields are exactly the names the grammar reads", "[layout-builder]") {
    // Every F.L and F.L.H the grammar accepts is a pick, and every pick is one the grammar accepts: no layout the
    // grammar has is out of the sheet's reach, and no button makes a name that is not a layout.
    std::size_t grammar = 0;
    for (int f = 0; f <= 9; ++f) {
        for (int l = 0; l <= 3; ++l) {
            for (int h = 0; h <= 8; ++h) {
                const std::string name = std::to_string(f) + "." + std::to_string(l) + "." + std::to_string(h);
                INFO(name);
                const bool reads = OutputLayout::parse(name).has_value();
                const std::optional<LayoutPick> pick = iclforge::pick_of(name);
                if (pick && iclforge::pick_slots(*pick) > iclforge::kMaxLayoutOutputs) {
                    // 9.2.6 is a pick the sheet can make and the verdict refuses, with its own sentence.
                    REQUIRE_FALSE(reads);
                    continue;
                }
                REQUIRE(reads == pick.has_value());
                grammar += reads ? 1 : 0;
                if (h == 0) {
                    // The short form reads the same.
                    const std::string short_name = std::to_string(f) + "." + std::to_string(l);
                    REQUIRE(OutputLayout::parse(short_name).has_value() == reads);
                    REQUIRE(iclforge::pick_of(short_name).has_value() == reads);
                }
            }
        }
    }
    // Seven rings, three sub counts and four height counts, less the one combination past sixteen (9.2.6).
    REQUIRE(grammar == 7 * 3 * 4 - 1);
}

TEST_CASE("a name that is not a pick says so", "[layout-builder]") {
    for (const char* text : {"", "L,R,C", "5", "5.1.", "5.1.4.2", "6.1", "5.3", "5.1.3", "5.1.4:small", "5.1:top", "a.b",
                             "5,1", "10.1", "5.1.10", "5.1.4:"}) {
        INFO(text);
        REQUIRE_FALSE(iclforge::pick_of(text).has_value());
    }
    // The other spellings of High are High, and ":top" is the ceiling.
    for (const char* text : {"7.1.4:height", "7.1.4:upfiring", "7.1.4"}) {
        const std::optional<LayoutPick> high = iclforge::pick_of(text);
        INFO(text);
        REQUIRE(high.has_value());
        REQUIRE(high->mount == HeightMount::kHigh);
        REQUIRE(iclforge::pick_name(*high) == "7.1.4");
    }
    REQUIRE(iclforge::pick_of("7.1.4:top")->mount == HeightMount::kCeiling);
    // Heights of none have nothing to mount, however it is spelled.
    REQUIRE_FALSE(iclforge::pick_of("5.1:upfiring").has_value());
    REQUIRE_FALSE(iclforge::pick_of("5.1:height").has_value());
}

TEST_CASE("the words for a pick", "[layout-builder]") {
    REQUIRE(iclforge::pick_summary({7, 1, 4, HeightMount::kHigh}) ==
            "7.1.4: seven around you, one subwoofer, four high speakers");
    REQUIRE(iclforge::pick_summary({2, 0, 0, HeightMount::kHigh}) == "2.0: two around you");
    REQUIRE(iclforge::pick_summary({5, 2, 0, HeightMount::kHigh}) == "5.2: five around you, two subwoofers");
    REQUIRE(iclforge::pick_summary({9, 1, 6, HeightMount::kCeiling}) ==
            "9.1.6:top: nine around you, one subwoofer, six ceiling speakers");
    REQUIRE(iclforge::pick_summary({5, 1, 2, HeightMount::kCeiling}) ==
            "5.1.2:top: five around you, one subwoofer, two ceiling speakers");
}

TEST_CASE("a pick is judged by the one verdict", "[layout-builder]") {
    for (const Case& board : boards()) {
        for (const LayoutPick& pick : all_picks()) {
            const iclforge::PickVerdict verdict = iclforge::judge_pick(board.caps, pick);
            const std::string name = iclforge::pick_name(pick);
            INFO(board.name << " " << name);
            REQUIRE(verdict.name == name);
            REQUIRE(verdict.slots == iclforge::pick_slots(pick));
            if (verdict.slots > iclforge::kMaxLayoutOutputs) {
                REQUIRE_FALSE(verdict.accepted());
                REQUIRE(verdict.reason.find("at most 16") != std::string::npos);
                continue;
            }
            const iclforge::LayoutOption option = iclforge::evaluate_layout(board.caps, name, verdict.slots);
            REQUIRE(verdict.verdict == option.verdict);
            REQUIRE(verdict.fix == option.fix);
            REQUIRE(verdict.reason == option.reason);
            // What PUT /layout takes (hearth_sink.cpp's accept_layout): no more slots than the sink carries.
            REQUIRE(verdict.accepted() == (verdict.slots <= static_cast<std::size_t>(board.caps.sink.slots())));
            REQUIRE(verdict.accepted() == (verdict.reason.empty() || verdict.verdict == iclforge::LayoutVerdict::with_limits));
        }
    }
}

TEST_CASE("a seventeen-output pick is refused with its own sentence", "[layout-builder]") {
    const iclforge::PickVerdict verdict =
        iclforge::judge_pick(variants::p4_rev3_kit(), {9, 2, 6, HeightMount::kHigh});
    REQUIRE_FALSE(verdict.accepted());
    REQUIRE(verdict.reason == "9.2.6 would be 17 outputs, and a layout has at most 16.");
}

TEST_CASE("a greyed button is greyed for a reason, and never withholds what the board takes", "[layout-builder]") {
    const std::vector<LayoutPick> picks = all_picks();
    for (const Case& board : boards()) {
        for (const PickField field :
             {PickField::kAround, PickField::kSubwoofers, PickField::kHeights, PickField::kMount}) {
            // The buttons do not depend on the pick but for the mount's (it needs heights).
            for (const LayoutPick& now : {LayoutPick{2, 0, 0, HeightMount::kHigh}, LayoutPick{5, 1, 4, HeightMount::kHigh}}) {
                const std::vector<iclforge::PickStep> steps = iclforge::pick_steps(board.caps, now, field);
                REQUIRE_FALSE(steps.empty());
                for (const iclforge::PickStep& step : steps) {
                    INFO(board.name << " field " << static_cast<int>(field) << " value " << step.value << " " << step.label);
                    REQUIRE(step.available == step.reason.empty());
                    REQUIRE_FALSE(step.label.empty());
                    if (field == PickField::kMount) {
                        REQUIRE(step.available == (now.heights > 0));
                        continue;
                    }
                    // Soundness: a button greyed means that no layout with that value is taken by the board.
                    if (!step.available) {
                        for (const LayoutPick& pick : picks) {
                            if (iclforge::field_value(pick, field) == step.value) {
                                REQUIRE_FALSE(iclforge::judge_pick(board.caps, pick).accepted());
                            }
                        }
                    }
                }
                // Completeness: a button that is not greyed has a layout the board takes.
                if (field != PickField::kMount) {
                    for (const iclforge::PickStep& step : steps) {
                        if (!step.available) {
                            continue;
                        }
                        bool takes = false;
                        for (const LayoutPick& pick : picks) {
                            takes = takes || (iclforge::field_value(pick, field) == step.value &&
                                              iclforge::judge_pick(board.caps, pick).accepted());
                        }
                        INFO(board.name << " " << step.label);
                        REQUIRE(takes);
                    }
                }
            }
        }
    }
}

TEST_CASE("the buttons of a four-output board", "[layout-builder]") {
    // An S3 with 32-bit slots on one line carries four: five around you is out whatever else is chosen, a sub and a
    // pair of heights are in.
    BoardCaps caps = variants::s3_n16r8();
    caps.sink.slot_bits = 32;
    caps.sink.second_line = false;
    REQUIRE(caps.sink.slots() == 4);
    const auto around = iclforge::pick_steps(caps, {}, PickField::kAround);
    REQUIRE(around.size() == 7);
    REQUIRE(around[0].available);   // 1
    REQUIRE(around[3].available);   // 4
    REQUIRE_FALSE(around[4].available);  // 5
    REQUIRE(around[4].reason.rfind("Five around you needs 5 slots", 0) == 0);
    REQUIRE(around[4].reason.find("carries 4 (32-bit slots, one line)") != std::string::npos);
    REQUIRE(around[4].reason.find("16-bit slots would carry 8") != std::string::npos);
    const auto subs = iclforge::pick_steps(caps, {}, PickField::kSubwoofers);
    REQUIRE((subs[0].available && subs[1].available && subs[2].available));
    const auto heights = iclforge::pick_steps(caps, {}, PickField::kHeights);
    REQUIRE(heights[0].available);        // none
    REQUIRE(heights[1].available);        // 2: one around you and two heights is three outputs
    REQUIRE_FALSE(heights[2].available);  // 4: one around you and four heights is five
    REQUIRE_FALSE(heights[3].available);  // 6
    REQUIRE(heights[2].reason.rfind("Four heights needs 5 slots", 0) == 0);
}

TEST_CASE("every layout the old list had is a quick pick, and the picks are the board's verdict", "[layout-builder]") {
    for (const iclforge::CatalogLayout& layout : iclforge::kLayoutCatalog) {
        bool found = false;
        for (const iclforge::QuickPick& quick : iclforge::kQuickPicks) {
            found = found || quick.name == layout.name;
        }
        INFO(layout.name);
        REQUIRE(found);
    }
    for (const Case& board : boards()) {
        const std::vector<iclforge::QuickState> states = iclforge::quick_states(board.caps);
        REQUIRE(states.size() == iclforge::kQuickPicks.size());
        for (const iclforge::QuickState& state : states) {
            INFO(board.name << " " << state.verdict.name);
            const std::optional<OutputLayout> parsed = OutputLayout::parse(state.verdict.name);
            REQUIRE(parsed.has_value());
            REQUIRE(parsed->slots() == state.verdict.slots);
            REQUIRE(state.verdict.accepted() == iclforge::evaluate_layout(board.caps, state.verdict.name, state.verdict.slots).accepted());
        }
    }
}

TEST_CASE("the sentence at the head of the sheet", "[layout-builder]") {
    BoardCaps s3 = variants::s3_n16r8();
    s3.sink.slot_bits = 32;
    s3.sink.second_line = false;
    REQUIRE(iclforge::capacity_line(s3) ==
            "This board carries up to 4 speakers (32-bit slots, one line); 16-bit slots would carry 8.");
    REQUIRE(iclforge::capacity_line(BoardCaps{}) == "This board has no audio output to set a layout for.");
    // A chip that cannot open a TDM frame says why, and that stereo plays.
    const std::string rev1 = iclforge::capacity_line(variants::p4_rev1());
    REQUIRE(rev1.find("so only 1.0 and 2.0 can play.") != std::string::npos);
    // The kit carries sixteen: nothing to add.
    const std::string kit = iclforge::capacity_line(variants::p4_rev3_kit());
    REQUIRE(kit.rfind("This board carries up to 16 speakers", 0) == 0);
    REQUIRE(kit.find("would carry") == std::string::npos);
}

TEST_CASE("applying a pick asks the verdict first and the board only for a layout it takes", "[layout-builder]") {
    using iclforge::prefs::testing::FakeBackend;
    // A board that carries 16: the kit.
    FakeBackend kit(variants::p4_rev3_kit());
    const auto taken = iclforge::apply_pick(kit, {7, 1, 4, HeightMount::kCeiling});
    REQUIRE(taken.ok);
    REQUIRE(kit.layout_ == "7.1.4:top");
    REQUIRE(kit.sets == 1);

    // A layout past sixteen is refused with its own sentence, and the board is never asked.
    const auto too_wide = iclforge::apply_pick(kit, {9, 2, 6, HeightMount::kHigh});
    REQUIRE_FALSE(too_wide.ok);
    REQUIRE(too_wide.message == "9.2.6 would be 17 outputs, and a layout has at most 16.");
    REQUIRE(kit.layout_ == "7.1.4:top");
    REQUIRE(kit.sets == 1);

    // A board that carries four refuses 9.1.6 with the verdict's sentence.
    BoardCaps four = variants::s3_n16r8();
    four.sink.slot_bits = 32;
    four.sink.second_line = false;
    FakeBackend s3(four);
    const auto refused = iclforge::apply_pick(s3, {9, 1, 6, HeightMount::kHigh});
    REQUIRE_FALSE(refused.ok);
    REQUIRE(refused.message == iclforge::judge_pick(four, {9, 1, 6, HeightMount::kHigh}).reason);
    REQUIRE(s3.layout_ == "2.0");
    REQUIRE(s3.sets == 0);

    // A board with no output says so.
    FakeBackend none{BoardCaps{}};
    const auto nothing = iclforge::apply_pick(none, {2, 0, 0, HeightMount::kHigh});
    REQUIRE_FALSE(nothing.ok);
    REQUIRE(nothing.message == "This board has no audio output to set a layout for.");

    // A board that could not store it says so.
    kit.refuse_next = true;
    const auto failed = iclforge::apply_pick(kit, {5, 1, 0, HeightMount::kHigh});
    REQUIRE_FALSE(failed.ok);
    REQUIRE(failed.message == "The board could not take that.");
    REQUIRE(kit.layout_ == "7.1.4:top");
}

TEST_CASE("high is one rendering whether it is on the wall or up-firing, and the ceiling is another", "[layout-builder]") {
    // The reason the sheet offers one button for the two: libs/render draws them at the same angles. ":top" is the one
    // that moves the heights (to straight overhead).
    std::size_t compared = 0;
    for (const LayoutPick& pick : all_picks()) {
        if (pick.heights == 0 || pick.mount != HeightMount::kHigh || iclforge::pick_slots(pick) > iclforge::kMaxLayoutOutputs) {
            continue;
        }
        const std::string name = iclforge::pick_name(pick);
        const std::optional<OutputLayout> plain = OutputLayout::parse(name);
        const std::optional<OutputLayout> wall = OutputLayout::parse(name + ":height");
        const std::optional<OutputLayout> up = OutputLayout::parse(name + ":upfiring");
        const std::optional<OutputLayout> top = OutputLayout::parse(name + ":top");
        INFO(name);
        REQUIRE((plain && wall && up && top));
        REQUIRE(plain->slots() == top->slots());
        bool ceiling_moved = false;
        for (std::size_t i = 0; i < plain->slots(); ++i) {
            REQUIRE(plain->slot(i).direction.azimuth_deg == wall->slot(i).direction.azimuth_deg);
            REQUIRE(plain->slot(i).direction.elevation_deg == wall->slot(i).direction.elevation_deg);
            REQUIRE(plain->slot(i).direction.azimuth_deg == up->slot(i).direction.azimuth_deg);
            REQUIRE(plain->slot(i).direction.elevation_deg == up->slot(i).direction.elevation_deg);
            ceiling_moved = ceiling_moved || top->slot(i).direction.elevation_deg != plain->slot(i).direction.elevation_deg;
        }
        REQUIRE(ceiling_moved);
        ++compared;
    }
    REQUIRE(compared >= 40);
}
