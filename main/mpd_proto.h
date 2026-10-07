#pragma once
/*
 * Analyse du protocole MPD (sans dépendance ESP-IDF, testée sur PC) :
 * découpage des commandes, plages, filtres de recherche.
 */
#include <stdbool.h>
#include <stddef.h>

#define MPD_MAX_ARGS 24

typedef enum {
    MPD_TAG_ARTIST = 0,
    MPD_TAG_ALBUMARTIST,
    MPD_TAG_ALBUM,
    MPD_TAG_TITLE,
    MPD_TAG_TRACK,
    MPD_TAG_GENRE,
    MPD_TAG_DATE,
    MPD_TAG_COUNT, /* tags "réels" ci-dessus */
    MPD_TAG_FILE = MPD_TAG_COUNT,
    MPD_TAG_ANY,
} mpd_tag_t;

/* Noms MPD des tags réels (Artist, AlbumArtist...). */
extern const char *const mpd_tag_names[MPD_TAG_COUNT];
/* Insensible à la casse. -1 si inconnu. */
int mpd_tag_from_name(const char *name);

/* Découpe une ligne sur place. Retourne argc, ou -1 (guillemet non fermé, trop d'arguments). */
int mpd_tokenize(char *line, char **argv, int max);

bool mpd_parse_int(const char *s, int *out);
bool mpd_parse_float(const char *s, double *out);
/* "N" -> [N, N+1) ; "A:B" -> [A, B) ; "A:" -> [A, -1). */
bool mpd_parse_range(const char *s, int *start, int *end);

/* ---- Filtres (find, search, list, count, playlistfind...) ---- */

typedef enum {
    MPD_F_EQ,
    MPD_F_NE,
    MPD_F_CONTAINS,
    MPD_F_STARTS,
    MPD_F_BASE,
    MPD_F_AND,
    MPD_F_NOT,
} mpd_filter_op_t;

typedef struct mpd_filter {
    mpd_filter_op_t op;
    int tag;
    char *value;
    bool icase;
    struct mpd_filter *a, *b;
} mpd_filter_t;

/* Accès aux valeurs d'un morceau : NULL ou "" si absent. */
typedef const char *(*mpd_tag_getter_t)(void *ctx, int tag);

/* Syntaxe moderne : "((artist == 'X') AND (album contains 'Y'))". */
mpd_filter_t *mpd_filter_parse_expr(const char *expr, bool icase, char *err, size_t errlen);
/* Syntaxe historique : TAG VALEUR [TAG VALEUR...]. search=true : contient, sans casse. */
mpd_filter_t *mpd_filter_parse_pairs(int argc, char **argv, bool search, char *err, size_t errlen);
/* Analyse les arguments d'une commande (expression ou paires) ; *used = arguments consommés. */
mpd_filter_t *mpd_filter_parse_args(int argc, char **argv, bool search, int *used, char *err, size_t errlen);
bool mpd_filter_match(const mpd_filter_t *f, mpd_tag_getter_t get, void *ctx);
/* Minuscules UTF-8 (ASCII et lettres latines accentuées) pour les comparaisons sans casse. */
void mpd_utf8_fold(const char *in, char *out, size_t outlen);
void mpd_filter_free(mpd_filter_t *f);
