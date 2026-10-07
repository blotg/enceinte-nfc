#include "tls_cert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "tls";
static const char *NS = "tls";

#define CERT_MAX 2048
#define KEY_MAX 512

static char *s_cert, *s_key;

static esp_err_t generate(const char *hostname, char *cert, char *key)
{
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context pk;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);

    char name[96], dns_local[48], dns_short[40];
    snprintf(name, sizeof(name), "CN=%s.local,O=Enceinte NFC", hostname);
    snprintf(dns_local, sizeof(dns_local), "%s.local", hostname);
    snprintf(dns_short, sizeof(dns_short), "%s", hostname);
    mbedtls_x509_san_list san_short = {
        .node = {.type = MBEDTLS_X509_SAN_DNS_NAME,
                 .san.unstructured_name = {.p = (unsigned char *)dns_short, .len = strlen(dns_short)}},
        .next = NULL,
    };
    mbedtls_x509_san_list san_local = {
        .node = {.type = MBEDTLS_X509_SAN_DNS_NAME,
                 .san.unstructured_name = {.p = (unsigned char *)dns_local, .len = strlen(dns_local)}},
        .next = &san_short,
    };
    unsigned char serial[16];

    int ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"enceinte", 8);
    if (!ret) {
        ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    }
    if (!ret) {
        ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &drbg);
    }
    if (!ret) {
        ret = mbedtls_ctr_drbg_random(&drbg, serial, sizeof(serial));
        serial[0] = (serial[0] & 0x7F) | 0x01; /* numéro de série positif, sans zéro de tête */
    }
    if (!ret) {
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
        ret = mbedtls_x509write_crt_set_subject_name(&crt, name);
    }
    if (!ret) {
        ret = mbedtls_x509write_crt_set_issuer_name(&crt, name);
    }
    if (!ret) {
        ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    }
    if (!ret) {
        ret = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20450101000000");
    }
    if (!ret) {
        ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    }
    if (!ret) {
        ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                                            MBEDTLS_X509_KU_KEY_AGREEMENT);
    }
    if (!ret) {
        ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_local);
    }
    if (!ret) {
        ret = mbedtls_x509write_crt_pem(&crt, (unsigned char *)cert, CERT_MAX, mbedtls_ctr_drbg_random, &drbg);
    }
    if (!ret) {
        ret = mbedtls_pk_write_key_pem(&pk, (unsigned char *)key, KEY_MAX);
    }
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    if (ret) {
        ESP_LOGE(TAG, "génération du certificat impossible (-0x%04x)", (unsigned)-ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool load(const char *hostname, char *cert, char *key)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(CFG_PARTITION, NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char host[40];
    size_t lh = sizeof(host), lc = CERT_MAX, lk = KEY_MAX;
    bool ok = nvs_get_str(h, "host", host, &lh) == ESP_OK && strcmp(host, hostname) == 0 &&
              nvs_get_str(h, "crt", cert, &lc) == ESP_OK && nvs_get_str(h, "key", key, &lk) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void save(const char *hostname, const char *cert, const char *key)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(CFG_PARTITION, NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_str(h, "crt", cert) == ESP_OK && nvs_set_str(h, "key", key) == ESP_OK &&
        nvs_set_str(h, "host", hostname) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

esp_err_t tls_cert_get(const char *hostname, const char **cert_pem, size_t *cert_len, const char **key_pem,
                       size_t *key_len)
{
    if (!s_cert) {
        s_cert = calloc(1, CERT_MAX);
        s_key = calloc(1, KEY_MAX);
        if (!s_cert || !s_key) {
            free(s_cert);
            free(s_key);
            s_cert = s_key = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (!load(hostname, s_cert, s_key)) {
        ESP_LOGI(TAG, "génération du certificat auto-signé pour %s.local", hostname);
        esp_err_t err = generate(hostname, s_cert, s_key);
        if (err != ESP_OK) {
            return err;
        }
        save(hostname, s_cert, s_key);
    }
    *cert_pem = s_cert;
    *cert_len = strlen(s_cert) + 1;
    *key_pem = s_key;
    *key_len = strlen(s_key) + 1;
    return ESP_OK;
}
