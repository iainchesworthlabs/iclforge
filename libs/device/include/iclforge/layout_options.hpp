#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/sink_plan.hpp"

// Which speaker layouts a board can be set to, and why not for the ones it cannot: the answer behind
// GET /layout-options, the device web page's greyed presets and the screen's Preferences list.
//
// A board's layout is a name (5.1.4) or a speaker list; what it can carry depends on the DAC wiring
// and the silicon, not on the layout. The facts are a board's own, gathered once from the sink
// (sink_slots(), sink_max_slots(), the slot width and the second line) and from the build (the
// profile id, the widest coded stream the Sendspin player decodes) and the chip (a revision that
// cannot open TDM). This header only turns them into a verdict per layout, so a wrong sentence is a
// wrong rule here or a wrong input, never something only a board shows. Free of ESP-IDF, like
// sink_plan.hpp beside it; libs/device/tests/test_layout_options.cpp runs it for every board the
// project builds for, with libs/render's own parser as the judge of what a layout is.
//
// Three verdicts.
//   unavailable   PUT /layout would not play it, or this board cannot play it at all. The reason says
//                 what the layout needs and what the board has, and `fix` says whether a setting
//                 (the slot width, the second line) would change that.
//   with_limits   The board accepts it, and something known or not yet known sits on it: a build that
//                 decodes only stereo, a stream width measured falling behind, a second line never
//                 checked with a DAC, or a width nobody has measured on this kind of board.
//   supported     Accepted, and a play this wide has been measured keeping up on this kind of board.
//
// "Unavailable" is never looser than accept_layout() (hearth_sink.cpp), which refuses a layout with
// more slots than sink_slots(): everything this calls available or limited is accepted by PUT
// /layout, and everything accept_layout() refuses is unavailable here. The other direction is
// deliberately not equal. A chip below revision v3.0 (an ESP32-P4) accepts any layout and then
// fails to open TDM at the play; PUT /layout stays permissive, because the board suite sets a 5.1
// layout on such a board to test the fold, and the listing is where a person is told it cannot work.
//
// The evidence is a table keyed by the build's profile id (firmware/hearth-sink/profiles.json), each
// row with the record it comes from. A profile with no row, or a scratch build, has measured nothing
// beyond stereo, which every board plays, and says so ("not verified") rather than inventing a limit.

namespace iclforge {

enum class LayoutVerdict : std::uint8_t { supported, with_limits, unavailable };

// What a person could change to make an unavailable layout available.
enum class LayoutFix : std::uint8_t {
    none,                        // nothing to change (the layout is available)
    slot_width,                  // 16-bit slots carry more of them
    second_line,                 // a second I2S line, if one is wired
    slot_width_and_second_line,  // both
    hardware,                    // no setting reaches it: a different board, or a newer chip
};

[[nodiscard]] constexpr std::string_view word(LayoutVerdict verdict) {
    switch (verdict) {
        case LayoutVerdict::supported: return "supported";
        case LayoutVerdict::with_limits: return "limits";
        case LayoutVerdict::unavailable: return "unavailable";
    }
    return "unavailable";
}

[[nodiscard]] constexpr std::string_view word(LayoutFix fix) {
    switch (fix) {
        case LayoutFix::none: return "none";
        case LayoutFix::slot_width: return "slot-width";
        case LayoutFix::second_line: return "second-line";
        case LayoutFix::slot_width_and_second_line: return "slot-width-and-second-line";
        case LayoutFix::hardware: return "hardware";
    }
    return "hardware";
}

// What the board says about itself. Every field is filled by the owner of the board from the same
// places GET /status and GET /hardware read; 0 and empty mean "not stated".
struct LayoutFacts {
    // The build's locked profile id ("s3-n16r8", "p4-rev3"): ICLFORGE_PROFILE_ID. Selects the
    // evidence row; empty for a build that states none.
    std::string_view profile;
    // player::sink_slots(): the ceiling for the settings in force, and the number accept_layout()
    // refuses a layout past. 0 means the board has no audio output.
    int slots_now = 0;
    // player::sink_slot_bits(): the slot width in force, 16 or 32; 0 for a sink whose ceiling is not
    // a function of the width (a capture or null sink), which no setting changes.
    int slot_bits = 0;
    // player::sink_max_slots() and the width it is reached at (sink_max_slots_bit_width()): the most
    // the sink carries at any setting, with a second line where the part has a controller for one.
    int slots_max = 0;
    int slots_max_bits = 0;
    // Whether the part can run a second line at all (sink_second_line_possible()), and whether the
    // board's settings say one is wired (Settings::second_line).
    bool second_line_possible = false;
    bool second_line_wired = false;
    // The widest syncframe, in coded channels, the Sendspin player decodes in this build
    // (CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS); 0 states no limit.
    int coded_channels_max = 0;
    // Non-empty when the chip cannot open TDM above two channels (an ESP32-P4 below revision v3.0:
    // HardwareFacts::revision_hard_limit_below), as one short clause: "This chip (v1.3) has no clock
    // for multi-channel output".
    std::string_view tdm_blocked;
};

// The layouts the chooser lists. Not every name the grammar reads (layout.hpp reads F.L.H for 84
// combinations, and a speaker list besides): the ones a home has, in the order a person looks for
// them, widest last. Any other name or list is still accepted by PUT /layout up to the sink's slots.
// The test holds each `slots` to OutputLayout::parse(name)->slots().
struct CatalogLayout {
    std::string_view name;
    std::size_t slots;
};

inline constexpr std::array<CatalogLayout, 13> kLayoutCatalog{{
    {"1.0", 1},
    {"2.0", 2},
    {"2.1", 3},
    {"5.0", 5},
    {"5.1", 6},
    {"5.1.2", 8},
    {"7.1", 8},
    {"5.1.4", 10},
    {"7.1.2", 10},
    {"7.1.4", 12},
    {"9.1.2", 12},
    {"9.1.4", 14},
    {"9.1.6", 16},
}};

// What has been measured on a kind of board, from the records the comments name. `proven_slots` is
// the widest play, in output slots, that kept up with no underruns; stereo is always proven (every
// profile's baseline plays 2.0). `behind_slots` is the layout width from which a stream was measured
// falling behind (0: none measured), with `behind` saying what was measured.
struct LayoutEvidence {
    std::string_view profile;
    std::size_t proven_slots;
    std::size_t behind_slots;
    std::string_view behind;
};

// planning/esp32-platform-profiles.md's baselines (the 2.0 rows on every profile; p4-rev3's
// ac3-5.1-tdm16 row: AC-3 5.1 rendered onto 9.1.6, sixteen 32-bit TDM slots, 142 permille of a frame,
// no underruns, on the kit, 2026-10-20), and planning/esp32-sink-compatibility-matrix.md, Matrix 1
// (the S3 plays a 7.1.4 Atmos stream "at the line", 1.08 of real time with Wi-Fi up). No S3 or C6
// profile has a play wider than stereo in its suite, so theirs stop at 2.
inline constexpr std::array<LayoutEvidence, 7> kLayoutEvidence{{
    {"s3-n16r8", 2, 12, "a 7.1.4 Atmos stream took 1.08 of real time on this board with Wi-Fi up"},
    {"s3-n16r8-ac4", 2, 12, "a 7.1.4 Atmos stream took 1.08 of real time on this board with Wi-Fi up"},
    {"c6-16mb", 2, 0, {}},
    {"c6-4mb", 2, 0, {}},
    {"p4-rev1", 2, 0, {}},
    {"p4-rev1-ac4", 2, 0, {}},
    {"p4-rev3", 16, 0, {}},
}};

// Stereo plays on every board that plays anything.
inline constexpr std::size_t kAlwaysProvenSlots = 2;

[[nodiscard]] constexpr LayoutEvidence layout_evidence(std::string_view profile) {
    for (const LayoutEvidence& row : kLayoutEvidence) {
        if (row.profile == profile) {
            return row;
        }
    }
    return {profile, kAlwaysProvenSlots, 0, {}};
}

// accept_layout()'s rule for a layout that parses: no more slots than the sink has now.
[[nodiscard]] constexpr bool layout_fits_sink(std::size_t slots, int sink_slots) {
    return sink_slots > 0 && slots <= static_cast<std::size_t>(sink_slots);
}

struct LayoutOption {
    std::string_view name;
    std::size_t slots = 0;
    LayoutVerdict verdict = LayoutVerdict::unavailable;
    LayoutFix fix = LayoutFix::none;
    // True when the verdict rests on a play measured on this kind of board; false for "not verified"
    // and for an unavailable layout.
    bool verified = false;
    // One sentence a person can act on; empty for a supported layout.
    std::string reason;
};

namespace detail {

// The TDM frame, in bits, that sink_max_slots() was reached at: the widest width's slot count on
// one line times the width (sink_plan.hpp: slots = frame_bit_max / slot_bits). 0 when the sink states
// no width.
[[nodiscard]] constexpr int frame_bits(const LayoutFacts& facts) {
    if (facts.slots_max <= 0 || facts.slots_max_bits <= 0) {
        return 0;
    }
    const int lines = facts.second_line_possible ? 2 : 1;
    return (facts.slots_max / lines) * facts.slots_max_bits;
}

// The ceiling at a slot width and wiring the board could be set to; 0 when the sink states none.
[[nodiscard]] constexpr std::size_t ceiling_at(const LayoutFacts& facts, int bits, bool second_line) {
    const int frame = frame_bits(facts);
    if (frame == 0) {
        return 0;
    }
    return sink_ceiling(bits, second_line && facts.second_line_possible, frame);
}

[[nodiscard]] inline std::string number(std::size_t n) { return std::to_string(n); }

// "32-bit slots, one line": what the ceiling in force is made of.
[[nodiscard]] inline std::string setup(const LayoutFacts& facts) {
    // A sink with no width to speak of (capture, null) reports one it was built with, which no
    // setting moves and its ceiling does not follow.
    if (facts.slot_bits == 0 || frame_bits(facts) == 0) {
        return {};
    }
    return " (" + std::to_string(facts.slot_bits) + "-bit slots, " +
           (facts.second_line_wired && facts.second_line_possible ? "two lines" : "one line") + ")";
}

// The cheapest setting change that reaches `slots`, and the ceiling it gives; hardware when none does.
struct Reach {
    LayoutFix fix = LayoutFix::hardware;
    std::size_t carries = 0;
};

[[nodiscard]] constexpr Reach reach(const LayoutFacts& facts, std::size_t slots) {
    if (facts.slot_bits != 16 && facts.slot_bits != 32) {
        return {};
    }
    const bool wired = facts.second_line_wired && facts.second_line_possible;
    const bool can_narrow = facts.slot_bits != 16;
    const bool can_wire = facts.second_line_possible && !wired;
    if (can_narrow) {
        const std::size_t narrower = ceiling_at(facts, 16, wired);
        if (narrower >= slots) {
            return {LayoutFix::slot_width, narrower};
        }
    }
    if (can_wire) {
        const std::size_t with_line = ceiling_at(facts, facts.slot_bits, true);
        if (with_line >= slots) {
            return {LayoutFix::second_line, with_line};
        }
    }
    if (can_narrow && can_wire) {
        const std::size_t both = ceiling_at(facts, 16, true);
        if (both >= slots) {
            return {LayoutFix::slot_width_and_second_line, both};
        }
    }
    return {};
}

}  // namespace detail

// The verdict for one layout of `slots` slots called `name`. `name` is only echoed back.
[[nodiscard]] inline LayoutOption evaluate_layout(const LayoutFacts& facts, std::string_view name,
                                                  std::size_t slots) {
    using detail::number;
    LayoutOption out;
    out.name = name;
    out.slots = slots;
    const std::string label{name};

    if (facts.slots_now <= 0) {
        out.reason = "This board has no audio output to set a layout for.";
        out.fix = LayoutFix::hardware;
        return out;
    }

    // Past what the sink carries now: the sentence names the need, the supply and the way out.
    if (!layout_fits_sink(slots, facts.slots_now)) {
        const std::size_t now = static_cast<std::size_t>(facts.slots_now);
        const detail::Reach reach = detail::reach(facts, slots);
        out.fix = reach.fix;
        out.reason = label + " needs " + number(slots) + " slots; this board's output carries " +
                     number(now) + detail::setup(facts) + ".";
        switch (reach.fix) {
            case LayoutFix::slot_width:
                out.reason += " 16-bit slots would carry " + number(reach.carries) + ".";
                break;
            case LayoutFix::second_line:
                out.reason += " A second line wired would carry " + number(reach.carries) + ".";
                break;
            case LayoutFix::slot_width_and_second_line:
                out.reason += " 16-bit slots and a second line would carry " + number(reach.carries) + ".";
                break;
            default:
                out.reason += " The most this board can carry is " +
                              number(static_cast<std::size_t>(facts.slots_max > 0 ? facts.slots_max : facts.slots_now)) +
                              ".";
                break;
        }
        return out;
    }

    // Accepted by the route, but the chip cannot open the output above stereo.
    if (!facts.tdm_blocked.empty() && slots > 2) {
        out.fix = LayoutFix::hardware;
        out.reason = std::string{facts.tdm_blocked} + ", so only 1.0 and 2.0 can play.";
        return out;
    }

    const LayoutEvidence evidence = layout_evidence(facts.profile);
    const std::size_t proven = evidence.proven_slots > kAlwaysProvenSlots ? evidence.proven_slots
                                                                          : kAlwaysProvenSlots;
    out.verdict = LayoutVerdict::with_limits;

    // The build decodes only narrow streams: a wider layout plays the front pair of whatever arrives.
    if (facts.coded_channels_max > 0 && slots > static_cast<std::size_t>(facts.coded_channels_max)) {
        const std::size_t cap = static_cast<std::size_t>(facts.coded_channels_max);
        out.reason = "This build decodes streams of at most " + number(cap) +
                     " channels, so a wider stream is decoded elsewhere and sent narrower, and " + label +
                     " plays through no more than " + number(cap) + " of its speakers.";
        return out;
    }

    // The second line carries this layout: the part can run it, nobody has put a DAC on both lines.
    if (facts.second_line_wired && facts.second_line_possible && facts.slot_bits != 0) {
        const std::size_t one_line = detail::ceiling_at(facts, facts.slot_bits, false);
        if (one_line != 0 && slots > one_line) {
            out.reason = label + " needs the second I2S line, which has been brought up on a development "
                                 "board but never checked with a DAC on both lines.";
            return out;
        }
    }

    // A stream this wide was measured falling behind.
    if (evidence.behind_slots != 0 && slots >= evidence.behind_slots) {
        out.reason = std::string{evidence.behind} + ", so " + label + " can break up.";
        return out;
    }

    // Wider than anything measured on this kind of board.
    if (slots > proven) {
        out.reason = "Not verified: the widest layout measured on this kind of board is " + number(proven) +
                     " slots.";
        return out;
    }

    out.verdict = LayoutVerdict::supported;
    out.verified = true;
    return out;
}

// The whole catalog, in its order.
[[nodiscard]] inline std::vector<LayoutOption> layout_options(const LayoutFacts& facts) {
    std::vector<LayoutOption> out;
    out.reserve(kLayoutCatalog.size());
    for (const CatalogLayout& layout : kLayoutCatalog) {
        out.push_back(evaluate_layout(facts, layout.name, layout.slots));
    }
    return out;
}

}  // namespace iclforge
