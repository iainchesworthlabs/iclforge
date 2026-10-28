# ---------------------------------------------------------------------------
# iclforge's crypto bench: SHA-256, HMAC-SHA-256 and the Sendspin transport AEAD on an ESP32 board,
# through the PSA Crypto API the firmware uses (which ESP-IDF routes to the SHA and AES peripherals),
# against iclforge::base::crypto's portable C++ as the software reference. See main/crypto_bench.cpp
# and README.md. It is a measurement tool, not firmware: it has its own partition table and takes a
# board's flash, so a board's as-found image has to be backed up first (README.md).
#
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32s3       # or esp32c6, esp32p4
#   idf.py -p <PORT> flash monitor
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The build that is put on the P4X kit over OTA carries the Hearth sink's project name: the sink's
# update takes an image of its own project and no other (libs/device firmware_image.hpp,
# refuse_image). It is not the sink: the version says so, and it puts the board back on its own image
# by itself (main/net_report.hpp). Every other build is the bench under its own name.
if(CRYPTO_BENCH_REPORT STREQUAL "ethernet")
    set(PROJECT_VER "crypto-bench-ota")
    project(iclforge_hearth_sink LANGUAGES C CXX)
else()
    project(iclforge_crypto_bench LANGUAGES C CXX)
endif()
