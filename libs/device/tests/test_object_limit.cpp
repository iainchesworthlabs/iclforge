#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

#include "iclforge/object_limit.hpp"

// The rule the Sendspin player decides a stream's bed fallback by (iclforge/object_limit.hpp).

namespace {

using iclforge::declared_objects;
using iclforge::objects_past_limit;

}  // namespace

TEST_CASE("object limit: a stream declares its complexity index less the LFE",
          "[device][objects]") {
    CHECK(declared_objects(std::nullopt) == 0);
    CHECK(declared_objects(0) == 0);
    CHECK(declared_objects(1) == 0);
    CHECK(declared_objects(5) == 4);
    CHECK(declared_objects(16) == 15);
}

TEST_CASE("object limit: a stream is past a stated figure when it declares more than it",
          "[device][objects]") {
    // Nothing stated: the board places what it is sent.
    CHECK_FALSE(objects_past_limit(16, std::nullopt));
    // A channel-based stream has no objects, so no figure holds it.
    CHECK_FALSE(objects_past_limit(std::nullopt, std::uint8_t{0}));
    // 0 is a statement: any object past none is past it.
    CHECK_FALSE(objects_past_limit(1, std::uint8_t{0}));
    CHECK(objects_past_limit(2, std::uint8_t{0}));
    // The S3's 2, the P4's 12: on the figure is within it.
    CHECK_FALSE(objects_past_limit(3, std::uint8_t{2}));
    CHECK(objects_past_limit(4, std::uint8_t{2}));
    CHECK_FALSE(objects_past_limit(13, std::uint8_t{12}));
    CHECK(objects_past_limit(14, std::uint8_t{12}));
}
