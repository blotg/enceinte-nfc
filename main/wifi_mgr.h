#pragma once
/*
 * Gestion du Wi-Fi :
 *  - sortie d'usine ou aucun réseau configuré : point d'accès "Enceinte-XXXX" + portail captif ;
 *  - réseau configuré : connexion ; en cas d'échec prolongé, le point d'accès est ouvert
 *    en secours pendant que les tentatives continuent (suspendues tant qu'un appareil
 *    y est connecté, car elles perturbent le point d'accès) ;
 *  - mDNS (nom.local) et mise à l'heure NTP.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#define AP_IP_STR "192.168.4.1"

typedef struct {
    bool sta_configured;
    bool sta_connected;
    char sta_ssid[33];
    char sta_ip[16];
    int8_t rssi;
    bool ap_active;
    char ap_ssid[33];
    int ap_clients;
    char hostname[33];
} wifi_status_t;

esp_err_t wifi_mgr_start(void);
void wifi_mgr_get_status(wifi_status_t *st);
bool wifi_mgr_ap_active(void);
/* Recharge les identifiants enregistrés et relance la connexion. */
void wifi_mgr_reconnect(void);
/* Idem après un délai : laisse partir la réponse HTTP avant de couper la connexion. */
void wifi_mgr_reconnect_later(uint32_t delay_ms);
/* Analyse des réseaux (bloquant ~3 s). Retourne le nombre de réseaux. */
int wifi_mgr_scan(wifi_ap_record_t *out, int max);
/* Applique un nouveau nom d'hôte (mDNS immédiatement, DHCP à la prochaine connexion). */
void wifi_mgr_set_hostname(const char *hostname);
