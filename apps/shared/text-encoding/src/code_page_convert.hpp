#pragma once

#include <string>
#include <string_view>

#include "code_pages.hpp"

// The two functions each platform defines, exactly one src/platform/<os>/code_page_convert.cpp in
// a build and no #ifdef choosing between them: the shape src/platform/ has in apps/hearth/engine.
// code_pages.cpp's code_page_available() and code_page_to_utf8() are these, with the table.
namespace iclforge::apps::platform {

// Whether the operating system has the page.
[[nodiscard]] bool code_page_installed(const CodePage& page);

// `bytes` in `page` as UTF-8, replacing `utf8`. False, and `utf8` empty, for bytes the page does
// not have or a page the system lacks.
[[nodiscard]] bool convert_to_utf8(const CodePage& page, std::string_view bytes, std::string& utf8);

}  // namespace iclforge::apps::platform
