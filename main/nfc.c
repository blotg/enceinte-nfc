#include "nfc.h"

#include <stdio.h>
#include <string.h>

#include "cards.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pn5180.h"
#include "pn5180_proto.h"
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
#define UID_MAX 10
_Static_assert(UID_MAX >= PN532_UID_MAX && UID_MAX >= PN5180_UID_MAX, "UID_MAX trop petit");

/* Lecteurs reconnus, essayés dans l'ordre : le premier qui répond est utilisé. */
typedef struct {
    const char *name;
    esp_err_t (*setup)(char *desc, size_t desc_len);
    esp_err_t (*read_uid)(uint8_t *uid, uint8_t *uid_len, bool *found);
    void (*rf_reset)(void); /* NULL : le champ est déjà coupé entre deux interrogations */
} reader_t;

static esp_err_t pn532_setup_desc(char *desc, size_t desc_len)
{
    uint32_t fw = 0;
    esp_err_t err = pn532_setup(&fw);
    if (err == ESP_OK) {
        snprintf(desc, desc_len, "PN5%02X, firmware %u.%u", (unsigned)(fw >> 24) & 0xFF, (unsigned)(fw >> 16) & 0xFF,
                 (unsigned)(fw >> 8) & 0xFF);
    }
    return err;
}

/* Remise à zéro du champ RF : une carte restée dans un état non inactif redevient détectable. */
static void pn532_rf_reset(void)
{
    pn532_rf_field(false);
    vTaskDelay(pdMS_TO_TICKS(10));
    pn532_rf_field(true);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static const reader_t s_pn5180 = {"PN5180", pn5180_setup, pn5180_read_uid, NULL};
static const reader_t s_pn532 = {"PN532", pn532_setup_desc, pn532_read_uid, pn532_rf_reset};

static const reader_t *s_readers[2];
static int s_reader_count;
static const reader_t *s_reader;

static nfc_event_cb_t s_cb;
static volatile bool s_ok;
static char s_desc[48];
static char s_current[UID_STR_MAX];
static SemaphoreHandle_t s_lock;

static void set_current(const char *uid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    str_copy(s_current, uid, sizeof(s_current));
    xSemaphoreGive(s_lock);
}

/* Cherche un lecteur parmi ceux dont le bus est prêt. */
static esp_err_t detect(void)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_reader_count; i++) {
        char desc[sizeof(s_desc)] = "";
        err = s_readers[i]->setup(desc, sizeof(desc));
        if (err == ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            str_copy(s_desc, desc[0] ? desc : s_readers[i]->name, sizeof(s_desc));
            xSemaphoreGive(s_lock);
            s_reader = s_readers[i];
            return ESP_OK;
        }
    }
    return err;
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
            esp_err_t err = detect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "aucun lecteur NFC (%s), nouvel essai dans %d ms", esp_err_to_name(err), backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms = backoff_ms * 2 > 5000 ? 5000 : backoff_ms * 2;
                continue;
            }
            ESP_LOGI(TAG, "lecteur prêt : %s", s_desc);
            s_ok = true;
            backoff_ms = 500;
            errors = 0;
        }

        uint8_t uid[UID_MAX];
        uint8_t uid_len = 0;
        bool found = false;
        esp_err_t err = s_reader->read_uid(uid, &uid_len, &found);
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
            if (misses == 1 && s_reader->rf_reset) {
                /* Évite les faux retraits : nouvel essai immédiat avec un champ RF neuf. */
                s_reader->rf_reset();
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
    esp_err_t err = pn5180_init();
    if (err == ESP_OK) {
        s_readers[s_reader_count++] = &s_pn5180;
    } else if (err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "SPI PN5180 : %s", esp_err_to_name(err));
    }
    err = pn532_init();
    if (err == ESP_OK) {
        s_readers[s_reader_count++] = &s_pn532;
    } else {
        ESP_LOGE(TAG, "UART PN532 : %s", esp_err_to_name(err));
    }
    if (s_reader_count == 0) {
        return ESP_FAIL;
    }
    xTaskCreate(nfc_task, "nfc", 4096, NULL, 5, NULL);
    return ESP_OK;
}

bool nfc_reader_ok(void)
{
    return s_ok;
}

void nfc_reader_desc(char *out, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    str_copy(out, s_ok ? s_desc : "", len);
    xSemaphoreGive(s_lock);
}

void nfc_current_uid(char *out, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    str_copy(out, s_current, len);
    xSemaphoreGive(s_lock);
}
