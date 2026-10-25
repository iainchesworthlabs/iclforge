#pragma once

// Every board this product knows about, and the ones it is built to be able to meet, as BoardCaps
// (board_caps.hpp). The host tests check each preference (preferences.hpp) against every one of these, the
// preview tool draws the Preferences screen as each of them, and a session adding a preference or a variant adds
// to this list and sees at once what that does to the others.
//
// The first five are real builds (firmware/hearth-sink/profiles.json) as the firmware fills them in at boot; the
// rest are made up - an ESP32-S3 and an ESP32-C3 with a small panel, and a P4 with a smaller one - because the point
// of describing a board as data is that a board nobody has built yet already has an answer.
// Free of ESP-IDF, like the rest of libs/device.

#include <cstddef>
#include <vector>

#include "iclforge/board_caps.hpp"

namespace iclforge {

struct BoardVariant {
    const char* name;
    BoardCaps caps;
};

namespace variants {

// HardwareFacts, which GET /hardware gathers field by field: a chip's identity and what its sink reaches. (Assigned
// and not brace-initialised: its strings have no defaults to leave out.)
[[nodiscard]] inline HardwareFacts chip(const char* target, const char* name, int major, int minor, int cores,
                                        bool fpu, int mhz, std::size_t psram_bytes, int max_slots) {
    HardwareFacts facts;
    facts.target = target;
    facts.chip = name;
    facts.revision_major = major;
    facts.revision_minor = minor;
    facts.cores = cores;
    facts.fpu = fpu;
    facts.cpu_freq_mhz = mhz;
    facts.psram_bytes = psram_bytes;
    facts.sink_max_slots = max_slots;
    facts.sink_max_slots_bits = 16;
    return facts;
}

// An ESP32-S3-DevKitC-1 N16R8: 8 MB of PSRAM, one DAC bus and room for a second, Wi-Fi, no screen.
[[nodiscard]] inline BoardCaps s3_n16r8() {
    BoardCaps caps;
    caps.hardware = chip("esp32s3", "ESP32-S3", 0, 2, 2, true, 240, 8U * 1024 * 1024, 16);
    caps.profile = "s3-n16r8";
    caps.sink = {.frame_bit_max = 128, .second_line_possible = true, .slot_bits = 32};
    caps.network = {.wifi = true};
    caps.wall_clock = true;
    caps.sendspin = true;
    caps.max_objects = 0;
    caps.firmware_slots = true;
    return caps;
}

// An ESP32-C6 with 16 MB of flash: one core, no PSRAM, no FPU, one I2S controller, Wi-Fi, no screen.
[[nodiscard]] inline BoardCaps c6_16mb() {
    BoardCaps caps;
    caps.hardware = chip("esp32c6", "ESP32-C6", 0, 1, 1, false, 160, 0, 8);
    caps.profile = "c6-16mb";
    // 16-bit slots (sdkconfig.sendspin-c6), and a build that decodes two coded channels at most.
    caps.sink = {.frame_bit_max = 128, .second_line_possible = false, .slot_bits = 16};
    caps.network = {.wifi = true};
    caps.wall_clock = true;
    caps.sendspin = true;
    caps.max_objects = 0;
    caps.coded_channels_max = 2;
    caps.firmware_slots = true;
    return caps;
}

// An ESP32-P4 of silicon below v3.0 (the FireBeetle 2): a wide TDM frame the chip cannot clock, Wi-Fi through
// the C6 beside it, no cable and no screen.
[[nodiscard]] inline BoardCaps p4_rev1() {
    BoardCaps caps;
    // The sink reports what the chip reaches (SinkCaps::slots_most): two, not the frame's thirty-two.
    caps.hardware = chip("esp32p4", "ESP32-P4", 1, 3, 2, true, 360, 32U * 1024 * 1024, 2);
    caps.hardware.revision_hard_limit_below = 300;
    caps.hardware.revision_hard_limit_cost = "TDM output cannot open above 2 channels on this silicon.";
    caps.profile = "p4-rev1";
    caps.sink = {.frame_bit_max = 512, .second_line_possible = false, .tdm_usable = false, .slot_bits = 32};
    caps.network = {.wifi = true};
    caps.wall_clock = true;
    caps.sendspin = true;
    caps.max_objects = 12;
    caps.firmware_slots = true;
    return caps;
}

// The P4X kit: an ESP32-P4 of silicon v3.2 on Espressif's ESP32-P4X-Function-EV-Board, its 7-inch 1024 by 600
// MIPI DSI panel with a GT911 touch controller, a cable and Wi-Fi through the C6.
[[nodiscard]] inline BoardCaps p4_rev3_kit() {
    BoardCaps caps;
    caps.hardware = chip("esp32p4", "ESP32-P4", 3, 2, 2, true, 400, 32U * 1024 * 1024, 32);
    caps.profile = "p4-rev3";
    caps.display = {.kind = DisplayKind::kDsi, .width = 1024, .height = 600, .touch = true, .backlight = true};
    caps.sink = {.frame_bit_max = 512, .second_line_possible = false, .tdm_usable = true, .slot_bits = 32};
    caps.network = {.wifi = true, .ethernet = true};
    caps.wall_clock = true;
    caps.sendspin = true;
    caps.max_objects = 12;
    caps.firmware_slots = true;
    return caps;
}

// The same kit built with the frame-buffer panel (CONFIG_ICLFORGE_EXAMPLE_PANEL_NULL): what a board with no screen
// runs to see what the screen would cost. Touch is the injected taps of POST /panel/tap; no backlight to set.
[[nodiscard]] inline BoardCaps p4_rev3_frame_buffer() {
    BoardCaps caps = p4_rev3_kit();
    caps.profile = "p4-rev3-frame-buffer";
    caps.display = {.kind = DisplayKind::kFrameBuffer, .width = 1024, .height = 600, .touch = true, .backlight = false};
    return caps;
}

// An ESP32-S3 with a 320 by 240 SPI panel and a touch controller: the board the owner named ("if it were a C3 or
// an S3 with a screen, Preferences should still work"). No such board is built; the display backend for one is not
// written.
[[nodiscard]] inline BoardCaps s3_spi_320x240() {
    BoardCaps caps = s3_n16r8();
    caps.profile = "s3-spi-320x240";
    caps.display = {.kind = DisplayKind::kSpi, .width = 320, .height = 240, .touch = true, .backlight = true};
    return caps;
}

// An ESP32-C3 with a 320 by 240 panel and NO touch controller and no backlight control: a screen that is looked at
// and never touched. One core, no PSRAM, one I2S controller (eight 16-bit slots), Wi-Fi.
[[nodiscard]] inline BoardCaps c3_spi_320x240_no_touch() {
    BoardCaps caps;
    caps.hardware = chip("esp32c3", "ESP32-C3", 0, 4, 1, false, 160, 0, 8);
    caps.profile = "c3-spi-320x240";
    caps.display = {.kind = DisplayKind::kSpi, .width = 320, .height = 240, .touch = false, .backlight = false};
    caps.sink = {.frame_bit_max = 128, .second_line_possible = false, .slot_bits = 32};
    caps.network = {.wifi = true};
    caps.wall_clock = true;
    caps.sendspin = true;
    caps.max_objects = 0;
    caps.firmware_slots = true;
    return caps;
}

// A 480 by 320 panel on an ESP32-S3 (the size of a common 3.5-inch module), and an 800 by 480 one on a P4: the
// screen sizes the preview tool draws between 320 by 240 and the kit's.
[[nodiscard]] inline BoardCaps s3_parallel_480x320() {
    BoardCaps caps = s3_n16r8();
    caps.profile = "s3-parallel-480x320";
    caps.display = {.kind = DisplayKind::kParallel, .width = 480, .height = 320, .touch = true, .backlight = true};
    return caps;
}

[[nodiscard]] inline BoardCaps p4_dsi_800x480() {
    BoardCaps caps = p4_rev3_kit();
    caps.profile = "p4-dsi-800x480";
    caps.display = {.kind = DisplayKind::kDsi, .width = 800, .height = 480, .touch = true, .backlight = true};
    return caps;
}

// A JetKVM Mini with an ESP32-P4X (planning/jetkvm-mini.md): a square 240 by 240 IPS panel with NO touch and one or two
// side buttons, no audio output at all, and one network interface - the Ethernet port of the Mini, or the Wi-Fi of the
// Mini W. The first board whose screen is square, whose rows cannot be tapped, and whose sink has no slots.
[[nodiscard]] inline BoardCaps p4x_jetkvm_mini_240() {
    BoardCaps caps;
    caps.hardware = chip("esp32p4", "ESP32-P4", 3, 2, 2, true, 400, 32U * 1024 * 1024, 0);
    caps.hardware.sink_max_slots_bits = 0;
    caps.profile = "p4x-jetkvm-mini-240";
    caps.display = {.kind = DisplayKind::kSpi, .width = 240, .height = 240, .touch = false, .backlight = true, .keys = 2};
    caps.sink = {};
    caps.network = {.ethernet = true};
    caps.wall_clock = true;
    caps.sendspin = true;
    caps.max_objects = 12;
    caps.firmware_slots = true;
    return caps;
}

[[nodiscard]] inline BoardCaps p4x_jetkvm_mini_w_240() {
    BoardCaps caps = p4x_jetkvm_mini_240();
    caps.profile = "p4x-jetkvm-mini-w-240";
    caps.network = {.wifi = true};
    return caps;
}

}  // namespace variants

// All of them, in a fixed order. A variant added here is checked by every test that walks the list.
[[nodiscard]] inline std::vector<BoardVariant> known_variants() {
    return {
        {"s3-n16r8", variants::s3_n16r8()},
        {"c6-16mb", variants::c6_16mb()},
        {"p4-rev1", variants::p4_rev1()},
        {"p4-rev3-kit", variants::p4_rev3_kit()},
        {"p4-rev3-frame-buffer", variants::p4_rev3_frame_buffer()},
        {"s3-spi-320x240", variants::s3_spi_320x240()},
        {"c3-spi-320x240-no-touch", variants::c3_spi_320x240_no_touch()},
        {"s3-parallel-480x320", variants::s3_parallel_480x320()},
        {"p4-dsi-800x480", variants::p4_dsi_800x480()},
        {"p4x-jetkvm-mini-240", variants::p4x_jetkvm_mini_240()},
        {"p4x-jetkvm-mini-w-240", variants::p4x_jetkvm_mini_w_240()},
    };
}

}  // namespace iclforge
