#pragma once

// SHA-256 (FIPS 180-4), self-contained: the hash under hmac_sha256.hpp, which the EMDF Atmos
// signer (iclforge::ac3::signing) computes its tag with. A caller wants the MAC or the signer
// above it, not the raw hash. No third-party dependency by design:
// the codec's only third-party library is {fmt}, for formatting (see the top
// of vcpkg.json), so this is a from-the-standard implementation rather than a
// pull of OpenSSL/mbedTLS for one primitive.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

// Incremental so HMAC can feed it ipad/opad and the message in separate
// updates without first concatenating them into one buffer.
class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(std::span<const std::byte> data);
    // Finalizes into `out` and leaves the object reset(), ready to reuse.
    void finish(std::span<std::byte, 32> out);

private:
    void process_block(const std::uint8_t* block);

    std::array<std::uint32_t, 8> h_{};
    std::uint64_t total_bytes_ = 0;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
};

// Convenience one-shot over a whole buffer. Exported (unlike Sha256 itself) so that
// tests/base/test_crypto.cpp can run FIPS 180-4's known-answer vectors against a shared
// libiclforge_base.
ICLFORGE_BASE_EXPORT std::array<std::byte, 32> sha256(std::span<const std::byte> data);

}  // namespace iclforge::base::crypto
