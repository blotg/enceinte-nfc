#pragma once
/*
 * Règle de reprise d'une carte (logique pure, testée sur PC) :
 * une carte retirée met en pause. Si elle est reposée dans le délai, qu'aucune
 * autre carte n'a été posée entre-temps et que la playlist n'est pas terminée,
 * la lecture reprend ; sinon elle recommence au début.
 */
#include <stdbool.h>
#include <stdint.h>

#include "cards.h"

typedef enum {
    SESS_PLAYER_STOPPED = 0,
    SESS_PLAYER_PLAYING,
    SESS_PLAYER_PAUSED,
} sess_player_state_t;

typedef struct {
    char uid[UID_STR_MAX];  /* "" = aucune session */
    uint32_t queue_version; /* version de la file au chargement : détecte une modification extérieure */
    bool finished;          /* playlist jouée jusqu'au bout */
    bool removed;
    int64_t removed_at_us;
} card_session_t;

typedef enum {
    SESSION_START_NEW,
    SESSION_RESUME,
    SESSION_NOTHING, /* déjà en lecture */
} session_action_t;

session_action_t session_on_card(const card_session_t *s, const char *uid, int64_t now_us, uint32_t queue_version,
                                 sess_player_state_t player, int64_t timeout_us);

/* Vrai si la session en pause a dépassé le délai de reprise. */
bool session_expired(const card_session_t *s, int64_t now_us, int64_t timeout_us);
