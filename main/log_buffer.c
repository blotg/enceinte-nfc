#include "log_buffer.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_ring.h"
#include "sdkconfig.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include "esp_core_dump.h"
#endif

static const char *TAG = "journal";

#define SIZE_PSRAM (48 * 1024)
#define SIZE_INTERNAL (8 * 1024)
#define LINE_MAX_LEN 384

static log_ring_t s_ring;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev;
static char s_line[LINE_MAX_LEN]; /* protégé par s_lock */

/* Relais de ESP_LOG : port série comme avant, puis copie dans le tampon. Jamais d'attente :
 * une ligne émise pendant qu'une autre tâche écrit est seulement envoyée sur le port série. */
static int relay(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int ret = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
    if (!xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_lock, 0) == pdTRUE) {
        int n = vsnprintf(s_line, sizeof(s_line), fmt, copy);
        if (n > 0) {
            size_t len = (size_t)n < sizeof(s_line) ? (size_t)n : sizeof(s_line) - 1;
            log_ring_write(&s_ring, s_line, len);
            if (len < (size_t)n && s_line[len - 1] != '\n') {
                log_ring_write(&s_ring, "…\n", strlen("…\n")); /* ligne tronquée */
            }
        }
        xSemaphoreGive(s_lock);
    }
    va_end(copy);
    return ret;
}

void log_buffer_start(void)
{
    size_t size = SIZE_PSRAM;
    char *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        size = SIZE_INTERNAL;
        buf = malloc(size);
    }
    s_lock = xSemaphoreCreateMutex();
    if (!buf || !s_lock) {
        free(buf);
        return;
    }
    log_ring_init(&s_ring, buf, size);
    s_prev = esp_log_set_vprintf(relay);
}

size_t log_buffer_size(void)
{
    return s_ring.cap;
}

size_t log_buffer_read(uint64_t since, char *out, size_t cap, uint64_t *next, bool *reset)
{
    if (!s_lock) {
        *next = 0;
        *reset = false;
        if (cap) {
            out[0] = '\0';
        }
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = log_ring_read(&s_ring, since, out, cap, next, reset);
    xSemaphoreGive(s_lock);
    return n;
}

static const char *reset_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:
        return "mise sous tension";
    case ESP_RST_EXT:
        return "broche de réinitialisation";
    case ESP_RST_SW:
        return "redémarrage demandé (mise à jour, réglage, bouton)";
    case ESP_RST_PANIC:
        return "plantage du firmware";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return "chien de garde (tâche bloquée)";
    case ESP_RST_BROWNOUT:
        return "chute de tension de l'alimentation";
    case ESP_RST_DEEPSLEEP:
        return "réveil";
    case ESP_RST_USB:
        return "port USB";
    default:
        return NULL;
    }
}

void log_buffer_report_boot(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    const char *why = reset_text(r);
    bool bad = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
               r == ESP_RST_BROWNOUT;
    if (bad) {
        ESP_LOGW(TAG, "démarrage après : %s", why);
    } else if (why) {
        ESP_LOGI(TAG, "démarrage après : %s", why);
    } else {
        ESP_LOGI(TAG, "démarrage après : raison %d", (int)r);
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }
    esp_core_dump_summary_t *s = malloc(sizeof(*s));
    if (s && esp_core_dump_get_summary(s) == ESP_OK) {
        char bt[16 * 11 + 1] = "";
        size_t o = 0;
        uint32_t depth = s->exc_bt_info.depth < 16 ? s->exc_bt_info.depth : 16;
        for (uint32_t i = 0; i < depth && o < sizeof(bt); i++) {
            o += (size_t)snprintf(bt + o, sizeof(bt) - o, " 0x%08" PRIx32, s->exc_bt_info.bt[i]);
        }
        /* app_elf_sha256 est déjà du texte (hexadécimal) : 9 caractères, comme « build » */
        ESP_LOGW(TAG, "plantage précédent : tâche « %s », PC 0x%08" PRIx32 ", firmware %.9s, pile :%s%s", s->exc_task,
                 s->exc_pc, (const char *)s->app_elf_sha256, bt, s->exc_bt_info.corrupted ? " (incomplète)" : "");
    }
    free(s);
    esp_core_dump_image_erase(); /* signalé une seule fois */
#endif
}

#define STACK_STEP 256  /* nouvel enregistrement au-delà de cette baisse */
#define STACK_WARN 1536 /* en dessous : avertissement */

void log_buffer_stack_check(const char *task, const char *detail, uint32_t *low)
{
    uint32_t left = (uint32_t)uxTaskGetStackHighWaterMark(NULL); /* octets (ESP-IDF) */
    if (left + STACK_STEP > *low) {
        return;
    }
    *low = left;
    ESP_LOG_LEVEL(left < STACK_WARN ? ESP_LOG_WARN : ESP_LOG_INFO, TAG, "pile « %s » : %" PRIu32 " octets libres au plus bas%s%s%s",
                  task, left, detail ? " (" : "", detail ? detail : "", detail ? ")" : "");
}

void log_buffer_memory(const char *when)
{
    ESP_LOGI(TAG, "mémoire interne %s : %u Ko libres, plus grand bloc %u Ko, minimum %u Ko", when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
}
