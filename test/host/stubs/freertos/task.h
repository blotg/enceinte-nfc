#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
static inline BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack,
                                                         void *arg, UBaseType_t prio, TaskHandle_t *out,
                                                         BaseType_t core, uint32_t caps)
{
    (void)caps;
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, core);
}
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t t);
