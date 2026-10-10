#pragma once
/*
 * Podcasts : outils sans dépendance ESP-IDF (testés sur PC).
 *  - lecture d'un flux RSS 2.0 au fil de l'eau (le flux arrive par morceaux, il peut peser
 *    plusieurs Mo : rien n'est gardé hormis les champs utiles) ;
 *  - dates RFC 822 (« Wed, 08 Oct 2026 06:00:00 +0200 ») ;
 *  - nom de fichier d'un épisode (« 2026-10-08 06h00 - Titre.mp3 ») : l'ordre alphabétique
 *    de la carte SD est l'ordre chronologique ;
 *  - choix des épisodes à garder.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RSS_TITLE_MAX 160
#define RSS_GUID_MAX 200
#define RSS_URL_MAX 512

typedef struct {
    char title[RSS_TITLE_MAX];
    char guid[RSS_GUID_MAX]; /* identifiant ; à défaut, l'adresse du fichier */
    char url[RSS_URL_MAX];   /* fichier audio (<enclosure url=...>) */
    char type[48];
    int64_t pub;  /* date de publication (UTC), 0 si inconnue */
    char day[11]; /* "AAAA-MM-JJ", tel qu'écrit dans le flux */
    char hm[6];   /* "HHhMM" */
} rss_item_t;

typedef struct rss_parser rss_parser_t;

/* Appelée pour chaque épisode ayant un fichier audio ; false : arrêter la lecture du flux. */
typedef bool (*rss_item_cb_t)(const rss_item_t *item, void *ctx);

rss_parser_t *rss_new(rss_item_cb_t cb, void *ctx);
/* Données suivantes du flux. false : lecture arrêtée par le rappel. */
bool rss_feed(rss_parser_t *p, const char *data, size_t len);
const char *rss_channel_title(const rss_parser_t *p);
void rss_free(rss_parser_t *p);

/* Date RFC 822 (ou ISO 8601) -> secondes UTC ; day et hm tels qu'écrits. false si illisible. */
bool rss_parse_date(const char *s, int64_t *epoch, char day[11], char hm[6]);

/* Nom de fichier d'un épisode (sans dossier), au plus len octets. */
void rss_episode_name(const rss_item_t *it, char *out, size_t len);

/*
 * Les "keep" épisodes les plus récents, triés du plus récent au plus ancien, dans un tableau
 * tenu à jour au fil de la lecture (top doit contenir keep cases ; *count : nombre rempli).
 * Un épisode sans date compte comme le plus ancien.
 */
void rss_keep_newest(rss_item_t *top, int keep, int *count, const rss_item_t *it);
