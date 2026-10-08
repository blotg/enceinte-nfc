#include "web_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backup.h"
#include "buttons.h"
#include "cJSON.h"
#include "cards.h"
#include "changes.h"
#include "controller.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "media_info.h"
#include "nfc.h"
#include "ota.h"
#include "player.h"
#include "sdkconfig.h"
#include "settings.h"
#include "storage.h"
#include "tls_cert.h"
#include "util.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

#define BODY_MAX 4096
#define IMPORT_MAX (160 * 1024) /* 300 cartes aux noms de dossier très longs */
#define UPLOAD_BUF 8192
#define MAX_SESSIONS 8
#define SESSION_IDLE_US (30LL * 24 * 3600 * 1000000) /* 30 jours */
#define CSRF_HEADER "X-Requested-With"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");
extern const char style_css_start[] asm("_binary_style_css_start");
extern const char style_css_end[] asm("_binary_style_css_end");

typedef struct {
    char token[33];
    int64_t last_used;
} web_session_t;

static web_session_t s_sessions[MAX_SESSIONS];
static httpd_handle_t s_http, s_https;
static volatile bool s_https_busy; /* démarrage/arrêt du serveur HTTPS en cours */
/* user_ctx des gestionnaires : indique par quel serveur la requête est arrivée */
static const int s_mark_http = 0, s_mark_https = 1;
static volatile int s_transfers; /* envois en cours */
static volatile int64_t s_last_transfer_us;

bool web_server_busy(void)
{
    /* Le navigateur envoie les fichiers l'un après l'autre : on attend aussi un peu après le dernier. */
    return s_transfers > 0 || (s_last_transfer_us && esp_timer_get_time() - s_last_transfer_us < 5LL * 60 * 1000000);
}

bool web_server_transfers_active(void)
{
    return s_transfers > 0 || (s_last_transfer_us && esp_timer_get_time() - s_last_transfer_us < 10LL * 1000000);
}

static void transfer_begin(void)
{
    __atomic_add_fetch(&s_transfers, 1, __ATOMIC_SEQ_CST);
}

static void transfer_end(void)
{
    s_last_transfer_us = esp_timer_get_time();
    __atomic_sub_fetch(&s_transfers, 1, __ATOMIC_SEQ_CST);
}
static int s_login_failures;
static int64_t s_login_blocked_until;

/* ================= Outils HTTP ================= */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static cJSON *read_json_max(httpd_req_t *req, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) {
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    size_t got = 0;
    int retries = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && retries++ < 3) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        got += n;
    }
    buf[got] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static cJSON *read_json(httpd_req_t *req)
{
    return read_json_max(req, BODY_MAX);
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static bool get_query(httpd_req_t *req, const char *key, char *out, size_t len)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 1024) {
        return false;
    }
    char *q = malloc(qlen + 1);
    char *raw = malloc(len * 3 + 1);
    bool ok = q && raw && httpd_req_get_url_query_str(req, q, qlen + 1) == ESP_OK &&
              httpd_query_key_value(q, key, raw, len * 3 + 1) == ESP_OK && url_decode(raw, out, len);
    free(q);
    free(raw);
    return ok;
}

/* Adresse IPv4 (ordre « hôte ») de l'enceinte par laquelle la requête est arrivée. */
static uint32_t local_ip(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        return 0;
    }
    uint32_t ip = addr.sin6_family == AF_INET ? ((struct sockaddr_in *)&addr)->sin_addr.s_addr
                                              : addr.sin6_addr.un.u32_addr[3]; /* IPv4 mappée */
    return ntohl(ip);
}

static bool client_on_ap(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0) {
        return false;
    }
    uint32_t ip = 0;
    if (addr.sin6_family == AF_INET) {
        ip = ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    } else {
        ip = addr.sin6_addr.un.u32_addr[3]; /* IPv4 mappée */
    }
    return (ntohl(ip) & 0xFFFFFF00) == 0xC0A80400; /* 192.168.4.0/24 */
}

/* ================= HTTPS ================= */

static bool via_https(httpd_req_t *req)
{
    return req->user_ctx == &s_mark_https;
}

/* HTTPS activé : les accès HTTP depuis le réseau local passent en HTTPS (le portail de
 * configuration du point d'accès reste en HTTP, les téléphones l'exigent). */
static bool must_use_https(httpd_req_t *req)
{
    return s_https && !via_https(req) && !client_on_ap(req);
}

static void https_url(httpd_req_t *req, const char *uri, char *out, size_t len)
{
    char host[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || !host[0]) {
        settings_t cfg;
        settings_get(&cfg);
        snprintf(host, sizeof(host), "%s.local", cfg.hostname);
    }
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
    }
    snprintf(out, len, "https://%s%s", host, uri);
}

static esp_err_t redirect_https(httpd_req_t *req)
{
    static char location[320];
    https_url(req, req->uri, location, sizeof(location));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

/* API appelée en HTTP alors que HTTPS est actif : l'interface se recharge en HTTPS. */
static esp_err_t deny_http(httpd_req_t *req)
{
    char url[300];
    https_url(req, "/", url, sizeof(url));
    httpd_resp_set_status(req, "403 Forbidden");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "HTTPS requis");
    cJSON_AddStringToObject(root, "https", url);
    return send_json(req, root);
}

/* ================= Authentification ================= */

static web_session_t *find_session(httpd_req_t *req)
{
    char token[40];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, "sid", token, &len) != ESP_OK || strlen(token) != 32) {
        return NULL;
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        web_session_t *s = &s_sessions[i];
        if (s->token[0] && now - s->last_used < SESSION_IDLE_US) {
            uint8_t diff = 0;
            for (int k = 0; k < 32; k++) {
                diff |= (uint8_t)(s->token[k] ^ token[k]);
            }
            if (diff == 0) {
                s->last_used = now;
                return s;
            }
        }
    }
    return NULL;
}

static void create_session(httpd_req_t *req)
{
    int slot = 0;
    for (int i = 1; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].last_used < s_sessions[slot].last_used) {
            slot = i; /* la plus ancienne est remplacée */
        }
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    bytes_to_hex(rnd, sizeof(rnd), s_sessions[slot].token);
    s_sessions[slot].last_used = esp_timer_get_time();
    static char cookie[112];
    snprintf(cookie, sizeof(cookie), "sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000%s",
             s_sessions[slot].token, via_https(req) ? "; Secure" : "");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

static void clear_sessions(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
}

/* Les requêtes qui modifient l'état doivent porter l'en-tête CSRF (impossible à
 * ajouter depuis un autre site sans autorisation CORS, que l'enceinte ne donne pas). */
static bool csrf_ok(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return true;
    }
    char v[16];
    return httpd_req_get_hdr_value_str(req, CSRF_HEADER, v, sizeof(v)) == ESP_OK && strcmp(v, "enceinte") == 0;
}

static bool require_auth(httpd_req_t *req)
{
    if (must_use_https(req)) {
        deny_http(req);
        return false;
    }
    if (!csrf_ok(req)) {
        send_error(req, "403 Forbidden", "requête refusée");
        return false;
    }
    if (!find_session(req)) {
        send_error(req, "401 Unauthorized", "connexion requise");
        return false;
    }
    if (wifi_mgr_ip_testing()) {
        wifi_mgr_ip_confirm(local_ip(req)); /* administrateur connecté à la nouvelle adresse */
    }
    return true;
}

/* ================= Fichiers statiques ================= */

/*
 * Après une mise à jour, le navigateur ne doit pas garder l'ancienne interface :
 *  - chaque firmware a un identifiant (début de l'empreinte SHA-256 de son ELF) ;
 *  - index.html appelle app.js et style.css avec ?v=<identifiant> et le porte dans une
 *    balise meta : la page compare son identifiant à celui de l'enceinte et se recharge
 *    si l'enceinte a changé de firmware pendant qu'elle était ouverte ;
 *  - les trois fichiers sont revalidés à chaque chargement (no-cache) grâce à leur ETag :
 *    réponse 304 sans contenu tant que le firmware est le même.
 */
static char s_build[13];
static char s_etag[16];
static char *s_index; /* index.html, identifiant inséré */
static size_t s_index_len;

static void prepare_static(void)
{
    esp_app_get_elf_sha256(s_build, sizeof(s_build));
    snprintf(s_etag, sizeof(s_etag), "\"%s\"", s_build);
    static const char mark[] = "{{build}}";
    const char *src = index_html_start;
    size_t len = index_html_end - index_html_start - 1; /* EMBED_TXTFILES ajoute un octet nul final */
    s_index = malloc(len + 8 * strlen(s_build) + 1);
    if (!s_index) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        if (i + sizeof(mark) - 1 <= len && memcmp(src + i, mark, sizeof(mark) - 1) == 0) {
            memcpy(s_index + o, s_build, strlen(s_build));
            o += strlen(s_build);
            i += sizeof(mark) - 1;
        } else {
            s_index[o++] = src[i++];
        }
    }
    s_index[o] = '\0';
    s_index_len = o;
}

static esp_err_t send_static(httpd_req_t *req, const char *data, size_t len, const char *type)
{
    if (must_use_https(req)) {
        return redirect_https(req);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", s_etag);
    char inm[24];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK && strcmp(inm, s_etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, data, len);
}

static esp_err_t h_index(httpd_req_t *req)
{
    if (!s_index) {
        return send_static(req, index_html_start, index_html_end - index_html_start - 1, "text/html; charset=utf-8");
    }
    return send_static(req, s_index, s_index_len, "text/html; charset=utf-8");
}

static esp_err_t h_app_js(httpd_req_t *req)
{
    return send_static(req, app_js_start, app_js_end - app_js_start - 1, "application/javascript; charset=utf-8");
}

static esp_err_t h_style(httpd_req_t *req)
{
    return send_static(req, style_css_start, style_css_end - style_css_start - 1, "text/css; charset=utf-8");
}

/* Portail captif : toute adresse inconnue renvoie vers la page de configuration. */
static esp_err_t h_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (wifi_mgr_ap_active() && client_on_ap(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" AP_IP_STR "/");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "Redirection", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Introuvable");
    return ESP_FAIL;
}

/* ================= État / connexion ================= */

static esp_err_t h_state(httpd_req_t *req)
{
    if (must_use_https(req)) {
        return deny_http(req);
    }
    settings_t cfg;
    settings_get(&cfg);
    ota_status_t os;
    ota_get_status(&os);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "setup_required", !cfg.admin_set);
    cJSON_AddBoolToObject(root, "logged_in", cfg.admin_set && find_session(req) != NULL);
    cJSON_AddStringToObject(root, "version", os.current_version);
    cJSON_AddStringToObject(root, "build", s_build);
    cJSON_AddStringToObject(root, "hostname", cfg.hostname);
    cJSON_AddBoolToObject(root, "on_ap", client_on_ap(req));
    return send_json(req, root);
}

static esp_err_t h_setup(httpd_req_t *req)
{
    if (!csrf_ok(req)) {
        return send_error(req, "403 Forbidden", "requête refusée");
    }
    settings_t cfg;
    settings_get(&cfg);
    if (cfg.admin_set) {
        return send_error(req, "403 Forbidden", "déjà configurée");
    }
    cJSON *body = read_json(req);
    const char *pw = json_str(body, "password");
    const char *ssid = json_str(body, "ssid");
    const char *wpass = json_str(body, "wifi_password");
    const char *host = json_str(body, "hostname");
    if (!wpass) {
        wpass = "";
    }
    /* Tout est vérifié avant d'enregistrer quoi que ce soit : une erreur laisse
     * l'assistant utilisable. */
    char norm[33];
    const char *msg = NULL;
    if (!pw || strlen(pw) < 6 || strlen(pw) > 64) {
        msg = "mot de passe trop court (6 caractères minimum)";
    } else if (host && host[0] && !hostname_normalize(host, norm, sizeof(norm))) {
        msg = "nom invalide (lettres, chiffres et tirets, 32 max)";
    } else if (ssid && ssid[0] && (strlen(ssid) > 32 || strlen(wpass) > 64 || (wpass[0] && strlen(wpass) < 8))) {
        msg = "réseau Wi-Fi invalide (mot de passe de 8 caractères minimum)";
    }
    esp_err_t err = msg ? ESP_ERR_INVALID_ARG : ESP_OK;
    if (err == ESP_OK && host && host[0]) {
        err = settings_set_hostname(norm);
        if (err == ESP_OK) {
            wifi_mgr_set_hostname(norm);
        }
    }
    if (err == ESP_OK && ssid && ssid[0]) {
        err = settings_set_wifi(ssid, wpass);
    }
    if (err == ESP_OK) {
        err = settings_set_admin_password(pw); /* en dernier : marque la fin de l'assistant */
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", msg ? msg : "enregistrement impossible");
    }
    if (ssid && ssid[0]) {
        wifi_mgr_reconnect_later(1500);
    }
    ESP_LOGI(TAG, "configuration initiale terminée");
    create_session(req);
    return send_ok(req);
}

static esp_err_t h_login(httpd_req_t *req)
{
    if (must_use_https(req)) {
        return deny_http(req);
    }
    if (!csrf_ok(req)) {
        return send_error(req, "403 Forbidden", "requête refusée");
    }
    int64_t now = esp_timer_get_time();
    if (now < s_login_blocked_until) {
        return send_error(req, "429 Too Many Requests", "trop d'essais, patientez quelques secondes");
    }
    cJSON *body = read_json(req);
    const char *pw = json_str(body, "password");
    bool ok = pw && settings_check_admin_password(pw);
    cJSON_Delete(body);
    if (!ok) {
        if (++s_login_failures >= 5) {
            int shift = s_login_failures - 5 < 6 ? s_login_failures - 5 : 6;
            s_login_blocked_until = now + (30LL << shift) * 1000000;
        }
        return send_error(req, "401 Unauthorized", "mot de passe incorrect");
    }
    s_login_failures = 0;
    create_session(req);
    return send_ok(req);
}

static esp_err_t h_logout(httpd_req_t *req)
{
    web_session_t *s = find_session(req);
    if (s) {
        memset(s, 0, sizeof(*s));
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0");
    return send_ok(req);
}

/* ================= Statut ================= */

static const char *state_name(player_state_t st)
{
    return st == PLAYER_PLAYING ? "play" : (st == PLAYER_PAUSED ? "pause" : "stop");
}

/* Cache des tags du morceau courant (évite de relire le fichier à chaque rafraîchissement). */
static char s_tag_path[REL_PATH_MAX];
static char s_tag_title[128], s_tag_artist[128], s_tag_album[128];

static void current_tags(const char *rel)
{
    if (strcmp(rel, s_tag_path) == 0) {
        return;
    }
    str_copy(s_tag_path, rel, sizeof(s_tag_path));
    s_tag_title[0] = s_tag_artist[0] = s_tag_album[0] = '\0';
    char abs[ABS_PATH_MAX];
    if (!rel[0] || !path_to_abs(rel, abs, sizeof(abs))) {
        return;
    }
    FILE *f = fopen(abs, "rb");
    if (!f) {
        return;
    }
    media_info_t *mi = malloc(sizeof(media_info_t));
    if (mi && media_probe(f, audio_fmt_from_name(rel), mi)) {
        str_copy(s_tag_title, mi->title, sizeof(s_tag_title));
        str_copy(s_tag_artist, mi->artist, sizeof(s_tag_artist));
        str_copy(s_tag_album, mi->album, sizeof(s_tag_album));
    }
    free(mi);
    fclose(f);
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    player_status_t ps;
    player_get_status(&ps);
    controller_status_t cs;
    controller_get_status(&cs);
    wifi_status_t ws;
    wifi_mgr_get_status(&ws);
    ota_status_t os;
    ota_get_status(&os);
    current_tags(ps.file);

    cJSON *root = cJSON_CreateObject();
    cJSON *p = cJSON_AddObjectToObject(root, "player");
    cJSON_AddStringToObject(p, "state", state_name(ps.state));
    cJSON_AddStringToObject(p, "file", ps.file);
    cJSON_AddStringToObject(p, "title", s_tag_title[0] ? s_tag_title : path_basename(ps.file));
    cJSON_AddStringToObject(p, "artist", s_tag_artist);
    cJSON_AddStringToObject(p, "album", s_tag_album);
    cJSON_AddNumberToObject(p, "elapsed", ps.elapsed_ms / 1000.0);
    cJSON_AddNumberToObject(p, "duration", ps.duration_ms / 1000.0);
    cJSON_AddBoolToObject(p, "seekable", ps.seekable);
    cJSON_AddNumberToObject(p, "song", ps.song);
    cJSON_AddNumberToObject(p, "queue_len", ps.queue_len);
    cJSON_AddNumberToObject(p, "volume", ps.volume);
    cJSON_AddNumberToObject(p, "max_volume", player_get_max_volume());
    uint8_t normalize, compress;
    player_get_sound(&normalize, &compress);
    cJSON_AddNumberToObject(p, "normalize", normalize);
    cJSON_AddNumberToObject(p, "compress", compress);
    cJSON_AddBoolToObject(p, "repeat", ps.repeat);
    cJSON_AddBoolToObject(p, "random", ps.random);
    cJSON_AddStringToObject(p, "error", ps.error);

    cJSON *c = cJSON_AddObjectToObject(root, "card");
    cJSON_AddBoolToObject(c, "reader_ok", nfc_reader_ok());
    cJSON_AddStringToObject(c, "present", cs.present_uid);
    cJSON_AddStringToObject(c, "session", cs.session_uid);
    cJSON_AddStringToObject(c, "folder", cs.session_folder);
    cJSON_AddNumberToObject(c, "resume_remaining", cs.resume_remaining_s);
    cJSON_AddStringToObject(c, "last_unknown", cs.last_unknown_uid);

    cJSON *w = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(w, "connected", ws.sta_connected);
    cJSON_AddStringToObject(w, "ssid", ws.sta_ssid);
    cJSON_AddStringToObject(w, "ip", ws.sta_ip);
    cJSON_AddNumberToObject(w, "rssi", ws.rssi);
    cJSON_AddBoolToObject(w, "ap", ws.ap_active);
    cJSON_AddStringToObject(w, "ap_ssid", ws.ap_ssid);
    cJSON_AddStringToObject(w, "hostname", ws.hostname);
    cJSON_AddBoolToObject(w, "sta_available", ws.sta_available);
    cJSON_AddBoolToObject(w, "on_ap", client_on_ap(req));
    cJSON_AddNumberToObject(w, "ip_test_remaining", ws.ip_test_remaining);
    cJSON_AddStringToObject(w, "ip_test_address", ws.ip_test_address);

    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    uint64_t total = 0, freeb = 0;
    bool mounted = storage_get_usage(&total, &freeb);
    cJSON_AddBoolToObject(sd, "mounted", mounted);
    cJSON_AddNumberToObject(sd, "total", (double)total);
    cJSON_AddNumberToObject(sd, "free", (double)freeb);

    cJSON *o = cJSON_AddObjectToObject(root, "ota");
    cJSON_AddNumberToObject(o, "state", os.state);
    cJSON_AddStringToObject(o, "message", os.message);
    cJSON_AddStringToObject(o, "current", os.current_version);
    cJSON_AddStringToObject(o, "available", os.available_version);
    cJSON_AddNumberToObject(o, "progress", os.progress);
    cJSON_AddNumberToObject(o, "last_check", (double)os.last_check);

    cJSON_AddStringToObject(root, "build", s_build);
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return send_json(req, root);
}

/* ================= Lecture ================= */

static esp_err_t play_folder(httpd_req_t *req, const char *raw)
{
    char folder[REL_PATH_MAX];
    if (!path_sanitize(raw, folder, sizeof(folder))) {
        return send_error(req, "400 Bad Request", "dossier invalide");
    }
    path_list_t list;
    if (storage_list_tracks(folder, &list) != ESP_OK || list.count == 0) {
        path_list_free(&list);
        return send_error(req, "404 Not Found", "aucun morceau dans ce dossier");
    }
    esp_err_t err = player_queue_replace(&list);
    path_list_free(&list);
    if (err == ESP_OK) {
        player_play(0);
    }
    return err == ESP_OK ? send_ok(req) : send_error(req, "500 Internal Server Error", "mémoire insuffisante");
}

static esp_err_t h_player(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *action = json_str(body, "action");
    const cJSON *val = cJSON_GetObjectItem(body, "value");
    double v = cJSON_IsNumber(val) ? val->valuedouble : 0;
    esp_err_t err = ESP_OK;
    if (!action) {
        err = ESP_ERR_INVALID_ARG;
    } else if (strcmp(action, "play") == 0) {
        err = player_play(-1);
    } else if (strcmp(action, "pause") == 0) {
        err = player_pause(1);
    } else if (strcmp(action, "toggle") == 0) {
        player_status_t st;
        player_get_status(&st);
        err = st.state == PLAYER_STOPPED ? player_play(-1) : player_pause(-1);
    } else if (strcmp(action, "stop") == 0) {
        err = player_stop();
    } else if (strcmp(action, "next") == 0) {
        err = player_next();
    } else if (strcmp(action, "prev") == 0) {
        err = player_previous();
    } else if (strcmp(action, "volume") == 0) {
        player_set_volume((int)v);
    } else if (strcmp(action, "seek") == 0) {
        err = player_seek(-1, (uint32_t)(v * 1000));
    } else if (strcmp(action, "play_folder") == 0) {
        const char *folder = json_str(body, "folder");
        esp_err_t r = folder ? play_folder(req, folder) : send_error(req, "400 Bad Request", "dossier manquant");
        cJSON_Delete(body);
        return r;
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(body);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_error(req, "400 Bad Request", "déplacement impossible dans ce format");
    }
    return err == ESP_OK ? send_ok(req) : send_error(req, "400 Bad Request", "commande invalide");
}

/* ================= Cartes ================= */

static esp_err_t h_cards_get(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    card_entry_t *list;
    int n = cards_list(&list);
    controller_status_t cs;
    controller_get_status(&cs);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "cards");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "uid", list[i].uid);
        cJSON_AddStringToObject(e, "folder", list[i].folder);
        cJSON_AddBoolToObject(e, "exists", storage_is_dir(list[i].folder));
        if (list[i].resume_s >= 0) {
            cJSON_AddNumberToObject(e, "resume_s", list[i].resume_s);
        } else {
            cJSON_AddNullToObject(e, "resume_s");
        }
        if (list[i].resume_other >= 0) {
            cJSON_AddBoolToObject(e, "resume_other", list[i].resume_other == 1);
        } else {
            cJSON_AddNullToObject(e, "resume_other");
        }
        if (list[i].shuffle >= 0) {
            cJSON_AddBoolToObject(e, "shuffle", list[i].shuffle == 1);
        } else {
            cJSON_AddNullToObject(e, "shuffle");
        }
        if (list[i].normalize >= 0) {
            cJSON_AddNumberToObject(e, "normalize", list[i].normalize);
        } else {
            cJSON_AddNullToObject(e, "normalize");
        }
        if (list[i].compress >= 0) {
            cJSON_AddNumberToObject(e, "compress", list[i].compress);
        } else {
            cJSON_AddNullToObject(e, "compress");
        }
        cJSON_AddItemToArray(arr, e);
    }
    free(list);
    cJSON_AddBoolToObject(root, "learning", cs.learning);
    cJSON_AddNumberToObject(root, "learn_remaining", cs.learn_remaining_s);
    cJSON_AddStringToObject(root, "learned", cs.learned_uid);
    cJSON_AddStringToObject(root, "present", cs.present_uid);
    cJSON_AddStringToObject(root, "last_unknown", cs.last_unknown_uid);
    cJSON_AddBoolToObject(root, "reader_ok", nfc_reader_ok());
    return send_json(req, root);
}

static bool uid_valid(const char *uid)
{
    size_t n = strlen(uid);
    if (n < 8 || n >= UID_STR_MAX || n % 2) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((uid[i] >= '0' && uid[i] <= '9') || (uid[i] >= 'A' && uid[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

static esp_err_t h_cards_set(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *uid = json_str(body, "uid");
    const char *folder = json_str(body, "folder");
    /* Réglages propres à la carte : absent ou null = réglage général. */
    const cJSON *rs = cJSON_GetObjectItem(body, "resume_s");
    const cJSON *ro = cJSON_GetObjectItem(body, "resume_other");
    const cJSON *sh = cJSON_GetObjectItem(body, "shuffle");
    const cJSON *no = cJSON_GetObjectItem(body, "normalize");
    const cJSON *co = cJSON_GetObjectItem(body, "compress");
    card_entry_t e = {.resume_s = CARD_DEFAULT,
                      .resume_other = CARD_DEFAULT,
                      .shuffle = CARD_DEFAULT,
                      .normalize = CARD_DEFAULT,
                      .compress = CARD_DEFAULT};
    if (cJSON_IsNumber(rs) && rs->valuedouble >= 0 && rs->valuedouble <= 30 * 24 * 3600) {
        e.resume_s = (int32_t)rs->valuedouble;
    }
    if (cJSON_IsBool(ro)) {
        e.resume_other = cJSON_IsTrue(ro) ? 1 : 0;
    }
    if (cJSON_IsBool(sh)) {
        e.shuffle = cJSON_IsTrue(sh) ? 1 : 0;
    }
    if (cJSON_IsNumber(no) && no->valueint >= 0 && no->valueint <= SOUND_LEVEL_MAX) {
        e.normalize = (int8_t)no->valueint;
    }
    if (cJSON_IsNumber(co) && co->valueint >= 0 && co->valueint <= SOUND_LEVEL_MAX) {
        e.compress = (int8_t)co->valueint;
    }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *msg = "carte ou dossier invalide";
    if (uid && folder && uid_valid(uid) && path_sanitize(folder, e.folder, sizeof(e.folder)) && e.folder[0]) {
        if (!storage_is_dir(e.folder)) {
            msg = "dossier introuvable";
        } else {
            str_copy(e.uid, uid, sizeof(e.uid));
            err = cards_set(&e);
            msg = "enregistrement impossible";
        }
    }
    cJSON_Delete(body);
    return err == ESP_OK ? send_ok(req) : send_error(req, "400 Bad Request", msg);
}

static esp_err_t h_cards_delete(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *uid = json_str(body, "uid");
    esp_err_t err = uid ? cards_remove(uid) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    return err == ESP_OK ? send_ok(req) : send_error(req, "404 Not Found", "carte inconnue");
}

static esp_err_t h_cards_learn(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *action = json_str(body, "action");
    if (action && strcmp(action, "cancel") == 0) {
        controller_learn_cancel();
    } else {
        controller_learn_start();
    }
    cJSON_Delete(body);
    return send_ok(req);
}

/* ================= Fichiers ================= */

static esp_err_t h_files_list(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char raw[REL_PATH_MAX] = "", rel[REL_PATH_MAX];
    get_query(req, "path", raw, sizeof(raw));
    if (!path_sanitize(raw, rel, sizeof(rel))) {
        return send_error(req, "400 Bad Request", "chemin invalide");
    }
    if (!storage_is_mounted()) {
        return send_error(req, "503 Service Unavailable", "carte SD absente");
    }
    dir_entry_t *entries;
    int n;
    esp_err_t err = storage_list_dir(rel, &entries, &n);
    if (err != ESP_OK) {
        return send_error(req, "404 Not Found", "dossier introuvable");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", rel);
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", entries[i].name);
        cJSON_AddBoolToObject(e, "dir", entries[i].is_dir);
        cJSON_AddNumberToObject(e, "size", entries[i].size);
        cJSON_AddBoolToObject(e, "audio", !entries[i].is_dir && is_audio_file(entries[i].name));
        cJSON_AddItemToArray(arr, e);
    }
    storage_free_dir(entries, n);
    uint64_t total = 0, freeb = 0;
    storage_get_usage(&total, &freeb);
    cJSON_AddNumberToObject(root, "total", (double)total);
    cJSON_AddNumberToObject(root, "free", (double)freeb);
    return send_json(req, root);
}

static bool upload_allowed(const char *name)
{
    static const char *const extra[] = {".jpg", ".jpeg", ".png", ".txt", ".m3u"};
    if (audio_fmt_from_name(name) != AUDIO_FMT_NONE) {
        return true;
    }
    const char *ext = strrchr(name, '.');
    for (size_t i = 0; ext && i < sizeof(extra) / sizeof(extra[0]); i++) {
        if (strcasecmp(ext, extra[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Crée les dossiers parents d'un chemin relatif (envoi d'un dossier complet). */
static bool mkdir_parents(const char *rel)
{
    char buf[REL_PATH_MAX];
    str_copy(buf, rel, sizeof(buf));
    for (char *p = strchr(buf, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (!storage_is_dir(buf) && storage_mkdir(buf) != ESP_OK) {
            return false;
        }
        *p = '/';
    }
    return true;
}

static esp_err_t h_upload(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char raw[REL_PATH_MAX] = "", rel[REL_PATH_MAX], abs[ABS_PATH_MAX], part[ABS_PATH_MAX + 8];
    if (!get_query(req, "path", raw, sizeof(raw)) || !path_sanitize(raw, rel, sizeof(rel)) || !rel[0] ||
        !path_to_abs(rel, abs, sizeof(abs))) {
        return send_error(req, "400 Bad Request", "chemin invalide");
    }
    if (!upload_allowed(rel)) {
        return send_error(req, "400 Bad Request", "type de fichier refusé (audio ou image uniquement)");
    }
    if (!storage_is_mounted()) {
        return send_error(req, "503 Service Unavailable", "carte SD absente");
    }
    uint64_t total = 0, freeb = 0;
    if (storage_get_usage(&total, &freeb) && req->content_len + 1024 * 1024 > freeb) {
        return send_error(req, "507 Insufficient Storage", "carte SD pleine");
    }
    if (!mkdir_parents(rel)) {
        return send_error(req, "500 Internal Server Error", "création du dossier impossible");
    }
    snprintf(part, sizeof(part), "%s.part", abs);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        return send_error(req, "500 Internal Server Error", "écriture impossible");
    }
    transfer_begin();
    uint8_t *buf = heap_caps_malloc(UPLOAD_BUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        buf = malloc(UPLOAD_BUF);
    }
    size_t remaining = req->content_len;
    int timeouts = 0;
    bool ok = buf != NULL;
    while (ok && remaining > 0) {
        int n = httpd_req_recv(req, (char *)buf, remaining < UPLOAD_BUF ? remaining : UPLOAD_BUF);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;
        }
        if (n <= 0) {
            ok = false;
            break;
        }
        timeouts = 0;
        if (write(fd, buf, n) != n) {
            ok = false;
            break;
        }
        remaining -= n;
    }
    free(buf);
    if (close(fd) != 0) {
        ok = false;
    }
    transfer_end();
    if (ok) {
        unlink(abs); /* remplacement d'un fichier existant */
        ok = rename(part, abs) == 0;
    }
    if (!ok) {
        unlink(part);
        ESP_LOGW(TAG, "envoi de %s interrompu", rel);
        return send_error(req, "500 Internal Server Error", "envoi interrompu");
    }
    ESP_LOGI(TAG, "fichier reçu : %s (%u o)", rel, (unsigned)req->content_len);
    changes_notify(CHG_DATABASE);
    return send_ok(req);
}

static esp_err_t h_mkdir(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *path = json_str(body, "path");
    char rel[REL_PATH_MAX];
    esp_err_t err = (path && path_sanitize(path, rel, sizeof(rel)) && rel[0]) ? storage_mkdir(rel) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "ce dossier existe déjà");
    }
    return err == ESP_OK ? send_ok(req) : send_error(req, "400 Bad Request", "nom de dossier invalide");
}

static esp_err_t h_rename(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *from = json_str(body, "from");
    const char *to = json_str(body, "to");
    char rf[REL_PATH_MAX], rt[REL_PATH_MAX];
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *msg = "renommage ou déplacement impossible";
    if (from && to && path_sanitize(from, rf, sizeof(rf)) && path_sanitize(to, rt, sizeof(rt)) && rf[0] && rt[0]) {
        size_t fl = strlen(rf);
        char parent[REL_PATH_MAX];
        path_dirname(rt, parent, sizeof(parent));
        if (strncmp(rt, rf, fl) == 0 && rt[fl] == '/') {
            msg = "impossible de déplacer un dossier dans lui-même";
        } else if (!storage_is_dir(parent)) {
            msg = "dossier de destination introuvable";
        } else {
            bool was_dir = storage_is_dir(rf);
            err = storage_rename(rf, rt);
            if (err == ESP_OK) {
                if (was_dir) {
                    cards_on_folder_renamed(rf, rt);
                }
                controller_on_path_renamed(rf, rt);
            }
        }
    }
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "ce nom est déjà utilisé à cet endroit");
    }
    return err == ESP_OK ? send_ok(req) : send_error(req, "400 Bad Request", msg);
}

static esp_err_t h_delete(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *path = json_str(body, "path");
    char rel[REL_PATH_MAX];
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (path && path_sanitize(path, rel, sizeof(rel)) && rel[0]) {
        bool was_dir = storage_is_dir(rel);
        err = storage_remove_recursive(rel);
        if (was_dir && !storage_exists(rel)) {
            cards_remove_under(rel); /* leur fichier d'associations est parti avec le dossier */
        }
    }
    cJSON_Delete(body);
    return err == ESP_OK ? send_ok(req) : send_error(req, "400 Bad Request", "suppression impossible");
}

/* ================= Réglages ================= */

static esp_err_t h_settings_get(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    settings_t cfg;
    settings_get(&cfg);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", cfg.hostname);
    cJSON_AddStringToObject(root, "wifi_ssid", cfg.wifi_ssid);
    cJSON_AddStringToObject(root, "ota_url", cfg.ota_url);
    cJSON_AddStringToObject(root, "ota_default_url", CONFIG_ENC_OTA_DEFAULT_URL);
    cJSON_AddNumberToObject(root, "ota_interval_h", cfg.ota_interval_h);
    cJSON_AddNumberToObject(root, "max_volume", cfg.max_volume);
    cJSON_AddNumberToObject(root, "normalize", cfg.normalize);
    cJSON_AddNumberToObject(root, "compress", cfg.compress);
    cJSON_AddBoolToObject(root, "vol_touch", cfg.vol_touch);
    cJSON_AddNumberToObject(root, "touch_threshold_pct", cfg.touch_threshold / 10.0);
    cJSON_AddNumberToObject(root, "touch_hold_ms", cfg.touch_hold_ms);
    char a[16];
    cJSON *ip = cJSON_AddObjectToObject(root, "ip");
    cJSON_AddStringToObject(ip, "mode", cfg.ip.static_ip ? "static" : "dhcp");
    const uint32_t vals[] = {cfg.ip.address, cfg.ip.netmask, cfg.ip.gateway, cfg.ip.dns};
    const char *keys[] = {"address", "netmask", "gateway", "dns"};
    for (int i = 0; i < 4; i++) {
        a[0] = '\0';
        if (cfg.ip.static_ip && vals[i]) {
            ip4_format(vals[i], a);
        }
        cJSON_AddStringToObject(ip, keys[i], a);
    }
    wifi_status_t ws;
    wifi_mgr_get_status(&ws);
    cJSON *cur = cJSON_AddObjectToObject(root, "ip_current"); /* pour pré-remplir une adresse fixe */
    cJSON_AddStringToObject(cur, "address", ws.sta_ip);
    cJSON_AddStringToObject(cur, "netmask", ws.sta_netmask);
    cJSON_AddStringToObject(cur, "gateway", ws.sta_gateway);
    cJSON_AddStringToObject(cur, "dns", ws.sta_dns);
    cJSON_AddNumberToObject(root, "ip_test_s", IP_TEST_S);
    cJSON_AddNumberToObject(root, "resume_s", cfg.resume_timeout_s);
    cJSON_AddBoolToObject(root, "resume_after_other", cfg.resume_after_other);
    cJSON_AddBoolToObject(root, "shuffle", cfg.shuffle);
    cJSON_AddBoolToObject(root, "https_enabled", cfg.https_enabled);
    cJSON_AddBoolToObject(root, "https_active", s_https != NULL);
    cJSON_AddBoolToObject(root, "https_pending", s_https_busy);
    cJSON_AddBoolToObject(root, "mpd_password_set", cfg.mpd_pass_set);
    cJSON_AddNumberToObject(root, "mpd_port", CONFIG_ENC_MPD_PORT);
    return send_json(req, root);
}

static esp_err_t h_settings_set(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return send_error(req, "400 Bad Request", "requête invalide");
    }
    settings_t cfg;
    settings_get(&cfg);
    const char *host = json_str(body, "hostname");
    if (host && strcmp(host, cfg.hostname) != 0) {
        if (settings_set_hostname(host) != ESP_OK) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "nom invalide (lettres, chiffres et tirets, 32 max)");
        }
        settings_get(&cfg);
        wifi_mgr_set_hostname(cfg.hostname);
    }
    const char *url = json_str(body, "ota_url");
    const cJSON *interval = cJSON_GetObjectItem(body, "ota_interval_h");
    if (url || cJSON_IsNumber(interval)) {
        uint16_t ih = cJSON_IsNumber(interval) ? (uint16_t)interval->valueint : cfg.ota_interval_h;
        if (settings_set_ota(url ? url : cfg.ota_url, ih) != ESP_OK) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "adresse de mise à jour invalide (http:// ou https://)");
        }
        if (url && url[0] && strcmp(url, cfg.ota_url) != 0) {
            ota_check_now();
        }
    }
    const cJSON *rs = cJSON_GetObjectItem(body, "resume_s");
    const cJSON *ro = cJSON_GetObjectItem(body, "resume_after_other");
    if (cJSON_IsNumber(rs) || cJSON_IsBool(ro)) {
        uint32_t timeout = cJSON_IsNumber(rs) && rs->valuedouble >= 0 ? (uint32_t)rs->valuedouble : cfg.resume_timeout_s;
        bool other = cJSON_IsBool(ro) ? cJSON_IsTrue(ro) : cfg.resume_after_other;
        if ((cJSON_IsNumber(rs) && rs->valuedouble < 0) || settings_set_resume(timeout, other) != ESP_OK) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "délai de reprise invalide (30 jours maximum)");
        }
    }
    const cJSON *shuffle = cJSON_GetObjectItem(body, "shuffle");
    if (cJSON_IsBool(shuffle) && settings_set_shuffle(cJSON_IsTrue(shuffle)) != ESP_OK) {
        cJSON_Delete(body);
        return send_error(req, "500 Internal Server Error", "enregistrement impossible");
    }
    const cJSON *norm = cJSON_GetObjectItem(body, "normalize");
    const cJSON *comp = cJSON_GetObjectItem(body, "compress");
    if (cJSON_IsNumber(norm) || cJSON_IsNumber(comp)) {
        int n = cJSON_IsNumber(norm) ? norm->valueint : cfg.normalize;
        int c = cJSON_IsNumber(comp) ? comp->valueint : cfg.compress;
        if (n < 0 || n > SOUND_LEVEL_MAX || c < 0 || c > SOUND_LEVEL_MAX ||
            settings_set_sound((uint8_t)n, (uint8_t)c) != ESP_OK) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "réglage du son invalide");
        }
    }
    const cJSON *vt = cJSON_GetObjectItem(body, "vol_touch");
    const cJSON *tt = cJSON_GetObjectItem(body, "touch_threshold_pct");
    const cJSON *th = cJSON_GetObjectItem(body, "touch_hold_ms");
    if (cJSON_IsBool(vt) || cJSON_IsNumber(tt) || cJSON_IsNumber(th)) {
        bool touch = cJSON_IsBool(vt) ? cJSON_IsTrue(vt) : cfg.vol_touch;
        double thr = cJSON_IsNumber(tt) ? tt->valuedouble * 10.0 + 0.5 : cfg.touch_threshold;
        double hold = cJSON_IsNumber(th) ? th->valuedouble : cfg.touch_hold_ms;
        if (thr < TOUCH_THRESHOLD_MIN || thr > TOUCH_THRESHOLD_MAX + 0.5 || hold < 0 || hold > TOUCH_HOLD_MAX_MS ||
            settings_set_controls(touch, (uint16_t)thr, (uint16_t)hold) != ESP_OK) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "réglage des touches invalide (seuil 0,3 à 30 %, maintien 0 à 3 s)");
        }
    }
    const cJSON *maxv = cJSON_GetObjectItem(body, "max_volume");
    if (cJSON_IsNumber(maxv)) {
        if (settings_set_max_volume((uint8_t)maxv->valueint) != ESP_OK) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "volume maximum invalide");
        }
        player_set_max_volume((uint8_t)maxv->valueint);
    }
    cJSON_Delete(body);
    return send_ok(req);
}

static esp_err_t h_password(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *cur = json_str(body, "current");
    const char *nw = json_str(body, "new");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *msg = "mot de passe actuel incorrect";
    if (cur && nw && settings_check_admin_password(cur)) {
        err = settings_set_admin_password(nw);
        msg = "nouveau mot de passe trop court (6 caractères minimum)";
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", msg);
    }
    clear_sessions(); /* déconnecte les autres appareils */
    create_session(req);
    return send_ok(req);
}

static esp_err_t h_mpd(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *pw = json_str(body, "password");
    esp_err_t err = pw ? settings_set_mpd_password(pw) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    return err == ESP_OK ? send_ok(req) : send_error(req, "400 Bad Request", "mot de passe MPD invalide");
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    settings_t cfg;
    settings_get(&cfg);
    /* Autorisé sans connexion uniquement pendant l'assistant de première configuration. */
    if (cfg.admin_set && !require_auth(req)) {
        return ESP_OK;
    }
    wifi_ap_record_t *recs = calloc(24, sizeof(wifi_ap_record_t));
    if (!recs) {
        return send_error(req, "500 Internal Server Error", "mémoire insuffisante");
    }
    int n = wifi_mgr_scan(recs, 24);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        bool dup = !ssid[0];
        for (int j = 0; j < i && !dup; j++) {
            dup = strcmp(ssid, (const char *)recs[j].ssid) == 0;
        }
        if (dup) {
            continue; /* les résultats sont triés par signal : on garde le meilleur */
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", ssid);
        cJSON_AddNumberToObject(e, "rssi", recs[i].rssi);
        cJSON_AddBoolToObject(e, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, e);
    }
    free(recs);
    return send_json(req, root);
}

static esp_err_t h_wifi_set(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *ssid = json_str(body, "ssid");
    const char *pw = json_str(body, "password");
    esp_err_t err = ssid ? settings_set_wifi(ssid, pw ? pw : "") : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "réseau invalide (mot de passe de 8 caractères minimum)");
    }
    wifi_mgr_reconnect_later(1500);
    return send_ok(req);
}

/* Mesures des touches tactiles en direct, pour régler le seuil à travers le bois. */
static esp_err_t h_touch(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    buttons_diag_t d;
    buttons_get_diag(&d);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "touch", d.touch);
    cJSON_AddBoolToObject(root, "ok", d.touch_ok);
    cJSON_AddNumberToObject(root, "threshold_pct", d.threshold / 10.0);
    cJSON *arr = cJSON_AddArrayToObject(root, "keys");
    for (int i = 0; i < 2; i++) {
        cJSON *k = cJSON_CreateObject();
        cJSON_AddStringToObject(k, "name", i == 0 ? "+" : "-");
        cJSON_AddNumberToObject(k, "delta_pct", d.key[i].delta_permille / 10.0);
        cJSON_AddNumberToObject(k, "value", d.key[i].value);
        cJSON_AddNumberToObject(k, "baseline", d.key[i].baseline);
        cJSON_AddBoolToObject(k, "touched", d.key[i].touched);
        cJSON_AddItemToArray(arr, k);
    }
    return send_json(req, root);
}

static esp_err_t h_wifi_switch(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    wifi_mgr_switch_now();
    return send_ok(req);
}

static bool json_ip(const cJSON *body, const char *key, uint32_t *out, bool optional)
{
    const char *v = json_str(body, key);
    if (!v || !v[0]) {
        *out = 0;
        return optional;
    }
    return ip4_parse(v, out);
}

/* Nouvelle configuration IP : essayée, puis enregistrée à la première connexion administrateur
 * à la nouvelle adresse ; sinon retour à l'ancienne au bout de 5 minutes. */
static esp_err_t h_network(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *mode = json_str(body, "mode");
    ip_config_t ip = {0};
    const char *why = "configuration invalide";
    bool ok = false;
    if (mode && strcmp(mode, "dhcp") == 0) {
        ok = true;
    } else if (mode && strcmp(mode, "static") == 0) {
        ip.static_ip = true;
        if (!json_ip(body, "address", &ip.address, false)) {
            why = "adresse IP mal écrite (exemple : 192.168.1.50)";
        } else if (!json_ip(body, "netmask", &ip.netmask, false)) {
            why = "masque de sous-réseau mal écrit (exemple : 255.255.255.0)";
        } else if (!json_ip(body, "gateway", &ip.gateway, false)) {
            why = "passerelle mal écrite (exemple : 192.168.1.1)";
        } else if (!json_ip(body, "dns", &ip.dns, true)) {
            why = "serveur DNS mal écrit";
        } else {
            ok = settings_ip_valid(&ip, &why);
        }
    }
    cJSON_Delete(body);
    if (!ok) {
        return send_error(req, "400 Bad Request", why ? why : "configuration invalide");
    }
    wifi_mgr_ip_test(&ip);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "test_s", IP_TEST_S);
    return send_json(req, root);
}

/* ================= Export / import ================= */

static esp_err_t h_config_export(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char v[4] = "";
    bool secrets = get_query(req, "secrets", v, sizeof(v)) && strcmp(v, "1") == 0;
    cJSON *doc = backup_export(secrets);
    char *txt = doc ? cJSON_Print(doc) : NULL;
    cJSON_Delete(doc);
    if (!txt) {
        return send_error(req, "500 Internal Server Error", "mémoire insuffisante");
    }
    settings_t cfg;
    settings_get(&cfg);
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"reglages-%s.json\"", cfg.hostname);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);
    return err;
}

static esp_err_t h_config_import(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *doc = read_json_max(req, IMPORT_MAX);
    if (!doc) {
        return send_error(req, "400 Bad Request", "fichier illisible (JSON attendu, 160 Ko maximum)");
    }
    char msg[200];
    bool ip_test = false;
    esp_err_t err = backup_import(doc, msg, sizeof(msg), &ip_test);
    cJSON_Delete(doc);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", msg);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", msg);
    cJSON_AddBoolToObject(root, "ip_test", ip_test);
    return send_json(req, root);
}

/* ================= Système ================= */

static esp_err_t h_ota_check(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    ota_check_now();
    return send_ok(req);
}

static esp_err_t h_ota_upload(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    esp_err_t err = ota_upload_begin(req->content_len);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "taille de firmware invalide");
    }
    transfer_begin();
    char *buf = malloc(4096);
    size_t remaining = req->content_len;
    int timeouts = 0;
    while (buf && err == ESP_OK && remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        err = ota_upload_write((const uint8_t *)buf, n);
        remaining -= n;
    }
    free(buf);
    transfer_end();
    char msg[96];
    if (err == ESP_ERR_INVALID_VERSION) {
        ota_upload_end(false, msg, sizeof(msg));
        return send_error(req, "400 Bad Request", "ce fichier n'est pas un firmware d'enceinte");
    }
    err = ota_upload_end(err == ESP_OK && remaining == 0, msg, sizeof(msg));
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", msg);
    }
    return send_ok(req);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    send_ok(req);
    ota_schedule_restart(1000);
    return ESP_OK;
}

static esp_err_t h_factory_reset(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const char *pw = json_str(body, "password");
    bool ok = pw && settings_check_admin_password(pw);
    cJSON_Delete(body);
    if (!ok) {
        return send_error(req, "400 Bad Request", "mot de passe incorrect");
    }
    send_ok(req);
    backup_factory_reset(); /* sinon la carte SD rétablirait réglages et mot de passe */
    settings_factory_reset();
    ota_schedule_restart(1000);
    return ESP_OK;
}

/* ================= Démarrage ================= */

static esp_err_t h_https(httpd_req_t *req);

static const httpd_uri_t s_uris[] = {
    {"/", HTTP_GET, h_index, NULL},
    {"/index.html", HTTP_GET, h_index, NULL},
    {"/app.js", HTTP_GET, h_app_js, NULL},
    {"/style.css", HTTP_GET, h_style, NULL},
    {"/api/state", HTTP_GET, h_state, NULL},
    {"/api/setup", HTTP_POST, h_setup, NULL},
    {"/api/login", HTTP_POST, h_login, NULL},
    {"/api/logout", HTTP_POST, h_logout, NULL},
    {"/api/status", HTTP_GET, h_status, NULL},
    {"/api/player", HTTP_POST, h_player, NULL},
    {"/api/cards", HTTP_GET, h_cards_get, NULL},
    {"/api/cards", HTTP_POST, h_cards_set, NULL},
    {"/api/cards/delete", HTTP_POST, h_cards_delete, NULL},
    {"/api/cards/learn", HTTP_POST, h_cards_learn, NULL},
    {"/api/files", HTTP_GET, h_files_list, NULL},
    {"/api/upload", HTTP_PUT, h_upload, NULL},
    {"/api/files/mkdir", HTTP_POST, h_mkdir, NULL},
    {"/api/files/rename", HTTP_POST, h_rename, NULL},
    {"/api/files/delete", HTTP_POST, h_delete, NULL},
    {"/api/settings", HTTP_GET, h_settings_get, NULL},
    {"/api/settings", HTTP_POST, h_settings_set, NULL},
    {"/api/password", HTTP_POST, h_password, NULL},
    {"/api/mpd", HTTP_POST, h_mpd, NULL},
    {"/api/https", HTTP_POST, h_https, NULL},
    {"/api/wifi/scan", HTTP_GET, h_wifi_scan, NULL},
    {"/api/wifi", HTTP_POST, h_wifi_set, NULL},
    {"/api/wifi/switch", HTTP_POST, h_wifi_switch, NULL},
    {"/api/touch", HTTP_GET, h_touch, NULL},
    {"/api/network", HTTP_POST, h_network, NULL},
    {"/api/config/export", HTTP_GET, h_config_export, NULL},
    {"/api/config/import", HTTP_POST, h_config_import, NULL},
    {"/api/ota/check", HTTP_POST, h_ota_check, NULL},
    {"/api/ota/upload", HTTP_PUT, h_ota_upload, NULL},
    {"/api/reboot", HTTP_POST, h_reboot, NULL},
    {"/api/factory-reset", HTTP_POST, h_factory_reset, NULL},
};

static void register_handlers(httpd_handle_t srv, bool secure)
{
    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        httpd_uri_t u = s_uris[i];
        u.user_ctx = (void *)(secure ? &s_mark_https : &s_mark_http);
        httpd_register_uri_handler(srv, &u);
    }
}

static void base_config(httpd_config_t *cfg)
{
    cfg->max_uri_handlers = 40;
    cfg->lru_purge_enable = true;
    cfg->recv_wait_timeout = 10;
    cfg->send_wait_timeout = 10;
    cfg->uri_match_fn = httpd_uri_match_wildcard;
}

/* Démarre ou arrête le serveur HTTPS selon le réglage. Tâche dédiée : la génération du
 * certificat demande de la pile, et un serveur ne peut pas s'arrêter depuis sa propre requête. */
static void https_apply_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500)); /* laisse partir la réponse HTTP en cours */
    settings_t cfg;
    settings_get(&cfg);
    if (!cfg.https_enabled && s_https) {
        httpd_ssl_stop(s_https);
        s_https = NULL;
        ESP_LOGI(TAG, "HTTPS arrêté");
    } else if (cfg.https_enabled && !s_https) {
        httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
        size_t cert_len, key_len;
        const char *cert, *key;
        if (tls_cert_get(cfg.hostname, &cert, &cert_len, &key, &key_len) == ESP_OK) {
            ssl.servercert = (const uint8_t *)cert;
            ssl.servercert_len = cert_len;
            ssl.prvtkey_pem = (const uint8_t *)key;
            ssl.prvtkey_len = key_len;
            base_config(&ssl.httpd);
            ssl.httpd.stack_size = 12288;
            ssl.httpd.max_open_sockets = 4;
            httpd_handle_t srv = NULL;
            esp_err_t err = httpd_ssl_start(&srv, &ssl);
            if (err == ESP_OK) {
                register_handlers(srv, true);
                s_https = srv;
                ESP_LOGI(TAG, "HTTPS actif : https://%s.local", cfg.hostname);
            } else {
                ESP_LOGE(TAG, "démarrage HTTPS impossible : %s", esp_err_to_name(err));
            }
        }
    }
    s_https_busy = false;
    vTaskDelete(NULL);
}

void web_server_https_apply(void)
{
    s_https_busy = true;
    if (xTaskCreate(https_apply_task, "https_apply", 8192, NULL, 4, NULL) != pdPASS) {
        s_https_busy = false;
    }
}

static esp_err_t h_https(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    const cJSON *en = cJSON_GetObjectItem(body, "enabled");
    esp_err_t err = cJSON_IsBool(en) ? settings_set_https(cJSON_IsTrue(en)) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "réglage invalide");
    }
    web_server_https_apply();
    return send_ok(req);
}

esp_err_t web_server_start(void)
{
    prepare_static();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    base_config(&cfg);
    cfg.stack_size = 10240;
    cfg.max_open_sockets = 7;
    esp_err_t err = httpd_start(&s_http, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "démarrage impossible : %s", esp_err_to_name(err));
        return err;
    }
    register_handlers(s_http, false);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, h_not_found);
    settings_t st;
    settings_get(&st);
    if (st.https_enabled) {
        web_server_https_apply();
    }
    ESP_LOGI(TAG, "interface web prête");
    return ESP_OK;
}
