#include "iclforge/ac3/signing/emdf_atmos_signer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/crc16.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/emdf/frame_layout.hpp"
#include "iclforge/ac3/io/object_strip.hpp"
#include "iclforge/base/crypto/hmac_sha256.hpp"
#include "iclforge/base/crypto/signing_key.hpp"

namespace iclforge::ac3::signing {
namespace {

// Where the frame's fields are is iclforge::ac3::emdf::walk_frame's job (see
// ac3/emdf/frame_layout.hpp): one bit-accurate walk of the syncframe, shared
// with the object-layer strip in iclforge::ac3::io, so the two cannot drift apart. What
// is left here is the part that is actually about signing - which of those
// regions are excluded from the authenticated message, and what is hashed
// over the rest.
using ac3::emdf::BitRange;
using ac3::emdf::FrameLayout;

int prot_bits(int code) { return (code == 0) ? 0 : (code == 1) ? 8 : (code == 2) ? 32 : 128; }

bool bit_at(std::span<const std::byte> f, std::size_t p) {
    // Matches BitReader::read_bit()'s own contract: past the end reads as
    // zero rather than indexing out of bounds. Every caller here derives its
    // range from the layout's own container_len/container_start, so this
    // should never actually trip for a well-formed frame - it is
    // defence in depth for a function that indexes `f` directly, outside
    // BitReader.
    if ((p >> 3) >= f.size()) return false;
    return (std::to_integer<std::uint32_t>(f[p >> 3]) >> (7 - (p & 7))) & 1;
}

// Whether the container the tag covers is ALL of the object layer the frame
// carries. The authenticated message hashes the container's content (B) and
// the audio around it (A), and every skip field - the whole of it, flag and
// length and data - is a hole in A. So a second payload tucked into another
// block's skip field, or bytes trailing the container inside its own, are
// invisible to the tag: a frame could keep a valid tag over its real container
// and carry anything beside it. This project's encoder writes the one
// container, exactly, in one block's skip field and nothing in the others
// (put_skip_field in eac3_frame.cpp), so a frame that carries more than that
// was not written by the signer's own pipeline and is not covered by its tag.
// Reporting it as valid would let a licensed-style gate play objects nothing
// authenticated.
bool container_is_sole_skip_payload(const FrameLayout& p) {
    // skiple (1) + skipl (9) precede the skip data.
    constexpr std::size_t kSkipHeaderBits = 10;
    constexpr std::size_t kEmdfHeaderBits = 32;  // sync word + container length
    bool found = false;
    for (const auto& field : p.skip_fields) {
        if (!field.present) {
            continue;  // skiple = 0: the block says it carries nothing
        }
        if (!field.carries_container || found) {
            return false;
        }
        found = true;
        const std::size_t data_start = field.range.first + kSkipHeaderBits;
        const std::size_t data_bits = field.range.bits() - kSkipHeaderBits;
        if (p.container_start != data_start ||
            data_bits != kEmdfHeaderBits + static_cast<std::size_t>(p.container_len) * 8) {
            return false;
        }
    }
    return found;
}

// The message a frame's tag is computed over (A||B), and where in the frame
// the tag belongs. Everything that does not depend on the key, so that signing
// and verifying - and verifying against several keys - build it once and
// differ only in what they do with the HMAC: signing writes the digest into
// the frame at `prim_off`, verifying reads what is already there and compares.
// nullopt means "no container to sign/verify", the same as the layout's own
// has_container.
struct TagMessage {
    std::vector<std::byte> message;
    int np = 0;
    std::size_t prim_off = 0;
    // container_is_sole_skip_payload(): only then does a matching tag say
    // anything about the frame's whole object layer.
    bool sole_container = false;
};

std::optional<TagMessage> build_tag_message(std::span<const std::byte> frame) {
    const FrameLayout p = ac3::emdf::walk_frame(frame);
    // A frame outside the walker's scope, or one whose fields stopped making
    // sense part-way through, reports no container - so it is left unsigned
    // rather than signed over a bit range that was never confirmed.
    if (!p.supported || !p.has_container) return std::nullopt;

    // Reconstruct A: excise holes, pack MSB-first, round to nearest 16-bit word.
    const std::size_t total = frame.size() * 8;
    std::vector<std::uint8_t> kept;
    kept.reserve(total);
    std::size_t pos = 0;
    // holes are recorded in walk order, which is ascending; sort defensively.
    auto holes = p.holes;
    std::sort(holes.begin(), holes.end(),
              [](const BitRange& x, const BitRange& y) { return x.first < y.first; });
    for (const auto& h : holes) {
        for (std::size_t q = pos; q < h.first; ++q) kept.push_back(bit_at(frame, q) ? 1 : 0);
        if (h.last + 1 > pos) pos = h.last + 1;
    }
    for (std::size_t q = pos; q < total; ++q) kept.push_back(bit_at(frame, q) ? 1 : 0);
    const std::size_t words = (kept.size() + 8) / 16;
    const std::size_t target = words * 16;
    if (target > kept.size()) kept.resize(target, 0);
    else kept.resize(target);
    std::vector<std::uint8_t> a_bytes((kept.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < kept.size(); ++i)
        if (kept[i]) a_bytes[i >> 3] |= std::uint8_t(1u << (7 - (i & 7)));

    // Build B: container content, primary+secondary tag bits zeroed - always,
    // regardless of what those bits currently hold, so verifying reproduces
    // exactly the message signing itself hashed.
    const int np = prot_bits(p.protection_primary_code);
    const int ms = prot_bits(p.protection_secondary_code);
    const std::size_t clen = std::size_t(p.container_len);
    std::vector<std::uint8_t> content(clen * 8);
    for (std::size_t k = 0; k < clen * 8; ++k)
        content[k] = bit_at(frame, p.container_start + 32 + k) ? 1 : 0;
    const std::size_t pb = p.container_parsed_bits;
    for (std::size_t k = (pb - std::size_t(np) - std::size_t(ms) - 32); k < pb - 32; ++k)
        if (k < content.size()) content[k] = 0;
    std::vector<std::uint8_t> b_bytes((content.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < content.size(); ++i)
        if (content[i]) b_bytes[i >> 3] |= std::uint8_t(1u << (7 - (i & 7)));

    // tag = HMAC(key, A||B)[:np/8], with the key supplied by the operator.
    std::vector<std::byte> msg;
    msg.reserve(a_bytes.size() + b_bytes.size());
    for (std::uint8_t x : a_bytes) msg.push_back(std::byte{x});
    for (std::uint8_t x : b_bytes) msg.push_back(std::byte{x});

    const std::size_t prim_off = p.container_start + pb - std::size_t(np) - std::size_t(ms);
    return TagMessage{.message = std::move(msg),
                      .np = np,
                      .prim_off = prim_off,
                      .sole_container = container_is_sole_skip_payload(p)};
}

// The tag `key` gives a frame whose message is `message`: the full digest,
// of which the primary field holds the first np bits.
std::array<std::byte, 32> tag_digest(const TagMessage& message,
                                     const base::crypto::SigningKey& key) {
    return base::crypto::hmac_sha256(key.bytes(), message.message);
}

// Whether the np bits the frame holds at the tag's position are `digest`'s
// first np. A container that declares no primary protection field at all
// (protection_length_primary 00, which Table H.2.5 reserves) has no tag to
// match: comparing zero bits would "match" every key, so it is never a match.
bool tag_matches(std::span<const std::byte> frame, const TagMessage& message,
                 const std::array<std::byte, 32>& digest) {
    if (message.np <= 0) {
        return false;
    }
    for (int i = 0; i < message.np; ++i) {
        const std::size_t q = message.prim_off + std::size_t(i);
        const bool actual = bit_at(frame, q);
        const bool expected = (std::to_integer<std::uint32_t>(digest[std::size_t(i / 8)]) >>
                               (7 - (i & 7))) &
                              1;
        if (actual != expected) return false;
    }
    return true;
}

}  // namespace

bool has_authenticity_tag(std::span<const std::byte> frame) {
    // Deliberately key-free: where the tag LIVES is fixed by the container's
    // own protection-length codes, and only whether it matches needs a key.
    // So an inspection tool can answer "is this stream signed at all" - the
    // question `forge probe` asks - without holding anything secret, which
    // is the whole point of keeping the key out of this tool (see
    // docs/concepts/object-signing.md).
    // walk_frame screens the frame's shape itself and reports no container
    // for anything outside this signer's subset - an ordinary non-Atmos
    // frame included - so nothing here has to pre-qualify what it is handed.
    const FrameLayout p = ac3::emdf::walk_frame(frame);
    if (!p.supported || !p.has_container) {
        return false;
    }
    const int np = prot_bits(p.protection_primary_code);
    if (np <= 0) {
        // No primary protection field at all - the container declared it
        // absent, so there is nowhere for a tag to be.
        return false;
    }
    const std::size_t prim_off =
        p.container_start + p.container_parsed_bits - std::size_t(np) -
        std::size_t(prot_bits(p.protection_secondary_code));
    // An all-zero field is what an unsigned container carries: §H.2.2.4 leaves
    // the content implementation-defined, and this project's own writer emits
    // zeros until sign_atmos_frame replaces them. A real HMAC truncation
    // being all-zero is a 2^-np coincidence.
    for (int i = 0; i < np; ++i) {
        const std::size_t q = prim_off + static_cast<std::size_t>(i);
        if ((q >> 3) >= frame.size()) {
            return false;
        }
        if (bit_at(frame, q)) {
            return true;
        }
    }
    return false;
}

bool sign_atmos_frame(std::span<std::byte> frame, const base::crypto::SigningKey& key) {
    if (key.empty()) return false;
    const auto message = build_tag_message(frame);
    // A container that declares no primary protection field has nowhere to
    // hold a tag: nothing would be written, and "signed" would be a lie.
    if (!message || message->np <= 0) return false;
    const std::array<std::byte, 32> digest = tag_digest(*message, key);

    // write protection_bits_primary (np bits) at prim_off. Bounds-checked for
    // the same reason bit_at() is: this indexes `frame` directly. A
    // well-formed match from walk_frame's single-container-per-frame rule
    // should never actually reach the out-of-range branch, but a write past
    // the end would corrupt the wrong memory rather than just read garbage,
    // so this one fails safe by skipping instead of clamping.
    for (int i = 0; i < message->np; ++i) {
        std::size_t q = message->prim_off + std::size_t(i);
        if ((q >> 3) >= frame.size()) continue;
        const bool bit = (std::to_integer<std::uint32_t>(digest[std::size_t(i / 8)]) >>
                          (7 - (i & 7))) &
                         1;
        std::byte& byte = frame[q >> 3];
        if (bit)
            byte |= static_cast<std::byte>(1u << (7 - (q & 7)));
        else
            byte &= static_cast<std::byte>(~(1u << (7 - (q & 7))) & 0xFFu);
    }
    // recompute crc2 (last two bytes; covers everything after the 16-bit sync)
    const std::uint16_t c = crc16(std::span<const std::byte>(frame).subspan(2, frame.size() - 4));
    frame[frame.size() - 2] = std::byte(c >> 8);
    frame[frame.size() - 1] = std::byte(c & 0xFF);
    return true;
}

int sign_atmos_stream(std::span<std::byte> stream, const base::crypto::SigningKey& key) {
    if (key.empty()) return 0;
    int signed_count = 0;
    std::size_t off = 0;
    while (off + 6 <= stream.size()) {
        const std::size_t size = ac3::emdf::syncframe_size(stream.subspan(off));
        if (off + size > stream.size()) break;
        if (sign_atmos_frame(stream.subspan(off, size), key)) ++signed_count;
        off += size;
    }
    return signed_count;
}

VerifyResult verify_atmos_frame(std::span<const std::byte> frame, const base::crypto::SigningKey& key) {
    const auto message = build_tag_message(frame);
    if (!message) return VerifyResult::kNoContainer;
    // Compare the digest just computed against whatever tag bits the frame
    // already carries at prim_off - unlike sign_atmos_frame, nothing here is
    // written back. A tag over a container that is not the frame's whole
    // object layer vouches for less than a caller reads "valid" to mean.
    if (!message->sole_container || !tag_matches(frame, *message, tag_digest(*message, key))) {
        return VerifyResult::kMismatch;
    }
    return VerifyResult::kValid;
}

KeyringVerdict verify_atmos_frame_any(std::span<const std::byte> frame,
                                      std::span<const base::crypto::SigningKey> keys) {
    const auto message = build_tag_message(frame);
    if (!message) return {};  // kNoContainer: no key changes that
    if (!message->sole_container) {
        return {.result = VerifyResult::kMismatch, .key_index = 0};
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].empty()) continue;
        if (tag_matches(frame, *message, tag_digest(*message, keys[i]))) {
            return {.result = VerifyResult::kValid, .key_index = i};
        }
    }
    return {.result = VerifyResult::kMismatch, .key_index = 0};
}

VerifySummary verify_atmos_stream(std::span<const std::byte> stream, const base::crypto::SigningKey& key) {
    VerifySummary summary;
    std::size_t off = 0;
    while (off + 6 <= stream.size()) {
        const std::size_t size = ac3::emdf::syncframe_size(stream.subspan(off));
        if (off + size > stream.size()) break;
        switch (verify_atmos_frame(stream.subspan(off, size), key)) {
            case VerifyResult::kValid: ++summary.valid; break;
            case VerifyResult::kMismatch: ++summary.mismatch; break;
            case VerifyResult::kNoContainer: ++summary.no_container; break;
        }
        off += size;
    }
    return summary;
}

KeyringSummary verify_atmos_stream_any(std::span<const std::byte> stream,
                                       std::span<const base::crypto::SigningKey> keys) {
    KeyringSummary summary;
    summary.per_key.assign(keys.size(), 0);
    std::size_t off = 0;
    while (off + 6 <= stream.size()) {
        const std::size_t size = ac3::emdf::syncframe_size(stream.subspan(off));
        if (off + size > stream.size()) break;
        const KeyringVerdict verdict = verify_atmos_frame_any(stream.subspan(off, size), keys);
        switch (verdict.result) {
            case VerifyResult::kValid:
                ++summary.totals.valid;
                ++summary.per_key[verdict.key_index];
                break;
            case VerifyResult::kMismatch: ++summary.totals.mismatch; break;
            case VerifyResult::kNoContainer: ++summary.totals.no_container; break;
        }
        off += size;
    }
    return summary;
}

std::expected<GatedStream, io::StripError> gate_atmos_stream(
    std::span<const std::byte> stream, std::span<const base::crypto::SigningKey> keys) {
    // The same refusals strip_objects makes of a stream it cannot walk, in its
    // own words, so a caller reports one family of errors whichever it called.
    if (stream.size() < 6) {
        return std::unexpected(io::StripError::kEmpty);
    }
    const auto frames = split_frames(stream);
    if (!frames) {
        return std::unexpected(frames.error() == DecodeError::kTruncated
                                   ? io::StripError::kTruncated
                                   : io::StripError::kLostSync);
    }
    if (frames->empty()) {
        return std::unexpected(io::StripError::kEmpty);
    }

    GatedStream out;
    out.bytes.reserve(stream.size());
    out.summary.per_key.assign(keys.size(), 0);
    for (const std::span<const std::byte> frame : *frames) {
        // An AC-3 syncframe (bsid <= 8) has no Annex E skip-field object
        // layer - the §E2.3.1.2 legacy core keeps its objects in the
        // dependent behind it - so it is already as bed as it gets.
        const auto bsid = stream_bsid(frame);
        if (!bsid || *bsid <= 8) {
            out.bytes.insert(out.bytes.end(), frame.begin(), frame.end());
            ++out.summary.no_objects;
            continue;
        }
        const KeyringVerdict verdict = verify_atmos_frame_any(frame, keys);
        if (verdict.result == VerifyResult::kValid) {
            out.bytes.insert(out.bytes.end(), frame.begin(), frame.end());
            ++out.summary.passed;
            ++out.summary.per_key[verdict.key_index];
            continue;
        }
        // Not authenticated, or nothing the signer's walk can read. Whether
        // there is an object layer to take out is strip_objects's call, made
        // with the same frame map: it copies a frame with none through and
        // refuses one it cannot rewrite - which here is an object layer that
        // could be neither verified nor removed, so the gate fails closed.
        const auto stripped = io::strip_objects(frame);
        if (!stripped) {
            return std::unexpected(stripped.error());
        }
        out.bytes.insert(out.bytes.end(), stripped->bytes.begin(), stripped->bytes.end());
        if (stripped->frames_stripped > 0) {
            ++out.summary.gated;
        } else {
            ++out.summary.no_objects;
        }
    }
    return out;
}

}  // namespace iclforge::ac3::signing
