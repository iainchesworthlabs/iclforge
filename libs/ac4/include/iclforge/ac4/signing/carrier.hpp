#pragma once

// Where an AC-4 content signature travels, and how a stream is taken apart to find it.
//
// The specification says only that the serialised ContentSignature is "set as AuxData on the
// audio track" (its section 8.1) and gives no bit syntax, so the carrier is a choice of this
// project (decision 1 of planning/ac4-content-signing.md). Two are built:
//
//   ElementaryCarrier  an `.ac4` elementary stream, with the signature in a file beside it
//                      (`<stream>.sig`). The stream is not touched.
//   Mp4UuidCarrier     an MP4 file, with the signature in a `uuid` box in the AC-4 track's `trak`.
//                      It lives in the forge command line's sources (apps/forge/cli/src), because
//                      iclforge::ac4 does not use the container library.
//
// A Carrier reads. Writing is each writer's own affair, since a writer already knows its
// container: sign_frames() (content_signing.hpp) returns the payload, the sidecar writer stores it
// and mp4::mux() takes it as an AudioTrack::user_boxes entry. A later carrier that places the
// payload inside the frames (an EMDF payload, the specification's "AuxData") adds a class and a
// writer, and verify_frames() is unchanged.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac4/export.hpp"

namespace iclforge::ac4::signing {

// A stream taken apart.
struct CarriedContent {
    // The raw_ac4_frame of every frame, in stream order: the signed span (content_signing.hpp).
    // Views into the bytes the carrier was given, which must outlive this.
    std::vector<std::span<const std::byte>> frames;
    // The serialised ContentSignature, copied, when signature_present.
    std::vector<std::byte> signature;
    // False when the stream carries no signature. An empty `signature` with this set is a
    // signature of no bytes, which verification refuses as it does any malformed one.
    bool signature_present = false;
};

enum class CarrierErrorKind : std::uint8_t {
    kLostSync,             // an elementary stream has bytes that are not a sync frame
    kTruncatedFrame,       // an elementary stream ends inside a frame
    kContainerUnreadable,  // the container does not parse
    kNoAc4Track,           // the container holds no AC-4 track
    kAmbiguousSignature,   // the stream carries more than one content signature
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view describe(CarrierErrorKind kind);

struct CarrierError {
    CarrierErrorKind kind = CarrierErrorKind::kLostSync;
    // Where in the stream, for the kinds that have a place; 0 otherwise.
    std::size_t offset = 0;
    // A sentence naming what was found: "the stream loses sync at byte 4096".
    std::string detail;
};

class Carrier {
   public:
    Carrier() = default;
    Carrier(const Carrier&) = delete;
    Carrier& operator=(const Carrier&) = delete;
    virtual ~Carrier() = default;

    // A short name for messages: "elementary stream with sidecar", "MP4 uuid box".
    [[nodiscard]] virtual std::string_view name() const = 0;

    // Takes `stream` apart. `companion` is the bytes of a second file for a carrier that keeps
    // one (the sidecar), and is ignored by one that does not. A stream that cannot be read to
    // its end is an error and not a shorter stream: frames after a bad place would be bytes the
    // signature does not cover.
    [[nodiscard]] virtual std::expected<CarriedContent, CarrierError> read(
        std::span<const std::byte> stream, std::span<const std::byte> companion) const = 0;
};

// An AC-4 elementary stream (Annex G.3.1's ac4_syncframe() back to back) and a sidecar file.
// An empty `companion` means there is no sidecar: the content comes back with signature_present
// false. The stream must scan to its last byte; trailing bytes that are not a frame are
// kLostSync (or kTruncatedFrame) and not ignored, since they would be unauthenticated.
class ICLFORGE_AC4_EXPORT ElementaryCarrier final : public Carrier {
   public:
    [[nodiscard]] std::string_view name() const override {
        return "elementary stream with sidecar";
    }

    [[nodiscard]] std::expected<CarriedContent, CarrierError> read(
        std::span<const std::byte> stream, std::span<const std::byte> companion) const override;
};

// The sidecar's name for a stream's: the stream's path with ".sig" after it.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::string sidecar_path(std::string_view stream_path);

}  // namespace iclforge::ac4::signing
