#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/board_caps.hpp"
#include "iclforge/layout_options.hpp"
#include "iclforge/preferences.hpp"

// Choosing a speaker layout by saying what the room has, not by finding a name in a list: how many speakers are around
// you, how many subwoofers, how many heights and how the heights are mounted. The Speaker layout sheet of the
// Preferences (firmware/hearth-sink/main/panel/lvgl/layout_sheet.cpp) is drawn from this; the arithmetic is here, free
// of ESP-IDF and LVGL, so that the host tests it for every board in board_variants.hpp and the sheet is only drawing.
//
// THE VERDICT IS layout_options.hpp's. A pick becomes a name ("7.1.4:top"), and the name is judged by evaluate_layout()
// over the board's BoardCaps: the one verdict PUT /layout, GET /layout-options and the web page ask. Nothing here
// decides what a board can carry; it only turns a question about a step ("can I have nine around me?") into the
// layouts that step would make, and asks.
//
// WHAT THE GRAMMAR CAN SAY (libs/render's layout.hpp), and so what the sheet offers. Around you: 1, 2, 3, 4, 5, 7 or 9
// (C; L R; L C R; L R Ls Rs; L C R Ls Rs; plus Lrs Rrs; plus Lw Rw). Subwoofers: none, one (LFE) or two (LFE, LFE2).
// Heights: none, 2 (Vhl Vhr), 4 (plus Lts Rts) or 6 (plus Vhc Ts). The grammar has three ways to realize a height, and
// the board plays two differently: IN THE CEILING (":top") puts the heights straight overhead (elevation 90 degrees),
// and the other two - on the wall (no suffix, or ":height") and up-firing (":upfiring", a module that bounces the sound
// off the ceiling) - leave them where the layout's own angle puts them. The library draws no separate tier for either,
// so they are played alike and differ only as labels /status carries (a test holds the two renderings equal). The
// owner's call, 2026-10-26: "if it's the difference we can't make", so the sheet offers ONE of them, HIGH, and
// the ceiling. A layout set another way as ":upfiring" or ":height" reads back as High, and is "in use" as it is.
// NOT OFFERED: ":small" (a speaker that cannot reproduce the bottom octaves has its bass sent to the subwoofer). The
// name form has no spelling for it, a list does, and a list of a 9.1.4 room with every ring speaker marked is longer
// than the 95 characters the layout echo keeps (OutputLayout::kTextBytes), so the layout would not read back; a list
// is still taken by PUT /layout and the web page.
//
// More than sixteen outputs is not a layout whatever the board ("9.2.6" is 17): a pick that sums to more is refused with
// that sentence before the board is asked.
//
// WHAT A STEP IS GREYED FOR. A button (one subwoofer, nine around you, six heights) is greyed when the SMALLEST layout
// it could be part of is unavailable on this board - the other fields at their least - so that a button is never greyed
// because of the order a person pressed the others in: from 5.1.2 on a board that carries 8, "7" is not greyed (7.0 is
// seven outputs) but the layout it makes, 7.1.2, says why it cannot be applied. The sentence of a greyed button is the
// verdict's own, for that smallest layout.

namespace iclforge {

// How the heights are mounted. kHigh is the grammar's default and spells no suffix: on the wall or up-firing, which
// the renderer plays the same (the library's one upper tier). kCeiling is ":top", straight overhead.
enum class HeightMount : std::uint8_t { kHigh, kCeiling };

// What the room has.
struct LayoutPick {
    int around = 2;
    int subwoofers = 0;
    int heights = 0;
    HeightMount mount = HeightMount::kHigh;

    friend bool operator==(const LayoutPick&, const LayoutPick&) = default;
};

inline constexpr std::array<int, 7> kAroundSteps{1, 2, 3, 4, 5, 7, 9};
inline constexpr std::array<int, 3> kSubwooferSteps{0, 1, 2};
inline constexpr std::array<int, 4> kHeightSteps{0, 2, 4, 6};
inline constexpr std::size_t kMountSteps = 2;

// A layout is at most this many outputs (OutputLayout::kMaxSlots).
inline constexpr std::size_t kMaxLayoutOutputs = 16;

[[nodiscard]] constexpr std::size_t pick_slots(const LayoutPick& pick) {
    return static_cast<std::size_t>(pick.around + pick.subwoofers + pick.heights);
}

// "5.1.4", "7.1.4:top", "2.0": the name the pick is, the canonical short form (the heights field is left out when
// there are none, the suffix when the heights are high).
[[nodiscard]] inline std::string pick_name(const LayoutPick& pick) {
    std::string name = std::to_string(pick.around) + "." + std::to_string(pick.subwoofers);
    if (pick.heights > 0) {
        name += "." + std::to_string(pick.heights);
        if (pick.mount == HeightMount::kCeiling) {
            name += ":top";
        }
    }
    return name;
}

namespace builder_detail {

template <std::size_t N>
[[nodiscard]] constexpr bool among(const std::array<int, N>& steps, int value) {
    for (const int step : steps) {
        if (step == value) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr std::string_view word_of(int count) {
    switch (count) {
        case 1: return "one";
        case 2: return "two";
        case 3: return "three";
        case 4: return "four";
        case 5: return "five";
        case 6: return "six";
        case 7: return "seven";
        case 9: return "nine";
        default: break;
    }
    return "no";
}

}  // namespace builder_detail

// The pick a layout NAME is, when it is one the sheet can show: "5.1", "7.1.4", "9.1.6:top". Anything else (a speaker
// list, a name the grammar does not read, a field the sheet has no button for) is nullopt, and the sheet starts from
// its own default and says what is in use.
[[nodiscard]] inline std::optional<LayoutPick> pick_of(std::string_view name) {
    LayoutPick pick;
    std::string_view rest = name;
    if (const std::size_t colon = rest.find(':'); colon != std::string_view::npos) {
        const std::string_view suffix = rest.substr(colon + 1);
        if (suffix == "top") {
            pick.mount = HeightMount::kCeiling;
        } else if (suffix != "upfiring" && suffix != "height") {
            return std::nullopt;  // those two are the other spellings of High
        }
        rest = rest.substr(0, colon);
    }
    // "F.L" or "F.L.H", each a single digit.
    const bool two = rest.size() == 3 && rest[1] == '.';
    const bool three = rest.size() == 5 && rest[1] == '.' && rest[3] == '.';
    if (!(two || three) || !builder_detail::is_digit(rest[0]) || !builder_detail::is_digit(rest[2]) ||
        (three && !builder_detail::is_digit(rest[4]))) {
        return std::nullopt;
    }
    pick.around = rest[0] - '0';
    pick.subwoofers = rest[2] - '0';
    pick.heights = three ? rest[4] - '0' : 0;
    if (!builder_detail::among(kAroundSteps, pick.around) || !builder_detail::among(kSubwooferSteps, pick.subwoofers) ||
        !builder_detail::among(kHeightSteps, pick.heights)) {
        return std::nullopt;
    }
    if (pick.heights == 0 && (pick.mount != HeightMount::kHigh || name.find(':') != std::string_view::npos)) {
        return std::nullopt;  // "5.1:top" has no heights to realize
    }
    return pick;
}

// In words: "seven around you, one subwoofer, four heights in the ceiling". What the name says, for a person who does
// not read 7.1.4.
[[nodiscard]] inline std::string pick_words(const LayoutPick& pick) {
    using builder_detail::word_of;
    std::string out{word_of(pick.around)};
    out += " around you";
    if (pick.subwoofers > 0) {
        out += ", " + std::string{word_of(pick.subwoofers)} + (pick.subwoofers == 1 ? " subwoofer" : " subwoofers");
    }
    if (pick.heights > 0) {
        out += ", " + std::string{word_of(pick.heights)};
        out += pick.mount == HeightMount::kCeiling ? " ceiling speakers" : " high speakers";
    }
    return out;
}

// "7.1.4: seven around you, one subwoofer, four heights".
[[nodiscard]] inline std::string pick_summary(const LayoutPick& pick) {
    return pick_name(pick) + ": " + pick_words(pick);
}

// One verdict, copied out of evaluate_layout() so that it owns its text.
struct PickVerdict {
    std::string name;
    std::size_t slots = 0;
    LayoutVerdict verdict = LayoutVerdict::unavailable;
    LayoutFix fix = LayoutFix::none;
    bool verified = false;
    // One sentence a person can act on; empty for a supported layout.
    std::string reason;

    // Whether PUT /layout takes it - and so whether the sheet can apply it.
    [[nodiscard]] bool accepted() const { return verdict != LayoutVerdict::unavailable; }
};

namespace builder_detail {

[[nodiscard]] inline PickVerdict own(const LayoutOption& option, std::string name) {
    PickVerdict out;
    out.name = std::move(name);
    out.slots = option.slots;
    out.verdict = option.verdict;
    out.fix = option.fix;
    out.verified = option.verified;
    out.reason = option.reason;
    return out;
}

// The verdict for `slots` outputs called `label` in the sentence: the more-than-sixteen rule first, then the board's.
[[nodiscard]] inline PickVerdict judge_slots(const BoardCaps& caps, std::string name, std::string_view label,
                                             std::size_t slots) {
    if (slots > kMaxLayoutOutputs) {
        PickVerdict out;
        out.name = std::move(name);
        out.slots = slots;
        out.fix = LayoutFix::none;
        out.reason = std::string{label} + " would be " + std::to_string(slots) + " outputs, and a layout has at most " +
                     std::to_string(kMaxLayoutOutputs) + ".";
        return out;
    }
    return own(evaluate_layout(caps, label, slots), std::move(name));
}

}  // namespace builder_detail

// Whether this board takes the layout the pick makes, and if not why.
[[nodiscard]] inline PickVerdict judge_pick(const BoardCaps& caps, const LayoutPick& pick) {
    std::string name = pick_name(pick);
    const std::string label = name;
    return builder_detail::judge_slots(caps, std::move(name), label, pick_slots(pick));
}

// --- the buttons ---------------------------------------------------------------------------------------

enum class PickField : std::uint8_t { kAround, kSubwoofers, kHeights, kMount };

// One button of a group.
struct PickStep {
    // The field's value: a count, or the HeightMount.
    int value = 0;
    std::string label;
    bool available = true;
    // Why a greyed button is greyed; empty when it is not.
    std::string reason;
};

namespace builder_detail {

[[nodiscard]] inline std::string count_label(int count, bool none_word) {
    if (count == 0 && none_word) {
        return "None";
    }
    return std::to_string(count);
}

[[nodiscard]] inline std::string subwoofer_label(int count) {
    return count == 0 ? "None" : count == 1 ? "One" : "Two";
}

}  // namespace builder_detail

// "Nine around you": what a greyed button's reason calls the layout, because "9.0 needs 9 slots" says something the
// person did not ask for.
[[nodiscard]] inline std::string step_phrase(PickField field, int value) {
    using builder_detail::word_of;
    std::string out;
    switch (field) {
        case PickField::kAround: out = std::string{word_of(value)} + " around you"; break;
        case PickField::kSubwoofers: out = std::string{word_of(value)} + (value == 1 ? " subwoofer" : " subwoofers"); break;
        case PickField::kHeights: out = std::string{word_of(value)} + " heights"; break;
        case PickField::kMount: return {};
    }
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

// The buttons of a group, each with whether the board can have it. See the header comment for what greyed means.
[[nodiscard]] inline std::vector<PickStep> pick_steps(const BoardCaps& caps, const LayoutPick& pick, PickField field) {
    std::vector<PickStep> out;
    const auto least_with = [&](int around, int subs, int heights) {
        return LayoutPick{around, subs, heights, HeightMount::kHigh};
    };
    const auto add = [&](int value, std::string label, const LayoutPick& smallest, PickField which) {
        PickStep step;
        step.value = value;
        step.label = std::move(label);
        const PickVerdict verdict =
            builder_detail::judge_slots(caps, pick_name(smallest), step_phrase(which, value), pick_slots(smallest));
        step.available = verdict.accepted();
        if (!step.available) {
            step.reason = verdict.reason;
        }
        out.push_back(std::move(step));
    };
    switch (field) {
        case PickField::kAround:
            for (const int value : kAroundSteps) {
                add(value, std::to_string(value), least_with(value, 0, 0), field);
            }
            break;
        case PickField::kSubwoofers:
            for (const int value : kSubwooferSteps) {
                add(value, builder_detail::subwoofer_label(value), least_with(1, value, 0), field);
            }
            break;
        case PickField::kHeights:
            for (const int value : kHeightSteps) {
                add(value, builder_detail::count_label(value, true), least_with(1, 0, value), field);
            }
            break;
        case PickField::kMount: {
            const bool has_heights = pick.heights > 0;
            const char* const reason = "Only a layout with heights has them to mount.";
            const std::array<std::pair<HeightMount, const char*>, kMountSteps> mounts{{
                {HeightMount::kHigh, "High"},
                {HeightMount::kCeiling, "In the ceiling"},
            }};
            for (const auto& [mount, label] : mounts) {
                PickStep step;
                step.value = static_cast<int>(mount);
                step.label = label;
                step.available = has_heights;
                if (!has_heights) {
                    step.reason = reason;
                }
                out.push_back(std::move(step));
            }
            break;
        }
    }
    return out;
}

// The pick with one field set. Heights of none put the mount back to High (it has nothing to say then).
[[nodiscard]] inline LayoutPick with_field(LayoutPick pick, PickField field, int value) {
    switch (field) {
        case PickField::kAround: pick.around = value; break;
        case PickField::kSubwoofers: pick.subwoofers = value; break;
        case PickField::kHeights:
            pick.heights = value;
            if (value == 0) {
                pick.mount = HeightMount::kHigh;
            }
            break;
        case PickField::kMount: pick.mount = static_cast<HeightMount>(value); break;
    }
    return pick;
}

[[nodiscard]] constexpr int field_value(const LayoutPick& pick, PickField field) {
    switch (field) {
        case PickField::kAround: return pick.around;
        case PickField::kSubwoofers: return pick.subwoofers;
        case PickField::kHeights: return pick.heights;
        case PickField::kMount: return static_cast<int>(pick.mount);
    }
    return 0;
}

// --- the quick picks -----------------------------------------------------------------------------------

enum class QuickGroup : std::uint8_t { kAroundYou, kWithHeights };

struct QuickPick {
    std::string_view name;
    QuickGroup group;
};

// The layouts a home has, in the order a person looks for them: widest last in each group.
inline constexpr std::array<QuickPick, 15> kQuickPicks{{
    {"1.0", QuickGroup::kAroundYou},
    {"2.0", QuickGroup::kAroundYou},
    {"2.1", QuickGroup::kAroundYou},
    {"3.1", QuickGroup::kAroundYou},
    {"5.0", QuickGroup::kAroundYou},
    {"5.1", QuickGroup::kAroundYou},
    {"7.1", QuickGroup::kAroundYou},
    {"9.1", QuickGroup::kAroundYou},
    {"5.1.2", QuickGroup::kWithHeights},
    {"5.1.4", QuickGroup::kWithHeights},
    {"7.1.2", QuickGroup::kWithHeights},
    {"7.1.4", QuickGroup::kWithHeights},
    {"9.1.2", QuickGroup::kWithHeights},
    {"9.1.4", QuickGroup::kWithHeights},
    {"9.1.6", QuickGroup::kWithHeights},
}};

[[nodiscard]] constexpr std::string_view group_name(QuickGroup group) {
    return group == QuickGroup::kAroundYou ? "Around you" : "With heights";
}

// A quick pick with the board's verdict on it.
struct QuickState {
    LayoutPick pick;
    PickVerdict verdict;
    QuickGroup group = QuickGroup::kAroundYou;
};

[[nodiscard]] inline std::vector<QuickState> quick_states(const BoardCaps& caps) {
    std::vector<QuickState> out;
    out.reserve(kQuickPicks.size());
    for (const QuickPick& quick : kQuickPicks) {
        const std::optional<LayoutPick> pick = pick_of(quick.name);
        if (!pick) {
            continue;
        }
        out.push_back({*pick, judge_pick(caps, *pick), quick.group});
    }
    return out;
}

// --- what the board carries ----------------------------------------------------------------------------

// The sentence at the head of the sheet: what this board's output carries and what would change that. "This board
// carries up to 4 speakers (32-bit slots, one line); 16-bit slots would carry 8."
[[nodiscard]] inline std::string capacity_line(const BoardCaps& caps) {
    const SinkCaps& sink = caps.sink;
    const std::size_t now = detail::ceiling_at(sink, sink.slot_bits, sink.second_line);
    if (now == 0) {
        return "This board has no audio output to set a layout for.";
    }
    if (!sink.tdm_usable) {
        std::string clause = tdm_blocked_clause(caps.hardware);
        if (clause.empty()) {
            clause = "This chip has no clock for multi-channel output";
        }
        return clause + ", so only 1.0 and 2.0 can play.";
    }
    // A layout is at most sixteen outputs, so a board that carries more says sixteen.
    const std::size_t shown = std::min(now, kMaxLayoutOutputs);
    std::string out = "This board carries up to " + detail::number(shown) + (shown == 1 ? " speaker" : " speakers") +
                      detail::setup(sink);
    if (now < kMaxLayoutOutputs) {
        const detail::Reach reach = detail::reach(sink, now + 1);
        const std::string carries = detail::number(std::min(reach.carries, kMaxLayoutOutputs));
        switch (reach.fix) {
            case LayoutFix::slot_width: out += "; 16-bit slots would carry " + carries; break;
            case LayoutFix::second_line: out += "; a second line wired would carry " + carries; break;
            case LayoutFix::slot_width_and_second_line:
                out += "; 16-bit slots and a second line would carry " + carries;
                break;
            default: break;
        }
    }
    return out + ".";
}

// --- applying -------------------------------------------------------------------------------------------

// Sets the layout a pick makes through the preferences' backend, as set_value() sets a choice: the entry's own
// availability (a board with no output), then the verdict - what PUT /layout would answer, with its sentence - and only
// then the board. The reason of a refusal is the sentence the sheet shows; a layout taken with a limit is taken.
[[nodiscard]] inline prefs::SetResult apply_pick(prefs::Backend& backend, const LayoutPick& pick) {
    const BoardCaps caps = backend.caps();
    if (const prefs::Pref* const entry = prefs::find("speaker-layout"); entry != nullptr) {
        const prefs::Availability whole = prefs::availability_of(*entry, caps);
        if (!whole.available) {
            return {false, whole.reason};
        }
    }
    const PickVerdict verdict = judge_pick(caps, pick);
    if (!verdict.accepted()) {
        return {false, verdict.reason};
    }
    if (!backend.set_layout(verdict.name)) {
        return {false, "The board could not take that."};
    }
    return {true, {}};
}

}  // namespace iclforge
