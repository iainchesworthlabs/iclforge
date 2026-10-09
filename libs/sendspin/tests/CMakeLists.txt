# iclforge-sendspin-tests - the Sendspin library's Catch2 tests (cmake/IclforgeTests.cmake).
cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH _sendspin_root)

iclforge_add_test_binary(iclforge-sendspin-tests PROJECT sendspin SOURCES
    test_iclforge_player.cpp
    test_arbiter.cpp
    test_base64.cpp
    test_base64url.cpp
    test_chunks.cpp
    test_clock_sync.cpp
    test_discovery.cpp
    test_frames.cpp
    test_handshake.cpp
    test_json.cpp
    test_messages.cpp
    test_pairing_messages.cpp
    test_state_roles.cpp
    test_stream_roles.cpp
    test_transport.cpp)

target_link_libraries(iclforge-sendspin-tests PRIVATE iclforge::sendspin iclforge::ac3 iclforge::containers)

# The parts that need vcpkg's "hearth" feature, which ICLFORGE_SENDSPIN_CORE_ONLY leaves out.
if(NOT ICLFORGE_SENDSPIN_CORE_ONLY)
    target_sources(iclforge-sendspin-tests PRIVATE
        test_codec.cpp
        test_cpace.cpp
        test_crypto.cpp
        test_handshake_session.cpp
        test_mdns.cpp
        test_noise.cpp
        test_pairing.cpp
        test_pairing_flow.cpp
        test_pairing_flow_errors.cpp
        test_session_driver.cpp
        test_sessions.cpp)
    # test_mdns.cpp reads the mdns backend's packet layer, which is private to the library.
    target_include_directories(iclforge-sendspin-tests PRIVATE "${_sendspin_root}/src")
    # test_websocket.cpp writes a WebSocket client's bytes by hand through cpp-httplib's socket
    # functions, so it is compiled on its own with cpp-httplib configured as libs/sendspin
    # configures it (iclforge_sendspin_httplib), leaving the rest of the suite's flags alone.
    add_library(iclforge_sendspin_tests_websocket OBJECT test_websocket.cpp)
    target_link_libraries(iclforge_sendspin_tests_websocket
        PRIVATE iclforge::sendspin iclforge_sendspin_httplib iclforge::warnings iclforge::coverage Catch2::Catch2)
    target_link_libraries(iclforge-sendspin-tests PRIVATE iclforge_sendspin_tests_websocket)
endif()
