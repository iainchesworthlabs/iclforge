#pragma once

#include <cstdint>
#include <optional>

// Whether an E-AC-3 stream is one a board plays as its bed, because it declares more objects than
// the board says it places in real time (the support object's max_objects, which the firmware
// states from CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_OBJECTS).
//
// The count is read from the first unit's header, before any decoding: the OBA complexity index of
// TS 103 420 (the addbsi's, ScannedStream::oba_complexity_index) is the number of objects plus the
// LFE the stream was coded with, so a stream's declared objects are the index less one. That is the
// figure a server falls back to when it cannot count a stream's JOC payload
// (apps/hearth/engine/src/session.cpp), and what a board can say before it allocates the object
// reconstruction, which is the point: a board that would not keep up with the objects does not
// allocate its 147 KB for them either.
//
// A stream with no index is a channel-based one, with no objects to place.

namespace iclforge {

// The objects a stream declares, from its complexity index: none when it has no index.
[[nodiscard]] constexpr int declared_objects(std::optional<int> oba_complexity_index) {
    if (!oba_complexity_index || *oba_complexity_index < 1) {
        return 0;
    }
    return *oba_complexity_index - 1;
}

// Whether the stream declares more objects than `limit`: a board with no figure (none stated)
// places whatever it is sent, and a figure of 0 is a board that places none.
[[nodiscard]] constexpr bool objects_past_limit(std::optional<int> oba_complexity_index,
                                                std::optional<std::uint8_t> limit) {
    return limit.has_value() && declared_objects(oba_complexity_index) > static_cast<int>(*limit);
}

}  // namespace iclforge
