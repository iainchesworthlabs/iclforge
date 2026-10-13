#include "iclforge/base/number_parse.hpp"

#include <cmath>
#include <system_error>

#include <fast_float/fast_float.h>

namespace iclforge::base {

std::optional<ParsedDouble> parse_double(std::string_view text, NumberSyntax syntax) noexcept {
    namespace ff = fast_float;

    ff::chars_format format = ff::chars_format::general;
    if (syntax.leading_plus) {
        format |= ff::chars_format::allow_leading_plus;
    }
    if (syntax.leading_space) {
        format |= ff::chars_format::skip_white_space;
    }

    const char* const first = text.data();
    const char* const last = first + text.size();
    double value = 0.0;
    const ff::from_chars_result result = ff::from_chars(first, last, value, format);

    // invalid_argument leaves ptr at the start; an empty text leaves it at the end, so the code is
    // asked as well as the position.
    if (result.ec == std::errc::invalid_argument || result.ptr != last) {
        return std::nullopt;
    }
    if (result.ec == std::errc::result_out_of_range) {
        if (std::isinf(value)) {
            return std::nullopt;
        }
        return ParsedDouble{.value = value, .underflowed = true};
    }
    return ParsedDouble{.value = value, .underflowed = false};
}

}  // namespace iclforge::base
