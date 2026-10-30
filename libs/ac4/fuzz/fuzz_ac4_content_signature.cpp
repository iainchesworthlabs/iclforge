#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/ac4/io/content_signature.hpp"
#include "iclforge/base/crypto/content_signature.hpp"

// iclforge::ac4::locate_signature_trailer, signed_content_digest, summarise_signature,
// strip_signature and verify_stream (libs/ac4/src/io/content_signature.cpp): a raw .ac4 stream with
// a content signature after its last sync frame, as `forge decode ... verify-content`,
// `ac4-verify` and `probe` read one.
//
// The trailer is found by walking the sync frames forward by their own sizes until the bytes stop
// being a sync word, so the walk decides where the signed content ends: a frame size that runs past
// the end, an escaped size, a trailer that itself begins with 0xAC, fewer than four bytes after the
// last frame. Everything after that point goes to the Thrift and X.509 readers fuzz_content_
// signature presses directly; this harness presses the stream-level joins, which hand them a span
// they must not read past and a digest boundary that must not move.
//
// The same input is verified under each way of judging time, with no trust anchor and with the
// certificates the blob carries taken as anchors, so the anchored paths are reached without a
// key. A stream that signs nothing simply fails to verify.
namespace {

namespace crypto = iclforge::base::crypto;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> stream{reinterpret_cast<const std::byte*>(data), size};

    const auto located = iclforge::ac4::locate_signature_trailer(stream);
    (void)iclforge::ac4::signed_content_digest(stream);
    (void)iclforge::ac4::summarise_signature(stream);
    (void)iclforge::ac4::strip_signature(stream);

    std::vector<crypto::Certificate> anchors;
    if (located.has_value() && located->present) {
        // The trailer is inside the stream and the prefix before it is the content: the two
        // spans meet exactly, which is what the digest boundary relies on.
        if (located->content_size + located->blob.size() != stream.size() ||
            located->blob.data() != stream.data() + located->content_size) {
            __builtin_trap();
        }
        if (const auto parsed = crypto::parse_content_signature(located->blob)) {
            anchors = parsed->chain;
        }
    }
    for (const auto basis : {crypto::TimeBasis::kNow, crypto::TimeBasis::kSignedDate,
                             crypto::TimeBasis::kInstant, crypto::TimeBasis::kIgnore}) {
        crypto::VerifyPolicy policy;
        policy.time_basis = basis;
        policy.now = 1'780'000'000;
        policy.instant = 1'700'000'000;
        (void)iclforge::ac4::verify_stream(stream, policy);
        if (!anchors.empty()) {
            policy.trust_anchors = anchors;
            policy.required_leaf_title.clear();
            policy.require_digital_signature = false;
            (void)iclforge::ac4::verify_stream(stream, policy);
        }
    }
    return 0;
}
