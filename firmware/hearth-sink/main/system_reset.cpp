// Restarting a board whose Wi-Fi is behind esp_hosted (the ESP32-P4's C6, over SDIO). See
// system_reset.hpp.
//
// THE FAULT. esp_restart() on the P4 ends in a reset of the CPU core alone (ESP-IDF's
// esp_restart_noos: `rst:0xc (SW_CPU_RESET)` in the ROM's line), after resetting a chosen list of
// modules. Sometimes the board that comes back has no Wi-Fi, until a USB flash:
//
//     H_SDIO_DRV: Reset slave using GPIO[54]
//     H_SDIO_DRV: Card init success, TRANSPORT_RX_ACTIVE
//     E (4300) SD_TRANS: sd_host_wait_for_event returned 0x107
//     E (6800) H_SDIO_DRV: sdio_read_task: Failed to read data - -1 838860 838860
//     I (13070) transport: Not able to connect with ESP-Hosted slave device
//     E (14620) H_SDIO_DRV: failed to read registers
//     I (14620) H_SDIO_DRV: Host is resetting itself, to avoid any sdio race condition
//
// The card answers and its data never arrives, and every boot after that does the same, whatever
// restarts the board. 2026-10-12, P4a and P4b: 2 of 10 updates over the network; 8 of 16 of a USB
// flash, twenty seconds and an update; none of 30 `POST /restart`s of a board that had not just
// been updated, whether idle, playing or flooded with traffic.
//
// WHAT CLEARS IT. A reset of the whole system. `esptool --after watchdog-reset`, nothing written,
// brought a dead board back in 11 s; a USB flash did so only by the watchdog reset at its end. A
// CPU reset does not, nor does esp_hosted's own reset of the C6 at every boot (a 10 ms pulse on
// GPIO 54, not configurable in 2.13.0). Everything else tried, on a board that was in it:
//
//   - GPIO 54 held low for two seconds over JTAG with the P4 halted, then a CPU reset of the P4
//     and nothing else: back in nine seconds, twice on each board. So the C6 needed a longer reset
//     than the pulse, and holding the pin low through the CPU reset (the pin stays driven through
//     it, and a board on which it is not driven pulls it low) gives it one.
//   - The same held pin from a shutdown handler of the firmware's own, in an image that did the
//     restart: no fault in 10 of 10 cycles of the protocol that faulted 8 of 16, but a board that
//     was already dead was not brought back by it, in 2 of 2 tries on two boards, where the
//     system reset below brought back 8 of 8. Why a held pin cures from JTAG and not from here is
//     not established. What is established is that the system reset does both.
//
// THE REMEDY is what esptool does: let the RTC watchdog reset the system. It is armed here as
// esp_restart_noos arms it for its own safety net - one second, then a reset of the RTC as well,
// and the flash boot that follows protected - and allowed to expire, from the last shutdown
// handler esp_restart() runs. 24 of 24 updates over the network were clean, 8 of them on the
// protocol that faulted 8 of 16, and a board that came back dead (the update's restart was done by
// an image without this) was healed in 8 of 8: esp_hosted restarts the host when the link fails
// to come up, about fifteen seconds in, and that restart now resets the system.
//
// Three requests for a system reset that look the same and are not, each tried on a board and each
// leaving it dead until a USB-JTAG reset:
//
//   - esp_rom_software_reset_system(), "digital core include RTC": both cores parked in the ROM
//     (pc 0x4fc00b18 and 0x4fc0283e).
//   - a stage armed for 20 ms: its timeout lives in the RTC domain, survives the reset, and
//     expires again before the ROM has reached the bootloader, so the board resets for ever. One
//     second is ESP-IDF's.
//   - a core stalled with esp_cpu_stall(): the stall is a flag in the RTC domain that a system
//     reset does not clear, and the ROM parks the same way. esp_restart_noos_dig() releases it for
//     that reason. This does not stall the other core.
//
// The cost: a restart takes a second longer, and the USB-Serial-JTAG port re-enumerates (the COM
// port is gone for a second or two) where a CPU reset kept it. A panic is not an esp_restart() and
// still resets the CPU alone.

#include "system_reset.hpp"

#include <unistd.h>
#include <cstdint>
#include <cstdio>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"

// Defined only by a build that links esp_hosted (main/idf_component.yml: the ESP32-P4 and the
// ESP32-H2), and so the one question that asks "is this board's Wi-Fi behind a companion chip"
// without a platform branch in the source.
extern "C" int esp_hosted_init(void) __attribute__((weak));

namespace player {
namespace {

void flush_console() {
    std::fflush(stdout);
    (void)fsync(fileno(stdout));
}

// Arms the RTC watchdog and waits for it. Does not return.
[[noreturn]] void reset_by_rtc_watchdog() {
    flush_console();
    // The scheduler stops on this core, so nothing else on it runs into the second. The other core
    // is not stalled: see above.
    vTaskSuspendAll();
    wdt_hal_context_t rtc_wdt;
    wdt_hal_init(&rtc_wdt, WDT_RWDT, 0, false);
    const std::uint32_t one_second = static_cast<std::uint32_t>(rtc_clk_slow_freq_get_hz());
    wdt_hal_write_protect_disable(&rtc_wdt);
    wdt_hal_config_stage(&rtc_wdt, WDT_STAGE0, one_second, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_config_stage(&rtc_wdt, WDT_STAGE1, one_second, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&rtc_wdt, true);
    wdt_hal_enable(&rtc_wdt);
    wdt_hal_feed(&rtc_wdt);
    wdt_hal_write_protect_enable(&rtc_wdt);
    for (;;) {
    }
}

void on_restart() {
    std::printf("restart: resetting the system through the RTC watchdog\n");
    reset_by_rtc_watchdog();
}

}  // namespace

void restart_through_system_reset() {
    if (esp_hosted_init == nullptr) {
        return;
    }
    if (esp_register_shutdown_handler(&on_restart) != ESP_OK) {
        std::printf(
            "warning: could not register the restart that resets the whole system; a restart "
            "resets the CPU alone, and may leave the Wi-Fi link dead\n");
    }
}

}  // namespace player
