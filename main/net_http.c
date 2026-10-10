#include "net_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "util.h"

#define MAX_REDIRECTS 8
#define URL_MAX 640

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->user_data) {
        http_info_t *h = e->user_data;
        if (strcasecmp(e->header_key, "Content-Type") == 0) {
            str_copy(h->content_type, e->header_value, sizeof(h->content_type));
        } else if (strcasecmp(e->header_key, "icy-metaint") == 0) {
            h->icy_metaint = (uint32_t)strtoul(e->header_value, NULL, 10);
        }
    }
    return ESP_OK;
}

void http_close(esp_http_client_handle_t h)
{
    if (h) {
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
    }
}

esp_http_client_handle_t http_get_open(const char *start_url, bool icy, int timeout_ms, http_info_t *info, char *final_url,
                                       size_t url_len, char *err, size_t errlen)
{
    err[0] = '\0';
    char *url = malloc(URL_MAX);
    if (!url) {
        snprintf(err, errlen, "mémoire insuffisante");
        return NULL;
    }
    str_copy(url, start_url, URL_MAX);
    esp_http_client_handle_t h = NULL;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        memset(info, 0, sizeof(*info));
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = timeout_ms,
            .buffer_size = 4096,
            .buffer_size_tx = 1536,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = on_event,
            .user_data = info,
            .disable_auto_redirect = true,
            .user_agent = "Enceinte-NFC",
        };
        h = esp_http_client_init(&cfg);
        if (!h) {
            snprintf(err, errlen, "adresse invalide");
            break;
        }
        if (icy) {
            esp_http_client_set_header(h, "Icy-MetaData", "1");
        }
        esp_err_t e = esp_http_client_open(h, 0);
        if (e != ESP_OK) {
            snprintf(err, errlen, "serveur injoignable (%s)", esp_err_to_name(e));
            esp_http_client_cleanup(h);
            h = NULL;
            break;
        }
        info->content_length = esp_http_client_fetch_headers(h);
        int status = esp_http_client_get_status_code(h);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            bool ok = esp_http_client_set_redirection(h) == ESP_OK && esp_http_client_get_url(h, url, URL_MAX) == ESP_OK;
            http_close(h);
            h = NULL;
            if (!ok) {
                snprintf(err, errlen, "redirection invalide");
                break;
            }
            continue;
        }
        if (status != 200) {
            snprintf(err, errlen, "erreur HTTP %d", status);
            http_close(h);
            h = NULL;
            break;
        }
        if (info->content_length <= 0 && esp_http_client_is_chunked_response(h)) {
            info->content_length = -1;
        }
        if (final_url) {
            str_copy(final_url, url, url_len);
        }
        break;
    }
    if (!h && !err[0]) {
        snprintf(err, errlen, "trop de redirections");
    }
    free(url);
    return h;
}
