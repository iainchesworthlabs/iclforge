#include "crypto_clock.hpp"

#include "soc/hp_sys_clkrst_reg.h"
#include "soc/soc.h"

namespace bench {

int crypto_clock_selector() {
    return static_cast<int>(
        REG_GET_FIELD(HP_SYS_CLKRST_PERI_CLK_CTRL25_REG, HP_SYS_CLKRST_REG_CRYPTO_CLK_SRC_SEL));
}

bool crypto_clock_use_crystal(bool crystal) {
    // Read before the first write, which is the value ESP-IDF set at startup.
    static const int at_boot = crypto_clock_selector();
    REG_SET_FIELD(HP_SYS_CLKRST_PERI_CLK_CTRL25_REG, HP_SYS_CLKRST_REG_CRYPTO_CLK_SRC_SEL,
                  crystal ? 0 : at_boot);
    return true;
}

}  // namespace bench
