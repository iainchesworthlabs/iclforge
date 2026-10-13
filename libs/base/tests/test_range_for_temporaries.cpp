#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <utility>

// What a range-for does with the temporaries of its range expression, on the toolchains this tree
// is built with.
//
// `for (x : f().member)` binds `auto&& __range = f().member;`. The language extends the lifetime of
// the temporary that `f()` returns for as long as that reference lives, when the access is a plain
// non-reference data member reached through `.` (class.temporary), so the loop is sound on every
// compiler. `for (x : f()->member)` is not: `->` is optional's or unique_ptr's operator->, a call,
// and so is `f().get()`, `f().value()` and `*f()`. Every temporary in such an expression ends with
// the range initialiser before the first iteration, until P2718R0 (C++23: GCC 15, Clang 19, and
// __cpp_range_based_for >= 202211L). NDK r26's Clang 17 is the compiler here that is before it:
// `iamf::layout_info(layout)->substreams` iterated a freed vector on a Shield (#1514), while
// `ac4::scan(bytes).frames` and the other plain-member loops in this tree had never been wrong:
// the two cases below, built for the Shield with r26 and with r28 (Clang 19) at -O0 and -O2.
//
// So a loop over a plain member of a returned object needs no local; a loop over a call on one does,
// on anything before the rule, and nothing warns: Clang 17's -Wdangling-gsl caught `.value().m` and
// `(*opt).m` in a probe, not `->` or a member function.
//
// Each case counts the Holders destroyed before the body of the loop first runs. The call shapes
// iterate storage the Holder only refers to, so a temporary that is gone is counted and never read.

namespace {

constexpr std::array<int, 3> kElsewhere{1, 2, 3};

// What the compiler says about the rule. Not an #if: nothing under tests/ has one.
constexpr bool kKeepsRangeTemporaries = __cpp_range_based_for >= 202211L;

int destroyed = 0;

struct Holder {
    std::array<int, 3> items{1, 2, 3};
    const std::array<int, 3>& elsewhere = kElsewhere;

    Holder() = default;
    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;
    ~Holder() { ++destroyed; }

    [[nodiscard]] const std::array<int, 3>& get() const { return elsewhere; }
};

struct Wrapper {
    Holder inner;
};

Holder make() { return Holder{}; }
Wrapper make_wrapper() { return Wrapper{}; }
std::optional<Holder> make_optional() { return std::optional<Holder>{std::in_place}; }
std::unique_ptr<Holder> make_owned() { return std::make_unique<Holder>(); }

// How many Holders were destroyed when `for (value : RANGE)` ran its body the first time: 0 when the
// temporary lived into the loop, 1 when the range initialiser ended it. -1 for an empty range.
// The loop runs to its end rather than leaving on the first iteration: MSVC at /Od reports the
// code after such a loop as unreachable (C4702), and the tree builds with /WX.
#define ICLFORGE_DESTROYED_BEFORE_FIRST_ITERATION(RANGE) \
    ([] {                                                \
        const int before = destroyed;                    \
        int first = -1;                                  \
        for (const int value : RANGE) {                  \
            static_cast<void>(value);                    \
            if (first < 0) {                             \
                first = destroyed - before;              \
            }                                            \
        }                                                \
        return first;                                    \
    }())

}  // namespace

TEST_CASE("a range-for keeps the returned object when it iterates a plain data member of it",
          "[base][range-for][toolchain]") {
    const int direct = ICLFORGE_DESTROYED_BEFORE_FIRST_ITERATION(make().items);
    const int nested = ICLFORGE_DESTROYED_BEFORE_FIRST_ITERATION(make_wrapper().inner.items);
    CHECK(direct == 0);
    CHECK(nested == 0);
}

TEST_CASE("a range-for over a call on the returned object keeps it from P2718R0 on",
          "[base][range-for][toolchain]") {
    const int expected = kKeepsRangeTemporaries ? 0 : 1;

    const int by_method = ICLFORGE_DESTROYED_BEFORE_FIRST_ITERATION(make().get());
    const int through_optional =
        ICLFORGE_DESTROYED_BEFORE_FIRST_ITERATION(make_optional()->elsewhere);
    const int through_pointer = ICLFORGE_DESTROYED_BEFORE_FIRST_ITERATION(make_owned()->elsewhere);
    CHECK(by_method == expected);
    CHECK(through_optional == expected);
    CHECK(through_pointer == expected);
}
