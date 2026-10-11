#pragma once

// The AC-4 decoder as the player uses it (planning/ac4.md, D14b): what reading
// an AC-4 stream needs that is not the player's own loop. player.cpp includes
// this only when CONFIG_ICLFORGE_AC4 is on, and with it off none of this is in
// the build.
//
// The decoder itself is libs/ac4/src/decoder's iclforge::ac4::Decoder, in the float scalar, built
// as a static archive with the minimum-footprint profile's compile options (ICLFORGE_MINIMAL_AC4,
// root CMakeLists.txt). It is asked for what a player asks of the AC-3 and E-AC-3 decoders: blocks
// of 256 samples a channel, handed to a callback as the decoder completes them
// (iclforge::ac4::Decoder::decode_by_block), so the player holds one block of the audio and not a
// frame's worth.

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/decoder/output.hpp"

#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/decoder/executor.hpp"

namespace iclforge::ac4bridge {

// Whether `bytes` opens with an AC-4 sync word, 0xAC40 or 0xAC41 (ETSI TS 103
// 190-2 Annex G.4.1) where AC-3's and E-AC-3's is 0x0B77: how every front end
// that reads a stream decides which decoder reads it (apps/shared/media/src/
// ac4_sync_word.hpp, which the desktop applications use and this component
// cannot reach, since its archive carries no apps/).
[[nodiscard]] inline bool is_ac4(std::span<const std::byte> bytes) noexcept {
    return bytes.size() >= 2 && std::to_integer<unsigned>(bytes[0]) == 0xACU &&
           (std::to_integer<unsigned>(bytes[1]) & 0xFEU) == 0x40U;
}

// Where an AC-4 speaker is among A/52's Table E2.5 locations, for the layout
// renderer that places a decoded bed on the player's output layout: the same
// reading as apps/shared/media/src/ac4_channels.hpp's ac4_location(), which the desktop
// applications place a decoded presentation by. Lb and Rb are the rear
// surrounds, Lw and Rw the wides, the top front pair the vertical heights, the
// top back and top side pairs the top surrounds (Table E2.5 has one pair for
// both), the second LFE LFE2, and 22.2's Tfc the vertical height centre, Tc and
// Tbc the top surround and Cb the centre surround. 22.2's bottom channels and
// 9.X.4's screen pair have no location there: nothing.
[[nodiscard]] inline std::optional<iclforge::ac3::eac3::chanmap::Location> location(
    iclforge::ac4::Speaker speaker) {
    using L = iclforge::ac3::eac3::chanmap::Location;
    switch (speaker) {
        case iclforge::ac4::Speaker::kLeft:
            return L::kLeft;
        case iclforge::ac4::Speaker::kRight:
            return L::kRight;
        case iclforge::ac4::Speaker::kCentre:
            return L::kCentre;
        case iclforge::ac4::Speaker::kLfe:
            return L::kLfe;
        case iclforge::ac4::Speaker::kLeftSurround:
            return L::kLeftSurround;
        case iclforge::ac4::Speaker::kRightSurround:
            return L::kRightSurround;
        case iclforge::ac4::Speaker::kLeftBack:
            return L::kLrs;
        case iclforge::ac4::Speaker::kRightBack:
            return L::kRrs;
        case iclforge::ac4::Speaker::kLeftWide:
            return L::kLw;
        case iclforge::ac4::Speaker::kRightWide:
            return L::kRw;
        case iclforge::ac4::Speaker::kTopFrontLeft:
            return L::kVhl;
        case iclforge::ac4::Speaker::kTopFrontRight:
            return L::kVhr;
        case iclforge::ac4::Speaker::kTopBackLeft:
        case iclforge::ac4::Speaker::kTopSideLeft:
            return L::kLts;
        case iclforge::ac4::Speaker::kTopBackRight:
        case iclforge::ac4::Speaker::kTopSideRight:
            return L::kRts;
        case iclforge::ac4::Speaker::kLfe2:
            return L::kLfe2;
        case iclforge::ac4::Speaker::kTopFrontCentre:
            return L::kVhc;
        case iclforge::ac4::Speaker::kTopCentre:
        case iclforge::ac4::Speaker::kTopBackCentre:
            return L::kTs;
        case iclforge::ac4::Speaker::kCentreBack:
            return L::kCs;
        case iclforge::ac4::Speaker::kLeftScreen:
        case iclforge::ac4::Speaker::kRightScreen:
        case iclforge::ac4::Speaker::kBottomFrontLeft:
        case iclforge::ac4::Speaker::kBottomFrontRight:
        case iclforge::ac4::Speaker::kBottomFrontCentre:
            return std::nullopt;
    }
    return std::nullopt;
}

// Whether every one of `speakers` has a location, which a bed made of them needs.
[[nodiscard]] inline bool placeable(std::span<const iclforge::ac4::Speaker> speakers) {
    for (const iclforge::ac4::Speaker speaker : speakers) {
        if (!location(speaker).has_value()) {
            return false;
        }
    }
    return true;
}

// The coded layout a decoded block's channels are in, as the renderer takes it:
// each channel's location, in the decoder's order. Channels past what a
// renderer's bed holds are left out.
[[nodiscard]] inline iclforge::ac3::eac3::chanmap::Layout bed(
    std::span<const iclforge::ac4::Speaker> speakers) {
    iclforge::ac3::eac3::chanmap::Layout layout{};
    for (const iclforge::ac4::Speaker speaker : speakers) {
        if (layout.count >= iclforge::ac3::eac3::chanmap::kMaxChannels) {
            break;
        }
        layout.items[static_cast<std::size_t>(layout.count)] =
            location(speaker).value_or(iclforge::ac3::eac3::chanmap::Location::kCentre);
        ++layout.count;
    }
    return layout;
}

// The decoder's own fold for the player's serving of a stereo or mono layout
// (iclforge::ac3::render::serve): the decoder folds where the player would fold, and
// hands the renderer what it hands it for AC-3, two channels or one.
[[nodiscard]] inline iclforge::ac4::DownmixTarget target(
    std::optional<iclforge::ac3::DownmixTarget> fold) noexcept {
    if (!fold.has_value()) {
        return iclforge::ac4::DownmixTarget::kAsCoded;
    }
    switch (*fold) {
        case iclforge::ac3::DownmixTarget::kLoRo:
            return iclforge::ac4::DownmixTarget::kLoRo;
        case iclforge::ac3::DownmixTarget::kLtRt:
            return iclforge::ac4::DownmixTarget::kLtRt;
        case iclforge::ac3::DownmixTarget::kMono:
            return iclforge::ac4::DownmixTarget::kMono;
        case iclforge::ac3::DownmixTarget::kAsCoded:
            break;
    }
    return iclforge::ac4::DownmixTarget::kAsCoded;
}

// Every delivered sample's bit pattern, in delivery order, through FNV-1a: the
// probe's own hash (firmware/baremetal/probe.cpp's PcmHash), so a value printed
// here is comparable with one printed there. Decision 26 of planning/ac4.md
// promises the float tier's output identical on the host, the Cortex-M3 leg and
// the ESP32s, and this is what says whether it is.
struct PcmHash {
    std::uint64_t state = 14695981039346656037ULL;

    void add(std::span<const float> pcm) noexcept {
        for (const float sample : pcm) {
            const auto bits = std::bit_cast<std::uint32_t>(sample);
            for (int shift = 0; shift < 32; shift += 8) {
                state ^= (bits >> shift) & 0xFFU;
                state *= 1099511628211ULL;
            }
        }
    }
};

// The decoder's Executor over the part's other core: one worker task pinned there, woken by a
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
class TaskExecutor final : public iclforge::ac4::Executor {
   public:
    TaskExecutor() = default;
    ~TaskExecutor() override { stop(); }

    // Starts the worker on `core` at `priority`, with its stack in PSRAM where `stack_in_psram`
    // asks for it and there is PSRAM to give (the decode task's is there, and the internal RAM
    // of a part with Wi-Fi up is the scarce thing); false, and lanes() 1, if the task cannot be
    // made.
    bool start(BaseType_t core, UBaseType_t priority, std::uint32_t stack_bytes,
               bool stack_in_psram) {
        if (worker_ != nullptr) {
            return true;
        }
        quit_.store(false);
        exited_.store(false);
        with_caps_ = false;
        if (stack_in_psram && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
            with_caps_ = xTaskCreatePinnedToCoreWithCaps(
                             &TaskExecutor::entry, "ac4-lane", stack_bytes, this, priority,
                             &worker_, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
        }
        stack_bytes_ = stack_bytes;
        if (with_caps_) {
            return true;
        }
        worker_ = nullptr;
        return xTaskCreatePinnedToCore(&TaskExecutor::entry, "ac4-lane", stack_bytes, this,
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
        std::printf("player: AC-4 lane stack: %lu of %lu bytes never used\n",
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

}  // namespace iclforge::ac4bridge
