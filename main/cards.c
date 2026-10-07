#include "cards.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "cards";

typedef struct {
    char uid[UID_STR_MAX];
    char *folder;
} entry_t;

static entry_t *s_entries;
static int s_count;
static SemaphoreHandle_t s_lock;

/* Format du blob : lignes "UID\tdossier\n". */
static esp_err_t save_locked(void)
{
    size_t size = 1;
    for (int i = 0; i < s_count; i++) {
        size += strlen(s_entries[i].uid) + strlen(s_entries[i].folder) + 2;
    }
    char *blob = malloc(size);
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }
    size_t o = 0;
    for (int i = 0; i < s_count; i++) {
        o += sprintf(blob + o, "%s\t%s\n", s_entries[i].uid, s_entries[i].folder);
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(CFG_PARTITION, "cards", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "map", blob, o);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(blob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sauvegarde impossible : %s", esp_err_to_name(err));
    }
    return err;
}

static int find_locked(const char *uid)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].uid, uid) == 0) {
            return i;
        }
    }
    return -1;
}

static bool add_locked(const char *uid, const char *folder)
{
    entry_t *n = realloc(s_entries, (s_count + 1) * sizeof(entry_t));
    if (!n) {
        return false;
    }
    s_entries = n;
    char *f = strdup(folder);
    if (!f) {
        return false;
    }
    str_copy(s_entries[s_count].uid, uid, UID_STR_MAX);
    s_entries[s_count].folder = f;
    s_count++;
    return true;
}

esp_err_t cards_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open_from_partition(CFG_PARTITION, "cards", NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK; /* aucune carte enregistrée */
    }
    size_t size = 0;
    char *blob = NULL;
    if (nvs_get_blob(h, "map", NULL, &size) == ESP_OK && size > 0) {
        blob = malloc(size + 1);
        if (blob && nvs_get_blob(h, "map", blob, &size) == ESP_OK) {
            blob[size] = '\0';
        } else {
            free(blob);
            blob = NULL;
        }
    }
    nvs_close(h);
    if (!blob) {
        return ESP_OK;
    }
    char *save = NULL;
    for (char *line = strtok_r(blob, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab) {
            continue;
        }
        *tab = '\0';
        if (strlen(line) < UID_STR_MAX && s_count < CARDS_MAX) {
            add_locked(line, tab + 1);
        }
    }
    free(blob);
    ESP_LOGI(TAG, "%d carte(s) associée(s)", s_count);
    return ESP_OK;
}

bool cards_lookup(const char *uid, char *folder, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_locked(uid);
    if (i >= 0) {
        str_copy(folder, s_entries[i].folder, len);
    }
    xSemaphoreGive(s_lock);
    return i >= 0;
}

esp_err_t cards_set(const char *uid, const char *folder)
{
    if (!uid[0] || strlen(uid) >= UID_STR_MAX || strchr(folder, '\t') || strchr(folder, '\n')) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    int i = find_locked(uid);
    if (i >= 0) {
        char *f = strdup(folder);
        if (f) {
            free(s_entries[i].folder);
            s_entries[i].folder = f;
        } else {
            err = ESP_ERR_NO_MEM;
        }
    } else if (s_count >= CARDS_MAX) {
        err = ESP_ERR_NO_MEM;
    } else if (!add_locked(uid, folder)) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = save_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cards_remove(const char *uid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_locked(uid);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (i >= 0) {
        free(s_entries[i].folder);
        memmove(&s_entries[i], &s_entries[i + 1], (s_count - i - 1) * sizeof(entry_t));
        s_count--;
        err = save_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

int cards_list(card_entry_t **out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count;
    *out = calloc(n ? n : 1, sizeof(card_entry_t));
    if (!*out) {
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        str_copy((*out)[i].uid, s_entries[i].uid, UID_STR_MAX);
        str_copy((*out)[i].folder, s_entries[i].folder, REL_PATH_MAX);
    }
    xSemaphoreGive(s_lock);
    return n;
}

void cards_on_folder_renamed(const char *old_rel, const char *new_rel)
{
    size_t ol = strlen(old_rel);
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        const char *f = s_entries[i].folder;
        if (strncmp(f, old_rel, ol) == 0 && (f[ol] == '\0' || f[ol] == '/')) {
            char buf[REL_PATH_MAX];
            int n = snprintf(buf, sizeof(buf), "%s%s", new_rel, f + ol);
            if (n > 0 && (size_t)n < sizeof(buf)) {
                char *nf = strdup(buf);
                if (nf) {
                    free(s_entries[i].folder);
                    s_entries[i].folder = nf;
                    changed = true;
                }
            }
        }
    }
    if (changed) {
        save_locked();
    }
    xSemaphoreGive(s_lock);
}
