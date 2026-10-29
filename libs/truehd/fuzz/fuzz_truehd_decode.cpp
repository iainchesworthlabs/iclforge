// libFuzzer harness: the TrueHD / MLP decoder over arbitrary bytes, in every presentation, strict and
// lenient. The decoder has to return an error or samples, and stay usable after either.
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace iclforge::truehd;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);

    constexpr Presentation kPresentations[] = {Presentation::kBest, Presentation::k2ch, Presentation::k6ch,
                                               Presentation::k8ch, Presentation::kAll};
    for (const Presentation presentation : kPresentations) {
        for (const Strictness strictness : {Strictness::kStrict, Strictness::kLenient}) {
            DecoderConfig config;
            config.presentation = presentation;
            config.strictness = strictness;
            Decoder decoder(config);
            ElementaryReader reader(bytes);
            while (const auto unit = reader.next()) {
                (void)decoder.decode(*unit);
            }
        }
    }

    // The same bytes as one unit: the decoder's own framing checks.
    Decoder decoder;
    (void)decoder.decode(bytes);
    return 0;
}
