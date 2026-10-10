#pragma once
/*
 * Tampon circulaire de texte pour le journal : garde les derniers octets écrits.
 * Sans dépendance ESP-IDF (testé sur PC).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *buf;
    size_t cap;
    uint64_t written; /* total écrit depuis le démarrage */
} log_ring_t;

void log_ring_init(log_ring_t *r, char *buf, size_t cap);
void log_ring_write(log_ring_t *r, const char *s, size_t n);

/*
 * Texte écrit depuis la position "since" (0 : tout ce qui reste), au plus cap - 1 octets (les
 * plus récents), terminé par '\0'. *next : position à redemander ensuite. *reset : la suite
 * ne prolonge pas la lecture précédente (texte écrasé entre-temps, ou redémarrage) ; la
 * lecture repart alors au début d'une ligne. Retourne la longueur copiée.
 */
size_t log_ring_read(const log_ring_t *r, uint64_t since, char *out, size_t cap, uint64_t *next, bool *reset);
