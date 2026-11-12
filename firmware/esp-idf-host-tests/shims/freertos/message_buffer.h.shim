#pragma once
#include "freertos/FreeRTOS.h"

using MessageBufferHandle_t = void*;

MessageBufferHandle_t xMessageBufferCreateStatic(std::size_t bytes, std::uint8_t* storage,
                                                 StaticMessageBuffer_t* control);
void vMessageBufferDelete(MessageBufferHandle_t buffer);
std::size_t xMessageBufferSpacesAvailable(MessageBufferHandle_t buffer);
std::size_t xMessageBufferSend(MessageBufferHandle_t buffer, const void* data, std::size_t bytes,
                               TickType_t ticks);
std::size_t xMessageBufferReceive(MessageBufferHandle_t buffer, void* data, std::size_t capacity,
                                  TickType_t ticks);
