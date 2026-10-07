#pragma once
/*
 * Règles de reprise d'une carte (logique pure, testée sur PC).
 *
 * Retirer une carte met en pause et mémorise son point de reprise (morceau, position).
 * Reposée, elle reprend si :
 *  - sa playlist n'était pas terminée ;
 *  - le délai n'est pas dépassé (délai 0 = toujours) ;
 *  - aucune autre carte n'a été posée entre-temps, sauf si la règle de la carte
 *    autorise la reprise après une autre carte.
 * Sinon, elle recommence au début de son dossier.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cards.h"

typedef enum {
    SESS_PLAYER_STOPPED = 0,
    SESS_PLAYER_PLAYING,
    SESS_PLAYER_PAUSED,
} sess_player_state_t;

typedef struct {
    uint32_t timeout_s; /* 0 = toujours reprendre */
    bool after_other;   /* reprendre même si une autre carte a été posée entre-temps */
} resume_policy_t;

typedef struct {
    char uid[UID_STR_MAX];   /* "" = emplacement libre */
    char folder[REL_PATH_MAX];
    char track[REL_PATH_MAX]; /* morceau en cours au retrait */
    uint32_t position_ms;
    uint32_t queue_version;   /* version de la file pendant que la carte jouait */
    uint32_t card_seq;        /* numéro de la dernière pose de cette carte */
    int64_t removed_at_us;
    int64_t removed_epoch;    /* heure du retrait (time_t), 0 si l'horloge n'était pas à l'heure */
    bool removed;
    bool finished;            /* playlist jouée jusqu'au bout */
    bool restored;            /* rechargé depuis la mémoire permanente au démarrage */
} resume_point_t;

typedef enum {
    SESSION_START_NEW,   /* recommencer au début du dossier */
    SESSION_RESUME_LIVE, /* le lecteur est encore en pause sur cette carte : reprise immédiate */
    SESSION_RESUME_SEEK, /* recharger le dossier, reprendre au morceau et à la position mémorisés */
    SESSION_NOTHING,     /* déjà en lecture */
} session_action_t;

/*
 * p : point de reprise de la carte (NULL si aucun).
 * queue_version / player : état actuel du lecteur.
 * card_seq_now : numéro de la pose en cours (incrémenté à chaque carte posée).
 */
session_action_t session_decide(const resume_point_t *p, const resume_policy_t *pol, int64_t now_us,
                                uint32_t queue_version, sess_player_state_t player, uint32_t card_seq_now);

/* Vrai si le point de reprise a dépassé son délai. */
bool session_expired(const resume_point_t *p, const resume_policy_t *pol, int64_t now_us);

/*
 * Sérialisation d'un point de reprise pour la mémoire permanente.
 * Retourne la taille écrite (0 si "out" est trop petit) / false si les données sont invalides.
 * Les champs propres à l'exécution (version de file, numéro de pose) ne sont pas enregistrés.
 */
size_t resume_encode(const resume_point_t *p, uint8_t *out, size_t cap);
bool resume_decode(const uint8_t *in, size_t len, resume_point_t *p);

/* Règle effective d'une carte : réglage de la carte, sinon réglage général. */
resume_policy_t session_policy(int32_t card_resume_s, int8_t card_resume_other, uint32_t default_timeout_s,
                               bool default_after_other);
