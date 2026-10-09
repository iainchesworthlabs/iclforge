# iclforge-objects-tests - the object model's Catch2 tests (cmake/IclforgeTests.cmake).
iclforge_add_test_binary(iclforge-objects-tests PROJECT objects SOURCES
    test_scene.cpp
    test_scene_osc.cpp)

target_link_libraries(iclforge-objects-tests PRIVATE iclforge::objects iclforge::ac3)
