#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/board_caps.hpp"
#include "iclforge/sink_plan.hpp"

// Which speaker layouts a board can be set to, and why not for the ones it cannot: the ONE verdict behind
// PUT /layout (accept_layout, hearth_sink.cpp), GET /layout-options, the Preferences registry's speaker-layout
// choices (preferences.hpp) and the device web page's greyed presets. All of them ask evaluate_layout() over the
// board's BoardCaps (board_caps.hpp), so a layout the screen offers is one the route takes, and a layout the
// route refuses is greyed on the screen with the sentence the route answers with.
//
// A board's layout is a name (5.1.4) or a speaker list; what it can carry depends on the DAC wiring and the
// silicon, not on the layout: the slot width and the second I2S line (the sink's configuration), the TDM frame
// the part opens, whether the chip can open one at all (an ESP32-P4 below v3.0 cannot), and the widest stream
// the build decodes. This header turns those into a verdict per layout. Free of ESP-IDF, like board_caps.hpp:
// libs/device/tests/test_layout_options.cpp runs it for every board in board_variants.hpp, with libs/render's own
// parser as the judge of what a layout is.
//
// Three verdicts.
//   unavailable   The board cannot play it, and PUT /layout refuses it with the sentence. The reason says what
//                 the layout needs and what the board has, and `fix` says whether a setting (the slot width, the
//                 second line) would change that.
//   with_limits   The board takes it, and something known or not yet known sits on it: a build that decodes only
//                 stereo, a stream width measured falling behind, a second line never checked with a DAC, or a
//                 width nobody has measured on this kind of board. PUT /layout accepts it and answers with the
//                 sentence as a note; a Preferences choice is available and carries it.
//   supported     Accepted, and a play this wide has been measured keeping up on this kind of board.
//
// "Unavailable" is exactly what the sink cannot carry: a layout is accepted if and only if it has no more slots
// than SinkCaps::slots(), which is what sink_slots() reports (a chip that cannot open TDM reports two). Nothing
// the screen offers can be refused by the route, and nothing the route refuses is offered.
//
// The evidence is a table keyed by the build's profile id (firmware/hearth-sink/profiles.json), each row with the
// record it comes from. A profile with no row, or a scratch build, has measured nothing beyond stereo, which every
// board plays, and says so ("not verified") rather than inventing a limit.

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

// The layouts the chooser lists. Not every name the grammar reads (layout.hpp reads F.L.H for 84
// combinations, and a speaker list besides): the ones a home has, in the order a person looks for
// them, widest last. Any other name or list is still judged by evaluate_layout() and taken by PUT /layout up to
// what the sink carries. The test holds each `slots` to OutputLayout::parse(name)->slots().
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
inline constexpr std::array<LayoutEvidence, 5> kLayoutEvidence{{
    {"s3-n16r8", 2, 12, "a 7.1.4 Atmos stream took 1.08 of real time on this board with Wi-Fi up"},
    {"c6-16mb", 2, 0, {}},
    {"c6-4mb", 2, 0, {}},
    {"p4-rev1", 2, 0, {}},
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

    // Whether PUT /layout takes it.
    [[nodiscard]] bool accepted() const { return verdict != LayoutVerdict::unavailable; }
};

namespace detail {

[[nodiscard]] constexpr bool wired(const SinkCaps& sink) { return sink.second_line && sink.second_line_possible; }

// The slots the sink carries at a slot width and wiring, whether or not the chip can clock them (tdm_usable is a
// rule of its own, tried first). 0 for a width the sink does not have.
[[nodiscard]] constexpr std::size_t ceiling_at(const SinkCaps& sink, int bits, bool second_line) {
    if (sink.frame_bit_max <= 0) {
        return sink.fixed_slots > 0 ? static_cast<std::size_t>(sink.fixed_slots) : 0;
    }
    return sink_ceiling(bits, second_line && sink.second_line_possible, sink.frame_bit_max);
}

// A sink with a TDM frame whose slot width is a setting; a capture or null sink has one it was built with, which
// no setting moves.
[[nodiscard]] constexpr bool settable(const SinkCaps& sink) {
    return sink.frame_bit_max > 0 && (sink.slot_bits == 16 || sink.slot_bits == 32);
}

[[nodiscard]] inline std::string number(std::size_t n) { return std::to_string(n); }

// "32-bit slots, one line": what the ceiling in force is made of.
[[nodiscard]] inline std::string setup(const SinkCaps& sink) {
    if (!settable(sink)) {
        return {};
    }
    return " (" + std::to_string(sink.slot_bits) + "-bit slots, " + (wired(sink) ? "two lines" : "one line") + ")";
}

// The cheapest setting change that reaches `slots`, and the ceiling it gives; hardware when none does.
struct Reach {
    LayoutFix fix = LayoutFix::hardware;
    std::size_t carries = 0;
};

[[nodiscard]] constexpr Reach reach(const SinkCaps& sink, std::size_t slots) {
    if (!settable(sink)) {
        return {};
    }
    const bool is_wired = wired(sink);
    const bool can_narrow = sink.slot_bits != 16;
    const bool can_wire = sink.second_line_possible && !is_wired;
    if (can_narrow) {
        const std::size_t narrower = ceiling_at(sink, 16, is_wired);
        if (narrower >= slots) {
            return {LayoutFix::slot_width, narrower};
        }
    }
    if (can_wire) {
        const std::size_t with_line = ceiling_at(sink, sink.slot_bits, true);
        if (with_line >= slots) {
            return {LayoutFix::second_line, with_line};
        }
    }
    if (can_narrow && can_wire) {
        const std::size_t both = ceiling_at(sink, 16, true);
        if (both >= slots) {
            return {LayoutFix::slot_width_and_second_line, both};
        }
    }
    return {};
}

}  // namespace detail

// The verdict for one layout of `slots` slots called `name`: a name from the catalog, or whatever text PUT /layout
// was sent (a long speaker list is "This layout" in the sentence).
[[nodiscard]] inline LayoutOption evaluate_layout(const BoardCaps& caps, std::string_view name, std::size_t slots) {
    using detail::number;
    const SinkCaps& sink = caps.sink;
    LayoutOption out;
    out.name = name;
    out.slots = slots;
    const std::string label = name.size() <= 24 && name.find(',') == std::string_view::npos
                                  ? std::string{name}
                                  : std::string{"This layout"};

    const std::size_t now = detail::ceiling_at(sink, sink.slot_bits, sink.second_line);
    if (now == 0) {
        out.reason = "This board has no audio output to set a layout for.";
        out.fix = LayoutFix::hardware;
        return out;
    }

    // The chip cannot open a TDM frame, so nothing past stereo opens whatever the frame says.
    if (!sink.tdm_usable && slots > 2) {
        std::string clause = tdm_blocked_clause(caps.hardware);
        if (clause.empty()) {
            clause = "This chip has no clock for multi-channel output";
        }
        out.fix = LayoutFix::hardware;
        out.reason = clause + ", so only 1.0 and 2.0 can play.";
        return out;
    }

    // Past what the sink carries now: the sentence names the need, the supply and the way out.
    if (slots > now) {
        const detail::Reach reach = detail::reach(sink, slots);
        out.fix = reach.fix;
        out.reason = label + " needs " + number(slots) + " slots; this board's output carries " + number(now) +
                     detail::setup(sink) + ".";
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
                              number(static_cast<std::size_t>(sink.slots_most())) + ".";
                break;
        }
        return out;
    }

    const LayoutEvidence evidence = layout_evidence(caps.profile);
    const std::size_t proven = evidence.proven_slots > kAlwaysProvenSlots ? evidence.proven_slots
                                                                          : kAlwaysProvenSlots;
    out.verdict = LayoutVerdict::with_limits;

    // The build decodes only narrow streams: a wider layout plays through no more speakers than the stream has.
    if (caps.coded_channels_max > 0 && slots > static_cast<std::size_t>(caps.coded_channels_max)) {
        const std::size_t cap = static_cast<std::size_t>(caps.coded_channels_max);
        out.reason = "This build decodes streams of at most " + number(cap) +
                     " channels, so a wider stream is decoded elsewhere and sent narrower, and " + label +
                     " plays through no more than " + number(cap) + " of its speakers.";
        return out;
    }

    // The second line carries this layout: the part can run it, nobody has put a DAC on both lines.
    if (detail::wired(sink) && detail::settable(sink)) {
        const std::size_t one_line = detail::ceiling_at(sink, sink.slot_bits, false);
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
[[nodiscard]] inline std::vector<LayoutOption> layout_options(const BoardCaps& caps) {
    std::vector<LayoutOption> out;
    out.reserve(kLayoutCatalog.size());
    for (const CatalogLayout& layout : kLayoutCatalog) {
        out.push_back(evaluate_layout(caps, layout.name, layout.slots));
    }
    return out;
}

// --- GET /layout-options ---------------------------------------------------------------------------------
//
// The head of the answer, then one object a layout: control.cpp sends them one at a time (the list is about 3 KB
// and the server task has 6 KB of stack), and layout_options_json() is the same bytes in one string for the tests
// and for a client that wants one. The keys are the contract the device page and the stand-in in
// apps/demos/wasm/tests/device-ui/stub.js hold to.

namespace detail {

inline void json_text(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        if (static_cast<unsigned char>(c) >= 0x20) {
            out += c;
        }
    }
    out += '"';
}

}  // namespace detail

// {"layout":"2.0","slots":4,"slot_bits":32,"second_line":false,"profile":"s3-n16r8","options":[
// `current` is the layout in force (GET /layout); `slots` is what the sink carries now (/status's sink_slots).
[[nodiscard]] inline std::string layout_options_head_json(const BoardCaps& caps, std::string_view current) {
    std::string out = "{\"layout\":";
    detail::json_text(out, current);
    out += ",\"slots\":" + std::to_string(caps.sink.slots());
    if (caps.sink.slot_bits > 0) {
        out += ",\"slot_bits\":" + std::to_string(caps.sink.slot_bits);
    }
    if (caps.sink.second_line_possible) {
        out += caps.sink.second_line ? ",\"second_line\":true" : ",\"second_line\":false";
    }
    out += ",\"profile\":";
    detail::json_text(out, caps.profile);
    out += ",\"options\":[";
    return out;
}

[[nodiscard]] inline std::string layout_option_json(const LayoutOption& option, bool first) {
    std::string out = first ? "{\"layout\":" : ",{\"layout\":";
    detail::json_text(out, option.name);
    out += ",\"slots\":" + std::to_string(option.slots);
    out += ",\"verdict\":";
    detail::json_text(out, word(option.verdict));
    out += ",\"fix\":";
    detail::json_text(out, word(option.fix));
    out += option.verified ? ",\"verified\":true" : ",\"verified\":false";
    if (!option.reason.empty()) {
        out += ",\"reason\":";
        detail::json_text(out, option.reason);
    }
    out += '}';
    return out;
}

[[nodiscard]] inline std::string layout_options_json(const BoardCaps& caps, std::string_view current) {
    std::string out = layout_options_head_json(caps, current);
    bool first = true;
    for (const LayoutOption& option : layout_options(caps)) {
        out += layout_option_json(option, first);
        first = false;
    }
    out += "]}\n";
    return out;
}

}  // namespace iclforge
