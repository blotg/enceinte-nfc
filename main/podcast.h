#pragma once
/*
 * Podcasts : un dossier de la carte SD abonné à un flux RSS (fichier .podcast.json dans le
 * dossier, qui le suit quand on le déplace et voyage avec une carte SD clonée).
 *
 * Les nouveaux épisodes sont téléchargés chaque nuit (entre 2 h et 5 h, enceinte inactive), à
 * l'abonnement et sur demande ; si l'enceinte est éteinte la nuit, au plus tard 48 h après la
 * dernière vérification. Tous les épisodes du flux sont gardés, ou seulement les N plus
 * récents (les plus anciens sont alors supprimés). Les fichiers sont nommés
 * « AAAA-MM-JJ HHhMM - Titre » : une carte associée au dossier les joue dans l'ordre
 * chronologique, avec la reprise habituelle.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "rss.h"

#define PODCAST_FILE ".podcast.json"
#define PODCAST_BASE "Podcasts"
#define PODCAST_KEEP_DEFAULT 10 /* épisodes gardés ; 0 : tous */
#define PODCAST_KEEP_MAX 99999

typedef struct {
    char url[RSS_URL_MAX];
    char title[RSS_TITLE_MAX];
    int keep;           /* 0 : tous */
    int episodes;       /* épisodes présents dans le dossier */
    int64_t last_check; /* dernière lecture réussie du flux (0 : jamais) */
    char last_error[96];
    bool syncing;       /* téléchargement en cours pour ce dossier */
    int progress;       /* % de l'épisode en cours, -1 si inconnu */
    int dl_index;       /* épisode en cours de téléchargement (1 à dl_count), 0 : aucun */
    int dl_count;       /* épisodes à télécharger pendant cette vérification */
} podcast_info_t;

void podcast_start(void);
/*
 * Abonnement : crée « Podcasts/<nom> » (sans nom : provisoire, remplacé par le titre du flux
 * au premier téléchargement) et lance le téléchargement. folder_out : dossier créé.
 */
esp_err_t podcast_subscribe(const char *url, const char *name, int keep, char *folder_out, size_t len, char *err,
                            size_t errlen);
/* url NULL ou keep < 0 : inchangé. */
esp_err_t podcast_update(const char *folder, const char *url, int keep);
/* Désabonnement : les épisodes restent dans le dossier. */
esp_err_t podcast_unsubscribe(const char *folder);
bool podcast_is_folder(const char *folder);
bool podcast_get(const char *folder, podcast_info_t *out);
/* Vérifie maintenant (NULL : tous les podcasts), même pendant l'écoute. */
void podcast_sync_now(const char *folder);
/* Téléchargement en cours (la mise à jour du firmware attend). */
bool podcast_busy(void);
