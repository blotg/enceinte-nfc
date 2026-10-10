#include "config_json.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

#define FAIL(...)                              \
    do {                                       \
        snprintf(err, errlen, __VA_ARGS__);    \
        return false;                          \
    } while (0)

cJSON *config_document(const char *format, const char *device)
{
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "format", format);
        cJSON_AddNumberToObject(root, "version", CONFIG_VERSION);
        if (device && device[0]) {
            cJSON_AddStringToObject(root, "device", device);
        }
    }
    return root;
}

bool config_document_check(const cJSON *root, const char *format, char *err, size_t errlen)
{
    const cJSON *f = cJSON_GetObjectItem(root, "format");
    const cJSON *v = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsObject(root) || !cJSON_IsString(f) || strcmp(f->valuestring, format) != 0) {
        FAIL("ce fichier n'est pas un fichier de réglages d'enceinte");
    }
    if (cJSON_IsNumber(v) && v->valuedouble > CONFIG_VERSION) {
        FAIL("fichier créé par une version plus récente du firmware");
    }
    return true;
}

static void add_ip(cJSON *obj, const char *key, uint32_t ip)
{
    char buf[16];
    ip4_format(ip, buf);
    cJSON_AddStringToObject(obj, key, buf);
}

static void add_hash(cJSON *obj, const char *key, const uint8_t *hash)
{
    char b64[4 * ((SETTINGS_PW_HASH_LEN + 2) / 3) + 1];
    base64_encode(hash, SETTINGS_PW_HASH_LEN, b64);
    cJSON_AddStringToObject(obj, key, b64);
}

cJSON *config_to_json(const config_t *c, bool secrets)
{
    const settings_t *s = &c->s;
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "hostname", s->hostname);
    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(w, "ssid", s->wifi_ssid);
    if (secrets) {
        cJSON_AddStringToObject(w, "password", s->wifi_pass);
    }
    cJSON *ip = cJSON_AddObjectToObject(o, "ip");
    cJSON_AddStringToObject(ip, "mode", s->ip.static_ip ? "static" : "dhcp");
    if (s->ip.static_ip) {
        add_ip(ip, "address", s->ip.address);
        add_ip(ip, "netmask", s->ip.netmask);
        add_ip(ip, "gateway", s->ip.gateway);
        if (s->ip.dns) {
            add_ip(ip, "dns", s->ip.dns);
        }
    }
    cJSON_AddNumberToObject(o, "max_volume", s->max_volume);
    cJSON_AddNumberToObject(o, "resume_s", s->resume_timeout_s);
    cJSON_AddBoolToObject(o, "resume_after_other", s->resume_after_other);
    cJSON_AddBoolToObject(o, "shuffle", s->shuffle);
    cJSON_AddBoolToObject(o, "repeat", s->repeat);
    cJSON_AddNumberToObject(o, "normalize", s->normalize);
    cJSON_AddNumberToObject(o, "compress", s->compress);
    cJSON *ctl = cJSON_AddObjectToObject(o, "controls");
    cJSON_AddStringToObject(ctl, "type", s->vol_touch ? "touch" : "buttons");
    cJSON_AddNumberToObject(ctl, "touch_threshold_pct", s->touch_threshold / 10.0);
    cJSON_AddNumberToObject(ctl, "touch_hold_ms", s->touch_hold_ms);
    cJSON_AddBoolToObject(o, "https", s->https_enabled);
    cJSON_AddStringToObject(o, "ota_url", s->ota_url);
    cJSON_AddNumberToObject(o, "ota_interval_h", s->ota_interval_h);
    if (secrets) {
        if (c->admin_hash) {
            add_hash(o, "admin_password_hash", c->admin);
        }
        if (c->mpd_hash == 1) {
            add_hash(o, "mpd_password_hash", c->mpd);
        } else if (c->mpd_hash == 0) {
            cJSON_AddNullToObject(o, "mpd_password_hash");
        }
    }
    return o;
}

/* Entier facultatif dans [lo, hi] ; absent : true sans rien changer. */
static bool get_int(const cJSON *o, const char *key, long lo, long hi, long *out, bool *present)
{
    const cJSON *it = cJSON_GetObjectItem(o, key);
    *present = it != NULL;
    if (!it) {
        return true;
    }
    double d = cJSON_IsNumber(it) ? it->valuedouble : -1e300;
    if (d < lo || d > hi || d != (double)(long)d) {
        return false;
    }
    *out = (long)d;
    return true;
}

static bool get_bool(const cJSON *o, const char *key, bool *out)
{
    const cJSON *it = cJSON_GetObjectItem(o, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsBool(it)) {
        return false;
    }
    *out = cJSON_IsTrue(it);
    return true;
}

static bool get_str(const cJSON *o, const char *key, char *out, size_t len, bool *present)
{
    const cJSON *it = cJSON_GetObjectItem(o, key);
    if (present) {
        *present = it != NULL;
    }
    if (!it) {
        return true;
    }
    if (!cJSON_IsString(it) || strlen(it->valuestring) >= len) {
        return false;
    }
    str_copy(out, it->valuestring, len);
    return true;
}

static bool get_ip(const cJSON *o, const char *key, uint32_t *out)
{
    const cJSON *it = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(it) && ip4_parse(it->valuestring, out);
}

static bool get_hash(const cJSON *it, uint8_t *out)
{
    return cJSON_IsString(it) && base64_decode(it->valuestring, out, SETTINGS_PW_HASH_LEN) == SETTINGS_PW_HASH_LEN &&
           settings_password_hash_valid(out);
}

bool config_from_json(const cJSON *o, config_t *out, char *err, size_t errlen)
{
    if (!cJSON_IsObject(o)) {
        FAIL("réglages absents du fichier");
    }
    config_t c = *out;
    settings_t *s = &c.s;
    bool present;
    long v;

    char host[64];
    str_copy(host, s->hostname, sizeof(host));
    if (!get_str(o, "hostname", host, sizeof(host), &present) || !hostname_normalize(host, s->hostname, sizeof(s->hostname))) {
        FAIL("nom de l'enceinte invalide");
    }

    const cJSON *w = cJSON_GetObjectItem(o, "wifi");
    if (w) {
        char ssid[sizeof(s->wifi_ssid)], pass[sizeof(s->wifi_pass)];
        bool has_ssid, has_pass;
        if (!cJSON_IsObject(w) || !get_str(w, "ssid", ssid, sizeof(ssid), &has_ssid) ||
            !get_str(w, "password", pass, sizeof(pass), &has_pass)) {
            FAIL("réseau Wi-Fi invalide");
        }
        /* Sans mot de passe (export sans secrets), le réseau n'est pas modifié. */
        if (has_ssid && has_pass) {
            size_t pl = strlen(pass);
            if ((pl > 0 && pl < 8) || (!ssid[0] && pl)) {
                FAIL("mot de passe du Wi-Fi invalide (8 caractères minimum)");
            }
            str_copy(s->wifi_ssid, ssid, sizeof(s->wifi_ssid));
            str_copy(s->wifi_pass, pass, sizeof(s->wifi_pass));
            c.wifi = true;
        }
    }

    const cJSON *ip = cJSON_GetObjectItem(o, "ip");
    if (ip) {
        const cJSON *mode = cJSON_GetObjectItem(ip, "mode");
        if (!cJSON_IsObject(ip) || !cJSON_IsString(mode)) {
            FAIL("adresse IP invalide");
        }
        if (strcmp(mode->valuestring, "dhcp") == 0) {
            s->ip.static_ip = false;
        } else if (strcmp(mode->valuestring, "static") == 0) {
            ip_config_t n = {.static_ip = true};
            if (!get_ip(ip, "address", &n.address) || !get_ip(ip, "netmask", &n.netmask) ||
                !get_ip(ip, "gateway", &n.gateway) || (cJSON_GetObjectItem(ip, "dns") && !get_ip(ip, "dns", &n.dns))) {
                FAIL("adresse IP fixe incomplète ou mal écrite");
            }
            const char *why = NULL;
            if (!settings_ip_valid(&n, &why)) {
                FAIL("adresse IP fixe : %s", why ? why : "invalide");
            }
            s->ip = n;
        } else {
            FAIL("mode d'adresse IP inconnu (dhcp ou static)");
        }
    }

    v = s->max_volume;
    if (!get_int(o, "max_volume", 1, 100, &v, &present)) {
        FAIL("volume maximum invalide (1 à 100)");
    }
    s->max_volume = (uint8_t)v;
    v = (long)s->resume_timeout_s;
    if (!get_int(o, "resume_s", 0, 30L * 24 * 3600, &v, &present)) {
        FAIL("durée de conservation de la progression invalide (30 jours maximum)");
    }
    s->resume_timeout_s = (uint32_t)v;
    if (!get_bool(o, "resume_after_other", &s->resume_after_other) || !get_bool(o, "shuffle", &s->shuffle) ||
        !get_bool(o, "repeat", &s->repeat) || !get_bool(o, "https", &s->https_enabled)) {
        FAIL("réglage oui/non invalide");
    }
    v = s->normalize;
    if (!get_int(o, "normalize", 0, SOUND_LEVEL_MAX, &v, &present)) {
        FAIL("normalisation invalide (0 à %d)", SOUND_LEVEL_MAX);
    }
    s->normalize = (uint8_t)v;
    v = s->compress;
    if (!get_int(o, "compress", 0, SOUND_LEVEL_MAX, &v, &present)) {
        FAIL("compression invalide (0 à %d)", SOUND_LEVEL_MAX);
    }
    s->compress = (uint8_t)v;
    const cJSON *ctl = cJSON_GetObjectItem(o, "controls");
    if (ctl) {
        const cJSON *type = cJSON_GetObjectItem(ctl, "type");
        const cJSON *thr = cJSON_GetObjectItem(ctl, "touch_threshold_pct");
        if (!cJSON_IsObject(ctl) || (type && (!cJSON_IsString(type) || (strcmp(type->valuestring, "touch") != 0 &&
                                                                         strcmp(type->valuestring, "buttons") != 0)))) {
            FAIL("commandes de volume invalides (touch ou buttons)");
        }
        if (type) {
            s->vol_touch = strcmp(type->valuestring, "touch") == 0;
        }
        if (thr) {
            double p = cJSON_IsNumber(thr) ? thr->valuedouble * 10.0 : -1;
            if (p < TOUCH_THRESHOLD_MIN - 0.01 || p > TOUCH_THRESHOLD_MAX + 0.01) {
                FAIL("seuil des touches tactiles invalide (0,3 à 30 %%)");
            }
            s->touch_threshold = (uint16_t)(p + 0.5);
        }
        v = s->touch_hold_ms;
        if (!get_int(ctl, "touch_hold_ms", 0, TOUCH_HOLD_MAX_MS, &v, &present)) {
            FAIL("maintien des touches tactiles invalide (0 à 3000 ms)");
        }
        s->touch_hold_ms = (uint16_t)v;
    }
    if (!get_str(o, "ota_url", s->ota_url, sizeof(s->ota_url), &present) ||
        (s->ota_url[0] && strncmp(s->ota_url, "http://", 7) != 0 && strncmp(s->ota_url, "https://", 8) != 0)) {
        FAIL("source des mises à jour invalide (http:// ou https://)");
    }
    v = s->ota_interval_h;
    if (!get_int(o, "ota_interval_h", 1, 24 * 30, &v, &present)) {
        FAIL("intervalle des mises à jour invalide (1 à 720 heures)");
    }
    s->ota_interval_h = (uint16_t)v;

    const cJSON *ah = cJSON_GetObjectItem(o, "admin_password_hash");
    if (ah) {
        if (!get_hash(ah, c.admin)) {
            FAIL("empreinte du mot de passe administrateur invalide");
        }
        c.admin_hash = true;
    }
    const cJSON *mh = cJSON_GetObjectItem(o, "mpd_password_hash");
    if (cJSON_IsNull(mh)) {
        c.mpd_hash = 0;
    } else if (mh) {
        if (!get_hash(mh, c.mpd)) {
            FAIL("empreinte du mot de passe MPD invalide");
        }
        c.mpd_hash = 1;
    }
    *out = c;
    return true;
}

static void add_opt_int(cJSON *o, const char *key, long v)
{
    if (v == CARD_DEFAULT) {
        cJSON_AddNullToObject(o, key);
    } else {
        cJSON_AddNumberToObject(o, key, (double)v);
    }
}

static void add_opt_bool(cJSON *o, const char *key, int8_t v)
{
    if (v == CARD_DEFAULT) {
        cJSON_AddNullToObject(o, key);
    } else {
        cJSON_AddBoolToObject(o, key, v == 1);
    }
}

cJSON *card_to_json(const card_entry_t *e, bool with_folder)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "uid", e->uid);
    if (with_folder) {
        cJSON_AddStringToObject(o, "folder", e->folder);
    }
    add_opt_int(o, "resume_s", e->resume_s);
    add_opt_bool(o, "resume_other", e->resume_other);
    add_opt_bool(o, "shuffle", e->shuffle);
    add_opt_bool(o, "repeat", e->repeat);
    add_opt_int(o, "normalize", e->normalize);
    add_opt_int(o, "compress", e->compress);
    cJSON_AddNumberToObject(o, "sleep_tracks", e->sleep_tracks);
    cJSON_AddNumberToObject(o, "sleep_minutes", e->sleep_minutes);
    return o;
}

/* Réglage de carte facultatif : absent ou null = réglage général. */
static bool opt_int(const cJSON *o, const char *key, long hi, int32_t *out)
{
    const cJSON *it = cJSON_GetObjectItem(o, key);
    if (!it || cJSON_IsNull(it)) {
        *out = CARD_DEFAULT;
        return true;
    }
    double d = cJSON_IsNumber(it) ? it->valuedouble : -1;
    if (d < 0 || d > hi || d != (double)(long)d) {
        return false;
    }
    *out = (int32_t)d;
    return true;
}

/* Compteur facultatif : absent ou null = 0. */
static bool opt_count(const cJSON *o, const char *key, long hi, uint16_t *out)
{
    const cJSON *it = cJSON_GetObjectItem(o, key);
    if (!it || cJSON_IsNull(it)) {
        *out = 0;
        return true;
    }
    double d = cJSON_IsNumber(it) ? it->valuedouble : -1;
    if (d < 0 || d > hi || d != (double)(long)d) {
        return false;
    }
    *out = (uint16_t)d;
    return true;
}

static bool opt_bool(const cJSON *o, const char *key, int8_t *out)
{
    const cJSON *it = cJSON_GetObjectItem(o, key);
    if (!it || cJSON_IsNull(it)) {
        *out = CARD_DEFAULT;
        return true;
    }
    if (!cJSON_IsBool(it)) {
        return false;
    }
    *out = cJSON_IsTrue(it) ? 1 : 0;
    return true;
}

bool card_from_json(const cJSON *o, card_entry_t *e, bool with_folder, char *err, size_t errlen)
{
    memset(e, 0, sizeof(*e));
    const cJSON *uid = cJSON_GetObjectItem(o, "uid");
    size_t n = cJSON_IsString(uid) ? strlen(uid->valuestring) : 0;
    if (!cJSON_IsObject(o) || n < 8 || n >= UID_STR_MAX || n % 2) {
        FAIL("identifiant de carte invalide");
    }
    for (size_t i = 0; i < n; i++) {
        char c = (char)toupper((unsigned char)uid->valuestring[i]);
        if (!isxdigit((unsigned char)c)) {
            FAIL("identifiant de carte invalide : %s", uid->valuestring);
        }
        e->uid[i] = c;
    }
    e->uid[n] = '\0';
    if (with_folder) {
        const cJSON *f = cJSON_GetObjectItem(o, "folder");
        if (!cJSON_IsString(f) || !path_sanitize(f->valuestring, e->folder, sizeof(e->folder)) || !e->folder[0]) {
            FAIL("dossier invalide pour la carte %s", e->uid);
        }
    }
    int32_t normalize, compress;
    if (!opt_int(o, "resume_s", 30L * 24 * 3600, &e->resume_s) || !opt_bool(o, "resume_other", &e->resume_other) ||
        !opt_bool(o, "shuffle", &e->shuffle) || !opt_bool(o, "repeat", &e->repeat) ||
        !opt_int(o, "normalize", SOUND_LEVEL_MAX, &normalize) ||
        !opt_int(o, "compress", SOUND_LEVEL_MAX, &compress)) {
        FAIL("réglages invalides pour la carte %s", e->uid);
    }
    if (!opt_count(o, "sleep_tracks", SLEEP_TRACKS_MAX, &e->sleep_tracks) ||
        !opt_count(o, "sleep_minutes", SLEEP_MINUTES_MAX, &e->sleep_minutes)) {
        FAIL("mode sommeil invalide pour la carte %s (%d morceaux, %d minutes au plus)", e->uid, SLEEP_TRACKS_MAX,
             SLEEP_MINUTES_MAX);
    }
    e->normalize = (int8_t)normalize;
    e->compress = (int8_t)compress;
    return true;
}
