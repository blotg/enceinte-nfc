#pragma once
/*
 * Gestion du Wi-Fi :
 *  - sortie d'usine ou aucun réseau configuré : point d'accès "Enceinte-XXXX" + portail captif ;
 *  - réseau configuré : connexion ; s'il reste injoignable, le point d'accès s'ouvre en
 *    secours et l'enceinte continue de chercher le réseau (analyses, sans quitter le point
 *    d'accès). Quand il réapparaît : reconnexion immédiate si personne n'est connecté au
 *    point d'accès et qu'aucun envoi n'est en cours ; sinon l'interface propose de basculer,
 *    et la bascule se fait d'elle-même dès que le dernier appareil est parti ;
 *  - adresse IP par DHCP ou fixe. Une nouvelle adresse est d'abord essayée : sans connexion
 *    administrateur à cette adresse dans les 5 minutes, l'ancienne configuration revient ;
 *  - mDNS (nom.local) et mise à l'heure NTP.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "settings.h"

#define AP_IP_STR "192.168.4.1"
#define IP_TEST_S 300

typedef struct {
    bool sta_configured;
    bool sta_connected;
    char sta_ssid[33];
    char sta_ip[16];
    char sta_netmask[16];
    char sta_gateway[16];
    char sta_dns[16];
    int8_t rssi;
    bool ap_active;
    char ap_ssid[33];
    int ap_clients;
    bool sta_available;     /* point d'accès actif et réseau configuré de nouveau à portée */
    int ip_test_remaining;  /* > 0 : secondes pour confirmer la nouvelle adresse IP */
    char ip_test_address[16]; /* adresse à confirmer ("" : DHCP) */
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

/* Essaie une configuration IP sans l'enregistrer (appliquée dans 1,5 s). */
void wifi_mgr_ip_test(const ip_config_t *ip);
bool wifi_mgr_ip_testing(void);
/* Requête administrateur reçue sur l'adresse locale local_ip (ordre « hôte ») : si c'est la
 * nouvelle adresse, elle est confirmée et enregistrée. */
void wifi_mgr_ip_confirm(uint32_t local_ip);
/* L'utilisateur accepte de quitter le point d'accès pour le réseau configuré. */
void wifi_mgr_switch_now(void);
