#include "../../code_page_convert.hpp"

#include <string>
#include <string_view>

// Android: no conversion. Bionic declares iconv from API 28 and the Android builds of this
// repository are made for API 26 (CMakePresets.json's android-arm64, apps/hearth/android), so the
// header is not there to call. Nothing on Android reads a lyrics file in a code page: the sink's
// documents come from a server as UTF-8, and this project is in the Android configure only
// because the tests that are on link it. A page is reported as not installed, and a conversion
// as not made, which is what code_pages.cpp already says of a page a system lacks.

namespace iclforge::apps::platform {

bool code_page_installed(const CodePage& /*page*/) {
    return false;
}

bool convert_to_utf8(const CodePage& /*page*/, std::string_view /*bytes*/, std::string& utf8) {
    utf8.clear();
    return false;
}

}  // namespace iclforge::apps::platform
