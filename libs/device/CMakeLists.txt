# ---------------------------------------------------------------------------
# iclforge::device - the parts of an ESP32 sink or player that need no SDK, so that the host builds
# and tests them: the planar-to-interleaved conversion with its slot conversion, the block ring and
# the unit hold, the DAC queue model, the sink's mode and slot-count planner, the playout model, the
# Wi-Fi provisioning protocol (Improv), the Sendspin player's pairing records, the console's log
# ring, the TCP arrival log, the firmware image header, status and trial rules, and the hardware
# report. Header-only and standard library only: none of them includes anything from ESP-IDF, which
# is what lets libs/device/tests run them on every host, and is worth keeping true.
#
# The headers keep the names the component's users include (iclforge/interleave.hpp and the rest).
# The ESP-IDF component (firmware/esp-idf/iclforge) puts this directory on its include path, so a
# project that builds on it sees them as it always did; Hearth's engine links iclforge::device for the
# two it shares with the board (firmware_image.hpp, firmware_status.hpp), so that the board and the
# server agree by construction.
#
# Two directories hold slot_conversion.hpp, which interleave.hpp includes: conversion/float/ for a
# part with a floating-point unit and conversion/bits/ for one without (the same integers, computed
# with integer operations). A host has an FPU, so iclforge::device carries the float one; the
# component chooses by CONFIG_SOC_CPU_HAS_FPU, and the tests name both conversions where it matters.
# ---------------------------------------------------------------------------
# The minimum-footprint profile (the ESP-IDF component) puts the include paths on its own target,
# so no library is declared here for it.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    return()
endif()

add_library(iclforge_device INTERFACE)
add_library(iclforge::device ALIAS iclforge_device)
target_include_directories(iclforge_device INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/conversion/float>")
target_compile_features(iclforge_device INTERFACE cxx_std_23)
