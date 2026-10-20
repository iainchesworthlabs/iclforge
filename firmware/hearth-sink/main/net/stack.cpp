#include "stack.hpp"

#include <mutex>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

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
    started = true;
}

}  // namespace player::net
