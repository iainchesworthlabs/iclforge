#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// How much of a Dolby stream's dynamic range a listener wants to hear, with no codec in it.
//
// The streams a board decodes itself (AC-3, E-AC-3 with or without JOC, AC-4) carry the mixer's own
// instructions for turning the loud passages down and the quiet ones up, so that a film can be
// heard at night without the neighbours hearing the explosions. What those instructions are, and
// which of them a decoder can apply, differs by codec: iclforge/ac3/decoder/dynamic_range.hpp and
// iclforge/ac4/decoder/dynamic_range.hpp say what each choice means to theirs. The choice itself is
// here, header-only and standard library only, so that the settings of a board, its REST route, its
// screen and the two decoders' mappings all name the same two things.
//
// Nothing here is exported. The stored values are these numbers: never renumbered, never reused.

namespace iclforge::base {

enum class DynamicRange : std::uint8_t {
    // What a decoder does without being asked: AC-3 and E-AC-3 as the build's operating mode and a
    // server's own decoder settings say, AC-4 as coded. The default, and bit for bit what a board
    // did before this choice existed.
    kStandard = 0,
    // Late night: the loud passages down and the quiet ones up as far as the stream's own heavy
    // compression allows, with dialogue left where the stream's dialnorm puts it. The listener's
    // choice at the device, so it wins over a server's mode and compression settings for as long
    // as it is on.
    kLateNight = 1,
};

inline constexpr DynamicRange kDefaultDynamicRange = DynamicRange::kStandard;

// The words PUT /dynamic-range takes and GET /dynamic-range and /status report.
[[nodiscard]] constexpr std::string_view dynamic_range_name(DynamicRange range) {
    return range == DynamicRange::kLateNight ? "late-night" : "standard";
}

namespace detail {

[[nodiscard]] constexpr std::string_view trimmed_word(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace detail

// One of the two words, with or without a line ending after it.
[[nodiscard]] constexpr std::optional<DynamicRange> parse_dynamic_range(std::string_view text) {
    text = detail::trimmed_word(text);
    for (const DynamicRange range : {DynamicRange::kStandard, DynamicRange::kLateNight}) {
        if (text == dynamic_range_name(range)) {
            return range;
        }
    }
    return std::nullopt;
}

// A stored number, or nothing for one this build does not know (a partition written by another build).
[[nodiscard]] constexpr std::optional<DynamicRange> dynamic_range_from_stored(std::int32_t value) {
    if (value == static_cast<std::int32_t>(DynamicRange::kStandard)) {
        return DynamicRange::kStandard;
    }
    if (value == static_cast<std::int32_t>(DynamicRange::kLateNight)) {
        return DynamicRange::kLateNight;
    }
    return std::nullopt;
}

}  // namespace iclforge::base
