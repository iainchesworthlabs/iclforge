#include "../../code_page_convert.hpp"

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>

#include <iconv.h>

namespace iclforge::apps::platform {

namespace {

// iconv_open's failure value, which is (iconv_t)-1 and is named by no macro.
bool opened(iconv_t cd) {
    return cd != reinterpret_cast<iconv_t>(-1);
}

}  // namespace

bool code_page_installed(const CodePage& page) {
    // The name is a string literal in the table, but iconv_open takes a C string and a string_view
    // is not promised one.
    const std::string name{page.iconv_name};
    const iconv_t cd = iconv_open("UTF-8", name.c_str());
    if (!opened(cd)) {
        return false;
    }
    iconv_close(cd);
    return true;
}

bool convert_to_utf8(const CodePage& page, std::string_view bytes, std::string& utf8) {
    utf8.clear();
    if (bytes.empty()) {
        return true;
    }
    const std::string name{page.iconv_name};
    const iconv_t cd = iconv_open("UTF-8", name.c_str());
    if (!opened(cd)) {
        return false;
    }
    // iconv wants a non-const input pointer and moves it; a copy is the input.
    std::string input{bytes};
    char* in = input.data();
    std::size_t in_left = input.size();
    char buffer[4096];
    bool ok = true;
    while (in_left > 0 && ok) {
        char* out = buffer;
        std::size_t out_left = sizeof buffer;
        const std::size_t result = iconv(cd, &in, &in_left, &out, &out_left);
        utf8.append(buffer, sizeof buffer - out_left);
        // E2BIG: the buffer filled, and the rest is still to convert. Anything else (an invalid
        // sequence, or one cut short at the end) is bytes the page does not have.
        if (result == static_cast<std::size_t>(-1) && errno != E2BIG) {
            ok = false;
        }
    }
    if (ok) {
        // The end of a shift sequence; none of these pages has one, and it costs nothing to ask.
        char* out = buffer;
        std::size_t out_left = sizeof buffer;
        if (iconv(cd, nullptr, nullptr, &out, &out_left) == static_cast<std::size_t>(-1)) {
            ok = false;
        } else {
            utf8.append(buffer, sizeof buffer - out_left);
        }
    }
    iconv_close(cd);
    if (!ok) {
        utf8.clear();
    }
    return ok;
}

}  // namespace iclforge::apps::platform
