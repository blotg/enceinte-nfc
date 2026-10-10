#pragma once
/*
 * Réglages persistants (partition NVS "cfg").
 * Les mots de passe ne sont jamais stockés en clair (PBKDF2-HMAC-SHA256 salé).
 */
#include <stdbool.h>
#include <stdint.h>

#include <string.h>

#include "esp_err.h"
#include "util.h"

#define CFG_PARTITION "cfg"

#define SOUND_LEVEL_MAX 3        /* normalisation et compression : 0 = désactivée, 1 à 3 */
#define TOUCH_THRESHOLD_MIN 3     /* touches tactiles : seuil en millièmes (0,3 % à 30 %) */
#define TOUCH_THRESHOLD_MAX 300
#define TOUCH_HOLD_MAX_MS 3000
#define SETTINGS_PW_HASH_LEN 52  /* empreinte d'un mot de passe : itérations, sel, PBKDF2 */

/* Adresse IP de l'enceinte sur le réseau Wi-Fi (ordre « hôte », cf. ip4_parse). */
typedef struct {
    bool static_ip; /* false : DHCP */
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns; /* 0 : la passerelle */
} ip_config_t;

typedef struct {
    char hostname[33];
    char wifi_ssid[33];
    char wifi_pass[65];
    char ota_url[256];
    uint16_t ota_interval_h;
    uint32_t resume_timeout_s; /* délai de reprise après retrait, 0 = toujours */
    bool resume_after_other;   /* reprendre même si une autre carte a été posée entre-temps */
    bool shuffle;              /* cartes : playlist dans un ordre aléatoire */
    bool repeat;               /* cartes : la playlist recommence quand elle est finie */
    bool https_enabled;        /* interface web aussi en HTTPS (certificat auto-signé) */
    uint8_t normalize;         /* égalisation du niveau entre morceaux et playlists, 0-3 */
    uint8_t compress;          /* réduction des écarts de volume dans un morceau, 0-3 */
    ip_config_t ip;
    bool vol_touch;            /* volume : touches tactiles (sinon boutons poussoirs) */
    uint16_t touch_threshold;  /* touches tactiles : seuil, millièmes de la valeur de repos */
    uint16_t touch_hold_ms;    /* touches tactiles : maintien avant le premier cran */
    uint8_t volume;     /* 0-max_volume, dernier volume utilisé */
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
esp_err_t settings_set_shuffle(bool on);
esp_err_t settings_set_repeat(bool on);
esp_err_t settings_set_https(bool enabled);
esp_err_t settings_set_sound(uint8_t normalize, uint8_t compress);
esp_err_t settings_set_ip(const ip_config_t *ip);
esp_err_t settings_set_controls(bool touch, uint16_t threshold, uint16_t hold_ms);

/* Adresse fixe cohérente (DHCP : toujours valide). why : message d'erreur en français. */
static inline bool settings_ip_valid(const ip_config_t *ip, const char **why)
{
    if (why) {
        *why = NULL;
    }
    if (!ip->static_ip) {
        return true;
    }
    if (!ip4_config_check(ip->address, ip->netmask, ip->gateway, why)) {
        return false;
    }
    if (ip->dns && (ip->dns >> 24 == 0 || ip->dns >> 24 == 127 || ip->dns >> 24 >= 224)) {
        if (why) {
            *why = "serveur DNS invalide";
        }
        return false;
    }
    return true;
}
/*
 * Remplace d'un coup tous les réglages de configuration (carte SD, import), après les
 * avoir tous vérifiés. Le volume courant et les mots de passe ne sont pas concernés.
 */
esp_err_t settings_set_all(const settings_t *cfg);
/* Volume enregistré en flash 1 s après le dernier changement (un bouton maintenu ou un
 * curseur déplacé n'écrit qu'une fois). */
void settings_set_volume_deferred(uint8_t volume);
/* Enregistre tout de suite un volume en attente (avant un redémarrage). */
void settings_flush(void);
/* Appelée après chaque modification enregistrée de la configuration (copie sur la carte SD). */
void settings_set_observer(void (*cb)(void));

esp_err_t settings_set_admin_password(const char *password);
bool settings_check_admin_password(const char *password);
/* Chaîne vide = pas de mot de passe MPD. */
esp_err_t settings_set_mpd_password(const char *password);
bool settings_check_mpd_password(const char *password);
/* Empreintes (copie sur carte SD, export) : false si aucun mot de passe. */
bool settings_get_password_hash(bool admin, uint8_t out[SETTINGS_PW_HASH_LEN]);
static inline bool settings_password_hash_valid(const uint8_t *hash)
{
    uint32_t it; /* nombre d'itérations PBKDF2, en tête */
    memcpy(&it, hash, 4);
    return it > 0 && it <= 1000000;
}
/* hash NULL : supprime le mot de passe (MPD seulement). */
esp_err_t settings_set_password_hash(bool admin, const uint8_t *hash);

/* Efface réglages, mots de passe et associations de cartes. */
esp_err_t settings_factory_reset(void);
