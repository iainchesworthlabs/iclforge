// The host build of the component's player code: stand-ins for the part's kernel (threads, a
// message buffer, event groups), heap and clock, enough for burst_player.cpp to run as it does on a
// board, one decode task fed through its ring. See host_control.hpp for what a test controls.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"
#include "host_control.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// --- the clock ---------------------------------------------------------------------------------

std::atomic<bool> g_virtual_clock{false};
std::atomic<std::int64_t> g_virtual_now{0};
const Clock::time_point g_epoch = Clock::now();

// --- the heap ----------------------------------------------------------------------------------

std::mutex g_heap_mutex;
std::map<void*, fwhost::Allocation> g_live;
std::map<std::uint32_t, std::size_t> g_held;
std::map<std::uint32_t, std::size_t> g_peak;
std::atomic<bool> g_psram{true};

constexpr std::uint32_t kTracked[] = {MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL};

void note(void* memory, std::size_t bytes, std::uint32_t caps) {
    const std::lock_guard lock(g_heap_mutex);
    g_live[memory] = fwhost::Allocation{bytes, caps};
    for (const std::uint32_t tracked : kTracked) {
        if ((caps & tracked) != 0) {
            g_held[tracked] += bytes;
            g_peak[tracked] = std::max(g_peak[tracked], g_held[tracked]);
        }
    }
}

// --- the message buffer: a queue of messages ---------------------------------------------------

struct Queue {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::vector<std::uint8_t>> messages;
    std::size_t capacity = 0;
    std::size_t used = 0;
    // The receiver is blocked in xMessageBufferReceive with nothing to take.
    bool waiting = false;
};

std::atomic<Queue*> g_queue{nullptr};

constexpr std::size_t kMessageOverhead = 4;

// --- the tasks ---------------------------------------------------------------------------------

struct TaskExit {};

struct Task {
    std::thread thread;
};

struct EventGroup {
    std::mutex mutex;
    std::condition_variable changed;
    EventBits_t bits = 0;
};

}  // namespace

// --- esp_timer, heap_caps ----------------------------------------------------------------------

int64_t esp_timer_get_time() {
    if (g_virtual_clock.load()) {
        return g_virtual_now.load();
    }
    return 1'000'000 + std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_epoch).count();
}

void* heap_caps_malloc(std::size_t size, std::uint32_t caps) {
    void* memory = std::malloc(size);
    if (memory != nullptr) {
        note(memory, size, caps);
    }
    return memory;
}

void* heap_caps_calloc(std::size_t count, std::size_t size, std::uint32_t caps) {
    void* memory = std::calloc(count, size);
    if (memory != nullptr) {
        note(memory, count * size, caps);
    }
    return memory;
}

void heap_caps_free(void* memory) {
    if (memory == nullptr) {
        return;
    }
    {
        const std::lock_guard lock(g_heap_mutex);
        const auto found = g_live.find(memory);
        if (found != g_live.end()) {
            for (const std::uint32_t tracked : kTracked) {
                if ((found->second.caps & tracked) != 0) {
                    g_held[tracked] -= found->second.bytes;
                }
            }
            g_live.erase(found);
        }
    }
    std::free(memory);
}

std::size_t heap_caps_get_total_size(std::uint32_t caps) {
    if ((caps & MALLOC_CAP_SPIRAM) != 0) {
        return g_psram.load() ? std::size_t{8} * 1024 * 1024 : 0;
    }
    return std::size_t{300} * 1024;
}

std::size_t heap_caps_get_free_size(std::uint32_t /*caps*/) {
    return std::size_t{8} * 1024 * 1024;
}

std::size_t heap_caps_get_largest_free_block(std::uint32_t /*caps*/) {
    return std::size_t{4} * 1024 * 1024;
}

std::size_t heap_caps_get_minimum_free_size(std::uint32_t /*caps*/) {
    return std::size_t{4} * 1024 * 1024;
}

int heap_caps_monitor_local_minimum_free_size_start() {
    return 0;
}

int heap_caps_monitor_local_minimum_free_size_stop() {
    return 0;
}

// --- message buffer ----------------------------------------------------------------------------

MessageBufferHandle_t xMessageBufferCreateStatic(std::size_t bytes, std::uint8_t* /*storage*/,
                                                 StaticMessageBuffer_t* control) {
    auto* queue = new Queue;
    queue->capacity = bytes;
    control->queue = queue;
    g_queue.store(queue);
    return queue;
}

void vMessageBufferDelete(MessageBufferHandle_t buffer) {
    auto* queue = static_cast<Queue*>(buffer);
    Queue* expected = queue;
    g_queue.compare_exchange_strong(expected, nullptr);
    delete queue;
}

std::size_t xMessageBufferSpacesAvailable(MessageBufferHandle_t buffer) {
    auto* queue = static_cast<Queue*>(buffer);
    const std::lock_guard lock(queue->mutex);
    return queue->capacity > queue->used ? queue->capacity - queue->used : 0;
}

std::size_t xMessageBufferSend(MessageBufferHandle_t buffer, const void* data, std::size_t bytes,
                               TickType_t /*ticks*/) {
    auto* queue = static_cast<Queue*>(buffer);
    {
        const std::lock_guard lock(queue->mutex);
        if (queue->used + bytes + kMessageOverhead > queue->capacity) {
            return 0;
        }
        const auto* first = static_cast<const std::uint8_t*>(data);
        queue->messages.emplace_back(first, first + bytes);
        queue->used += bytes + kMessageOverhead;
    }
    queue->changed.notify_all();
    return bytes;
}

std::size_t xMessageBufferReceive(MessageBufferHandle_t buffer, void* data, std::size_t capacity,
                                  TickType_t ticks) {
    auto* queue = static_cast<Queue*>(buffer);
    std::unique_lock lock(queue->mutex);
    if (queue->messages.empty()) {
        queue->waiting = true;
        queue->changed.notify_all();
        queue->changed.wait_for(lock, std::chrono::milliseconds(ticks), [queue] { return !queue->messages.empty(); });
        queue->waiting = false;
    }
    if (queue->messages.empty()) {
        return 0;
    }
    std::vector<std::uint8_t> message = std::move(queue->messages.front());
    queue->messages.pop_front();
    queue->used -= message.size() + kMessageOverhead;
    const std::size_t bytes = std::min(capacity, message.size());
    std::memcpy(data, message.data(), bytes);
    return bytes;
}

// --- event groups ------------------------------------------------------------------------------

EventGroupHandle_t xEventGroupCreate() {
    return new EventGroup;
}

void vEventGroupDelete(EventGroupHandle_t group) {
    delete static_cast<EventGroup*>(group);
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    auto* events = static_cast<EventGroup*>(group);
    EventBits_t now = 0;
    {
        const std::lock_guard lock(events->mutex);
        events->bits |= bits;
        now = events->bits;
    }
    events->changed.notify_all();
    return now;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    auto* events = static_cast<EventGroup*>(group);
    const std::lock_guard lock(events->mutex);
    const EventBits_t before = events->bits;
    events->bits &= ~bits;
    return before;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, BaseType_t clear_on_exit,
                                BaseType_t wait_for_all, TickType_t ticks) {
    auto* events = static_cast<EventGroup*>(group);
    std::unique_lock lock(events->mutex);
    const auto satisfied = [&] {
        return wait_for_all != pdFALSE ? (events->bits & bits) == bits : (events->bits & bits) != 0;
    };
    events->changed.wait_for(lock, std::chrono::milliseconds(ticks), satisfied);
    const EventBits_t now = events->bits;
    if (satisfied() && clear_on_exit != pdFALSE) {
        events->bits &= ~bits;
    }
    return now;
}

// --- tasks -------------------------------------------------------------------------------------

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char* /*name*/, std::uint32_t /*stack_bytes*/,
                                   void* argument, UBaseType_t /*priority*/, TaskHandle_t* handle,
                                   BaseType_t /*core*/) {
    auto* task = new Task;
    task->thread = std::thread([entry, argument] {
        try {
            entry(argument);
        } catch (const TaskExit&) {
        }
    });
    task->thread.detach();
    if (handle != nullptr) {
        *handle = task;
    }
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t entry, const char* name, std::uint32_t stack_bytes,
                                           void* argument, UBaseType_t priority, TaskHandle_t* handle,
                                           BaseType_t core, std::uint32_t /*caps*/) {
    return xTaskCreatePinnedToCore(entry, name, stack_bytes, argument, priority, handle, core);
}

void vTaskDelete(TaskHandle_t task) {
    if (task == nullptr) {
        throw TaskExit{};
    }
}

void vTaskDeleteWithCaps(TaskHandle_t /*task*/) {}

void vTaskSuspend(TaskHandle_t /*task*/) {
    throw TaskExit{};
}

void vTaskDelay(TickType_t ticks) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t /*task*/) {
    return 0;
}

BaseType_t xTaskNotifyGive(TaskHandle_t /*task*/) {
    return pdPASS;
}

std::uint32_t ulTaskNotifyTake(BaseType_t /*clear_on_exit*/, TickType_t /*ticks*/) {
    return 0;
}

void taskYIELD() {
    std::this_thread::yield();
}

// --- what a test controls ----------------------------------------------------------------------

namespace fwhost {

void set_psram_present(bool present) {
    g_psram.store(present);
}

std::vector<Allocation> live_allocations() {
    const std::lock_guard lock(g_heap_mutex);
    std::vector<Allocation> out;
    out.reserve(g_live.size());
    for (const auto& entry : g_live) {
        out.push_back(entry.second);
    }
    return out;
}

std::size_t peak_bytes(std::uint32_t caps) {
    const std::lock_guard lock(g_heap_mutex);
    const auto found = g_peak.find(caps);
    return found == g_peak.end() ? 0 : found->second;
}

void reset_allocation_peaks() {
    const std::lock_guard lock(g_heap_mutex);
    g_peak = g_held;
}

void use_virtual_clock(std::int64_t now_us) {
    g_virtual_now.store(now_us);
    g_virtual_clock.store(true);
}

void set_virtual_now(std::int64_t now_us) {
    g_virtual_now.store(now_us);
}

void use_real_clock() {
    g_virtual_clock.store(false);
}

void wait_idle() {
    Queue* queue = g_queue.load();
    if (queue == nullptr) {
        return;
    }
    std::unique_lock lock(queue->mutex);
    queue->changed.wait(lock, [queue] { return queue->messages.empty() && queue->waiting; });
}

}  // namespace fwhost
