#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "truehd_objects.hpp"

// The channels of substream 3 placed on loudspeakers by libs/render, the way Hearth plays E-AC-3's
// objects (iclforge::render::LayoutRenderer): a bed channel goes to the speaker of its Table 12
// label (panned onto the layout where it has none), the LFE to the LFE feed, and a dynamic object
// is panned from its position at its gain. An update takes effect at its sample and the object's
// speaker gains move to it linearly over the update's ramp; the first update of an object takes
// effect at once. An object is silent until its first update. Width, divergence, zones, snapping
// and the screen factor are not rendered, as the layout renderer renders none of them for E-AC-3.
// There is no reconstruction delay to put back (a TrueHD object is lossless, not reconstructed from
// a downmix), so the LFE is not held back. PROVISIONAL (libs/truehd/ERRATA.md entry 61).

namespace forge_cli::commands {

class TrueHdObjectRenderer {
   public:
    static constexpr std::size_t kMaxSlots = iclforge::render::OutputLayout::kMaxSlots;

    TrueHdObjectRenderer(const iclforge::render::OutputLayout& layout, std::uint32_t sample_rate);

    // What each channel of substream 3 is. Called once, when the first programme has been read.
    void set_plan(const ObjectPlan& plan);

    // The update of the dynamic object at `channel` that takes effect at `sample` (counted from the
    // stream's first), ramping over `ramp` samples.
    void add_update(std::size_t channel, std::uint64_t sample, int ramp,
                    const iclforge::objects::oba::DynamicObject& state);

    // Renders the next `samples` samples of the `channels` channels in `pcm` (interleaved, 24 bit
    // values) into `out`, interleaved for the layout's slots, as 24 bit values.
    void render(std::span<const std::int32_t> pcm, std::size_t channels, std::size_t samples,
                std::vector<std::int32_t>& out);

    [[nodiscard]] std::size_t slots() const noexcept { return slots_; }
    [[nodiscard]] const std::string& slot_names() const noexcept { return names_; }
    [[nodiscard]] std::uint64_t clipped() const noexcept { return clipped_; }

   private:
    using Gains = std::array<double, kMaxSlots>;
    struct Event {
        std::uint64_t sample = 0;
        int ramp = 0;
        Gains target{};
    };
    struct Track {
        Gains gains{};
        Gains step{};
        Gains target{};
        int left = 0;
        bool primed = false;  // has had an update
        bool dynamic = false;
        bool placed = false;  // gains are fixed (a bed channel)
        std::deque<Event> events;
    };

    [[nodiscard]] Gains bed_gains(iclforge::objects::oba::BedLabel label);

    iclforge::render::OutputLayout layout_;
    iclforge::render::LayoutRenderer renderer_;
    std::size_t slots_ = 0;
    std::string names_;
    std::vector<Track> tracks_;
    std::uint64_t position_ = 0;
    std::uint64_t clipped_ = 0;
};

}  // namespace forge_cli::commands
