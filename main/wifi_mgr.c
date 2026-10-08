#include "wifi_mgr.h"

#include <stdlib.h>
#include <string.h>

#include "dns_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "util.h"
#include "web_server.h"

static const char *TAG = "wifi";

#define FIRST_CONNECT_GRACE_US (30LL * 1000000)  /* au démarrage ou après changement de réseau */
#define LOST_CONNECT_GRACE_US (120LL * 1000000)  /* coupure en cours de fonctionnement */
#define AP_SCAN_PERIOD_US (30LL * 1000000)       /* recherche du réseau, point d'accès actif */
#define AP_FIRST_SCAN_US (5LL * 1000000)
#define SEEN_VALID_US (90LL * 1000000)           /* réseau vu il y a moins de 90 s : à portée */
#define AP_LINGER_US (60LL * 1000000)            /* garde le point d'accès après connexion réussie */
#define IP_TEST_US ((int64_t)IP_TEST_S * 1000000)
#define SWITCH_DELAY_US (1500LL * 1000)          /* laisse partir la réponse HTTP */

static esp_netif_t *s_sta, *s_ap;
static SemaphoreHandle_t s_lock, s_scan_lock;
static volatile bool s_connected;
static volatile bool s_ap_on;
static volatile int s_ap_clients;
static bool s_configured;
static bool s_sntp_started;
static int64_t s_disconnected_since;
static int64_t s_connected_since;
static int64_t s_next_attempt;
static int64_t s_grace_us = FIRST_CONNECT_GRACE_US;
static int s_backoff_s = 2;
static char s_ap_ssid[33];
static char s_captive_uri[] = "http://" AP_IP_STR "/";
static volatile bool s_scanning;
static esp_timer_handle_t s_reconnect_timer;
/* Point d'accès de secours : recherche du réseau configuré */
static int64_t s_next_scan;
static volatile int64_t s_sta_seen; /* dernière fois que le réseau a été vu, 0 : jamais */
static volatile int64_t s_switch_at; /* bascule demandée par l'utilisateur */
/* Adresse IP à l'essai */
static ip_config_t s_ip_test;
static volatile bool s_ip_testing;
static volatile bool s_ip_test_applied; /* reconnexion faite avec la configuration à l'essai */
static volatile bool s_ip_test_live;    /* et adresse obtenue depuis */
static int64_t s_ip_test_deadline;

/* Configuration IP en vigueur : celle à l'essai, sinon celle enregistrée. */
static void apply_ip_config(void)
{
    settings_t cfg;
    settings_get(&cfg);
    ip_config_t ip = s_ip_testing ? s_ip_test : cfg.ip;
    if (ip.static_ip) {
        esp_netif_dhcpc_stop(s_sta);
        esp_netif_ip_info_t info = {0};
        info.ip.addr = htonl(ip.address);
        info.netmask.addr = htonl(ip.netmask);
        info.gw.addr = htonl(ip.gateway);
        esp_err_t err = esp_netif_set_ip_info(s_sta, &info);
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = htonl(ip.dns ? ip.dns : ip.gateway);
        esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns);
        char a[16];
        ip4_format(ip.address, a);
        ESP_LOGI(TAG, "adresse IP fixe %s%s (%s)", a, s_ip_testing ? " à l'essai" : "", esp_err_to_name(err));
    } else {
        esp_err_t err = esp_netif_dhcpc_start(s_sta);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "client DHCP : %s", esp_err_to_name(err));
        }
    }
}

static void apply_sta_config(void)
{
    settings_t cfg;
    settings_get(&cfg);
    s_configured = cfg.wifi_ssid[0] != '\0';
    wifi_config_t wc = {0};
    str_copy((char *)wc.sta.ssid, cfg.wifi_ssid, sizeof(wc.sta.ssid));
    str_copy((char *)wc.sta.password, cfg.wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = cfg.wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_netif_set_hostname(s_sta, cfg.hostname);
    apply_ip_config();
}

static void ap_start(void)
{
    if (s_ap_on) {
        return;
    }
    ESP_LOGI(TAG, "point d'accès \"%s\" actif (http://%s)", s_ap_ssid, AP_IP_STR);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    s_ap_on = true;
}

static void ap_stop(void)
{
    if (!s_ap_on) {
        return;
    }
    ESP_LOGI(TAG, "point d'accès arrêté");
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_on = false;
    s_ap_clients = 0;
}

static void start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    int64_t now = esp_timer_get_time();
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            if (s_connected) {
                ESP_LOGW(TAG, "connexion perdue (raison %d)", d->reason);
                s_disconnected_since = now;
                s_grace_us = LOST_CONNECT_GRACE_US;
                s_backoff_s = 2;
            } else {
                ESP_LOGD(TAG, "échec de connexion (raison %d)", d->reason);
            }
            s_connected = false;
            break;
        }
        case WIFI_EVENT_AP_START:
            /* Portail captif : option DHCP 114 (RFC 8910) en plus du DNS captif. */
            esp_netif_dhcps_stop(s_ap);
            esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_captive_uri,
                                   strlen(s_captive_uri));
            esp_netif_dhcps_start(s_ap);
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            s_ap_clients++;
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) {
                s_ap_clients--;
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connecté, adresse " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_connected_since = now;
        s_backoff_s = 2;
        s_sta_seen = 0;
        s_switch_at = 0;
        if (s_ip_test_applied) {
            s_ip_test_live = true;
        }
        start_sntp();
    }
}

static bool try_connect(int64_t now)
{
    if (s_scanning) {
        return false;
    }
    ESP_LOGI(TAG, "connexion au réseau configuré...");
    esp_wifi_disconnect();
    esp_wifi_connect();
    s_next_attempt = now + (int64_t)s_backoff_s * 1000000;
    if (s_backoff_s < 30) {
        s_backoff_s = s_backoff_s * 2 > 30 ? 30 : s_backoff_s * 2;
    }
    return true;
}

static int scan(wifi_ap_record_t *out, int max, const char *ssid)
{
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    s_scanning = true;
    /* Point d'accès actif : retour sur son canal entre deux canaux analysés (appareils connectés). */
    wifi_scan_config_t sc = {.ssid = (uint8_t *)ssid, .show_hidden = ssid != NULL, .home_chan_dwell_time = 30};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    s_scanning = false;
    int n = 0;
    if (err == ESP_OK) {
        uint16_t count = (uint16_t)max;
        if (esp_wifi_scan_get_ap_records(&count, out) == ESP_OK) {
            n = count;
        }
    } else {
        ESP_LOGD(TAG, "analyse impossible : %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_scan_lock);
    return n;
}

/* Point d'accès actif : le réseau configuré est-il de nouveau à portée ? */
static bool configured_network_visible(void)
{
    settings_t cfg;
    settings_get(&cfg);
    if (!cfg.wifi_ssid[0]) {
        return false;
    }
    wifi_ap_record_t recs[2];
    int n = scan(recs, 2, cfg.wifi_ssid);
    for (int i = 0; i < n; i++) {
        if (strcmp((const char *)recs[i].ssid, cfg.wifi_ssid) == 0) {
            return true;
        }
    }
    return false;
}

/* Diagnostic : réseaux reçus et puissance d'émission (antenne, alimentation). */
static void log_radio_diagnostic(void)
{
    wifi_ap_record_t *recs = calloc(8, sizeof(wifi_ap_record_t));
    if (!recs) {
        return;
    }
    int n = wifi_mgr_scan(recs, 8);
    ESP_LOGI(TAG, "diagnostic radio : %d réseau(x) reçu(s)", n);
    for (int i = 0; i < n && i < 4; i++) {
        ESP_LOGI(TAG, "  \"%s\" %d dBm (canal %d)", (const char *)recs[i].ssid, recs[i].rssi, recs[i].primary);
    }
    int8_t pwr = 0;
    if (esp_wifi_get_max_tx_power(&pwr) == ESP_OK) {
        ESP_LOGI(TAG, "puissance d'émission maximale : %d,%02d dBm", pwr / 4, (pwr % 4) * 25);
    }
    free(recs);
}

static void reconnect_locked(int64_t now)
{
    apply_sta_config();
    s_connected = false;
    s_disconnected_since = now;
    s_grace_us = FIRST_CONNECT_GRACE_US;
    s_backoff_s = 2;
    s_ip_test_applied = s_ip_testing;
    s_ip_test_live = false;
    if (s_configured) {
        try_connect(now);
    }
}

static void wifi_task(void *arg)
{
    if (!s_configured) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        log_radio_diagnostic();
    }
    bool announced = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        bool search = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_ip_testing && now >= s_ip_test_deadline) {
            ESP_LOGW(TAG, "nouvelle adresse IP non confirmée en %d min : retour à l'ancienne configuration",
                     IP_TEST_S / 60);
            s_ip_testing = false;
            reconnect_locked(now);
        }
        if (!s_configured) {
            ap_start();
        } else if (s_connected) {
            announced = false;
            if (s_ap_on && s_ap_clients == 0 && now - s_connected_since > AP_LINGER_US) {
                ap_stop();
            }
        } else if (!s_ap_on) {
            if (now - s_disconnected_since > s_grace_us) {
                ESP_LOGW(TAG, "réseau configuré injoignable : point d'accès de secours (recherche du réseau continue)");
                ap_start();
                s_next_scan = now + AP_FIRST_SCAN_US;
            } else if (now >= s_next_attempt) {
                try_connect(now);
            }
        } else {
            /* Point d'accès actif : on cherche le réseau configuré sans le quitter. */
            bool seen = s_sta_seen && now - s_sta_seen < SEEN_VALID_US;
            bool idle = s_ap_clients == 0 && !web_server_transfers_active();
            bool asked = s_switch_at && now >= s_switch_at;
            if (seen && (idle || asked)) {
                if (try_connect(now)) {
                    ESP_LOGI(TAG, "réseau configuré à portée : reconnexion (%s)",
                             asked ? "demandée" : "personne sur le point d'accès");
                    s_switch_at = 0;
                    s_sta_seen = 0;
                    s_next_scan = now + AP_SCAN_PERIOD_US; /* le temps de se connecter */
                }
            } else if (seen && !announced) {
                ESP_LOGI(TAG, "réseau configuré à portée : bascule dès que le point d'accès sera libre");
                announced = true;
            } else if (now >= s_next_scan) {
                s_next_scan = now + AP_SCAN_PERIOD_US;
                search = true;
            }
            if (s_switch_at && now - s_switch_at > SEEN_VALID_US) {
                s_switch_at = 0; /* réseau plus vu depuis la demande */
            }
        }
        xSemaphoreGive(s_lock);
        /* L'analyse (~2 s) se fait sans bloquer le reste du gestionnaire. */
        if (search && configured_network_visible()) {
            s_sta_seen = esp_timer_get_time();
        }
    }
}

static void start_mdns(const char *hostname)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mDNS indisponible");
        return;
    }
    mdns_hostname_set(hostname);
    mdns_instance_name_set("Enceinte NFC");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_mpd", "_tcp", CONFIG_ENC_MPD_PORT, NULL, 0);
}

esp_err_t wifi_mgr_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_scan_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_wifi_set_storage(WIFI_STORAGE_RAM); /* identifiants gérés par settings */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", CONFIG_ENC_AP_SSID_PREFIX, mac[4], mac[5]);
    wifi_config_t ap = {0};
    str_copy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN; /* l'interface reste protégée par le mot de passe administrateur */
    ap.ap.max_connection = 4;

    settings_t cfg;
    settings_get(&cfg);
    s_configured = cfg.wifi_ssid[0] != '\0';
    esp_wifi_set_mode(s_configured ? WIFI_MODE_STA : WIFI_MODE_APSTA);
    s_ap_on = !s_configured;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    apply_sta_config();
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); /* alimentation secteur : latence minimale */
    esp_wifi_set_max_tx_power(CONFIG_ENC_WIFI_TX_POWER_DBM * 4); /* unité : 0,25 dBm */

    int64_t now = esp_timer_get_time();
    s_disconnected_since = now;
    if (s_configured) {
        try_connect(now);
    } else {
        ESP_LOGI(TAG, "aucun réseau configuré : point d'accès \"%s\"", s_ap_ssid);
    }

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap, &ip);
    dns_server_start(ip.ip.addr, wifi_mgr_ap_active);
    start_mdns(cfg.hostname);
    xTaskCreate(wifi_task, "wifi_mgr", 4096, NULL, 4, NULL);
    return ESP_OK;
}

bool wifi_mgr_ap_active(void)
{
    return s_ap_on;
}

static void fmt_ip(const esp_ip4_addr_t *a, char out[16])
{
    ip4_format(ntohl(a->addr), out);
}

void wifi_mgr_get_status(wifi_status_t *st)
{
    memset(st, 0, sizeof(*st));
    settings_t cfg;
    settings_get(&cfg);
    st->sta_configured = cfg.wifi_ssid[0] != '\0';
    str_copy(st->sta_ssid, cfg.wifi_ssid, sizeof(st->sta_ssid));
    str_copy(st->hostname, cfg.hostname, sizeof(st->hostname));
    st->sta_connected = s_connected;
    if (s_connected) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) {
            fmt_ip(&ip.ip, st->sta_ip);
            fmt_ip(&ip.netmask, st->sta_netmask);
            fmt_ip(&ip.gw, st->sta_gateway);
        }
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4) {
            fmt_ip(&dns.ip.u_addr.ip4, st->sta_dns);
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st->rssi = ap.rssi;
        }
    }
    st->ap_active = s_ap_on;
    str_copy(st->ap_ssid, s_ap_ssid, sizeof(st->ap_ssid));
    st->ap_clients = s_ap_clients;
    int64_t now = esp_timer_get_time();
    int64_t seen = s_sta_seen;
    st->sta_available = s_ap_on && !s_connected && seen && now - seen < SEEN_VALID_US;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ip_testing) {
        int64_t left = (s_ip_test_deadline - now) / 1000000;
        st->ip_test_remaining = left > 0 ? (int)left : 1;
        if (s_ip_test.static_ip) {
            ip4_format(s_ip_test.address, st->ip_test_address);
        }
    }
    xSemaphoreGive(s_lock);
}

void wifi_mgr_reconnect(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reconnect_locked(esp_timer_get_time());
    xSemaphoreGive(s_lock);
}

static void reconnect_cb(void *arg)
{
    wifi_mgr_reconnect();
}

void wifi_mgr_reconnect_later(uint32_t delay_ms)
{
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t a = {.callback = reconnect_cb, .name = "wifi_reco"};
        esp_timer_create(&a, &s_reconnect_timer);
    }
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
}

int wifi_mgr_scan(wifi_ap_record_t *out, int max)
{
    return scan(out, max, NULL);
}

void wifi_mgr_set_hostname(const char *hostname)
{
    mdns_hostname_set(hostname);
    esp_netif_set_hostname(s_sta, hostname);
}

void wifi_mgr_ip_test(const ip_config_t *ip)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ip_test = *ip;
    s_ip_testing = true;
    s_ip_test_applied = s_ip_test_live = false;
    s_ip_test_deadline = esp_timer_get_time() + IP_TEST_US;
    xSemaphoreGive(s_lock);
    char a[16] = "DHCP";
    if (ip->static_ip) {
        ip4_format(ip->address, a);
    }
    ESP_LOGI(TAG, "essai de la configuration IP %s : à confirmer sous %d min", a, IP_TEST_S / 60);
    wifi_mgr_reconnect_later(1500);
}

bool wifi_mgr_ip_testing(void)
{
    return s_ip_testing;
}

void wifi_mgr_ip_confirm(uint32_t local_ip)
{
    if (!s_ip_testing || !s_ip_test_live || !s_connected) {
        return;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta, &info) != ESP_OK || ntohl(info.ip.addr) != local_ip || local_ip == 0) {
        return; /* requête arrivée par le point d'accès, ou avant la bascule */
    }
    ip_config_t ip;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool still = s_ip_testing;
    ip = s_ip_test;
    s_ip_testing = false;
    s_ip_test_applied = s_ip_test_live = false;
    xSemaphoreGive(s_lock);
    if (still) {
        esp_err_t err = settings_set_ip(&ip);
        char a[16];
        ip4_format(local_ip, a);
        ESP_LOGI(TAG, "nouvelle adresse IP %s confirmée : %s", a, err == ESP_OK ? "enregistrée" : esp_err_to_name(err));
    }
}

void wifi_mgr_switch_now(void)
{
    s_switch_at = esp_timer_get_time() + SWITCH_DELAY_US;
}
