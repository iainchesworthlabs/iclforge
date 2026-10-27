#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "source_frame.hpp"

// Object mode's bed-pinned channels, split by what a pin is, and the LFE feed the split leaves
// (encoder_controller.cpp's encodeObjects and startMotionPreview). Header-only and free of Qt, so the
// Qt-free test binary can hold what it says.
//
// A speaker pin is an object held at its speaker's azimuth. An LFE pin is not an object at all: the
// LFE has no direction to hold one at, and an object reaches it only as lfe_send * gain, with that
// same gain panning it into the speakers - so a channel pinned there as an object (gain 0,
// lfe_send 1, which is how it was built) is silent, and spent one of the fifteen object slots being
// so. It is the bed's own LFE instead: AtmosEncoder::encode_frame's `lfe` span, no slot at all.
namespace forge_gui {

struct SplitPins {
    std::vector<std::pair<std::size_t, iclforge::ac3::eac3::chanmap::Location>> speakers;
    std::vector<std::size_t> lfes;  // flat channel indices, in source order
};

// `pinned` is EncoderController::pinnedObjectChannels(): (flat channel, bed location) pairs. LFE and
// LFE2 are the LFE pins; every other location is a speaker pin.
[[nodiscard]] inline SplitPins split_pins(
    const std::vector<std::pair<std::size_t, iclforge::ac3::eac3::chanmap::Location>>& pinned) {
    using iclforge::ac3::eac3::chanmap::Location;
    SplitPins out;
    for (const auto& pin : pinned) {
        if (pin.second == Location::kLfe || pin.second == Location::kLfe2) {
            out.lfes.push_back(pin.first);
        } else {
            out.speakers.push_back(pin);
        }
    }
    return out;
}

// One frame of the LFE feed from sample `start`: every LFE plane added together - a stream has one
// LFE, and two channels pinned to it (LFE and LFE2) go into it as one. Each plane holds its last
// sample past its end, as an object's does (source_frame.hpp). `scratch` is the same length as
// `out`.
inline void fill_lfe_frame(std::span<const std::vector<float>> planes, std::size_t start,
                           std::span<float> out, std::span<float> scratch) {
    std::ranges::fill(out, 0.0F);
    for (const auto& plane : planes) {
        iclforge::apps::fill_source_frame(plane, 0, start, scratch);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] += scratch[i];
        }
    }
}

}  // namespace forge_gui
