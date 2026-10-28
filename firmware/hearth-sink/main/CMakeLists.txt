# The sink is a DIRECTORY CMake picks, not a flag the player branches on - the
# same rule the library follows for its own platform choices. See
# main/audio_sink.hpp.
#
#   i2s      the I2S peripheral, standard mode or TDM depending on what the
#            active layout needs (iclforge/sink_plan.hpp), on up to two of a
#            part's I2S lines sharing one bit clock - the default, correct on
#            any part whose own I2S_LL_SLOT_FRAME_BIT_MAX is 128 (ESP32-S3,
#            ESP32-C6)
#   i2s_wide the same arithmetic, one line, for a part whose controller alone
#            reaches the product's full channel target at a wider TDM frame
#            (512 bits on the ESP32-P4) - see that directory's own top-of-file
#            comment for why this is a fork of sink/i2s rather than a second
#            line's worth of branching inside it
#   capture  the conversion code the i2s sink uses, checked, with no
#            peripheral behind it: what CI runs, because qemu-system-xtensa has
#            no I2S and the real sink's first write would never return
#   null     counts blocks and returns; CI builds it and does not run it
# From Kconfig, not a -D cache variable: this file is evaluated in IDF's
# separate requirements-expansion process, which does not inherit cache
# variables from the project above it. See main/Kconfig.projbuild.
if(CONFIG_ICLFORGE_EXAMPLE_SINK_NULL)
    set(ICLFORGE_SINK_DIR "sink/null")
elseif(CONFIG_ICLFORGE_EXAMPLE_SINK_CAPTURE)
    set(ICLFORGE_SINK_DIR "sink/capture")
elseif(CONFIG_ICLFORGE_EXAMPLE_SINK_I2S_WIDE)
    set(ICLFORGE_SINK_DIR "sink/i2s_wide")
else()
    set(ICLFORGE_SINK_DIR "sink/i2s")
endif()

# And where the bytes come from - the mirror of the same idea, see
# main/byte_source.hpp. CI runs the partition, fatfs and http sources under
# QEMU, http over the emulated Ethernet below; sd needs an SDMMC host, which
# QEMU does not have, so CI only builds it.
if(CONFIG_ICLFORGE_EXAMPLE_SOURCE_SD)
    set(ICLFORGE_SOURCE_DIR "source/sd")
elseif(CONFIG_ICLFORGE_EXAMPLE_SOURCE_FATFS)
    set(ICLFORGE_SOURCE_DIR "source/fatfs")
elseif(CONFIG_ICLFORGE_EXAMPLE_SOURCE_HTTP)
    set(ICLFORGE_SOURCE_DIR "source/http")
else()
    set(ICLFORGE_SOURCE_DIR "source/partition")
endif()

# And which network, which is now every build's question rather than the HTTP
# source's alone: WiFi on a board; the OpenCores Ethernet MAC under QEMU, which
# is what lets CI run this end to end with no board and no access point; and
# none at all for a build that plays from flash, FAT or an SD card. See
# main/network.hpp. A sink is FOUND before it is played to - mDNS answering and
# the control surface reachable while the board sits idle - so the network
# comes up at boot, and a build with no network says so once and costs nothing.
#
# A board with an Ethernet PHY as well as the radio (CONFIG_ICLFORGE_EXAMPLE_NETWORK_ETHERNET) has
# net/dual, which is the WiFi station (net/wifi/station.cpp, the code a board with the station
# alone has too) and the cable (net/wired/ethernet.cpp) with the choice between them. The IP stack
# they share is net/stack.cpp.
#
# The time of day (main/wall_clock.hpp) follows the network: every build with a radio or a cable keeps
# its time with a time server (clock/sntp), which is all the hardware; QEMU's network has no time server
# to ask and CI must not depend on one, and a build with no network has nothing to ask over (clock/none).
if(CONFIG_ICLFORGE_EXAMPLE_SOURCE_HTTP AND CONFIG_ICLFORGE_EXAMPLE_NETWORK_OPENETH)
    set(ICLFORGE_NETWORK_SRCS "net/openeth/network.cpp")
    set(ICLFORGE_CLOCK_SRCS "clock/none/wall_clock.cpp")
elseif(CONFIG_ICLFORGE_EXAMPLE_SOURCE_HTTP AND CONFIG_ICLFORGE_EXAMPLE_NETWORK_ETHERNET)
    set(ICLFORGE_NETWORK_SRCS "net/dual/network.cpp" "net/wifi/station.cpp" "net/wired/ethernet.cpp"
                              "net/stack.cpp")
    set(ICLFORGE_CLOCK_SRCS "clock/sntp/wall_clock.cpp")
elseif(CONFIG_ICLFORGE_EXAMPLE_SOURCE_HTTP)
    set(ICLFORGE_NETWORK_SRCS "net/wifi/network.cpp" "net/wifi/station.cpp" "net/stack.cpp")
    set(ICLFORGE_CLOCK_SRCS "clock/sntp/wall_clock.cpp")
else()
    set(ICLFORGE_NETWORK_SRCS "net/none/network.cpp")
    set(ICLFORGE_CLOCK_SRCS "clock/none/wall_clock.cpp")
endif()

# And the Sendspin player, which needs the component's (CONFIG_ICLFORGE_SENDSPIN)
# and a network to be found on. See main/sendspin.hpp.
if(CONFIG_ICLFORGE_SENDSPIN AND CONFIG_ICLFORGE_EXAMPLE_SOURCE_HTTP)
    set(ICLFORGE_SENDSPIN_SRCS "sendspin/player/sendspin.cpp" "sendspin/player/psram_scope.cpp")
else()
    set(ICLFORGE_SENDSPIN_SRCS "sendspin/none/sendspin.cpp")
endif()

# esp_driver_i2s is required whichever sink is chosen. The capture and null
# builds do not call it, but a component's REQUIRES is resolved before the
# source list is, and listing it conditionally would make the builds differ in
# a second place for no gain.
# EVERY component every sink and source might need, unconditionally, whichever
# pair this build selected.
#
# Not laziness - a measured constraint. IDF expands REQUIRES in a separate,
# earlier pass than the one that evaluates the source list, and CONFIG_ values
# are not reliably set in it: a conditional REQUIRES silently resolved to the
# default branch while the SOURCES resolved correctly, so an SD build compiled
# source/sd/byte_source.cpp against a component list that did not include
# esp_driver_sdmmc and failed on a missing header. Listing them all makes the
# builds differ in exactly one place, which is the directory each seam picks.
#
# sdmmc as well as esp_driver_sdmmc: sdmmc_cmd.h lives in the former and
# sdmmc_host.h in the latter. fatfs carries esp_vfs_fat.h. esp_eth for the
# QEMU network under the HTTP source, listed for every build for the reason
# above.
# The stage-timer backend the bare-metal probe links (firmware/baremetal/
# stage_timers.cpp), when the repository is here to provide it. With the
# library built with ICLFORGE_STAGE_TIMERS it gives each play a
# play.stage[<zone>] line per decoder stage - `idf.py -DICLFORGE_STAGE_TIMERS=ON
# build` - and without that option nothing calls it and the linker drops it, so
# a plain build is unchanged. A component archive carries no apps/, and there
# hearth_sink.cpp's own no-op stand-ins are what links.
set(ICLFORGE_STAGE_TIMER_SRCS "")
set(ICLFORGE_STAGE_TIMER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../baremetal")
if(EXISTS "${ICLFORGE_STAGE_TIMER_DIR}/stage_timers.cpp")
    set(ICLFORGE_STAGE_TIMER_SRCS "${ICLFORGE_STAGE_TIMER_DIR}/stage_timers.cpp")
endif()

# And the panel, the screen the lyrics are drawn on (main/panel/panel.hpp; a directory CMake picks, as the
# sink and the source are). panel/lvgl is LVGL drawing the lyric screen, onto a MIPI DSI panel with touch
# or into a frame buffer with no panel behind it; panel/none is a build with no screen. LVGL and the
# panel's drivers are the manifest's, for the ESP32-P4 only (idf_component.yml), and an image that
# picks panel/none links none of them.
if(CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI OR CONFIG_ICLFORGE_EXAMPLE_PANEL_NULL)
    set(ICLFORGE_PANEL_SRCS "panel/lvgl/panel.cpp" "panel/lvgl/lyric_screen.cpp"
                            "panel/lvgl/dev_screen.cpp" "panel/lvgl/dev_info.cpp"
                            "panel/lvgl/fonts.cpp" "panel/lvgl/glyphs.cpp" "panel/lvgl/now_screen.cpp" "panel/lvgl/meters_screen.cpp"
                            "panel/lvgl/prefs_screen.cpp" "panel/lvgl/prefs_pages.cpp" "panel/lvgl/prefs_editors.cpp" "panel/lvgl/sound_sheet.cpp" "panel/lvgl/layout_sheet.cpp"
                            "panel/lvgl/lv_memory.cpp")
    if(CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI)
        list(APPEND ICLFORGE_PANEL_SRCS "panel/lvgl/display_dsi.cpp")
    else()
        list(APPEND ICLFORGE_PANEL_SRCS "panel/lvgl/display_null.cpp")
    endif()
else()
    set(ICLFORGE_PANEL_SRCS "panel/none/panel.cpp")
endif()

# The Meters view's analysis (a 1,024- and a 512-point transform in float) and its drawing are the panel's heaviest loops, and the project's -Os does not
# inline the transform's passes or unroll its loops: the kit measured 5 to 10 ms an update for about 100 kflops (planning/p4x-meters-view.md, "Kit
# results"). meters_screen.cpp at -O2, as the component's player.cpp and burst_player.cpp are; and panel.cpp, where the player's tap
# (iclforge/meters_tap.hpp, header-only, so its block() is compiled where the tap is made) runs inside the decode task's burst: the kit measured 370 to
# 860 us a burst for it.
if(CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI OR CONFIG_ICLFORGE_EXAMPLE_PANEL_NULL)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/panel/lvgl/meters_screen.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/panel/lvgl/panel.cpp"
                                PROPERTIES COMPILE_OPTIONS "-O2")
endif()

idf_component_register(
    SRCS "hearth_sink.cpp"
         "board_caps.cpp"
         "preferences.cpp"
         "settings.cpp"
         "sound.cpp"
         "provision.cpp"
         "system_reset.cpp"
         "discovery.cpp"
         "${ICLFORGE_SINK_DIR}/audio_sink.cpp"
         "${ICLFORGE_SOURCE_DIR}/byte_source.cpp"
         ${ICLFORGE_NETWORK_SRCS}
         ${ICLFORGE_CLOCK_SRCS}
         ${ICLFORGE_SENDSPIN_SRCS}
         ${ICLFORGE_PANEL_SRCS}
         ${ICLFORGE_STAGE_TIMER_SRCS}
    INCLUDE_DIRS "." "source/http"
    REQUIRES iclforge esp_driver_i2s esp_timer freertos
             esp_system esp_hw_support esp_hal_wdt
             esp_partition esp_psram app_update espcoredump heap
             esp_driver_sdmmc sdmmc fatfs wear_levelling
             esp_wifi esp_eth esp_http_client esp_netif esp_event lwip nvs_flash
             esp_app_format espressif__mdns
             esp_lcd esp_driver_ledc esp_driver_i2c esp_driver_gpio esp_http_server)

# The trust anchors a pull verifies a signed update index against (planning/esp32-ota.md, O10): a PEM file of
# public certificates, embedded as it is. ICLFORGE_TRUST_ANCHORS_FILE (a cache variable, for CI's QEMU test,
# whose throwaway CA is made before the build) wins over the project's ICLFORGE_EXAMPLE_TRUST_ANCHORS; with
# neither the image carries an empty file, and says it cannot verify a pull. Copied to a name of its own so
# that the symbols the linker makes (_binary_trust_anchors_pem_start and _end) do not depend on the file's.
# Only a build that pulls carries it.
set(ICLFORGE_TRUST_ANCHORS_FILE "" CACHE FILEPATH "CI only: the trust anchors to embed")
if(CONFIG_ICLFORGE_FIRMWARE_PULL)
    set(iclforge_anchors_copy "${CMAKE_CURRENT_BINARY_DIR}/trust_anchors.pem")
    if(ICLFORGE_TRUST_ANCHORS_FILE)
        configure_file("${ICLFORGE_TRUST_ANCHORS_FILE}" "${iclforge_anchors_copy}" COPYONLY)
    elseif(CONFIG_ICLFORGE_EXAMPLE_TRUST_ANCHORS)
        configure_file("${PROJECT_DIR}/${CONFIG_ICLFORGE_EXAMPLE_TRUST_ANCHORS}" "${iclforge_anchors_copy}" COPYONLY)
    else()
        file(WRITE "${iclforge_anchors_copy}" "")
    endif()
    target_add_binary_data(${COMPONENT_LIB} "${iclforge_anchors_copy}" TEXT)
    # hearth_sink.cpp names the symbols weakly, so that a build without them links, and a weak reference
    # does not take an object out of an archive: the data would be left in libmain.a and the board would
    # say it has no trust anchor. Naming the start undefined takes the member, and the end with it.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=_binary_trust_anchors_pem_start")
endif()

# The panel's fonts, in the image (planning/karaoke-p4x-panel.md, D3): Archivo, which the apps bundle, and
# Noto Sans for Greek and Cyrillic and for Hebrew, under the SIL Open Font License (panel/fonts/*OFL.txt),
# rasterised by LVGL's tiny_ttf from flash. In the
# application rather than a partition of their own, so that an update over the network carries them.
if(CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI OR CONFIG_ICLFORGE_EXAMPLE_PANEL_NULL)
    foreach(_panel_font IN ITEMS Archivo-Regular Archivo-SemiBold NotoSansGreekCyrillic-Regular
                                  NotoSansGreekCyrillic-SemiBold NotoSansHebrew-Regular NotoSansHebrew-SemiBold)
        target_add_binary_data(${COMPONENT_LIB}
            "${CMAKE_CURRENT_SOURCE_DIR}/panel/fonts/${_panel_font}.ttf" BINARY)
    endforeach()
endif()

# The backend's object has to be in the link. Nothing that comes before it in
# the link asks for it: this component's own reset_stages and report_stages
# (hearth_sink.cpp) are weak, so that a component archive without apps/ links,
# and a weak definition in an earlier object is all the linker needs to leave the
# strong one in libmain.a where it is. The two zone functions it defines are
# wanted by the library (iclforge, and the AC-4 archives), which comes later in
# the link than this archive and cannot pull a member out of one already passed;
# without this the link ends in undefined references to
# iclforge::internal::profiling::zone_enter and zone_leave. Naming the first makes it
# undefined from the start of the link, so the member is taken from libmain.a
# when it is reached, and the rest of that object with it. Only for a build that
# has markers to serve: a plain build carries none of it.
if(ICLFORGE_STAGE_TIMERS AND ICLFORGE_STAGE_TIMER_SRCS)
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--undefined=_ZN8iclforge8internal9profiling10zone_enterEPKc")
endif()

# How long the audio actually is. The partition is 256 KB and the sample is
# 10,752 bytes, so without this the player reads a quarter of a megabyte of
# erased flash every lap and the accumulator dutifully skips all of it looking
# for a sync word - which "works", and is 251,392 bytes of resynchronisation
# reported on every pass.
#
# A real source knows its length: Content-Length, a file size, a directory
# entry. A raw partition does not, so the build supplies it - which is the
# honest version of the same fact rather than a heuristic like "stop at the
# first run of 0xFF".
file(SIZE "${CMAKE_CURRENT_SOURCE_DIR}/../stream/sample.ac3" ICLFORGE_STREAM_BYTES)
target_compile_definitions(${COMPONENT_LIB}
    PRIVATE "ICLFORGE_STREAM_BYTES=${ICLFORGE_STREAM_BYTES}")
# Defined for every build even though only the partition source reads it: an SD
# file and an HTTP body report their own length, and a definition that appears
# and disappears with the source choice is one more way the builds can differ.

# The chosen sink's own source, under the same switch the component's
# CMakeLists.txt puts player.cpp behind: ICLFORGE_MINIMAL_HOT_O2, a cache
# variable set there and so visible here too. Measured on an ESP32-C6 playing
# a 7.1 stream onto eight 16-bit TDM slots: sink_us_per_frame 12,689 to
# 11,551, because -Os calls the bits conversion once a sample rather than
# inlining it (iclforge/interleave.hpp). i2s and capture both convert through
# it; null has nothing to gain and stays untouched.
if(ICLFORGE_MINIMAL_HOT_O2)
    set_source_files_properties("${ICLFORGE_SINK_DIR}/audio_sink.cpp" PROPERTIES
        COMPILE_OPTIONS "-O2")
endif()

# CI's rollback tests under QEMU need images that fail their trial
# (planning/esp32-ota.md, "Tests"): ICLFORGE_FIRMWARE_TEST=unhealthy never
# reports healthy and goes back at the deadline, and =panic panics as its
# trial starts. A cache variable rather than Kconfig, so that each variant
# recompiles this one file rather than the whole image; defined for every
# build, 0 or 1, for the reason ICLFORGE_STREAM_BYTES is. Never set for a
# board.
set(ICLFORGE_FIRMWARE_TEST "" CACHE STRING "CI only: unhealthy, panic, or empty")
set(iclforge_firmware_test_unhealthy 0)
set(iclforge_firmware_test_panic 0)
if(ICLFORGE_FIRMWARE_TEST STREQUAL "unhealthy")
    set(iclforge_firmware_test_unhealthy 1)
elseif(ICLFORGE_FIRMWARE_TEST STREQUAL "panic")
    set(iclforge_firmware_test_panic 1)
elseif(NOT ICLFORGE_FIRMWARE_TEST STREQUAL "")
    message(FATAL_ERROR "ICLFORGE_FIRMWARE_TEST is unhealthy, panic or empty, not \"${ICLFORGE_FIRMWARE_TEST}\"")
endif()
set_property(SOURCE "hearth_sink.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
    "ICLFORGE_FIRMWARE_TEST_UNHEALTHY=${iclforge_firmware_test_unhealthy}"
    "ICLFORGE_FIRMWARE_TEST_PANIC_ON_TRIAL=${iclforge_firmware_test_panic}")

# Which locked profile this image was built as (firmware/hearth-sink/profiles.json,
# planning/esp32-platform-profiles.md): its id and a hash of what it locks, from
# `tools/hearth/sink_profile.py identity <profile> --cmake`. GET /hardware reports both, so a
# board says what it runs and the board suite can refuse one that runs another profile or a
# scratch build. Cache variables for the reason ICLFORGE_FIRMWARE_TEST is one: a change
# recompiles this one file. Empty for a build that was not made as a profile.
set(ICLFORGE_PROFILE "" CACHE STRING "the locked profile this image is, or empty")
set(ICLFORGE_PROFILE_HASH "" CACHE STRING "a hash of that profile's locked values")
set_property(SOURCE "hearth_sink.cpp" "board_caps.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
    "ICLFORGE_PROFILE_ID=\"${ICLFORGE_PROFILE}\""
    "ICLFORGE_PROFILE_HASH_TEXT=\"${ICLFORGE_PROFILE_HASH}\"")