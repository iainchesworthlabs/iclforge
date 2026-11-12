#pragma once
#include "freertos/FreeRTOS.h"

using EventGroupHandle_t = void*;

EventGroupHandle_t xEventGroupCreate();
void vEventGroupDelete(EventGroupHandle_t group);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, BaseType_t clear_on_exit,
                                BaseType_t wait_for_all, TickType_t ticks);
