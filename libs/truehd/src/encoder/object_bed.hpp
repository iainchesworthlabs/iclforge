#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>
#include <vector>

#include "decoder/kernels.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/encoder/object_paths.hpp"

// The matrices of an object substream written the way the owner's streams have it (ERRATA.md
// entry 69, 70): substream 3 codes the channels from 8 up and its primitive matrices rebuild the
// output channels from the 7.1 bed rows of substreams 0 to 2 and those coded channels. The encoder
// runs the matrices backwards over the object signals it is given, in integer arithmetic that
// includes the matrices' noise, LSB bypass and slopes, and so finds the rows it has to code.
//
// The matrices change from block to block. A *timeline* holds, for each block of a restart
// interval, the matrices in force, the decoder's running state for them at the block's first
// sample (the coefficients move with the slopes, SUBSTREAM3.md section 3), and the section that
// block's header sends to get there (nothing, new coefficients, new slopes, or new headers); the
// same timeline drives the backward run, the forward check and the block data's bypass bits.
// Everything here is a pure function of its arguments. Agreement with this library's decoder is not
// agreement with Dolby's: the sections are read back by this library's reader and the arithmetic
// mirrors this library's kernels.

namespace iclforge::truehd::detail {

using ObjectPlane = std::vector<std::int32_t>;

inline constexpr int kBedChannels = 8;  // rows 0 to 7 come from substreams 0 to 2

// The restart header's noise seed, which every restart of the object substream reloads (the owner's
// streams restart every 128 access units).
inline constexpr std::uint32_t kObjectNoiseSeed = 0x2A5F31;

// How many gain steps the mix of the objects into the bed knows: full gain, half, a quarter and
// none (the encoder takes the first whose bed stays within 24 bits).
inline constexpr int kObjectMixSteps = 4;

// The gain of an object with no path, into its own bed row: 1/sqrt(2), -3 dB, in units of
// 2^-kObjectGainBits.
inline constexpr std::int32_t kDefaultMixGain = 46341;

// The longest stretch of samples between two knots of a path (keeps the integer arithmetic of the
// trajectory in 64 bits).
inline constexpr std::int64_t kMaxKnotSpan = std::int64_t{1} << 26;

// The slopes' scale the encoder writes (ext_field2: a delta is worth 2^-3 raw places a unit, the
// finest the 2 bits give), the widest delta (width 15 is 16 bits, signed), and the tolerances of
// the generator in units of 2^-18 of a coefficient: a coefficient further than the jump tolerance
// from its path is sent again (half a raw place at 12 fractional bits), a slope whose unit end is
// further than the slope tolerance from the path's is replaced, and a move of more than the step
// limit in a unit (a quarter of a gain of 1.0) is a step of the path, not a slope.
inline constexpr int kSlopeScaleField = 3;
inline constexpr int kMaxDeltaWidth = 15;
inline constexpr std::int32_t kJumpTolerance = 32;
inline constexpr std::int32_t kSlopeTolerance = 16;
inline constexpr std::int32_t kStepLimit = 1 << 16;

// The bed row (L R C LFE Ls Rs Lrs Rrs) each object above the bed is mixed into when it has no
// path: object 8 into L, 9 into R, 10 into C, and so on round the surrounds, the LFE last. A
// placeholder for a renderer (ERRATA.md entry 70): the encoder makes a valid bed, not Dolby's.
inline constexpr int kBedRowOfObject[kBedChannels] = {0, 1, 2, 4, 5, 6, 7, 3};

// --- arithmetic shared with the Python reference (tools/references/truehd_writer.py) -----------

// numerator / denominator rounded to the nearest whole number, halves away from zero (what lround
// does); denominator > 0.
[[nodiscard]] constexpr std::int64_t round_div(std::int64_t numerator,
                                               std::int64_t denominator) noexcept {
    const std::int64_t magnitude = numerator < 0 ? -numerator : numerator;
    const std::int64_t quotient = (2 * magnitude + denominator) / (2 * denominator);
    return numerator < 0 ? -quotient : quotient;
}

// A path's gain into one bed row at one sample, as the fraction `numerator / denominator` in units
// of 2^-kObjectGainBits (the knots' units). Between knots it is linear; outside them it holds; at a
// knot's own sample the later knot of that sample holds.
struct GainFraction {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
};
[[nodiscard]] GainFraction path_gain(const ObjectPath& path, int row, std::int64_t sample) noexcept;

// --- the plan -----------------------------------------------------------------------------------

// The decoder's running state of one object matrix (kernels.hpp): the coefficients at 2^-18, the
// slopes as sent, the ramp inside the access unit, the scale of the slopes and whether the
// extension is on.
using ObjectRun = kernels::ObjectMatrixState;

// One block of an interval: where it is, the matrices in force while it plays (headers, and the
// widths and scales of the slopes in `ext_width` and `ext_field2`), the decoder's state for each
// at the block's first sample after the section, and the section the block's header sends, if any.
struct ObjectBlock {
    std::size_t start = 0;  // first sample, counted from the interval's
    int size = 0;
    std::vector<MatrixSpec> matrices;
    std::vector<ObjectRun> run;
    bool send = false;  // the header carries `section`
    MatrixSet section;
};

struct ObjectTimeline {
    std::vector<ObjectBlock> blocks;
};

// What the encoder asks of the plan.
struct ObjectTimelineRequest {
    int objects = 12;     // channels of substream 3, 9 to 16
    int noise_shift = 0;  // every matrix but the sign flip
    int bypass_bits = 0;  // 0: no LSB bypass matrix; 1 to 3: its bits a sample
    int mix_step = 0;     // 0 to kObjectMixSteps - 1: the gains halved this many times, none at 3
    std::vector<ObjectPath> paths;
    ObjectExtension extension = ObjectExtension::kNone;
    int update_units = 1;  // updates every this many access units (without slopes)
    bool drop_idle = false;
    bool split_mixes = false;  // one mixing matrix for each object a bed row takes, not one a row
    std::int64_t first_sample = 0;  // the stream's sample the interval starts at
    int nominal = 40;               // samples to an access unit
    // The interval's blocks: first sample (from the interval's) and size.
    std::vector<std::pair<std::size_t, int>> blocks;
};

// The timeline of the interval, or why the request cannot be planned (in words).
[[nodiscard]] std::expected<ObjectTimeline, std::string_view> build_object_timeline(
    const ObjectTimelineRequest& request);

// The words for a path that is not valid, empty when it is (objects in 8..objects-1, knots in
// ascending order within kMaxKnotSpan of each other, gains within +-1.98).
[[nodiscard]] std::string_view object_path_problem(const ObjectPath& path, int objects);

// --- sections -----------------------------------------------------------------------------------

// What the decoder holds for the object matrices: the headers of the list in force and each
// matrix's running state.
struct ObjectState {
    std::vector<MatrixSpec> matrices;
    std::vector<ObjectRun> run;
};

// What the decoder holds after a section, from what it held before (empty after a restart): this
// library's decoder, which the section's forms drive (SUBSTREAM3.md section 2): A sets every
// coefficient from the raw values (clear, they run on), B sends new headers (the widths and
// scales of the matrix at the same place stand), A2 turns the extension on (clear, every slope is
// cleared), B2 replaces the slopes (clear, they stand, also across new headers), C2 replaces the
// widths and scales.
[[nodiscard]] ObjectState apply_object_section(const ObjectState& before, const MatrixSet& section);

// --- running the matrices -----------------------------------------------------------------------

// What running the matrices needs besides them: the quantiser step the rows share, the restart
// header's noise seed, and the shape of the interval.
struct ObjectChainShape {
    int quant = 0;
    std::uint32_t noise_seed = kObjectNoiseSeed;
    int units = 0;        // access units in the interval
    int nominal = 0;      // samples to an access unit
    int noise_size = 64;  // entries of generator 1's buffer (core/noise.hpp)
};

// The bits the LSB bypass matrices send: `bits[k][t]` is the value (0 to 2^n - 1) for the k-th
// matrix with bypass in the list in force at sample t.
using ObjectBypass = std::vector<std::vector<std::uint8_t>>;

// Runs the matrices backwards: on entry `rows` are the output channels (in units of 2^quant, all
// multiples of it); on return they are the rows the decoder starts from, the bed rows 0 to 7 and
// the coded channels. False when the matrices cannot be run (one that is not primitive, a bypass
// matrix with a quantiser step).
[[nodiscard]] bool invert_object_timeline(const ObjectTimeline& timeline,
                                          const ObjectChainShape& shape,
                                          std::vector<ObjectPlane>& rows, ObjectBypass& bypass);

// The decoder's side, applied to `rows` in place: the check that invert_object_timeline gave the
// rows back.
void apply_object_timeline(const ObjectTimeline& timeline, const ObjectChainShape& shape,
                           const ObjectBypass& bypass, std::vector<ObjectPlane>& rows);

// The same for one chain of matrices with fixed coefficients (no slopes) over the whole of `rows`.
[[nodiscard]] bool invert_object_chain(const std::vector<MatrixSpec>& chain,
                                       const ObjectChainShape& shape,
                                       std::vector<ObjectPlane>& rows, ObjectBypass& bypass);
void apply_object_chain(const std::vector<MatrixSpec>& chain, const ObjectChainShape& shape,
                        const ObjectBypass& bypass, std::vector<ObjectPlane>& rows);

// The bypass bits of sample `i` of block `block`, in the order of the matrices that have them
// (first bit of each value first), as the block data carries them ahead of the residuals.
void put_object_bypass(BitWriter& writer, const ObjectTimeline& timeline,
                       const ObjectBypass& bypass, std::size_t block, int i);

// The most matrices any block's list holds.
[[nodiscard]] std::size_t object_matrices_used(const ObjectTimeline& timeline) noexcept;

}  // namespace iclforge::truehd::detail
