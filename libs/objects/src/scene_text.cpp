#include "scene_text.hpp"

#include <string_view>

#include "iclforge/base/number_parse.hpp"

namespace iclforge::objects::oba {

bool read_double(std::string_view token, double& out) {
    // The two ways out of range are not the same thing. Overflow is a genuine
    // refusal ("1e999" is not a number this can carry) and parse_double
    // returns nothing for it. Underflow hands back a subnormal or zero, which
    // IS the correctly rounded value - refusing it would make
    // fmt::format("{}", ...) unable to find any spelling of a subnormal that
    // this accepts back, and the two have to agree for a round trip to hold
    // at all - so `underflowed` is not looked at. A leading '+' is the one
    // thing beyond std::from_chars' syntax, as this file's header says.
    const auto parsed =
        iclforge::base::parse_double(token, iclforge::base::NumberSyntax{.leading_plus = true});
    if (!parsed) {
        return false;
    }
    out = parsed->value;
    return true;
}

}  // namespace iclforge::objects::oba
