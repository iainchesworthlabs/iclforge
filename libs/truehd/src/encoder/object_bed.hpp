#pragma once

#include <cstdint>
#include <vector>

#include "iclforge/truehd/core/syntax.hpp"

// The matrices of an object substream written the way the owner's streams have it (ERRATA.md
// entry 69, 70): substream 3 codes the channels from 8 up and its primitive matrices rebuild the
// output channels from the 7.1 bed rows of substreams 0 to 2 and those coded channels. The encoder
// runs the matrices backwards over the object signals it is given, in integer arithmetic that
// includes the matrices' noise and LSB bypass, and so finds the rows it has to code. Everything
// here is a pure function of its arguments.

namespace iclforge::truehd::detail {

using ObjectPlane = std::vector<std::int32_t>;

inline constexpr int kBedChannels = 8;  // rows 0 to 7 come from substreams 0 to 2

// The restart header's noise seed, which every restart of the object substream reloads (the owner's
// streams restart every 128 access units).
inline constexpr std::uint32_t kObjectNoiseSeed = 0x2A5F31;

// How many gain steps make_object_bed_chain knows: the mix of the objects above the bed into the
// bed at full gain, half, a quarter and none.
inline constexpr int kObjectMixSteps = 4;

// The matrices, in the order the decoder applies them, for `objects` (9 to 16) output channels:
//   1. out 8, frac_bits 0, f_shift -1: -1.0 on itself (the sparse matrix of the owner's streams);
//   2. with `lsb_bypass`, out objects - 1: 2.0 on itself (frac_bits 12, f_shift 1) and the matrix's
//      LSB bypass, which sends the bit the doubling loses;
//   3. for each object above the bed (8 and up) a matrix on one bed row: 1.0 on itself and the
//      negative of the object's gain on the object, so that the bed row is the object channel plus
//      the mixed-in objects. Which bed row an object goes to is a fixed table.
// Every matrix of 2 and 3 carries `noise_shift` (0 to 15; 0 draws no noise). The gain of 3 is
// 1/sqrt(2) halved `mix_step` times, none at the last step.
[[nodiscard]] std::vector<MatrixSpec> make_object_bed_chain(int objects, int mix_step,
                                                            int noise_shift, bool lsb_bypass);

// What running a chain over a block of rows needs besides the chain: the quantiser step the rows
// share, the restart header's noise seed, and the shape of the interval.
struct ObjectChainShape {
    int quant = 0;
    std::uint32_t noise_seed = kObjectNoiseSeed;
    int units = 0;        // access units in the interval
    int nominal = 0;      // samples to an access unit
    int noise_size = 64;  // entries of generator 1's buffer (core/noise.hpp)
};

// Runs the chain backwards: on entry `rows` are the output channels (in units of 2^quant, all
// multiples of it); on return they are the rows the decoder starts from, the bed rows 0 to 7 and
// the coded channels. `bypass[k]` is, for the k-th matrix with LSB bypass, the bit sent for each
// sample. False when the chain cannot be run (a matrix that is not primitive, a bypass matrix with
// a quantiser step).
[[nodiscard]] bool invert_object_chain(const std::vector<MatrixSpec>& chain,
                                       const ObjectChainShape& shape,
                                       std::vector<ObjectPlane>& rows,
                                       std::vector<std::vector<std::uint8_t>>& bypass);

// The decoder's side of the chain, applied to `rows` in place: the check that invert_object_chain
// gave the rows back.
void apply_object_chain(const std::vector<MatrixSpec>& chain, const ObjectChainShape& shape,
                        const std::vector<std::vector<std::uint8_t>>& bypass,
                        std::vector<ObjectPlane>& rows);

}  // namespace iclforge::truehd::detail
