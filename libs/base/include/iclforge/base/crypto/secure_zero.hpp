#pragma once

// Scrubbing memory that held a secret.
//
// A plain std::fill or memset on a buffer that is about to be freed or to go out of scope is a
// dead store the optimiser may remove, which leaves the private key, the ECDSA nonce or the PEM
// text it was decoded from in freed heap or in a stack slot the next call reuses. Writes through a
// volatile pointer may not be elided, which is the guarantee wanted here. The same technique
// SigningKey (signing_key.hpp) uses for its bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iclforge::base::crypto {

inline void secure_zero(void* data, std::size_t size) noexcept {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

template <typename T, std::size_t N>
void secure_zero(std::array<T, N>& array) noexcept {
    secure_zero(array.data(), sizeof(T) * N);
}

template <typename T>
void secure_zero(std::vector<T>& vector) noexcept {
    secure_zero(vector.data(), sizeof(T) * vector.size());
}

inline void secure_zero(std::string& text) noexcept {
    secure_zero(text.data(), text.size());
}

}  // namespace iclforge::base::crypto
