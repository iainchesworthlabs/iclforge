#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/containers/iamf/container.hpp"
#include "iclforge/containers/iamf/iamf.hpp"
#include "iclforge/containers/iamf/sequence.hpp"

// iclforge::containers::iamf::read_sequence (the standalone raw OBU stream) and read_isobmff (the
// ISO-BMFF encapsulation, whole files and movie fragments) on the same bytes, then the paths that
// consume what they return: writing the Sequence back out and decoding an Audio Element's PCM
// (libs/containers/src/iamf/sequence_read.cpp, container.cpp, iamf.cpp).
//
// Both readers exist to read files this project did not write, and both size almost everything from
// numbers the file chose: leb128 OBU sizes, the counts of substreams, parameters, layouts, labels
// and sub blocks, the Parameter Blocks' durations, and the box sizes, sample tables and trun
// entries of the ISO-BMFF walk. Two framings of one input share a corpus better than two harnesses
// would.
//
// Writing a Sequence back checks what the reader produced is something the writer accepts or
// refuses cleanly, and decode_pcm reaches the sample reader with sizes and trims the file chose.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace iamf = iclforge::containers::iamf;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);

    if (auto sequence = iamf::read_sequence(bytes); sequence.has_value()) {
        (void)iamf::write_sequence(*sequence);
        for (const auto& element : sequence->audio_elements) {
            (void)iamf::decode_pcm(*sequence, element.audio_element_id);
        }
    }
    if (auto file = iamf::read_isobmff(bytes); file.has_value()) {
        (void)iamf::write_isobmff(file->sequence);
        for (const auto& element : file->sequence.audio_elements) {
            (void)iamf::decode_pcm(file->sequence, element.audio_element_id);
        }
    }
    return 0;
}
