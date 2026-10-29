#include "crypto_clock.hpp"

namespace bench {

int crypto_clock_selector() {
    return -1;
}

bool crypto_clock_use_crystal(bool /*crystal*/) {
    return false;
}

}  // namespace bench
