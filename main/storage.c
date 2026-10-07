#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_sdmmc.h" /* après ff.h (type BYTE) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#include "changes.h"

static const char *TAG = "storage";

#define MAX_TRACKS 2000
#define MAX_DEPTH 6

static sdmmc_card_t *s_card;
static volatile bool s_mounted;
static volatile int s_io_errors;
static char s_drive[8] = "0:";
static spi_host_device_t s_host = SPI2_HOST;

static esp_err_t do_mount(void)
{
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false, /* ne jamais formater la musique de l'utilisateur */
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_host;
    host.max_freq_khz = CONFIG_ENC_SD_FREQ_KHZ;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = CONFIG_ENC_SD_CS_GPIO;
    slot.host_id = s_host;
    esp_err_t err = esp_vfs_fat_sdspi_mount(MUSIC_ROOT, &host, &slot, &mount_cfg, &s_card);
    if (err == ESP_OK) {
        snprintf(s_drive, sizeof(s_drive), "%u:", ff_diskio_get_pdrv_card(s_card));
        s_mounted = true;
        s_io_errors = 0;
        ESP_LOGI(TAG, "carte SD montée (%s, %llu Mo)", s_card->cid.name,
                 ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
        changes_notify(CHG_DATABASE);
    }
    return err;
}

static void do_unmount(void)
{
    if (s_mounted) {
        s_mounted = false;
        esp_vfs_fat_sdcard_unmount(MUSIC_ROOT, s_card);
        s_card = NULL;
        changes_notify(CHG_DATABASE);
    }
}

static void storage_task(void *arg)
{
    int delay_ms = 2000;
    for (;;) {
        if (!s_mounted) {
            esp_err_t err = do_mount();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "carte SD absente ou illisible (%s), nouvel essai dans %d s", esp_err_to_name(err),
                         delay_ms / 1000);
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
                if (delay_ms < 10000) {
                    delay_ms += 2000;
                }
                continue;
            }
            delay_ms = 2000;
        }
        if (s_io_errors >= 3) {
            ESP_LOGW(TAG, "erreurs d'E/S répétées, remontage de la carte");
            do_unmount();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_io_errors > 0) {
            s_io_errors--; /* oubli progressif des erreurs isolées */
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t storage_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_ENC_SD_MOSI_GPIO,
        .miso_io_num = CONFIG_ENC_SD_MISO_GPIO,
        .sclk_io_num = CONFIG_ENC_SD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(s_host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus SPI : %s", esp_err_to_name(err));
        return err;
    }
    do_mount();
    xTaskCreate(storage_task, "storage", 4096, NULL, 3, NULL);
    return ESP_OK;
}

bool storage_is_mounted(void)
{
    return s_mounted;
}

void storage_report_io_error(void)
{
    s_io_errors++;
}

bool storage_get_usage(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted) {
        return false;
    }
    return esp_vfs_fat_info(MUSIC_ROOT, total_bytes, free_bytes) == ESP_OK;
}

/* ---- Accès FatFs direct (taille et date sans stat() par fichier) ---- */

static bool ff_path(const char *rel, char *out, size_t len)
{
    int n = snprintf(out, len, "%s/%s", s_drive, rel);
    return n > 0 && (size_t)n < len;
}

static time_t fat_time(WORD fdate, WORD ftime)
{
    if (fdate == 0) {
        return 0;
    }
    struct tm tm = {
        .tm_year = ((fdate >> 9) & 0x7F) + 80,
        .tm_mon = ((fdate >> 5) & 0xF) - 1,
        .tm_mday = fdate & 0x1F,
        .tm_hour = (ftime >> 11) & 0x1F,
        .tm_min = (ftime >> 5) & 0x3F,
        .tm_sec = (ftime & 0x1F) * 2,
        .tm_isdst = -1,
    };
    return mktime(&tm);
}

static int entry_cmp(const void *a, const void *b)
{
    const dir_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) {
        return x->is_dir ? -1 : 1;
    }
    return natural_casecmp(x->name, y->name);
}

esp_err_t storage_list_dir(const char *rel_dir, dir_entry_t **entries, int *count)
{
    *entries = NULL;
    *count = 0;
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[ABS_PATH_MAX];
    if (!ff_path(rel_dir, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }
    FF_DIR *dir = malloc(sizeof(FF_DIR));
    FILINFO *fi = malloc(sizeof(FILINFO));
    if (!dir || !fi) {
        free(dir);
        free(fi);
        return ESP_ERR_NO_MEM;
    }
    FRESULT fr = f_opendir(dir, path);
    if (fr != FR_OK) {
        free(dir);
        free(fi);
        if (fr == FR_DISK_ERR || fr == FR_NOT_READY) {
            storage_report_io_error();
        }
        return fr == FR_NO_PATH || fr == FR_NO_FILE ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    int cap = 32, n = 0;
    dir_entry_t *list = malloc(cap * sizeof(dir_entry_t));
    esp_err_t err = list ? ESP_OK : ESP_ERR_NO_MEM;
    while (err == ESP_OK) {
        fr = f_readdir(dir, fi);
        if (fr != FR_OK) {
            err = ESP_FAIL;
            break;
        }
        if (fi->fname[0] == '\0') {
            break;
        }
        if (name_is_hidden(fi->fname) || (fi->fattrib & (AM_HID | AM_SYS))) {
            continue;
        }
        if (n == cap) {
            cap *= 2;
            dir_entry_t *nl = realloc(list, cap * sizeof(dir_entry_t));
            if (!nl) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            list = nl;
        }
        list[n].name = strdup(fi->fname);
        if (!list[n].name) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        list[n].is_dir = (fi->fattrib & AM_DIR) != 0;
        list[n].size = (uint32_t)fi->fsize;
        list[n].mtime = fat_time(fi->fdate, fi->ftime);
        n++;
    }
    f_closedir(dir);
    free(dir);
    free(fi);
    if (err != ESP_OK) {
        storage_free_dir(list, n);
        return err;
    }
    qsort(list, n, sizeof(dir_entry_t), entry_cmp);
    *entries = list;
    *count = n;
    return ESP_OK;
}

void storage_free_dir(dir_entry_t *entries, int count)
{
    if (!entries) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}

static int path_cmp(const void *a, const void *b)
{
    return natural_casecmp(*(char *const *)a, *(char *const *)b);
}

static esp_err_t collect_tracks(const char *rel_dir, path_list_t *out, int *cap, int depth)
{
    dir_entry_t *entries;
    int n;
    esp_err_t err = storage_list_dir(rel_dir, &entries, &n);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < n && err == ESP_OK && out->count < MAX_TRACKS; i++) {
        char child[REL_PATH_MAX];
        int len = rel_dir[0] ? snprintf(child, sizeof(child), "%s/%s", rel_dir, entries[i].name)
                             : snprintf(child, sizeof(child), "%s", entries[i].name);
        if (len <= 0 || (size_t)len >= sizeof(child)) {
            continue;
        }
        if (entries[i].is_dir) {
            if (depth < MAX_DEPTH) {
                err = collect_tracks(child, out, cap, depth + 1);
            }
        } else if (is_audio_file(entries[i].name)) {
            if (out->count == *cap) {
                *cap = *cap ? *cap * 2 : 32;
                char **ni = realloc(out->items, *cap * sizeof(char *));
                if (!ni) {
                    err = ESP_ERR_NO_MEM;
                    break;
                }
                out->items = ni;
            }
            out->items[out->count] = strdup(child);
            if (!out->items[out->count]) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            out->count++;
        }
    }
    storage_free_dir(entries, n);
    return err;
}

esp_err_t storage_list_tracks(const char *rel_dir, path_list_t *out)
{
    out->items = NULL;
    out->count = 0;
    int cap = 0;
    esp_err_t err = collect_tracks(rel_dir, out, &cap, 0);
    if (err != ESP_OK) {
        path_list_free(out);
        return err;
    }
    qsort(out->items, out->count, sizeof(char *), path_cmp);
    return ESP_OK;
}

void path_list_free(path_list_t *list)
{
    for (int i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

bool storage_is_dir(const char *rel)
{
    if (!s_mounted) {
        return false;
    }
    if (rel[0] == '\0') {
        return true;
    }
    char path[ABS_PATH_MAX];
    struct stat st;
    return path_to_abs(rel, path, sizeof(path)) && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool storage_exists(const char *rel)
{
    char path[ABS_PATH_MAX];
    struct stat st;
    return s_mounted && path_to_abs(rel, path, sizeof(path)) && stat(path, &st) == 0;
}

esp_err_t storage_mkdir(const char *rel)
{
    char path[ABS_PATH_MAX];
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!rel[0] || !path_to_abs(rel, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mkdir(path, 0775) != 0) {
        return errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    changes_notify(CHG_DATABASE);
    return ESP_OK;
}

esp_err_t storage_rename(const char *rel_from, const char *rel_to)
{
    char from[ABS_PATH_MAX], to[ABS_PATH_MAX];
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!rel_from[0] || !rel_to[0] || !path_to_abs(rel_from, from, sizeof(from)) ||
        !path_to_abs(rel_to, to, sizeof(to))) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(to, &st) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rename(from, to) != 0) {
        return ESP_FAIL;
    }
    changes_notify(CHG_DATABASE);
    return ESP_OK;
}

typedef struct {
    char path[ABS_PATH_MAX];
    char fpath[ABS_PATH_MAX];
    char child[REL_PATH_MAX];
    FF_DIR dir;
    FILINFO fi;
} rm_ctx_t;

/* Tampons sur le tas : la récursion est appelée depuis la tâche HTTP. */
static esp_err_t remove_rec(const char *rel, int depth)
{
    rm_ctx_t *c = malloc(sizeof(rm_ctx_t));
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    struct stat st;
    if (!path_to_abs(rel, c->path, sizeof(c->path)) || !ff_path(rel, c->fpath, sizeof(c->fpath))) {
        err = ESP_ERR_INVALID_ARG;
    } else if (stat(c->path, &st) != 0) {
        err = ESP_ERR_NOT_FOUND;
    } else if (!S_ISDIR(st.st_mode)) {
        err = unlink(c->path) == 0 ? ESP_OK : ESP_FAIL;
    } else if (depth > MAX_DEPTH + 2) {
        err = ESP_FAIL;
    } else {
        /* Liste FatFs brute : inclut les fichiers cachés (._xxx de macOS) pour pouvoir vider le dossier. */
        for (;;) {
            if (f_opendir(&c->dir, c->fpath) != FR_OK) {
                err = ESP_FAIL;
                break;
            }
            bool found = false;
            while (f_readdir(&c->dir, &c->fi) == FR_OK && c->fi.fname[0]) {
                int len = snprintf(c->child, sizeof(c->child), "%s/%s", rel, c->fi.fname);
                if (len > 0 && (size_t)len < sizeof(c->child)) {
                    found = true;
                    break;
                }
            }
            f_closedir(&c->dir);
            if (!found) {
                break;
            }
            err = remove_rec(c->child, depth + 1);
            if (err != ESP_OK) {
                break;
            }
        }
        if (err == ESP_OK && rmdir(c->path) != 0) {
            err = ESP_FAIL;
        }
    }
    free(c);
    return err;
}

esp_err_t storage_remove_recursive(const char *rel)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!rel[0]) {
        return ESP_ERR_INVALID_ARG; /* jamais la racine */
    }
    esp_err_t err = remove_rec(rel, 0);
    changes_notify(CHG_DATABASE);
    return err;
}
