#pragma once
/*
 * Flux réseau d'une webradio (HTTP ou HTTPS) pour la tâche de lecture du lecteur :
 * redirections et listes .m3u/.pls suivies, métadonnées ICY retirées du flux audio.
 * Sur PC, une version simulée est fournie par les tests (test/host/shims.c).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

typedef struct stream stream_t;

/* Connexion (bloquante, quelques secondes au plus). NULL et message en français si échec. */
stream_t *stream_open(const char *url, audio_fmt_t *fmt, char *err, size_t errlen);
/* Données audio : nombre d'octets (> 0), 0 si rien n'est arrivé à temps, < 0 si la connexion
 * est perdue ou le flux terminé. */
int stream_read(stream_t *s, uint8_t *buf, int len);
/* Titre diffusé par la radio, s'il a changé depuis le dernier appel. */
bool stream_take_title(stream_t *s, char *out, size_t len);
void stream_close(stream_t *s);
