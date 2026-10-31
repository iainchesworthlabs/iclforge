// The stand-in for test_performance_truehd.cpp when iclforge::truehd was not built
// (ICLFORGE_BUILD_TRUEHD=OFF): the same four cases, each skipping, so a run says the guards were not
// run and the set of tests is the same in both configurations.

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the TrueHD stereo encoder stays faster than real time") {
    SKIP("this build has no TrueHD library (ICLFORGE_BUILD_TRUEHD=OFF)");
}

TEST_CASE("the TrueHD 5.1 encoder stays faster than real time") {
    SKIP("this build has no TrueHD library (ICLFORGE_BUILD_TRUEHD=OFF)");
}

TEST_CASE("the TrueHD stereo decoder stays faster than real time") {
    SKIP("this build has no TrueHD library (ICLFORGE_BUILD_TRUEHD=OFF)");
}

TEST_CASE("the TrueHD 5.1 decoder stays faster than real time") {
    SKIP("this build has no TrueHD library (ICLFORGE_BUILD_TRUEHD=OFF)");
}
