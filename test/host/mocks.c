/*
 * Simulations de la carte SD (dossier local MUSIC_ROOT) et des réglages pour les
 * tests d'intégration du lecteur et du serveur MPD sur PC.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "changes.h"
#include "settings.h"
#include "storage.h"

/* ---------- Réglages ---------- */

static char s_mpd_password[65];

void mock_set_mpd_password(const char *pw)
{
    snprintf(s_mpd_password, sizeof(s_mpd_password), "%s", pw ? pw : "");
}

void settings_get(settings_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->hostname, sizeof(out->hostname), "enceinte");
    out->max_volume = 100;
    out->volume = 30;
    out->admin_set = true;
    out->mpd_pass_set = s_mpd_password[0] != '\0';
}

bool settings_check_mpd_password(const char *password)
{
    return s_mpd_password[0] && strcmp(password, s_mpd_password) == 0;
}

void settings_set_volume_deferred(uint8_t volume)
{
}

/* ---------- Carte SD ---------- */

bool storage_is_mounted(void)
{
    return true;
}

void storage_report_io_error(void)
{
}

bool storage_get_usage(uint64_t *total, uint64_t *free_bytes)
{
    *total = 8ULL << 30;
    *free_bytes = 4ULL << 30;
    return true;
}

static int entry_cmp(const void *a, const void *b)
{
    const dir_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) {
        return x->is_dir ? -1 : 1;
    }
    return natural_casecmp(x->name, y->name);
}

esp_err_t storage_list_dir(const char *rel, dir_entry_t **entries, int *count)
{
    char abs[ABS_PATH_MAX];
    *entries = NULL;
    *count = 0;
    if (!path_to_abs(rel, abs, sizeof(abs))) {
        return ESP_ERR_INVALID_ARG;
    }
    DIR *d = opendir(abs);
    if (!d) {
        return ESP_ERR_NOT_FOUND;
    }
    int cap = 16, n = 0;
    dir_entry_t *list = malloc(cap * sizeof(dir_entry_t));
    struct dirent *de;
    while ((de = readdir(d))) {
        if (name_is_hidden(de->d_name)) {
            continue;
        }
        char p[ABS_PATH_MAX + 260];
        snprintf(p, sizeof(p), "%s/%s", abs, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) {
            continue;
        }
        if (n == cap) {
            cap *= 2;
            list = realloc(list, cap * sizeof(dir_entry_t));
        }
        list[n].name = strdup(de->d_name);
        list[n].is_dir = S_ISDIR(st.st_mode);
        list[n].size = (uint32_t)st.st_size;
        list[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    qsort(list, n, sizeof(dir_entry_t), entry_cmp);
    *entries = list;
    *count = n;
    return ESP_OK;
}

void storage_free_dir(dir_entry_t *entries, int count)
{
    for (int i = 0; entries && i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}

static void collect(const char *rel, path_list_t *out, int depth)
{
    dir_entry_t *e;
    int n;
    if (depth > 6 || storage_list_dir(rel, &e, &n) != ESP_OK) {
        return;
    }
    for (int i = 0; i < n; i++) {
        char child[REL_PATH_MAX];
        snprintf(child, sizeof(child), rel[0] ? "%s/%s" : "%s%s", rel, e[i].name);
        if (e[i].is_dir) {
            collect(child, out, depth + 1);
        } else if (is_audio_file(e[i].name)) {
            out->items = realloc(out->items, (out->count + 1) * sizeof(char *));
            out->items[out->count++] = strdup(child);
        }
    }
    storage_free_dir(e, n);
}

static int path_cmp(const void *a, const void *b)
{
    return natural_casecmp(*(char *const *)a, *(char *const *)b);
}

esp_err_t storage_list_tracks(const char *rel, path_list_t *out)
{
    out->items = NULL;
    out->count = 0;
    if (!storage_is_dir(rel)) {
        return ESP_ERR_NOT_FOUND;
    }
    collect(rel, out, 0);
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
    char abs[ABS_PATH_MAX];
    struct stat st;
    return path_to_abs(rel, abs, sizeof(abs)) && stat(abs, &st) == 0 && S_ISDIR(st.st_mode);
}

bool storage_exists(const char *rel)
{
    char abs[ABS_PATH_MAX];
    struct stat st;
    return path_to_abs(rel, abs, sizeof(abs)) && stat(abs, &st) == 0;
}
