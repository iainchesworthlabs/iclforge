#pragma once
// The host build of the component's player code (iclforge-host-tests/): the FreeRTOS types and constants the
// player names. The kernel itself is shim_runtime.cpp's threads, condition variables and queues.
#include <cstddef>
#include <cstdint>

using BaseType_t = int;
using UBaseType_t = unsigned int;
using TickType_t = std::uint32_t;
using EventBits_t = std::uint32_t;

inline constexpr BaseType_t pdPASS = 1;
inline constexpr BaseType_t pdFAIL = 0;
inline constexpr BaseType_t pdTRUE = 1;
inline constexpr BaseType_t pdFALSE = 0;
inline constexpr TickType_t portMAX_DELAY = 0xFFFFFFFFU;

// One tick is one millisecond here.
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
#define BIT0 (1U << 0)
#define BIT1 (1U << 1)

// What a static message buffer is made over: the host's queue hangs from the pointer.
struct StaticMessageBuffer_t {
    void* queue = nullptr;
};
