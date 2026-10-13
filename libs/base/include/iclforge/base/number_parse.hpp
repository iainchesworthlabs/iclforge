#pragma once

#include <optional>
#include <string_view>

#include "iclforge/base/export.hpp"

// Decimal text to a double: what std::from_chars reads, and not what strtod reads.
//
// strtod's answer depends on the process locale (a decimal comma under a German LC_NUMERIC turns
// "1.5" into 1 with ".5" left over), on the C library (a subnormal result sets ERANGE on Windows'
// and not on Android's), and it takes things nobody means: a leading '+', leading white space, hex
// floats. std::from_chars has none of that, and its floating-point overloads are missing from
// NDK r28's libc++ and from the macOS wheel's deployment target (CONTRIBUTING.md), which is why
// this exists: it is fast_float's from_chars (the library std::from_chars for floating point was
// standardised from), built into iclforge::base, with one place to say what the tree accepts.
//
// The whole of `text` must be one number. "1.5 ", "1.5abc", "1,5" and "" are refused, which is
// the contract every caller in this tree wanted of strtod and wrote `end == buffer + size` to get.
// "inf", "infinity" and "nan" (any case) are numbers, as for std::from_chars.
//
// The syntax is strict by default. A caller reading text a person typed (a command line, a field
// in a window, a hand-written file) asks for kUserNumber, which also takes what strtod used to: a
// leading '+' and leading white space. Hex floats are refused either way.
//
// The result separates the two ways a decimal can be out of range, because the callers differ on
// the second. Too large for a double: std::nullopt, a refusal. Too small, a decimal that rounds to
// zero ("1e-999"): the correctly rounded value, a signed zero, with `underflowed` set; a caller
// that must not take a nonzero token as zero refuses it, one that wants the nearest double takes
// it. A subnormal ("4.9e-324") is a value, not an underflow.
//
// A build with no third-party libraries (ICLFORGE_MINIMAL_DECODER, ICLFORGE_MINIMAL_ENCODER) gets
// this function from libs/base/src/number_parse_libc.cpp, which checks the same syntax by hand and
// reads the digits with strtod, given the locale's own decimal point so that it too is locale-
// independent; the two are held to the same table of cases (libs/base/tests/test_number_parse.cpp).

namespace iclforge::base {

struct NumberSyntax {
    bool leading_plus = false;   // "+1.5"
    bool leading_space = false;  // " \t1.5": ASCII white space before the number
};

inline constexpr NumberSyntax kStrictNumber{};
inline constexpr NumberSyntax kUserNumber{.leading_plus = true, .leading_space = true};

struct ParsedDouble {
    double value = 0.0;
    bool underflowed = false;  // a nonzero decimal that rounds to (signed) zero
};

[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<ParsedDouble> parse_double(
    std::string_view text, NumberSyntax syntax = kStrictNumber) noexcept;

}  // namespace iclforge::base
