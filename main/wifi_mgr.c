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
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "settings.h"
#include "util.h"

static const char *TAG = "wifi";

#define FIRST_CONNECT_GRACE_US (30LL * 1000000)  /* au démarrage ou après changement de réseau */
#define LOST_CONNECT_GRACE_US (120LL * 1000000)  /* coupure en cours de fonctionnement */
#define AP_RETRY_PERIOD_US (60LL * 1000000)      /* tentatives pendant que le point d'accès est actif */
#define AP_LINGER_US (60LL * 1000000)            /* garde le point d'accès après connexion réussie */

static esp_netif_t *s_sta, *s_ap;
static SemaphoreHandle_t s_lock;
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
        start_sntp();
    }
}

static void try_connect(int64_t now)
{
    if (s_scanning) {
        return;
    }
    ESP_LOGI(TAG, "connexion au réseau configuré...");
    esp_wifi_disconnect();
    esp_wifi_connect();
    s_next_attempt = now + (int64_t)s_backoff_s * 1000000;
    if (s_backoff_s < 30) {
        s_backoff_s = s_backoff_s * 2 > 30 ? 30 : s_backoff_s * 2;
    }
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

static void wifi_task(void *arg)
{
    if (!s_configured) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        log_radio_diagnostic();
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_configured) {
            ap_start();
        } else if (s_connected) {
            if (s_ap_on && s_ap_clients == 0 && now - s_connected_since > AP_LINGER_US) {
                ap_stop();
            }
        } else {
            if (!s_ap_on && now - s_disconnected_since > s_grace_us) {
                ESP_LOGW(TAG, "réseau configuré injoignable : ouverture du point d'accès de secours");
                ap_start();
                s_next_attempt = now + AP_RETRY_PERIOD_US;
            }
            if (now >= s_next_attempt) {
                if (!s_ap_on) {
                    try_connect(now);
                } else if (s_ap_clients == 0) {
                    /* Une tentative fait changer de canal le point d'accès : seulement s'il est inutilisé. */
                    try_connect(now);
                    s_next_attempt = now + AP_RETRY_PERIOD_US;
                }
            }
        }
        xSemaphoreGive(s_lock);
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
            snprintf(st->sta_ip, sizeof(st->sta_ip), IPSTR, IP2STR(&ip.ip));
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st->rssi = ap.rssi;
        }
    }
    st->ap_active = s_ap_on;
    str_copy(st->ap_ssid, s_ap_ssid, sizeof(st->ap_ssid));
    st->ap_clients = s_ap_clients;
}

void wifi_mgr_reconnect(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    apply_sta_config();
    s_connected = false;
    s_disconnected_since = now;
    s_grace_us = FIRST_CONNECT_GRACE_US;
    s_backoff_s = 2;
    if (s_configured) {
        try_connect(now);
    }
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
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    s_scanning = true;
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    s_scanning = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "analyse impossible : %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t n = (uint16_t)max;
    if (esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) {
        return 0;
    }
    return n;
}

void wifi_mgr_set_hostname(const char *hostname)
{
    mdns_hostname_set(hostname);
    esp_netif_set_hostname(s_sta, hostname);
}
