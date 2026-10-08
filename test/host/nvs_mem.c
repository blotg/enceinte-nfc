/* NVS simulée : entrées en mémoire, recopiées dans le fichier $NVS_FILE à chaque validation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"

#define MAX_ENTRIES 64
#define MAX_NS 8

typedef struct {
    char ns[16];
    char key[16];
    size_t len;
    uint8_t *data;
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
static int s_count;
static char s_ns[MAX_NS][16];
static int s_nns;
static int s_loaded;

static void load(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = 1;
    const char *path = getenv("NVS_FILE");
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) {
        return;
    }
    entry_t e;
    while (s_count < MAX_ENTRIES && fread(e.ns, 16, 1, f) == 1 && fread(e.key, 16, 1, f) == 1 &&
           fread(&e.len, sizeof(e.len), 1, f) == 1) {
        e.data = malloc(e.len ? e.len : 1);
        if (fread(e.data, 1, e.len, f) != e.len) {
            free(e.data);
            break;
        }
        s_entries[s_count++] = e;
    }
    fclose(f);
}

static void save(void)
{
    const char *path = getenv("NVS_FILE");
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (!f) {
        return;
    }
    for (int i = 0; i < s_count; i++) {
        fwrite(s_entries[i].ns, 16, 1, f);
        fwrite(s_entries[i].key, 16, 1, f);
        fwrite(&s_entries[i].len, sizeof(size_t), 1, f);
        fwrite(s_entries[i].data, 1, s_entries[i].len, f);
    }
    fclose(f);
}

static entry_t *find(const char *ns, const char *key)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].ns, ns) == 0 && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

esp_err_t nvs_open_from_partition(const char *part, const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    load();
    int exists = 0;
    for (int i = 0; i < s_count; i++) {
        exists |= strcmp(s_entries[i].ns, ns) == 0;
    }
    if (mode == NVS_READONLY && !exists) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    for (int i = 0; i < s_nns; i++) {
        if (strcmp(s_ns[i], ns) == 0) {
            *h = (nvs_handle_t)i;
            return ESP_OK;
        }
    }
    snprintf(s_ns[s_nns], 16, "%s", ns);
    *h = (nvs_handle_t)s_nns++;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    entry_t *e = find(s_ns[h], key);
    if (!e) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out) {
        if (*len < e->len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, e->data, e->len);
    }
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    entry_t *e = find(s_ns[h], key);
    if (!e) {
        if (s_count >= MAX_ENTRIES) {
            return ESP_ERR_NO_MEM;
        }
        e = &s_entries[s_count++];
        snprintf(e->ns, 16, "%s", s_ns[h]);
        snprintf(e->key, 16, "%s", key);
        e->data = NULL;
    }
    free(e->data);
    e->data = malloc(len ? len : 1);
    memcpy(e->data, value, len);
    e->len = len;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    size_t len = 1;
    return nvs_get_blob(h, key, out, &len);
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    return nvs_set_blob(h, key, &value, 1);
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    entry_t *e = find(s_ns[h], key);
    if (!e) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    free(e->data);
    *e = s_entries[--s_count];
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    save();
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
}
