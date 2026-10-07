#pragma once
/*
 * Chef d'orchestre : relie les cartes NFC, les associations et le lecteur.
 * Tous les évènements passent par une seule tâche (pas de concurrence sur la session).
 */
#include <stdbool.h>

#include "cards.h"
#include "esp_err.h"
#include "player.h"

typedef struct {
    bool learning;                     /* attente d'une carte à associer */
    int learn_remaining_s;
    char learned_uid[UID_STR_MAX];     /* dernière carte capturée en mode association */
    char present_uid[UID_STR_MAX];     /* carte posée actuellement */
    char last_unknown_uid[UID_STR_MAX];
    char session_uid[UID_STR_MAX];
    char session_folder[REL_PATH_MAX];
    int resume_remaining_s;            /* > 0 : la carte retirée peut reprendre */
} controller_status_t;

esp_err_t controller_start(void);
void controller_on_nfc(bool present, const char *uid);
void controller_on_player_event(player_event_t evt);
void controller_learn_start(void);
void controller_learn_cancel(void);
void controller_get_status(controller_status_t *st);
