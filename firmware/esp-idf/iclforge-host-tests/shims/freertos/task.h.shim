#pragma once
#include "freertos/FreeRTOS.h"

using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char* name, std::uint32_t stack_bytes,
                                   void* argument, UBaseType_t priority, TaskHandle_t* handle,
                                   BaseType_t core);
// A task ends itself with vTaskDelete(nullptr); another task's handle is a lane the player's tests
// do not start.
void vTaskDelete(TaskHandle_t task);
void vTaskSuspend(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
std::uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks);
void taskYIELD();
