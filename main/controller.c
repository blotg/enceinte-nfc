#include "controller.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "session.h"
#include "storage.h"

static const char *TAG = "controller";

#define RESUME_TIMEOUT_US ((int64_t)CONFIG_ENC_RESUME_TIMEOUT_S * 1000000)
#define LEARN_TIMEOUT_US (60LL * 1000000)

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
static SemaphoreHandle_t s_lock; /* protège l'état exposé (status) */
static card_session_t s_sess;
static char s_folder[REL_PATH_MAX];
static char s_present[UID_STR_MAX];
static char s_learned[UID_STR_MAX];
static char s_unknown[UID_STR_MAX];
static bool s_learning;
static int64_t s_learn_deadline;

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

static bool session_owns_player(void)
{
    return s_sess.uid[0] && s_sess.queue_version == player_queue_version();
}

/* Abandonne la session ; arrête le lecteur s'il était en pause pour elle. */
static void drop_session(const char *why)
{
    if (!s_sess.uid[0]) {
        return;
    }
    ESP_LOGI(TAG, "fin de session %s : %s", s_sess.uid, why);
    if (session_owns_player() && player_state() == SESS_PLAYER_PAUSED) {
        player_stop();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_sess, 0, sizeof(s_sess));
    s_folder[0] = '\0';
    xSemaphoreGive(s_lock);
}

static void start_card(const char *uid)
{
    char folder[REL_PATH_MAX];
    bool known = cards_lookup(uid, folder, sizeof(folder));
    if (!known && storage_is_dir(uid)) {
        /* Compatibilité avec l'ancien firmware : dossier nommé d'après l'UID
         * (FAT ignore la casse : "04ab53..." correspond à "04AB53..."). */
        str_copy(folder, uid, sizeof(folder));
        known = true;
    }
    if (!known) {
        ESP_LOGI(TAG, "carte inconnue : %s", uid);
        drop_session("autre carte posée");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        str_copy(s_unknown, uid, sizeof(s_unknown));
        xSemaphoreGive(s_lock);
        player_beep(BEEP_UNKNOWN);
        return;
    }
    path_list_t tracks = {0};
    esp_err_t err = storage_is_mounted() ? storage_list_tracks(folder, &tracks) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK || tracks.count == 0) {
        ESP_LOGW(TAG, "dossier \"%s\" vide ou illisible (%s)", folder, esp_err_to_name(err));
        path_list_free(&tracks);
        drop_session("dossier vide");
        player_beep(BEEP_ERROR);
        return;
    }
    ESP_LOGI(TAG, "carte %s -> \"%s\" (%d morceaux)", uid, folder, tracks.count);
    err = player_queue_replace(&tracks);
    path_list_free(&tracks);
    if (err != ESP_OK) {
        player_beep(BEEP_ERROR);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_sess, 0, sizeof(s_sess));
    str_copy(s_sess.uid, uid, sizeof(s_sess.uid));
    s_sess.queue_version = player_queue_version();
    str_copy(s_folder, folder, sizeof(s_folder));
    xSemaphoreGive(s_lock);
    player_play(0);
}

static void on_card_on(const char *uid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    str_copy(s_present, uid, sizeof(s_present));
    bool learning = s_learning;
    if (learning) {
        str_copy(s_learned, uid, sizeof(s_learned));
        s_learning = false;
    }
    xSemaphoreGive(s_lock);
    if (learning) {
        ESP_LOGI(TAG, "carte capturée pour association : %s", uid);
        player_beep(BEEP_OK);
        return;
    }

    int64_t now = esp_timer_get_time();
    session_action_t action =
        session_on_card(&s_sess, uid, now, player_queue_version(), player_state(), RESUME_TIMEOUT_US);
    switch (action) {
    case SESSION_RESUME:
        ESP_LOGI(TAG, "reprise de la carte %s", uid);
        s_sess.removed = false;
        player_pause(0);
        break;
    case SESSION_NOTHING:
        s_sess.removed = false;
        break;
    case SESSION_START_NEW:
        start_card(uid);
        break;
    }
}

static void on_card_off(const char *uid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_present[0] = '\0';
    xSemaphoreGive(s_lock);
    if (strcmp(s_sess.uid, uid) != 0 || !session_owns_player()) {
        return;
    }
    if (player_state() == SESS_PLAYER_PLAYING) {
        player_pause(1);
    }
    s_sess.removed = true;
    s_sess.removed_at_us = esp_timer_get_time();
    ESP_LOGI(TAG, "carte %s retirée : pause (reprise possible %d s)", uid, CONFIG_ENC_RESUME_TIMEOUT_S);
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
                if (session_owns_player()) {
                    ESP_LOGI(TAG, "playlist de la carte %s terminée", s_sess.uid);
                    s_sess.finished = true;
                }
                break;
            case EV_LEARN_START:
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_learning = true;
                s_learned[0] = '\0';
                s_learn_deadline = esp_timer_get_time() + LEARN_TIMEOUT_US;
                xSemaphoreGive(s_lock);
                break;
            case EV_LEARN_CANCEL:
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_learning = false;
                xSemaphoreGive(s_lock);
                break;
            }
        }
        int64_t now = esp_timer_get_time();
        if (session_expired(&s_sess, now, RESUME_TIMEOUT_US)) {
            drop_session("délai de reprise dépassé");
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_learning && now > s_learn_deadline) {
            s_learning = false;
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t controller_start(void)
{
    s_q = xQueueCreate(16, sizeof(ev_t));
    s_lock = xSemaphoreCreateMutex();
    if (!s_q || !s_lock) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(controller_task, "controller", 5120, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void controller_get_status(controller_status_t *st)
{
    memset(st, 0, sizeof(*st));
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->learning = s_learning;
    st->learn_remaining_s = s_learning ? (int)((s_learn_deadline - now) / 1000000) : 0;
    str_copy(st->learned_uid, s_learned, sizeof(st->learned_uid));
    str_copy(st->present_uid, s_present, sizeof(st->present_uid));
    str_copy(st->last_unknown_uid, s_unknown, sizeof(st->last_unknown_uid));
    str_copy(st->session_uid, s_sess.uid, sizeof(st->session_uid));
    str_copy(st->session_folder, s_folder, sizeof(st->session_folder));
    if (s_sess.uid[0] && s_sess.removed && !s_sess.finished) {
        int64_t left = RESUME_TIMEOUT_US - (now - s_sess.removed_at_us);
        st->resume_remaining_s = left > 0 ? (int)(left / 1000000) : 0;
    }
    xSemaphoreGive(s_lock);
}
