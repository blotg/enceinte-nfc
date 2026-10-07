#include "session.h"
#include "test.h"

#define S 1000000LL
#define TIMEOUT (600 * S)

void test_session(void)
{
    card_session_t s = {0};

    /* Aucune session : démarrage */
    CHECK(session_on_card(&s, "AA", 0, 1, SESS_PLAYER_STOPPED, TIMEOUT) == SESSION_START_NEW);

    strcpy(s.uid, "AA");
    s.queue_version = 5;

    /* Carte retirée puis reposée dans le délai, en pause : reprise */
    s.removed = true;
    s.removed_at_us = 100 * S;
    CHECK(session_on_card(&s, "AA", 100 * S + 599 * S, 5, SESS_PLAYER_PAUSED, TIMEOUT) == SESSION_RESUME);

    /* Hors délai : recommence */
    CHECK(session_on_card(&s, "AA", 100 * S + 601 * S, 5, SESS_PLAYER_PAUSED, TIMEOUT) == SESSION_START_NEW);
    CHECK(session_expired(&s, 100 * S + 601 * S, TIMEOUT));
    CHECK(!session_expired(&s, 100 * S + 599 * S, TIMEOUT));

    /* Autre carte : recommence (la session appartient à AA) */
    CHECK(session_on_card(&s, "BB", 101 * S, 5, SESS_PLAYER_PAUSED, TIMEOUT) == SESSION_START_NEW);

    /* File modifiée entre-temps (client MPD, interface web) : recommence */
    CHECK(session_on_card(&s, "AA", 101 * S, 6, SESS_PLAYER_PAUSED, TIMEOUT) == SESSION_START_NEW);

    /* Playlist terminée : recommence */
    s.finished = true;
    CHECK(session_on_card(&s, "AA", 101 * S, 5, SESS_PLAYER_STOPPED, TIMEOUT) == SESSION_START_NEW);
    CHECK(!session_expired(&s, 1000 * S, TIMEOUT)); /* rien à expirer */
    s.finished = false;

    /* Lecture relancée depuis l'application pendant l'absence de la carte : rien à faire */
    CHECK(session_on_card(&s, "AA", 101 * S, 5, SESS_PLAYER_PLAYING, TIMEOUT) == SESSION_NOTHING);

    /* Lecteur arrêté (stop manuel) : recommence */
    CHECK(session_on_card(&s, "AA", 101 * S, 5, SESS_PLAYER_STOPPED, TIMEOUT) == SESSION_START_NEW);
}
