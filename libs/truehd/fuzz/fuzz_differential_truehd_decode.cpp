#include <cstddef>
#include <cstdint>
#include <span>

#include "differential_oracle.hpp"
#include "truehd_mutator.hpp"

// Differential mode for TrueHD and MLP: the stream fuzz_truehd_decode reads (this harness shares
// its seeds and its dictionary, see tools/fuzz/run.sh's seed_source_for), decoded by this library
// and then by FFmpeg's strict decoder, and the samples compared. The codec is lossless, so when
// both decode the input without a failed check the samples are equal, and a difference is a bug in
// one of them. libs/truehd/fuzz/differential_oracle.hpp has the rule, and says what is left out
// as not comparable (a stream either refuses, or FFmpeg warns of a failed lossless check, which it
// does not treat as an error).
//
// Unlike the AC-3 differential harnesses, this one has the repairing mutator
// (truehd_mutator.hpp). There the CRC repair would multiply the inputs both decoders accept and
// each of those costs an FFmpeg process; here, without it, a mutation almost never leaves a
// stream the strict library decode accepts (a unit's check nibble, major sync CRC, restart CRC,
// parity and CRC bytes, and the lossless check of each restart interval all have to hold), and
// the harness would spend its budget on inputs the first rule skips. A mutation in the audio of
// an interval that a restart header follows still fails the lossless check and is skipped; one in
// the last interval, which nothing checks, or in the fields that decode to the same samples, is
// what reaches FFmpeg.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    iclforge::truehd_fuzzdiff::run_differential(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), size});
    return 0;
}

extern "C" std::size_t LLVMFuzzerCustomMutator(std::uint8_t* data, std::size_t size,
                                               std::size_t max_size, unsigned int seed) {
    return iclforge_fuzz::truehd_repairing_mutate(data, size, max_size, seed);
}
