#pragma once
#include "freertos/FreeRTOS.h"
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
