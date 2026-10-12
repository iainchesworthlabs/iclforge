// The AC-4 decoder of a build that has none (CONFIG_ICLFORGE_AC4 off). See ac4_stream.hpp.

#include "ac4_stream.hpp"

namespace iclforge {

struct Ac4Stream::Impl {
    Report report;
};

Ac4Stream::Ac4Stream() : impl_(std::make_unique<Impl>()) {}

Ac4Stream::~Ac4Stream() = default;

bool Ac4Stream::available() {
    return false;
}

std::size_t Ac4Stream::heap_floor_bytes() {
    return 0;
}

void Ac4Stream::begin_stream() {}

void Ac4Stream::end_stream() {}

bool Ac4Stream::open(const Config&) {
    return false;
}

bool Ac4Stream::is_open() const {
    return false;
}

void Ac4Stream::reset() {}

void Ac4Stream::close() {}

Ac4Stream::Result Ac4Stream::decode(std::span<const std::byte>, BlockFn, void*) {
    return Result::kInvalid;
}

std::string_view Ac4Stream::reason() const {
    return "this build has no AC-4 decoder";
}

std::size_t Ac4Stream::frame_samples() const {
    return 0;
}

const Ac4Stream::Report& Ac4Stream::report() const {
    return impl_->report;
}

}  // namespace iclforge
