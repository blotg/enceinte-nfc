#pragma once
/*
 * Serveur DNS captif : répond à toute requête A par l'adresse du point d'accès,
 * pour que les téléphones ouvrent automatiquement la page de configuration.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ap_ip : adresse IPv4 du point d'accès (ordre réseau). active() : faut-il répondre ? */
esp_err_t dns_server_start(uint32_t ap_ip, bool (*active)(void));

/* Construit la réponse à une requête (fonction pure, testée sur PC). Retourne 0 si la requête est ignorée. */
size_t dns_build_response(const uint8_t *req, size_t len, uint32_t ip, uint8_t *out, size_t outlen);
