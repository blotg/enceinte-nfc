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

#define CARD_DEFAULT -1 /* réglage par carte : utiliser le réglage général */

typedef struct {
    char uid[UID_STR_MAX];
    char folder[REL_PATH_MAX];
    int32_t resume_s;    /* délai de reprise : CARD_DEFAULT, 0 = toujours, sinon secondes */
    int8_t resume_other; /* après une autre carte : CARD_DEFAULT, 0 = recommencer, 1 = reprendre */
    int8_t shuffle;      /* ordre de lecture : CARD_DEFAULT, 0 = dans l'ordre, 1 = aléatoire */
} card_entry_t;

esp_err_t cards_init(void);
bool cards_get(const char *uid, card_entry_t *out);
esp_err_t cards_set(const card_entry_t *entry);
esp_err_t cards_remove(const char *uid);
/* Copie de la liste (à libérer avec free). Retourne le nombre d'entrées. */
int cards_list(card_entry_t **out);
/* Met à jour les associations après renommage d'un dossier (ou d'un parent). */
void cards_on_folder_renamed(const char *old_rel, const char *new_rel);
