#include "nfc.h"

#include <string.h>

#include "cards.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pn532.h"
#include "util.h"

static const char *TAG = "nfc";

#define POLL_IDLE_MS 150    /* aucune carte : détection rapide */
#define POLL_PRESENT_MS 200 /* carte posée */
#define POLL_RECHECK_MS 80  /* après une lecture manquée : relectures rapprochées */
/* Absence continue avant de déclarer le retrait : les lectures ratent parfois quand
 * l'alimentation est perturbée (ampli, carte SD), une carte posée ne doit pas « clignoter ». */
#define REMOVAL_DELAY_US (1200 * 1000)
#define ERRORS_FOR_RESET 3

static nfc_event_cb_t s_cb;
static volatile bool s_ok;
static uint32_t s_fw;
static char s_current[UID_STR_MAX];
static SemaphoreHandle_t s_lock;

static void set_current(const char *uid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    str_copy(s_current, uid, sizeof(s_current));
    xSemaphoreGive(s_lock);
}

static void nfc_task(void *arg)
{
    int backoff_ms = 500;
    int errors = 0;
    int misses = 0;
    int64_t last_seen = 0;
    char current[UID_STR_MAX] = "";

    for (;;) {
        if (!s_ok) {
            esp_err_t err = pn532_setup(&s_fw);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "PN532 introuvable (%s), nouvel essai dans %d ms", esp_err_to_name(err), backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms = backoff_ms * 2 > 5000 ? 5000 : backoff_ms * 2;
                continue;
            }
            ESP_LOGI(TAG, "PN5%02X firmware %u.%u prêt", (unsigned)(s_fw >> 24) & 0xFF, (unsigned)(s_fw >> 16) & 0xFF,
                     (unsigned)(s_fw >> 8) & 0xFF);
            s_ok = true;
            backoff_ms = 500;
            errors = 0;
        }

        uint8_t uid[PN532_UID_MAX];
        uint8_t uid_len = 0;
        bool found = false;
        esp_err_t err = pn532_read_uid(uid, &uid_len, &found);
        if (err != ESP_OK) {
            if (++errors >= ERRORS_FOR_RESET) {
                ESP_LOGW(TAG, "le lecteur ne répond plus (%s), réinitialisation", esp_err_to_name(err));
                s_ok = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        errors = 0;

        if (found) {
            char hex[UID_STR_MAX];
            bytes_to_hex(uid, uid_len, hex);
            if (misses > 0 && strcmp(hex, current) == 0) {
                ESP_LOGI(TAG, "carte toujours présente après %d lecture(s) manquée(s) (%d ms)", misses,
                         (int)((esp_timer_get_time() - last_seen) / 1000));
            }
            misses = 0;
            last_seen = esp_timer_get_time();
            if (strcmp(hex, current) != 0) {
                if (current[0]) {
                    ESP_LOGI(TAG, "carte retirée : %s", current);
                    s_cb(false, current);
                }
                str_copy(current, hex, sizeof(current));
                set_current(current);
                ESP_LOGI(TAG, "carte posée : %s", current);
                s_cb(true, current);
            }
        } else if (current[0]) {
            misses++;
            if (misses == 1) {
                /* Remise à zéro du champ RF : une carte restée dans un état
                 * non inactif redevient détectable. Évite les faux retraits. */
                pn532_rf_field(false);
                vTaskDelay(pdMS_TO_TICKS(10));
                pn532_rf_field(true);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (esp_timer_get_time() - last_seen < REMOVAL_DELAY_US) {
                vTaskDelay(pdMS_TO_TICKS(POLL_RECHECK_MS));
                continue;
            }
            ESP_LOGI(TAG, "carte retirée : %s (%d lectures manquées)", current, misses);
            char old[UID_STR_MAX];
            str_copy(old, current, sizeof(old));
            current[0] = '\0';
            set_current("");
            misses = 0;
            s_cb(false, old);
        }
        vTaskDelay(pdMS_TO_TICKS(current[0] ? POLL_PRESENT_MS : POLL_IDLE_MS));
    }
}

esp_err_t nfc_start(nfc_event_cb_t cb)
{
    s_cb = cb;
    s_lock = xSemaphoreCreateMutex();
    esp_err_t err = pn532_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART PN532 : %s", esp_err_to_name(err));
        return err;
    }
    xTaskCreate(nfc_task, "nfc", 4096, NULL, 5, NULL);
    return ESP_OK;
}

bool nfc_reader_ok(void)
{
    return s_ok;
}

uint32_t nfc_firmware_version(void)
{
    return s_fw;
}

void nfc_current_uid(char *out, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    str_copy(out, s_current, len);
    xSemaphoreGive(s_lock);
}
