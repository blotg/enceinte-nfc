#pragma once
/*
 * Certificat auto-signé de l'interface web en HTTPS (option des réglages).
 * Clé ECDSA P-256, valable 20 ans pour "<nom>.local" et "<nom>", conservé en NVS et
 * régénéré si le nom de l'enceinte change.
 */
#include <stddef.h>

#include "esp_err.h"

/* Longueurs : terminateur nul compris (format attendu par esp_https_server). */
esp_err_t tls_cert_get(const char *hostname, const char **cert_pem, size_t *cert_len, const char **key_pem,
                       size_t *key_len);
