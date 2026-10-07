#pragma once
/*
 * Compteurs de changements par sous-système, au sens du protocole MPD
 * (commande "idle"). Le serveur MPD compare des instantanés de ces compteurs.
 */
#include <stdint.h>

typedef enum {
    CHG_DATABASE = 0,
    CHG_UPDATE,
    CHG_STORED_PLAYLIST,
    CHG_PLAYLIST,
    CHG_PLAYER,
    CHG_MIXER,
    CHG_OUTPUT,
    CHG_OPTIONS,
    CHG_COUNT,
} change_t;

void changes_notify(change_t c);
void changes_snapshot(uint32_t out[CHG_COUNT]);
const char *changes_name(change_t c);
/* Retourne -1 si le nom n'est pas un sous-système connu. */
int changes_from_name(const char *name);
