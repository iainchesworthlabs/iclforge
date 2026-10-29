#pragma once

// The decode lane: a worker task on the part's other core that a decoder (the AC-4 decoder, the
// AC-3 and E-AC-3 decoders: DecoderConfig::executor) hands its independent per-channel and
// per-object work to, through iclforge::base::Executor. It was ac4_bridge.hpp's TaskExecutor and
// moved here when the AC-3 and E-AC-3 players took it as well; ac4_bridge.hpp names it still.
//
// Nothing here is codec-specific and nothing includes a codec: the player and the Sendspin
// burst player include this where the build has two cores to give.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "iclforge/base/executor.hpp"

namespace iclforge::lanes {

// A decoder's Executor over the part's other core: one worker task pinned there, woken by a
// task notification when the decode task has a stage of per-channel work for it, which the two
// then take a task at a time from one shared cursor until they are all done. The decode task
// is lane 0 and the worker lane 1; where the worker does not start, lanes() is 1 and run() does
// the work in order on the caller, as a decoder without an executor does.
//
// The worker also takes the decoder's one background task a frame (run_async(): the next frame's
// syntax, read while this frame is reconstructed), and runs it to its end before it looks at the
// cursor again: a run() that starts meanwhile is done by the decode task alone, which is the
// worker's share of it that was not going to be in time.
//
// The cursor holds the run's generation, its task count and the next index in one word, so that
// a worker woken late for a run that has finished meets the next run's cursor and takes its
// tasks (it was woken for that one too) and never one of the run it was woken for, whose tasks
// the other lane has done: a claim succeeds only on the cursor it read, and a run does not
// begin until the last claim of the one before has returned.
class TaskExecutor final : public iclforge::base::Executor {
   public:
    TaskExecutor() = default;
    ~TaskExecutor() override { stop(); }

    // Starts the worker on `core` at `priority`, with its stack in PSRAM where `stack_in_psram`
    // asks for it and there is PSRAM to give (the decode task's is there, and the internal RAM
    // of a part with Wi-Fi up is the scarce thing); false, and lanes() 1, if the task cannot be
    // made. A stack asked for in internal RAM goes to PSRAM when there is no internal block to
    // hold it (the P4X's screen, TDM queue and 40 KB decode stack leave one of 7 KB): a lane
    // whose stack is read through the cache is better than a decoder on one core.
    bool start(BaseType_t core, UBaseType_t priority, std::uint32_t stack_bytes,
               bool stack_in_psram) {
        if (worker_ != nullptr) {
            return true;
        }
        quit_.store(false);
        exited_.store(false);
        with_caps_ = false;
        const bool have_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
        const bool internal_fits =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
            stack_bytes + kInternalSlackBytes;
        if (have_psram && (stack_in_psram || !internal_fits)) {
            with_caps_ = xTaskCreatePinnedToCoreWithCaps(
                             &TaskExecutor::entry, "decode-lane", stack_bytes, this, priority,
                             &worker_, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
            if (with_caps_ && !stack_in_psram) {
                std::printf("player: decode lane stack in PSRAM: no internal block of %lu bytes\n",
                            static_cast<unsigned long>(stack_bytes + kInternalSlackBytes));
            }
        }
        stack_bytes_ = stack_bytes;
        if (with_caps_) {
            return true;
        }
        worker_ = nullptr;
        return xTaskCreatePinnedToCore(&TaskExecutor::entry, "decode-lane", stack_bytes, this,
                                       priority, &worker_, core) == pdPASS;
    }

    // The worker's stack that was never used, in bytes, at its worst so far.
    [[nodiscard]] std::size_t stack_free() const {
        return worker_ != nullptr ? static_cast<std::size_t>(uxTaskGetStackHighWaterMark(worker_))
                                  : 0;
    }

    void stop() {
        if (worker_ == nullptr) {
            return;
        }
        std::printf("player: decode lane stack: %lu of %lu bytes never used\n",
                    static_cast<unsigned long>(stack_free()),
                    static_cast<unsigned long>(stack_bytes_));
        quit_.store(true);
        xTaskNotifyGive(worker_);
        while (!exited_.load()) {
            vTaskDelay(1);
        }
        // The task has parked itself: a task made with caps is deleted by another, with them.
        if (with_caps_) {
            vTaskDeleteWithCaps(worker_);
        } else {
            vTaskDelete(worker_);
        }
        worker_ = nullptr;
    }

    [[nodiscard]] std::size_t lanes() const noexcept override { return worker_ != nullptr ? 2 : 1; }

    void run(std::size_t count, Task task, void* context) override {
        if (worker_ == nullptr || count < 2 || count > 255) {
            for (std::size_t i = 0; i < count; ++i) {
                task(context, i, 0);
            }
            return;
        }
        task_ = task;
        context_ = context;
        done_.store(0, std::memory_order_relaxed);
        generation_ = static_cast<std::uint16_t>(generation_ + 1);
        cursor_.store((static_cast<std::uint32_t>(generation_) << 16) |
                          (static_cast<std::uint32_t>(count) << 8),
                      std::memory_order_release);
        xTaskNotifyGive(worker_);
        drain(0);
        while (done_.load(std::memory_order_acquire) < count) {
            taskYIELD();
        }
    }

    void run_async(Task task, void* context) override {
        if (worker_ == nullptr) {
            task(context, 0, 0);
            return;
        }
        async_task_ = task;
        async_context_ = context;
        async_over_.store(false, std::memory_order_relaxed);
        async_pending_.store(true, std::memory_order_release);
        xTaskNotifyGive(worker_);
    }

    void wait_async() override {
        if (worker_ == nullptr) {
            return;
        }
        while (async_pending_.load(std::memory_order_acquire) ||
               !async_over_.load(std::memory_order_acquire)) {
            taskYIELD();
        }
    }

   private:
    // What a task's control block and alignment take beside its stack.
    static constexpr std::uint32_t kInternalSlackBytes = 1024;

    void drain(std::size_t lane) {
        for (;;) {
            std::uint32_t cursor = cursor_.load(std::memory_order_acquire);
            std::uint32_t index = 0;
            for (;;) {
                index = cursor & 0xFFU;
                if (index >= ((cursor >> 8) & 0xFFU)) {
                    return;
                }
                if (cursor_.compare_exchange_weak(cursor, cursor + 1, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                    break;
                }
            }
            task_(context_, index, lane);
            done_.fetch_add(1, std::memory_order_release);
        }
    }

    static void entry(void* self) {
        auto* executor = static_cast<TaskExecutor*>(self);
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (executor->quit_.load()) {
                break;
            }
            if (executor->async_pending_.exchange(false, std::memory_order_acq_rel)) {
                executor->async_task_(executor->async_context_, 0, 1);
                executor->async_over_.store(true, std::memory_order_release);
            }
            executor->drain(1);
        }
        executor->exited_.store(true);
        // Parked until stop() deletes it, which is the one that can free a stack made with caps.
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }

    TaskHandle_t worker_ = nullptr;
    bool with_caps_ = false;
    std::uint32_t stack_bytes_ = 0;
    Task task_ = nullptr;
    void* context_ = nullptr;
    std::uint16_t generation_ = 0;
    std::atomic<std::uint32_t> cursor_{0};
    std::atomic<std::size_t> done_{0};
    Task async_task_ = nullptr;
    void* async_context_ = nullptr;
    std::atomic<bool> async_pending_{false};
    std::atomic<bool> async_over_{true};
    std::atomic<bool> quit_{false};
    std::atomic<bool> exited_{false};
};

}  // namespace iclforge::lanes
