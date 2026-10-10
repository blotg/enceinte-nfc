#pragma once
/*
 * Webradio : outils sans dépendance ESP-IDF (testés sur PC).
 *  - adresses de flux (http:// ou https://) dans la file de lecture ;
 *  - listes .m3u et .pls (fichiers de la carte SD ou réponses de serveurs) ;
 *  - format audio d'après l'en-tête Content-Type ;
 *  - métadonnées ICY (titre diffusé par la radio, entrelacé dans le flux audio).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "util.h"

#define RADIO_URL_MAX REL_PATH_MAX /* une adresse de flux tient dans la file comme un chemin */
#define RADIO_FILE "webradio.m3u"    /* liste créée par l'interface dans le dossier d'une webradio */
#define RADIO_BASE "Webradios"       /* dossier des webradios créées par l'interface */
#define RADIO_TITLE_MAX 128

static inline bool radio_is_url(const char *s)
{
    return s && (strncasecmp(s, "http://", 7) == 0 || strncasecmp(s, "https://", 8) == 0);
}

/*
 * Adresses de flux d'une liste .m3u/.m3u8 (lignes hors commentaires) ou .pls (« FileN= »),
 * dans l'ordre. Chaque adresse valide est passée à cb ; retourne le nombre d'adresses.
 * Les chemins locaux et les adresses trop longues sont ignorés.
 */
int radio_playlist_urls(const char *text, void (*cb)(const char *url, void *ctx), void *ctx);
/* Fichier de liste de lecture (.m3u, .m3u8, .pls) d'après son nom. */
bool radio_is_playlist_name(const char *name);
/* Adresses de flux d'un fichier de liste (16 Ko lus au plus). */
int radio_playlist_file(const char *abs_path, void (*cb)(const char *url, void *ctx), void *ctx);
/* Première adresse de la liste (false si aucune). */
bool radio_playlist_first(const char *text, char *url, size_t len);
/* Une réponse de serveur est-elle une liste (à suivre) plutôt qu'un flux audio ? */
bool radio_is_playlist(const char *content_type, const char *url);
/* Format audio d'après Content-Type, sinon d'après l'extension de l'adresse, sinon MP3. */
audio_fmt_t radio_fmt(const char *content_type, const char *url);

/* Démultiplexage ICY : retire les blocs de métadonnées insérés tous les "metaint" octets. */
typedef struct {
    uint32_t metaint;    /* 0 : pas de métadonnées */
    uint32_t until_meta; /* octets audio avant le prochain bloc */
    uint32_t meta_left;  /* octets du bloc en cours restant à lire */
    uint32_t meta_len;
    bool in_len;         /* prochain octet : longueur du bloc (x16) */
    char meta[16 * 255 + 1];
    char title[RADIO_TITLE_MAX];
    bool title_changed;
} icy_t;

void icy_init(icy_t *s, uint32_t metaint);
/* Retire les métadonnées de buf (en place). Retourne le nombre d'octets audio restants. */
size_t icy_strip(icy_t *s, uint8_t *buf, size_t len);
/* Titre extrait d'un bloc « StreamTitle='...';... » (Latin-1 converti en UTF-8). */
bool icy_parse_title(const char *meta, char *out, size_t len);

/* Texte en UTF-8 valide : copié tel quel, sinon lu comme du Latin-1 et converti. */
void text_to_utf8(const char *in, char *out, size_t len);
