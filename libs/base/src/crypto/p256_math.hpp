#pragma once

// NIST P-256 (secp256r1) arithmetic, private to iclforge::base's crypto code.
//
// Parameters: SEC 2 v2 section 2.4.2 and FIPS 186-4 D.1.2.3, the same values as the AC-4 content
// signing specification's section 2.2 table. They are written once, below, and
// libs/base/tests/test_p256.cpp compares them with that table.
//
// Everything here is written to take the same time and touch the same memory whatever the values
// are, because the scalar of a signature is the operator's private key (or the nonce that would
// reveal it):
//
//   - 256-bit values are eight 32-bit limbs, little endian, with 64-bit intermediate products. No
//     compiler extension (__int128) is used, so the code builds on MSVC and on 32-bit targets.
//   - Modular multiplication is Montgomery multiplication (CIOS, Koc, Acar and Kaliski 1996),
//     with a final subtraction selected by a mask. One implementation serves the field prime p and
//     the group order n.
//   - Inversion is Fermat exponentiation. The exponent (m - 2) is public, so the sequence of
//     operations does not depend on the value being inverted.
//   - Points are Jacobian. Addition computes the general sum and the doubling and selects between
//     them by masks, so the exceptional cases (either input at infinity, equal inputs) take no
//     branch. Opposite inputs give Z = 0 from the formula itself.
//   - Scalar multiplication uses a fixed 4-bit window and reads the whole table for every lookup.
//
// What is not claimed: the property is by construction and is not machine-verified, and an
// optimiser is free in principle to turn a mask select into a branch. libs/base/tests/
// test_p256.cpp runs signing under valgrind where that is available (see the design record,
// planning/ac4-content-signing.md).

#include <array>
#include <cstddef>
#include <cstdint>

namespace iclforge::base::crypto::detail {

using Limbs = std::array<std::uint32_t, 8>;  // little-endian 32-bit limbs

// 1 when v is not zero, else 0, without a branch.
[[nodiscard]] constexpr std::uint32_t nonzero_bit(std::uint32_t v) noexcept {
    return (v | (0u - v)) >> 31;
}

// All ones when v is zero, else all zeros.
[[nodiscard]] constexpr std::uint32_t zero_mask(std::uint32_t v) noexcept {
    return nonzero_bit(v) - 1u;
}

[[nodiscard]] constexpr std::uint32_t is_zero_mask(const Limbs& a) noexcept {
    std::uint32_t any = 0;
    for (const std::uint32_t limb : a) {
        any |= limb;
    }
    return zero_mask(any);
}

[[nodiscard]] constexpr std::uint32_t equal_mask(const Limbs& a, const Limbs& b) noexcept {
    std::uint32_t any = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        any |= a[i] ^ b[i];
    }
    return zero_mask(any);
}

// `a` where mask is all ones, `b` where it is zero.
[[nodiscard]] constexpr Limbs select(std::uint32_t mask, const Limbs& a, const Limbs& b) noexcept {
    Limbs out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = (a[i] & mask) | (b[i] & ~mask);
    }
    return out;
}

// out = a + b; returns the carry out of the top limb (0 or 1).
constexpr std::uint32_t add_with_carry(Limbs& out, const Limbs& a, const Limbs& b) noexcept {
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint64_t sum = std::uint64_t{a[i]} + b[i] + carry;
        out[i] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
    }
    return static_cast<std::uint32_t>(carry);
}

// out = a - b; returns the borrow out of the top limb (0 or 1).
constexpr std::uint32_t sub_with_borrow(Limbs& out, const Limbs& a, const Limbs& b) noexcept {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint64_t diff = std::uint64_t{a[i]} - b[i] - borrow;
        out[i] = static_cast<std::uint32_t>(diff);
        borrow = (diff >> 32) & 1u;
    }
    return static_cast<std::uint32_t>(borrow);
}

// (a + b) mod m for a, b < m.
[[nodiscard]] constexpr Limbs mod_add(const Limbs& a, const Limbs& b, const Limbs& m) noexcept {
    Limbs sum{};
    const std::uint32_t carry = add_with_carry(sum, a, b);
    Limbs diff{};
    const std::uint32_t borrow = sub_with_borrow(diff, sum, m);
    // The sum is reduced when it overflowed 256 bits (then the subtraction wrapped back to the
    // right value) or is at least m (no borrow).
    const std::uint32_t reduce = carry | (borrow ^ 1u);
    return select(0u - reduce, diff, sum);
}

// (a - b) mod m for a, b < m.
[[nodiscard]] constexpr Limbs mod_sub(const Limbs& a, const Limbs& b, const Limbs& m) noexcept {
    Limbs diff{};
    const std::uint32_t borrow = sub_with_borrow(diff, a, b);
    Limbs wrapped{};
    add_with_carry(wrapped, diff, m);
    return select(0u - borrow, wrapped, diff);
}

// A modulus with the constants Montgomery multiplication needs, for R = 2^256.
struct Modulus {
    Limbs m{};
    Limbs one{};               // R mod m, the Montgomery form of 1
    Limbs r2{};                // R^2 mod m, which turns a plain value into Montgomery form
    std::uint32_t n0 = 0;      // -m^-1 mod 2^32
    Limbs exponent_inverse{};  // m - 2, the Fermat exponent
};

// Both p and n of P-256 exceed 2^255, so R mod m is 2^256 - m, and the constants are computed here
// rather than typed in.
[[nodiscard]] constexpr Modulus make_modulus(const Limbs& m) noexcept {
    Modulus mod{};
    mod.m = m;
    // Newton's iteration for the inverse of an odd m[0] modulo 2^32: the starting value m[0] is
    // right to three bits and each step doubles that.
    std::uint32_t inverse = m[0];
    for (int i = 0; i < 5; ++i) {
        inverse *= 2u - m[0] * inverse;
    }
    mod.n0 = 0u - inverse;
    const Limbs zero{};
    sub_with_borrow(mod.one, zero, m);
    Limbs x = mod.one;
    for (int i = 0; i < 256; ++i) {
        x = mod_add(x, x, m);
    }
    mod.r2 = x;
    const Limbs two = {2, 0, 0, 0, 0, 0, 0, 0};
    sub_with_borrow(mod.exponent_inverse, m, two);
    return mod;
}

// a * b / R mod m, for a, b < m.
[[nodiscard]] constexpr Limbs mont_mul(const Limbs& a, const Limbs& b,
                                       const Modulus& mod) noexcept {
    std::array<std::uint32_t, 10> t{};
    for (std::size_t i = 0; i < 8; ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < 8; ++j) {
            const std::uint64_t cur = std::uint64_t{a[j]} * b[i] + t[j] + carry;
            t[j] = static_cast<std::uint32_t>(cur);
            carry = cur >> 32;
        }
        std::uint64_t cur = std::uint64_t{t[8]} + carry;
        t[8] = static_cast<std::uint32_t>(cur);
        t[9] = static_cast<std::uint32_t>(cur >> 32);

        const std::uint32_t q = t[0] * mod.n0;
        cur = std::uint64_t{q} * mod.m[0] + t[0];
        carry = cur >> 32;
        for (std::size_t j = 1; j < 8; ++j) {
            cur = std::uint64_t{q} * mod.m[j] + t[j] + carry;
            t[j - 1] = static_cast<std::uint32_t>(cur);
            carry = cur >> 32;
        }
        cur = std::uint64_t{t[8]} + carry;
        t[7] = static_cast<std::uint32_t>(cur);
        t[8] = t[9] + static_cast<std::uint32_t>(cur >> 32);
    }
    Limbs value{};
    for (std::size_t i = 0; i < 8; ++i) {
        value[i] = t[i];
    }
    Limbs diff{};
    const std::uint32_t borrow = sub_with_borrow(diff, value, mod.m);
    // The result is below 2m: subtract m when t[8] holds a ninth limb or the value is at least m.
    const std::uint32_t reduce = (t[8] & 1u) | (borrow ^ 1u);
    return select(0u - reduce, diff, value);
}

[[nodiscard]] constexpr Limbs to_mont(const Limbs& a, const Modulus& mod) noexcept {
    return mont_mul(a, mod.r2, mod);
}

[[nodiscard]] constexpr Limbs from_mont(const Limbs& a, const Modulus& mod) noexcept {
    const Limbs plain_one = {1, 0, 0, 0, 0, 0, 0, 0};
    return mont_mul(a, plain_one, mod);
}

// base^(m-2), all in Montgomery form: the inverse, and 0 for 0. The exponent is public.
[[nodiscard]] constexpr Limbs mont_inverse(const Limbs& a, const Modulus& mod) noexcept {
    Limbs result = mod.one;
    for (int bit = 255; bit >= 0; --bit) {
        result = mont_mul(result, result, mod);
        const std::size_t limb = static_cast<std::size_t>(bit / 32);
        if (((mod.exponent_inverse[limb] >> (bit % 32)) & 1u) != 0) {
            result = mont_mul(result, a, mod);
        }
    }
    return result;
}

// 64 hexadecimal digits, most significant first, to limbs.
[[nodiscard]] consteval Limbs limbs_from_hex(const char (&text)[65]) {
    Limbs out{};
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = text[i];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint32_t>(c - 'a') + 10u;
        } else {
            digit = static_cast<std::uint32_t>(c - 'A') + 10u;
        }
        const std::size_t nibble = 63 - i;  // position from the least significant end
        out[nibble / 8] |= digit << (4 * (nibble % 8));
    }
    return out;
}

// --- The curve (spec section 2.2) ---------------------------------------------------------

inline constexpr Limbs kPrimeLimbs =
    limbs_from_hex("FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF");
inline constexpr Limbs kOrderLimbs =
    limbs_from_hex("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551");
inline constexpr Limbs kCurveBLimbs =
    limbs_from_hex("5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B");
inline constexpr Limbs kGeneratorXLimbs =
    limbs_from_hex("6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296");
inline constexpr Limbs kGeneratorYLimbs =
    limbs_from_hex("4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5");

inline constexpr Modulus kField = make_modulus(kPrimeLimbs);  // arithmetic mod p
inline constexpr Modulus kOrder = make_modulus(kOrderLimbs);  // arithmetic mod n

inline constexpr Limbs kCurveB = to_mont(kCurveBLimbs, kField);

// --- Field helpers (Montgomery form) -------------------------------------------------------

[[nodiscard]] inline Limbs fe_add(const Limbs& a, const Limbs& b) noexcept {
    return mod_add(a, b, kField.m);
}
[[nodiscard]] inline Limbs fe_sub(const Limbs& a, const Limbs& b) noexcept {
    return mod_sub(a, b, kField.m);
}
[[nodiscard]] inline Limbs fe_mul(const Limbs& a, const Limbs& b) noexcept {
    return mont_mul(a, b, kField);
}
[[nodiscard]] inline Limbs fe_sqr(const Limbs& a) noexcept {
    return mont_mul(a, a, kField);
}

// --- Points ---------------------------------------------------------------------------------

// Jacobian coordinates (X : Y : Z) in Montgomery form, the affine point (X/Z^2, Y/Z^3). Z = 0 is
// the point at infinity, whatever X and Y hold.
struct Jacobian {
    Limbs x{};
    Limbs y{};
    Limbs z{};
};

[[nodiscard]] inline Jacobian infinity_point() noexcept {
    return Jacobian{kField.one, kField.one, Limbs{}};
}

[[nodiscard]] inline Jacobian select_point(std::uint32_t mask, const Jacobian& a,
                                           const Jacobian& b) noexcept {
    return Jacobian{select(mask, a.x, b.x), select(mask, a.y, b.y), select(mask, a.z, b.z)};
}

// "dbl-2001-b" for a = -3 (Bernstein and Lange, Explicit-Formulas Database). The Z of a doubled
// point at infinity is 2YZ = 0, so infinity doubles to infinity without a case.
[[nodiscard]] inline Jacobian point_double(const Jacobian& p) noexcept {
    const Limbs delta = fe_sqr(p.z);
    const Limbs gamma = fe_sqr(p.y);
    const Limbs beta = fe_mul(p.x, gamma);
    const Limbs t = fe_mul(fe_sub(p.x, delta), fe_add(p.x, delta));
    const Limbs alpha = fe_add(fe_add(t, t), t);
    const Limbs beta2 = fe_add(beta, beta);
    const Limbs beta4 = fe_add(beta2, beta2);
    const Limbs beta8 = fe_add(beta4, beta4);
    Jacobian out;
    out.x = fe_sub(fe_sqr(alpha), beta8);
    out.z = fe_sub(fe_sub(fe_sqr(fe_add(p.y, p.z)), gamma), delta);
    const Limbs gamma2 = fe_sqr(gamma);
    const Limbs gamma2x2 = fe_add(gamma2, gamma2);
    const Limbs gamma2x4 = fe_add(gamma2x2, gamma2x2);
    const Limbs gamma2x8 = fe_add(gamma2x4, gamma2x4);
    out.y = fe_sub(fe_mul(alpha, fe_sub(beta4, out.x)), gamma2x8);
    return out;
}

// The general sum (the textbook Jacobian addition), with the cases it does not cover selected by
// mask: p at infinity gives q, q at infinity gives p, equal points give the doubling. Opposite
// points give H = 0 and so Z3 = 0, the point at infinity, without a case.
[[nodiscard]] inline Jacobian point_add(const Jacobian& p, const Jacobian& q) noexcept {
    const Limbs z1z1 = fe_sqr(p.z);
    const Limbs z2z2 = fe_sqr(q.z);
    const Limbs u1 = fe_mul(p.x, z2z2);
    const Limbs u2 = fe_mul(q.x, z1z1);
    const Limbs s1 = fe_mul(fe_mul(p.y, q.z), z2z2);
    const Limbs s2 = fe_mul(fe_mul(q.y, p.z), z1z1);
    const Limbs h = fe_sub(u2, u1);
    const Limbs r = fe_sub(s2, s1);
    const Limbs hh = fe_sqr(h);
    const Limbs hhh = fe_mul(h, hh);
    const Limbs v = fe_mul(u1, hh);
    Jacobian sum;
    sum.x = fe_sub(fe_sub(fe_sqr(r), hhh), fe_add(v, v));
    sum.y = fe_sub(fe_mul(r, fe_sub(v, sum.x)), fe_mul(s1, hhh));
    sum.z = fe_mul(fe_mul(p.z, q.z), h);

    const std::uint32_t p_infinite = is_zero_mask(p.z);
    const std::uint32_t q_infinite = is_zero_mask(q.z);
    const std::uint32_t same = is_zero_mask(h) & is_zero_mask(r) & ~p_infinite & ~q_infinite;
    Jacobian out = select_point(same, point_double(p), sum);
    out = select_point(p_infinite, q, out);
    out = select_point(q_infinite, p, out);
    return out;
}

// k * p for a scalar of eight limbs. Fixed 4-bit windows from the top; the table is read in full
// for every window so that no memory index depends on k.
[[nodiscard]] inline Jacobian scalar_multiply(const Limbs& k, const Jacobian& p) noexcept {
    std::array<Jacobian, 16> table;
    table[0] = infinity_point();
    table[1] = p;
    for (std::size_t i = 2; i < table.size(); ++i) {
        table[i] = (i % 2 == 0) ? point_double(table[i / 2]) : point_add(table[i - 1], p);
    }
    Jacobian acc = infinity_point();
    for (int window = 63; window >= 0; --window) {
        for (int i = 0; i < 4; ++i) {
            acc = point_double(acc);
        }
        const std::size_t limb = static_cast<std::size_t>(window / 8);
        const std::uint32_t nibble = (k[limb] >> (4 * (window % 8))) & 0xFu;
        Jacobian chosen = table[0];
        for (std::uint32_t i = 1; i < 16; ++i) {
            const std::uint32_t hit = zero_mask(i ^ nibble);
            chosen = select_point(hit, table[i], chosen);
        }
        acc = point_add(acc, chosen);
    }
    return acc;
}

// The affine coordinates, out of Montgomery form. False for the point at infinity.
[[nodiscard]] inline bool to_affine(const Jacobian& p, Limbs& x, Limbs& y) noexcept {
    if (is_zero_mask(p.z) != 0) {
        return false;
    }
    const Limbs z_inverse = mont_inverse(p.z, kField);
    const Limbs z_inverse2 = fe_sqr(z_inverse);
    const Limbs z_inverse3 = fe_mul(z_inverse2, z_inverse);
    x = from_mont(fe_mul(p.x, z_inverse2), kField);
    y = from_mont(fe_mul(p.y, z_inverse3), kField);
    return true;
}

[[nodiscard]] inline Jacobian from_affine(const Limbs& x, const Limbs& y) noexcept {
    return Jacobian{to_mont(x, kField), to_mont(y, kField), kField.one};
}

[[nodiscard]] inline Jacobian generator() noexcept {
    return from_affine(kGeneratorXLimbs, kGeneratorYLimbs);
}

// y^2 = x^3 - 3x + b (mod p), for coordinates already below p.
[[nodiscard]] inline bool on_curve(const Limbs& x, const Limbs& y) noexcept {
    const Limbs xm = to_mont(x, kField);
    const Limbs ym = to_mont(y, kField);
    const Limbs x2 = fe_sqr(xm);
    const Limbs x3 = fe_mul(x2, xm);
    const Limbs three_x = fe_add(fe_add(xm, xm), xm);
    const Limbs rhs = fe_add(fe_sub(x3, three_x), kCurveB);
    return equal_mask(fe_sqr(ym), rhs) != 0;
}

// --- Byte conversions -----------------------------------------------------------------------

[[nodiscard]] inline Limbs limbs_from_bytes(const std::byte* big_endian32) noexcept {
    Limbs out{};
    for (std::size_t i = 0; i < 8; ++i) {
        const std::size_t offset = (7 - i) * 4;
        out[i] = (std::uint32_t{std::to_integer<std::uint8_t>(big_endian32[offset])} << 24) |
                 (std::uint32_t{std::to_integer<std::uint8_t>(big_endian32[offset + 1])} << 16) |
                 (std::uint32_t{std::to_integer<std::uint8_t>(big_endian32[offset + 2])} << 8) |
                 std::uint32_t{std::to_integer<std::uint8_t>(big_endian32[offset + 3])};
    }
    return out;
}

inline void limbs_to_bytes(const Limbs& limbs, std::byte* big_endian32) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        const std::size_t offset = (7 - i) * 4;
        big_endian32[offset] = static_cast<std::byte>(limbs[i] >> 24);
        big_endian32[offset + 1] = static_cast<std::byte>(limbs[i] >> 16);
        big_endian32[offset + 2] = static_cast<std::byte>(limbs[i] >> 8);
        big_endian32[offset + 3] = static_cast<std::byte>(limbs[i]);
    }
}

// 1 when a < b as 256-bit integers, else 0.
[[nodiscard]] inline std::uint32_t less_than(const Limbs& a, const Limbs& b) noexcept {
    Limbs scratch{};
    return sub_with_borrow(scratch, a, b);
}

}  // namespace iclforge::base::crypto::detail
