// libFuzzer harness: the TrueHD / MLP readers over arbitrary bytes. Nothing it is given may crash it, read
// out of bounds or loop without end.
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/base/bitreader.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace iclforge::truehd;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);

    (void)probe_stream(bytes);
    (void)parse_major_sync(bytes, false);
    (void)parse_access_unit(bytes, 1, false);

    ElementaryReader reader(bytes);
    int substreams = 0;
    while (const auto unit = reader.next()) {
        if (const auto parsed = parse_access_unit(*unit, substreams, false)) {
            if (parsed->major_sync) {
                substreams = parsed->major_sync->num_substreams;
            }
            for (int i = 0; i < parsed->num_substreams; ++i) {
                iclforge::BitReader bits(parsed->segments[static_cast<std::size_t>(i)]);
                if (bits.read(1) != 0 && bits.read(1) != 0) {
                    (void)read_restart_header(bits, parsed->segments[static_cast<std::size_t>(i)], false);
                }
            }
        }
    }

    // The block header reader over the raw bytes, in a context the data picks.
    if (size >= 2) {
        BlockContext context;
        context.max_matrix_chan = std::to_integer<int>(bytes[0]) & 15;
        context.max_chan = std::to_integer<int>(bytes[1]) & 15;
        if (context.max_chan > context.max_matrix_chan) {
            context.max_chan = context.max_matrix_chan;
        }
        context.noise_type = (std::to_integer<int>(bytes[1]) & 0x80) != 0;
        iclforge::BitReader bits(bytes.subspan(2));
        (void)read_block_header(bits, context);
    }
    return 0;
}
