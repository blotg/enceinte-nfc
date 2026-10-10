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
_Static_assert(PW_BLOB_LEN == SETTINGS_PW_HASH_LEN, "taille de l'empreinte");

#define VOLUME_SAVE_DELAY_US (1000 * 1000)

#ifdef CONFIG_ENC_DEFAULT_VOL_TOUCH
#define DEFAULT_VOL_TOUCH 1
#else
#define DEFAULT_VOL_TOUCH 0
#endif

static settings_t s_cfg;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_volume_timer;
static void (*s_observer)(void);

static void notify(void)
{
    if (s_observer) {
        s_observer();
    }
}

void settings_set_observer(void (*cb)(void))
{
    s_observer = cb;
}

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
        if (nvs_get_u32(h, "resume_s", &s_cfg.resume_timeout_s) != ESP_OK) {
            s_cfg.resume_timeout_s = CONFIG_ENC_RESUME_TIMEOUT_S;
        }
        uint8_t other = 0;
        nvs_get_u8(h, "resume_other", &other);
        s_cfg.resume_after_other = other != 0;
        uint8_t shuffle = 0;
        nvs_get_u8(h, "shuffle", &shuffle);
        s_cfg.shuffle = shuffle != 0;
        uint8_t repeat = 0;
        nvs_get_u8(h, "repeat", &repeat);
        s_cfg.repeat = repeat != 0;
        uint8_t https = 0;
        nvs_get_u8(h, "https", &https);
        s_cfg.https_enabled = https != 0;
        if (nvs_get_u8(h, "normalize", &s_cfg.normalize) != ESP_OK) {
            s_cfg.normalize = CONFIG_ENC_DEFAULT_NORMALIZE;
        }
        if (nvs_get_u8(h, "compress", &s_cfg.compress) != ESP_OK) {
            s_cfg.compress = CONFIG_ENC_DEFAULT_COMPRESS;
        }
        uint8_t st = 0;
        nvs_get_u8(h, "ip_static", &st);
        s_cfg.ip.static_ip = st != 0;
        nvs_get_u32(h, "ip_addr", &s_cfg.ip.address);
        nvs_get_u32(h, "ip_mask", &s_cfg.ip.netmask);
        nvs_get_u32(h, "ip_gw", &s_cfg.ip.gateway);
        nvs_get_u32(h, "ip_dns", &s_cfg.ip.dns);
        uint8_t touch = DEFAULT_VOL_TOUCH;
        nvs_get_u8(h, "vol_touch", &touch);
        s_cfg.vol_touch = touch != 0;
        if (nvs_get_u16(h, "touch_thr", &s_cfg.touch_threshold) != ESP_OK) {
            s_cfg.touch_threshold = CONFIG_ENC_DEFAULT_TOUCH_THRESHOLD;
        }
        if (nvs_get_u16(h, "touch_hold", &s_cfg.touch_hold_ms) != ESP_OK) {
            s_cfg.touch_hold_ms = CONFIG_ENC_DEFAULT_TOUCH_HOLD_MS;
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
        s_cfg.resume_timeout_s = CONFIG_ENC_RESUME_TIMEOUT_S;
        s_cfg.normalize = CONFIG_ENC_DEFAULT_NORMALIZE;
        s_cfg.compress = CONFIG_ENC_DEFAULT_COMPRESS;
        s_cfg.vol_touch = DEFAULT_VOL_TOUCH;
        s_cfg.touch_threshold = CONFIG_ENC_DEFAULT_TOUCH_THRESHOLD;
        s_cfg.touch_hold_ms = CONFIG_ENC_DEFAULT_TOUCH_HOLD_MS;
    }
    if (s_cfg.touch_threshold < TOUCH_THRESHOLD_MIN || s_cfg.touch_threshold > TOUCH_THRESHOLD_MAX) {
        s_cfg.touch_threshold = CONFIG_ENC_DEFAULT_TOUCH_THRESHOLD;
    }
    if (s_cfg.touch_hold_ms > TOUCH_HOLD_MAX_MS) {
        s_cfg.touch_hold_ms = CONFIG_ENC_DEFAULT_TOUCH_HOLD_MS;
    }
    if (s_cfg.max_volume == 0 || s_cfg.max_volume > 100) {
        s_cfg.max_volume = 100;
    }
    if (s_cfg.volume > 100) {
        s_cfg.volume = CONFIG_ENC_DEFAULT_VOLUME;
    }
    if (s_cfg.normalize > SOUND_LEVEL_MAX) {
        s_cfg.normalize = CONFIG_ENC_DEFAULT_NORMALIZE;
    }
    if (s_cfg.compress > SOUND_LEVEL_MAX) {
        s_cfg.compress = CONFIG_ENC_DEFAULT_COMPRESS;
    }
    if (s_cfg.ip.static_ip && !settings_ip_valid(&s_cfg.ip, NULL)) {
        ESP_LOGW(TAG, "adresse IP fixe enregistrée invalide : DHCP");
        s_cfg.ip.static_ip = false;
    }

    const esp_timer_create_args_t targs = {.callback = volume_timer_cb, .name = "vol_save"};
    esp_timer_create(&targs, &s_volume_timer);

    char ip[16] = "DHCP";
    if (s_cfg.ip.static_ip) {
        ip4_format(s_cfg.ip.address, ip);
    }
    ESP_LOGI(TAG, "nom=%s wifi=%s ip=%s admin=%s mpd_pw=%s maj=%s", s_cfg.hostname,
             s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "(aucun)", ip, s_cfg.admin_set ? "oui" : "non",
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
        notify();
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
        notify();
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
        notify();
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
        notify();
    }
    return err;
}

esp_err_t settings_set_resume(uint32_t timeout_s, bool after_other)
{
    if (timeout_s > 30 * 24 * 3600) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, "resume_s", timeout_s);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "resume_other", after_other ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.resume_timeout_s = timeout_s;
        s_cfg.resume_after_other = after_other;
        xSemaphoreGive(s_lock);
        notify();
    }
    return err;
}

esp_err_t settings_set_shuffle(bool on)
{
    esp_err_t err = save_u8("shuffle", on ? 1 : 0);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.shuffle = on;
        xSemaphoreGive(s_lock);
        notify();
    }
    return err;
}

esp_err_t settings_set_repeat(bool on)
{
    esp_err_t err = save_u8("repeat", on ? 1 : 0);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.repeat = on;
        xSemaphoreGive(s_lock);
        notify();
    }
    return err;
}

esp_err_t settings_set_https(bool enabled)
{
    esp_err_t err = save_u8("https", enabled ? 1 : 0);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.https_enabled = enabled;
        xSemaphoreGive(s_lock);
        notify();
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
        esp_timer_start_once(s_volume_timer, VOLUME_SAVE_DELAY_US);
    }
}

void settings_flush(void)
{
    if (s_volume_timer && esp_timer_is_active(s_volume_timer)) {
        esp_timer_stop(s_volume_timer);
        volume_timer_cb(NULL);
    }
}

esp_err_t settings_set_sound(uint8_t normalize, uint8_t compress)
{
    if (normalize > SOUND_LEVEL_MAX || compress > SOUND_LEVEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "normalize", normalize);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "compress", compress);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.normalize = normalize;
        s_cfg.compress = compress;
        xSemaphoreGive(s_lock);
        notify();
    }
    return err;
}

static esp_err_t write_ip(nvs_handle_t h, const ip_config_t *ip)
{
    esp_err_t err = nvs_set_u8(h, "ip_static", ip->static_ip ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "ip_addr", ip->address);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "ip_mask", ip->netmask);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "ip_gw", ip->gateway);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "ip_dns", ip->dns);
    }
    return err;
}

esp_err_t settings_set_ip(const ip_config_t *ip)
{
    if (!settings_ip_valid(ip, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = write_ip(h, ip);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.ip = *ip;
        xSemaphoreGive(s_lock);
        notify();
    }
    return err;
}

esp_err_t settings_set_controls(bool touch, uint16_t threshold, uint16_t hold_ms)
{
    if (threshold < TOUCH_THRESHOLD_MIN || threshold > TOUCH_THRESHOLD_MAX || hold_ms > TOUCH_HOLD_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "vol_touch", touch ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "touch_thr", threshold);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "touch_hold", hold_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg.vol_touch = touch;
        s_cfg.touch_threshold = threshold;
        s_cfg.touch_hold_ms = hold_ms;
        xSemaphoreGive(s_lock);
        notify();
    }
    return err;
}

esp_err_t settings_set_all(const settings_t *in)
{
    settings_t c = *in;
    size_t sl = strlen(c.wifi_ssid), pl = strlen(c.wifi_pass);
    if (!hostname_normalize(in->hostname, c.hostname, sizeof(c.hostname)) || sl > 32 || pl > 64 ||
        (sl == 0 && pl > 0) || (pl > 0 && pl < 8) || strlen(c.ota_url) >= sizeof(c.ota_url) ||
        (c.ota_url[0] && strncmp(c.ota_url, "http://", 7) != 0 && strncmp(c.ota_url, "https://", 8) != 0) ||
        c.ota_interval_h == 0 || c.ota_interval_h > 24 * 30 || c.resume_timeout_s > 30 * 24 * 3600 ||
        c.max_volume == 0 || c.max_volume > 100 || c.normalize > SOUND_LEVEL_MAX || c.compress > SOUND_LEVEL_MAX ||
        !settings_ip_valid(&c.ip, NULL) || c.touch_threshold < TOUCH_THRESHOLD_MIN ||
        c.touch_threshold > TOUCH_THRESHOLD_MAX || c.touch_hold_ms > TOUCH_HOLD_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    const struct {
        const char *key;
        uint8_t value;
    } u8s[] = {
        {"max_vol", c.max_volume},       {"resume_other", c.resume_after_other}, {"shuffle", c.shuffle},
        {"repeat", c.repeat},            {"https", c.https_enabled},      {"normalize", c.normalize},             {"compress", c.compress},
        {"vol_touch", c.vol_touch},
    };
    for (size_t i = 0; err == ESP_OK && i < sizeof(u8s) / sizeof(u8s[0]); i++) {
        err = nvs_set_u8(h, u8s[i].key, u8s[i].value);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "hostname", c.hostname);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wifi_ssid", c.wifi_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wifi_pass", c.wifi_pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ota_url", c.ota_url);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "ota_int_h", c.ota_interval_h);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "resume_s", c.resume_timeout_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "touch_thr", c.touch_threshold);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "touch_hold", c.touch_hold_ms);
    }
    if (err == ESP_OK) {
        err = write_ip(h, &c.ip);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c.volume = s_cfg.volume;
    c.admin_set = s_cfg.admin_set;
    c.mpd_pass_set = s_cfg.mpd_pass_set;
    s_cfg = c;
    xSemaphoreGive(s_lock);
    notify();
    return ESP_OK;
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

static bool pw_load(const char *key, uint8_t blob[PW_BLOB_LEN])
{
    nvs_handle_t h;
    if (open_ns(NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz = PW_BLOB_LEN;
    esp_err_t err = nvs_get_blob(h, key, blob, &sz);
    nvs_close(h);
    return err == ESP_OK && sz == PW_BLOB_LEN && settings_password_hash_valid(blob);
}

static bool pw_check(const char *key, const char *password)
{
    uint8_t blob[PW_BLOB_LEN];
    if (!pw_load(key, blob)) {
        return false;
    }
    uint32_t it;
    memcpy(&it, blob, 4);
    uint8_t hash[PW_HASH_LEN];
    pw_derive(password, blob + 4, it, hash);
    uint8_t diff = 0; /* comparaison en temps constant */
    for (int i = 0; i < PW_HASH_LEN; i++) {
        diff |= hash[i] ^ blob[4 + PW_SALT_LEN + i];
    }
    return diff == 0;
}

static esp_err_t pw_store_blob(const char *key, const uint8_t *blob)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (blob) {
        err = nvs_set_blob(h, key, blob, PW_BLOB_LEN);
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool settings_get_password_hash(bool admin, uint8_t out[SETTINGS_PW_HASH_LEN])
{
    return pw_load(admin ? "admin_pw" : "mpd_pw", out);
}

esp_err_t settings_set_password_hash(bool admin, const uint8_t *hash)
{
    if ((admin && !hash) || (hash && !settings_password_hash_valid(hash))) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t current[PW_BLOB_LEN];
    bool had = pw_load(admin ? "admin_pw" : "mpd_pw", current);
    if ((!hash && !had) || (hash && had && memcmp(hash, current, PW_BLOB_LEN) == 0)) {
        return ESP_OK; /* inchangé : pas d'écriture */
    }
    esp_err_t err = pw_store_blob(admin ? "admin_pw" : "mpd_pw", hash);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (admin) {
            s_cfg.admin_set = true;
        } else {
            s_cfg.mpd_pass_set = hash != NULL;
        }
        xSemaphoreGive(s_lock);
        notify();
    }
    return err;
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
        notify();
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
        notify();
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
    if (s_volume_timer) {
        esp_timer_stop(s_volume_timer);
    }
    nvs_flash_deinit_partition(CFG_PARTITION);
    esp_err_t err = nvs_flash_erase_partition(CFG_PARTITION);
    nvs_flash_init_partition(CFG_PARTITION);
    return err;
}
