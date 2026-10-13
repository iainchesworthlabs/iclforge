// iclforge::base::parse_double for the builds that carry no third-party library: the minimum-
// footprint decoder and encoder (libs/ac3/minimal.cmake lists this file, libs/base/CMakeLists.txt
// does not). The syntax is checked here, by hand, against what std::from_chars takes, and only
// then is the token handed to strtod: with it already known to be plain decimal text, strtod's
// other behaviours (hex floats, a leading '+' or white space) cannot be reached. The one that
// remains is the process locale's decimal point, which strtod reads and the token does not carry:
// the token's '.' is replaced by the locale's own (localeconv) before the call, so that the answer
// is the same under any LC_NUMERIC, as it is for fast_float. On the targets this file is for there
// is no setlocale and the replacement is the identity.
//
// The ordinary build uses number_parse.cpp, which is fast_float. Both are held to the same cases
// by libs/base/tests/test_number_parse.cpp.

#include "iclforge/base/number_parse.hpp"

#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace iclforge::base {

namespace {

[[nodiscard]] constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

[[nodiscard]] constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// `text` from `at` is, ignoring case, `word` and nothing longer.
[[nodiscard]] bool word_at(std::string_view text, std::size_t at, std::string_view word) noexcept {
    if (text.size() - at < word.size()) {
        return false;
    }
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (lower(text[at + i]) != word[i]) {
            return false;
        }
    }
    return true;
}

// The number's own syntax, from `i`, to the end of `text`: [-] digits [. digits] [e [+-] digits],
// with at least one digit in the significand, or inf / infinity / nan / nan(...). Whether the
// whole of `text` was it is the return value. `minus` is whether a '-' may start it: not after a
// '+' that was already taken, so that "+-1" is refused as std::from_chars refuses it.
[[nodiscard]] bool is_decimal_number(std::string_view text, std::size_t i, bool minus) noexcept {
    const std::size_t n = text.size();
    if (minus && i < n && text[i] == '-') {
        ++i;
    }
    if (i == n) {
        return false;
    }
    if (text[i] == 'i' || text[i] == 'I' || text[i] == 'n' || text[i] == 'N') {
        if (word_at(text, i, "infinity") && n - i == 8) {
            return true;
        }
        if (word_at(text, i, "inf") && n - i == 3) {
            return true;
        }
        if (word_at(text, i, "nan")) {
            if (n - i == 3) {
                return true;
            }
            // nan(n-char-sequence)
            if (text[i + 3] != '(' || text[n - 1] != ')') {
                return false;
            }
            for (std::size_t k = i + 4; k + 1 < n; ++k) {
                const char c = text[k];
                if (!(is_digit(c) || (lower(c) >= 'a' && lower(c) <= 'z') || c == '_')) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }
    std::size_t digits = 0;
    while (i < n && is_digit(text[i])) {
        ++i;
        ++digits;
    }
    if (i < n && text[i] == '.') {
        ++i;
        while (i < n && is_digit(text[i])) {
            ++i;
            ++digits;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < n && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        std::size_t exponent_digits = 0;
        while (i < n && is_digit(text[i])) {
            ++i;
            ++exponent_digits;
        }
        if (exponent_digits == 0) {
            return false;
        }
    }
    return i == n;
}

}  // namespace

std::optional<ParsedDouble> parse_double(std::string_view text, NumberSyntax syntax) noexcept {
    std::size_t i = 0;
    if (syntax.leading_space) {
        while (i < text.size() && is_space(text[i])) {
            ++i;
        }
    }
    // A '+' is only ever the very first character after the white space, and only when asked for;
    // it is dropped here and the sign is the digits' own from then on.
    std::string_view number = text.substr(i);
    bool minus = true;
    if (syntax.leading_plus && !number.empty() && number.front() == '+') {
        number.remove_prefix(1);
        minus = false;
    }
    if (number.empty() || !is_decimal_number(number, 0, minus)) {
        return std::nullopt;
    }

    // strtod needs a NUL-terminated buffer, and `number` is a view into someone else's storage.
    std::string buffer(number);
    // The locale's decimal point, in place of the token's '.', so that strtod reads what was
    // written.
    const char* const point = std::localeconv()->decimal_point;
    if (point != nullptr && point[0] != '\0' && !(point[0] == '.' && point[1] == '\0')) {
        const std::size_t dot = buffer.find('.');
        if (dot != std::string::npos) {
            buffer.replace(dot, 1, point);
        }
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size()) {
        return std::nullopt;
    }
    if (errno == ERANGE) {
        if (std::isinf(value)) {
            return std::nullopt;
        }
        if (value == 0.0) {
            return ParsedDouble{.value = value, .underflowed = true};
        }
    }
    return ParsedDouble{.value = value, .underflowed = false};
}

}  // namespace iclforge::base
