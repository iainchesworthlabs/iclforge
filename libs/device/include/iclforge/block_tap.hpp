#pragma once

// Something that wants to see each block of rendered output on its way to the meters (the Meters view's mix and its 50 ms peaks,
// iclforge/meters_tap.hpp; planning/p4x-meters-view.md). The player owns no copy of it and knows nothing of what it does: with none set, or
// one that does not want blocks, the player's path is one pointer load and nothing else, so an image with no screen pays for an interface and not a
// feature.

#include <cstddef>
#include <cstdint>
#include <span>

namespace iclforge {

class BlockTap {
   public:
    virtual ~BlockTap() = default;

    // Asked once a block, before block(): false leaves the player's path as it was.
    [[nodiscard]] virtual bool wants_blocks() const noexcept = 0;

    // The block that is about to be metered: one span of `frames` floats for each open output, the ones not in `live` silent (and not
    // to be read). Called on the player's decode task, after the speakers' trims and the volume are applied, so it is what is heard;
    // it must not block, allocate or take a lock.
    virtual void block(std::span<const std::span<float>> outputs, std::size_t frames, std::uint32_t live) noexcept = 0;
};

}  // namespace iclforge
