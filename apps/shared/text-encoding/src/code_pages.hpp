#pragma once

#include <span>
#include <string>
#include <string_view>

#include "iclforge/lyrics/lyrics.hpp"

// The legacy text encodings a lyrics file can be read in, and the platform's conversion of them to
// UTF-8 (planning/karaoke-formats-options.md, D16).
//
// libs/lyrics holds no table of any page (it is linked into firmware, and the tables of Shift-JIS,
// GBK and Big5 are tens of kilobytes each): it asks the program for a decoder, and only for text
// that is not UTF-8 and has no UTF-16 mark. This is the program's side. The conversion is the
// operating system's own, so that no table is carried here either: MultiByteToWideChar on Windows,
// iconv elsewhere (src/platform/windows, src/platform/posix).
//
// A page is never guessed at. A caller names one: forge's `lyrics-encoding=`, or the choice on the
// Forge GUI's Lyrics row.
namespace iclforge::apps {

struct CodePage {
    // What `lyrics-encoding=` takes and a project keeps: lower case, no spaces.
    std::string_view name;
    // The language group a picker puts it under, in English; the program translates it.
    std::string_view group;
    // The page's own name, as a person knows it ("Windows-1252", "Shift-JIS"); not translated.
    std::string_view title;
    // The Windows code page number (MultiByteToWideChar's first argument).
    unsigned windows_id;
    // The name iconv knows it by (glibc and GNU libiconv, which macOS carries).
    std::string_view iconv_name;
    // Other names it is known by, separated by spaces, in the same lower-case form as `name`.
    std::string_view aliases;
};

// Every page, in the order a picker lists them: the language groups together, each starting with
// the page that is most often meant.
[[nodiscard]] std::span<const CodePage> code_pages() noexcept;

// The page `name` stands for, by its name or one of its aliases. Case does not matter and `_`, a
// space and `-` are the same; `cp1252`, `latin1`, `sjis` and `Shift_JIS` are found. nullptr for a
// name that is none of them.
[[nodiscard]] const CodePage* find_code_page(std::string_view name) noexcept;

// The names `lyrics-encoding=` takes, in the picker's order, separated by ", ": for an error that
// says what would have worked.
[[nodiscard]] std::string code_page_names();

// Whether this system can convert the page. Windows carries all of them on any current version;
// a system with a smaller iconv (musl's, a stripped container) may not carry every one.
[[nodiscard]] bool code_page_available(const CodePage& page);

// `bytes` in `page`, as UTF-8 in `utf8`. False, with `utf8` empty, for bytes the page does not
// have (a lead byte with no trail, an unassigned code) or a page this system cannot convert. A
// page that leaves a single byte unassigned is the system's to say so: Windows maps some such
// bytes to the control character of the same value and iconv refuses them.
[[nodiscard]] bool code_page_to_utf8(const CodePage& page, std::string_view bytes, std::string& utf8);

// `page` as the decoder libs/lyrics asks a program for. `page` must outlive its use; a table
// entry (code_pages(), find_code_page()) always does.
[[nodiscard]] iclforge::lyrics::TextEncoding lyrics_encoding(const CodePage& page) noexcept;

}  // namespace iclforge::apps
