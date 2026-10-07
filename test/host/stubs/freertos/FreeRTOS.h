#pragma once
/* Substitut FreeRTOS sur pthreads pour les tests sur PC. 1 tick = 1 ms. */
#include <stdbool.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef struct shim_queue *QueueHandle_t;
typedef struct shim_sem *SemaphoreHandle_t;
typedef struct shim_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
