#include "stack.hpp"

#include <mutex>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "sdkconfig.h"

namespace player::net {

void stack_start() {
    static std::mutex mutex;
    static bool started = false;
    const std::lock_guard lock(mutex);
    if (started) {
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_LWIP_DHCP_GET_NTP_SRV
    // The time servers a router gives (DHCP option 42) are kept by lwIP when a lease is bound, so the clock
    // view of a panel (panel/lvgl/wall_clock.cpp) finds them whenever it is chosen. This is a flag in lwIP
    // and contacts nobody: nothing asks a time server until that view is the chosen one.
    esp_sntp_servermode_dhcp(true);
#endif
    started = true;
}

}  // namespace player::net
