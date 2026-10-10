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

/*
 * Nom de fichier ou de dossier valide tiré d'un texte libre (titre de radio, de podcast,
 * d'épisode) : caractères interdits par FAT remplacés, espaces réunis, au plus maxlen octets
 * sans couper un caractère UTF-8. Texte vide : « fallback ».
 */
void name_from_text(const char *in, char *out, size_t maxlen, const char *fallback);

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

/*
 * Adresses IPv4 en ordre « hôte » : a.b.c.d -> (a << 24) | (b << 16) | (c << 8) | d.
 * ip4_parse refuse les zéros non significatifs ("010.0.0.1") et tout ce qui n'est pas a.b.c.d.
 */
bool ip4_parse(const char *s, uint32_t *out);
void ip4_format(uint32_t ip, char out[16]);
/* Adresse fixe cohérente : masque contigu (/8 à /30), adresse ni réseau ni diffusion,
 * passerelle dans le même sous-réseau et différente de l'adresse. Message d'erreur sinon. */
bool ip4_config_check(uint32_t ip, uint32_t netmask, uint32_t gateway, const char **why);

/* Base64 standard (avec « = »). base64_encode : out doit contenir 4 * ((n + 2) / 3) + 1
 * caractères. base64_decode retourne le nombre d'octets, ou -1 si invalide ou trop long. */
void base64_encode(const uint8_t *in, size_t n, char *out);
int base64_decode(const char *in, uint8_t *out, size_t outlen);
