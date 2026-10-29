#pragma once

// The clock the chip's crypto peripherals (SHA, AES, ...) run from, on the one chip family where it
// is selectable. CMake picks crypto_clock_esp32p4.cpp or crypto_clock_none.cpp, so no source file
// asks which chip it is built for.

namespace bench {

// The selector's raw value, or -1 on a chip without one. On the ESP32-P4 ESP-IDF's startup sets it to
// 2, the 240 MHz PLL (esp_security/src/esp32p4/esp_crypto_clk.h); the register's reset value is 0.
int crypto_clock_selector();

// Selects the crystal (true) or puts back what the selector held the first time this was called
// (false). Returns false, doing nothing, on a chip without the selector.
bool crypto_clock_use_crystal(bool crystal);

}  // namespace bench
