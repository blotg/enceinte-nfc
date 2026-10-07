#pragma once
/*
 * Fonctions utilitaires sans dépendance ESP-IDF (testées sur PC, cf. test/host).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef MUSIC_ROOT
#define MUSIC_ROOT "/sdcard" /* surchargeable pour les tests sur PC */
#endif
#define REL_PATH_MAX 256  /* chemin relatif à la racine de la carte SD */
#define ABS_PATH_MAX (REL_PATH_MAX + sizeof(MUSIC_ROOT) + 1)

typedef enum {
    AUDIO_FMT_NONE = 0,
    AUDIO_FMT_MP3,
    AUDIO_FMT_AAC,
    AUDIO_FMT_FLAC,
    AUDIO_FMT_WAV,
    AUDIO_FMT_M4A,
    AUDIO_FMT_OGG,
} audio_fmt_t;

/* Décode une chaîne encodée URL (%XX et '+'). Retourne false si invalide ou trop long. */
bool url_decode(const char *in, char *out, size_t outlen);

/*
 * Normalise un chemin relatif fourni par un client (web, MPD) :
 * "/a//b/" -> "a/b", racine -> "". Refuse "..", ".", les caractères interdits
 * par FAT et les noms finissant par un espace ou un point.
 */
bool path_sanitize(const char *in, char *out, size_t outlen);

/* Chemin relatif (déjà normalisé) -> chemin absolu VFS. */
bool path_to_abs(const char *rel, char *out, size_t outlen);

/* Valide un nom de fichier ou dossier isolé (sans '/'). */
bool name_is_valid(const char *name);

const char *path_basename(const char *path);
void path_dirname(const char *path, char *out, size_t outlen);

/* Fichiers cachés / métadonnées macOS (._xxx, .Trashes...) à ignorer. */
bool name_is_hidden(const char *name);

audio_fmt_t audio_fmt_from_name(const char *name);
static inline bool is_audio_file(const char *name)
{
    return !name_is_hidden(name) && audio_fmt_from_name(name) != AUDIO_FMT_NONE;
}

/* Comparaison "naturelle" insensible à la casse : "piste 2" < "piste 10". */
int natural_casecmp(const char *a, const char *b);

/* Compare deux versions "1.2.3" (préfixe 'v' toléré). <0, 0, >0. */
int semver_cmp(const char *a, const char *b);

/* Octets -> hexadécimal majuscule. out doit contenir 2*n+1 caractères. */
void bytes_to_hex(const uint8_t *bytes, size_t n, char *out);

/* Nom d'hôte mDNS valide : 1-32 caractères [a-z0-9-]. Met en minuscules. */
bool hostname_normalize(const char *in, char *out, size_t outlen);

/* Copie bornée qui termine toujours la chaîne. */
void str_copy(char *dst, const char *src, size_t dstlen);
