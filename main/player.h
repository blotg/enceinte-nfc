#pragma once
/*
 * Lecteur audio : file d'attente (au sens MPD), décodage, sortie I2S.
 *
 * Deux tâches :
 *  - "reader" lit la carte SD en avance dans un réservoir de blocs (absorbe les
 *    lenteurs ponctuelles de la carte, par exemple pendant un envoi de fichiers) ;
 *  - "player" décode et alimente l'I2S, et traite les commandes.
 *
 * Toutes les fonctions sont utilisables depuis n'importe quelle tâche. Les commandes
 * de transport sont exécutées par la tâche "player" et la fonction attend leur prise
 * en compte (au plus 2 s).
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "storage.h"
#include "util.h"

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} player_state_t;

typedef struct {
    player_state_t state;
    int song;             /* position du morceau courant, -1 si aucun */
    uint32_t song_id;
    int next_song;        /* -1 si aucun */
    uint32_t next_song_id;
    int queue_len;
    uint32_t queue_version;
    uint32_t elapsed_ms;
    uint32_t duration_ms; /* 0 si inconnue */
    uint32_t bitrate_kbps;
    uint32_t sample_rate;
    uint8_t bits;
    uint8_t channels;
    uint8_t volume;
    bool repeat;
    bool random;
    bool consume;
    uint8_t single; /* 0, 1, 2 = "oneshot" */
    bool seekable;
    uint32_t paused_s; /* durée de la pause en cours */
    char file[REL_PATH_MAX];
    char error[96];
} player_status_t;

typedef struct {
    uint32_t id;
    char path[REL_PATH_MAX];
} queue_item_t;

typedef enum {
    PLAYER_EVT_QUEUE_END, /* fin naturelle de la file */
} player_event_t;

typedef void (*player_event_cb_t)(player_event_t evt);

/* Bips joués au volume réglé (et jamais au-delà du volume maximum). */
typedef enum {
    BEEP_OK,      /* un bip aigu */
    BEEP_UNKNOWN, /* deux bips : carte inconnue */
    BEEP_ERROR,   /* trois bips graves : dossier vide ou introuvable */
    BEEP_TICK,    /* bip très court : nouveau volume (boutons, hors lecture) */
} beep_t;

esp_err_t player_init(uint8_t volume, uint8_t max_volume, player_event_cb_t cb);
void player_get_status(player_status_t *st);

/* ---- File d'attente ---- */
int player_queue_length(void);
uint32_t player_queue_version(void);
bool player_queue_get(int pos, queue_item_t *out);
int player_queue_pos_of_id(uint32_t id);
/* pos < 0 : ajout en fin. */
esp_err_t player_queue_add(const char *rel_path, int pos, uint32_t *id_out);
/* Supprime les positions [start, end). */
esp_err_t player_queue_delete(int start, int end);
/* Déplace [start, end) vers la position "to" (calculée après retrait). */
esp_err_t player_queue_move(int start, int end, int to);
esp_err_t player_queue_clear(void);
esp_err_t player_queue_shuffle(void);
/* Remplace toute la file (utilisé par les cartes NFC). */
esp_err_t player_queue_replace(const path_list_t *list);
/* Un fichier ou dossier a été déplacé : met à jour les chemins de la file. Retourne true si
 * la file a changé. */
bool player_queue_rename(const char *from, const char *to);

/* ---- Transport ---- */
esp_err_t player_play(int pos); /* pos < 0 : reprendre / démarrer */
esp_err_t player_play_id(uint32_t id);
esp_err_t player_pause(int mode); /* 1 pause, 0 reprise, -1 bascule */
esp_err_t player_stop(void);
esp_err_t player_next(void);
esp_err_t player_previous(void);
/* pos < 0 : morceau courant. ESP_ERR_NOT_SUPPORTED si le format ne le permet pas. */
esp_err_t player_seek(int pos, uint32_t ms);
void player_beep(beep_t beep);
/* Efface le dernier message d'erreur (il reste affiché jusqu'à une lecture explicite). */
void player_clear_error(void);

/* ---- Volume et options ---- */
/* Volume borné entre 0 et le volume maximum ; enregistré en flash (cf. settings). */
void player_set_volume(int volume);
int player_get_volume(void);
/* Un volume au-dessus du nouveau maximum est abaissé. */
void player_set_max_volume(uint8_t max_volume);
int player_get_max_volume(void);
/* Normalisation et compression (0 à 3, cf. dsp.h), appliquées dès le bloc suivant. */
void player_set_sound(uint8_t normalize, uint8_t compress);
void player_get_sound(uint8_t *normalize, uint8_t *compress);
void player_set_repeat(bool on);
void player_set_random(bool on);
void player_set_single(uint8_t mode);
void player_set_consume(bool on);
