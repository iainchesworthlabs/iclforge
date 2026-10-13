#include "../../code_page_convert.hpp"

#include <algorithm>
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

// iconv's own failure value, with errno saying which.
constexpr std::size_t kFailed = static_cast<std::size_t>(-1);

// The longest character of any page in the table, in bytes: GB18030's.
constexpr std::size_t kLongestCharacter = 4;

// Whether `text` is characters of the page from its first byte to its last, each one asked of the
// conversion alone; `cd` is left in its initial state.
//
// Converting the whole text does not say so on every iconv. macOS 15's takes a pair of Shift-JIS
// bytes that is no character (a lead byte and a trail that is none) for nothing at all when a
// character comes before it in the same call: no error, a count of 0, the pair consumed and
// nothing written for it. First in a call the same pair is EILSEQ, there as everywhere, so a
// character offered alone has nothing before it to hide behind. (Its Shift-JIS reader moves the
// input on before it refuses the trail, and the loop that called it carries on from there; macOS
// 26's puts the input back.)
//
// A character is found by offering one byte and, while iconv says the bytes stop short of a
// character (EINVAL), one more. What the conversion wrote for it is counted only after it has
// been asked for what it holds: glibc's windows-1255 and windows-1258 hold a letter back, to join
// it to a mark that may follow, and write nothing for it until the next character or the end of
// the text. So taking a character and writing nothing is not by itself a refusal; having nothing
// to show for it when asked is, since every character of these pages has one in Unicode.
bool every_character_is_the_pages(iconv_t cd, std::string& text) {
    // One character as UTF-8, or the two an iconv may write for one.
    char buffer[32];
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t most = std::min(kLongestCharacter, text.size() - at);
        std::size_t length = 1;
        for (;; ++length) {
            char* in = text.data() + at;
            std::size_t in_left = length;
            char* out = buffer;
            std::size_t out_left = sizeof buffer;
            const std::size_t result = iconv(cd, &in, &in_left, &out, &out_left);
            const int error = result == kFailed ? errno : 0;
            // What it holds, and back to the initial state for the next offer: whatever this one
            // left behind (a letter held back, or the start of a character taken in before the
            // rest was missed) is not the next one's.
            const std::size_t held = iconv(cd, nullptr, nullptr, &out, &out_left);
            if (result == kFailed && error == EINVAL && length < most) {
                continue;
            }
            // A failure is bytes the page does not have, or a character cut short by the end of
            // the text. A count is a character another was put in the place of (see below).
            if (result != 0 || held == kFailed || in_left != 0 || out_left == sizeof buffer) {
                return false;
            }
            break;
        }
        at += length;
    }
    return true;
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
    // The text is converted whole, as it always was, so that a letter and the mark after it are
    // joined where the system's conversion joins them; but only a text every character of which
    // the page has, since the whole conversion cannot be trusted to refuse the others.
    bool ok = every_character_is_the_pages(cd, input);
    char* in = input.data();
    std::size_t in_left = input.size();
    char buffer[4096];
    while (in_left > 0 && ok) {
        char* out = buffer;
        std::size_t out_left = sizeof buffer;
        const std::size_t result = iconv(cd, &in, &in_left, &out, &out_left);
        utf8.append(buffer, sizeof buffer - out_left);
        // E2BIG: the buffer filled, and the rest is still to convert. Anything else (an invalid
        // sequence, or one cut short at the end) is bytes the page does not have.
        if (result == kFailed) {
            if (errno != E2BIG) {
                ok = false;
            }
        } else if (result != 0) {
            // A count of conversions that cannot be reversed. Every character of these pages has
            // one in Unicode, so a conversion that put another in a character's place was given
            // bytes the page does not have: an iconv may substitute and go on, counting it here,
            // where glibc's and GNU libiconv stop with EILSEQ.
            ok = false;
        }
    }
    if (ok) {
        // What the conversion still holds: on glibc the last letter of a windows-1255 or
        // windows-1258 text, which is written here and nowhere else.
        char* out = buffer;
        std::size_t out_left = sizeof buffer;
        if (iconv(cd, nullptr, nullptr, &out, &out_left) == kFailed) {
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
