#pragma once
/*
 * Associations carte NFC (UID hexadécimal) -> dossier de la carte SD.
 * Stockées en NVS (résistant aux coupures de courant).
 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "util.h"

#define UID_STR_MAX 21 /* UID de 10 octets max en hexadécimal */
#define CARDS_MAX 300

typedef struct {
    char uid[UID_STR_MAX];
    char folder[REL_PATH_MAX];
} card_entry_t;

esp_err_t cards_init(void);
bool cards_lookup(const char *uid, char *folder, size_t len);
esp_err_t cards_set(const char *uid, const char *folder);
esp_err_t cards_remove(const char *uid);
/* Copie de la liste (à libérer avec free). Retourne le nombre d'entrées. */
int cards_list(card_entry_t **out);
/* Met à jour les associations après renommage d'un dossier (ou d'un parent). */
void cards_on_folder_renamed(const char *old_rel, const char *new_rel);
