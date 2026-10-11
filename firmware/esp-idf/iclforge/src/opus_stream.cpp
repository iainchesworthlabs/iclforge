#include "opus_stream.hpp"

#include <algorithm>

#include <opus.h>

#include "esp_heap_caps.h"

// libopus's fixed-point decoder behind OpusStream (opus.cmake builds it). The decoder's state is
// one block from the heap, which opus_decoder_get_size() sizes for the channel count.

namespace iclforge {

struct OpusStream::Impl {
    OpusDecoder* decoder = nullptr;
    std::int32_t channels = 0;
};

OpusStream::OpusStream() : impl_(std::make_unique<Impl>()) {}

OpusStream::~OpusStream() {
    if (impl_->decoder != nullptr) {
        heap_caps_free(impl_->decoder);
    }
}

bool OpusStream::available() {
    return true;
}

bool OpusStream::open(std::int32_t sample_rate, std::int32_t channels) {
    if (impl_->decoder != nullptr || channels < 1 || channels > 2) {
        return false;
    }
    const int size = opus_decoder_get_size(channels);
    if (size <= 0) {
        return false;
    }
    // Internal RAM first: the decoder reads its state all through a packet, and where there is
    // PSRAM it is the slower of the two.
    void* state =
        heap_caps_malloc(static_cast<std::size_t>(size), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (state == nullptr) {
        state = heap_caps_malloc(static_cast<std::size_t>(size), MALLOC_CAP_8BIT);
    }
    if (state == nullptr) {
        return false;
    }
    auto* decoder = static_cast<OpusDecoder*>(state);
    if (opus_decoder_init(decoder, sample_rate, channels) != OPUS_OK) {
        heap_caps_free(state);
        return false;
    }
    impl_->decoder = decoder;
    impl_->channels = channels;
    return true;
}

void OpusStream::reset() {
    if (impl_->decoder != nullptr) {
        (void)opus_decoder_ctl(impl_->decoder, OPUS_RESET_STATE);
    }
}

int OpusStream::decode(std::span<const std::uint8_t> packet, std::span<std::int16_t> out) {
    if (impl_->decoder == nullptr || packet.empty()) {
        return -1;
    }
    const std::size_t frames =
        std::min(kMaxFrames, out.size() / static_cast<std::size_t>(impl_->channels));
    return opus_decode(impl_->decoder, packet.data(), static_cast<opus_int32>(packet.size()),
                       out.data(), static_cast<int>(frames), 0);
}

}  // namespace iclforge
