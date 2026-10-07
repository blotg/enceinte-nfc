#include "mpd_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "changes.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "media_info.h"
#include "mpd_proto.h"
#include "player.h"
#include "sdkconfig.h"
#include "settings.h"
#include "storage.h"
#include "util.h"

static const char *TAG = "mpd";

#define MPD_GREETING "OK MPD 0.23.5\n"
#define MAX_CLIENTS 3
#define LINE_MAX_LEN 2048
#define OUT_FLUSH 8192
#define LIST_MAX_BYTES 65536
#define INACTIVITY_S 600
#define CACHE_SIZE 48
#define MAX_WALK_DEPTH 6

enum {
    ACK_ERROR_NOT_LIST = 1,
    ACK_ERROR_ARG = 2,
    ACK_ERROR_PASSWORD = 3,
    ACK_ERROR_PERMISSION = 4,
    ACK_ERROR_UNKNOWN = 5,
    ACK_ERROR_NO_EXIST = 50,
    ACK_ERROR_SYSTEM = 52,
};

typedef struct {
    int fd;
    bool authed;
    bool closing;
    char *out;
    size_t out_len, out_cap;
    int err_code;
    char err_msg[160];
    uint32_t seen[CHG_COUNT];
    uint32_t tag_mask;
    size_t binary_limit;
    char in[LINE_MAX_LEN];
    size_t in_len;
    char line[LINE_MAX_LEN];
} client_t;

static volatile int s_clients;
static int64_t s_start_us;

/* ======================= Sortie ======================= */

static void flush_out(client_t *c)
{
    size_t sent = 0;
    while (!c->closing && sent < c->out_len) {
        int n = send(c->fd, c->out + sent, c->out_len - sent, 0);
        if (n <= 0) {
            c->closing = true;
            break;
        }
        sent += n;
    }
    c->out_len = 0;
}

static void out_write(client_t *c, const char *s, size_t n)
{
    if (c->out_len + n > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 1024;
        while (cap < c->out_len + n) {
            cap *= 2;
        }
        char *nb = realloc(c->out, cap);
        if (!nb) {
            flush_out(c);
            if (n > c->out_cap) {
                return;
            }
        } else {
            c->out = nb;
            c->out_cap = cap;
        }
    }
    memcpy(c->out + c->out_len, s, n);
    c->out_len += n;
    if (c->out_len >= OUT_FLUSH) {
        flush_out(c);
    }
}

static void outs(client_t *c, const char *s)
{
    out_write(c, s, strlen(s));
}

static void outf(client_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(client_t *c, const char *fmt, ...)
{
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n < sizeof(buf)) {
        out_write(c, buf, n);
        return;
    }
    char *big = malloc(n + 1);
    if (!big) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, n + 1, fmt, ap);
    va_end(ap);
    out_write(c, big, n);
    free(big);
}

/* "Nom: valeur" en neutralisant les retours à la ligne. */
static void out_pair(client_t *c, const char *name, const char *value)
{
    outs(c, name);
    outs(c, ": ");
    for (const char *p = value; *p; p++) {
        char ch = (*p == '\n' || *p == '\r') ? ' ' : *p;
        out_write(c, &ch, 1);
    }
    outs(c, "\n");
}

static void out_mtime(client_t *c, time_t t)
{
    if (t <= 0) {
        return;
    }
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    outf(c, "Last-Modified: %s\n", buf);
}

static int fail(client_t *c, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(client_t *c, int code, const char *fmt, ...)
{
    c->err_code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err_msg, sizeof(c->err_msg), fmt, ap);
    va_end(ap);
    return -1;
}

/* ======================= Cache des métadonnées ======================= */

typedef struct {
    char *tags[MPD_TAG_COUNT];
    uint32_t duration_ms;
    time_t mtime;
    const char *file; /* non possédé */
} song_meta_t;

typedef struct {
    char *path;
    song_meta_t meta;
} cache_entry_t;

static cache_entry_t s_cache[CACHE_SIZE];
static int s_cache_next;
static uint32_t s_cache_db_version;
static SemaphoreHandle_t s_cache_lock;

static void meta_free(song_meta_t *m)
{
    for (int i = 0; i < MPD_TAG_COUNT; i++) {
        free(m->tags[i]);
        m->tags[i] = NULL;
    }
}

static void meta_copy(song_meta_t *dst, const song_meta_t *src)
{
    for (int i = 0; i < MPD_TAG_COUNT; i++) {
        dst->tags[i] = src->tags[i] ? strdup(src->tags[i]) : NULL;
    }
    dst->duration_ms = src->duration_ms;
    dst->mtime = src->mtime;
}

static void cache_check_version_locked(void)
{
    uint32_t snap[CHG_COUNT];
    changes_snapshot(snap);
    if (snap[CHG_DATABASE] != s_cache_db_version) {
        for (int i = 0; i < CACHE_SIZE; i++) {
            free(s_cache[i].path);
            s_cache[i].path = NULL;
            meta_free(&s_cache[i].meta);
        }
        s_cache_db_version = snap[CHG_DATABASE];
    }
}

static char *dup_nonempty(const char *s)
{
    return s && *s ? strdup(s) : NULL;
}

static bool load_meta(const char *rel, song_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    m->file = rel;
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    cache_check_version_locked();
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].path && strcmp(s_cache[i].path, rel) == 0) {
            meta_copy(m, &s_cache[i].meta);
            xSemaphoreGive(s_cache_lock);
            return true;
        }
    }
    xSemaphoreGive(s_cache_lock);

    char abs[ABS_PATH_MAX];
    if (!path_to_abs(rel, abs, sizeof(abs))) {
        return false;
    }
    struct stat st;
    if (stat(abs, &st) != 0) {
        return false;
    }
    m->mtime = st.st_mtime;
    FILE *f = fopen(abs, "rb");
    media_info_t *mi = malloc(sizeof(media_info_t));
    if (f && mi && media_probe(f, audio_fmt_from_name(rel), mi)) {
        m->tags[MPD_TAG_ARTIST] = dup_nonempty(mi->artist);
        m->tags[MPD_TAG_ALBUMARTIST] = dup_nonempty(mi->album_artist);
        m->tags[MPD_TAG_ALBUM] = dup_nonempty(mi->album);
        m->tags[MPD_TAG_TITLE] = dup_nonempty(mi->title);
        m->tags[MPD_TAG_TRACK] = dup_nonempty(mi->track);
        m->tags[MPD_TAG_GENRE] = dup_nonempty(mi->genre);
        m->tags[MPD_TAG_DATE] = dup_nonempty(mi->date);
        m->duration_ms = mi->duration_ms;
    }
    free(mi);
    if (f) {
        fclose(f);
    }

    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    cache_entry_t *e = &s_cache[s_cache_next];
    s_cache_next = (s_cache_next + 1) % CACHE_SIZE;
    free(e->path);
    meta_free(&e->meta);
    e->path = strdup(rel);
    if (e->path) {
        meta_copy(&e->meta, m);
    }
    xSemaphoreGive(s_cache_lock);
    return true;
}

static const char *meta_get(void *ctx, int tag)
{
    song_meta_t *m = ctx;
    if (tag == MPD_TAG_FILE) {
        return m->file;
    }
    return tag >= 0 && tag < MPD_TAG_COUNT ? m->tags[tag] : NULL;
}

static void out_meta(client_t *c, const song_meta_t *m)
{
    out_mtime(c, m->mtime);
    for (int t = 0; t < MPD_TAG_COUNT; t++) {
        if (m->tags[t] && (c->tag_mask & (1u << t))) {
            out_pair(c, mpd_tag_names[t], m->tags[t]);
        }
    }
    if (m->duration_ms) {
        outf(c, "Time: %u\nduration: %.3f\n", (unsigned)((m->duration_ms + 500) / 1000), m->duration_ms / 1000.0);
    }
}

static void out_song(client_t *c, const char *rel, int pos, uint32_t id)
{
    out_pair(c, "file", rel);
    song_meta_t m;
    if (load_meta(rel, &m)) {
        out_meta(c, &m);
        meta_free(&m);
    }
    if (pos >= 0) {
        outf(c, "Pos: %d\nId: %u\n", pos, (unsigned)id);
    }
}

/* ======================= Outils ======================= */

static bool uri_arg(client_t *c, const char *in, char *rel)
{
    if (strstr(in, "://")) {
        fail(c, ACK_ERROR_NO_EXIST, "Unsupported URI scheme");
        return false;
    }
    if (!path_sanitize(in, rel, REL_PATH_MAX)) {
        fail(c, ACK_ERROR_ARG, "Malformed URI");
        return false;
    }
    return true;
}

static bool need_storage(client_t *c)
{
    if (!storage_is_mounted()) {
        fail(c, ACK_ERROR_SYSTEM, "SD card not available");
        return false;
    }
    return true;
}

static bool is_audio_path(const char *rel)
{
    return rel[0] && is_audio_file(path_basename(rel)) && storage_exists(rel) && !storage_is_dir(rel);
}

static const char *join(char *buf, size_t len, const char *dir, const char *name)
{
    int n = dir[0] ? snprintf(buf, len, "%s/%s", dir, name) : snprintf(buf, len, "%s", name);
    return (n > 0 && (size_t)n < len) ? buf : NULL;
}

typedef enum { WALK_LISTALL, WALK_LISTALLINFO } walk_mode_t;

static void walk(client_t *c, const char *rel, walk_mode_t mode, int depth)
{
    dir_entry_t *entries;
    int n;
    if (depth > MAX_WALK_DEPTH || c->closing || storage_list_dir(rel, &entries, &n) != ESP_OK) {
        return;
    }
    char *child = malloc(REL_PATH_MAX);
    for (int i = 0; child && i < n && !c->closing; i++) {
        if (!join(child, REL_PATH_MAX, rel, entries[i].name)) {
            continue;
        }
        if (entries[i].is_dir) {
            out_pair(c, "directory", child);
            if (mode == WALK_LISTALLINFO) {
                out_mtime(c, entries[i].mtime);
            }
            walk(c, child, mode, depth + 1);
        } else if (is_audio_file(entries[i].name)) {
            if (mode == WALK_LISTALLINFO) {
                out_song(c, child, -1, 0);
            } else {
                out_pair(c, "file", child);
            }
        }
    }
    free(child);
    storage_free_dir(entries, n);
}

/* Applique fn à chaque morceau de la carte (ou d'un sous-dossier) correspondant au filtre. */
typedef void (*song_fn_t)(client_t *c, const char *rel, song_meta_t *m, void *ctx);

static int for_each_song(client_t *c, const char *base, const mpd_filter_t *flt, song_fn_t fn, void *ctx)
{
    path_list_t list;
    if (storage_list_tracks(base, &list) != ESP_OK) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such directory");
    }
    for (int i = 0; i < list.count && !c->closing; i++) {
        song_meta_t m;
        if (!load_meta(list.items[i], &m)) {
            continue;
        }
        if (mpd_filter_match(flt, meta_get, &m)) {
            fn(c, list.items[i], &m, ctx);
        }
        meta_free(&m);
    }
    path_list_free(&list);
    return 0;
}

static int queue_range(client_t *c, int argc, char **argv, int idx, int *start, int *end)
{
    int len = player_queue_length();
    *start = 0;
    *end = len;
    if (argc > idx) {
        if (!mpd_parse_range(argv[idx], start, end)) {
            return fail(c, ACK_ERROR_ARG, "Bad song index");
        }
        if (*end < 0 || *end > len) {
            *end = len;
        }
        if (*start > len || (*start == len && argc > idx && !strchr(argv[idx], ':'))) {
            return fail(c, ACK_ERROR_ARG, "Bad song index");
        }
    }
    return 0;
}

static int add_path(client_t *c, const char *rel, int pos, uint32_t *id_out)
{
    if (storage_is_dir(rel)) {
        path_list_t list;
        if (storage_list_tracks(rel, &list) != ESP_OK) {
            return fail(c, ACK_ERROR_NO_EXIST, "No such directory");
        }
        for (int i = 0; i < list.count; i++) {
            player_queue_add(list.items[i], pos >= 0 ? pos + i : -1, NULL);
        }
        path_list_free(&list);
        return 0;
    }
    if (!is_audio_path(rel)) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such song");
    }
    if (player_queue_add(rel, pos, id_out) != ESP_OK) {
        return fail(c, ACK_ERROR_ARG, "Bad song index");
    }
    return 0;
}

/* ======================= Commandes : statut ======================= */

static const char *state_str(player_state_t s)
{
    return s == PLAYER_PLAYING ? "play" : (s == PLAYER_PAUSED ? "pause" : "stop");
}

static int c_status(client_t *c, int argc, char **argv)
{
    player_status_t st;
    player_get_status(&st);
    outf(c, "volume: %d\nrepeat: %d\nrandom: %d\n", st.volume, st.repeat, st.random);
    outs(c, st.single == 2 ? "single: oneshot\n" : (st.single ? "single: 1\n" : "single: 0\n"));
    outf(c, "consume: %d\npartition: default\nplaylist: %u\nplaylistlength: %d\nmixrampdb: 0.000000\nstate: %s\n",
         st.consume, (unsigned)st.queue_version, st.queue_len, state_str(st.state));
    if (st.song >= 0) {
        outf(c, "song: %d\nsongid: %u\n", st.song, (unsigned)st.song_id);
    }
    if (st.next_song >= 0) {
        outf(c, "nextsong: %d\nnextsongid: %u\n", st.next_song, (unsigned)st.next_song_id);
    }
    if (st.state != PLAYER_STOPPED) {
        outf(c, "time: %u:%u\nelapsed: %.3f\n", (unsigned)(st.elapsed_ms / 1000),
             (unsigned)((st.duration_ms + 500) / 1000), st.elapsed_ms / 1000.0);
        if (st.duration_ms) {
            outf(c, "duration: %.3f\n", st.duration_ms / 1000.0);
        }
        outf(c, "bitrate: %u\naudio: %u:%u:%u\n", (unsigned)st.bitrate_kbps, (unsigned)st.sample_rate, st.bits,
             st.channels);
    }
    if (st.error[0]) {
        out_pair(c, "error", st.error);
    }
    return 0;
}

static int c_currentsong(client_t *c, int argc, char **argv)
{
    player_status_t st;
    player_get_status(&st);
    if (st.song >= 0) {
        out_song(c, st.file, st.song, st.song_id);
    }
    return 0;
}

static int c_stats(client_t *c, int argc, char **argv)
{
    uint32_t uptime = (uint32_t)((esp_timer_get_time() - s_start_us) / 1000000);
    outf(c, "artists: 0\nalbums: 0\nsongs: 0\nuptime: %u\nplaytime: 0\ndb_playtime: 0\ndb_update: %lld\n",
         (unsigned)uptime, (long long)time(NULL));
    return 0;
}

static int c_ok(client_t *c, int argc, char **argv)
{
    return 0;
}

static int c_close(client_t *c, int argc, char **argv)
{
    c->closing = true;
    return 0;
}

static int c_password(client_t *c, int argc, char **argv)
{
    if (!settings_check_mpd_password(argv[1])) {
        return fail(c, ACK_ERROR_PASSWORD, "incorrect password");
    }
    c->authed = true;
    return 0;
}

/* ======================= Commandes : lecture ======================= */

static int check_err(client_t *c, esp_err_t err)
{
    if (err == ESP_OK) {
        return 0;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return fail(c, ACK_ERROR_SYSTEM, "Not seekable");
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_FOUND) {
        return fail(c, ACK_ERROR_ARG, "Bad song index");
    }
    return fail(c, ACK_ERROR_SYSTEM, "Player busy");
}

static int c_play(client_t *c, int argc, char **argv)
{
    int pos = -1;
    if (argc > 1 && (!mpd_parse_int(argv[1], &pos) || pos < -1 || pos >= player_queue_length())) {
        return fail(c, ACK_ERROR_ARG, "Bad song index");
    }
    return check_err(c, player_play(pos));
}

static int c_playid(client_t *c, int argc, char **argv)
{
    int id = -1;
    if (argc > 1 && !mpd_parse_int(argv[1], &id)) {
        return fail(c, ACK_ERROR_ARG, "Bad song id");
    }
    if (id < 0) {
        return check_err(c, player_play(-1));
    }
    if (player_play_id((uint32_t)id) != ESP_OK) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such song");
    }
    return 0;
}

static int c_pause(client_t *c, int argc, char **argv)
{
    int mode = -1;
    if (argc > 1 && (!mpd_parse_int(argv[1], &mode) || mode < 0 || mode > 1)) {
        return fail(c, ACK_ERROR_ARG, "Boolean (0/1) expected");
    }
    return check_err(c, player_pause(mode));
}

static int c_stop(client_t *c, int argc, char **argv)
{
    return check_err(c, player_stop());
}

static int c_next(client_t *c, int argc, char **argv)
{
    return check_err(c, player_next());
}

static int c_previous(client_t *c, int argc, char **argv)
{
    return check_err(c, player_previous());
}

static int c_seek(client_t *c, int argc, char **argv)
{
    int pos;
    double t;
    if (!mpd_parse_int(argv[1], &pos) || !mpd_parse_float(argv[2], &t) || t < 0) {
        return fail(c, ACK_ERROR_ARG, "Bad arguments");
    }
    return check_err(c, player_seek(pos, (uint32_t)(t * 1000)));
}

static int c_seekid(client_t *c, int argc, char **argv)
{
    int id;
    double t;
    if (!mpd_parse_int(argv[1], &id) || !mpd_parse_float(argv[2], &t) || t < 0) {
        return fail(c, ACK_ERROR_ARG, "Bad arguments");
    }
    int pos = player_queue_pos_of_id((uint32_t)id);
    if (pos < 0) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such song");
    }
    return check_err(c, player_seek(pos, (uint32_t)(t * 1000)));
}

static int c_seekcur(client_t *c, int argc, char **argv)
{
    double t;
    if (!mpd_parse_float(argv[1], &t)) {
        return fail(c, ACK_ERROR_ARG, "Bad time");
    }
    player_status_t st;
    player_get_status(&st);
    if (st.state == PLAYER_STOPPED) {
        return fail(c, ACK_ERROR_SYSTEM, "Not playing");
    }
    double target = (argv[1][0] == '+' || argv[1][0] == '-') ? st.elapsed_ms / 1000.0 + t : t;
    if (target < 0) {
        target = 0;
    }
    return check_err(c, player_seek(-1, (uint32_t)(target * 1000)));
}

static int c_setvol(client_t *c, int argc, char **argv)
{
    int v;
    if (!mpd_parse_int(argv[1], &v) || v < 0 || v > 100) {
        return fail(c, ACK_ERROR_ARG, "Invalid volume value");
    }
    player_set_volume(v);
    return 0;
}

static int c_volume(client_t *c, int argc, char **argv)
{
    int d;
    if (!mpd_parse_int(argv[1], &d) || d < -100 || d > 100) {
        return fail(c, ACK_ERROR_ARG, "Invalid volume value");
    }
    player_set_volume(player_get_volume() + d);
    return 0;
}

static int c_getvol(client_t *c, int argc, char **argv)
{
    outf(c, "volume: %d\n", player_get_volume());
    return 0;
}

static int parse_bool_arg(client_t *c, const char *s, bool *out)
{
    if (strcmp(s, "0") == 0 || strcmp(s, "1") == 0) {
        *out = s[0] == '1';
        return 0;
    }
    return fail(c, ACK_ERROR_ARG, "Boolean (0/1) expected: %s", s);
}

static int c_repeat(client_t *c, int argc, char **argv)
{
    bool b;
    if (parse_bool_arg(c, argv[1], &b)) {
        return -1;
    }
    player_set_repeat(b);
    return 0;
}

static int c_random(client_t *c, int argc, char **argv)
{
    bool b;
    if (parse_bool_arg(c, argv[1], &b)) {
        return -1;
    }
    player_set_random(b);
    return 0;
}

static int c_single(client_t *c, int argc, char **argv)
{
    if (strcmp(argv[1], "oneshot") == 0) {
        player_set_single(2);
        return 0;
    }
    bool b;
    if (parse_bool_arg(c, argv[1], &b)) {
        return -1;
    }
    player_set_single(b ? 1 : 0);
    return 0;
}

static int c_consume(client_t *c, int argc, char **argv)
{
    if (strcmp(argv[1], "oneshot") == 0) {
        player_set_consume(true);
        return 0;
    }
    bool b;
    if (parse_bool_arg(c, argv[1], &b)) {
        return -1;
    }
    player_set_consume(b);
    return 0;
}

static int c_clearerror(client_t *c, int argc, char **argv)
{
    player_clear_error();
    return 0;
}

static int c_replay_gain_status(client_t *c, int argc, char **argv)
{
    outs(c, "replay_gain_mode: off\n");
    return 0;
}

/* ======================= Commandes : file d'attente ======================= */

static int c_add(client_t *c, int argc, char **argv)
{
    char rel[REL_PATH_MAX];
    int pos = -1;
    if (!uri_arg(c, argv[1], rel) || !need_storage(c)) {
        return -1;
    }
    if (argc > 2 && (!mpd_parse_int(argv[2], &pos) || pos < 0 || pos > player_queue_length())) {
        return fail(c, ACK_ERROR_ARG, "Bad position");
    }
    return add_path(c, rel, pos, NULL);
}

static int c_addid(client_t *c, int argc, char **argv)
{
    char rel[REL_PATH_MAX];
    int pos = -1;
    if (!uri_arg(c, argv[1], rel) || !need_storage(c)) {
        return -1;
    }
    if (argc > 2 && (!mpd_parse_int(argv[2], &pos) || pos < 0 || pos > player_queue_length())) {
        return fail(c, ACK_ERROR_ARG, "Bad position");
    }
    if (storage_is_dir(rel)) {
        return fail(c, ACK_ERROR_ARG, "Directory not allowed");
    }
    uint32_t id = 0;
    if (add_path(c, rel, pos, &id)) {
        return -1;
    }
    outf(c, "Id: %u\n", (unsigned)id);
    return 0;
}

static int c_clear(client_t *c, int argc, char **argv)
{
    player_queue_clear();
    return 0;
}

static int c_delete(client_t *c, int argc, char **argv)
{
    int start, end;
    if (!mpd_parse_range(argv[1], &start, &end)) {
        return fail(c, ACK_ERROR_ARG, "Bad song index");
    }
    if (end < 0) {
        end = player_queue_length();
    }
    return check_err(c, player_queue_delete(start, end));
}

static int c_deleteid(client_t *c, int argc, char **argv)
{
    int id;
    if (!mpd_parse_int(argv[1], &id)) {
        return fail(c, ACK_ERROR_ARG, "Bad song id");
    }
    int pos = player_queue_pos_of_id((uint32_t)id);
    if (pos < 0) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such song");
    }
    return check_err(c, player_queue_delete(pos, pos + 1));
}

static int c_move(client_t *c, int argc, char **argv)
{
    int start, end, to;
    if (!mpd_parse_range(argv[1], &start, &end) || !mpd_parse_int(argv[2], &to)) {
        return fail(c, ACK_ERROR_ARG, "Bad song index");
    }
    if (end < 0) {
        end = player_queue_length();
    }
    return check_err(c, player_queue_move(start, end, to));
}

static int c_moveid(client_t *c, int argc, char **argv)
{
    int id, to;
    if (!mpd_parse_int(argv[1], &id) || !mpd_parse_int(argv[2], &to)) {
        return fail(c, ACK_ERROR_ARG, "Bad song id");
    }
    int pos = player_queue_pos_of_id((uint32_t)id);
    if (pos < 0) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such song");
    }
    return check_err(c, player_queue_move(pos, pos + 1, to));
}

static int swap_pos(client_t *c, int a, int b)
{
    int len = player_queue_length();
    if (a < 0 || b < 0 || a >= len || b >= len) {
        return fail(c, ACK_ERROR_ARG, "Bad song index");
    }
    if (a == b) {
        return 0;
    }
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    player_queue_move(b, b + 1, a);     /* b passe en a, l'ancien a glisse en a+1 */
    player_queue_move(a + 1, a + 2, b); /* l'ancien a rejoint b */
    return 0;
}

static int c_swap(client_t *c, int argc, char **argv)
{
    int a, b;
    if (!mpd_parse_int(argv[1], &a) || !mpd_parse_int(argv[2], &b)) {
        return fail(c, ACK_ERROR_ARG, "Bad song index");
    }
    return swap_pos(c, a, b);
}

static int c_swapid(client_t *c, int argc, char **argv)
{
    int a, b;
    if (!mpd_parse_int(argv[1], &a) || !mpd_parse_int(argv[2], &b)) {
        return fail(c, ACK_ERROR_ARG, "Bad song id");
    }
    return swap_pos(c, player_queue_pos_of_id((uint32_t)a), player_queue_pos_of_id((uint32_t)b));
}

static int c_shuffle(client_t *c, int argc, char **argv)
{
    player_queue_shuffle();
    return 0;
}

static int c_playlistinfo(client_t *c, int argc, char **argv)
{
    int start, end;
    if (queue_range(c, argc, argv, 1, &start, &end)) {
        return -1;
    }
    queue_item_t *it = malloc(sizeof(queue_item_t));
    for (int i = start; it && i < end && !c->closing; i++) {
        if (player_queue_get(i, it)) {
            out_song(c, it->path, i, it->id);
        }
    }
    free(it);
    return 0;
}

static int c_playlistid(client_t *c, int argc, char **argv)
{
    if (argc < 2) {
        return c_playlistinfo(c, 1, argv);
    }
    int id;
    if (!mpd_parse_int(argv[1], &id)) {
        return fail(c, ACK_ERROR_ARG, "Bad song id");
    }
    int pos = player_queue_pos_of_id((uint32_t)id);
    queue_item_t *it = malloc(sizeof(queue_item_t));
    bool ok = it && pos >= 0 && player_queue_get(pos, it);
    if (ok) {
        out_song(c, it->path, pos, it->id);
    }
    free(it);
    return ok ? 0 : fail(c, ACK_ERROR_NO_EXIST, "No such song");
}

static int c_plchanges(client_t *c, int argc, char **argv)
{
    /* Sans historique des modifications, on renvoie toute la file : c'est un sur-ensemble valide. */
    return c_playlistinfo(c, argc > 2 ? 2 : 1, argc > 2 ? argv + 1 : argv);
}

static int c_plchangesposid(client_t *c, int argc, char **argv)
{
    queue_item_t *it = malloc(sizeof(queue_item_t));
    int len = player_queue_length();
    for (int i = 0; it && i < len; i++) {
        if (player_queue_get(i, it)) {
            outf(c, "cpos: %d\nId: %u\n", i, (unsigned)it->id);
        }
    }
    free(it);
    return 0;
}

static int c_playlist(client_t *c, int argc, char **argv)
{
    queue_item_t *it = malloc(sizeof(queue_item_t));
    int len = player_queue_length();
    for (int i = 0; it && i < len; i++) {
        if (player_queue_get(i, it)) {
            outf(c, "%d:file: %s\n", i, it->path);
        }
    }
    free(it);
    return 0;
}

static int queue_filter(client_t *c, int argc, char **argv, bool search)
{
    char err[96] = "";
    int used = 0;
    mpd_filter_t *flt = mpd_filter_parse_args(argc - 1, argv + 1, search, &used, err, sizeof(err));
    if (!flt) {
        return fail(c, ACK_ERROR_ARG, "%s", err[0] ? err : "Bad filter");
    }
    queue_item_t *it = malloc(sizeof(queue_item_t));
    int len = player_queue_length();
    for (int i = 0; it && i < len && !c->closing; i++) {
        if (!player_queue_get(i, it)) {
            continue;
        }
        song_meta_t m;
        if (load_meta(it->path, &m)) {
            m.file = it->path;
            if (mpd_filter_match(flt, meta_get, &m)) {
                out_song(c, it->path, i, it->id);
            }
            meta_free(&m);
        }
    }
    free(it);
    mpd_filter_free(flt);
    return 0;
}

static int c_playlistfind(client_t *c, int argc, char **argv)
{
    return queue_filter(c, argc, argv, false);
}

static int c_playlistsearch(client_t *c, int argc, char **argv)
{
    return queue_filter(c, argc, argv, true);
}

/* ======================= Commandes : bibliothèque ======================= */

static int c_lsinfo(client_t *c, int argc, char **argv)
{
    char rel[REL_PATH_MAX] = "";
    if ((argc > 1 && !uri_arg(c, argv[1], rel)) || !need_storage(c)) {
        return -1;
    }
    if (rel[0] && !storage_is_dir(rel)) {
        if (!is_audio_path(rel)) {
            return fail(c, ACK_ERROR_NO_EXIST, "No such directory");
        }
        out_song(c, rel, -1, 0);
        return 0;
    }
    dir_entry_t *entries;
    int n;
    if (storage_list_dir(rel, &entries, &n) != ESP_OK) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such directory");
    }
    char *child = malloc(REL_PATH_MAX);
    for (int i = 0; child && i < n && !c->closing; i++) {
        if (!join(child, REL_PATH_MAX, rel, entries[i].name)) {
            continue;
        }
        if (entries[i].is_dir) {
            out_pair(c, "directory", child);
            out_mtime(c, entries[i].mtime);
        } else if (is_audio_file(entries[i].name)) {
            out_song(c, child, -1, 0);
        }
    }
    free(child);
    storage_free_dir(entries, n);
    return 0;
}

static int c_listfiles(client_t *c, int argc, char **argv)
{
    char rel[REL_PATH_MAX] = "";
    if ((argc > 1 && !uri_arg(c, argv[1], rel)) || !need_storage(c)) {
        return -1;
    }
    dir_entry_t *entries;
    int n;
    if (storage_list_dir(rel, &entries, &n) != ESP_OK) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such directory");
    }
    for (int i = 0; i < n; i++) {
        out_pair(c, entries[i].is_dir ? "directory" : "file", entries[i].name);
        if (!entries[i].is_dir) {
            outf(c, "size: %u\n", (unsigned)entries[i].size);
        }
        out_mtime(c, entries[i].mtime);
    }
    storage_free_dir(entries, n);
    return 0;
}

static int c_listall(client_t *c, int argc, char **argv)
{
    char rel[REL_PATH_MAX] = "";
    if ((argc > 1 && !uri_arg(c, argv[1], rel)) || !need_storage(c)) {
        return -1;
    }
    if (rel[0] && !storage_is_dir(rel)) {
        if (!is_audio_path(rel)) {
            return fail(c, ACK_ERROR_NO_EXIST, "No such directory");
        }
        out_pair(c, "file", rel);
        return 0;
    }
    walk(c, rel, strcmp(argv[0], "listallinfo") == 0 ? WALK_LISTALLINFO : WALK_LISTALL, 0);
    return 0;
}

typedef struct {
    int window_start, window_end, index;
    bool add;
} find_ctx_t;

static void find_cb(client_t *c, const char *rel, song_meta_t *m, void *ctx)
{
    find_ctx_t *fc = ctx;
    int i = fc->index++;
    if (i < fc->window_start || (fc->window_end >= 0 && i >= fc->window_end)) {
        return;
    }
    if (fc->add) {
        player_queue_add(rel, -1, NULL);
        return;
    }
    out_pair(c, "file", rel);
    out_meta(c, m);
}

static int find_common(client_t *c, int argc, char **argv, bool search, bool add)
{
    if (!need_storage(c)) {
        return -1;
    }
    char err[96] = "";
    int used = 0;
    mpd_filter_t *flt = mpd_filter_parse_args(argc - 1, argv + 1, search, &used, err, sizeof(err));
    if (!flt) {
        return fail(c, ACK_ERROR_ARG, "%s", err[0] ? err : "Bad filter");
    }
    find_ctx_t fc = {.window_start = 0, .window_end = -1, .add = add};
    for (int i = 1 + used; i + 1 < argc; i += 2) {
        if (strcasecmp(argv[i], "window") == 0 && !mpd_parse_range(argv[i + 1], &fc.window_start, &fc.window_end)) {
            mpd_filter_free(flt);
            return fail(c, ACK_ERROR_ARG, "Bad window");
        }
    }
    int r = for_each_song(c, "", flt, find_cb, &fc);
    mpd_filter_free(flt);
    return r;
}

static int c_find(client_t *c, int argc, char **argv)
{
    return find_common(c, argc, argv, false, false);
}

static int c_search(client_t *c, int argc, char **argv)
{
    return find_common(c, argc, argv, true, false);
}

static int c_findadd(client_t *c, int argc, char **argv)
{
    return find_common(c, argc, argv, false, true);
}

static int c_searchadd(client_t *c, int argc, char **argv)
{
    return find_common(c, argc, argv, true, true);
}

typedef struct {
    int tag;
    char **values;
    int count, cap;
    uint32_t songs, playtime_ms;
} list_ctx_t;

static void list_cb(client_t *c, const char *rel, song_meta_t *m, void *ctx)
{
    list_ctx_t *lc = ctx;
    lc->songs++;
    lc->playtime_ms += m->duration_ms;
    const char *v = lc->tag == MPD_TAG_FILE ? rel : (lc->tag >= 0 && lc->tag < MPD_TAG_COUNT ? m->tags[lc->tag] : NULL);
    if (!v || !*v) {
        return;
    }
    for (int i = 0; i < lc->count; i++) {
        if (strcmp(lc->values[i], v) == 0) {
            return;
        }
    }
    if (lc->count == lc->cap) {
        int cap = lc->cap ? lc->cap * 2 : 32;
        char **nv = realloc(lc->values, cap * sizeof(char *));
        if (!nv) {
            return;
        }
        lc->values = nv;
        lc->cap = cap;
    }
    lc->values[lc->count] = strdup(v);
    if (lc->values[lc->count]) {
        lc->count++;
    }
}

static int str_ptr_cmp(const void *a, const void *b)
{
    return natural_casecmp(*(char *const *)a, *(char *const *)b);
}

static int c_list(client_t *c, int argc, char **argv)
{
    if (!need_storage(c)) {
        return -1;
    }
    int tag = mpd_tag_from_name(argv[1]);
    if (tag < 0 || tag == MPD_TAG_ANY) {
        return fail(c, ACK_ERROR_ARG, "Unknown tag type: %s", argv[1]);
    }
    mpd_filter_t *flt = NULL;
    char err[96] = "";
    if (argc == 3 && tag == MPD_TAG_ALBUM && argv[2][0] != '(') {
        char *pair[2] = {"artist", argv[2]}; /* ancienne syntaxe "list album ARTISTE" */
        flt = mpd_filter_parse_pairs(2, pair, false, err, sizeof(err));
    } else if (argc > 2) {
        int used = 0;
        flt = mpd_filter_parse_args(argc - 2, argv + 2, false, &used, err, sizeof(err));
        if (!flt && used > 0) {
            return fail(c, ACK_ERROR_ARG, "%s", err[0] ? err : "Bad filter");
        }
    }
    list_ctx_t lc = {.tag = tag};
    int r = for_each_song(c, "", flt, list_cb, &lc);
    mpd_filter_free(flt);
    if (lc.values) {
        qsort(lc.values, lc.count, sizeof(char *), str_ptr_cmp);
    }
    for (int i = 0; i < lc.count; i++) {
        if (r == 0) {
            out_pair(c, tag == MPD_TAG_FILE ? "file" : mpd_tag_names[tag], lc.values[i]);
        }
        free(lc.values[i]);
    }
    free(lc.values);
    return r;
}

static int c_count(client_t *c, int argc, char **argv)
{
    if (!need_storage(c)) {
        return -1;
    }
    mpd_filter_t *flt = NULL;
    if (argc > 1) {
        char err[96] = "";
        int used = 0;
        flt = mpd_filter_parse_args(argc - 1, argv + 1, false, &used, err, sizeof(err));
        if (!flt && used > 0) {
            return fail(c, ACK_ERROR_ARG, "%s", err[0] ? err : "Bad filter");
        }
    }
    list_ctx_t lc = {.tag = -1};
    int r = for_each_song(c, "", flt, list_cb, &lc);
    mpd_filter_free(flt);
    if (r == 0) {
        outf(c, "songs: %u\nplaytime: %u\n", (unsigned)lc.songs, (unsigned)(lc.playtime_ms / 1000));
    }
    return r;
}

static int c_update(client_t *c, int argc, char **argv)
{
    changes_notify(CHG_UPDATE);
    changes_notify(CHG_DATABASE);
    outs(c, "updating_db: 1\n");
    return 0;
}

/* ---- Listes de lecture = dossiers de premier niveau (lecture seule) ---- */

static int c_listplaylists(client_t *c, int argc, char **argv)
{
    if (!need_storage(c)) {
        return -1;
    }
    dir_entry_t *entries;
    int n;
    if (storage_list_dir("", &entries, &n) != ESP_OK) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) {
            out_pair(c, "playlist", entries[i].name);
            out_mtime(c, entries[i].mtime);
        }
    }
    storage_free_dir(entries, n);
    return 0;
}

static int playlist_tracks(client_t *c, const char *name, path_list_t *list)
{
    char rel[REL_PATH_MAX];
    if (!uri_arg(c, name, rel) || !need_storage(c)) {
        return -1;
    }
    if (!rel[0] || strchr(rel, '/') || !storage_is_dir(rel) || storage_list_tracks(rel, list) != ESP_OK) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such playlist");
    }
    return 0;
}

static int c_listplaylist(client_t *c, int argc, char **argv)
{
    path_list_t list;
    if (playlist_tracks(c, argv[1], &list)) {
        return -1;
    }
    bool info = strcmp(argv[0], "listplaylistinfo") == 0;
    for (int i = 0; i < list.count && !c->closing; i++) {
        if (info) {
            out_song(c, list.items[i], -1, 0);
        } else {
            out_pair(c, "file", list.items[i]);
        }
    }
    path_list_free(&list);
    return 0;
}

static int c_load(client_t *c, int argc, char **argv)
{
    path_list_t list;
    if (playlist_tracks(c, argv[1], &list)) {
        return -1;
    }
    int start = 0, end = list.count, pos = -1;
    if (argc > 2 && !mpd_parse_range(argv[2], &start, &end)) {
        path_list_free(&list);
        return fail(c, ACK_ERROR_ARG, "Bad range");
    }
    if (end < 0 || end > list.count) {
        end = list.count;
    }
    if (argc > 3 && (!mpd_parse_int(argv[3], &pos) || pos < 0 || pos > player_queue_length())) {
        path_list_free(&list);
        return fail(c, ACK_ERROR_ARG, "Bad position");
    }
    for (int i = start; i < end; i++) {
        player_queue_add(list.items[i], pos >= 0 ? pos + (i - start) : -1, NULL);
    }
    path_list_free(&list);
    return 0;
}

static int c_readonly(client_t *c, int argc, char **argv)
{
    return fail(c, ACK_ERROR_PERMISSION, "Playlists are read-only (SD card folders)");
}

/* ---- Pochettes ---- */

static int c_albumart(client_t *c, int argc, char **argv)
{
    static const char *const names[] = {"cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg", "front.png"};
    char rel[REL_PATH_MAX], dir[REL_PATH_MAX], img[REL_PATH_MAX], abs[ABS_PATH_MAX];
    int offset;
    if (!uri_arg(c, argv[1], rel) || !need_storage(c)) {
        return -1;
    }
    if (!mpd_parse_int(argv[2], &offset) || offset < 0) {
        return fail(c, ACK_ERROR_ARG, "Bad offset");
    }
    if (storage_is_dir(rel)) {
        str_copy(dir, rel, sizeof(dir));
    } else {
        path_dirname(rel, dir, sizeof(dir));
    }
    dir_entry_t *entries;
    int n;
    bool found = false;
    if (storage_list_dir(dir, &entries, &n) == ESP_OK) {
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]) && !found; k++) {
            for (int i = 0; i < n && !found; i++) {
                if (!entries[i].is_dir && strcasecmp(entries[i].name, names[k]) == 0) {
                    found = join(img, sizeof(img), dir, entries[i].name) != NULL;
                }
            }
        }
        storage_free_dir(entries, n);
    }
    FILE *f = found && path_to_abs(img, abs, sizeof(abs)) ? fopen(abs, "rb") : NULL;
    if (!f) {
        return fail(c, ACK_ERROR_NO_EXIST, "No file exists");
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (offset > size) {
        fclose(f);
        return fail(c, ACK_ERROR_ARG, "Bad offset");
    }
    size_t chunk = c->binary_limit;
    if ((long)chunk > size - offset) {
        chunk = size - offset;
    }
    char *buf = malloc(chunk ? chunk : 1);
    size_t got = 0;
    if (buf) {
        fseek(f, offset, SEEK_SET);
        got = fread(buf, 1, chunk, f);
    }
    fclose(f);
    outf(c, "size: %ld\nbinary: %u\n", size, (unsigned)got);
    if (buf) {
        out_write(c, buf, got);
    }
    outs(c, "\n");
    free(buf);
    return 0;
}

static int c_readpicture(client_t *c, int argc, char **argv)
{
    return 0; /* images intégrées non prises en charge : réponse vide = pas d'image */
}

static int c_binarylimit(client_t *c, int argc, char **argv)
{
    int v;
    if (!mpd_parse_int(argv[1], &v) || v < 64) {
        return fail(c, ACK_ERROR_ARG, "Value too small");
    }
    c->binary_limit = v > 65536 ? 65536 : (size_t)v;
    return 0;
}

/* ======================= Commandes : divers ======================= */

static int c_outputs(client_t *c, int argc, char **argv)
{
    outs(c, "outputid: 0\noutputname: Enceinte\nplugin: i2s\noutputenabled: 1\n");
    return 0;
}

static int c_output_ctl(client_t *c, int argc, char **argv)
{
    int id;
    if (!mpd_parse_int(argv[1], &id) || id != 0) {
        return fail(c, ACK_ERROR_NO_EXIST, "No such audio output");
    }
    return 0;
}

static int c_decoders(client_t *c, int argc, char **argv)
{
    outs(c, "plugin: esp_audio_codec\nsuffix: mp3\nsuffix: aac\nsuffix: m4a\nsuffix: flac\nsuffix: wav\n"
            "suffix: ogg\nsuffix: opus\nmime_type: audio/mpeg\nmime_type: audio/flac\nmime_type: audio/ogg\n");
    return 0;
}

static int c_tagtypes(client_t *c, int argc, char **argv)
{
    if (argc == 1) {
        for (int t = 0; t < MPD_TAG_COUNT; t++) {
            if (c->tag_mask & (1u << t)) {
                outf(c, "tagtype: %s\n", mpd_tag_names[t]);
            }
        }
        return 0;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "clear") == 0) {
        c->tag_mask = 0;
    } else if (strcmp(sub, "all") == 0 || strcmp(sub, "reset") == 0) {
        c->tag_mask = (1u << MPD_TAG_COUNT) - 1;
    } else if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
        for (int i = 2; i < argc; i++) {
            int t = mpd_tag_from_name(argv[i]);
            if (t >= 0 && t < MPD_TAG_COUNT) {
                if (sub[0] == 'e') {
                    c->tag_mask |= 1u << t;
                } else {
                    c->tag_mask &= ~(1u << t);
                }
            }
        }
    } else {
        return fail(c, ACK_ERROR_ARG, "Unknown sub command");
    }
    return 0;
}

static int c_listpartitions(client_t *c, int argc, char **argv)
{
    outs(c, "partition: default\n");
    return 0;
}

static int c_config(client_t *c, int argc, char **argv)
{
    return fail(c, ACK_ERROR_PERMISSION, "you don't have permission for \"config\"");
}

static int c_commands(client_t *c, int argc, char **argv);
static int c_notcommands(client_t *c, int argc, char **argv);

typedef int (*cmd_fn_t)(client_t *c, int argc, char **argv);

typedef struct {
    const char *name;
    cmd_fn_t fn;
    int min_args; /* nom de commande compris */
    int max_args;
    bool open; /* autorisée sans mot de passe */
} cmd_def_t;

static const cmd_def_t s_cmds[] = {
    {"add", c_add, 2, 3, false},
    {"addid", c_addid, 2, 3, false},
    {"albumart", c_albumart, 3, 3, false},
    {"binarylimit", c_binarylimit, 2, 2, true},
    {"channels", c_ok, 1, 1, false},
    {"clear", c_clear, 1, 1, false},
    {"clearerror", c_clearerror, 1, 1, false},
    {"close", c_close, 1, 1, true},
    {"commands", c_commands, 1, 1, true},
    {"config", c_config, 1, 1, false},
    {"consume", c_consume, 2, 2, false},
    {"count", c_count, 1, MPD_MAX_ARGS, false},
    {"crossfade", c_ok, 2, 2, false},
    {"currentsong", c_currentsong, 1, 1, false},
    {"decoders", c_decoders, 1, 1, false},
    {"delete", c_delete, 2, 2, false},
    {"deleteid", c_deleteid, 2, 2, false},
    {"disableoutput", c_output_ctl, 2, 2, false},
    {"enableoutput", c_output_ctl, 2, 2, false},
    {"find", c_find, 2, MPD_MAX_ARGS, false},
    {"findadd", c_findadd, 2, MPD_MAX_ARGS, false},
    {"getvol", c_getvol, 1, 1, false},
    {"list", c_list, 2, MPD_MAX_ARGS, false},
    {"listall", c_listall, 1, 2, false},
    {"listallinfo", c_listall, 1, 2, false},
    {"listfiles", c_listfiles, 1, 2, false},
    {"listpartitions", c_listpartitions, 1, 1, false},
    {"listplaylist", c_listplaylist, 2, 3, false},
    {"listplaylistinfo", c_listplaylist, 2, 3, false},
    {"listplaylists", c_listplaylists, 1, 1, false},
    {"load", c_load, 2, 4, false},
    {"lsinfo", c_lsinfo, 1, 2, false},
    {"mixrampdb", c_ok, 2, 2, false},
    {"mixrampdelay", c_ok, 2, 2, false},
    {"move", c_move, 3, 3, false},
    {"moveid", c_moveid, 3, 3, false},
    {"next", c_next, 1, 1, false},
    {"notcommands", c_notcommands, 1, 1, true},
    {"outputs", c_outputs, 1, 1, false},
    {"password", c_password, 2, 2, true},
    {"pause", c_pause, 1, 2, false},
    {"ping", c_ok, 1, 1, true},
    {"play", c_play, 1, 2, false},
    {"playid", c_playid, 1, 2, false},
    {"playlist", c_playlist, 1, 1, false},
    {"playlistadd", c_readonly, 3, 4, false},
    {"playlistclear", c_readonly, 2, 2, false},
    {"playlistdelete", c_readonly, 3, 3, false},
    {"playlistfind", c_playlistfind, 2, MPD_MAX_ARGS, false},
    {"playlistid", c_playlistid, 1, 2, false},
    {"playlistinfo", c_playlistinfo, 1, 2, false},
    {"playlistmove", c_readonly, 4, 4, false},
    {"playlistsearch", c_playlistsearch, 2, MPD_MAX_ARGS, false},
    {"plchanges", c_plchanges, 2, 3, false},
    {"plchangesposid", c_plchangesposid, 2, 3, false},
    {"previous", c_previous, 1, 1, false},
    {"prio", c_ok, 3, MPD_MAX_ARGS, false},
    {"prioid", c_ok, 3, MPD_MAX_ARGS, false},
    {"random", c_random, 2, 2, false},
    {"rangeid", c_ok, 3, 3, false},
    {"readmessages", c_ok, 1, 1, false},
    {"readpicture", c_readpicture, 3, 3, false},
    {"rename", c_readonly, 3, 3, false},
    {"repeat", c_repeat, 2, 2, false},
    {"replay_gain_mode", c_ok, 2, 2, false},
    {"replay_gain_status", c_replay_gain_status, 1, 1, false},
    {"rescan", c_update, 1, 2, false},
    {"rm", c_readonly, 2, 2, false},
    {"save", c_readonly, 2, 3, false},
    {"search", c_search, 2, MPD_MAX_ARGS, false},
    {"searchadd", c_searchadd, 2, MPD_MAX_ARGS, false},
    {"seek", c_seek, 3, 3, false},
    {"seekcur", c_seekcur, 2, 2, false},
    {"seekid", c_seekid, 3, 3, false},
    {"sendmessage", c_ok, 3, 3, false},
    {"setvol", c_setvol, 2, 2, false},
    {"shuffle", c_shuffle, 1, 2, false},
    {"single", c_single, 2, 2, false},
    {"stats", c_stats, 1, 1, false},
    {"status", c_status, 1, 1, false},
    {"stop", c_stop, 1, 1, false},
    {"subscribe", c_ok, 2, 2, false},
    {"swap", c_swap, 3, 3, false},
    {"swapid", c_swapid, 3, 3, false},
    {"tagtypes", c_tagtypes, 1, MPD_MAX_ARGS, true},
    {"toggleoutput", c_output_ctl, 2, 2, false},
    {"unsubscribe", c_ok, 2, 2, false},
    {"update", c_update, 1, 2, false},
    {"urlhandlers", c_ok, 1, 1, false},
    {"volume", c_volume, 2, 2, false},
};

#define NUM_CMDS (sizeof(s_cmds) / sizeof(s_cmds[0]))

static bool permitted(const client_t *c, const cmd_def_t *d)
{
    if (d->open || c->authed) {
        return true;
    }
    settings_t cfg;
    settings_get(&cfg);
    return !cfg.mpd_pass_set;
}

static int c_commands(client_t *c, int argc, char **argv)
{
    for (size_t i = 0; i < NUM_CMDS; i++) {
        if (permitted(c, &s_cmds[i])) {
            outf(c, "command: %s\n", s_cmds[i].name);
        }
    }
    outs(c, "command: idle\ncommand: noidle\n");
    return 0;
}

static int c_notcommands(client_t *c, int argc, char **argv)
{
    for (size_t i = 0; i < NUM_CMDS; i++) {
        if (!permitted(c, &s_cmds[i])) {
            outf(c, "command: %s\n", s_cmds[i].name);
        }
    }
    return 0;
}

static const cmd_def_t *find_cmd(const char *name)
{
    for (size_t i = 0; i < NUM_CMDS; i++) {
        if (strcmp(s_cmds[i].name, name) == 0) {
            return &s_cmds[i];
        }
    }
    return NULL;
}

/* ======================= Boucle client ======================= */

/* Exécute une ligne. Retourne false et écrit l'ACK en cas d'erreur. */
static bool execute(client_t *c, char *line, int list_index)
{
    char *argv[MPD_MAX_ARGS];
    int argc = mpd_tokenize(line, argv, MPD_MAX_ARGS);
    c->err_code = 0;
    c->err_msg[0] = '\0';
    const char *name = argc > 0 ? argv[0] : "";
    if (argc <= 0) {
        outf(c, "ACK [%d@%d] {} %s\n", ACK_ERROR_UNKNOWN, list_index,
             argc < 0 ? "Invalid argument syntax" : "No command given");
        return false;
    }
    const cmd_def_t *d = find_cmd(name);
    if (!d) {
        outf(c, "ACK [%d@%d] {} unknown command \"%s\"\n", ACK_ERROR_UNKNOWN, list_index, name);
        return false;
    }
    if (!permitted(c, d)) {
        outf(c, "ACK [%d@%d] {%s} you don't have permission for \"%s\"\n", ACK_ERROR_PERMISSION, list_index, name,
             name);
        return false;
    }
    if (argc < d->min_args || argc > d->max_args) {
        outf(c, "ACK [%d@%d] {%s} wrong number of arguments for \"%s\"\n", ACK_ERROR_ARG, list_index, name, name);
        return false;
    }
    if (d->fn(c, argc, argv) != 0) {
        outf(c, "ACK [%d@%d] {%s} %s\n", c->err_code ? c->err_code : ACK_ERROR_SYSTEM, list_index, name,
             c->err_msg[0] ? c->err_msg : "error");
        return false;
    }
    return true;
}

typedef enum { READ_OK, READ_TIMEOUT, READ_CLOSED } read_result_t;

/* Lit une ligne complète dans c->line (sans le \n). */
static read_result_t read_line(client_t *c, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        char *nl = memchr(c->in, '\n', c->in_len);
        if (nl) {
            size_t len = (size_t)(nl - c->in);
            memcpy(c->line, c->in, len);
            c->line[len] = '\0';
            if (len > 0 && c->line[len - 1] == '\r') {
                c->line[len - 1] = '\0';
            }
            c->in_len -= len + 1;
            memmove(c->in, nl + 1, c->in_len);
            return READ_OK;
        }
        if (c->in_len >= sizeof(c->in) - 1) {
            return READ_CLOSED; /* ligne trop longue */
        }
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            return READ_TIMEOUT;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c->fd, &rfds);
        struct timeval tv = {.tv_sec = remaining / 1000000, .tv_usec = remaining % 1000000};
        int r = select(c->fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            return READ_CLOSED;
        }
        if (r == 0) {
            continue;
        }
        int n = recv(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len, 0);
        if (n <= 0) {
            return READ_CLOSED;
        }
        c->in_len += n;
    }
}

static bool report_changes(client_t *c, uint32_t mask)
{
    uint32_t now[CHG_COUNT];
    changes_snapshot(now);
    bool any = false;
    for (int i = 0; i < CHG_COUNT; i++) {
        if ((mask & (1u << i)) && now[i] != c->seen[i]) {
            outf(c, "changed: %s\n", changes_name((change_t)i));
            c->seen[i] = now[i];
            any = true;
        }
    }
    return any;
}

static void do_idle(client_t *c, int argc, char **argv)
{
    uint32_t mask = 0;
    for (int i = 1; i < argc; i++) {
        int s = changes_from_name(argv[i]);
        if (s >= 0) {
            mask |= 1u << s;
        }
    }
    if (!mask) {
        mask = (1u << CHG_COUNT) - 1;
    }
    for (;;) {
        if (report_changes(c, mask)) {
            outs(c, "OK\n");
            return;
        }
        read_result_t r = read_line(c, 100);
        if (r == READ_CLOSED) {
            c->closing = true;
            return;
        }
        if (r == READ_OK) {
            if (strcmp(c->line, "noidle") == 0) {
                report_changes(c, mask);
                outs(c, "OK\n");
            } else {
                c->closing = true; /* seule "noidle" est permise pendant idle */
            }
            return;
        }
    }
}

static void run_command_list(client_t *c, bool ok_mode)
{
    char *buf = NULL;
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        if (read_line(c, INACTIVITY_S * 1000) != READ_OK) {
            c->closing = true;
            free(buf);
            return;
        }
        if (strcmp(c->line, "command_list_end") == 0) {
            break;
        }
        size_t l = strlen(c->line);
        if (overflow || len + l + 1 > LIST_MAX_BYTES) {
            overflow = true;
            continue;
        }
        char *nb = realloc(buf, len + l + 1);
        if (!nb) {
            overflow = true;
            continue;
        }
        buf = nb;
        memcpy(buf + len, c->line, l + 1);
        len += l + 1;
    }
    if (overflow) {
        outf(c, "ACK [%d@0] {} command list too long\n", ACK_ERROR_NOT_LIST);
        free(buf);
        return;
    }
    int index = 0;
    for (size_t p = 0; p < len && !c->closing; index++) {
        char *cmd = buf + p;
        p += strlen(cmd) + 1;
        if (!execute(c, cmd, index)) {
            free(buf);
            return;
        }
        if (ok_mode) {
            outs(c, "list_OK\n");
        }
    }
    free(buf);
    outs(c, "OK\n");
}

static void client_task(void *arg)
{
    client_t *c = arg;
    c->tag_mask = (1u << MPD_TAG_COUNT) - 1;
    c->binary_limit = 8192;
    changes_snapshot(c->seen);
    outs(c, MPD_GREETING);
    flush_out(c);
    while (!c->closing) {
        read_result_t r = read_line(c, INACTIVITY_S * 1000);
        if (r != READ_OK) {
            break;
        }
        if (strcmp(c->line, "command_list_begin") == 0) {
            run_command_list(c, false);
        } else if (strcmp(c->line, "command_list_ok_begin") == 0) {
            run_command_list(c, true);
        } else if (strncmp(c->line, "idle", 4) == 0 && (c->line[4] == '\0' || c->line[4] == ' ')) {
            char *argv[MPD_MAX_ARGS];
            int argc = mpd_tokenize(c->line, argv, MPD_MAX_ARGS);
            do_idle(c, argc > 0 ? argc : 1, argv);
        } else if (strcmp(c->line, "noidle") == 0) {
            /* ignorée hors idle */
        } else if (execute(c, c->line, 0)) {
            if (!c->closing) {
                outs(c, "OK\n");
            }
        }
        flush_out(c);
    }
    close(c->fd);
    free(c->out);
    free(c);
    __atomic_sub_fetch(&s_clients, 1, __ATOMIC_SEQ_CST);
    vTaskDelete(NULL);
}

static void listen_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_ENC_MPD_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ls < 0 || bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 2) != 0) {
        ESP_LOGE(TAG, "port %d indisponible", CONFIG_ENC_MPD_PORT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "serveur MPD prêt sur le port %d", CONFIG_ENC_MPD_PORT);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s_clients >= MAX_CLIENTS) {
            static const char busy[] = "ACK [52@0] {} too many clients\n";
            send(fd, busy, sizeof(busy) - 1, 0);
            close(fd);
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval tv = {.tv_sec = 10};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        client_t *c = calloc(1, sizeof(client_t));
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        __atomic_add_fetch(&s_clients, 1, __ATOMIC_SEQ_CST);
        if (xTaskCreate(client_task, "mpd_client", 5120, c, 5, NULL) != pdPASS) {
            __atomic_sub_fetch(&s_clients, 1, __ATOMIC_SEQ_CST);
            close(fd);
            free(c);
        }
    }
}

esp_err_t mpd_server_start(void)
{
    s_start_us = esp_timer_get_time();
    s_cache_lock = xSemaphoreCreateMutex();
    return xTaskCreate(listen_task, "mpd", 3584, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
