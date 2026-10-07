#include "dns_server.h"

#include <string.h>

#ifndef DNS_HOST_TEST
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";
#endif

size_t dns_build_response(const uint8_t *req, size_t len, uint32_t ip, uint8_t *out, size_t outlen)
{
    if (len < 12 || (req[2] & 0x80) || ((req[2] >> 3) & 0x0F) != 0) {
        return 0; /* trop court, déjà une réponse, ou pas une requête standard */
    }
    uint16_t qd = (uint16_t)(req[4] << 8 | req[5]);
    if (qd < 1) {
        return 0;
    }
    size_t p = 12;
    while (p < len && req[p] != 0) {
        if ((req[p] & 0xC0) != 0) {
            return 0; /* compression interdite dans la question */
        }
        p += req[p] + 1;
    }
    if (p + 5 > len) {
        return 0;
    }
    p++; /* octet nul final */
    uint16_t qtype = (uint16_t)(req[p] << 8 | req[p + 1]);
    uint16_t qclass = (uint16_t)(req[p + 2] << 8 | req[p + 3]);
    p += 4;
    bool answer = (qtype == 1 || qtype == 255) && (qclass & 0x7FFF) == 1;
    size_t total = p + (answer ? 16 : 0);
    if (total > outlen) {
        return 0;
    }
    memcpy(out, req, p);
    out[2] = 0x80 | 0x04 | (req[2] & 0x01); /* QR, AA, RD recopié */
    out[3] = 0x00;                          /* pas d'erreur */
    out[4] = 0;
    out[5] = 1; /* une seule question */
    out[6] = 0;
    out[7] = answer ? 1 : 0;
    memset(out + 8, 0, 4);
    if (answer) {
        uint8_t *a = out + p;
        a[0] = 0xC0; /* pointeur vers le nom de la question */
        a[1] = 0x0C;
        a[2] = 0;
        a[3] = 1; /* A */
        a[4] = 0;
        a[5] = 1; /* IN */
        a[6] = 0;
        a[7] = 0;
        a[8] = 0;
        a[9] = 60; /* TTL 60 s */
        a[10] = 0;
        a[11] = 4;
        memcpy(a + 12, &ip, 4);
    }
    return total;
}

#ifndef DNS_HOST_TEST

static uint32_t s_ip;
static bool (*s_active)(void);

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket impossible");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "port 53 indisponible");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t req[512], resp[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, req, sizeof(req), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0 || !s_active || !s_active()) {
            continue;
        }
        /* Uniquement les clients du point d'accès (même sous-réseau /24). */
        if ((from.sin_addr.s_addr & htonl(0xFFFFFF00)) != (s_ip & htonl(0xFFFFFF00))) {
            continue;
        }
        size_t len = dns_build_response(req, n, s_ip, resp, sizeof(resp));
        if (len) {
            sendto(sock, resp, len, 0, (struct sockaddr *)&from, flen);
        }
    }
}

esp_err_t dns_server_start(uint32_t ap_ip, bool (*active)(void))
{
    s_ip = ap_ip;
    s_active = active;
    return xTaskCreate(dns_task, "dns", 3584, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#endif
