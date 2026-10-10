#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/io/metadata_edit.hpp"

// iclforge::ac3::io::scan is the first thing that touches a stream nobody has looked at
// yet: format-sniffing for AC-3 vs E-AC-3 vs garbage, called before any
// decoder commits to a layout (see libs/ac3/src/io/elementary.cpp). It must
// never crash or hang on arbitrary bytes, only return an error.
//
// The metadata edit reads the same bytes the same way and then WRITES: the
// insert rebuilds a syncframe bit by bit around a flag it located and moves
// frmsiz, so a header that lies about where a field sits is the input that
// matters. They run here, on the same seeds, because every one of them is a
// stream shape scan() already has to survive, and they share its entry point's
// first requirement - an error, never an out-of-range bit position.
//
// What to ask for is read from bytes of the stream itself, at fixed positions
// and without removing them, so the committed seeds (valid streams) reach the
// edit intact and the mutation engine still steers which fields are named.
// What comes back is held to the one contract that does not depend on the
// stream being valid: it must scan again, because an insert that produced a
// stream the framing refuses has corrupted what it was given.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    (void)iclforge::ac3::io::scan(bytes);

    if (size < 8) {
        return 0;
    }
    const auto pick = [&](std::size_t salt) {
        return data[(salt * 40503U + 7U) % size];
    };
    iclforge::ac3::io::MetadataEdit edit;
    const std::uint8_t choose = pick(1);
    if ((choose & 0x01U) != 0) {
        edit.compr = pick(2);
    }
    if ((choose & 0x02U) != 0) {
        edit.bsmod = pick(3) & 0x07;
    }
    if ((choose & 0x04U) != 0) {
        edit.dsurmod = pick(4) & 0x03;
    }
    if ((choose & 0x08U) != 0) {
        edit.dialnorm = 1 + (pick(5) % 31);
    }
    if ((choose & 0x10U) != 0) {
        edit.compr2 = pick(6);
    }
    if ((choose & 0x20U) != 0) {
        edit.dialnorm2 = 1 + (pick(7) % 31);
    }

    if (const auto grown = iclforge::ac3::io::insert_stream_metadata(bytes, edit)) {
        (void)iclforge::ac3::io::scan(grown->bytes);
        volatile std::byte sink{};
        for (const auto b : grown->bytes) {
            sink = b;
        }
        (void)sink;
    }

    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    (void)iclforge::ac3::io::edit_stream_metadata(copy, edit);
    return 0;
}
