# Host tests of the ESP-IDF component's (../iclforge) TrueHD path (a directory of its own, so that the
# component archive tools/packaging/pack_esp_component.py makes carries none of it): burst_player.cpp and truehd_stream.cpp compiled
# for the host over stand-ins for the part's kernel, heap and clock (shims/*.shim, shim_runtime.cpp), and
# run as the Sendspin session drives them - chunks into the player's ring, a decode task, a sink that
# keeps what it is written. Off unless ICLFORGE_BUILD_FIRMWARE_HOST_TESTS is on (Hearth, TrueHD and
# the tests are needed, and it is one more leg of the build to hold).
#
#   iclforge-fw-host-tests    the Catch2 tests: batched blocks equal one unit a block, bit for bit
#   iclforge-fw-host-bench    timings and a digest of the output (bench_truehd_block_path.cpp)
#   iclforge-fw-host-stages   the cost of each stage of the path a call and a sample (bench_stages.cpp)
#
# ICLFORGE_FW_HOST_ALT_ROOT, when set to another copy of the component's player (a mutant, to see the
# tests fail), runs the tests against that copy as iclforge-fw-host-tests-alt.
#
# ICLFORGE_FW_HOST_BASE_ROOT, when set to another copy of the component (a checkout of an earlier
# commit's firmware/esp-idf/iclforge), builds that copy's player the same way as
# iclforge-fw-host-bench-base, so that the digests of the two can be compared.

set(ICLFORGE_FW_HOST_ALT_ROOT "" CACHE PATH
    "Another copy of firmware/esp-idf/iclforge to run the host tests against as iclforge-fw-host-tests-alt")
set(ICLFORGE_FW_HOST_BASE_ROOT "" CACHE PATH
    "Another copy of firmware/esp-idf/iclforge to build the host bench against as iclforge-fw-host-bench-base")

# The stand-ins for the part's headers (shims/*.shim), as the headers they stand in for, in the build
# directory: a tracked header spelled freertos/FreeRTOS.h would be the one every firmware project's
# include of it resolves to in tools/checks/check_layering.py.
set(ICLFORGE_FW_HOST_SHIMS sdkconfig.h esp_timer.h esp_heap_caps.h freertos/FreeRTOS.h freertos/event_groups.h
    freertos/idf_additions.h freertos/message_buffer.h freertos/task.h)
foreach(_shim IN LISTS ICLFORGE_FW_HOST_SHIMS)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/shims/${_shim}.shim" "${CMAKE_CURRENT_BINARY_DIR}/shims/${_shim}" COPYONLY)
endforeach()

# The component's player, from the sources of the copy at `root`, as a library of its own: it is
# written for the part's compiler and the part's warnings, so it is built without the project's.
function(iclforge_fw_host_player target root)
    add_library(${target} STATIC
        "${root}/src/burst_player.cpp"
        "${root}/src/truehd_stream.cpp"
        "${root}/src/ac4_stream_none.cpp"
        "${root}/src/opus_stream_none.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/shim_runtime.cpp")
    target_include_directories(${target}
        PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/shims" "${CMAKE_CURRENT_LIST_DIR}" "${root}/include" "${root}/src")
    target_link_libraries(${target}
        PUBLIC iclforge::device iclforge::render iclforge::ac3 iclforge::truehd iclforge::sendspin iclforge::base
               iclforge::objects)
    target_compile_features(${target} PUBLIC cxx_std_23)
    find_package(Threads REQUIRED)
    target_link_libraries(${target} PUBLIC Threads::Threads)
endfunction()

iclforge_fw_host_player(iclforge_fw_host_player "${CMAKE_CURRENT_LIST_DIR}/../iclforge")

iclforge_add_test_binary(iclforge-fw-host-tests PROJECT firmware SOURCES test_truehd_block_path.cpp)
target_link_libraries(iclforge-fw-host-tests PRIVATE iclforge_fw_host_player)
target_compile_definitions(iclforge-fw-host-tests PRIVATE "ICLFORGE_TESTDATA_DIR=\"${CMAKE_SOURCE_DIR}/testdata\"")

add_executable(iclforge-fw-host-bench bench_truehd_block_path.cpp)
target_link_libraries(iclforge-fw-host-bench PRIVATE iclforge_fw_host_player)

# What each stage of the path costs a call and a sample (bench_stages.cpp): the libraries alone.
add_executable(iclforge-fw-host-stages bench_stages.cpp)
target_link_libraries(iclforge-fw-host-stages PRIVATE iclforge::device iclforge::render iclforge::ac3)
target_include_directories(iclforge-fw-host-stages PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../iclforge/include")
target_compile_features(iclforge-fw-host-stages PRIVATE cxx_std_23)

if(ICLFORGE_FW_HOST_BASE_ROOT)
    iclforge_fw_host_player(iclforge_fw_host_player_base "${ICLFORGE_FW_HOST_BASE_ROOT}")
    add_executable(iclforge-fw-host-bench-base bench_truehd_block_path.cpp)
    target_link_libraries(iclforge-fw-host-bench-base PRIVATE iclforge_fw_host_player_base)
endif()

if(ICLFORGE_FW_HOST_ALT_ROOT)
    iclforge_fw_host_player(iclforge_fw_host_player_alt "${ICLFORGE_FW_HOST_ALT_ROOT}")
    iclforge_add_test_binary(iclforge-fw-host-tests-alt PROJECT firmware SOURCES test_truehd_block_path.cpp)
    target_link_libraries(iclforge-fw-host-tests-alt PRIVATE iclforge_fw_host_player_alt)
    target_compile_definitions(iclforge-fw-host-tests-alt PRIVATE "ICLFORGE_TESTDATA_DIR=\"${CMAKE_SOURCE_DIR}/testdata\"")
endif()
