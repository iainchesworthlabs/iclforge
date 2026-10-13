#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

// NDK r26's libc++ (LLVM 17) puts std::expected's has-value flag in the tail padding of a
// trivially copyable payload and then copies the payload, padding included, over it (llvm-project
// #68552, #70494; android/ndk#1963; cmake/LibcxxExpectedLayout.cmake has the account). A
// std::expected<libs/ac3's FrameMetadata, EditError> came back as "no value, error 0" from a plain
// `return meta;`, on a Shield and nowhere else. Every toolchain this tree is built with elsewhere
// is past it, so this is the test that runs where it matters: iclforge-base-tests built for
// arm64-android (config-android-arm64) and run on a device.
//
// The payloads are built in storage whose every byte is set first, so what is in the padding is
// the test's choice and not whatever the stack held: 0x00 turns a clobbered "has value" into a
// refusal, 0xFF turns a clobbered "has error" into a value.

namespace {

// Trivially copyable, with three bytes of tail padding, and not a POD for the purpose of layout
// (the default member initialisers see to that): the shape the Itanium ABI lets an enclosing class
// put its own members into.
struct Padded {
    int a = 0;
    bool b = false;
};

// Padded the way libs/ac3's FrameMetadata is: through std::optional members, no initialiser to
// blame.
struct Optionals {
    std::optional<int> a;
    std::optional<std::uint8_t> b;
    bool c = false;
};

struct PaddedError {
    int code = 0;
    bool fatal = false;
};

enum class Small : std::uint8_t { kA, kB };

static_assert(std::is_trivially_copyable_v<Padded> && std::is_trivially_copyable_v<Optionals> &&
                  std::is_trivially_copyable_v<PaddedError>,
              "the case is a trivially copyable payload");
static_assert(sizeof(Padded) > sizeof(int) + sizeof(bool), "Padded has tail padding");
static_assert(sizeof(PaddedError) > sizeof(int) + sizeof(bool), "PaddedError has tail padding");

// What the padding holds. Zero reads as "no value" if it lands on the flag; anything else, as a
// value.
constexpr std::array<unsigned char, 2> kFills{0x00, 0xFF};

// Calls body(T&) with a T constructed in storage that was filled with `fill` first. The members are
// default-initialised, not value-initialised: that would zero the padding the test wants set.
template <class T, class Body>
void with_padding(unsigned char fill, Body&& body) {
    alignas(T) unsigned char raw[sizeof(T)];
    std::memset(raw, fill, sizeof raw);
    body(*::new (static_cast<void*>(raw)) T);
}

// The shape of every `return meta;` in the library: a T becomes the expected's value by conversion.
template <class T, class E>
std::expected<T, E> returned(const T& value) {
    return value;
}

template <class T, class E>
std::expected<T, E> failed(const E& error) {
    return std::unexpected(error);
}

}  // namespace

TEST_CASE("std::expected keeps a padded payload's value", "[base][expected][toolchain]") {
    for (const unsigned char fill : kFills) {
        with_padding<Padded>(fill, [](Padded& source) {
            source.a = 7;
            source.b = true;

            const std::expected<Padded, Small> by_return = returned<Padded, Small>(source);
            REQUIRE(by_return.has_value());
            CHECK(by_return->a == 7);
            CHECK(by_return->b);

            const std::expected<Padded, Small> by_copy = by_return;
            REQUIRE(by_copy.has_value());
            CHECK(by_copy->a == 7);

            std::expected<Padded, Small> assigned = std::unexpected(Small::kB);
            assigned = source;
            REQUIRE(assigned.has_value());
            CHECK(assigned->a == 7);

            std::expected<Padded, Small> moved(std::move(assigned));
            REQUIRE(moved.has_value());
            CHECK(moved->b);
        });
    }
}

TEST_CASE("std::expected keeps a payload made of optionals", "[base][expected][toolchain]") {
    for (const unsigned char fill : kFills) {
        with_padding<Optionals>(fill, [](Optionals& source) {
            source.a = 3;
            source.b = std::uint8_t{9};
            source.c = true;

            const std::expected<Optionals, Small> result = returned<Optionals, Small>(source);
            REQUIRE(result.has_value());
            CHECK(result->a == 3);
            CHECK(result->b == std::uint8_t{9});
            CHECK(result->c);
        });
    }
}

TEST_CASE("std::expected keeps a padded error an error", "[base][expected][toolchain]") {
    for (const unsigned char fill : kFills) {
        with_padding<PaddedError>(fill, [](PaddedError& source) {
            source.code = 5;
            source.fatal = true;

            const std::expected<int, PaddedError> result = failed<int, PaddedError>(source);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == 5);
            CHECK(result.error().fatal);

            const std::expected<void, PaddedError> none = std::unexpected(source);
            REQUIRE_FALSE(none.has_value());
            CHECK(none.error().code == 5);
        });
    }
}

TEST_CASE("std::expected emplace and swap leave the flag alone", "[base][expected][toolchain]") {
    for (const unsigned char fill : kFills) {
        with_padding<Padded>(fill, [](Padded& source) {
            source.a = 11;
            source.b = true;

            std::expected<Padded, Small> value = source;
            std::expected<Padded, Small> error = std::unexpected(Small::kA);

            value.swap(error);
            REQUIRE_FALSE(value.has_value());
            CHECK(value.error() == Small::kA);
            REQUIRE(error.has_value());
            CHECK(error->a == 11);

            value.emplace(source);
            REQUIRE(value.has_value());
            CHECK(value->a == 11);
        });
    }
}
