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
    int32_t resume_s;
    int8_t resume_other;
    int8_t shuffle;
} entry_t;

static entry_t *s_entries;
static int s_count;
static SemaphoreHandle_t s_lock;

/* Format du blob : lignes "UID\tdossier\tdélai\tautre_carte\taléatoire\n" (champs
 * facultatifs après le dossier : absents des associations créées par les versions 1.0 à 1.3). */
static esp_err_t save_locked(void)
{
    size_t size = 1;
    for (int i = 0; i < s_count; i++) {
        size += strlen(s_entries[i].uid) + strlen(s_entries[i].folder) + 32;
    }
    char *blob = malloc(size);
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }
    size_t o = 0;
    for (int i = 0; i < s_count; i++) {
        o += sprintf(blob + o, "%s\t%s\t%ld\t%d\t%d\n", s_entries[i].uid, s_entries[i].folder,
                     (long)s_entries[i].resume_s, s_entries[i].resume_other, s_entries[i].shuffle);
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

static bool add_locked(const char *uid, const char *folder, int32_t resume_s, int8_t resume_other, int8_t shuffle)
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
    s_entries[s_count].resume_s = resume_s;
    s_entries[s_count].resume_other = resume_other;
    s_entries[s_count].shuffle = shuffle;
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
        char *fields[5] = {line, NULL, NULL, NULL, NULL};
        for (int i = 1; i < 5; i++) {
            char *tab = fields[i - 1] ? strchr(fields[i - 1], '\t') : NULL;
            if (tab) {
                *tab = '\0';
                fields[i] = tab + 1;
            }
        }
        if (!fields[1] || strlen(line) >= UID_STR_MAX || s_count >= CARDS_MAX) {
            continue;
        }
        int32_t resume_s = fields[2] ? (int32_t)strtol(fields[2], NULL, 10) : CARD_DEFAULT;
        int8_t other = fields[3] ? (int8_t)strtol(fields[3], NULL, 10) : CARD_DEFAULT;
        int8_t shuffle = fields[4] ? (int8_t)strtol(fields[4], NULL, 10) : CARD_DEFAULT;
        add_locked(line, fields[1], resume_s < 0 ? CARD_DEFAULT : resume_s, other < 0 || other > 1 ? CARD_DEFAULT : other,
                   shuffle < 0 || shuffle > 1 ? CARD_DEFAULT : shuffle);
    }
    free(blob);
    ESP_LOGI(TAG, "%d carte(s) associée(s)", s_count);
    return ESP_OK;
}

bool cards_get(const char *uid, card_entry_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_locked(uid);
    if (i >= 0) {
        str_copy(out->uid, s_entries[i].uid, sizeof(out->uid));
        str_copy(out->folder, s_entries[i].folder, sizeof(out->folder));
        out->resume_s = s_entries[i].resume_s;
        out->resume_other = s_entries[i].resume_other;
        out->shuffle = s_entries[i].shuffle;
    }
    xSemaphoreGive(s_lock);
    return i >= 0;
}

esp_err_t cards_set(const card_entry_t *e)
{
    const char *uid = e->uid, *folder = e->folder;
    if (!uid[0] || strlen(uid) >= UID_STR_MAX || strchr(folder, '\t') || strchr(folder, '\n') ||
        e->resume_s < CARD_DEFAULT || e->resume_other < CARD_DEFAULT || e->resume_other > 1 ||
        e->shuffle < CARD_DEFAULT || e->shuffle > 1) {
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
            s_entries[i].resume_s = e->resume_s;
            s_entries[i].resume_other = e->resume_other;
            s_entries[i].shuffle = e->shuffle;
        } else {
            err = ESP_ERR_NO_MEM;
        }
    } else if (s_count >= CARDS_MAX) {
        err = ESP_ERR_NO_MEM;
    } else if (!add_locked(uid, folder, e->resume_s, e->resume_other, e->shuffle)) {
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
        (*out)[i].resume_s = s_entries[i].resume_s;
        (*out)[i].resume_other = s_entries[i].resume_other;
        (*out)[i].shuffle = s_entries[i].shuffle;
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
