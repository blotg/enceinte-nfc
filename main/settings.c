#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "util.h"

static const char *TAG = "settings";
static const char *NS = "settings";

#define PW_SALT_LEN 16
#define PW_HASH_LEN 32
#define PW_ITERATIONS 10000
#define PW_BLOB_LEN (4 + PW_SALT_LEN + PW_HASH_LEN)

static settings_t s_cfg;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_volume_timer;

static esp_err_t open_ns(nvs_open_mode_t mode, nvs_handle_t *h)
{
    return nvs_open_from_partition(CFG_PARTITION, NS, mode, h);
}

static void load_str(nvs_handle_t h, const char *key, char *out, size_t len, const char *def)
{
    size_t sz = len;
    if (nvs_get_str(h, key, out, &sz) != ESP_OK) {
        str_copy(out, def, len);
    }
}

static bool blob_exists(nvs_handle_t h, const char *key)
{
    size_t sz = 0;
    return nvs_get_blob(h, key, NULL, &sz) == ESP_OK && sz == PW_BLOB_LEN;
}

static esp_err_t save_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void volume_timer_cb(void *arg)
{
    uint8_t v;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v = s_cfg.volume;
    xSemaphoreGive(s_lock);
    save_u8("volume", v);
}

esp_err_t settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    esp_err_t err = nvs_flash_init_partition(CFG_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "partition %s illisible, effacement", CFG_PARTITION);
        nvs_flash_erase_partition(CFG_PARTITION);
        err = nvs_flash_init_partition(CFG_PARTITION);
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t h;
    bool opened = open_ns(NVS_READONLY, &h) == ESP_OK;
    if (opened) {
        load_str(h, "hostname", s_cfg.hostname, sizeof(s_cfg.hostname), CONFIG_ENC_DEFAULT_HOSTNAME);
        load_str(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "");
        load_str(h, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "");
        load_str(h, "ota_url", s_cfg.ota_url, sizeof(s_cfg.ota_url), CONFIG_ENC_OTA_DEFAULT_URL);
        if (nvs_get_u16(h, "ota_int_h", &s_cfg.ota_interval_h) != ESP_OK) {
            s_cfg.ota_interval_h = CONFIG_ENC_OTA_DEFAULT_INTERVAL_H;
        }
        if (nvs_get_u8(h, "volume", &s_cfg.volume) != ESP_OK) {
            s_cfg.volume = CONFIG_ENC_DEFAULT_VOLUME;
        }
        if (nvs_get_u8(h, "max_vol", &s_cfg.max_volume) != ESP_OK) {
            s_cfg.max_volume = CONFIG_ENC_DEFAULT_MAX_VOLUME;
        }
        s_cfg.admin_set = blob_exists(h, "admin_pw");
        s_cfg.mpd_pass_set = blob_exists(h, "mpd_pw");
        nvs_close(h);
    } else {
        /* Namespace absent : sortie d'usine. */
        str_copy(s_cfg.hostname, CONFIG_ENC_DEFAULT_HOSTNAME, sizeof(s_cfg.hostname));
        str_copy(s_cfg.ota_url, CONFIG_ENC_OTA_DEFAULT_URL, sizeof(s_cfg.ota_url));
        s_cfg.ota_interval_h = CONFIG_ENC_OTA_DEFAULT_INTERVAL_H;
        s_cfg.volume = CONFIG_ENC_DEFAULT_VOLUME;
        s_cfg.max_volume = CONFIG_ENC_DEFAULT_MAX_VOLUME;
    }
    if (s_cfg.max_volume == 0 || s_cfg.max_volume > 100) {
        s_cfg.max_volume = 100;
    }
    if (s_cfg.volume > 100) {
        s_cfg.volume = CONFIG_ENC_DEFAULT_VOLUME;
    }

    const esp_timer_create_args_t targs = {.callback = volume_timer_cb, .name = "vol_save"};
    esp_timer_create(&targs, &s_volume_timer);

    ESP_LOGI(TAG, "nom=%s wifi=%s admin=%s mpd_pw=%s maj=%s", s_cfg.hostname,
             s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "(aucun)", s_cfg.admin_set ? "oui" : "non",
             s_cfg.mpd_pass_set ? "oui" : "non", s_cfg.ota_url[0] ? s_cfg.ota_url : "(aucune)");
    return ESP_OK;
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_set_hostname(const char *hostname)
{
    char norm[33];
    if (!hostname_normalize(hostname, norm, sizeof(norm))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_str("hostname", norm);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        str_copy(s_cfg.hostname, norm, sizeof(s_cfg.hostname));
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t settings_set_wifi(const char *ssid, const char *pass)
{
    size_t sl = strlen(ssid), pl = strlen(pass);
    if (sl == 0 || sl > 32 || pl > 64 || (pl > 0 && pl < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "wifi_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wifi_pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        str_copy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid));
        str_copy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass));
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t settings_set_ota(const char *url, uint16_t interval_h)
{
    if (strlen(url) >= sizeof(s_cfg.ota_url) || interval_h == 0 || interval_h > 24 * 30) {
        return ESP_ERR_INVALID_ARG;
    }
    if (url[0] && strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ota_url", url);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "ota_int_h", interval_h);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        str_copy(s_cfg.ota_url, url, sizeof(s_cfg.ota_url));
        s_cfg.ota_interval_h = interval_h;
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t settings_set_max_volume(uint8_t max_volume)
{
    if (max_volume == 0 || max_volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_u8("max_vol", max_volume);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.max_volume = max_volume;
        xSemaphoreGive(s_lock);
    }
    return err;
}

void settings_set_volume_deferred(uint8_t volume)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_cfg.volume != volume;
    s_cfg.volume = volume;
    xSemaphoreGive(s_lock);
    if (changed && s_volume_timer) {
        esp_timer_stop(s_volume_timer);
        esp_timer_start_once(s_volume_timer, 5 * 1000 * 1000);
    }
}

/* ---- Mots de passe ---- */

static void pw_derive(const char *password, const uint8_t *salt, uint32_t iterations, uint8_t *out)
{
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)password, strlen(password), salt,
                                  PW_SALT_LEN, iterations, PW_HASH_LEN, out);
}

static esp_err_t pw_store(const char *key, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (password == NULL || password[0] == '\0') {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        uint8_t blob[PW_BLOB_LEN];
        uint32_t it = PW_ITERATIONS;
        memcpy(blob, &it, 4);
        esp_fill_random(blob + 4, PW_SALT_LEN);
        pw_derive(password, blob + 4, it, blob + 4 + PW_SALT_LEN);
        err = nvs_set_blob(h, key, blob, sizeof(blob));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool pw_check(const char *key, const char *password)
{
    nvs_handle_t h;
    if (open_ns(NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t blob[PW_BLOB_LEN];
    size_t sz = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, key, blob, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(blob)) {
        return false;
    }
    uint32_t it;
    memcpy(&it, blob, 4);
    if (it == 0 || it > 1000000) {
        return false;
    }
    uint8_t hash[PW_HASH_LEN];
    pw_derive(password, blob + 4, it, hash);
    uint8_t diff = 0; /* comparaison en temps constant */
    for (int i = 0; i < PW_HASH_LEN; i++) {
        diff |= hash[i] ^ blob[4 + PW_SALT_LEN + i];
    }
    return diff == 0;
}

esp_err_t settings_set_admin_password(const char *password)
{
    size_t len = strlen(password);
    if (len < 6 || len > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = pw_store("admin_pw", password);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.admin_set = true;
        xSemaphoreGive(s_lock);
    }
    return err;
}

bool settings_check_admin_password(const char *password)
{
    return pw_check("admin_pw", password);
}

esp_err_t settings_set_mpd_password(const char *password)
{
    if (strlen(password) > 64 || strpbrk(password, "\r\n")) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = pw_store("mpd_pw", password);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.mpd_pass_set = password[0] != '\0';
        xSemaphoreGive(s_lock);
    }
    return err;
}

bool settings_check_mpd_password(const char *password)
{
    return pw_check("mpd_pw", password);
}

esp_err_t settings_factory_reset(void)
{
    ESP_LOGW(TAG, "réinitialisation usine");
    nvs_flash_deinit_partition(CFG_PARTITION);
    esp_err_t err = nvs_flash_erase_partition(CFG_PARTITION);
    nvs_flash_init_partition(CFG_PARTITION);
    return err;
}
