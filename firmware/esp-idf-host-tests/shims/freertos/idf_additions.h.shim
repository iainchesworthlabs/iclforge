#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t entry, const char* name, std::uint32_t stack_bytes,
                                           void* argument, UBaseType_t priority, TaskHandle_t* handle,
                                           BaseType_t core, std::uint32_t caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
