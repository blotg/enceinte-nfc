#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "net_http.h"
#include "radio.h"

static const char *TAG = "stream";

#define MAX_HOPS 4            /* listes .m3u/.pls imbriquées */
#define PLAYLIST_MAX 8192     /* réponse lue pour une liste .m3u/.pls */
#define TIMEOUT_MS 5000

struct stream {
    esp_http_client_handle_t http;
    icy_t icy; /* ~4 Ko : la structure est allouée en PSRAM */
};

/* Lit le corps d'une réponse (liste de lecture). */
static bool read_body(esp_http_client_handle_t h, char *buf, size_t cap)
{
    size_t n = 0;
    while (n < cap - 1) {
        int r = esp_http_client_read(h, buf + n, (int)(cap - 1 - n));
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
    }
    buf[n] = '\0';
    return n > 0;
}

stream_t *stream_open(const char *start_url, audio_fmt_t *fmt, char *err, size_t errlen)
{
    char *url = malloc(RADIO_URL_MAX);
    char *body = NULL;
    http_info_t *info = calloc(1, sizeof(http_info_t));
    stream_t *s = NULL;
    err[0] = '\0';
    if (!url || !info) {
        snprintf(err, errlen, "mémoire insuffisante");
        goto done;
    }
    str_copy(url, start_url, RADIO_URL_MAX);
    for (int hop = 0; hop < MAX_HOPS; hop++) {
        char e[80] = "";
        esp_http_client_handle_t h = http_get_open(url, true, TIMEOUT_MS, info, url, RADIO_URL_MAX, e, sizeof(e));
        if (!h) {
            snprintf(err, errlen, "radio injoignable : %s", e);
            goto done;
        }
        if (radio_is_playlist(info->content_type, url)) {
            /* liste .m3u/.pls : on suit la première adresse de flux */
            if (!body) {
                body = malloc(PLAYLIST_MAX);
            }
            bool ok = body && read_body(h, body, PLAYLIST_MAX) && radio_playlist_first(body, url, RADIO_URL_MAX);
            http_close(h);
            if (!ok) {
                snprintf(err, errlen, "liste de lecture sans adresse de flux");
                goto done;
            }
            continue;
        }
        if (strncasecmp(info->content_type, "text/", 5) == 0) {
            snprintf(err, errlen, "l'adresse ne mène pas à un flux audio (%s)", info->content_type);
            http_close(h);
            goto done;
        }
        s = malloc(sizeof(stream_t));
        if (!s) {
            snprintf(err, errlen, "mémoire insuffisante");
            http_close(h);
            goto done;
        }
        s->http = h;
        icy_init(&s->icy, info->icy_metaint);
        *fmt = radio_fmt(info->content_type, url);
        ESP_LOGI(TAG, "flux %s (%s%s)", url, info->content_type[0] ? info->content_type : "type inconnu",
                 info->icy_metaint ? ", titres" : "");
        goto done;
    }
    snprintf(err, errlen, "trop de listes de lecture imbriquées");
done:
    free(url);
    free(body);
    free(info);
    return s;
}

int stream_read(stream_t *s, uint8_t *buf, int len)
{
    int n = esp_http_client_read(s->http, (char *)buf, len);
    if (n == -ESP_ERR_HTTP_EAGAIN) {
        return 0; /* rien n'est arrivé à temps */
    }
    if (n <= 0) {
        return -1; /* connexion perdue ou flux terminé */
    }
    return (int)icy_strip(&s->icy, buf, (size_t)n);
}

bool stream_take_title(stream_t *s, char *out, size_t len)
{
    if (!s->icy.title_changed) {
        return false;
    }
    s->icy.title_changed = false;
    str_copy(out, s->icy.title, len);
    return true;
}

void stream_close(stream_t *s)
{
    if (!s) {
        return;
    }
    http_close(s->http);
    free(s);
}
