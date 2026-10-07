#pragma once

#include "iclforge/objects/oamd.hpp"

#include "iclforge/objects/aliases.hpp"

namespace iclforge::objects::oba {

// One object's placement for one frame. Positions are room-anchored per
// §4.2.1; the gain is linear and applies to the bed as well as being sent as
// object_gain, because the object and its contribution to the downmix have to
// agree or the reconstruction is scaled wrong.
struct ObjectPlacement {
    Position position{};
    double gain = 1.0;
    // Objects never reach the LFE by panning (there is no direction that
    // points at it), so this is the only route - and it is deliberately not
    // part of the JOC matrix, because §6.3.2.2 bypasses the LFE entirely.
    double lfe_send = 0.0;

    // --- extent and rendering constraints (TS 103 420 §5.6.1) --------------
    // These reach the OAMD payload and stop there. Nothing in this encoder's
    // own bed render or JOC solve reads them: §4.3 makes the renderer, not
    // the decoder, responsible for turning an extent into loudspeaker feeds,
    // and the 5.1 downmix here is a point-source VBAP pan by construction. So
    // a sized object is transmitted as sized and rendered by this encoder as
    // a point - which is the honest split, since a downmix that spread the
    // object would then be spread AGAIN by the receiving renderer.
    //
    // §5.6.1.2. A point source at 0/0/0, which is the default Table 29 gives.
    ObjectSize size{};
    // §5.6.1.5.1 b_object_snap - what ADM calls channelLock: render the
    // object to its nearest speaker rather than panning between speakers.
    bool snap = false;
    // §5.6.1.6.1 Table 20: which horizontal zones the renderer may use.
    ZoneConstraint zone = ZoneConstraint::kNone;
    // §5.6.1.6.2 Table 21: false excludes the Top-Bottom zone, holding the
    // object on the listener plane however high its z says it is.
    bool enable_elevation = true;
    // §5.5.14 / Tables 40-42: how much of the object's energy is spread into two objects along X
    // (§5.2.7), 0 for none to 1. Sent in the extended_object_element, which is only written when
    // some object has a non-zero value; quantized to Table 42's 62 values.
    double divergence = 0.0;
    // §5.6.1.1.18-.20: the position is screen-anchored rather than room-anchored, and how far
    // (screen_factor, 1/8 to 1) and with what depth (depth_factor, 1/4 to 2) the renderer follows
    // the screen. Like the other rendering constraints these reach the OAMD payload and stop there.
    bool screen_reference = false;
    double screen_factor = 1.0;
    double depth_factor = 1.0;
};

}  // namespace iclforge::objects::oba
