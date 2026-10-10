#pragma once
/*
 * Requête HTTP(S) GET en flux, redirections suivies (webradios, podcasts). Certificats :
 * paquet de certificats racine d'ESP-IDF.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_client.h"

typedef struct {
    char content_type[64];
    uint32_t icy_metaint;   /* webradio : intervalle des métadonnées, 0 si absentes */
    int64_t content_length; /* -1 si inconnue */
} http_info_t;

/*
 * Ouvre "url" (réponse 200 attendue). icy : demande les titres d'une webradio. final_url
 * (facultatif) reçoit l'adresse après redirections. NULL et message en français si échec.
 * À fermer avec http_close.
 */
esp_http_client_handle_t http_get_open(const char *url, bool icy, int timeout_ms, http_info_t *info, char *final_url,
                                       size_t url_len, char *err, size_t errlen);
void http_close(esp_http_client_handle_t h);
