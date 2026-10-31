#pragma once

// What one board can do, as data (planning/p4x-preferences.md): the screen, the sink's slots, the networks, the
// clock, the object cap. A Hearth sink is one product base that many boards run with different supported
// configurations - an ESP32-S3 with a rack of DACs, an ESP32-C6 with eight slots, an ESP32-P4 with a 7-inch
// panel - and what separates them is this description, filled in ONCE at boot from the build and the hardware
// (firmware/hearth-sink/main/board_caps.cpp), never an #ifdef at the point of use and never "is this the P4X".
// The preferences (preferences.hpp), the screen that draws them, the REST listing (GET /preferences) and the
// web page all ask the same description, so a choice a board cannot make is greyed everywhere, with the same
// reason, and a new variant is a new set of facts and not a new branch of code.
//
// Free of ESP-IDF, like hardware_info.hpp and sink_plan.hpp beside it and for the same reason: the rules (what
// a slot width does to the ceiling, which chip cannot open a TDM frame) are worth getting wrong on a laptop and
// not on a board. libs/device/tests/test_board_caps.cpp builds every known variant (board_variants.hpp).
//
// Two kinds of fact live here, and the comments say which is which. HARDWARE is fixed for the life of the
// firmware: the display, the chip, the networks the build has, whether a clock exists. CONFIGURATION is what
// the board is set to (the slot width, the second I2S line) and changes with a PUT; the firmware's
// board_caps() builds the whole value afresh from the settings each time it is asked, so a reader holds a
// snapshot and never a reference.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include "iclforge/hardware_info.hpp"
#include "iclforge/sink_plan.hpp"

namespace iclforge {

// --- the screen -------------------------------------------------------------------------------------

// How the display is attached. Informational for a preference (nothing is offered or withheld because a panel
// is on DSI and not on SPI); it is here so that GET /preferences, the developer view and a bug report can say
// what they are looking at, and so that a build with a new kind of panel says so in one place.
enum class DisplayKind : std::uint8_t {
    kNone,         // no screen
    kFrameBuffer,  // a frame buffer in memory with nothing behind it (the bench build that measures the screen)
    kDsi,          // a MIPI DSI panel
    kSpi,          // a panel on an SPI bus
    kParallel,     // a panel on a parallel (8080 / RGB) bus
};

// "none", "null", "dsi", "spi" or "parallel": the words GET :8081/panel's kind and GET /preferences use.
[[nodiscard]] constexpr const char* display_kind_name(DisplayKind kind) {
    switch (kind) {
        case DisplayKind::kFrameBuffer:
            return "null";
        case DisplayKind::kDsi:
            return "dsi";
        case DisplayKind::kSpi:
            return "spi";
        case DisplayKind::kParallel:
            return "parallel";
        case DisplayKind::kNone:
            break;
    }
    return "none";
}

struct DisplayCaps {
    DisplayKind kind = DisplayKind::kNone;
    int width = 0;
    int height = 0;
    // A touch controller is reading (or, on a frame buffer, the injected taps of POST /panel/tap). A screen with
    // no touch is looked at and never touched: a view that needs a touch to light it cannot be offered.
    bool touch = false;
    // The backlight can be set to a level. Without it a dimmed screen is a black cover over a lit panel.
    bool backlight = false;
    // Side buttons a person can press to drive the screen without touching it (a JetKVM Mini has one or two). No screen
    // code reads this yet: there is no button-navigation model, and a board whose screen has no touch and no keys has
    // no way to change a preference from the glass at all, which is what its greyed rows say.
    int keys = 0;

    [[nodiscard]] constexpr bool present() const { return kind != DisplayKind::kNone && width > 0 && height > 0; }
    // Something to touch or press: a screen a person can drive.
    [[nodiscard]] constexpr bool operable() const { return present() && (touch || keys > 0); }
};

// --- the sink ---------------------------------------------------------------------------------------

struct SinkCaps {
    // HARDWARE. The bits one I2S line's TDM frame holds (I2S_LL_SLOT_FRAME_BIT_MAX: 128 on an ESP32-S3 and an
    // ESP32-C6, 512 on an ESP32-P4), which sink_plan.hpp turns into slots. 0 for a sink with no TDM frame (the
    // capture and null sinks), whose slot count is `fixed_slots`.
    int frame_bit_max = 0;
    int fixed_slots = 0;
    // The part has a second I2S controller a second line can run on (an ESP32-S3; not an ESP32-C6, and not the
    // ESP32-P4's wide sink).
    bool second_line_possible = false;
    // A TDM frame can be opened at all. False on an ESP32-P4 below chip revision v3.0, which has no PLL clock
    // source for I2S: only standard 1-2 channel I2S is reachable there (HardwareFacts::revision_hard_limit_below).
    bool tdm_usable = true;
    // CONFIGURATION. The slot width in force (16 or 32; 0 for a sink that has none) and whether a second line is
    // wired. A setting of the board, not of the chip: PUT /slot-width and PUT /wiring.
    int slot_bits = 0;
    bool second_line = false;

    // The most slots the sink carries at a given setting: sink_plan.hpp's ceiling, held to two where no TDM frame
    // opens. 0 for a width the sink does not have.
    [[nodiscard]] constexpr int slots_at(int bits, bool second) const {
        if (frame_bit_max <= 0) {
            return fixed_slots;
        }
        const int ceiling = static_cast<int>(sink_ceiling(bits, second && second_line_possible, frame_bit_max));
        return tdm_usable ? ceiling : std::min(ceiling, 2);
    }
    // ... at the setting in force: what a layout set now has to fit in.
    [[nodiscard]] constexpr int slots() const { return slots_at(slot_bits, second_line); }
    // ... at the best setting the hardware takes (either width, a second line where the part has one): what
    // HardwareFacts::sink_max_slots reports, and the most a layout could ever need to fit in on this board.
    [[nodiscard]] constexpr int slots_most() const {
        return std::max(slots_at(16, true), slots_at(32, true));
    }
};

// --- the networks -----------------------------------------------------------------------------------

struct NetworkCaps {
    bool wifi = false;
    bool ethernet = false;

    [[nodiscard]] constexpr bool any() const { return wifi || ethernet; }
    // A board with both has a choice between them (PUT /network-preference).
    [[nodiscard]] constexpr bool choice() const { return wifi && ethernet; }
};

// --- the whole board --------------------------------------------------------------------------------

struct BoardCaps {
    // What the chip is: its model and revision, cores, PSRAM, and the sink ceiling it reports on GET /hardware.
    // Reused, not restated: the same facts GET /hardware describes in words.
    HardwareFacts hardware;
    // The locked build profile this image is (firmware/hearth-sink/profiles.json), or empty.
    std::string profile;
    DisplayCaps display;
    SinkCaps sink;
    NetworkCaps network;
    // The board can keep the time of day: a network and a time server to ask (main/wall_clock.hpp).
    bool wall_clock = false;
    // The build has the Sendspin player, and so servers to pair with.
    bool sendspin = false;
    // The E-AC-3 objects the board places in real time (CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_OBJECTS): 0 none,
    // nothing when the build states no figure and places whatever it is sent.
    std::optional<int> max_objects;
    // The widest syncframe, in coded channels, the Sendspin player decodes in this build
    // (CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS: 2 on the ESP32-C6); 0 states no limit. A layout
    // wider than this plays through no more speakers than the stream has channels (layout_options.hpp).
    int coded_channels_max = 0;
    // The image can be replaced over the network: two app slots with a trial and a rollback
    // (planning/esp32-ota.md).
    bool firmware_slots = false;

    [[nodiscard]] bool has_screen() const { return display.present(); }
};

}  // namespace iclforge
