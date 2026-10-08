#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>

// A syntax trace: one record per syntax element a reader reads or a writer writes, in one shape
// for both directions, so that a decoder's trace, an encoder's and an independent transcription's
// can be compared record for record. iclforge::BitReader and iclforge::BitWriter emit it when a
// sink is attached (bitreader.hpp, bitwriter.hpp); what counts as one element is the codec's to
// say (AC-4's: iclforge/ac4/core/syntax.hpp).

namespace iclforge::base {

struct SyntaxRecord {
    int substream = 0;             // the unit the offsets count from (AC-4: a substream's index)
    std::uint32_t bit_offset = 0;  // from the first bit of that unit
    std::uint16_t bits = 0;
    std::uint64_t value = 0;
    std::string_view name;         // the element's name in the syntax table
};

// Where a configuration sends its trace. It owns a copy of the callable, so a lambda written in
// place lives as long as the configuration, and the decoder or encoder built from it keeps a copy
// of its own. What the callable refers to (the vector a lambda captures by reference, say) must
// still outlive the decoder or encoder, as with any callable. Empty, the default, traces nothing,
// at the cost of one branch per syntax element.
using SyntaxTrace = std::function<void(const SyntaxRecord&)>;

// A non-owning reference to any callable taking a const SyntaxRecord&, in the shape of
// iclforge::ac3::BlockSink: what a reader or a writer holds while it runs. It refers to a named
// callable only; a temporary would be gone before the first record, so binding one does not
// compile.
class SyntaxSink {
   public:
    SyntaxSink() noexcept = default;

    template <typename F>
        requires std::is_lvalue_reference_v<F> && std::invocable<F, const SyntaxRecord&> &&
                     (!std::same_as<std::remove_cvref_t<F>, SyntaxSink>)
    // NOLINTNEXTLINE(google-explicit-constructor): the call site is the point
    SyntaxSink(F&& f) noexcept
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_([](void* object, const SyntaxRecord& record) {
              (*static_cast<std::remove_reference_t<F>*>(object))(record);
          }) {}

    // A temporary callable: name it first, or hand it to a SyntaxTrace, which keeps a copy.
    template <typename F>
        requires(!std::is_lvalue_reference_v<F>) &&
                    (!std::same_as<std::remove_cvref_t<F>, SyntaxSink>)
    SyntaxSink(F&&) = delete;

    explicit operator bool() const noexcept { return call_ != nullptr; }
    void operator()(const SyntaxRecord& record) const { call_(object_, record); }

   private:
    void* object_ = nullptr;
    void (*call_)(void*, const SyntaxRecord&) = nullptr;
};

// The reference a reader or writer holds for `trace` while it runs: null when `trace` is empty, so
// that nothing calls an empty std::function. It takes a SyntaxTrace itself, never one converted
// from another callable, which would be a temporary.
template <typename T>
    requires std::same_as<T, SyntaxTrace>
[[nodiscard]] SyntaxSink sink_of(const T& trace) noexcept {
    return trace ? SyntaxSink(trace) : SyntaxSink{};
}

}  // namespace iclforge::base
