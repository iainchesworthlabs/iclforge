#pragma once

// Whether this build runs the tests under the sanitizers (cmake/Sanitizers.cmake).
//
// Under ASan and UBSan (built at -O1, CMakePresets.json) the codecs run
// several times slower than in a release build, and the sanitizer leg of CI
// runs ctest serially (the tests share scratch paths), so the heaviest tests
// take less there: fewer frames, shorter signals, a stride through their
// cases, one frame rate or layout where a normal build takes each. Each still
// runs every code path it covers at least once, to the same tolerances; a
// normal build runs every case at full length.
//
// tests/support/CMakeLists.txt defines ICLFORGE_TEST_SANITIZED as 1 when
// ICLFORGE_SANITIZERS names any and as 0 when it does not, so that no test
// asks the preprocessor which it is.

namespace iclforge::test {

inline constexpr bool kSanitized = ICLFORGE_TEST_SANITIZED != 0;

}  // namespace iclforge::test
