#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/mlp/decoder/decoder.hpp"
#include "iclforge/mlp/encoder/atmos.hpp"
#include "iclforge/mlp/io/elementary.hpp"
#include "iclforge/mlp/io/extra_data.hpp"

// iclforge::mlp's decode path on untrusted bytes (libs/mlp/src): scan() and probe() walk the
// access units, then two decoders read every unit scan() found, one at the richest presentation
// with every check on and one at the presentation the input's last byte picks with the checks
// off, so that a stream whose CRCs no longer hold still reaches the sample decode, the
// prediction filters and the matrices behind them. The whole input also goes through
// decode_stream() and decode_atmos(), which frame it themselves, and read_extra_data() alone.
// Every count, length and coefficient comes from the input; the harness holds the library to
// never reading outside it, never looping on a count with nothing behind it, and never tripping
// ASan or UBSan.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    namespace mlp = iclforge::mlp;

    const mlp::ScanResult walk = mlp::scan(bytes);
    (void)mlp::probe(bytes);

    mlp::Decoder checked;
    mlp::DecoderConfig loose_config;
    loose_config.verify_checks = false;
    loose_config.verify_lossless_check = false;
    if (size > 0) {
        static constexpr int kPresentations[] = {0, 2, 6, 8};
        loose_config.presentation_channels = kPresentations[data[size - 1] % 4U];
    }
    mlp::Decoder loose(loose_config);
    for (const mlp::AccessUnitInfo& unit : walk.units) {
        const auto au = bytes.subspan(unit.offset, unit.size);
        (void)checked.decode(au);
        (void)loose.decode(au);
    }
    (void)mlp::decode_stream(bytes);
    (void)mlp::decode_atmos(bytes);
    (void)mlp::read_extra_data(bytes);
    return 0;
}
