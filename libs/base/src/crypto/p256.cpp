#include "iclforge/base/crypto/p256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "iclforge/base/crypto/hmac_sha256.hpp"
#include "iclforge/base/crypto/secure_zero.hpp"
#include "p256_math.hpp"

namespace iclforge::base::crypto {
namespace {

using detail::Jacobian;
using detail::Limbs;

// x mod n for x < 2^256 < 2n: one conditional subtraction.
Limbs reduce_mod_order(const Limbs& x) {
    Limbs diff{};
    const std::uint32_t borrow = detail::sub_with_borrow(diff, x, detail::kOrder.m);
    return detail::select(0u - (borrow ^ 1u), diff, x);
}

// 1 <= x <= n-1, as a mask (all ones when it holds).
std::uint32_t scalar_in_range_mask(const Limbs& x) {
    const std::uint32_t below_order = 0u - detail::less_than(x, detail::kOrder.m);
    return below_order & ~detail::is_zero_mask(x);
}

}  // namespace

// --- P256PublicKey -------------------------------------------------------------------------

std::optional<P256PublicKey> P256PublicKey::from_uncompressed(std::span<const std::byte> point) {
    if (point.size() != 65 || point[0] != std::byte{0x04}) {
        return std::nullopt;
    }
    const Limbs x = detail::limbs_from_bytes(point.data() + 1);
    const Limbs y = detail::limbs_from_bytes(point.data() + 33);
    // SEC 1 section 3.2.3.1: each coordinate an integer in 0..p-1, and the point on the curve.
    // P-256 has cofactor 1, so every such point is in the group of prime order n.
    if (detail::less_than(x, detail::kField.m) == 0 ||
        detail::less_than(y, detail::kField.m) == 0 || !detail::on_curve(x, y)) {
        return std::nullopt;
    }
    P256PublicKey key;
    std::copy(point.begin() + 1, point.end(), key.coordinates_.begin());
    return key;
}

std::array<std::byte, 65> P256PublicKey::uncompressed() const {
    std::array<std::byte, 65> out{};
    out[0] = std::byte{0x04};
    std::copy(coordinates_.begin(), coordinates_.end(), out.begin() + 1);
    return out;
}

// --- P256PrivateKey ------------------------------------------------------------------------

P256PrivateKey::~P256PrivateKey() {
    secure_zero(scalar_);
}

std::optional<P256PrivateKey> P256PrivateKey::from_scalar(std::span<const std::byte> scalar) {
    if (scalar.size() != 32) {
        return std::nullopt;
    }
    Limbs d = detail::limbs_from_bytes(scalar.data());
    const bool valid = scalar_in_range_mask(d) != 0;
    secure_zero(d);
    if (!valid) {
        return std::nullopt;
    }
    P256PrivateKey key;
    std::copy(scalar.begin(), scalar.end(), key.scalar_.begin());
    key.set_ = true;
    return key;
}

std::optional<P256PublicKey> P256PrivateKey::public_key() const {
    if (set_) {
        Limbs d = detail::limbs_from_bytes(scalar_.data());
        const Jacobian q = detail::scalar_multiply(d, detail::generator());
        secure_zero(d);
        Limbs x{};
        Limbs y{};
        if (detail::to_affine(q, x, y)) {
            std::array<std::byte, 65> point{};
            point[0] = std::byte{0x04};
            detail::limbs_to_bytes(x, point.data() + 1);
            detail::limbs_to_bytes(y, point.data() + 33);
            return P256PublicKey::from_uncompressed(point);
        }
    }
    return std::nullopt;
}

bool p256_scalar_in_range(std::span<const std::byte, 32> value) {
    return scalar_in_range_mask(detail::limbs_from_bytes(value.data())) != 0;
}

// --- RFC 6979 ------------------------------------------------------------------------------

std::optional<std::array<std::byte, 32>> rfc6979_nonce(const P256PrivateKey& key,
                                                       const Digest256& digest,
                                                       std::uint32_t skip) {
    if (key.empty()) {
        return std::nullopt;
    }
    // Section 3.2 step a-b: h1 is the message hash; its octet string is bits2octets(h1), which for
    // qlen = hlen = 256 is h1 reduced modulo n. The x of step d is int2octets(x), the key as 32
    // bytes.
    std::array<std::byte, 32> h1{};
    {
        const Limbs reduced = reduce_mod_order(detail::limbs_from_bytes(digest.data()));
        detail::limbs_to_bytes(reduced, h1.data());
    }

    std::array<std::byte, 32> v{};
    std::array<std::byte, 32> k{};
    v.fill(std::byte{0x01});  // step b
    k.fill(std::byte{0x00});  // step c

    // V || marker || x || h1, steps d and f.
    std::array<std::byte, 97> material{};
    const auto update = [&](std::byte marker) {
        std::copy(v.begin(), v.end(), material.begin());
        material[32] = marker;
        std::copy(key.scalar().begin(), key.scalar().end(), material.begin() + 33);
        std::copy(h1.begin(), h1.end(), material.begin() + 65);
        k = hmac_sha256(k, material);
        v = hmac_sha256(k, v);
    };
    update(std::byte{0x00});  // steps d and e
    update(std::byte{0x01});  // steps f and g

    std::optional<std::array<std::byte, 32>> result;
    while (!result.has_value()) {
        // Step h.2: with qlen equal to hlen, one block of V is the candidate T.
        v = hmac_sha256(k, v);
        const Limbs candidate = detail::limbs_from_bytes(v.data());
        if (scalar_in_range_mask(candidate) != 0) {
            if (skip == 0) {
                result = v;
                break;
            }
            --skip;
        }
        // Step h.3: the candidate was not acceptable (or is not the one asked for).
        std::array<std::byte, 33> again{};
        std::copy(v.begin(), v.end(), again.begin());
        again[32] = std::byte{0x00};
        k = hmac_sha256(k, again);
        v = hmac_sha256(k, v);
    }
    secure_zero(h1);
    secure_zero(v);
    secure_zero(k);
    secure_zero(material);
    return result;
}

// --- ECDSA ---------------------------------------------------------------------------------

std::optional<EcdsaSignature> ecdsa_sign(const P256PrivateKey& key, const Digest256& digest) {
    if (key.empty()) {
        return std::nullopt;
    }
    using detail::kOrder;
    Limbs d = detail::limbs_from_bytes(key.scalar().data());
    Limbs d_mont = detail::to_mont(d, kOrder);
    secure_zero(d);
    const Limbs e_mont =
        detail::to_mont(reduce_mod_order(detail::limbs_from_bytes(digest.data())), kOrder);

    std::optional<EcdsaSignature> result;
    for (std::uint32_t attempt = 0; !result.has_value(); ++attempt) {
        auto k_bytes = rfc6979_nonce(key, digest, attempt);
        if (!k_bytes.has_value()) {
            break;
        }
        Limbs k = detail::limbs_from_bytes(k_bytes->data());
        secure_zero(*k_bytes);

        // FIPS 186-4 6.3: R = k G, r = x(R) mod n.
        const Jacobian big_r = detail::scalar_multiply(k, detail::generator());
        Limbs rx{};
        Limbs ry{};
        if (!detail::to_affine(big_r, rx, ry)) {
            secure_zero(k);
            continue;
        }
        const Limbs r = reduce_mod_order(rx);
        if (detail::is_zero_mask(r) != 0) {
            secure_zero(k);
            continue;
        }
        // s = k^-1 (e + r d) mod n, in Montgomery form throughout.
        Limbs k_mont = detail::to_mont(k, kOrder);
        secure_zero(k);
        Limbs k_inverse = detail::mont_inverse(k_mont, kOrder);
        secure_zero(k_mont);
        Limbs t = detail::mod_add(
            e_mont, detail::mont_mul(detail::to_mont(r, kOrder), d_mont, kOrder), kOrder.m);
        const Limbs s = detail::from_mont(detail::mont_mul(k_inverse, t, kOrder), kOrder);
        secure_zero(k_inverse);
        secure_zero(t);
        if (detail::is_zero_mask(s) != 0) {
            continue;
        }
        EcdsaSignature signature;
        detail::limbs_to_bytes(r, signature.r.data());
        detail::limbs_to_bytes(s, signature.s.data());
        result = signature;
    }
    secure_zero(d_mont);
    return result;
}

bool ecdsa_verify(const P256PublicKey& key, const Digest256& digest,
                  const EcdsaSignature& signature) {
    using detail::kOrder;
    const Limbs r = detail::limbs_from_bytes(signature.r.data());
    const Limbs s = detail::limbs_from_bytes(signature.s.data());
    if (scalar_in_range_mask(r) == 0 || scalar_in_range_mask(s) == 0) {
        return false;
    }
    const Limbs e = reduce_mod_order(detail::limbs_from_bytes(digest.data()));

    // FIPS 186-4 6.4.2: w = s^-1, u1 = e w, u2 = r w, R = u1 G + u2 Q, accept when x(R) mod n = r.
    const Limbs w = detail::mont_inverse(detail::to_mont(s, kOrder), kOrder);
    const Limbs u1 =
        detail::from_mont(detail::mont_mul(detail::to_mont(e, kOrder), w, kOrder), kOrder);
    const Limbs u2 =
        detail::from_mont(detail::mont_mul(detail::to_mont(r, kOrder), w, kOrder), kOrder);

    std::array<std::byte, 65> point = key.uncompressed();
    const Jacobian q = detail::from_affine(detail::limbs_from_bytes(point.data() + 1),
                                           detail::limbs_from_bytes(point.data() + 33));
    const Jacobian sum = detail::point_add(detail::scalar_multiply(u1, detail::generator()),
                                           detail::scalar_multiply(u2, q));
    Limbs x{};
    Limbs y{};
    if (!detail::to_affine(sum, x, y)) {
        return false;
    }
    return detail::equal_mask(reduce_mod_order(x), r) != 0;
}

}  // namespace iclforge::base::crypto
