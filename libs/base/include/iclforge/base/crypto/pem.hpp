#pragma once

// PEM (RFC 7468) and the base64 under it (RFC 4648 §4), for reading the operator's certificate
// chain and private key from the files OpenSSL writes.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

struct PemBlock {
    std::string label;           // "CERTIFICATE", "PRIVATE KEY", "EC PRIVATE KEY", ...
    std::vector<std::byte> der;  // the decoded body
};

// Every `-----BEGIN label-----` ... `-----END label-----` block in `text`, in order. Text outside
// the blocks (the explanatory lines OpenSSL's `x509 -text` leaves) is ignored. A block whose body
// is not base64, or whose END label differs from its BEGIN label, is an error for the whole input
// (nullopt): a half-read chain would be worse than none.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::vector<PemBlock>> pem_decode(
    std::string_view text);

[[nodiscard]] ICLFORGE_BASE_EXPORT std::string pem_encode(std::string_view label,
                                                          std::span<const std::byte> der);

// Standard-alphabet base64 with '=' padding. Whitespace is skipped on decode; anything else outside
// the alphabet, or padding in the wrong place, is nullopt.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::vector<std::byte>> base64_decode(
    std::string_view text);
[[nodiscard]] ICLFORGE_BASE_EXPORT std::string base64_encode(std::span<const std::byte> data);

}  // namespace iclforge::base::crypto
