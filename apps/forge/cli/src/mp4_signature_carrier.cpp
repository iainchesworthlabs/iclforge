#include "mp4_signature_carrier.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mp4/reader.hpp"

namespace iclforge::apps {

namespace {

namespace mp4 = iclforge::containers::mp4;
namespace signing = iclforge::ac4::signing;
namespace crypto = iclforge::base::crypto;

[[nodiscard]] std::unexpected<signing::CarrierError> refuse(signing::CarrierErrorKind kind,
                                                            std::string detail) {
    return std::unexpected(
        signing::CarrierError{.kind = kind, .offset = 0, .detail = std::move(detail)});
}

}  // namespace

mp4::UserBox content_signature_user_box(std::span<const std::byte> payload) {
    mp4::UserBox box;
    box.usertype = crypto::kContentSignatureUsertype;
    box.payload.assign(payload.begin(), payload.end());
    return box;
}

std::expected<signing::CarriedContent, signing::CarrierError> Mp4UuidCarrier::read(
    std::span<const std::byte> stream, std::span<const std::byte> /*companion*/) const {
    // The default options select the first ac-3, ec-3 or ac-4 track. A file whose first audio
    // track is another codec has no AC-4 for this carrier to read, and the message says which.
    const auto demuxed = mp4::demux(stream);
    if (!demuxed.has_value()) {
        const mp4::DemuxError error = demuxed.error();
        return refuse(error == mp4::DemuxError::kNoAudioTrack
                          ? signing::CarrierErrorKind::kNoAc4Track
                          : signing::CarrierErrorKind::kContainerUnreadable,
                      std::string{"the MP4 cannot be read: "} + std::string{mp4::describe(error)});
    }
    if (demuxed->track.codec_id != mp4::kCodecAc4) {
        return refuse(signing::CarrierErrorKind::kNoAc4Track,
                      "the file's audio track is '" + demuxed->track.codec_id + "', not 'ac-4'");
    }

    signing::CarriedContent out;
    out.frames.assign(demuxed->samples.begin(), demuxed->samples.end());

    const mp4::UserBox* found = nullptr;
    std::size_t count = 0;
    for (const mp4::UserBox& box : demuxed->track.user_boxes) {
        if (box.usertype == crypto::kContentSignatureUsertype) {
            found = &box;
            ++count;
        }
    }
    if (count > 1) {
        return refuse(signing::CarrierErrorKind::kAmbiguousSignature,
                      "the AC-4 track carries " + std::to_string(count) +
                          " content signature boxes; which one to believe is not for the "
                          "verifier to guess");
    }
    if (found != nullptr) {
        out.signature = found->payload;
        out.signature_present = true;
    }
    return out;
}

}  // namespace iclforge::apps
