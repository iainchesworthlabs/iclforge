#pragma once

#include <array>
#include <cstdint>
#include <vector>

// Where the objects of a TrueHD Atmos stream sit in the 7.1 bed over time, for the encoder's
// ObjectLayout::kBedMatrices (libs/truehd/ERRATA.md entry 70, libs/truehd/notes/
// feature-truehd-atmos-encoder-matrices.md). The matrices that mix an object into the bed take
// their coefficients from these paths, and the encoder sends them to the decoder as the matrix
// sections of substream 3 change from block to block.

namespace iclforge::truehd {

// Gains are whole numbers in units of 2^-kObjectGainBits, so that a path means the same to every
// implementation (the Python reference computes the same coefficients from the same integers).
inline constexpr int kObjectGainBits = 16;
inline constexpr int kObjectBedRows = 8;  // L R C LFE Ls Rs Lrs Rrs

// The gain of one object into each bed row at one sample of the stream (counted from its first).
struct ObjectGainKnot {
    std::int64_t sample = 0;
    std::array<std::int32_t, kObjectBedRows> gain{};
};

// One object's gain into the bed rows. `object` is its channel in the PCM given to the encoder and
// in the stream, 8 up. Between two knots the gain moves in a straight line; before the first and
// after the last it holds. Two knots at one sample make a jump (the later one holds from there).
// Knots are in ascending order of sample. An object with no path is mixed into its own bed row at
// -3 dB, the encoder's fixed render.
struct ObjectPath {
    int object = 8;
    std::vector<ObjectGainKnot> knots;
};

// What the extension of the matrix sections does (SUBSTREAM3.md section 6, provisional): `kNone`
// sends no deltas, so a coefficient holds from one update to the next. With a ramp, each block's
// section sends a delta for every coefficient of a matrix that moves, and the coefficient goes
// from its raw value to raw + delta / 8 (in the last places of the raw value) across the block,
// reaching the end at the block's last sample (`kEnd`, the value at sample i of the block is
// raw + delta / 8 * (i + 1) / size) or one sample short of the block's end (`kStart`, i / size).
// Only the decoder's lab settings read a ramp (DecoderLab::object_extension 4 and 5 with
// object_extension_fixed 19); the decoder's rule is something else, and no outside decoder is
// known to read either.
enum class ObjectRamp : std::uint8_t {
    kNone,
    kEnd,
    kStart,
};

}  // namespace iclforge::truehd
