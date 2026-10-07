#include "resume_store.h"

#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "resume";
static const char *NS = "resume";

#define BLOB_MAX (14 + UID_STR_MAX + 2 * REL_PATH_MAX)

static void key_for(int slot, char key[16])
{
    snprintf(key, 16, "rp%u", (unsigned)slot & 0xFF);
}

static void pos_key_for(int slot, char key[16])
{
    snprintf(key, 16, "rq%u", (unsigned)slot & 0xFF);
}

void resume_store_load(resume_point_t *points, int count)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(CFG_PARTITION, NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* rien d'enregistré */
    }
    uint8_t blob[BLOB_MAX];
    int loaded = 0;
    for (int i = 0; i < count; i++) {
        char key[16];
        key_for(i, key);
        size_t len = sizeof(blob);
        if (nvs_get_blob(h, key, blob, &len) == ESP_OK && resume_decode(blob, len, &points[i])) {
            uint32_t pos;
            pos_key_for(i, key);
            if (nvs_get_u32(h, key, &pos) == ESP_OK) {
                points[i].position_ms = pos; /* position plus récente que le point complet */
            }
            loaded++;
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d position(s) de reprise rechargée(s)", loaded);
}

esp_err_t resume_store_save(int slot, const resume_point_t *p)
{
    uint8_t blob[BLOB_MAX];
    size_t len = resume_encode(p, blob, sizeof(blob));
    if (!len) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(CFG_PARTITION, NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[16];
    key_for(slot, key);
    err = nvs_set_blob(h, key, blob, len);
    if (err == ESP_OK) {
        pos_key_for(slot, key);
        err = nvs_set_u32(h, key, p->position_ms); /* garde les deux clés cohérentes */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t resume_store_save_position(int slot, uint32_t position_ms)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(CFG_PARTITION, NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[16];
    pos_key_for(slot, key);
    err = nvs_set_u32(h, key, position_ms);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t resume_store_erase(int slot)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(CFG_PARTITION, NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[16];
    key_for(slot, key);
    err = nvs_erase_key(h, key);
    pos_key_for(slot, key);
    esp_err_t err2 = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK && err2 != ESP_OK && err2 != ESP_ERR_NVS_NOT_FOUND) {
        err = err2;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
