#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

#include "iclforge/ac4/signing/carrier.hpp"
#include "iclforge/containers/mp4/mp4.hpp"

// The MP4 carrier of an AC-4 content signature: a `uuid` box whose usertype is the content
// signature UUID (iclforge::base::crypto::kContentSignatureUsertype), a direct child of the AC-4
// track's `trak`, whose payload is the serialised ContentSignature. The samples of the track are
// the frames the signature covers, byte for byte, so a stream and its MP4 have one digest.
//
// In the forge command line's sources, not in iclforge::ac4, because the codec library does not
// use the container library (the layering of tools/checks/projects.json): finding a box in a file
// needs both of them and the signature code of iclforge::base, and an application is the layer
// that may use all three.
//
// Writing is the other half and is one line for a caller: put content_signature_user_box(payload)
// in AudioTrack::user_boxes and mux() or fragment() writes it. A fragmented file carries it in
// its initialization segment. Reading a folder of segments back to verify them is not built
// (planning/ac4-content-signing.md, "Out of scope"); a single fragmented file is read like any.

namespace iclforge::apps {

// The `uuid` box that carries `payload` (a serialised ContentSignature).
[[nodiscard]] iclforge::containers::mp4::UserBox content_signature_user_box(
    std::span<const std::byte> payload);

class Mp4UuidCarrier final : public iclforge::ac4::signing::Carrier {
   public:
    [[nodiscard]] std::string_view name() const override { return "MP4 uuid box"; }

    // `stream` is the whole MP4 file; `companion` is ignored. The frames are the samples of the
    // file's audio track, which must be AC-4, as views into `stream`. A file with more than one
    // content signature box on the track is kAmbiguousSignature: the verifier would otherwise be
    // choosing which of two claims to believe.
    [[nodiscard]] std::expected<iclforge::ac4::signing::CarriedContent,
                                iclforge::ac4::signing::CarrierError>
    read(std::span<const std::byte> stream, std::span<const std::byte> companion) const override;
};

}  // namespace iclforge::apps
