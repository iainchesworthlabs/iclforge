#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "iclforge/containers/iec61937/mat.hpp"

// iclforge::containers::iec61937::unwrap_mat_stream and MatPacker
// (libs/containers/src/iec61937/mat.cpp).
//
// The carrier is, by definition, something that came off a wire: an HDMI capture or a file somebody
// saved from one. Everything the unwrap acts on is self-declared: the preamble that picks the word
// order, the three code words, and the 12-bit length at the head of each of the 24 slots, which is
// the number a naive parser would hand to a copy. The property the harness holds is that no
// declared length leaves its slot.
//
// Three things are driven. The unwrap itself. The units it returns, wrapped again: each was found
// in a slot, so it is a unit by the packer's own test, and if the second wrap succeeds its unwrap
// has to return the same units (a unit that fit slot k may be too big for the slot a repack gives
// it, which is a legitimate refusal, not a finding). And the packer fed the input as a single
// access unit, which it must either refuse or hold.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace mat = iclforge::containers::iec61937;
    const std::span<const std::byte> carrier{reinterpret_cast<const std::byte*>(data), size};

    const auto units = mat::unwrap_mat_stream(carrier);
    if (units) {
        std::vector<std::span<const std::byte>> views;
        views.reserve(units->size());
        for (const auto& unit : *units) {
            views.emplace_back(unit);
        }
        if (const auto again = mat::wrap_mat_stream(views)) {
            const auto back = mat::unwrap_mat_stream(*again);
            if (!back || *back != *units) {
                std::abort();
            }
        }
    }

    mat::MatPacker packer;
    if (packer.push(carrier) && packer.pending() != 1) {
        std::abort();
    }
    (void)packer.flush();
    return 0;
}
