/*
 * Implémentations de substitution (FreeRTOS sur pthreads, horloge, I2S, décodeur)
 * pour exécuter le vrai lecteur et le vrai serveur MPD sur PC.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "driver/i2s_std.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ---------- Divers ---------- */

void shim_log(char level, const char *tag, const char *fmt, ...)
{
    if (level == 'D' || !getenv("SHIM_LOG")) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%c (%s) ", level, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

uint32_t esp_random(void)
{
    return (uint32_t)random();
}

void esp_fill_random(void *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = (uint8_t)random();
    }
}

const char *esp_err_to_name(esp_err_t code)
{
    return code == ESP_OK ? "ESP_OK" : "ESP_ERR";
}

static struct timespec deadline_after(TickType_t ticks)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ticks / 1000;
    ts.tv_nsec += (long)(ticks % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

/* ---------- Files ---------- */

struct shim_queue {
    pthread_mutex_t m;
    pthread_cond_t not_empty, not_full;
    size_t item, len, count, head;
    uint8_t *buf;
};

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    struct shim_queue *q = calloc(1, sizeof(*q));
    q->buf = calloc(len, item_size);
    q->item = item_size;
    q->len = len;
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

static bool wait_cond(pthread_cond_t *c, pthread_mutex_t *m, TickType_t ticks, const struct timespec *dl)
{
    if (ticks == 0) {
        return false;
    }
    if (ticks == portMAX_DELAY) {
        pthread_cond_wait(c, m);
        return true;
    }
    return pthread_cond_timedwait(c, m, dl) != ETIMEDOUT;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    struct timespec dl = deadline_after(ticks);
    pthread_mutex_lock(&q->m);
    while (q->count == q->len) {
        if (!wait_cond(&q->not_full, &q->m, ticks, &dl) && q->count == q->len) {
            pthread_mutex_unlock(&q->m);
            return pdFALSE;
        }
    }
    memcpy(q->buf + ((q->head + q->count) % q->len) * q->item, item, q->item);
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item)
{
    pthread_mutex_lock(&q->m);
    memcpy(q->buf, item, q->item); /* file de longueur 1 */
    q->head = 0;
    q->count = 1;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    struct timespec dl = deadline_after(ticks);
    pthread_mutex_lock(&q->m);
    while (q->count == 0) {
        if (!wait_cond(&q->not_empty, &q->m, ticks, &dl) && q->count == 0) {
            pthread_mutex_unlock(&q->m);
            return pdFALSE;
        }
    }
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->len;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

/* ---------- Mutex ---------- */

struct shim_sem {
    pthread_mutex_t m;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct shim_sem *s = calloc(1, sizeof(*s));
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK); /* détecte les doubles prises */
    pthread_mutex_init(&s->m, &a);
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    int r;
    if (ticks == portMAX_DELAY) {
        r = pthread_mutex_lock(&s->m);
    } else {
        struct timespec dl = deadline_after(ticks);
        r = pthread_mutex_timedlock(&s->m, &dl);
    }
    if (r == EDEADLK) {
        fprintf(stderr, "INTERBLOCAGE : mutex déjà détenu par ce fil\n");
        abort();
    }
    return r == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (pthread_mutex_unlock(&s->m) != 0) {
        fprintf(stderr, "ERREUR : libération d'un mutex non détenu\n");
        abort();
    }
    return pdTRUE;
}

/* ---------- Tâches ---------- */

struct shim_task {
    pthread_t th;
    TaskFunction_t fn;
    void *arg;
};

static __thread struct shim_task *t_self;

static void *trampoline(void *p)
{
    t_self = p;
    t_self->fn(t_self->arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    struct shim_task *t = calloc(1, sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    if (out) {
        *out = t;
    }
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack < 65536 ? 65536 : stack); /* marge pour ASan */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t->th, &a, trampoline, t);
    pthread_attr_destroy(&a);
    return r == 0 ? pdPASS : pdFAIL;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out)
{
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, 0);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return t_self;
}

void vTaskDelay(TickType_t ticks)
{
    usleep((useconds_t)ticks * 1000);
}

void vTaskDelete(TaskHandle_t t)
{
    if (t == NULL) {
        pthread_exit(NULL);
    }
}

/* ---------- I2S ---------- */

static uint64_t s_frames;
static uint32_t s_rate = 44100;

static int speedup(void)
{
    const char *s = getenv("SHIM_SPEEDUP");
    int v = s ? atoi(s) : 20;
    return v > 0 ? v : 20;
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    *tx = (i2s_chan_handle_t)1;
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t *cfg)
{
    s_rate = cfg->clk_cfg.sample_rate_hz;
    return ESP_OK;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t h)
{
    return ESP_OK;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t h)
{
    return ESP_OK;
}

esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t h, const i2s_std_clk_config_t *clk)
{
    s_rate = clk->sample_rate_hz;
    return ESP_OK;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *src, size_t size, size_t *written, TickType_t ticks)
{
    uint64_t frames = size / 4;
    __atomic_add_fetch(&s_frames, frames, __ATOMIC_RELAXED);
    usleep((useconds_t)(frames * 1000000ULL / s_rate / speedup()));
    *written = size;
    return ESP_OK;
}

uint64_t shim_i2s_frames_written(void)
{
    return __atomic_load_n(&s_frames, __ATOMIC_RELAXED);
}

uint32_t shim_i2s_rate(void)
{
    return s_rate;
}

/* ---------- Décodeur ---------- */

#define FAKE_FRAME 2048

esp_audio_err_t esp_audio_dec_register_default(void)
{
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_audio_simple_dec_register_default(void)
{
    return ESP_AUDIO_ERR_OK;
}

typedef struct {
    bool started;
} fake_dec_t;

esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *cfg, esp_audio_simple_dec_handle_t *h)
{
    *h = calloc(1, sizeof(fake_dec_t));
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t h, esp_audio_simple_dec_raw_t *raw,
                                             esp_audio_simple_dec_out_t *out)
{
    fake_dec_t *d = h;
    raw->consumed = 0;
    out->decoded_size = 0;
    if (raw->len >= 7 && memcmp(raw->buffer, "CORRUPT", 7) == 0) {
        return ESP_AUDIO_ERR_FAIL;
    }
    uint32_t n = raw->len < FAKE_FRAME ? raw->len : FAKE_FRAME;
    n &= ~3u;
    if (n == 0) {
        raw->consumed = raw->len; /* reliquat < 4 octets */
        return ESP_AUDIO_ERR_OK;
    }
    if (out->len < n) {
        out->needed_size = FAKE_FRAME;
        return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;
    }
    memcpy(out->buffer, raw->buffer, n);
    raw->consumed = n;
    out->decoded_size = n;
    d->started = true;
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t h, esp_audio_simple_dec_info_t *info)
{
    fake_dec_t *d = h;
    if (!d->started) {
        return ESP_AUDIO_ERR_NOT_FOUND;
    }
    info->sample_rate = 44100;
    info->bits_per_sample = 16;
    info->channel = 2;
    info->bitrate = 1411200;
    return ESP_AUDIO_ERR_OK;
}

void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t h)
{
    free(h);
}
