#pragma once
/*
 * Réglages et associations de cartes <-> JSON, pour les fichiers de la carte SD
 * (/.enceinte.json, .cartes.json de chaque dossier) et l'export/import de l'interface web.
 * Sans dépendance ESP-IDF hormis cJSON (testé sur PC).
 */
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "cards.h"
#include "settings.h"

#define CONFIG_FORMAT "enceinte-reglages"
#define CARDS_FORMAT "enceinte-cartes"
#define CONFIG_VERSION 1

typedef struct {
    settings_t s;     /* réglages (volume courant et indicateurs ignorés) */
    bool wifi;        /* réseau Wi-Fi fourni (nom et mot de passe) */
    bool admin_hash;  /* empreinte du mot de passe administrateur fournie */
    int8_t mpd_hash;  /* -1 : non fourni, 0 : pas de mot de passe MPD, 1 : empreinte fournie */
    uint8_t admin[SETTINGS_PW_HASH_LEN];
    uint8_t mpd[SETTINGS_PW_HASH_LEN];
} config_t;

/* Document complet : {"format", "version", "device", ...}. */
cJSON *config_document(const char *format, const char *device);
/* Vérifie le format et la version d'un document ; message en français sinon. */
bool config_document_check(const cJSON *root, const char *format, char *err, size_t errlen);

/* Objet "settings". secrets : mot de passe du Wi-Fi et empreintes des mots de passe. */
cJSON *config_to_json(const config_t *c, bool secrets);
/* Lit l'objet "settings" par-dessus *c : les clés absentes gardent leur valeur. Une valeur
 * invalide : false, message en français, et *c inchangé. */
bool config_from_json(const cJSON *obj, config_t *c, char *err, size_t errlen);

/* Une carte. with_folder : export complet (le fichier d'un dossier ne répète pas son chemin). */
cJSON *card_to_json(const card_entry_t *e, bool with_folder);
/* Clés de réglage absentes ou null : réglage général. UID mis en majuscules. */
bool card_from_json(const cJSON *obj, card_entry_t *e, bool with_folder, char *err, size_t errlen);
