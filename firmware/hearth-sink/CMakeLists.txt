# ---------------------------------------------------------------------------
# iclforge's Hearth sink example: decode AC-3 or E-AC-3 from a flash
# partition (the default), a FAT volume in flash, an SD card or an HTTP body.
#
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32s3
#   idf.py -p <PORT> flash monitor
#
# `idf.py flash` writes stream/sample.ac3 into the `audio` partition as well as
# the application - see the flash-to-partition call below - so there is nothing
# to copy by hand.
#
# The sibling of i2s_player, differing in one thing: where the audio comes from.
# That one decodes a bitstream linked into its own image; this one reads the
# stream a piece at a time, through a ring between the player's fetch and
# decode tasks (32 KB by default, CONFIG_ICLFORGE_EXAMPLE_RING_BYTES) and a
# 16 KB framing buffer. See main/hearth_sink.cpp and
# firmware/esp-idf/iclforge/include/iclforge/player.hpp.
#
# WITHOUT A BOARD:
#
#   SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci" idf.py build
#   idf.py qemu
#
# which is the first of the four shapes CI runs under QEMU. sdkconfig.ci selects
# the capture sink, which converts with the code the sinks share
# (iclforge/interleave.hpp) and checks the result with no peripheral behind it;
# see main/audio_sink.hpp. The other three are sdkconfig.ci-tdm,
# sdkconfig.ci-render and sdkconfig.ci-http.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

# The iclforge component is this project's dependency, named in main/idf_component.yml with a
# path into this checkout (firmware/esp-idf/iclforge): the sink is Hearth's firmware and the
# component is the library's, so the one reaches the other as any project would.
set(ICLFORGE_ESP_PROFILE "decoder" CACHE STRING "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(iclforge_hearth_sink LANGUAGES C CXX)

# lwIP's IPv4 input hook from the iclforge component, so the Sendspin player
# dates a clock reply by when it arrived rather than by when its task read it
# (iclforge/tcp_arrivals.hpp).
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
idf_component_get_property(iclforge_dir iclforge COMPONENT_DIR)
target_compile_options(${lwip_lib} PRIVATE "-I${iclforge_dir}/lwip_hooks")
target_compile_definitions(${lwip_lib} PRIVATE "ESP_IDF_LWIP_HOOK_FILENAME=\"iclforge_lwip_hooks.h\"")

# LVGL keeps the rectangles that are invalid in a refresh in a table of 32 and redraws the whole screen when a refresh has more. The Meters view's spectrum
# changes sixty in a step: 11.5 million pixels a second with 32 on the 1024 x 600 stage, 1.2 to 2.1 with 256 (planning/p4x-meters-view.md). Only a panel
# build has LVGL; the table is 16 bytes a rectangle (4 KB).
if(CONFIG_ICLFORGE_EXAMPLE_PANEL_DSI OR CONFIG_ICLFORGE_EXAMPLE_PANEL_NULL)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PRIVATE LV_INV_BUF_SIZE=256)
endif()

# The audio, into the partition named in partitions.csv. Done here rather than
# through a separate esptool invocation so `idf.py flash` puts everything the
# application needs on the part in one step - a demo whose second half has to be
# flashed by hand is a demo that gets reported as broken.
#
# Only on a table that has the partition: the 4 MB C6's (partitions_c6.csv) has neither this nor
# `storage` below, its player takes its audio from the network, and esptool_py_flash_to_partition()
# stops the configure for a label the table does not have.
partition_table_get_partition_info(audio_offset "--partition-name audio" "offset")
if(audio_offset)
    esptool_py_flash_to_partition(flash "audio" "${CMAKE_CURRENT_SOURCE_DIR}/stream/sample.ac3")
endif()

# And the same bytes again as a FAT filesystem, for source/fatfs/.
#
# Not duplication for its own sake: source/sd/ cannot run under QEMU, which has
# no SD host, but FATFS does not care what block device it sits on. Mounting the
# same filesystem from flash runs the identical VFS, FATFS and stdio path that
# the SD source uses (main/source/file_common.hpp), so the file layer gets
# exercised on target and only the SDMMC host stays untested.
#
# read_only, because nothing writes to it and a read-only image needs no wear
# levelling - which keeps the mount to esp_vfs_fat_spiflash_mount_ro and the
# partition to its nominal size.
partition_table_get_partition_info(storage_offset "--partition-name storage" "offset")
if(storage_offset)
    fatfs_create_rawflash_image(storage "${CMAKE_CURRENT_SOURCE_DIR}/stream" FLASH_IN_PROJECT)
endif()
