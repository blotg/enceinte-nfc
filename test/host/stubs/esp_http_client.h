#pragma once
/* Simulation (tests sur PC) : seules les fonctions de lecture servent, cf. podcast_test.c. */
#include "esp_err.h"

typedef struct esp_http_client *esp_http_client_handle_t;

#define ESP_ERR_HTTP_BASE 0x7000
#define ESP_ERR_HTTP_EAGAIN (ESP_ERR_HTTP_BASE + 7)

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
