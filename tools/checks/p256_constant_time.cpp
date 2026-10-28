// The program tools/checks/check_p256_constant_time.py runs under valgrind: ECDSA signing and
// public key derivation with the private scalar marked as undefined memory.
//
// This is the technique usually called ctgrind. Memcheck tracks, bit by bit, which values derive
// from undefined memory, and reports a conditional jump or a memory address that depends on one. If
// the private scalar is the undefined memory, a report is a branch or a table index that depends on
// the secret, which is what constant-time code does not have. It runs on the machine code the
// compiler produced, so it also catches the case where a branch-free source is compiled to a
// branch.
//
// What it does not do: it sees only the executions it is given (one key, one digest), it does not
// see variable-time machine instructions (a multiply or divide whose time depends on its operands),
// and it says nothing about caches or power. See libs/base/src/crypto/p256_math.hpp for what is
// claimed.

#include <valgrind/memcheck.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/sha256.hpp"

int main() {
    using namespace iclforge::base::crypto;

    // An arbitrary scalar well inside 1..n-1; not a key anyone uses.
    std::array<std::byte, 32> scalar{};
    for (std::size_t i = 0; i < scalar.size(); ++i) {
        scalar[i] = static_cast<std::byte>(0x11 * (i + 3));
    }
    scalar[0] = std::byte{0x7A};
    auto key = P256PrivateKey::from_scalar(scalar);
    if (!key.has_value()) {
        std::puts("the test scalar is not a valid key");
        return 2;
    }
    const Digest256 digest = sha256(std::vector<std::byte>{std::byte{'a'}});

    // Everything derived from these 32 bytes from here on is secret as far as memcheck is
    // concerned.
    VALGRIND_MAKE_MEM_UNDEFINED(key->scalar().data(), key->scalar().size());

    auto signature = ecdsa_sign(*key, digest);
    // The signature and the public key are public outputs. Declassifying them keeps the printing
    // below from being reported, and does not hide anything computed before this point.
    VALGRIND_MAKE_MEM_DEFINED(&signature, sizeof signature);
    auto public_key = key->public_key();
    VALGRIND_MAKE_MEM_DEFINED(&public_key, sizeof public_key);

    std::printf("signature %s, public key %s\n", signature.has_value() ? "made" : "NOT made",
                public_key.has_value() ? "derived" : "NOT derived");
    return signature.has_value() && public_key.has_value() ? 0 : 1;
}
