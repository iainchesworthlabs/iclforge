# iclforge-device-tests - the device-side logic's Catch2 tests (cmake/IclforgeTests.cmake).
#
# They were libs/ac3/tests/io's, built against the ESP-IDF component's include directory; the
# headers they hold to the host are libs/device's now, and a test of a header is the header's.
iclforge_add_test_binary(iclforge-device-tests PROJECT device SOURCES
    test_about_view.cpp
    test_ac4_sync_frame.cpp
    test_block_ring.cpp
    test_board_caps.cpp
    test_dac_queue_model.cpp
    test_decoder_keep.cpp
    test_dev_view.cpp
    test_device_name.cpp
    test_firmware_image.cpp
    test_firmware_status.cpp
    test_firmware_trial.cpp
    test_firmware_update.cpp
    test_hardware_info.cpp
    test_improv.cpp
    test_interleave.cpp
    test_layout_builder.cpp
    test_layout_options.cpp
    test_log_ring.cpp
    test_meters_tap.cpp
    test_meters_view.cpp
    test_meters_draw.cpp
    test_meters_governor.cpp
    test_panel_stats.cpp
    test_now_view.cpp
    test_object_limit.cpp
    test_pairing_records.cpp
    test_panel_follow.cpp
    test_pcm_unpack.cpp
    test_playout.cpp
    test_preferences.cpp
    test_screen_metrics.cpp
    test_settings_keys.cpp
    test_sink_plan.cpp
    test_slot_width_persistence.cpp
    test_system_actions.cpp
    test_system_prefs.cpp
    test_tcp_arrivals.cpp
    test_time_zones.cpp
    test_title_card.cpp
    test_unit_hold.cpp
    test_wall_clock.cpp)

# iclforge::render is the judge of what a layout is in test_layout_options.cpp (its parser and slot
# counts); the headers under test stay free of it.
target_link_libraries(iclforge-device-tests PRIVATE iclforge::device iclforge::render)
# test_settings_keys.cpp reads two sources of the firmware, to hold the table to what settings.cpp does, and
# test_panel_follow.cpp reads the panel's, to hold its setters to taking no lock.
target_compile_definitions(iclforge-device-tests PRIVATE "ICLFORGE_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
