#include <catch2/catch_test_macros.hpp>

#include <array>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "iclforge/base/number_parse.hpp"

// iclforge::base::parse_double, and the same table for its libc variant: this file is compiled
// twice (libs/base/tests/CMakeLists.txt), once against the library's fast_float implementation and
// once with number_parse_libc.cpp, the one the minimum-footprint builds take.

namespace {

using iclforge::base::kStrictNumber;
using iclforge::base::kUserNumber;
using iclforge::base::parse_double;

constexpr double kMinNormal = std::numeric_limits<double>::min();
constexpr double kDenormMin = std::numeric_limits<double>::denorm_min();

struct Good {
    std::string_view text;
    double value;
};

}  // namespace

TEST_CASE("parse_double reads plain decimal numbers exactly", "[base][number-parse]") {
    constexpr std::array<Good, 14> kCases{{
        {"0", 0.0},
        {"1.5", 1.5},
        {"-0.25", -0.25},
        {"1e3", 1000.0},
        {"1E-3", 0.001},
        {".5", 0.5},
        {"5.", 5.0},
        {"-.5", -0.5},
        {"0.1", 0.1},
        {"1e23", 1e23},
        {"9007199254740993", 9007199254740992.0},  // 2^53 + 1, rounds to even
        {"2.2250738585072014e-308", kMinNormal},
        {"4.9e-324", kDenormMin},
        {"1.7976931348623157e308", std::numeric_limits<double>::max()},
    }};
    for (const Good& c : kCases) {
        CAPTURE(c.text);
        const auto parsed = parse_double(c.text);
        REQUIRE(parsed.has_value());
        CHECK(parsed->value == c.value);
        CHECK_FALSE(parsed->underflowed);
    }
    const auto negative_zero = parse_double("-0");
    REQUIRE(negative_zero.has_value());
    CHECK(std::signbit(negative_zero->value));
    CHECK(negative_zero->value == 0.0);
}

TEST_CASE("parse_double takes the infinities and the nan as std::from_chars does",
          "[base][number-parse]") {
    for (const std::string_view text : {"inf", "INF", "Infinity", "-inf", "-INFINITY"}) {
        CAPTURE(text);
        const auto parsed = parse_double(text);
        REQUIRE(parsed.has_value());
        CHECK(std::isinf(parsed->value));
        CHECK(std::signbit(parsed->value) == (text.front() == '-'));
    }
    for (const std::string_view text : {"nan", "NaN", "-nan", "nan(0x1)"}) {
        CAPTURE(text);
        const auto parsed = parse_double(text);
        REQUIRE(parsed.has_value());
        CHECK(std::isnan(parsed->value));
    }
}

TEST_CASE("parse_double is strict unless the caller asks for more", "[base][number-parse]") {
    // Each of these is something strtod used to take.
    for (const std::string_view text : {"+1.5", " 1.5", "\t1.5", "+inf", "0x1p3", "0X10", "1.5 "}) {
        CAPTURE(text);
        CHECK_FALSE(parse_double(text).has_value());
        CHECK_FALSE(parse_double(text, kStrictNumber).has_value());
    }
    // And these are never numbers.
    for (const std::string_view text :
         {"", "-", "+", ".", "e5", "1e", "1e+", "--1", "+-1", "-+1", "1,5", "1.5abc", "abc", "1 2",
          "nan(", "infinit", "in f", "1.5\n", "0x"}) {
        CAPTURE(text);
        CHECK_FALSE(parse_double(text).has_value());
        CHECK_FALSE(parse_double(text, kUserNumber).has_value());
    }
}

TEST_CASE("parse_double with the user syntax takes a leading plus and leading space",
          "[base][number-parse]") {
    constexpr std::array<Good, 6> kCases{{
        {"+1.5", 1.5},
        {" 1.5", 1.5},
        {"\t\n -2", -2.0},
        {" +3", 3.0},
        {"+.5", 0.5},
        {"  -0.25", -0.25},
    }};
    for (const Good& c : kCases) {
        CAPTURE(c.text);
        const auto parsed = parse_double(c.text, kUserNumber);
        REQUIRE(parsed.has_value());
        CHECK(parsed->value == c.value);
    }
    // Leading only: the whole of the text is still the number.
    CHECK_FALSE(parse_double("1.5 ", kUserNumber).has_value());
    CHECK_FALSE(parse_double("   ", kUserNumber).has_value());
    CHECK_FALSE(parse_double("+ 1", kUserNumber).has_value());
    // The two are separate options.
    constexpr iclforge::base::NumberSyntax only_plus{.leading_plus = true};
    constexpr iclforge::base::NumberSyntax only_space{.leading_space = true};
    CHECK(parse_double("+1", only_plus).has_value());
    CHECK_FALSE(parse_double(" 1", only_plus).has_value());
    CHECK(parse_double(" 1", only_space).has_value());
    CHECK_FALSE(parse_double("+1", only_space).has_value());
}

TEST_CASE("parse_double says which way a number is out of range", "[base][number-parse]") {
    for (const std::string_view text : {"1e999", "-1e999", "1.8e308", "1e+400"}) {
        CAPTURE(text);
        CHECK_FALSE(parse_double(text).has_value());
    }
    const auto tiny = parse_double("1e-999");
    REQUIRE(tiny.has_value());
    CHECK(tiny->underflowed);
    CHECK(tiny->value == 0.0);
    CHECK_FALSE(std::signbit(tiny->value));
    const auto negative_tiny = parse_double("-1e-999");
    REQUIRE(negative_tiny.has_value());
    CHECK(negative_tiny->underflowed);
    CHECK(std::signbit(negative_tiny->value));
    // A subnormal is a value, and a zero that is written as one is not an underflow.
    const auto subnormal = parse_double("1e-310");
    REQUIRE(subnormal.has_value());
    CHECK_FALSE(subnormal->underflowed);
    CHECK(subnormal->value > 0.0);
    const auto zero = parse_double("0e-999");
    REQUIRE(zero.has_value());
    CHECK_FALSE(zero->underflowed);
}

TEST_CASE("parse_double reads what %.17g writes back", "[base][number-parse]") {
    for (const double x : {0.1, -0.1, 1.0 / 3.0, 3.141592653589793, 1e-7, 123456.789, 1e21, 1e-300,
                           kMinNormal, kDenormMin, std::numeric_limits<double>::max(),
                           -std::numeric_limits<double>::max(), 5e-324, 0.30000000000000004}) {
        std::array<char, 40> buffer{};
        const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", x);
        REQUIRE(written > 0);
        CAPTURE(buffer.data());
        const auto parsed =
            parse_double(std::string_view(buffer.data(), static_cast<std::size_t>(written)));
        REQUIRE(parsed.has_value());
        CHECK(parsed->value == x);
    }
}

TEST_CASE("parse_double does not consult the process locale", "[base][number-parse]") {
    // A locale whose decimal point is a comma. Which of these a machine has is its own business;
    // none of them is a reason to fail, and a machine with none says so. Both implementations are
    // held to this: strtod alone would read "1.5" as 1 here.
    constexpr std::array<const char*, 6> kNames{"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8",
                                                "de-DE",       "fr-FR", "German_Germany.1252"};
    const char* chosen = nullptr;
    const std::string before = std::setlocale(LC_NUMERIC, nullptr);
    for (const char* name : kNames) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr) {
            chosen = name;
            break;
        }
    }
    if (chosen == nullptr) {
        SKIP("no locale with a decimal comma is installed here");
    }
    CAPTURE(chosen);
    const auto point = parse_double("1.5");
    const auto comma = parse_double("1,5");
    const auto exponent = parse_double("1234.5e-1");
    std::setlocale(LC_NUMERIC, before.c_str());
    REQUIRE(point.has_value());
    CHECK(point->value == 1.5);
    CHECK_FALSE(comma.has_value());
    REQUIRE(exponent.has_value());
    CHECK(exponent->value == 123.45);
}
