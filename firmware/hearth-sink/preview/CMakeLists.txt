# The lyric screen on a computer: the SAME firmware/hearth-sink/main/panel/lvgl/lyric_screen.cpp, drawn
# by LVGL's software renderer into a buffer and written out as pictures, for the design sheet
# (docs/hearth/sink-esp32-p4.md) and for checking a change to the screen in seconds rather than in a
# firmware build and a flash. What it leaves out is the board: the Sendspin player, the DSI host and
# the PSRAM.
#
#   cmake -S firmware/hearth-sink/preview -B build-preview -DLVGL_DIR=<managed_components/lvgl__lvgl>
#   cmake --build build-preview
#   build-preview/panel_preview <output directory>
#
# LVGL_DIR is a checkout of the lvgl 9.5 the firmware uses: the registry's copy that a P4 build of
# firmware/hearth-sink puts under managed_components/lvgl__lvgl. Not part of the project's own build,
# and no CI job runs it: it is a developer's tool.
cmake_minimum_required(VERSION 3.28)
project(panel_preview LANGUAGES C CXX)

set(LVGL_DIR "" CACHE PATH "lvgl 9.5 (managed_components/lvgl__lvgl of a P4 build)")
if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
    message(FATAL_ERROR "set LVGL_DIR to an lvgl 9.5 checkout (no lvgl.h in '${LVGL_DIR}')")
endif()

set(HERE "${CMAKE_CURRENT_LIST_DIR}")
set(MAIN "${HERE}/../main")
set(REPO "${HERE}/../../..")

set(LV_BUILD_CONF_PATH "${HERE}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_DIR}" lvgl EXCLUDE_FROM_ALL)

add_executable(panel_preview
    preview.cpp
    "${MAIN}/panel/lvgl/lyric_screen.cpp"
    "${MAIN}/panel/lvgl/dev_screen.cpp"
    "${MAIN}/panel/lvgl/glyphs.cpp"
    "${REPO}/libs/lyrics/src/lyrics.cpp"
    "${REPO}/libs/lyrics/src/stage.cpp")
target_compile_features(panel_preview PRIVATE cxx_std_23)
target_include_directories(panel_preview PRIVATE
    "${MAIN}"
    "${REPO}/libs/device/include"
    "${REPO}/libs/lyrics/include")
target_compile_definitions(panel_preview PRIVATE
    "PREVIEW_FONT_DIR=\"${MAIN}/panel/fonts\""
    "PREVIEW_EXAMPLES_DIR=\"${REPO}/examples/karaoke\"")
target_link_libraries(panel_preview PRIVATE lvgl)
