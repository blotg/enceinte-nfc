#include "controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "resume_store.h"
#include "sdkconfig.h"
#include "session.h"
#include "settings.h"
#include "storage.h"

static const char *TAG = "controller";

#define LEARN_TIMEOUT_US (60LL * 1000000)
#define MAX_POINTS 16 /* cartes dont la position est mémorisée */

/*
 * Mémoire permanente : la position est tenue à jour en mémoire vive chaque seconde.
 *  - Pendant la lecture d'un même morceau, seule la position est écrite (une entrée NVS de
 *    32 octets), toutes les 2 s si elle a avancé : une coupure fait perdre au plus ~2 s.
 *  - Le point complet (~600 octets) est écrit au changement de morceau et lors d'un
 *    évènement (retrait, nouvelle carte, fin de playlist), au plus une fois toutes les 10 s
 *    par carte (poses et retraits frénétiques).
 * NVS écrit en journal sur les 64 pages de la partition "cfg" (256 Ko) : en lecture continue,
 * ~5 effacements par page et par jour, pour une endurance de 100 000 cycles (~50 ans 24 h/24).
 */
#define SAVE_MIN_INTERVAL_US (10LL * 1000000)
#define SAVE_PERIOD_US (2LL * 1000000)
#define SAVE_MIN_PROGRESS_MS 1000

typedef enum {
    EV_CARD_ON,
    EV_CARD_OFF,
    EV_QUEUE_END,
    EV_LEARN_START,
    EV_LEARN_CANCEL,
    EV_RENAMED,
} ev_type_t;

typedef struct {
    ev_type_t type;
    char uid[UID_STR_MAX];
    char *from, *to; /* EV_RENAMED (alloués, libérés par la tâche) */
} ev_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_lock; /* protège l'état lu par controller_get_status */
static resume_point_t *s_points;
static int s_live = -1; /* point dont la file est chargée dans le lecteur */
static uint32_t s_card_seq;
static char s_present[UID_STR_MAX];
static char s_learned[UID_STR_MAX];
static char s_unknown[UID_STR_MAX];
static bool s_learning;
static int64_t s_learn_deadline;
/* suivi des écritures en mémoire permanente, par emplacement */
static int64_t s_last_save_us[MAX_POINTS]; /* dernier point complet */
static int64_t s_last_pos_us[MAX_POINTS];  /* dernière position (seule ou dans le point complet) */
static uint32_t s_saved_pos[MAX_POINTS];
static uint32_t s_saved_hash[MAX_POINTS];
static bool s_dirty[MAX_POINTS];
/* mode sommeil transmis au lecteur pour la carte qui joue */
static char s_sleep_uid[UID_STR_MAX];
static uint16_t s_sleep_tracks, s_sleep_minutes;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void post(ev_type_t type, const char *uid)
{
    ev_t ev = {.type = type};
    if (uid) {
        str_copy(ev.uid, uid, sizeof(ev.uid));
    }
    if (xQueueSend(s_q, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "file d'évènements pleine");
    }
}

void controller_on_nfc(bool present, const char *uid)
{
    post(present ? EV_CARD_ON : EV_CARD_OFF, uid);
}

void controller_on_player_event(player_event_t evt)
{
    if (evt == PLAYER_EVT_QUEUE_END) {
        post(EV_QUEUE_END, NULL);
    }
}

void controller_learn_start(void)
{
    post(EV_LEARN_START, NULL);
}

void controller_learn_cancel(void)
{
    post(EV_LEARN_CANCEL, NULL);
}

static sess_player_state_t player_state(void)
{
    player_status_t st;
    player_get_status(&st);
    switch (st.state) {
    case PLAYER_PLAYING:
        return SESS_PLAYER_PLAYING;
    case PLAYER_PAUSED:
        return SESS_PLAYER_PAUSED;
    default:
        return SESS_PLAYER_STOPPED;
    }
}

static resume_policy_t policy_for(const char *uid)
{
    settings_t cfg;
    settings_get(&cfg);
    card_entry_t e;
    if (cards_get(uid, &e)) {
        return session_policy(e.resume_s, e.resume_other, cfg.resume_timeout_s, cfg.resume_after_other);
    }
    return session_policy(CARD_DEFAULT, CARD_DEFAULT, cfg.resume_timeout_s, cfg.resume_after_other);
}

/* Ordre aléatoire : réglage de la carte, sinon réglage général. */
static bool shuffle_for(const char *uid)
{
    settings_t cfg;
    settings_get(&cfg);
    card_entry_t e;
    if (cards_get(uid, &e) && e.shuffle != CARD_DEFAULT) {
        return e.shuffle == 1;
    }
    return cfg.shuffle;
}

/* Normalisation et compression : réglage de la carte qui joue, sinon réglage général
 * (aussi pour la lecture lancée depuis l'interface web ou une application MPD). */
static void apply_sound(const char *uid)
{
    settings_t cfg;
    settings_get(&cfg);
    uint8_t normalize = cfg.normalize, compress = cfg.compress;
    card_entry_t e;
    if (uid && cards_get(uid, &e)) {
        if (e.normalize != CARD_DEFAULT) {
            normalize = (uint8_t)e.normalize;
        }
        if (e.compress != CARD_DEFAULT) {
            compress = (uint8_t)e.compress;
        }
    }
    player_set_sound(normalize, compress);
}

static int find_point(const char *uid)
{
    for (int i = 0; i < MAX_POINTS; i++) {
        if (s_points[i].uid[0] && strcmp(s_points[i].uid, uid) == 0) {
            return i;
        }
    }
    return -1;
}

/* Emplacement pour une carte : le sien, un libre, sinon le plus ancien. */
static int alloc_point(const char *uid)
{
    int i = find_point(uid);
    if (i >= 0) {
        return i;
    }
    int oldest = -1;
    for (i = 0; i < MAX_POINTS; i++) {
        if (!s_points[i].uid[0]) {
            return i;
        }
        if (i != s_live && (oldest < 0 || s_points[i].removed_at_us < s_points[oldest].removed_at_us)) {
            oldest = i;
        }
    }
    return oldest >= 0 ? oldest : 0;
}

static bool point_is_live(int i)
{
    return i >= 0 && i == s_live && s_points[i].queue_version == player_queue_version();
}

/* Mode sommeil de la carte : le décompte part de maintenant (pose de la carte). */
static void apply_sleep(const char *uid)
{
    card_entry_t e;
    uint16_t tracks = 0, minutes = 0;
    if (cards_get(uid, &e)) {
        tracks = e.sleep_tracks;
        minutes = e.sleep_minutes;
    }
    str_copy(s_sleep_uid, uid, sizeof(s_sleep_uid));
    s_sleep_tracks = tracks;
    s_sleep_minutes = minutes;
    player_set_sleep(tracks, (uint32_t)minutes * 60);
    if (tracks) {
        ESP_LOGI(TAG, "carte %s : mode sommeil, pause après %u morceau(x)", uid, (unsigned)tracks);
    } else if (minutes) {
        ESP_LOGI(TAG, "carte %s : mode sommeil, pause après %u min", uid, (unsigned)minutes);
    }
}

/* Le mode sommeil ne vaut que pour la file de la carte (pas pour une lecture lancée depuis
 * l'interface web ou une application) ; un réglage modifié pendant l'écoute relance le décompte. */
static void track_sleep(void)
{
    if (!point_is_live(s_live)) {
        if (s_sleep_uid[0]) {
            s_sleep_uid[0] = '\0';
            if (s_sleep_tracks || s_sleep_minutes) {
                player_set_sleep(0, 0);
            }
            s_sleep_tracks = s_sleep_minutes = 0;
        }
        return;
    }
    const char *uid = s_points[s_live].uid;
    card_entry_t e;
    uint16_t tracks = 0, minutes = 0;
    if (cards_get(uid, &e)) {
        tracks = e.sleep_tracks;
        minutes = e.sleep_minutes;
    }
    if (strcmp(uid, s_sleep_uid) != 0 || tracks != s_sleep_tracks || minutes != s_sleep_minutes) {
        apply_sleep(uid);
    }
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

static bool clock_valid(void)
{
    return time(NULL) > 1700000000; /* heure obtenue par NTP */
}

static void save_now(int i)
{
    resume_point_t copy;
    LOCK();
    copy = s_points[i];
    UNLOCK();
    esp_err_t err = copy.uid[0] ? resume_store_save(i, &copy) : resume_store_erase(i);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sauvegarde de la position impossible (%s)", esp_err_to_name(err));
    }
    s_last_save_us[i] = s_last_pos_us[i] = esp_timer_get_time();
    s_saved_pos[i] = copy.position_ms;
    s_saved_hash[i] = fnv1a(copy.track);
    s_dirty[i] = false;
}

static void save_position_now(int i)
{
    uint32_t pos;
    LOCK();
    pos = s_points[i].position_ms;
    UNLOCK();
    esp_err_t err = resume_store_save_position(i, pos);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sauvegarde de la position impossible (%s)", esp_err_to_name(err));
    }
    s_last_pos_us[i] = esp_timer_get_time();
    s_saved_pos[i] = pos;
}

/* Enregistre dès que possible, sans dépasser une écriture toutes les 10 s par carte. */
static void persist(int i)
{
    if (i < 0) {
        return;
    }
    if (esp_timer_get_time() - s_last_save_us[i] >= SAVE_MIN_INTERVAL_US || s_last_save_us[i] == 0) {
        save_now(i);
    } else {
        s_dirty[i] = true;
    }
}

/* Après un redémarrage, le temps écoulé depuis le retrait se déduit de l'heure réelle. */
static void refresh_removed_at(resume_point_t *p, int64_t now_us)
{
    if (p->removed && p->removed_epoch > 0 && clock_valid()) {
        int64_t elapsed = (int64_t)time(NULL) - p->removed_epoch;
        p->removed_at_us = now_us - (elapsed > 0 ? elapsed : 0) * 1000000;
    }
}

static bool card_folder(const char *uid, char *folder, size_t len)
{
    card_entry_t e;
    if (cards_get(uid, &e)) {
        str_copy(folder, e.folder, len);
        return true;
    }
    if (storage_is_dir(uid)) {
        /* Compatibilité avec l'ancien firmware : dossier nommé d'après l'UID
         * (FAT ignore la casse : "04ab53..." correspond à "04AB53..."). */
        str_copy(folder, uid, len);
        return true;
    }
    return false;
}

/*
 * Charge le dossier et lance la lecture au morceau "track" (ou au début) à "position_ms".
 * En ordre aléatoire, la même graine redonne le même ordre (reprise).
 */
static void play_folder_from(int pi, const char *folder, const char *track, uint32_t position_ms, bool shuffle,
                             uint32_t seed)
{
    path_list_t tracks = {0};
    esp_err_t err = storage_is_mounted() ? storage_list_tracks(folder, &tracks) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK || tracks.count == 0) {
        ESP_LOGW(TAG, "dossier \"%s\" vide ou illisible (%s)", folder, esp_err_to_name(err));
        path_list_free(&tracks);
        player_beep(BEEP_ERROR);
        return;
    }
    if (shuffle) {
        session_shuffle(tracks.items, tracks.count, seed);
    }
    int index = 0;
    if (track && track[0]) {
        index = -1;
        for (int i = 0; i < tracks.count; i++) {
            if (strcmp(tracks.items[i], track) == 0) {
                index = i;
                break;
            }
        }
        if (index < 0) { /* morceau supprimé ou renommé entre-temps */
            index = 0;
            position_ms = 0;
        }
    }
    char first[REL_PATH_MAX];
    str_copy(first, tracks.items[index], sizeof(first));
    err = player_queue_replace(&tracks);
    int count = tracks.count;
    path_list_free(&tracks);
    if (err != ESP_OK) {
        player_beep(BEEP_ERROR);
        return;
    }
    LOCK();
    resume_point_t *p = &s_points[pi];
    str_copy(p->folder, folder, sizeof(p->folder));
    str_copy(p->track, first, sizeof(p->track));
    p->position_ms = position_ms;
    p->queue_version = player_queue_version();
    p->card_seq = s_card_seq;
    p->removed = false;
    p->removed_epoch = 0;
    p->finished = false;
    p->restored = false;
    p->shuffle = shuffle;
    p->shuffle_seed = seed;
    s_live = pi;
    UNLOCK();
    persist(pi);
    apply_sound(p->uid);
    if (position_ms > 0) {
        ESP_LOGI(TAG, "carte %s : reprise de \"%s\" à %u s", p->uid, first, (unsigned)(position_ms / 1000));
        if (player_seek(index, position_ms) != ESP_OK) {
            player_play(index);
        }
    } else {
        ESP_LOGI(TAG, "carte %s -> \"%s\" (%d morceaux%s, morceau %d)", p->uid, folder, count,
                 shuffle ? " en ordre aléatoire" : "", index + 1);
        player_play(index);
    }
    apply_sleep(p->uid);
}

static void on_card_on(const char *uid)
{
    LOCK();
    str_copy(s_present, uid, sizeof(s_present));
    bool learning = s_learning;
    if (learning) {
        str_copy(s_learned, uid, sizeof(s_learned));
        s_learning = false;
    } else {
        s_card_seq++;
    }
    UNLOCK();
    if (learning) {
        ESP_LOGI(TAG, "carte capturée pour association : %s", uid);
        player_beep(BEEP_OK);
        return;
    }

    char folder[REL_PATH_MAX];
    if (!card_folder(uid, folder, sizeof(folder))) {
        ESP_LOGI(TAG, "carte inconnue : %s", uid);
        LOCK();
        str_copy(s_unknown, uid, sizeof(s_unknown));
        UNLOCK();
        player_beep(BEEP_UNKNOWN);
        return;
    }

    int pi = find_point(uid);
    if (pi >= 0) {
        refresh_removed_at(&s_points[pi], esp_timer_get_time());
    }
    resume_policy_t pol = policy_for(uid);
    /* Un point de reprise ne vaut que pour le dossier actuellement associé. */
    const resume_point_t *p = pi >= 0 && strcmp(s_points[pi].folder, folder) == 0 ? &s_points[pi] : NULL;
    session_action_t action =
        session_decide(p, &pol, esp_timer_get_time(), player_queue_version(), player_state(), s_card_seq);
    switch (action) {
    case SESSION_NOTHING:
    case SESSION_RESUME_LIVE:
        LOCK();
        s_points[pi].removed = false;
        s_points[pi].removed_epoch = 0;
        s_points[pi].card_seq = s_card_seq;
        s_live = pi;
        UNLOCK();
        persist(pi);
        if (action == SESSION_RESUME_LIVE) {
            ESP_LOGI(TAG, "reprise de la carte %s", uid);
            apply_sound(uid);
            player_pause(0);
            apply_sleep(uid);
        }
        break;
    case SESSION_RESUME_SEEK: {
        char track[REL_PATH_MAX];
        str_copy(track, s_points[pi].track, sizeof(track));
        play_folder_from(pi, folder, track, s_points[pi].position_ms, s_points[pi].shuffle,
                         s_points[pi].shuffle_seed);
        break;
    }
    case SESSION_START_NEW: {
        LOCK();
        pi = alloc_point(uid);
        memset(&s_points[pi], 0, sizeof(s_points[pi]));
        str_copy(s_points[pi].uid, uid, sizeof(s_points[pi].uid));
        UNLOCK();
        bool shuffle = shuffle_for(uid); /* nouvel ordre à chaque nouveau départ */
        play_folder_from(pi, folder, NULL, 0, shuffle, shuffle ? esp_random() : 0);
        break;
    }
    }
}

static void on_card_off(const char *uid)
{
    LOCK();
    s_present[0] = '\0';
    UNLOCK();
    int pi = find_point(uid);
    if (pi < 0) {
        return;
    }
    bool live = point_is_live(pi);
    if (live && player_state() == SESS_PLAYER_PLAYING) {
        player_pause(1);
    }
    player_status_t st;
    player_get_status(&st);
    LOCK();
    resume_point_t *p = &s_points[pi];
    if (live && st.state != PLAYER_STOPPED && st.file[0]) {
        str_copy(p->track, st.file, sizeof(p->track));
        p->position_ms = st.elapsed_ms;
    }
    p->removed = true;
    p->removed_at_us = esp_timer_get_time();
    p->removed_epoch = clock_valid() ? (int64_t)time(NULL) : 0;
    uint32_t position = p->position_ms;
    UNLOCK();
    persist(pi);
    if (live) {
        resume_policy_t pol = policy_for(uid);
        if (pol.timeout_s) {
            ESP_LOGI(TAG, "carte %s retirée : pause à %u s (reprise possible %u s)", uid, (unsigned)(position / 1000),
                     (unsigned)pol.timeout_s);
        } else {
            ESP_LOGI(TAG, "carte %s retirée : pause à %u s (reprise sans limite de temps)", uid,
                     (unsigned)(position / 1000));
        }
    }
}

/* Suit la position de la carte en cours et l'enregistre au rythme décrit plus haut. */
static void track_live_position(int64_t now)
{
    if (!point_is_live(s_live)) {
        return;
    }
    player_status_t st;
    player_get_status(&st);
    if (st.state == PLAYER_STOPPED || !st.file[0]) {
        return;
    }
    int i = s_live;
    LOCK();
    str_copy(s_points[i].track, st.file, sizeof(s_points[i].track));
    s_points[i].position_ms = st.elapsed_ms;
    UNLOCK();
    bool track_changed = fnv1a(st.file) != s_saved_hash[i];
    uint32_t moved = st.elapsed_ms > s_saved_pos[i] ? st.elapsed_ms - s_saved_pos[i] : s_saved_pos[i] - st.elapsed_ms;
    if (s_dirty[i]) {
        return; /* un point complet est déjà en attente d'écriture */
    }
    if (track_changed) {
        persist(i); /* nouveau morceau : point complet */
    } else if (now - s_last_pos_us[i] >= SAVE_PERIOD_US && moved >= SAVE_MIN_PROGRESS_MS) {
        save_position_now(i); /* même morceau : position seule */
    }
}

static void tick(int64_t now)
{
    track_live_position(now);
    track_sleep();
    apply_sound(point_is_live(s_live) ? s_points[s_live].uid : NULL); /* réglages modifiés entre-temps */
    for (int i = 0; i < MAX_POINTS; i++) {
        resume_point_t *p = &s_points[i];
        if (!p->uid[0]) {
            continue;
        }
        refresh_removed_at(p, now);
        resume_policy_t pol = policy_for(p->uid);
        if (session_expired(p, &pol, now)) {
            ESP_LOGI(TAG, "carte %s : délai de reprise dépassé", p->uid);
            if (point_is_live(i) && player_state() == SESS_PLAYER_PAUSED) {
                player_stop();
            }
            LOCK();
            memset(p, 0, sizeof(*p));
            if (s_live == i) {
                s_live = -1;
            }
            UNLOCK();
            persist(i); /* effacement */
            continue;
        }
        if (s_dirty[i] && now - s_last_save_us[i] >= SAVE_MIN_INTERVAL_US) {
            save_now(i);
        }
    }
    LOCK();
    if (s_learning && now > s_learn_deadline) {
        s_learning = false;
    }
    UNLOCK();
}

/* Dossier ou fichier déplacé/renommé : les positions mémorisées suivent. */
static void on_renamed(const char *from, const char *to)
{
    /* La file de lecture suit le déplacement ; la carte qui joue reste « la même ». */
    bool was_live = point_is_live(s_live);
    if (player_queue_rename(from, to) && was_live) {
        LOCK();
        s_points[s_live].queue_version = player_queue_version();
        UNLOCK();
    }
    size_t fl = strlen(from);
    for (int i = 0; i < MAX_POINTS; i++) {
        resume_point_t *p = &s_points[i];
        bool changed = false;
        char buf[REL_PATH_MAX];
        LOCK();
        char *fields[2] = {p->folder, p->track};
        for (int k = 0; k < 2 && p->uid[0]; k++) {
            char *v = fields[k];
            if (strncmp(v, from, fl) == 0 && (v[fl] == '\0' || v[fl] == '/')) {
                int n = snprintf(buf, sizeof(buf), "%s%s", to, v + fl);
                if (n > 0 && (size_t)n < sizeof(buf)) {
                    str_copy(v, buf, REL_PATH_MAX);
                    changed = true;
                }
            }
        }
        UNLOCK();
        if (changed) {
            persist(i);
        }
    }
}

void controller_on_path_renamed(const char *from, const char *to)
{
    ev_t ev = {.type = EV_RENAMED, .from = strdup(from), .to = strdup(to)};
    if (!ev.from || !ev.to || xQueueSend(s_q, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(ev.from);
        free(ev.to);
    }
}

static void controller_task(void *arg)
{
    for (;;) {
        ev_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (ev.type) {
            case EV_CARD_ON:
                on_card_on(ev.uid);
                break;
            case EV_CARD_OFF:
                on_card_off(ev.uid);
                break;
            case EV_QUEUE_END:
                if (point_is_live(s_live)) {
                    ESP_LOGI(TAG, "playlist de la carte %s terminée", s_points[s_live].uid);
                    LOCK();
                    s_points[s_live].finished = true;
                    UNLOCK();
                    persist(s_live);
                }
                break;
            case EV_RENAMED:
                on_renamed(ev.from, ev.to);
                free(ev.from);
                free(ev.to);
                break;
            case EV_LEARN_START:
                LOCK();
                s_learning = true;
                s_learned[0] = '\0';
                s_learn_deadline = esp_timer_get_time() + LEARN_TIMEOUT_US;
                UNLOCK();
                break;
            case EV_LEARN_CANCEL:
                LOCK();
                s_learning = false;
                UNLOCK();
                break;
            }
        }
        tick(esp_timer_get_time());
    }
}

esp_err_t controller_start(void)
{
    s_q = xQueueCreate(16, sizeof(ev_t));
    s_lock = xSemaphoreCreateMutex();
    s_points = calloc(MAX_POINTS, sizeof(resume_point_t));
    if (!s_q || !s_lock || !s_points) {
        return ESP_ERR_NO_MEM;
    }
    resume_store_load(s_points, MAX_POINTS);
    for (int i = 0; i < MAX_POINTS; i++) {
        s_saved_pos[i] = s_points[i].position_ms;
        s_saved_hash[i] = fnv1a(s_points[i].track);
    }
    return xTaskCreate(controller_task, "controller", 5120, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void controller_get_status(controller_status_t *st)
{
    memset(st, 0, sizeof(*st));
    int64_t now = esp_timer_get_time();
    resume_point_t p = {0};
    LOCK();
    st->learning = s_learning;
    st->learn_remaining_s = s_learning ? (int)((s_learn_deadline - now) / 1000000) : 0;
    str_copy(st->learned_uid, s_learned, sizeof(st->learned_uid));
    str_copy(st->present_uid, s_present, sizeof(st->present_uid));
    str_copy(st->last_unknown_uid, s_unknown, sizeof(st->last_unknown_uid));
    if (s_live >= 0) {
        p = s_points[s_live];
    }
    UNLOCK();
    if (!p.uid[0]) {
        return;
    }
    str_copy(st->session_uid, p.uid, sizeof(st->session_uid));
    str_copy(st->session_folder, p.folder, sizeof(st->session_folder));
    if (p.removed && !p.finished) {
        resume_policy_t pol = policy_for(p.uid);
        if (pol.timeout_s == 0) {
            st->resume_remaining_s = -1; /* sans limite */
        } else {
            int64_t left = (int64_t)pol.timeout_s * 1000000 - (now - p.removed_at_us);
            st->resume_remaining_s = left > 0 ? (int)(left / 1000000) : 0;
        }
    }
}
