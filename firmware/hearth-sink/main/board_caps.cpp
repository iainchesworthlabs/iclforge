// The board's capability description. See board_caps.hpp.

#include "board_caps.hpp"

#include <string_view>

#include "sdkconfig.h"

#include "iclforge/control.hpp"

#include "audio_sink.hpp"
#include "network.hpp"
#include "panel/panel.hpp"
#include "sendspin.hpp"
#include "settings.hpp"
#include "wall_clock.hpp"

namespace player {
namespace {

// The E-AC-3 objects this build places in real time: -1 is "none stated" (it places whatever it is sent).
constexpr int kMaxObjects = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_OBJECTS;
// The widest syncframe it decodes, in coded channels: 2 on an ESP32-C6, 0 for no limit.
constexpr int kMaxCodedChannels = CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS;

// The cable and the station this build drives. A board with a choice has both; one without has the interface
// network_link() names, and none at all in a build with no network.
iclforge::NetworkCaps network_caps() {
    if (network_has_choice()) {
        return {.wifi = true, .ethernet = true};
    }
    const NetworkLink link = network_link();
    const std::string_view kind = link.kind != nullptr ? std::string_view(link.kind) : std::string_view();
    return {.wifi = kind == "wifi", .ethernet = kind == "ethernet"};
}

}  // namespace

iclforge::BoardCaps board_caps() {
    // The chip does not change while the board runs: gathered once.
    static const iclforge::HardwareFacts facts =
        iclforge::hardware_facts(sink_max_slots(), sink_max_slots_bit_width());

    iclforge::BoardCaps caps;
    caps.hardware = facts;
    caps.profile = ICLFORGE_PROFILE_ID;
    caps.display = panel_display_caps();

    caps.sink.frame_bit_max = sink_frame_bit_max();
    caps.sink.fixed_slots = caps.sink.frame_bit_max == 0 ? sink_slots() : 0;
    caps.sink.second_line_possible = sink_second_line_possible();
    // A chip below the revision at which it can clock a wide TDM frame opens standard 1-2 channel I2S only. The sink
    // reads the revision itself and reports two slots then (sink_slots, sink_max_slots), so this and /hardware agree.
    caps.sink.tdm_usable = sink_tdm_usable();
    caps.sink.slot_bits = sink_slot_bits();
    caps.sink.second_line = settings().second_line && caps.sink.second_line_possible;

    caps.network = network_caps();
    caps.wall_clock = wall_clock_built();
    caps.sendspin = sendspin_built();
    if (kMaxObjects >= 0) {
        caps.max_objects = kMaxObjects;
    }
    // The widest stream the Sendspin player decodes (0: no limit; stated only by a build that has the player).
    if (caps.sendspin) {
        caps.coded_channels_max = kMaxCodedChannels;
    }
    caps.firmware_slots = true;
    return caps;
}

}  // namespace player
