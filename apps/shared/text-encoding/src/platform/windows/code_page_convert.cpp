#include "../../code_page_convert.hpp"

#include <climits>
#include <cstddef>
#include <string>
#include <string_view>

#include <windows.h>

namespace iclforge::apps::platform {

bool code_page_installed(const CodePage& page) {
    return IsValidCodePage(page.windows_id) != 0;
}

bool convert_to_utf8(const CodePage& page, std::string_view bytes, std::string& utf8) {
    utf8.clear();
    if (bytes.empty()) {
        return true;
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    const int in_size = static_cast<int>(bytes.size());
    // MB_ERR_INVALID_CHARS: a lead byte with no trail, or a byte the page has no character for,
    // fails the call, where without it each is replaced and the text is read as if it were fine.
    // Every page of the table takes the flag (a few Windows pages, ISO-2022 and UTF-7, do not, and
    // are not in it: EUC-JP is 20932 here and not 51932, which Windows' tables do not carry).
    const int wide_size =
        MultiByteToWideChar(page.windows_id, MB_ERR_INVALID_CHARS, bytes.data(), in_size, nullptr, 0);
    if (wide_size <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(page.windows_id, MB_ERR_INVALID_CHARS, bytes.data(), in_size, wide.data(),
                            wide_size) != wide_size) {
        return false;
    }
    const int out_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_size,
                                             nullptr, 0, nullptr, nullptr);
    if (out_size <= 0) {
        return false;
    }
    utf8.resize(static_cast<std::size_t>(out_size));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_size, utf8.data(), out_size,
                            nullptr, nullptr) != out_size) {
        utf8.clear();
        return false;
    }
    return true;
}

}  // namespace iclforge::apps::platform
