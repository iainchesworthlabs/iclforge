#include "psram_scope.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The task whose allocations a PsramScope sends to PSRAM, or none.
namespace {

std::atomic<TaskHandle_t> g_scoped_task{nullptr};

[[nodiscard]] void* take(std::size_t size) noexcept {
    if (size == 0) {
        size = 1;
    }
    const TaskHandle_t scoped = g_scoped_task.load(std::memory_order_relaxed);
    if (scoped != nullptr && scoped == xTaskGetCurrentTaskHandle()) {
        if (void* const block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
            return block;
        }
    }
    return std::malloc(size);
}

}  // namespace

namespace iclforge::player {

PsramScope::PsramScope() : previous_(g_scoped_task.exchange(xTaskGetCurrentTaskHandle())) {}

PsramScope::~PsramScope() {
    g_scoped_task.store(static_cast<TaskHandle_t>(previous_));
}

}  // namespace iclforge::player

// The replaceable global allocation functions, as libstdc++ has them without exceptions: an
// allocation that cannot be made aborts (the board restarts and says where), and the nothrow forms
// answer null. The aligned forms are left to libstdc++, which pairs its own new and delete.
void* operator new(std::size_t size) {
    void* const block = take(size);
    if (block == nullptr) {
        std::abort();
    }
    return block;
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return take(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return take(size);
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete[](void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

void operator delete[](void* block, std::size_t) noexcept {
    std::free(block);
}

void operator delete(void* block, const std::nothrow_t&) noexcept {
    std::free(block);
}

void operator delete[](void* block, const std::nothrow_t&) noexcept {
    std::free(block);
}
