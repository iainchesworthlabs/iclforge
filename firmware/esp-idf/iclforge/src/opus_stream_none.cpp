#include "opus_stream.hpp"

// OpusStream in a build without CONFIG_ICLFORGE_SENDSPIN_OPUS: nothing decodes, so the player lists
// no Opus format and refuses a stream of it (opus_stream.hpp).

namespace iclforge {

struct OpusStream::Impl {};

OpusStream::OpusStream() = default;
OpusStream::~OpusStream() = default;

bool OpusStream::available() {
    return false;
}

bool OpusStream::open(std::int32_t /*sample_rate*/, std::int32_t /*channels*/) {
    return false;
}

void OpusStream::reset() {}

int OpusStream::decode(std::span<const std::uint8_t> /*packet*/, std::span<std::int16_t> /*out*/) {
    return -1;
}

}  // namespace iclforge
