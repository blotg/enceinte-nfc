#include "podcast.h"

#include <fcntl.h>
#include <inttypes.h>
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
#include "log_buffer.h"
#include "net_http.h"
#include "player.h"
#include "radio.h"
#include "storage.h"
#include "util.h"
#include "wifi_mgr.h"

static const char *TAG = "podcast";

#define FORMAT "enceinte-podcast"
#define MANIFEST_MAX (128 * 1024)
#define KNOWN_MAX 10000 /* épisodes connus (~180 octets chacun, en PSRAM) */
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
    uint64_t id; /* rss_guid_id du guid */
    int64_t pub;
    char file[NAME_MAX_LEN]; /* vide : épisode écarté ou supprimé */
} known_t;

typedef struct {
    char url[RSS_URL_MAX];
    char title[RSS_TITLE_MAX];
    int keep; /* 0 : tous les épisodes */
    bool auto_name; /* dossier à renommer d'après le titre du flux */
    int64_t last_check;
    char last_error[96];
    known_t *known; /* épisodes connus (téléchargés, ou supprimés à la main), en PSRAM */
    int n_known, cap_known;
    bool episodes_loaded; /* sinon manifest_save ne réécrit pas la liste des épisodes */
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
static volatile int s_dl_index, s_dl_count; /* « épisode 3 sur 12 » */

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

/* ---------- Manifeste ---------- */

/*
 * Réglages dans .podcast.json ; épisodes connus dans .podcast-episodes.txt, une ligne par
 * épisode (« id date fichier ») : lue et écrite au fil de l'eau, la liste peut être longue
 * sans occuper de RAM interne (un arbre cJSON en prendrait beaucoup).
 */
#define EPISODES_FILE ".podcast-episodes.txt"

/* Gros tampons en PSRAM : la RAM interne est réservée aux piles, au Wi-Fi et au TLS. */
static void *big_realloc(void *p, size_t size)
{
    void *n = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return n ? n : realloc(p, size);
}

static void manifest_free(manifest_t *m)
{
    free(m->known);
    m->known = NULL;
    m->n_known = m->cap_known = 0;
}

static void manifest_init(manifest_t *m)
{
    memset(m, 0, sizeof(*m));
    m->keep = PODCAST_KEEP_DEFAULT;
}

static known_t *known_new(manifest_t *m)
{
    if (m->n_known == m->cap_known) {
        if (m->cap_known >= KNOWN_MAX) {
            return NULL;
        }
        int cap = m->cap_known ? m->cap_known * 2 : 32;
        cap = cap < KNOWN_MAX ? cap : KNOWN_MAX;
        known_t *n = big_realloc(m->known, (size_t)cap * sizeof(known_t));
        if (!n) {
            return NULL;
        }
        m->known = n;
        m->cap_known = cap;
    }
    known_t *k = &m->known[m->n_known++];
    memset(k, 0, sizeof(*k));
    return k;
}

static bool read_episodes(const char *folder, manifest_t *m)
{
    char abs[ABS_PATH_MAX];
    if (!abs_of(folder, EPISODES_FILE, abs, sizeof(abs))) {
        return false;
    }
    FILE *f = fopen(abs, "r");
    if (!f) {
        return true; /* pas encore d'épisode */
    }
    char line[NAME_MAX_LEN + 64];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || !strchr(line, '\n')) {
            continue; /* commentaire, ou ligne trop longue (jamais écrite ainsi) */
        }
        char *p = line, *e;
        uint64_t id = strtoull(p, &e, 16);
        if (e == p || *e != ' ') {
            continue;
        }
        p = e + 1;
        int64_t pub = strtoll(p, &e, 10);
        if (e == p || *e != ' ') {
            continue;
        }
        p = e + 1;
        p[strcspn(p, "\r\n")] = '\0';
        known_t *k = known_new(m);
        if (!k) {
            break;
        }
        k->id = id;
        k->pub = pub;
        str_copy(k->file, p, sizeof(k->file));
    }
    fclose(f);
    return true;
}

static bool manifest_load(const char *folder, manifest_t *m, bool episodes)
{
    manifest_init(m);
    char abs[ABS_PATH_MAX];
    FILE *f = abs_of(folder, PODCAST_FILE, abs, sizeof(abs)) ? fopen(abs, "rb") : NULL;
    if (!f) {
        return false;
    }
    /* jusqu'à 128 Ko : les manifestes de la version 1 contenaient aussi les épisodes */
    char *txt = big_realloc(NULL, MANIFEST_MAX + 1);
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
        return false;
    }
    str_copy(m->url, url->valuestring, sizeof(m->url));
    const cJSON *it = cJSON_GetObjectItem(root, "title");
    if (cJSON_IsString(it)) {
        str_copy(m->title, it->valuestring, sizeof(m->title));
    }
    it = cJSON_GetObjectItem(root, "keep");
    if (cJSON_IsNumber(it) && it->valuedouble >= 0 && it->valuedouble <= PODCAST_KEEP_MAX) {
        m->keep = it->valueint;
    }
    m->auto_name = cJSON_IsTrue(cJSON_GetObjectItem(root, "auto_name"));
    it = cJSON_GetObjectItem(root, "last_check");
    m->last_check = cJSON_IsNumber(it) ? (int64_t)it->valuedouble : 0;
    it = cJSON_GetObjectItem(root, "last_error");
    if (cJSON_IsString(it)) {
        str_copy(m->last_error, it->valuestring, sizeof(m->last_error));
    }
    bool ok = true;
    const cJSON *old = cJSON_GetObjectItem(root, "episodes");
    if (cJSON_IsArray(old)) {
        /* version 1 : épisodes dans le JSON, réécrits dans EPISODES_FILE au prochain enregistrement */
        const cJSON *e;
        cJSON_ArrayForEach(e, old)
        {
            const cJSON *g = cJSON_GetObjectItem(e, "guid");
            const cJSON *file = cJSON_GetObjectItem(e, "file");
            const cJSON *pub = cJSON_GetObjectItem(e, "pub");
            known_t *k = cJSON_IsString(g) ? known_new(m) : NULL;
            if (k) {
                k->id = rss_guid_id(g->valuestring);
                k->pub = cJSON_IsNumber(pub) ? (int64_t)pub->valuedouble : 0;
                str_copy(k->file, cJSON_IsString(file) ? file->valuestring : "", sizeof(k->file));
            }
        }
        m->episodes_loaded = true;
    } else if (episodes) {
        ok = read_episodes(folder, m);
        m->episodes_loaded = ok;
    }
    cJSON_Delete(root);
    if (!ok) {
        manifest_free(m);
    }
    return ok;
}

/* Écrit text (ou la liste des épisodes) dans folder/name, via un fichier provisoire. */
static bool write_atomic(const char *folder, const char *name, const char *text, const manifest_t *eps)
{
    char abs[ABS_PATH_MAX], tmp[ABS_PATH_MAX + 4];
    if (!abs_of(folder, name, abs, sizeof(abs))) {
        return false;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", abs);
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL;
    if (ok && text) {
        ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    } else if (ok) {
        ok = fputs("# épisodes connus : identifiant, date, fichier (vide : supprimé)\n", f) >= 0;
        for (int i = 0; ok && i < eps->n_known; i++) {
            const known_t *k = &eps->known[i];
            ok = fprintf(f, "%016" PRIx64 " %" PRId64 " %s\n", k->id, k->pub, k->file) > 0;
        }
    }
    if (f && fclose(f) != 0) {
        ok = false;
    }
    if (ok) {
        unlink(abs);
        ok = rename(tmp, abs) == 0;
    } else {
        unlink(tmp);
    }
    return ok;
}

static bool manifest_save(const char *folder, const manifest_t *m)
{
    /* épisodes d'abord : un manifeste de la version 1 ne perd les siens qu'une fois copiés */
    bool ok = !m->episodes_loaded || write_atomic(folder, EPISODES_FILE, NULL, m);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", FORMAT);
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "url", m->url);
    cJSON_AddStringToObject(root, "title", m->title);
    cJSON_AddNumberToObject(root, "keep", m->keep);
    if (m->auto_name) {
        cJSON_AddBoolToObject(root, "auto_name", true);
    }
    cJSON_AddNumberToObject(root, "last_check", (double)m->last_check);
    cJSON_AddStringToObject(root, "last_error", m->last_error);
    char *txt = ok ? cJSON_Print(root) : NULL;
    cJSON_Delete(root);
    ok = txt && write_atomic(folder, PODCAST_FILE, txt, NULL);
    free(txt);
    if (!ok) {
        ESP_LOGW(TAG, "%s : écriture de %s impossible", folder, PODCAST_FILE);
    }
    return ok;
}

static int known_find(const manifest_t *m, uint64_t id)
{
    for (int i = 0; i < m->n_known; i++) {
        if (m->known[i].id == id) {
            return i;
        }
    }
    return -1;
}

static void known_add(manifest_t *m, const rss_item_t *it, const char *file)
{
    known_t *k = known_new(m);
    if (!k) {
        /* liste pleine : on oublie le plus ancien */
        memmove(&m->known[0], &m->known[1], (size_t)(m->n_known - 1) * sizeof(known_t));
        k = &m->known[m->n_known - 1];
    }
    k->id = it->id;
    k->pub = it->pub;
    str_copy(k->file, file, sizeof(k->file));
}

/* ---------- Réseau ---------- */

typedef struct {
    rss_list_t *list;
    bool no_mem;
} feed_ctx_t;

static bool on_item(const rss_item_t *it, void *arg)
{
    feed_ctx_t *f = arg;
    f->no_mem = !rss_list_add(f->list, it);
    return !f->no_mem;
}

/* Lit le flux : ses épisodes (tous ou les plus récents, cf. rss_list_init) et son titre. */
static bool fetch_feed(const char *url, rss_list_t *list, char *title, size_t tlen, char *err, size_t errlen)
{
    http_info_t info;
    char e[64];
    esp_http_client_handle_t h = http_get_open(url, false, FEED_TIMEOUT_MS, &info, NULL, 0, e, sizeof(e));
    if (!h) {
        snprintf(err, errlen, "flux injoignable : %s", e);
        return false;
    }
    feed_ctx_t ctx = {.list = list};
    rss_parser_t *p = rss_new(on_item, &ctx);
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM); /* la RAM interne reste au TLS */
    if (!buf) {
        buf = malloc(4096);
    }
    size_t total = 0;
    int n = 0;
    while (p && buf && total < FEED_MAX && (n = esp_http_client_read(h, buf, 4096)) > 0) {
        total += (size_t)n;
        if (!rss_feed(p, buf, (size_t)n)) {
            break;
        }
    }
    bool ok = p && buf && total > 0 && n >= 0 && !ctx.no_mem;
    if (p) {
        str_copy(title, rss_channel_title(p), tlen);
    }
    rss_list_finish(list);
    if (ctx.no_mem || !p || !buf) {
        snprintf(err, errlen, "mémoire insuffisante pour lire le flux");
    } else if (!ok) {
        snprintf(err, errlen, "lecture du flux interrompue");
    } else if (!title[0] && list->count == 0) {
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

static bool player_busy(void)
{
    player_status_t st;
    player_get_status(&st);
    return st.state == PLAYER_PLAYING;
}

static void sync_folder(const char *start_folder, bool manual)
{
    char folder[REL_PATH_MAX];
    str_copy(folder, start_folder, sizeof(folder));
    manifest_t m;
    if (!manifest_load(folder, &m, true)) {
        return;
    }
    LOCK();
    str_copy(s_busy_folder, folder, sizeof(s_busy_folder));
    UNLOCK();
    changes_notify(CHG_DATABASE);
    ESP_LOGI(TAG, "%s : vérification du flux", folder);
    log_buffer_memory("avant");
    rss_list_t list;
    rss_list_init(&list, m.keep);
    char title[RSS_TITLE_MAX] = "", err[96] = "";
    int downloaded = 0, pruned = 0;
    bool interrupted = false;
    if (fetch_feed(m.url, &list, title, sizeof(title), err, sizeof(err))) {
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
        int todo = 0;
        for (int i = 0; i < list.count; i++) {
            todo += known_find(&m, list.items[i].id) < 0 ? 1 : 0;
        }
        s_dl_count = todo;
        s_dl_index = 0;
        /* du plus récent au plus ancien : le dernier épisode arrive en premier */
        for (int i = 0; i < list.count; i++) {
            const rss_item_t *it = &list.items[i];
            if (known_find(&m, it->id) >= 0) {
                continue;
            }
            if (!manual && player_busy()) {
                interrupted = true; /* automatique : la suite attendra la fin de l'écoute */
                break;
            }
            char base[NAME_MAX_LEN], name[NAME_MAX_LEN];
            rss_episode_name(it, base, sizeof(base));
            if (!unique_name(folder, base, name, sizeof(name))) {
                continue;
            }
            s_dl_index = s_dl_index < s_dl_count ? s_dl_index + 1 : s_dl_count;
            ESP_LOGI(TAG, "%s : téléchargement %d/%d : « %s »", folder, s_dl_index, s_dl_count, name);
            if (!download(it->url, folder, name, err, sizeof(err))) {
                ESP_LOGW(TAG, "%s : %s", folder, err);
                break; /* réseau ou carte SD : on réessaiera */
            }
            known_add(&m, it, name);
            manifest_save(folder, &m); /* chaque épisode compte, même si la suite échoue */
            changes_notify(CHG_DATABASE);
            downloaded++;
        }
        /* Épisodes sortis des N plus récents : supprimés (sauf celui qu'on écoute) et oubliés,
         * pour revenir si N augmente. Un épisode déjà supprimé à la main reste connu : il ne
         * revient jamais. Avec « tous » (0), rien n'est supprimé. */
        bool prune = m.keep > 0 && m.keep <= RSS_LIST_MAX;
        int w = 0;
        for (int k = 0; k < m.n_known; k++) {
            known_t *e = &m.known[k];
            bool listed = false;
            for (int i = 0; i < list.count && !listed; i++) {
                listed = list.items[i].id == e->id;
            }
            if (!listed && prune && e->file[0] && !playing_file(folder, e->file)) {
                char abs[ABS_PATH_MAX];
                if (abs_of(folder, e->file, abs, sizeof(abs)) && unlink(abs) == 0) {
                    e->file[0] = '\0';
                    pruned++;
                }
            }
            if (listed || e->file[0]) {
                m.known[w++] = *e;
            }
        }
        m.n_known = w;
        if (!interrupted) {
            m.last_check = clock_valid() ? (int64_t)time(NULL) : 0;
        }
    }
    s_dl_index = s_dl_count = 0;
    str_copy(m.last_error, err, sizeof(m.last_error));
    manifest_save(folder, &m);
    manifest_free(&m);
    rss_list_free(&list);
    if (downloaded || pruned) {
        changes_notify(CHG_DATABASE);
    }
    ESP_LOGI(TAG, "%s : %d épisode(s) téléchargé(s), %d supprimé(s)%s%s", folder, downloaded, pruned,
             err[0] ? " ; " : "", err);
    log_buffer_memory("après");
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
    if (!manifest_load(folder, &m, false)) {
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

static uint32_t s_stack_low = UINT32_MAX; /* d'une tâche à la suivante (même taille de pile) */

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
            sync_folder(f.folders[i], req->manual);
            log_buffer_stack_check("podcast", NULL, &s_stack_low);
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
    if (start && xTaskCreate(podcast_task, "podcast", 12288, NULL, 3, NULL) != pdPASS) {
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
    if (keep < 0 || keep > PODCAST_KEEP_MAX) {
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
    manifest_init(&m);
    m.episodes_loaded = true; /* liste vide, écrite avec le manifeste */
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
    if (!manifest_load(folder, &m, false)) {
        return ESP_ERR_NOT_FOUND;
    }
    bool changed = false;
    if (url && strcmp(url, m.url) != 0) {
        str_copy(m.url, url, sizeof(m.url));
        changed = true;
    }
    if (keep >= 0 && keep <= PODCAST_KEEP_MAX && keep != m.keep) {
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
    if (abs_of(folder, EPISODES_FILE, abs, sizeof(abs))) {
        unlink(abs);
    }
    ESP_LOGI(TAG, "désabonnement : %s (épisodes gardés)", folder);
    changes_notify(CHG_DATABASE);
    return ESP_OK;
}

bool podcast_get(const char *folder, podcast_info_t *out)
{
    manifest_t m;
    if (!manifest_load(folder, &m, false)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    str_copy(out->url, m.url, sizeof(out->url));
    str_copy(out->title, m.title, sizeof(out->title));
    out->keep = m.keep;
    out->last_check = m.last_check;
    str_copy(out->last_error, m.last_error, sizeof(out->last_error));
    manifest_free(&m);
    /* épisodes présents : fichiers audio du dossier (une seule lecture du dossier) */
    dir_entry_t *entries;
    int n;
    if (storage_list_dir(folder, &entries, &n) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            out->episodes += !entries[i].is_dir && is_audio_file(entries[i].name) ? 1 : 0;
        }
        storage_free_dir(entries, n);
    }
    LOCK();
    out->syncing = s_running && strcmp(s_busy_folder, folder) == 0;
    UNLOCK();
    int64_t total = s_dl_total, done = s_dl_done;
    out->progress = out->syncing && total > 0 ? (int)(done * 100 / total) : -1;
    if (out->syncing) {
        out->dl_index = s_dl_index;
        out->dl_count = s_dl_count;
    }
    return true;
}
