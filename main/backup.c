#include "backup.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cards.h"
#include "config_json.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "player.h"
#include "settings.h"
#include "storage.h"
#include "util.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "backup";

#define SETTINGS_FILE ".enceinte.json"
#define CARDS_FILE ".cartes.json"
#define FILE_MAX (64 * 1024)
#define MAX_DIRS 3000
#define MAX_DEPTH 7
#define NS "backup"

static SemaphoreHandle_t s_lock;     /* écritures et chargements de la carte SD */
static TaskHandle_t s_loader;        /* tâche en train de charger la carte SD : pas de recopie */
static char s_device[13];            /* adresse MAC : identifie l'enceinte qui a écrit le fichier */
static uint32_t s_settings_mount;    /* montage dont les réglages ont été traités */
static bool s_managed;               /* /.enceinte.json présent sur la carte au montage */
static bool s_foreign;               /* écrit par une autre enceinte (carte SD clonée) */
static volatile bool s_migrating;    /* première copie : /.enceinte.json écrit après les associations */
static volatile bool s_frozen;       /* réinitialisation usine en cours : plus aucune écriture */

#define LOCK() xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* ---- Modifications en attente (carte SD absente au moment du changement) ---- */

static bool flag_get(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open_from_partition(CFG_PARTITION, NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void flag_set(const char *key, bool on)
{
    if (flag_get(key) == on) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open_from_partition(CFG_PARTITION, NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- Fichiers ---- */

static bool file_path(const char *dir, const char *name, char *out, size_t len)
{
    char rel[REL_PATH_MAX + 16];
    int n = dir[0] ? snprintf(rel, sizeof(rel), "%s/%s", dir, name) : snprintf(rel, sizeof(rel), "%s", name);
    return n > 0 && (size_t)n < sizeof(rel) && path_to_abs(rel, out, len);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *buf = NULL;
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && st.st_size > 0 && st.st_size <= FILE_MAX) {
        buf = malloc(st.st_size + 1);
        if (buf && fread(buf, 1, st.st_size, f) == (size_t)st.st_size) {
            buf[st.st_size] = '\0';
        } else {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

static esp_err_t write_json(const char *path, cJSON *doc)
{
    char *txt = doc ? cJSON_Print(doc) : NULL;
    cJSON_Delete(doc);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    FILE *f = fopen(path, "w");
    esp_err_t err = ESP_FAIL;
    if (f) {
        size_t len = strlen(txt);
        bool ok = fwrite(txt, 1, len, f) == len && fputc('\n', f) != EOF;
        ok = fflush(f) == 0 && ok;
        ok = fsync(fileno(f)) == 0 && ok;
        ok = fclose(f) == 0 && ok;
        err = ok ? ESP_OK : ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "écriture impossible : %s (errno %d)", path, errno);
    }
    free(txt);
    return err;
}

static void current_config(config_t *c)
{
    memset(c, 0, sizeof(*c));
    settings_get(&c->s);
    c->wifi = true;
    c->admin_hash = settings_get_password_hash(true, c->admin);
    c->mpd_hash = settings_get_password_hash(false, c->mpd) ? 1 : 0;
}

static esp_err_t write_settings_file(void)
{
    char path[ABS_PATH_MAX + 16];
    if (!storage_is_mounted() || !file_path("", SETTINGS_FILE, path, sizeof(path))) {
        return ESP_ERR_INVALID_STATE;
    }
    config_t c;
    current_config(&c);
    cJSON *doc = config_document(CONFIG_FORMAT, s_device);
    if (doc) {
        cJSON_AddItemToObject(doc, "settings", config_to_json(&c, true));
    }
    return write_json(path, doc);
}

/* Réécrit le fichier des associations d'un dossier (supprimé s'il n'en reste aucune). */
static esp_err_t write_folder_file(const char *folder)
{
    char path[ABS_PATH_MAX + 16];
    if (!storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!folder[0] || !storage_is_dir(folder) || !file_path(folder, CARDS_FILE, path, sizeof(path))) {
        return ESP_OK; /* dossier absent : rien à écrire */
    }
    card_entry_t *list;
    int n = cards_list_folder(folder, &list);
    if (n == 0) {
        free(list);
        return unlink(path) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    cJSON *doc = config_document(CARDS_FORMAT, NULL);
    cJSON *arr = doc ? cJSON_AddArrayToObject(doc, "cards") : NULL;
    for (int i = 0; arr && i < n; i++) {
        cJSON_AddItemToArray(arr, card_to_json(&list[i], false));
    }
    free(list);
    return write_json(path, doc);
}

/* ---- Observateurs : chaque modification est recopiée sur la carte SD ---- */

static bool loading(void)
{
    return s_loader && xTaskGetCurrentTaskHandle() == s_loader;
}

static void on_settings_changed(void)
{
    /* Pendant la première copie, /.enceinte.json sera écrit à la fin, avec les réglages du moment. */
    if (loading() || s_frozen || s_migrating) {
        return;
    }
    LOCK();
    esp_err_t err = write_settings_file();
    UNLOCK();
    if (err != ESP_OK) {
        flag_set("set_dirty", true); /* recopié quand la carte SD sera de retour */
    }
}

static void on_cards_changed(const char *folder)
{
    if (loading() || s_frozen) {
        return;
    }
    LOCK();
    esp_err_t err = write_folder_file(folder);
    UNLOCK();
    if (err != ESP_OK) {
        flag_set("cards_dirty", true);
    }
}

/* ---- Application d'une configuration (carte SD, import) ---- */

static bool ip_equal(const ip_config_t *a, const ip_config_t *b)
{
    return a->static_ip == b->static_ip &&
           (!a->static_ip || (a->address == b->address && a->netmask == b->netmask && a->gateway == b->gateway &&
                              a->dns == b->dns));
}

/* Mêmes réglages de configuration (le volume courant n'en fait pas partie). */
static bool settings_same(const settings_t *a, const settings_t *b)
{
    return strcmp(a->hostname, b->hostname) == 0 && strcmp(a->wifi_ssid, b->wifi_ssid) == 0 &&
           strcmp(a->wifi_pass, b->wifi_pass) == 0 && strcmp(a->ota_url, b->ota_url) == 0 &&
           a->ota_interval_h == b->ota_interval_h && a->resume_timeout_s == b->resume_timeout_s &&
           a->resume_after_other == b->resume_after_other && a->shuffle == b->shuffle &&
           a->https_enabled == b->https_enabled && a->normalize == b->normalize && a->compress == b->compress &&
           a->max_volume == b->max_volume && ip_equal(&a->ip, &b->ip);
}

/*
 * live : l'enceinte fonctionne déjà (sinon démarrage : les modules liront les réglages).
 * ip_test : une nouvelle adresse IP n'est enregistrée qu'après confirmation (import).
 */
static esp_err_t apply_config(const config_t *c, bool live, bool ip_test, bool *ip_test_started)
{
    settings_t before;
    settings_get(&before);
    settings_t want = c->s;
    bool test = ip_test && live && !ip_equal(&want.ip, &before.ip);
    if (test) {
        want.ip = before.ip;
    }
    esp_err_t err = settings_same(&want, &before) ? ESP_OK : settings_set_all(&want);
    if (err != ESP_OK) {
        return err;
    }
    if (c->admin_hash) {
        settings_set_password_hash(true, c->admin);
    }
    if (c->mpd_hash >= 0) {
        settings_set_password_hash(false, c->mpd_hash ? c->mpd : NULL);
    }
    if (live) {
        if (strcmp(before.hostname, want.hostname) != 0) {
            wifi_mgr_set_hostname(want.hostname);
        }
        if (strcmp(before.wifi_ssid, want.wifi_ssid) != 0 || strcmp(before.wifi_pass, want.wifi_pass) != 0 ||
            !ip_equal(&before.ip, &want.ip)) {
            wifi_mgr_reconnect_later(1500);
        }
        if (before.max_volume != want.max_volume) {
            player_set_max_volume(want.max_volume);
        }
        if (before.https_enabled != want.https_enabled) {
            web_server_https_apply();
        }
        if (test) {
            wifi_mgr_ip_test(&c->s.ip);
        }
    }
    if (ip_test_started) {
        *ip_test_started = test;
    }
    return ESP_OK;
}

/* ---- Chargement depuis la carte SD ---- */

static void load_settings(bool live)
{
    char path[ABS_PATH_MAX + 16];
    s_managed = s_foreign = false;
    if (!file_path("", SETTINGS_FILE, path, sizeof(path))) {
        return;
    }
    char *txt = read_file(path);
    bool had_file = txt != NULL;
    cJSON *doc = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    char err[128] = "";
    const cJSON *dev = cJSON_GetObjectItem(doc, "device");
    s_managed = doc && config_document_check(doc, CONFIG_FORMAT, err, sizeof(err));
    s_foreign = s_managed && (!cJSON_IsString(dev) || strcmp(dev->valuestring, s_device) != 0);
    bool rewrite = false;
    if (!s_managed) {
        /* Une coupure entre les deux ne doit pas laisser une carte SD « de référence » sans ses
         * associations : /.enceinte.json est écrit après elles (load_cards). */
        ESP_LOGI(TAG, "pas de réglages %ssur la carte SD : copie de ceux de l'enceinte", had_file ? "lisibles " : "");
        s_migrating = true;
    } else if (!s_foreign && flag_get("set_dirty")) {
        ESP_LOGI(TAG, "réglages modifiés pendant l'absence de la carte SD : recopiés sur la carte");
        rewrite = true;
    } else {
        config_t c;
        current_config(&c);
        c.admin_hash = false;
        c.mpd_hash = -1;
        if (config_from_json(cJSON_GetObjectItem(doc, "settings"), &c, err, sizeof(err)) &&
            apply_config(&c, live, false, NULL) == ESP_OK) {
            ESP_LOGI(TAG, "réglages chargés depuis la carte SD%s", s_foreign ? " (venant d'une autre enceinte)" : "");
            rewrite = s_foreign; /* le fichier porte désormais l'identité de cette enceinte */
        } else {
            ESP_LOGW(TAG, "réglages de la carte SD ignorés : %s", err[0] ? err : "invalides");
            rewrite = true;
        }
    }
    cJSON_Delete(doc);
    if (rewrite && write_settings_file() == ESP_OK) {
        flag_set("set_dirty", false);
    }
}

typedef struct {
    card_entry_t *items;
    int count, cap;
    char **bad;      /* dossiers dont le fichier est illisible */
    int nbad;
    char **folders;  /* dossiers ayant un fichier d'associations */
    int nfolders;
    int dirs;
} scan_t;

static void str_list_add(char ***list, int *n, const char *s)
{
    char **nl = realloc(*list, (*n + 1) * sizeof(char *));
    if (nl) {
        *list = nl;
        nl[*n] = strdup(s);
        if (nl[*n]) {
            (*n)++;
        }
    }
}

static void str_list_free(char **list, int n)
{
    for (int i = 0; i < n; i++) {
        free(list[i]);
    }
    free(list);
}

static bool str_list_has(char **list, int n, const char *s)
{
    for (int i = 0; i < n; i++) {
        if (strcasecmp(list[i], s) == 0) {
            return true;
        }
    }
    return false;
}

static void scan_folder_file(scan_t *sc, const char *dir)
{
    char path[ABS_PATH_MAX + 16];
    struct stat st;
    if (!file_path(dir, CARDS_FILE, path, sizeof(path)) || stat(path, &st) != 0) {
        return;
    }
    str_list_add(&sc->folders, &sc->nfolders, dir);
    char *txt = read_file(path);
    cJSON *doc = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    char err[96];
    const cJSON *arr = cJSON_GetObjectItem(doc, "cards");
    if (!doc || !config_document_check(doc, CARDS_FORMAT, err, sizeof(err)) || !cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "%s/%s illisible", dir, CARDS_FILE);
        str_list_add(&sc->bad, &sc->nbad, dir);
        cJSON_Delete(doc);
        return;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        card_entry_t e;
        if (!card_from_json(it, &e, false, err, sizeof(err))) {
            ESP_LOGW(TAG, "%s/%s : %s", dir, CARDS_FILE, err);
            continue;
        }
        str_copy(e.folder, dir, sizeof(e.folder));
        bool dup = false;
        for (int i = 0; i < sc->count && !dup; i++) {
            dup = strcmp(sc->items[i].uid, e.uid) == 0;
        }
        if (dup) {
            ESP_LOGW(TAG, "carte %s associée à plusieurs dossiers : \"%s\" ignoré", e.uid, dir);
            continue;
        }
        if (sc->count == sc->cap) {
            int cap = sc->cap ? sc->cap * 2 : 32;
            card_entry_t *n = realloc(sc->items, cap * sizeof(card_entry_t));
            if (!n) {
                break;
            }
            sc->items = n;
            sc->cap = cap;
        }
        if (sc->count < CARDS_MAX) {
            sc->items[sc->count++] = e;
        }
    }
    cJSON_Delete(doc);
}

static void scan_dir(scan_t *sc, const char *dir, int depth)
{
    if (sc->dirs >= MAX_DIRS) {
        return;
    }
    sc->dirs++;
    if (dir[0]) {
        scan_folder_file(sc, dir);
    }
    if (depth >= MAX_DEPTH) {
        return;
    }
    dir_entry_t *entries;
    int n;
    if (storage_list_dir(dir, &entries, &n) != ESP_OK) {
        return;
    }
    for (int i = 0; i < n; i++) {
        if (!entries[i].is_dir) {
            continue;
        }
        char *child = malloc(REL_PATH_MAX);
        if (!child) {
            break;
        }
        int len = dir[0] ? snprintf(child, REL_PATH_MAX, "%s/%s", dir, entries[i].name)
                         : snprintf(child, REL_PATH_MAX, "%s", entries[i].name);
        if (len > 0 && len < REL_PATH_MAX) {
            scan_dir(sc, child, depth + 1);
        }
        free(child);
    }
    storage_free_dir(entries, n);
}

static void load_cards(void)
{
    card_entry_t *nvs;
    int nn = cards_list(&nvs);
    if (!s_managed) {
        /* Première fois sur cette carte SD : on y recopie les associations de l'enceinte, puis
         * les réglages (leur fichier marque la carte comme « de référence »). */
        for (int i = 0; i < nn; i++) {
            write_folder_file(nvs[i].folder);
        }
        free(nvs);
        flag_set("cards_dirty", false);
        s_migrating = false;
        if (write_settings_file() == ESP_OK) {
            flag_set("set_dirty", false);
            s_managed = true;
        }
        return;
    }
    scan_t sc = {0};
    scan_dir(&sc, "", 0);
    if (!s_foreign && flag_get("cards_dirty")) {
        /* Associations modifiées pendant l'absence de la carte SD : l'enceinte fait foi. */
        ESP_LOGI(TAG, "associations modifiées pendant l'absence de la carte SD : recopiées sur la carte");
        for (int i = 0; i < sc.nfolders; i++) {
            write_folder_file(sc.folders[i]);
        }
        for (int i = 0; i < nn; i++) {
            if (!str_list_has(sc.folders, sc.nfolders, nvs[i].folder)) {
                write_folder_file(nvs[i].folder);
            }
        }
        flag_set("cards_dirty", false);
    } else {
        /* La carte SD fait foi ; un fichier illisible garde les associations de l'enceinte. */
        for (int i = 0; i < nn && sc.count < CARDS_MAX; i++) {
            if (str_list_has(sc.bad, sc.nbad, nvs[i].folder)) {
                bool dup = false;
                for (int k = 0; k < sc.count && !dup; k++) {
                    dup = strcmp(sc.items[k].uid, nvs[i].uid) == 0;
                }
                if (!dup) {
                    if (sc.count == sc.cap) {
                        card_entry_t *n = realloc(sc.items, (sc.cap + 8) * sizeof(card_entry_t));
                        if (!n) {
                            break;
                        }
                        sc.items = n;
                        sc.cap += 8;
                    }
                    sc.items[sc.count++] = nvs[i];
                }
            }
        }
        if (cards_replace_all(sc.items, sc.count, false) == ESP_OK) {
            ESP_LOGI(TAG, "%d carte(s) associée(s) chargée(s) depuis la carte SD (%d dossiers parcourus)", sc.count,
                     sc.dirs);
        }
        for (int i = 0; i < sc.nbad; i++) {
            write_folder_file(sc.bad[i]);
        }
        flag_set("cards_dirty", false);
    }
    free(nvs);
    free(sc.items);
    str_list_free(sc.bad, sc.nbad);
    str_list_free(sc.folders, sc.nfolders);
}

static void load_from_sd(bool live, bool with_settings)
{
    LOCK();
    s_loader = xTaskGetCurrentTaskHandle();
    uint32_t mount = storage_mount_count();
    if (with_settings) {
        load_settings(live);
        s_settings_mount = mount;
    }
    load_cards();
    s_loader = NULL;
    UNLOCK();
}

static void backup_task(void *arg)
{
    uint32_t done = 0;
    for (;;) {
        uint32_t mount = storage_mount_count();
        if (storage_is_mounted() && mount != done) {
            done = mount;
            /* Au démarrage, les réglages ont déjà été lus (backup_boot) : seulement les cartes. */
            load_from_sd(true, mount != s_settings_mount);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void backup_boot(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    bytes_to_hex(mac, sizeof(mac), s_device);
    settings_set_observer(on_settings_changed);
    cards_set_observer(on_cards_changed);
    if (storage_is_mounted()) {
        LOCK();
        s_loader = xTaskGetCurrentTaskHandle();
        load_settings(false);
        s_settings_mount = storage_mount_count();
        s_loader = NULL;
        UNLOCK();
    }
}

void backup_start(void)
{
    xTaskCreate(backup_task, "backup", 6144, NULL, 3, NULL);
}

/* ---- Export / import ---- */

cJSON *backup_export(bool secrets)
{
    config_t c;
    current_config(&c);
    cJSON *doc = config_document(CONFIG_FORMAT, s_device);
    if (!doc) {
        return NULL;
    }
    cJSON_AddItemToObject(doc, "settings", config_to_json(&c, secrets));
    cJSON *arr = cJSON_AddArrayToObject(doc, "cards");
    card_entry_t *list;
    int n = cards_list(&list);
    for (int i = 0; arr && i < n; i++) {
        cJSON_AddItemToArray(arr, card_to_json(&list[i], true));
    }
    free(list);
    return doc;
}

esp_err_t backup_import(const cJSON *doc, char *msg, size_t msglen, bool *ip_test)
{
    char err[128] = "";
    *ip_test = false;
    if (!config_document_check(doc, CONFIG_FORMAT, err, sizeof(err))) {
        snprintf(msg, msglen, "%s", err);
        return ESP_ERR_INVALID_ARG;
    }
    config_t c;
    current_config(&c);
    c.wifi = false;
    c.admin_hash = false;
    c.mpd_hash = -1;
    const cJSON *settings = cJSON_GetObjectItem(doc, "settings");
    if (settings && !config_from_json(settings, &c, err, sizeof(err))) {
        snprintf(msg, msglen, "%s", err);
        return ESP_ERR_INVALID_ARG;
    }
    /* Associations : toutes vérifiées avant de rien modifier. */
    const cJSON *arr = cJSON_GetObjectItem(doc, "cards");
    card_entry_t *list = NULL;
    int n = 0, missing = 0;
    bool with_cards = cJSON_IsArray(arr) && storage_is_mounted();
    if (with_cards) {
        list = calloc(cJSON_GetArraySize(arr) + 1, sizeof(card_entry_t));
        if (!list) {
            snprintf(msg, msglen, "mémoire insuffisante");
            return ESP_ERR_NO_MEM;
        }
        const cJSON *it;
        cJSON_ArrayForEach(it, arr)
        {
            if (!card_from_json(it, &list[n], true, err, sizeof(err))) {
                snprintf(msg, msglen, "%s", err);
                free(list);
                return ESP_ERR_INVALID_ARG;
            }
            if (!storage_is_dir(list[n].folder)) {
                missing++; /* dossier absent de cette carte SD : association inutilisable */
            } else if (n < CARDS_MAX) {
                n++;
            }
        }
    }
    esp_err_t e = settings ? apply_config(&c, true, true, ip_test) : ESP_OK;
    if (e != ESP_OK) {
        free(list);
        snprintf(msg, msglen, "réglages refusés");
        return e;
    }
    if (with_cards) {
        e = cards_replace_all(list, n, true);
    }
    free(list);
    int o = snprintf(msg, msglen, "%s", settings ? "Réglages importés" : "Fichier importé");
    if (with_cards && o > 0 && (size_t)o < msglen) {
        o += snprintf(msg + o, msglen - o, ", %d carte%s associée%s", n, n > 1 ? "s" : "", n > 1 ? "s" : "");
    }
    if (missing && o > 0 && (size_t)o < msglen) {
        o += snprintf(msg + o, msglen - o, ", %d carte%s ignorée%s (dossier absent de la carte SD)", missing,
                      missing > 1 ? "s" : "", missing > 1 ? "s" : "");
    }
    if (cJSON_IsArray(arr) && !storage_is_mounted() && o > 0 && (size_t)o < msglen) {
        snprintf(msg + o, msglen - o, " ; associations non importées : carte SD absente");
    }
    return e;
}

/* ---- Réinitialisation usine ---- */

static void erase_dir(const char *dir, int depth)
{
    char path[ABS_PATH_MAX + 16];
    if (dir[0] && file_path(dir, CARDS_FILE, path, sizeof(path))) {
        unlink(path);
    }
    dir_entry_t *entries;
    int n;
    if (depth >= MAX_DEPTH || storage_list_dir(dir, &entries, &n) != ESP_OK) {
        return;
    }
    for (int i = 0; i < n; i++) {
        char *child = entries[i].is_dir ? malloc(REL_PATH_MAX) : NULL;
        if (child) {
            int len = dir[0] ? snprintf(child, REL_PATH_MAX, "%s/%s", dir, entries[i].name)
                             : snprintf(child, REL_PATH_MAX, "%s", entries[i].name);
            if (len > 0 && len < REL_PATH_MAX) {
                erase_dir(child, depth + 1);
            }
            free(child);
        }
    }
    storage_free_dir(entries, n);
}

void backup_factory_reset(void)
{
    if (!storage_is_mounted()) {
        ESP_LOGW(TAG, "carte SD absente : ses fichiers de réglages seront rechargés si on la remet");
        return;
    }
    LOCK();
    s_frozen = true; /* plus aucune recopie jusqu'au redémarrage */
    char path[ABS_PATH_MAX + 16];
    if (file_path("", SETTINGS_FILE, path, sizeof(path))) {
        unlink(path);
    }
    erase_dir("", 0);
    ESP_LOGW(TAG, "réglages et associations effacés de la carte SD");
    UNLOCK();
}
