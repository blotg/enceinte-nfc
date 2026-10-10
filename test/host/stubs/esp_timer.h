#pragma once
#include <stdint.h>

#include "esp_err.h"

int64_t esp_timer_get_time(void);

typedef struct esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    const char *name;
} esp_timer_create_args_t;
/* Minuteries périodiques : sans effet sur PC (les tests appellent directement les traitements). */
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
