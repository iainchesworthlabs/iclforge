# iclforge-device-tests - the device-side logic's Catch2 tests (cmake/IclforgeTests.cmake).
#
# They were libs/ac3/tests/io's, built against the ESP-IDF component's include directory; the
# headers they hold to the host are libs/device's now, and a test of a header is the header's.
iclforge_add_test_binary(iclforge-device-tests PROJECT device SOURCES
    test_ac4_sync_frame.cpp
    test_block_ring.cpp
    test_dac_queue_model.cpp
    test_dev_view.cpp
    test_firmware_image.cpp
    test_firmware_status.cpp
    test_firmware_trial.cpp
    test_firmware_update.cpp
    test_hardware_info.cpp
    test_improv.cpp
    test_interleave.cpp
    test_log_ring.cpp
    test_now_view.cpp
    test_object_limit.cpp
    test_pairing_records.cpp
    test_pcm_unpack.cpp
    test_playout.cpp
    test_sink_plan.cpp
    test_tcp_arrivals.cpp
    test_title_card.cpp
    test_unit_hold.cpp
    test_wall_clock.cpp)

target_link_libraries(iclforge-device-tests PRIVATE iclforge::device)
