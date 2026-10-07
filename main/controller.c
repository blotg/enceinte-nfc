#include "controller.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "session.h"
#include "settings.h"
#include "storage.h"

static const char *TAG = "controller";

#define LEARN_TIMEOUT_US (60LL * 1000000)
#define MAX_POINTS 16 /* cartes dont la position est mémorisée */

typedef enum {
    EV_CARD_ON,
    EV_CARD_OFF,
    EV_QUEUE_END,
    EV_LEARN_START,
    EV_LEARN_CANCEL,
} ev_type_t;

typedef struct {
    ev_type_t type;
    char uid[UID_STR_MAX];
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

/* Charge le dossier et lance la lecture au morceau "track" (ou au début) à "position_ms". */
static void play_folder_from(int pi, const char *folder, const char *track, uint32_t position_ms)
{
    path_list_t tracks = {0};
    esp_err_t err = storage_is_mounted() ? storage_list_tracks(folder, &tracks) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK || tracks.count == 0) {
        ESP_LOGW(TAG, "dossier \"%s\" vide ou illisible (%s)", folder, esp_err_to_name(err));
        path_list_free(&tracks);
        player_beep(BEEP_ERROR);
        return;
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
    p->finished = false;
    s_live = pi;
    UNLOCK();
    if (position_ms > 0) {
        ESP_LOGI(TAG, "carte %s : reprise de \"%s\" à %u s", p->uid, first, (unsigned)(position_ms / 1000));
        if (player_seek(index, position_ms) != ESP_OK) {
            player_play(index);
        }
    } else {
        ESP_LOGI(TAG, "carte %s -> \"%s\" (%d morceaux, morceau %d)", p->uid, folder, count, index + 1);
        player_play(index);
    }
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
        s_points[pi].card_seq = s_card_seq;
        s_live = pi;
        UNLOCK();
        if (action == SESSION_RESUME_LIVE) {
            ESP_LOGI(TAG, "reprise de la carte %s", uid);
            player_pause(0);
        }
        break;
    case SESSION_RESUME_SEEK: {
        char track[REL_PATH_MAX];
        str_copy(track, s_points[pi].track, sizeof(track));
        play_folder_from(pi, folder, track, s_points[pi].position_ms);
        break;
    }
    case SESSION_START_NEW:
        LOCK();
        pi = alloc_point(uid);
        memset(&s_points[pi], 0, sizeof(s_points[pi]));
        str_copy(s_points[pi].uid, uid, sizeof(s_points[pi].uid));
        UNLOCK();
        play_folder_from(pi, folder, NULL, 0);
        break;
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
    uint32_t position = p->position_ms;
    UNLOCK();
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

static void tick(int64_t now)
{
    /* Carte en pause dont le délai est dépassé : on libère le lecteur. */
    if (s_live >= 0 && s_points[s_live].uid[0]) {
        resume_policy_t pol = policy_for(s_points[s_live].uid);
        if (session_expired(&s_points[s_live], &pol, now)) {
            ESP_LOGI(TAG, "carte %s : délai de reprise dépassé", s_points[s_live].uid);
            if (point_is_live(s_live) && player_state() == SESS_PLAYER_PAUSED) {
                player_stop();
            }
            LOCK();
            memset(&s_points[s_live], 0, sizeof(s_points[s_live]));
            s_live = -1;
            UNLOCK();
        }
    }
    LOCK();
    if (s_learning && now > s_learn_deadline) {
        s_learning = false;
    }
    UNLOCK();
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
                }
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
