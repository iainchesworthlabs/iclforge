#include "iclforge/base/crypto/ecdsa_p256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/base/crypto/hmac_sha256.hpp"

// What the private-key path does about timing (the path that touches d and the nonce k):
//  - Field and scalar arithmetic is Montgomery multiplication on eight 32-bit limbs with fixed loop
//    bounds, and conditional subtraction by mask; nothing branches on a limb value.
//  - The point k*G is a Montgomery ladder (256 iterations, a constant-time swap by mask) over the
//    complete addition law of Renes, Costello and Batina, "Complete addition formulas for prime
//    order elliptic curves" (Eurocrypt 2016), Algorithm 4 for a = -3. A complete law has no special
//    cases (P = Q, P = -Q, the point at infinity), so the sequence of field operations does not
//    depend on the scalar. Doubling is the same law with both operands equal, which costs speed
//    (the dedicated doubling is cheaper) and buys one routine instead of two.
//  - The inversions (k^-1 mod n, Z^-1 mod p) are Fermat exponentiation, where the exponent is the
//    public modulus minus two: the multiply pattern depends on the exponent, not on the secret
//    base.
//  - Memory access is by fixed index; secrets are wiped from locals before returning.
// What it does not do: it makes no claim about power or electromagnetic side channels, nor about a
// compiler that turns a mask into a branch. It is for an operator signing on a machine they
// control.

namespace iclforge::base::crypto {
namespace {

using Word = std::uint32_t;
constexpr std::size_t kLimbs = 8;

// A 256-bit unsigned integer, little-endian 32-bit limbs.
struct U256 {
    std::array<Word, kLimbs> w{};
};

// Wipes with volatile stores, which the optimizer may not drop (the same reason signing_key.cpp
// uses them).
void wipe(U256& v) {
    volatile Word* p = v.w.data();
    for (std::size_t i = 0; i < kLimbs; ++i) {
        p[i] = 0;
    }
}

void wipe_bytes(std::span<std::byte> bytes) {
    volatile std::byte* p = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        p[i] = std::byte{0};
    }
}

constexpr Word mask_of(Word bit) {
    return static_cast<Word>(0) - bit;
}

U256 from_big_endian(std::span<const std::byte, 32> bytes) {
    U256 out;
    for (std::size_t i = 0; i < kLimbs; ++i) {
        const std::size_t base = 32 - 4 * (i + 1);
        out.w[i] = (static_cast<Word>(bytes[base]) << 24) |
                   (static_cast<Word>(bytes[base + 1]) << 16) |
                   (static_cast<Word>(bytes[base + 2]) << 8) | static_cast<Word>(bytes[base + 3]);
    }
    return out;
}

std::array<std::byte, 32> to_big_endian(const U256& v) {
    std::array<std::byte, 32> out{};
    for (std::size_t i = 0; i < kLimbs; ++i) {
        const std::size_t base = 32 - 4 * (i + 1);
        out[base] = static_cast<std::byte>(v.w[i] >> 24);
        out[base + 1] = static_cast<std::byte>(v.w[i] >> 16);
        out[base + 2] = static_cast<std::byte>(v.w[i] >> 8);
        out[base + 3] = static_cast<std::byte>(v.w[i]);
    }
    return out;
}

// r = a + b; returns the carry out (0 or 1).
Word add_raw(U256& r, const U256& a, const U256& b) {
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < kLimbs; ++i) {
        const std::uint64_t cur = static_cast<std::uint64_t>(a.w[i]) + b.w[i] + carry;
        r.w[i] = static_cast<Word>(cur);
        carry = cur >> 32;
    }
    return static_cast<Word>(carry);
}

// r = a - b; returns the borrow out (0 or 1).
Word sub_raw(U256& r, const U256& a, const U256& b) {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < kLimbs; ++i) {
        const std::uint64_t cur = static_cast<std::uint64_t>(a.w[i]) - b.w[i] - borrow;
        r.w[i] = static_cast<Word>(cur);
        borrow = (cur >> 32) & 1U;
    }
    return static_cast<Word>(borrow);
}

// flag ? a : b, for flag 0 or 1.
U256 select(Word flag, const U256& a, const U256& b) {
    const Word m = mask_of(flag);
    U256 out;
    for (std::size_t i = 0; i < kLimbs; ++i) {
        out.w[i] = (a.w[i] & m) | (b.w[i] & ~m);
    }
    return out;
}

Word is_zero(const U256& a) {
    Word acc = 0;
    for (std::size_t i = 0; i < kLimbs; ++i) {
        acc |= a.w[i];
    }
    // 1 when acc == 0: (acc | -acc) has its top bit set exactly when acc != 0.
    return 1U ^ ((acc | (static_cast<Word>(0) - acc)) >> 31);
}

bool equal(const U256& a, const U256& b) {
    Word diff = 0;
    for (std::size_t i = 0; i < kLimbs; ++i) {
        diff |= a.w[i] ^ b.w[i];
    }
    return diff == 0;
}

// 1 when a >= b.
Word greater_equal(const U256& a, const U256& b) {
    U256 scratch;
    return 1U ^ sub_raw(scratch, a, b);
}

// The modulus and the constants Montgomery multiplication needs (R = 2^256).
struct Modulus {
    U256 m;
    Word n0inv = 0;  // -m^-1 mod 2^32
    U256 r2;         // R^2 mod m
    U256 one;        // R mod m, the Montgomery form of 1
};

U256 mod_add(const Modulus& f, const U256& a, const U256& b) {
    U256 sum;
    const Word carry = add_raw(sum, a, b);
    U256 diff;
    const Word borrow = sub_raw(diff, sum, f.m);
    // sum - m is the answer when the addition overflowed 256 bits or sum >= m.
    return select(carry | (1U ^ borrow), diff, sum);
}

U256 mod_sub(const Modulus& f, const U256& a, const U256& b) {
    U256 diff;
    const Word borrow = sub_raw(diff, a, b);
    U256 fixed;
    add_raw(fixed, diff, f.m);
    return select(borrow, fixed, diff);
}

// CIOS Montgomery multiplication: a * b * R^-1 mod m, for a, b < m.
U256 mont_mul(const Modulus& f, const U256& a, const U256& b) {
    std::array<Word, kLimbs + 2> t{};
    for (std::size_t i = 0; i < kLimbs; ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < kLimbs; ++j) {
            const std::uint64_t cur = static_cast<std::uint64_t>(t[j]) +
                                      static_cast<std::uint64_t>(a.w[j]) * b.w[i] + carry;
            t[j] = static_cast<Word>(cur);
            carry = cur >> 32;
        }
        std::uint64_t cur = static_cast<std::uint64_t>(t[kLimbs]) + carry;
        t[kLimbs] = static_cast<Word>(cur);
        t[kLimbs + 1] = static_cast<Word>(cur >> 32);

        const Word q = t[0] * f.n0inv;
        cur = static_cast<std::uint64_t>(t[0]) + static_cast<std::uint64_t>(q) * f.m.w[0];
        carry = cur >> 32;
        for (std::size_t j = 1; j < kLimbs; ++j) {
            cur =
                static_cast<std::uint64_t>(t[j]) + static_cast<std::uint64_t>(q) * f.m.w[j] + carry;
            t[j - 1] = static_cast<Word>(cur);
            carry = cur >> 32;
        }
        cur = static_cast<std::uint64_t>(t[kLimbs]) + carry;
        t[kLimbs - 1] = static_cast<Word>(cur);
        t[kLimbs] = t[kLimbs + 1] + static_cast<Word>(cur >> 32);
    }
    U256 value;
    for (std::size_t i = 0; i < kLimbs; ++i) {
        value.w[i] = t[i];
    }
    U256 diff;
    const Word borrow = sub_raw(diff, value, f.m);
    // The result is value - m when the top word is set or value >= m.
    return select(t[kLimbs] | (1U ^ borrow), diff, value);
}

Modulus make_modulus(const U256& m) {
    Modulus f;
    f.m = m;
    // Newton iteration for m^-1 mod 2^32 (m is odd), then its negative.
    Word inv = m.w[0];
    for (int i = 0; i < 5; ++i) {
        inv *= 2U - m.w[0] * inv;
    }
    f.n0inv = static_cast<Word>(0) - inv;
    // R mod m = 2^256 - m, because m has its top bit set (so R < 2m); R^2 mod m by 256 doublings.
    U256 zero;
    sub_raw(f.one, zero, m);
    U256 x = f.one;
    for (int i = 0; i < 256; ++i) {
        x = mod_add(f, x, x);
    }
    f.r2 = x;
    return f;
}

U256 to_mont(const Modulus& f, const U256& a) {
    return mont_mul(f, a, f.r2);
}

U256 from_mont(const Modulus& f, const U256& a) {
    U256 one;
    one.w[0] = 1;
    return mont_mul(f, a, one);
}

// base^exponent in the Montgomery domain, for a PUBLIC exponent (the modulus minus two): the
// multiply pattern follows the exponent's bits, not the base.
U256 mont_pow(const Modulus& f, const U256& base, const U256& exponent) {
    U256 result = f.one;
    for (int bit = 255; bit >= 0; --bit) {
        result = mont_mul(f, result, result);
        if (((exponent.w[static_cast<std::size_t>(bit) / 32] >> (bit % 32)) & 1U) != 0) {
            result = mont_mul(f, result, base);
        }
    }
    return result;
}

U256 minus_two(const Modulus& f) {
    U256 two;
    two.w[0] = 2;
    U256 out;
    sub_raw(out, f.m, two);
    return out;
}

// SEC 2 v2 §2.4.2, secp256r1.
U256 u256_from_words_msb_first(const std::array<Word, 8>& words) {
    U256 out;
    for (std::size_t i = 0; i < kLimbs; ++i) {
        out.w[i] = words[kLimbs - 1 - i];
    }
    return out;
}

struct Point {
    U256 x, y, z;  // projective, Montgomery form modulo p
};

struct Curve {
    Modulus p;
    Modulus n;
    U256 b;  // Montgomery form modulo p
    Point g;
    U256 p_minus_two;
    U256 n_minus_two;
};

const Curve& curve() {
    static const Curve c = [] {
        Curve k;
        k.p = make_modulus(
            u256_from_words_msb_first({0xFFFFFFFFU, 0x00000001U, 0x00000000U, 0x00000000U,
                                       0x00000000U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU}));
        k.n = make_modulus(
            u256_from_words_msb_first({0xFFFFFFFFU, 0x00000000U, 0xFFFFFFFFU, 0xFFFFFFFFU,
                                       0xBCE6FAADU, 0xA7179E84U, 0xF3B9CAC2U, 0xFC632551U}));
        k.b = to_mont(
            k.p, u256_from_words_msb_first({0x5AC635D8U, 0xAA3A93E7U, 0xB3EBBD55U, 0x769886BCU,
                                            0x651D06B0U, 0xCC53B0F6U, 0x3BCE3C3EU, 0x27D2604BU}));
        k.g.x = to_mont(
            k.p, u256_from_words_msb_first({0x6B17D1F2U, 0xE12C4247U, 0xF8BCE6E5U, 0x63A440F2U,
                                            0x77037D81U, 0x2DEB33A0U, 0xF4A13945U, 0xD898C296U}));
        k.g.y = to_mont(
            k.p, u256_from_words_msb_first({0x4FE342E2U, 0xFE1A7F9BU, 0x8EE7EB4AU, 0x7C0F9E16U,
                                            0x2BCE3357U, 0x6B315ECEU, 0xCBB64068U, 0x37BF51F5U}));
        k.g.z = k.p.one;
        k.p_minus_two = minus_two(k.p);
        k.n_minus_two = minus_two(k.n);
        return k;
    }();
    return c;
}

Point infinity(const Curve& c) {
    Point o;
    o.y = c.p.one;  // (0 : 1 : 0)
    return o;
}

// P + Q for any two points, including P = Q, P = -Q and either at infinity: Renes, Costello and
// Batina 2016, Algorithm 4 (a = -3). t0..t4 and x3..z3 follow the numbering of the paper's steps.
Point point_add(const Curve& c, const Point& p, const Point& q) {
    const Modulus& f = c.p;
    const auto mul = [&f](const U256& a, const U256& b) { return mont_mul(f, a, b); };
    const auto add = [&f](const U256& a, const U256& b) { return mod_add(f, a, b); };
    const auto sub = [&f](const U256& a, const U256& b) { return mod_sub(f, a, b); };

    U256 t0 = mul(p.x, q.x);
    U256 t1 = mul(p.y, q.y);
    U256 t2 = mul(p.z, q.z);
    U256 t3 = add(p.x, p.y);
    U256 t4 = add(q.x, q.y);
    t3 = mul(t3, t4);
    t4 = add(t0, t1);
    t3 = sub(t3, t4);
    t4 = add(p.y, p.z);
    U256 x3 = add(q.y, q.z);
    t4 = mul(t4, x3);
    x3 = add(t1, t2);
    t4 = sub(t4, x3);
    x3 = add(p.x, p.z);
    U256 y3 = add(q.x, q.z);
    x3 = mul(x3, y3);
    y3 = add(t0, t2);
    y3 = sub(x3, y3);
    U256 z3 = mul(c.b, t2);
    x3 = sub(y3, z3);
    z3 = add(x3, x3);
    x3 = add(x3, z3);
    z3 = sub(t1, x3);
    x3 = add(t1, x3);
    y3 = mul(c.b, y3);
    t1 = add(t2, t2);
    t2 = add(t1, t2);
    y3 = sub(y3, t2);
    y3 = sub(y3, t0);
    t1 = add(y3, y3);
    y3 = add(t1, y3);
    t1 = add(t0, t0);
    t0 = add(t1, t0);
    t0 = sub(t0, t2);
    t1 = mul(t4, y3);
    t2 = mul(t0, y3);
    y3 = mul(x3, z3);
    y3 = add(y3, t2);
    x3 = mul(t3, x3);
    x3 = sub(x3, t1);
    z3 = mul(t4, z3);
    t1 = mul(t3, t0);
    z3 = add(z3, t1);
    return Point{x3, y3, z3};
}

void swap_if(Word flag, Point& a, Point& b) {
    const Word m = mask_of(flag);
    for (std::size_t i = 0; i < kLimbs; ++i) {
        Word t = m & (a.x.w[i] ^ b.x.w[i]);
        a.x.w[i] ^= t;
        b.x.w[i] ^= t;
        t = m & (a.y.w[i] ^ b.y.w[i]);
        a.y.w[i] ^= t;
        b.y.w[i] ^= t;
        t = m & (a.z.w[i] ^ b.z.w[i]);
        a.z.w[i] ^= t;
        b.z.w[i] ^= t;
    }
}

// k * P by the Montgomery ladder: R0 = (prefix of k) * P and R1 = R0 + P throughout.
Point scalar_mult(const Curve& c, const Point& p, const U256& k) {
    Point r0 = infinity(c);
    Point r1 = p;
    for (int bit = 255; bit >= 0; --bit) {
        const Word b = (k.w[static_cast<std::size_t>(bit) / 32] >> (bit % 32)) & 1U;
        swap_if(b, r0, r1);
        r1 = point_add(c, r0, r1);
        r0 = point_add(c, r0, r0);
        swap_if(b, r0, r1);
    }
    wipe(r1.x);
    wipe(r1.y);
    wipe(r1.z);
    return r0;
}

struct Affine {
    U256 x, y;  // plain (not Montgomery) integers below p
    bool infinity = false;
};

Affine to_affine(const Curve& c, const Point& p) {
    Affine out;
    if (is_zero(p.z) != 0) {
        out.infinity = true;
        return out;
    }
    const U256 zinv = mont_pow(c.p, p.z, c.p_minus_two);
    out.x = from_mont(c.p, mont_mul(c.p, p.x, zinv));
    out.y = from_mont(c.p, mont_mul(c.p, p.y, zinv));
    return out;
}

Point from_affine(const Curve& c, const U256& x, const U256& y) {
    return Point{to_mont(c.p, x), to_mont(c.p, y), c.p.one};
}

// x mod n for x < 2^256 < 2n.
U256 reduce_once(const Modulus& f, const U256& x) {
    U256 diff;
    const Word borrow = sub_raw(diff, x, f.m);
    return select(1U ^ borrow, diff, x);
}

bool on_curve(const Curve& c, const U256& x, const U256& y) {
    if (greater_equal(x, c.p.m) != 0 || greater_equal(y, c.p.m) != 0) {
        return false;
    }
    const U256 xm = to_mont(c.p, x);
    const U256 ym = to_mont(c.p, y);
    const U256 y2 = mont_mul(c.p, ym, ym);
    U256 rhs = mont_mul(c.p, mont_mul(c.p, xm, xm), xm);  // x^3
    const U256 three_x = mod_add(c.p, mod_add(c.p, xm, xm), xm);
    rhs = mod_sub(c.p, rhs, three_x);
    rhs = mod_add(c.p, rhs, c.b);
    return equal(y2, rhs);
}

// RFC 6979 §3.2 with HMAC-SHA-256 and a 256-bit curve order, so qlen = hlen = 256 and one HMAC
// output is one candidate.
class Rfc6979 {
   public:
    Rfc6979(const std::array<std::byte, 32>& x, const std::array<std::byte, 32>& h1) {
        v_.fill(std::byte{0x01});
        k_.fill(std::byte{0x00});
        // steps d to g
        update(std::byte{0x00}, &x, &h1);
        update(std::byte{0x01}, &x, &h1);
    }
    ~Rfc6979() {
        wipe_bytes(v_);
        wipe_bytes(k_);
    }
    Rfc6979(const Rfc6979&) = delete;
    Rfc6979& operator=(const Rfc6979&) = delete;

    // The next candidate k in [1, n-1] (step h). The first call is the first candidate; each later
    // call is the continuation after a rejected one.
    U256 next(const Modulus& n) {
        if (started_) {
            update(std::byte{0x00}, nullptr, nullptr);
        }
        started_ = true;
        for (;;) {
            v_ = hmac_sha256(k_, v_);
            U256 candidate = from_big_endian(v_);
            if (is_zero(candidate) == 0 && greater_equal(candidate, n.m) == 0) {
                return candidate;
            }
            update(std::byte{0x00}, nullptr, nullptr);
        }
    }

   private:
    // K = HMAC_K(V || sep || [x || h1]); V = HMAC_K(V)
    void update(std::byte separator, const std::array<std::byte, 32>* x,
                const std::array<std::byte, 32>* h1) {
        std::vector<std::byte> message(v_.begin(), v_.end());
        message.push_back(separator);
        if (x != nullptr) {
            message.insert(message.end(), x->begin(), x->end());
            message.insert(message.end(), h1->begin(), h1->end());
        }
        k_ = hmac_sha256(k_, message);
        wipe_bytes(message);
        v_ = hmac_sha256(k_, v_);
    }

    std::array<std::byte, 32> v_{};
    std::array<std::byte, 32> k_{};
    bool started_ = false;
};

}  // namespace

P256PrivateKey::~P256PrivateKey() {
    wipe_bytes(scalar_);
}

std::optional<P256PrivateKey> P256PrivateKey::from_scalar(
    std::span<const std::byte> scalar_big_endian) {
    if (scalar_big_endian.size() != 32) {
        return std::nullopt;
    }
    std::array<std::byte, 32> raw{};
    for (std::size_t i = 0; i < 32; ++i) {
        raw[i] = scalar_big_endian[i];
    }
    U256 d = from_big_endian(raw);
    const bool ok = is_zero(d) == 0 && greater_equal(d, curve().n.m) == 0;
    wipe(d);
    if (!ok) {
        wipe_bytes(raw);
        return std::nullopt;
    }
    P256PrivateKey key;
    key.scalar_ = raw;
    key.valid_ = true;
    wipe_bytes(raw);
    return key;
}

std::optional<P256PublicKey> p256_public_key(const P256PrivateKey& key) {
    if (!key.valid()) {
        return std::nullopt;
    }
    const Curve& c = curve();
    U256 d = from_big_endian(key.scalar());
    const Affine a = to_affine(c, scalar_mult(c, c.g, d));
    wipe(d);
    if (a.infinity) {
        return std::nullopt;
    }
    P256PublicKey out;
    out.x = to_big_endian(a.x);
    out.y = to_big_endian(a.y);
    return out;
}

bool p256_public_key_valid(const P256PublicKey& key) {
    return on_curve(curve(), from_big_endian(key.x), from_big_endian(key.y));
}

std::optional<P256PublicKey> p256_public_key_from_uncompressed(std::span<const std::byte> encoded) {
    if (encoded.size() != 65 || encoded[0] != std::byte{0x04}) {
        return std::nullopt;
    }
    P256PublicKey key;
    for (std::size_t i = 0; i < 32; ++i) {
        key.x[i] = encoded[1 + i];
        key.y[i] = encoded[33 + i];
    }
    if (!p256_public_key_valid(key)) {
        return std::nullopt;
    }
    return key;
}

std::array<std::byte, 65> p256_public_key_uncompressed(const P256PublicKey& key) {
    std::array<std::byte, 65> out{};
    out[0] = std::byte{0x04};
    for (std::size_t i = 0; i < 32; ++i) {
        out[1 + i] = key.x[i];
        out[33 + i] = key.y[i];
    }
    return out;
}

std::optional<EcdsaSignature> ecdsa_p256_sign(const P256PrivateKey& key,
                                              std::span<const std::byte, 32> digest) {
    if (!key.valid()) {
        return std::nullopt;
    }
    const Curve& c = curve();
    // FIPS 186-4 §6.4.1: e is the leftmost min(256, hash length) bits of the digest as an integer,
    // here the whole digest; reduced once because 2^256 < 2n.
    const U256 e = reduce_once(c.n, from_big_endian(digest));
    std::array<std::byte, 32> h1 = to_big_endian(e);  // bits2octets(h1), RFC 6979 §2.3.4

    std::array<std::byte, 32> x_octets{};
    for (std::size_t i = 0; i < 32; ++i) {
        x_octets[i] = key.scalar()[i];
    }
    Rfc6979 generator(x_octets, h1);
    wipe_bytes(h1);

    U256 d = from_big_endian(key.scalar());
    const U256 dm = to_mont(c.n, d);
    const U256 em = to_mont(c.n, e);
    std::optional<EcdsaSignature> result;
    while (!result.has_value()) {
        U256 k = generator.next(c.n);
        const Affine point = to_affine(c, scalar_mult(c, c.g, k));
        if (point.infinity) {
            wipe(k);
            continue;
        }
        const U256 r = reduce_once(c.n, point.x);  // p < 2n as well
        if (is_zero(r) != 0) {
            wipe(k);
            continue;
        }
        // s = k^-1 (e + r d) mod n, in the Montgomery domain of n.
        U256 km = to_mont(c.n, k);
        U256 kinv = mont_pow(c.n, km, c.n_minus_two);
        const U256 rm = to_mont(c.n, r);
        U256 t = mod_add(c.n, em, mont_mul(c.n, rm, dm));
        const U256 s = from_mont(c.n, mont_mul(c.n, kinv, t));
        wipe(k);
        wipe(km);
        wipe(kinv);
        wipe(t);
        if (is_zero(s) != 0) {
            continue;
        }
        EcdsaSignature sig;
        sig.r = to_big_endian(r);
        sig.s = to_big_endian(s);
        result = sig;
    }
    wipe(d);
    wipe_bytes(x_octets);
    return result;
}

bool ecdsa_p256_verify(const P256PublicKey& key, std::span<const std::byte, 32> digest,
                       const EcdsaSignature& signature) {
    const Curve& c = curve();
    const U256 qx = from_big_endian(key.x);
    const U256 qy = from_big_endian(key.y);
    if (!on_curve(c, qx, qy)) {
        return false;
    }
    const U256 r = from_big_endian(signature.r);
    const U256 s = from_big_endian(signature.s);
    if (is_zero(r) != 0 || is_zero(s) != 0 || greater_equal(r, c.n.m) != 0 ||
        greater_equal(s, c.n.m) != 0) {
        return false;
    }
    const U256 e = reduce_once(c.n, from_big_endian(digest));
    const U256 sinv = mont_pow(c.n, to_mont(c.n, s), c.n_minus_two);
    const U256 u1 = from_mont(c.n, mont_mul(c.n, to_mont(c.n, e), sinv));
    const U256 u2 = from_mont(c.n, mont_mul(c.n, to_mont(c.n, r), sinv));
    const Point sum =
        point_add(c, scalar_mult(c, c.g, u1), scalar_mult(c, from_affine(c, qx, qy), u2));
    const Affine a = to_affine(c, sum);
    if (a.infinity) {
        return false;
    }
    return equal(reduce_once(c.n, a.x), r);
}

namespace {

// One DER INTEGER for an unsigned big-endian value: minimal, with a 0x00 in front where the high
// bit would otherwise make it negative (X.690 §8.3).
void append_der_integer(std::vector<std::byte>& out, const std::array<std::byte, 32>& v) {
    std::size_t first = 0;
    while (first < 31 && v[first] == std::byte{0}) {
        ++first;
    }
    const bool pad = (std::to_integer<unsigned>(v[first]) & 0x80U) != 0;
    out.push_back(std::byte{0x02});
    out.push_back(static_cast<std::byte>(32 - first + (pad ? 1 : 0)));
    if (pad) {
        out.push_back(std::byte{0x00});
    }
    out.insert(out.end(), v.begin() + static_cast<std::ptrdiff_t>(first), v.end());
}

// Reads one strict DER INTEGER into 32 bytes, advancing `pos`.
bool read_der_integer(std::span<const std::byte> der, std::size_t& pos,
                      std::array<std::byte, 32>& out) {
    if (pos + 2 > der.size() || der[pos] != std::byte{0x02}) {
        return false;
    }
    const std::size_t length = std::to_integer<std::size_t>(der[pos + 1]);
    pos += 2;
    if (length == 0 || length > 33 || pos + length > der.size()) {
        return false;
    }
    std::span<const std::byte> body = der.subspan(pos, length);
    pos += length;
    if ((std::to_integer<unsigned>(body[0]) & 0x80U) != 0) {
        return false;  // negative
    }
    if (body.size() > 1 && body[0] == std::byte{0x00} &&
        (std::to_integer<unsigned>(body[1]) & 0x80U) == 0) {
        return false;  // not minimal
    }
    if (body[0] == std::byte{0x00} && body.size() > 1) {
        body = body.subspan(1);
    }
    if (body.size() > 32) {
        return false;
    }
    out.fill(std::byte{0});
    for (std::size_t i = 0; i < body.size(); ++i) {
        out[32 - body.size() + i] = body[i];
    }
    return true;
}

}  // namespace

std::vector<std::byte> ecdsa_signature_to_der(const EcdsaSignature& signature) {
    std::vector<std::byte> body;
    append_der_integer(body, signature.r);
    append_der_integer(body, signature.s);
    std::vector<std::byte> out;
    out.push_back(std::byte{0x30});
    out.push_back(static_cast<std::byte>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<EcdsaSignature> ecdsa_signature_from_der(std::span<const std::byte> der) {
    if (der.size() < 8 || der[0] != std::byte{0x30}) {
        return std::nullopt;
    }
    const std::size_t length = std::to_integer<std::size_t>(der[1]);
    if (length >= 0x80 || 2 + length != der.size()) {
        return std::nullopt;  // long form, or trailing / missing bytes
    }
    EcdsaSignature sig;
    std::size_t pos = 2;
    if (!read_der_integer(der, pos, sig.r) || !read_der_integer(der, pos, sig.s) ||
        pos != der.size()) {
        return std::nullopt;
    }
    const Curve& c = curve();
    const U256 r = from_big_endian(sig.r);
    const U256 s = from_big_endian(sig.s);
    if (is_zero(r) != 0 || is_zero(s) != 0 || greater_equal(r, c.n.m) != 0 ||
        greater_equal(s, c.n.m) != 0) {
        return std::nullopt;
    }
    return sig;
}

}  // namespace iclforge::base::crypto
