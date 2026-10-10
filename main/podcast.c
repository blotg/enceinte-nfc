#include "podcast.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "cards.h"
#include "changes.h"
#include "controller.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_http.h"
#include "player.h"
#include "radio.h"
#include "storage.h"
#include "util.h"
#include "wifi_mgr.h"

static const char *TAG = "podcast";

#define FORMAT "enceinte-podcast"
#define MANIFEST_MAX (128 * 1024)
#define KNOWN_MAX 300                       /* épisodes mémorisés (téléchargés ou écartés) */
#define FEED_MAX (16 * 1024 * 1024)         /* flux RSS lu au plus */
#define FEED_TIMEOUT_MS 15000
#define DL_TIMEOUT_MS 20000
#define DL_BUF 8192
#define MIN_FREE_BYTES (300LL * 1024 * 1024) /* place gardée libre sur la carte SD */
#define TICK_US (30LL * 60 * 1000000)        /* vérification des échéances */
#define NIGHT_FROM 2
#define NIGHT_TO 5
#define DUE_NIGHT_S (20 * 3600) /* une fois par nuit */
#define DUE_ANY_S (48 * 3600)   /* enceinte éteinte la nuit : à n'importe quelle heure */
#define MAX_DEPTH 4
#define NAME_MAX_LEN 160

typedef struct {
    char guid[RSS_GUID_MAX];
    char file[NAME_MAX_LEN]; /* vide : épisode écarté ou supprimé */
    int64_t pub;
} known_t;

typedef struct {
    char url[RSS_URL_MAX];
    char title[RSS_TITLE_MAX];
    int keep;
    bool auto_name; /* dossier à renommer d'après le titre du flux */
    int64_t last_check;
    char last_error[96];
    known_t *known;
    int n_known;
} manifest_t;

typedef struct {
    char folder[REL_PATH_MAX]; /* vide : tous les podcasts dont l'heure est venue */
    bool manual;
} request_t;

static QueueHandle_t s_requests;
static SemaphoreHandle_t s_lock;
static volatile bool s_running;
static char s_busy_folder[REL_PATH_MAX]; /* s_lock */
static volatile int64_t s_dl_done, s_dl_total;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static bool clock_valid(void)
{
    return time(NULL) > 1700000000;
}

static bool child_path(const char *folder, const char *name, char *out, size_t len)
{
    int n = snprintf(out, len, "%s/%s", folder, name);
    return n > 0 && (size_t)n < len;
}

static bool abs_of(const char *folder, const char *name, char *out, size_t len)
{
    char rel[REL_PATH_MAX];
    return child_path(folder, name, rel, sizeof(rel)) && path_to_abs(rel, out, len);
}

/* ---------- Manifeste (.podcast.json) ---------- */

static void manifest_free(manifest_t *m)
{
    free(m->known);
    m->known = NULL;
}

static bool manifest_init(manifest_t *m)
{
    memset(m, 0, sizeof(*m));
    m->keep = PODCAST_KEEP_DEFAULT;
    m->known = calloc(KNOWN_MAX, sizeof(known_t)); /* ~110 Ko, en PSRAM */
    return m->known != NULL;
}

static bool manifest_load(const char *folder, manifest_t *m)
{
    if (!manifest_init(m)) {
        return false;
    }
    char abs[ABS_PATH_MAX];
    FILE *f = abs_of(folder, PODCAST_FILE, abs, sizeof(abs)) ? fopen(abs, "rb") : NULL;
    if (!f) {
        manifest_free(m);
        return false;
    }
    char *txt = malloc(MANIFEST_MAX + 1);
    size_t n = txt ? fread(txt, 1, MANIFEST_MAX, f) : 0;
    fclose(f);
    cJSON *root = NULL;
    if (txt) {
        txt[n] = '\0';
        root = cJSON_Parse(txt);
        free(txt);
    }
    const cJSON *fmt = cJSON_GetObjectItem(root, "format");
    const cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(fmt) || strcmp(fmt->valuestring, FORMAT) != 0 || !cJSON_IsString(url)) {
        cJSON_Delete(root);
        manifest_free(m);
        return false;
    }
    str_copy(m->url, url->valuestring, sizeof(m->url));
    const cJSON *it = cJSON_GetObjectItem(root, "title");
    if (cJSON_IsString(it)) {
        str_copy(m->title, it->valuestring, sizeof(m->title));
    }
    it = cJSON_GetObjectItem(root, "keep");
    if (cJSON_IsNumber(it) && it->valueint >= 1 && it->valueint <= PODCAST_KEEP_MAX) {
        m->keep = it->valueint;
    }
    m->auto_name = cJSON_IsTrue(cJSON_GetObjectItem(root, "auto_name"));
    it = cJSON_GetObjectItem(root, "last_check");
    m->last_check = cJSON_IsNumber(it) ? (int64_t)it->valuedouble : 0;
    it = cJSON_GetObjectItem(root, "last_error");
    if (cJSON_IsString(it)) {
        str_copy(m->last_error, it->valuestring, sizeof(m->last_error));
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(root, "episodes"))
    {
        const cJSON *g = cJSON_GetObjectItem(e, "guid");
        const cJSON *file = cJSON_GetObjectItem(e, "file");
        const cJSON *pub = cJSON_GetObjectItem(e, "pub");
        if (!cJSON_IsString(g) || m->n_known >= KNOWN_MAX) {
            continue;
        }
        known_t *k = &m->known[m->n_known++];
        str_copy(k->guid, g->valuestring, sizeof(k->guid));
        str_copy(k->file, cJSON_IsString(file) ? file->valuestring : "", sizeof(k->file));
        k->pub = cJSON_IsNumber(pub) ? (int64_t)pub->valuedouble : 0;
    }
    cJSON_Delete(root);
    return true;
}

static bool manifest_save(const char *folder, const manifest_t *m)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", FORMAT);
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "url", m->url);
    cJSON_AddStringToObject(root, "title", m->title);
    cJSON_AddNumberToObject(root, "keep", m->keep);
    if (m->auto_name) {
        cJSON_AddBoolToObject(root, "auto_name", true);
    }
    cJSON_AddNumberToObject(root, "last_check", (double)m->last_check);
    cJSON_AddStringToObject(root, "last_error", m->last_error);
    cJSON *arr = cJSON_AddArrayToObject(root, "episodes");
    for (int i = 0; i < m->n_known; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "guid", m->known[i].guid);
        cJSON_AddStringToObject(e, "file", m->known[i].file);
        cJSON_AddNumberToObject(e, "pub", (double)m->known[i].pub);
        cJSON_AddItemToArray(arr, e);
    }
    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    char abs[ABS_PATH_MAX], tmp[ABS_PATH_MAX + 4];
    bool ok = txt && abs_of(folder, PODCAST_FILE, abs, sizeof(abs));
    if (ok) {
        snprintf(tmp, sizeof(tmp), "%s.tmp", abs);
        FILE *f = fopen(tmp, "wb");
        ok = f && fwrite(txt, 1, strlen(txt), f) == strlen(txt);
        if (f && fclose(f) != 0) {
            ok = false;
        }
        if (ok) {
            unlink(abs);
            ok = rename(tmp, abs) == 0;
        } else {
            unlink(tmp);
        }
    }
    free(txt);
    if (!ok) {
        ESP_LOGW(TAG, "%s : écriture de %s impossible", folder, PODCAST_FILE);
    }
    return ok;
}

static int known_find(const manifest_t *m, const char *guid)
{
    for (int i = 0; i < m->n_known; i++) {
        if (strcmp(m->known[i].guid, guid) == 0) {
            return i;
        }
    }
    return -1;
}

static void known_add(manifest_t *m, const rss_item_t *it, const char *file)
{
    if (m->n_known >= KNOWN_MAX) {
        /* place : on oublie d'abord un épisode écarté, sinon le plus ancien */
        int victim = 0;
        for (int i = 0; i < m->n_known; i++) {
            if (!m->known[i].file[0]) {
                victim = i;
                break;
            }
        }
        memmove(&m->known[victim], &m->known[victim + 1], (size_t)(m->n_known - victim - 1) * sizeof(known_t));
        m->n_known--;
    }
    known_t *k = &m->known[m->n_known++];
    str_copy(k->guid, it->guid, sizeof(k->guid));
    str_copy(k->file, file, sizeof(k->file));
    k->pub = it->pub;
}

/* ---------- Réseau ---------- */

typedef struct {
    rss_item_t *top;
    int keep;
    int count;
} feed_ctx_t;

static bool on_item(const rss_item_t *it, void *arg)
{
    feed_ctx_t *f = arg;
    rss_keep_newest(f->top, f->keep, &f->count, it);
    return true;
}

/* Lit le flux : les "keep" épisodes les plus récents, et le titre. */
static bool fetch_feed(const char *url, rss_item_t *top, int keep, int *count, char *title, size_t tlen, char *err,
                       size_t errlen)
{
    http_info_t info;
    char e[64];
    esp_http_client_handle_t h = http_get_open(url, false, FEED_TIMEOUT_MS, &info, NULL, 0, e, sizeof(e));
    if (!h) {
        snprintf(err, errlen, "flux injoignable : %s", e);
        return false;
    }
    feed_ctx_t ctx = {.top = top, .keep = keep};
    rss_parser_t *p = rss_new(on_item, &ctx);
    char *buf = malloc(4096);
    size_t total = 0;
    int n = 0;
    while (p && buf && total < FEED_MAX && (n = esp_http_client_read(h, buf, 4096)) > 0) {
        total += (size_t)n;
        rss_feed(p, buf, (size_t)n);
    }
    bool ok = p && buf && total > 0 && n >= 0;
    if (p) {
        str_copy(title, rss_channel_title(p), tlen);
    }
    *count = ctx.count;
    if (!ok) {
        snprintf(err, errlen, "lecture du flux interrompue");
    } else if (!title[0] && ctx.count == 0) {
        snprintf(err, errlen, "cette adresse n'est pas un flux de podcast (RSS)");
        ok = false;
    }
    rss_free(p);
    free(buf);
    http_close(h);
    return ok;
}

/* Télécharge un épisode dans folder/name (via un fichier .part). */
static bool download(const char *url, const char *folder, const char *name, char *err, size_t errlen)
{
    char abs[ABS_PATH_MAX], part[ABS_PATH_MAX + 8];
    if (!abs_of(folder, name, abs, sizeof(abs))) {
        snprintf(err, errlen, "nom de fichier trop long");
        return false;
    }
    snprintf(part, sizeof(part), "%s.part", abs);
    http_info_t info;
    char e[64];
    esp_http_client_handle_t h = http_get_open(url, false, DL_TIMEOUT_MS, &info, NULL, 0, e, sizeof(e));
    if (!h) {
        snprintf(err, errlen, "épisode injoignable : %s", e);
        return false;
    }
    uint64_t total = 0, freeb = 0;
    if (storage_get_usage(&total, &freeb) &&
        (int64_t)freeb < MIN_FREE_BYTES + (info.content_length > 0 ? info.content_length : 0)) {
        http_close(h);
        snprintf(err, errlen, "carte SD presque pleine");
        return false;
    }
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    /* tampon interne compatible DMA : écriture SD par blocs entiers */
    uint8_t *buf = heap_caps_malloc(DL_BUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        buf = malloc(DL_BUF);
    }
    s_dl_total = info.content_length;
    s_dl_done = 0;
    bool ok = fd >= 0 && buf;
    int stalls = 0;
    while (ok) {
        int n = esp_http_client_read(h, (char *)buf, DL_BUF);
        if (n == -ESP_ERR_HTTP_EAGAIN && ++stalls < 4) {
            continue;
        }
        if (n < 0) {
            snprintf(err, errlen, "téléchargement interrompu");
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        stalls = 0;
        if (write(fd, buf, (size_t)n) != n) {
            snprintf(err, errlen, "écriture sur la carte SD impossible");
            storage_report_io_error();
            ok = false;
            break;
        }
        s_dl_done += n;
    }
    if (ok && info.content_length > 0 && s_dl_done != info.content_length) {
        snprintf(err, errlen, "téléchargement incomplet");
        ok = false;
    }
    if (ok && s_dl_done == 0) {
        snprintf(err, errlen, "fichier vide");
        ok = false;
    }
    free(buf);
    if (fd >= 0 && close(fd) != 0) {
        ok = false;
    }
    http_close(h);
    if (ok) {
        unlink(abs);
        ok = rename(part, abs) == 0;
    }
    if (!ok) {
        unlink(part);
    }
    s_dl_total = s_dl_done = 0;
    return ok;
}

/* ---------- Synchronisation ---------- */

/* Nom libre dans le dossier : « nom », sinon « nom (2) »... */
static bool unique_name(const char *folder, const char *name, char *out, size_t len)
{
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    for (int i = 1; i < 10; i++) {
        if (i == 1) {
            str_copy(out, name, len);
        } else {
            snprintf(out, len, "%.*s (%d)%s", (int)base, name, i, dot ? dot : "");
        }
        char rel[REL_PATH_MAX];
        if (child_path(folder, out, rel, sizeof(rel)) && !storage_exists(rel)) {
            return true;
        }
    }
    return false;
}

/* Dossier provisoire : renommé d'après le titre du flux ; les cartes suivent. */
static void rename_folder(char *folder, size_t len, const char *title)
{
    char parent[REL_PATH_MAX], name[96], target[REL_PATH_MAX];
    path_dirname(folder, parent, sizeof(parent));
    name_from_text(title, name, sizeof(name), "Podcast");
    for (int i = 1; i < 10; i++) {
        int n = i == 1 ? snprintf(target, sizeof(target), "%s/%s", parent, name)
                       : snprintf(target, sizeof(target), "%s/%s (%d)", parent, name, i);
        if (n <= 0 || (size_t)n >= sizeof(target)) {
            return;
        }
        if (strcmp(target, folder) == 0) {
            return; /* déjà ce nom */
        }
        if (!storage_exists(target)) {
            break;
        }
    }
    if (storage_rename(folder, target) == ESP_OK) {
        ESP_LOGI(TAG, "dossier « %s » renommé « %s »", folder, target);
        cards_on_folder_renamed(folder, target);
        controller_on_path_renamed(folder, target);
        str_copy(folder, target, len);
    }
}

static bool playing_file(const char *folder, const char *file)
{
    player_status_t st;
    player_get_status(&st);
    char rel[REL_PATH_MAX];
    return st.state != PLAYER_STOPPED && child_path(folder, file, rel, sizeof(rel)) && strcmp(st.file, rel) == 0;
}

static void sync_folder(const char *start_folder)
{
    char folder[REL_PATH_MAX];
    str_copy(folder, start_folder, sizeof(folder));
    manifest_t m;
    if (!manifest_load(folder, &m)) {
        return;
    }
    LOCK();
    str_copy(s_busy_folder, folder, sizeof(s_busy_folder));
    UNLOCK();
    changes_notify(CHG_DATABASE);
    ESP_LOGI(TAG, "%s : vérification du flux", folder);
    rss_item_t *top = calloc((size_t)m.keep, sizeof(rss_item_t));
    char title[RSS_TITLE_MAX] = "", err[96] = "";
    int count = 0, downloaded = 0, pruned = 0;
    bool ok = top && fetch_feed(m.url, top, m.keep, &count, title, sizeof(title), err, sizeof(err));
    if (ok) {
        if (m.auto_name && title[0]) {
            rename_folder(folder, sizeof(folder), title);
            m.auto_name = false;
            LOCK();
            str_copy(s_busy_folder, folder, sizeof(s_busy_folder));
            UNLOCK();
        }
        if (title[0]) {
            str_copy(m.title, title, sizeof(m.title));
        }
        /* du plus récent au plus ancien : le dernier épisode arrive en premier */
        for (int i = 0; i < count; i++) {
            if (known_find(&m, top[i].guid) >= 0) {
                continue;
            }
            char base[NAME_MAX_LEN], name[NAME_MAX_LEN];
            rss_episode_name(&top[i], base, sizeof(base));
            if (!unique_name(folder, base, name, sizeof(name))) {
                continue;
            }
            ESP_LOGI(TAG, "%s : téléchargement de « %s »", folder, name);
            if (!download(top[i].url, folder, name, err, sizeof(err))) {
                ESP_LOGW(TAG, "%s : %s", folder, err);
                break; /* réseau ou carte SD : on réessaiera */
            }
            known_add(&m, &top[i], name);
            manifest_save(folder, &m); /* chaque épisode compte, même si la suite échoue */
            changes_notify(CHG_DATABASE);
            downloaded++;
        }
        /* épisodes sortis des N plus récents : supprimés (sauf celui qu'on écoute) */
        for (int k = 0; k < m.n_known; k++) {
            if (!m.known[k].file[0]) {
                continue;
            }
            bool keep = false;
            for (int i = 0; i < count && !keep; i++) {
                keep = strcmp(top[i].guid, m.known[k].guid) == 0;
            }
            if (keep || playing_file(folder, m.known[k].file)) {
                continue;
            }
            char abs[ABS_PATH_MAX];
            if (abs_of(folder, m.known[k].file, abs, sizeof(abs))) {
                unlink(abs);
            }
            m.known[k].file[0] = '\0';
            pruned++;
        }
        m.last_check = clock_valid() ? (int64_t)time(NULL) : 0;
    } else if (!top) {
        snprintf(err, sizeof(err), "mémoire insuffisante");
    }
    str_copy(m.last_error, err, sizeof(m.last_error));
    manifest_save(folder, &m);
    manifest_free(&m);
    free(top);
    if (downloaded || pruned) {
        changes_notify(CHG_DATABASE);
    }
    ESP_LOGI(TAG, "%s : %d épisode(s) téléchargé(s), %d supprimé(s)%s%s", folder, downloaded, pruned,
             err[0] ? " ; " : "", err);
    LOCK();
    s_busy_folder[0] = '\0';
    UNLOCK();
}

typedef struct {
    char (*folders)[REL_PATH_MAX];
    int count, cap;
} found_t;

static void find_podcasts(const char *rel, int depth, found_t *f)
{
    char m[REL_PATH_MAX];
    if (rel[0] && child_path(rel, PODCAST_FILE, m, sizeof(m)) && storage_exists(m) && f->count < f->cap) {
        str_copy(f->folders[f->count++], rel, REL_PATH_MAX);
        return; /* pas de podcast dans un podcast */
    }
    if (depth >= MAX_DEPTH) {
        return;
    }
    dir_entry_t *entries;
    int n;
    if (storage_list_dir(rel, &entries, &n) != ESP_OK) {
        return;
    }
    for (int i = 0; i < n; i++) {
        if (!entries[i].is_dir || name_is_hidden(entries[i].name)) {
            continue;
        }
        char child[REL_PATH_MAX];
        if (!rel[0]) {
            str_copy(child, entries[i].name, sizeof(child));
        } else if (!child_path(rel, entries[i].name, child, sizeof(child))) {
            continue;
        }
        find_podcasts(child, depth + 1, f);
    }
    storage_free_dir(entries, n);
}

/* Heure venue pour ce podcast (vérification automatique) ? */
static bool due(const char *folder)
{
    manifest_t m;
    if (!manifest_load(folder, &m)) {
        return false;
    }
    int64_t now = (int64_t)time(NULL);
    int64_t age = m.last_check ? now - m.last_check : INT64_MAX;
    manifest_free(&m);
    struct tm tm;
    time_t t = (time_t)now;
    localtime_r(&t, &tm);
    bool night = tm.tm_hour >= NIGHT_FROM && tm.tm_hour < NIGHT_TO;
    return (night && age > DUE_NIGHT_S) || age > DUE_ANY_S;
}

static bool player_busy(void)
{
    player_status_t st;
    player_get_status(&st);
    return st.state == PLAYER_PLAYING;
}

static void podcast_task(void *arg)
{
    request_t *req = malloc(sizeof(request_t));
    found_t f = {.cap = 64};
    f.folders = calloc((size_t)f.cap, REL_PATH_MAX);
    for (;;) {
        if (!req || !f.folders || xQueueReceive(s_requests, req, 0) != pdTRUE) {
            /* plus rien à faire : on s'arrête, sauf demande arrivée entre-temps (cf. request) */
            LOCK();
            bool more = req && f.folders && uxQueueMessagesWaiting(s_requests) > 0;
            if (!more) {
                s_running = false;
            }
            UNLOCK();
            if (!more) {
                break;
            }
            continue;
        }
        wifi_status_t ws;
        wifi_mgr_get_status(&ws);
        if (!storage_is_mounted() || !ws.sta_connected) {
            continue;
        }
        f.count = 0;
        if (req->folder[0]) {
            str_copy(f.folders[f.count++], req->folder, REL_PATH_MAX);
        } else {
            find_podcasts("", 0, &f);
        }
        for (int i = 0; i < f.count; i++) {
            if (!req->manual && (player_busy() || !due(f.folders[i]))) {
                continue; /* automatique : jamais pendant l'écoute */
            }
            sync_folder(f.folders[i]);
        }
    }
    free(req);
    free(f.folders);
    vTaskDelete(NULL);
}

static void request(const char *folder, bool manual)
{
    request_t req = {.manual = manual};
    if (folder) {
        str_copy(req.folder, folder, sizeof(req.folder));
    }
    if (xQueueSend(s_requests, &req, 0) != pdTRUE) {
        return; /* déjà beaucoup de demandes en attente */
    }
    LOCK();
    bool start = !s_running;
    s_running = true;
    UNLOCK();
    /* Tâche créée à la demande : sa pile (TLS, écriture SD) n'occupe la RAM interne que
     * pendant les téléchargements. */
    if (start && xTaskCreate(podcast_task, "podcast", 8192, NULL, 3, NULL) != pdPASS) {
        s_running = false;
    }
}

static void on_tick(void *arg)
{
    if (clock_valid()) {
        request(NULL, false);
    }
}

void podcast_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_requests = xQueueCreate(8, sizeof(request_t));
    esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = on_tick, .name = "podcast"};
    if (s_lock && s_requests && esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_periodic(t, TICK_US);
    }
}

void podcast_sync_now(const char *folder)
{
    request(folder, true);
}

bool podcast_busy(void)
{
    return s_running;
}

/* ---------- Abonnements ---------- */

bool podcast_is_folder(const char *folder)
{
    char rel[REL_PATH_MAX];
    return folder[0] && child_path(folder, PODCAST_FILE, rel, sizeof(rel)) && storage_exists(rel);
}

static bool valid_url(const char *url)
{
    return radio_is_url(url) && strlen(url) < RSS_URL_MAX && !strpbrk(url, " \t\r\n\"<>");
}

esp_err_t podcast_subscribe(const char *url, const char *name, int keep, char *folder_out, size_t len, char *err,
                            size_t errlen)
{
    if (!valid_url(url)) {
        snprintf(err, errlen, "adresse de flux invalide (http:// ou https://)");
        return ESP_ERR_INVALID_ARG;
    }
    if (keep < 1 || keep > PODCAST_KEEP_MAX) {
        keep = PODCAST_KEEP_DEFAULT;
    }
    if (!storage_is_mounted()) {
        snprintf(err, errlen, "carte SD absente");
        return ESP_ERR_INVALID_STATE;
    }
    bool auto_name = !name || !name[0];
    char base[96], folder[REL_PATH_MAX];
    name_from_text(auto_name ? "" : name, base, sizeof(base), "Nouveau podcast");
    if (!storage_is_dir(PODCAST_BASE) && storage_mkdir(PODCAST_BASE) != ESP_OK) {
        snprintf(err, errlen, "création du dossier « %s » impossible", PODCAST_BASE);
        return ESP_FAIL;
    }
    bool made = false;
    for (int i = 1; i < 10 && !made; i++) {
        if (i == 1) {
            snprintf(folder, sizeof(folder), "%s/%s", PODCAST_BASE, base);
        } else {
            snprintf(folder, sizeof(folder), "%s/%s (%d)", PODCAST_BASE, base, i);
        }
        made = !storage_exists(folder) && storage_mkdir(folder) == ESP_OK;
    }
    if (!made) {
        snprintf(err, errlen, "création du dossier impossible");
        return ESP_FAIL;
    }
    manifest_t m;
    if (!manifest_init(&m)) {
        snprintf(err, errlen, "mémoire insuffisante");
        return ESP_ERR_NO_MEM;
    }
    str_copy(m.url, url, sizeof(m.url));
    str_copy(m.title, auto_name ? "" : name, sizeof(m.title));
    m.keep = keep;
    m.auto_name = auto_name;
    bool ok = manifest_save(folder, &m);
    manifest_free(&m);
    if (!ok) {
        snprintf(err, errlen, "écriture sur la carte SD impossible");
        return ESP_FAIL;
    }
    str_copy(folder_out, folder, len);
    ESP_LOGI(TAG, "abonnement : %s -> %s", url, folder);
    podcast_sync_now(folder);
    return ESP_OK;
}

esp_err_t podcast_update(const char *folder, const char *url, int keep)
{
    if (url && !valid_url(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    manifest_t m;
    if (!manifest_load(folder, &m)) {
        return ESP_ERR_NOT_FOUND;
    }
    bool changed = false;
    if (url && strcmp(url, m.url) != 0) {
        str_copy(m.url, url, sizeof(m.url));
        changed = true;
    }
    if (keep >= 1 && keep <= PODCAST_KEEP_MAX && keep != m.keep) {
        m.keep = keep;
        changed = true;
    }
    bool ok = !changed || manifest_save(folder, &m);
    manifest_free(&m);
    if (changed && ok) {
        podcast_sync_now(folder);
    }
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t podcast_unsubscribe(const char *folder)
{
    char abs[ABS_PATH_MAX];
    if (!abs_of(folder, PODCAST_FILE, abs, sizeof(abs)) || unlink(abs) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "désabonnement : %s (épisodes gardés)", folder);
    changes_notify(CHG_DATABASE);
    return ESP_OK;
}

bool podcast_get(const char *folder, podcast_info_t *out)
{
    manifest_t m;
    if (!manifest_load(folder, &m)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    str_copy(out->url, m.url, sizeof(out->url));
    str_copy(out->title, m.title, sizeof(out->title));
    out->keep = m.keep;
    out->last_check = m.last_check;
    str_copy(out->last_error, m.last_error, sizeof(out->last_error));
    for (int i = 0; i < m.n_known; i++) {
        char rel[REL_PATH_MAX];
        if (m.known[i].file[0] && child_path(folder, m.known[i].file, rel, sizeof(rel)) && storage_exists(rel)) {
            out->episodes++;
        }
    }
    manifest_free(&m);
    LOCK();
    out->syncing = s_running && strcmp(s_busy_folder, folder) == 0;
    UNLOCK();
    int64_t total = s_dl_total, done = s_dl_done;
    out->progress = out->syncing && total > 0 ? (int)(done * 100 / total) : -1;
    return true;
}
