#include "cards.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "cards";

typedef struct {
    char uid[UID_STR_MAX];
    char *folder;
    int32_t resume_s;
    int8_t resume_other;
    int8_t shuffle;
    int8_t normalize;
    int8_t compress;
} entry_t;

static entry_t *s_entries;
static int s_count;
static SemaphoreHandle_t s_lock;
static void (*s_observer)(const char *folder);

/* Dossiers dont les associations ont changé, signalés une fois le verrou rendu. */
typedef struct {
    char **items;
    int count;
} changed_t;

static void changed_add(changed_t *c, const char *folder)
{
    if (!s_observer) {
        return;
    }
    for (int i = 0; i < c->count; i++) {
        if (strcasecmp(c->items[i], folder) == 0) {
            return;
        }
    }
    char **n = realloc(c->items, (c->count + 1) * sizeof(char *));
    if (!n) {
        return;
    }
    c->items = n;
    c->items[c->count] = strdup(folder);
    if (c->items[c->count]) {
        c->count++;
    }
}

static void changed_flush(changed_t *c)
{
    for (int i = 0; i < c->count; i++) {
        if (s_observer) {
            s_observer(c->items[i]);
        }
        free(c->items[i]);
    }
    free(c->items);
    c->items = NULL;
    c->count = 0;
}

static void entry_to_card(const entry_t *s, card_entry_t *d)
{
    str_copy(d->uid, s->uid, sizeof(d->uid));
    str_copy(d->folder, s->folder, sizeof(d->folder));
    d->resume_s = s->resume_s;
    d->resume_other = s->resume_other;
    d->shuffle = s->shuffle;
    d->normalize = s->normalize;
    d->compress = s->compress;
}

/* Format du blob : lignes "UID\tdossier\tdélai\tautre_carte\taléatoire\tnormalisation\tcompression\n".
 * Champs facultatifs après le dossier : absents des associations créées par les versions 1.0
 * à 1.3 (et ignorés par elles). */
static esp_err_t save_locked(void)
{
    size_t size = 1;
    for (int i = 0; i < s_count; i++) {
        size += strlen(s_entries[i].uid) + strlen(s_entries[i].folder) + 40;
    }
    char *blob = malloc(size);
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }
    size_t o = 0;
    for (int i = 0; i < s_count; i++) {
        const entry_t *e = &s_entries[i];
        o += sprintf(blob + o, "%s\t%s\t%ld\t%d\t%d\t%d\t%d\n", e->uid, e->folder, (long)e->resume_s, e->resume_other,
                     e->shuffle, e->normalize, e->compress);
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(CFG_PARTITION, "cards", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = o ? nvs_set_blob(h, "map", blob, o) : nvs_erase_key(h, "map");
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
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

static bool add_locked(const card_entry_t *e)
{
    entry_t *n = realloc(s_entries, (s_count + 1) * sizeof(entry_t));
    if (!n) {
        return false;
    }
    s_entries = n;
    char *f = strdup(e->folder);
    if (!f) {
        return false;
    }
    entry_t *d = &s_entries[s_count];
    str_copy(d->uid, e->uid, UID_STR_MAX);
    d->folder = f;
    d->resume_s = e->resume_s;
    d->resume_other = e->resume_other;
    d->shuffle = e->shuffle;
    d->normalize = e->normalize;
    d->compress = e->compress;
    s_count++;
    return true;
}

static void remove_at_locked(int i)
{
    free(s_entries[i].folder);
    memmove(&s_entries[i], &s_entries[i + 1], (s_count - i - 1) * sizeof(entry_t));
    s_count--;
}

static int8_t parse_opt(const char *field, int max)
{
    if (!field) {
        return CARD_DEFAULT;
    }
    long v = strtol(field, NULL, 10);
    return v < 0 || v > max ? CARD_DEFAULT : (int8_t)v;
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
        char *fields[7] = {line};
        for (int i = 1; i < 7; i++) {
            char *tab = fields[i - 1] ? strchr(fields[i - 1], '\t') : NULL;
            if (tab) {
                *tab = '\0';
                fields[i] = tab + 1;
            }
        }
        if (!fields[1] || strlen(line) >= UID_STR_MAX || s_count >= CARDS_MAX) {
            continue;
        }
        card_entry_t e = {
            .resume_s = fields[2] ? (int32_t)strtol(fields[2], NULL, 10) : CARD_DEFAULT,
            .resume_other = parse_opt(fields[3], 1),
            .shuffle = parse_opt(fields[4], 1),
            .normalize = parse_opt(fields[5], SOUND_LEVEL_MAX),
            .compress = parse_opt(fields[6], SOUND_LEVEL_MAX),
        };
        if (e.resume_s < 0) {
            e.resume_s = CARD_DEFAULT;
        }
        str_copy(e.uid, line, sizeof(e.uid));
        str_copy(e.folder, fields[1], sizeof(e.folder));
        add_locked(&e);
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
        entry_to_card(&s_entries[i], out);
    }
    xSemaphoreGive(s_lock);
    return i >= 0;
}

esp_err_t cards_set(const card_entry_t *e)
{
    if (!e->uid[0] || strlen(e->uid) >= UID_STR_MAX || !e->folder[0] || strchr(e->folder, '\t') ||
        strchr(e->folder, '\n') || !cards_entry_valid(e)) {
        return ESP_ERR_INVALID_ARG;
    }
    changed_t changed = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    int i = find_locked(e->uid);
    if (i >= 0) {
        char *f = strdup(e->folder);
        if (f) {
            changed_add(&changed, s_entries[i].folder);
            free(s_entries[i].folder);
            s_entries[i].folder = f;
            s_entries[i].resume_s = e->resume_s;
            s_entries[i].resume_other = e->resume_other;
            s_entries[i].shuffle = e->shuffle;
            s_entries[i].normalize = e->normalize;
            s_entries[i].compress = e->compress;
        } else {
            err = ESP_ERR_NO_MEM;
        }
    } else if (s_count >= CARDS_MAX) {
        err = ESP_ERR_NO_MEM;
    } else if (!add_locked(e)) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        changed_add(&changed, e->folder);
        err = save_locked();
    }
    xSemaphoreGive(s_lock);
    changed_flush(&changed);
    return err;
}

esp_err_t cards_remove(const char *uid)
{
    changed_t changed = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_locked(uid);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (i >= 0) {
        changed_add(&changed, s_entries[i].folder);
        remove_at_locked(i);
        err = save_locked();
    }
    xSemaphoreGive(s_lock);
    changed_flush(&changed);
    return err;
}

static int list_locked(const char *folder, card_entry_t **out)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        n += !folder || strcasecmp(s_entries[i].folder, folder) == 0;
    }
    *out = calloc(n ? n : 1, sizeof(card_entry_t));
    if (!*out) {
        return 0;
    }
    int k = 0;
    for (int i = 0; i < s_count && k < n; i++) {
        if (!folder || strcasecmp(s_entries[i].folder, folder) == 0) {
            entry_to_card(&s_entries[i], &(*out)[k++]);
        }
    }
    return k;
}

int cards_list(card_entry_t **out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = list_locked(NULL, out);
    xSemaphoreGive(s_lock);
    return n;
}

int cards_list_folder(const char *folder, card_entry_t **out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = list_locked(folder, out);
    xSemaphoreGive(s_lock);
    return n;
}

static bool under(const char *path, const char *dir)
{
    size_t dl = strlen(dir);
    return strncmp(path, dir, dl) == 0 && (path[dl] == '\0' || path[dl] == '/');
}

void cards_on_folder_renamed(const char *old_rel, const char *new_rel)
{
    size_t ol = strlen(old_rel);
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        const char *f = s_entries[i].folder;
        if (under(f, old_rel)) {
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
    /* Pas d'observateur : le fichier des associations suit le dossier sur la carte SD. */
    if (changed) {
        save_locked();
    }
    xSemaphoreGive(s_lock);
}

void cards_remove_under(const char *rel)
{
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = s_count - 1; i >= 0; i--) {
        if (under(s_entries[i].folder, rel)) {
            ESP_LOGI(TAG, "dossier supprimé : association de la carte %s retirée", s_entries[i].uid);
            remove_at_locked(i);
            changed = true;
        }
    }
    if (changed) {
        save_locked();
    }
    xSemaphoreGive(s_lock);
}

esp_err_t cards_replace_all(const card_entry_t *list, int count, bool notify)
{
    if (count > CARDS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < count; i++) {
        if (!list[i].uid[0] || !list[i].folder[0] || strchr(list[i].folder, '\t') || strchr(list[i].folder, '\n') ||
            !cards_entry_valid(&list[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    changed_t changed = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (notify) {
            changed_add(&changed, s_entries[i].folder);
        }
        free(s_entries[i].folder);
    }
    free(s_entries);
    s_entries = NULL;
    s_count = 0;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < count && err == ESP_OK; i++) {
        if (find_locked(list[i].uid) >= 0) {
            continue; /* doublon : la première association l'emporte */
        }
        if (!add_locked(&list[i])) {
            err = ESP_ERR_NO_MEM;
        } else if (notify) {
            changed_add(&changed, list[i].folder);
        }
    }
    esp_err_t serr = save_locked();
    xSemaphoreGive(s_lock);
    changed_flush(&changed);
    return err != ESP_OK ? err : serr;
}

void cards_set_observer(void (*cb)(const char *folder))
{
    s_observer = cb;
}
