#include "session.h"
#include "test.h"

#define S 1000000LL

static resume_point_t point(const char *uid, uint32_t qv, uint32_t seq, int64_t removed_at)
{
    resume_point_t p = {0};
    strcpy(p.uid, uid);
    strcpy(p.folder, "Dossier");
    strcpy(p.track, "Dossier/02.mp3");
    p.position_ms = 42000;
    p.queue_version = qv;
    p.card_seq = seq;
    p.removed = true;
    p.removed_at_us = removed_at;
    return p;
}

void test_session(void)
{
    const resume_policy_t def = {.timeout_s = 600, .after_other = false};
    const resume_policy_t always = {.timeout_s = 0, .after_other = false};
    const resume_policy_t keep = {.timeout_s = 600, .after_other = true};

    /* Aucun point : démarrage */
    CHECK(session_decide(NULL, &def, 0, 1, SESS_PLAYER_STOPPED, 1) == SESSION_START_NEW);

    /* Retirée puis reposée dans le délai, file intacte, en pause : reprise immédiate */
    resume_point_t p = point("AA", 5, 3, 100 * S);
    CHECK(session_decide(&p, &def, 699 * S, 5, SESS_PLAYER_PAUSED, 4) == SESSION_RESUME_LIVE);

    /* Délai dépassé : recommence ; délai 0 : jamais dépassé */
    CHECK(session_decide(&p, &def, 701 * S, 5, SESS_PLAYER_PAUSED, 4) == SESSION_START_NEW);
    CHECK(session_expired(&p, &def, 701 * S) && !session_expired(&p, &def, 699 * S));
    CHECK(session_decide(&p, &always, 100000 * S, 5, SESS_PLAYER_PAUSED, 4) == SESSION_RESUME_LIVE);
    CHECK(!session_expired(&p, &always, 100000 * S));

    /* Une autre carte posée entre-temps (numéro de pose : 3 -> 5) */
    CHECK(session_decide(&p, &def, 200 * S, 9, SESS_PLAYER_PLAYING, 5) == SESSION_START_NEW);
    /* ... mais la règle de la carte autorise la reprise : rechargement à la position mémorisée */
    CHECK(session_decide(&p, &keep, 200 * S, 9, SESS_PLAYER_PLAYING, 5) == SESSION_RESUME_SEEK);
    /* ... carte inconnue entre-temps (file intacte, lecteur en pause) : reprise immédiate */
    CHECK(session_decide(&p, &keep, 200 * S, 5, SESS_PLAYER_PAUSED, 5) == SESSION_RESUME_LIVE);
    CHECK(session_decide(&p, &def, 200 * S, 5, SESS_PLAYER_PAUSED, 5) == SESSION_START_NEW);
    /* ... délai dépassé quand même : recommence */
    CHECK(session_decide(&p, &keep, 701 * S, 9, SESS_PLAYER_PLAYING, 5) == SESSION_START_NEW);

    /* File modifiée par une application : comme une autre carte */
    CHECK(session_decide(&p, &def, 200 * S, 6, SESS_PLAYER_PAUSED, 4) == SESSION_START_NEW);
    CHECK(session_decide(&p, &keep, 200 * S, 6, SESS_PLAYER_PAUSED, 4) == SESSION_RESUME_SEEK);

    /* Playlist terminée : recommence, quelle que soit la règle */
    p.finished = true;
    CHECK(session_decide(&p, &keep, 101 * S, 5, SESS_PLAYER_STOPPED, 4) == SESSION_START_NEW);
    CHECK(!session_expired(&p, &def, 100000 * S));
    p.finished = false;

    /* Relancée depuis l'application pendant l'absence de la carte : rien à faire */
    CHECK(session_decide(&p, &def, 101 * S, 5, SESS_PLAYER_PLAYING, 4) == SESSION_NOTHING);
    /* Arrêt demandé dans l'application : recommence */
    CHECK(session_decide(&p, &def, 101 * S, 5, SESS_PLAYER_STOPPED, 4) == SESSION_START_NEW);

    /* Point sans morceau mémorisé : recommence plutôt que reprendre n'importe où */
    p.track[0] = '\0';
    CHECK(session_decide(&p, &keep, 200 * S, 9, SESS_PLAYER_PLAYING, 5) == SESSION_START_NEW);

    /* Règles : réglage de la carte prioritaire sur le réglage général */
    resume_policy_t r = session_policy(CARD_DEFAULT, CARD_DEFAULT, 600, false);
    CHECK(r.timeout_s == 600 && !r.after_other);
    r = session_policy(0, 1, 600, false);
    CHECK(r.timeout_s == 0 && r.after_other);
    r = session_policy(120, 0, 0, true);
    CHECK(r.timeout_s == 120 && !r.after_other);
}
