#pragma once
/*
 * Carte SD (SPI, FAT32) montée sur MUSIC_ROOT.
 * Tous les chemins manipulés par l'application sont relatifs à la racine de la carte
 * et normalisés par path_sanitize().
 */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "util.h"

typedef struct {
    char *name;
    bool is_dir;
    uint32_t size;
    time_t mtime;
} dir_entry_t;

typedef struct {
    char **items; /* chemins relatifs, triés */
    int count;
} path_list_t;

/* Lance le montage (et les nouvelles tentatives si la carte est absente). */
esp_err_t storage_init(void);
bool storage_is_mounted(void);
bool storage_get_usage(uint64_t *total_bytes, uint64_t *free_bytes);
/* Signale une erreur d'E/S : au-delà d'un seuil la carte est remontée. */
void storage_report_io_error(void);

/* Contenu d'un dossier (non récursif) : dossiers puis fichiers, ordre naturel, cachés exclus. */
esp_err_t storage_list_dir(const char *rel_dir, dir_entry_t **entries, int *count);
void storage_free_dir(dir_entry_t *entries, int count);

/* Fichiers audio d'un dossier et de ses sous-dossiers, ordre naturel des chemins. */
esp_err_t storage_list_tracks(const char *rel_dir, path_list_t *out);
void path_list_free(path_list_t *list);

bool storage_is_dir(const char *rel);
bool storage_exists(const char *rel);
esp_err_t storage_mkdir(const char *rel);
esp_err_t storage_rename(const char *rel_from, const char *rel_to);
esp_err_t storage_remove_recursive(const char *rel);
