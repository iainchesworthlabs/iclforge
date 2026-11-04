// The TrueHD decoder of a build that has none (CONFIG_ICLFORGE_TRUEHD off). See truehd_stream.hpp.

#include "truehd_stream.hpp"

namespace iclforge {

struct TruehdStream::Impl {
    Report report;
};

TruehdStream::TruehdStream() : impl_(std::make_unique<Impl>()) {}

TruehdStream::~TruehdStream() = default;

bool TruehdStream::available() {
    return false;
}

std::size_t TruehdStream::heap_floor_bytes() {
    return 0;
}

std::size_t TruehdStream::stack_bytes_needed() {
    return 0;
}

std::size_t TruehdStream::chunk_samples(std::span<const std::byte>, std::uint32_t) {
    return 0;
}

bool TruehdStream::open(const Config&) {
    return false;
}

bool TruehdStream::is_open() const {
    return false;
}

void TruehdStream::reset() {}

void TruehdStream::close() {}

TruehdStream::Result TruehdStream::decode(std::span<const std::byte>, BlockFn, void*) {
    return Result::kInvalid;
}

std::string_view TruehdStream::reason() const {
    return "this build has no TrueHD decoder";
}

std::size_t TruehdStream::unit_samples() const {
    return 0;
}

const TruehdStream::Report& TruehdStream::report() const {
    return impl_->report;
}

}  // namespace iclforge
