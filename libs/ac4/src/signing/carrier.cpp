#include "iclforge/ac4/signing/carrier.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/io/elementary.hpp"

namespace iclforge::ac4::signing {

std::string_view describe(CarrierErrorKind kind) {
    switch (kind) {
        case CarrierErrorKind::kLostSync:
            return "the stream has bytes that are not an AC-4 sync frame";
        case CarrierErrorKind::kTruncatedFrame:
            return "the stream ends inside a frame";
        case CarrierErrorKind::kContainerUnreadable:
            return "the container cannot be read";
        case CarrierErrorKind::kNoAc4Track:
            return "the container has no AC-4 track";
        case CarrierErrorKind::kAmbiguousSignature:
            return "the stream carries more than one content signature";
    }
    return "unknown carrier error";
}

std::expected<CarriedContent, CarrierError> ElementaryCarrier::read(
    std::span<const std::byte> stream, std::span<const std::byte> companion) const {
    const ScanResult scanned = scan(stream);
    if (scanned.stopped_at.has_value()) {
        // The walk stopped short of the end. Frames before the stop parsed, and a verdict on
        // them alone would pass a stream whose tail nobody has authenticated.
        const bool truncated = *scanned.stopped_at == Error::kTruncated;
        return std::unexpected(CarrierError{
            .kind = truncated ? CarrierErrorKind::kTruncatedFrame : CarrierErrorKind::kLostSync,
            .offset = scanned.stopped_at_offset,
            .detail = std::string{truncated ? "the stream ends inside the frame at byte "
                                            : "the stream loses sync at byte "} +
                      std::to_string(scanned.stopped_at_offset) + " (after " +
                      std::to_string(scanned.frames.size()) + " whole frames)",
        });
    }

    CarriedContent out;
    out.frames.reserve(scanned.frames.size());
    for (const SyncFrame& frame : scanned.frames) {
        out.frames.push_back(frame.raw_ac4_frame);
    }
    if (!companion.empty()) {
        out.signature.assign(companion.begin(), companion.end());
        out.signature_present = true;
    }
    return out;
}

std::string sidecar_path(std::string_view stream_path) {
    std::string path{stream_path};
    path += ".sig";
    return path;
}

}  // namespace iclforge::ac4::signing
