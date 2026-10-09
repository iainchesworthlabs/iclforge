# iclforge-adm-tests - the BW64/ADM parser's and its bridges' Catch2 tests
# (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-adm-tests PROJECT adm SOURCES
    test_adm.cpp
    test_adm_bridge.cpp
    test_adm_bridge_write.cpp
    test_iab_bridge.cpp)

target_link_libraries(iclforge-adm-tests PRIVATE iclforge::adm iclforge::iab iclforge::ac3)
