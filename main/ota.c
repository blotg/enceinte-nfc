#include "ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "controller.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "player.h"
#include "settings.h"
#include "util.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "ota";

#define BIT_CHECK BIT0
#define FIRST_CHECK_DELAY_MS (60 * 1000)
#define IDLE_POLL_MS (30 * 1000)
#define MANIFEST_MAX 2048

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_lock;
static ota_status_t s_st;
static esp_timer_handle_t s_restart_timer;

static void set_state(ota_state_t state, int progress, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_state(ota_state_t state, int progress, const char *fmt, ...)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.state = state;
    s_st.progress = progress;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s_st.message, sizeof(s_st.message), fmt, ap);
        va_end(ap);
    }
    xSemaphoreGive(s_lock);
}

static void restart_cb(void *arg)
{
    settings_flush(); /* volume modifié dans la dernière seconde */
    esp_restart();
}

void ota_schedule_restart(uint32_t delay_ms)
{
    if (!s_restart_timer) {
        const esp_timer_create_args_t a = {.callback = restart_cb, .name = "restart"};
        esp_timer_create(&a, &s_restart_timer);
    }
    esp_timer_stop(s_restart_timer);
    esp_timer_start_once(s_restart_timer, (uint64_t)delay_ms * 1000);
}

/* ---- Manifeste ---- */

static esp_err_t http_get(const char *url, char *buf, size_t cap, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            ESP_LOGW(TAG, "manifeste : HTTP %d", status);
            err = ESP_ERR_NOT_FOUND;
        } else {
            size_t total = 0;
            int n;
            while (total < cap - 1 && (n = esp_http_client_read(c, buf + total, cap - 1 - total)) > 0) {
                total += n;
            }
            buf[total] = '\0';
            *out_len = total;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

/* URL du binaire : absolue, ou relative au dossier du manifeste. */
static bool resolve_url(const char *manifest_url, const char *ref, char *out, size_t len)
{
    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        str_copy(out, ref, len);
        return strlen(ref) < len;
    }
    const char *slash = strrchr(manifest_url, '/');
    if (!slash) {
        return false;
    }
    int n = snprintf(out, len, "%.*s/%s", (int)(slash - manifest_url), manifest_url, ref);
    return n > 0 && (size_t)n < len;
}

static bool fetch_manifest(const char *url, char *version, size_t vlen, char *bin_url, size_t blen)
{
    char *buf = malloc(MANIFEST_MAX);
    if (!buf) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = http_get(url, buf, MANIFEST_MAX, &len);
    bool ok = false;
    if (err != ESP_OK) {
        set_state(OTA_ERROR, 0, "serveur injoignable (%s)", esp_err_to_name(err));
    } else {
        cJSON *root = cJSON_Parse(buf);
        const cJSON *v = cJSON_GetObjectItem(root, "version");
        const cJSON *u = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(v) && cJSON_IsString(u)) {
            str_copy(version, v->valuestring, vlen);
            ok = resolve_url(url, u->valuestring, bin_url, blen);
        }
        if (!ok) {
            set_state(OTA_ERROR, 0, "manifeste invalide");
        }
        cJSON_Delete(root);
    }
    free(buf);
    return ok;
}

/* ---- Téléchargement ---- */

/*
 * Source GitHub : "https://github.com/<compte>/<dépôt>". GitHub redirige
 * .../releases/latest/download/<fichier> vers le fichier de la dernière release
 * stable (ni brouillon ni pré-version).
 */
#define GITHUB_PREFIX "https://github.com/"
#define GITHUB_ASSET "enceinte.bin"

static bool github_bin_url(const char *url, char *out, size_t len)
{
    if (strncmp(url, GITHUB_PREFIX, strlen(GITHUB_PREFIX)) != 0) {
        return false;
    }
    char repo[128];
    str_copy(repo, url + strlen(GITHUB_PREFIX), sizeof(repo));
    size_t n = strlen(repo);
    while (n > 0 && repo[n - 1] == '/') {
        repo[--n] = '\0';
    }
    if (n > 4 && strcmp(repo + n - 4, ".git") == 0) {
        repo[n - 4] = '\0';
    }
    const char *slash = strchr(repo, '/');
    if (!slash || slash == repo || !slash[1] || strchr(slash + 1, '/')) {
        return false; /* il faut exactement compte/dépôt */
    }
    int w = snprintf(out, len, GITHUB_PREFIX "%s/releases/latest/download/" GITHUB_ASSET, repo);
    return w > 0 && (size_t)w < len;
}

static void http_config(esp_http_client_config_t *http, const char *url)
{
    memset(http, 0, sizeof(*http));
    http->url = url;
    http->timeout_ms = 15000;
    http->crt_bundle_attach = esp_crt_bundle_attach;
    http->keep_alive_enable = true;
    http->buffer_size = 4096;    /* en-tête Location des redirections GitHub (adresse signée longue) */
    http->buffer_size_tx = 2048; /* requête vers cette longue adresse */
}

/* Lit la version inscrite dans le binaire distant sans le télécharger en entier. */
static bool probe_image(const char *bin_url, char *version, size_t vlen)
{
    esp_http_client_config_t http;
    http_config(&http, bin_url);
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        set_state(OTA_ERROR, 0, "aucune release disponible ou GitHub injoignable (%s)", esp_err_to_name(err));
        return false;
    }
    esp_app_desc_t desc;
    err = esp_https_ota_get_img_desc(h, &desc);
    esp_https_ota_abort(h);
    if (err != ESP_OK) {
        set_state(OTA_ERROR, 0, "release illisible (%s)", esp_err_to_name(err));
        return false;
    }
    if (strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        set_state(OTA_ERROR, 0, "la release contient un autre projet (%s)", desc.project_name);
        return false;
    }
    str_copy(version, desc.version, vlen);
    return true;
}

static bool device_idle(void)
{
    player_status_t ps;
    player_get_status(&ps);
    controller_status_t cs;
    controller_get_status(&cs);
    /* Une pause longue (reprise « toujours ») ne bloque pas les mises à jour indéfiniment. */
    return !cs.present_uid[0] && !web_server_busy() &&
           (ps.state == PLAYER_STOPPED || (ps.state == PLAYER_PAUSED && ps.paused_s >= 2 * 3600));
}

static void install(const char *bin_url, const char *expected_version)
{
    set_state(OTA_DOWNLOADING, 0, "téléchargement de la version %s", expected_version);
    esp_http_client_config_t http;
    http_config(&http, bin_url);
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        set_state(OTA_ERROR, 0, "téléchargement impossible (%s)", esp_err_to_name(err));
        return;
    }
    esp_app_desc_t desc;
    err = esp_https_ota_get_img_desc(h, &desc);
    if (err == ESP_OK && strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        set_state(OTA_ERROR, 0, "firmware d'un autre projet (%s)", desc.project_name);
        esp_https_ota_abort(h);
        return;
    }
    if (err == ESP_OK && semver_cmp(desc.version, s_st.current_version) <= 0) {
        /* la release a pu changer depuis la vérification */
        set_state(OTA_IDLE, 0, "à jour (dernière version : %s)", desc.version);
        esp_https_ota_abort(h);
        return;
    }
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int total = esp_https_ota_get_image_size(h);
        int read = esp_https_ota_get_image_len_read(h);
        if (total > 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.progress = read * 100 / total;
            xSemaphoreGive(s_lock);
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        set_state(OTA_ERROR, 0, "téléchargement interrompu (%s)", esp_err_to_name(err));
        return;
    }
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        set_state(OTA_ERROR, 0, "image invalide (%s)", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "mise à jour %s installée, redémarrage", expected_version);
    set_state(OTA_REBOOTING, 100, "version %s installée, redémarrage", expected_version);
    ota_schedule_restart(2000);
}

static void ota_task(void *arg)
{
    TickType_t wait = pdMS_TO_TICKS(FIRST_CHECK_DELAY_MS);
    char version[32], bin_url[320];
    for (;;) {
        xEventGroupWaitBits(s_ev, BIT_CHECK, pdTRUE, pdFALSE, wait);
        settings_t cfg;
        settings_get(&cfg);
        wait = pdMS_TO_TICKS((uint64_t)cfg.ota_interval_h * 3600 * 1000);
        wifi_status_t ws;
        wifi_mgr_get_status(&ws);
        if (!cfg.ota_url[0]) {
            set_state(OTA_IDLE, 0, "aucune source de mise à jour configurée");
            continue;
        }
        if (!ws.sta_connected) {
            set_state(OTA_IDLE, 0, "pas de connexion réseau");
            wait = pdMS_TO_TICKS(10 * 60 * 1000); /* réessai dans 10 min */
            continue;
        }
        set_state(OTA_CHECKING, 0, "vérification...");
        bool ok;
        if (github_bin_url(cfg.ota_url, bin_url, sizeof(bin_url))) {
            ok = probe_image(bin_url, version, sizeof(version));
        } else {
            ok = fetch_manifest(cfg.ota_url, version, sizeof(version), bin_url, sizeof(bin_url));
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.last_check = time(NULL);
        xSemaphoreGive(s_lock);
        if (!ok) {
            continue;
        }
        if (semver_cmp(version, s_st.current_version) <= 0) {
            set_state(OTA_IDLE, 0, "à jour (dernière version : %s)", version);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.available_version[0] = '\0';
            xSemaphoreGive(s_lock);
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        str_copy(s_st.available_version, version, sizeof(s_st.available_version));
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "version %s disponible", version);
        /* Ne jamais couper la musique : attendre que l'enceinte soit inactive. */
        while (!device_idle()) {
            set_state(OTA_WAITING_IDLE, 0, "version %s disponible, installation dès que l'enceinte sera inactive",
                      version);
            if (xEventGroupWaitBits(s_ev, BIT_CHECK, pdFALSE, pdFALSE, pdMS_TO_TICKS(IDLE_POLL_MS)) & BIT_CHECK) {
                break; /* nouvelle vérification demandée */
            }
        }
        if (xEventGroupGetBits(s_ev) & BIT_CHECK) {
            wait = 0;
            continue;
        }
        install(bin_url, version);
    }
}

void ota_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_ev = xEventGroupCreate();
    const esp_app_desc_t *app = esp_app_get_description();
    str_copy(s_st.current_version, app->version, sizeof(s_st.current_version));
    str_copy(s_st.message, "en attente", sizeof(s_st.message));
    xTaskCreate(ota_task, "ota", 8192, NULL, 3, NULL);
}

void ota_confirm_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "nouveau firmware validé");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void ota_check_now(void)
{
    xEventGroupSetBits(s_ev, BIT_CHECK);
}

void ota_get_status(ota_status_t *st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *st = s_st;
    xSemaphoreGive(s_lock);
}

/* ---- Envoi manuel ---- */

#define HDR_CHECK_LEN \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static esp_ota_handle_t s_up;
static const esp_partition_t *s_up_part;
static uint8_t *s_up_hdr;
static size_t s_up_hdr_len;
static bool s_up_checked;
static size_t s_up_size, s_up_written;

esp_err_t ota_upload_begin(size_t size)
{
    s_up_part = esp_ota_get_next_update_partition(NULL);
    if (!s_up_part || size > s_up_part->size || size < HDR_CHECK_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_up_hdr = malloc(HDR_CHECK_LEN);
    if (!s_up_hdr) {
        return ESP_ERR_NO_MEM;
    }
    /* Effacement au fil de l'eau : pas de longue pause avant la réception. */
    esp_err_t err = esp_ota_begin(s_up_part, OTA_WITH_SEQUENTIAL_WRITES, &s_up);
    if (err != ESP_OK) {
        free(s_up_hdr);
        s_up_hdr = NULL;
        return err;
    }
    s_up_hdr_len = 0;
    s_up_checked = false;
    s_up_size = size;
    s_up_written = 0;
    set_state(OTA_DOWNLOADING, 0, "réception du firmware");
    return ESP_OK;
}

static esp_err_t check_header(void)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)s_up_hdr;
    const esp_app_desc_t *d =
        (const esp_app_desc_t *)(s_up_hdr + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name)) != 0) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

esp_err_t ota_upload_write(const uint8_t *data, size_t len)
{
    if (!s_up_hdr && !s_up_checked) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_up_checked) {
        size_t take = HDR_CHECK_LEN - s_up_hdr_len;
        if (take > len) {
            take = len;
        }
        memcpy(s_up_hdr + s_up_hdr_len, data, take);
        s_up_hdr_len += take;
        data += take;
        len -= take;
        if (s_up_hdr_len < HDR_CHECK_LEN) {
            return ESP_OK;
        }
        esp_err_t err = check_header();
        if (err != ESP_OK) {
            return err;
        }
        err = esp_ota_write(s_up, s_up_hdr, s_up_hdr_len);
        free(s_up_hdr);
        s_up_hdr = NULL;
        s_up_checked = true;
        s_up_written += s_up_hdr_len;
        if (err != ESP_OK) {
            return err;
        }
    }
    if (len == 0) {
        return ESP_OK;
    }
    esp_err_t err = esp_ota_write(s_up, data, len);
    s_up_written += len;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.progress = s_up_size ? (int)(s_up_written * 100 / s_up_size) : 0;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ota_upload_end(bool commit, char *msg, size_t msglen)
{
    free(s_up_hdr);
    s_up_hdr = NULL;
    if (!commit) {
        esp_ota_abort(s_up);
        set_state(OTA_ERROR, 0, "envoi du firmware interrompu");
        str_copy(msg, "envoi interrompu", msglen);
        return ESP_FAIL;
    }
    esp_err_t err = esp_ota_end(s_up);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(s_up_part);
    }
    if (err != ESP_OK) {
        set_state(OTA_ERROR, 0, "firmware refusé (%s)", esp_err_to_name(err));
        snprintf(msg, msglen, "firmware refusé (%s)", esp_err_to_name(err));
        return err;
    }
    set_state(OTA_REBOOTING, 100, "firmware installé, redémarrage");
    str_copy(msg, "firmware installé, redémarrage", msglen);
    ota_schedule_restart(1500);
    return ESP_OK;
}
