#pragma once
/*
 * Réglages persistants (partition NVS "cfg").
 * Les mots de passe ne sont jamais stockés en clair (PBKDF2-HMAC-SHA256 salé).
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define CFG_PARTITION "cfg"

typedef struct {
    char hostname[33];
    char wifi_ssid[33];
    char wifi_pass[65];
    char ota_url[256];
    uint16_t ota_interval_h;
    uint32_t resume_timeout_s; /* délai de reprise après retrait, 0 = toujours */
    bool resume_after_other;   /* reprendre même si une autre carte a été posée entre-temps */
    uint8_t volume;     /* 0-100, dernier volume utilisé */
    uint8_t max_volume; /* 1-100 */
    bool admin_set;     /* false = sortie d'usine, assistant de configuration */
    bool mpd_pass_set;
} settings_t;

esp_err_t settings_init(void);
void settings_get(settings_t *out);

esp_err_t settings_set_hostname(const char *hostname);
esp_err_t settings_set_wifi(const char *ssid, const char *pass);
esp_err_t settings_set_ota(const char *url, uint16_t interval_h);
esp_err_t settings_set_max_volume(uint8_t max_volume);
esp_err_t settings_set_resume(uint32_t timeout_s, bool after_other);
/* Enregistrement différé (évite d'user la flash quand le volume bouge beaucoup). */
void settings_set_volume_deferred(uint8_t volume);

esp_err_t settings_set_admin_password(const char *password);
bool settings_check_admin_password(const char *password);
/* Chaîne vide = pas de mot de passe MPD. */
esp_err_t settings_set_mpd_password(const char *password);
bool settings_check_mpd_password(const char *password);

/* Efface réglages, mots de passe et associations de cartes. */
esp_err_t settings_factory_reset(void);
