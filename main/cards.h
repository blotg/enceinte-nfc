#pragma once
/*
 * Associations carte NFC (UID hexadécimal) -> dossier de la carte SD.
 * Stockées en NVS (résistant aux coupures de courant) et recopiées sur la carte SD, dans le
 * dossier de chaque playlist (cf. backup.h).
 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "settings.h"
#include "util.h"

#define UID_STR_MAX 21 /* UID de 10 octets max en hexadécimal */
#define CARDS_MAX 300

#define CARD_DEFAULT -1 /* réglage par carte : utiliser le réglage général */
#define SLEEP_TRACKS_MAX 999
#define SLEEP_MINUTES_MAX 720

typedef struct {
    char uid[UID_STR_MAX];
    char folder[REL_PATH_MAX];
    int32_t resume_s;    /* délai de reprise : CARD_DEFAULT, 0 = toujours, sinon secondes */
    int8_t resume_other; /* après une autre carte : CARD_DEFAULT, 0 = recommencer, 1 = reprendre */
    int8_t shuffle;      /* ordre de lecture : CARD_DEFAULT, 0 = dans l'ordre, 1 = aléatoire */
    int8_t normalize;    /* normalisation : CARD_DEFAULT ou 0 à SOUND_LEVEL_MAX */
    int8_t compress;     /* compression : CARD_DEFAULT ou 0 à SOUND_LEVEL_MAX */
    /* mode sommeil : pause après ce nombre de morceaux ou de minutes d'écoute (0 : jamais) */
    uint16_t sleep_tracks;
    uint16_t sleep_minutes;
} card_entry_t;

/* Réglages propres à la carte valides (dossier et UID exceptés). */
static inline bool cards_entry_valid(const card_entry_t *e)
{
    return e->resume_s >= CARD_DEFAULT && e->resume_s <= 30 * 24 * 3600 && e->resume_other >= CARD_DEFAULT &&
           e->resume_other <= 1 && e->shuffle >= CARD_DEFAULT && e->shuffle <= 1 && e->normalize >= CARD_DEFAULT &&
           e->normalize <= SOUND_LEVEL_MAX && e->compress >= CARD_DEFAULT && e->compress <= SOUND_LEVEL_MAX &&
           e->sleep_tracks <= SLEEP_TRACKS_MAX && e->sleep_minutes <= SLEEP_MINUTES_MAX;
}

esp_err_t cards_init(void);
bool cards_get(const char *uid, card_entry_t *out);
esp_err_t cards_set(const card_entry_t *entry);
esp_err_t cards_remove(const char *uid);
/* Copie de la liste (à libérer avec free). Retourne le nombre d'entrées. */
int cards_list(card_entry_t **out);
/* Idem pour les seules cartes d'un dossier (FAT ignore la casse). */
int cards_list_folder(const char *folder, card_entry_t **out);
/* Met à jour les associations après renommage d'un dossier (ou d'un parent). */
void cards_on_folder_renamed(const char *old_rel, const char *new_rel);
/* Dossier supprimé : ses associations (et celles de ses sous-dossiers) aussi. */
void cards_remove_under(const char *rel);
/* Remplace toutes les associations (carte SD, import). notify : prévenir l'observateur
 * pour chaque dossier concerné. */
esp_err_t cards_replace_all(const card_entry_t *list, int count, bool notify);
/* Appelée hors verrou pour chaque dossier dont les associations ont changé. */
void cards_set_observer(void (*cb)(const char *folder));
