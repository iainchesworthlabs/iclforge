# ---------------------------------------------------------------------------
# The TrueHD decoder on a chip: what it costs on an ESP32-C6, -S3 or -P4, measured.
#
# planning/truehd-parity-esp32-sinks.md estimated these from a host and a ratio taken from the AC-3
# decoder's board tables; this program replaces the estimates. It decodes the committed TrueHD
# fixtures (testdata/truehd) from flash, checks each decode against the PCM hash FFmpeg's strict decode
# gives (testdata/truehd/manifest.json), and prints what each cost: microseconds and CPU cycles an
# access unit (the slowest and the mean), the real-time factor, the heap the decoder holds and the stack
# it reached. No network, no I2S, no PSRAM unless the overlay turns it on: the numbers are the decoder's
# alone, as firmware/baremetal's probe gives them for the AC-3 decoder.
#
#   . D:\esp\esp-idf\export.ps1   (with IDF_TOOLS_PATH=D:\esp\tools)
#   idf.py -B <dir> -DIDF_TARGET=esp32s3 "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.s3" build
#   idf.py -B <dir> -p <PORT> flash monitor
#
# The decoder is compiled from libs/truehd's own sources by main/CMakeLists.txt: the sources a decode
# reads (the CRCs, the syntax, the decoder, the access unit and the major sync) and the headers of
# libs/base it includes. Nothing else of the repository is in it.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(iclforge_truehd_probe LANGUAGES C CXX)
