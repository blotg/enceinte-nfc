#include <stdio.h>
#include <stdlib.h>

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

    /* Point rechargé après un redémarrage : la file n'existe plus, mais ce n'est pas
     * « une autre carte » ; la carte reprend à la position enregistrée. */
    resume_point_t r0 = point("AA", 0, 0, 0);
    r0.removed = false; /* coupure de courant pendant la lecture */
    r0.restored = true;
    CHECK(session_decide(&r0, &def, 5 * S, 1, SESS_PLAYER_STOPPED, 1) == SESSION_RESUME_SEEK);
    /* ... mais si une autre carte a été posée depuis le démarrage, la règle s'applique */
    CHECK(session_decide(&r0, &def, 5 * S, 3, SESS_PLAYER_PLAYING, 2) == SESSION_START_NEW);
    CHECK(session_decide(&r0, &keep, 5 * S, 3, SESS_PLAYER_PLAYING, 2) == SESSION_RESUME_SEEK);

    /* Sérialisation : aller-retour, données tronquées ou abîmées */
    resume_point_t src = point("04A1B2C3D4E5F6", 7, 9, 123);
    strcpy(src.folder, "Livres audio/Le Petit Prince");
    strcpy(src.track, "Livres audio/Le Petit Prince/03 - Chapitre trois.opus");
    src.position_ms = 3725000;
    src.removed_epoch = 1791234567;
    src.finished = false;
    uint8_t blob[600];
    size_t n = resume_encode(&src, blob, sizeof(blob));
    CHECK(n > 0);
    resume_point_t dst;
    CHECK(resume_decode(blob, n, &dst));
    CHECK_STR(dst.uid, src.uid);
    CHECK_STR(dst.folder, src.folder);
    CHECK_STR(dst.track, src.track);
    CHECK(dst.position_ms == 3725000 && dst.removed_epoch == 1791234567 && dst.removed && !dst.finished);
    CHECK(dst.restored && dst.queue_version == 0 && dst.card_seq == 0);
    CHECK(!resume_decode(blob, n - 1, &dst)); /* dernière chaîne sans terminateur */
    CHECK(!resume_decode(blob, 10, &dst));
    blob[0] = 99;
    CHECK(!resume_decode(blob, n, &dst)); /* version inconnue */
    CHECK(resume_encode(&src, blob, 20) == 0);    /* tampon trop petit */

    /* Ordre aléatoire mémorisé avec le point */
    src.shuffle = true;
    src.shuffle_seed = 0xC0FFEE42;
    n = resume_encode(&src, blob, sizeof(blob));
    CHECK(n > 0 && resume_decode(blob, n, &dst));
    CHECK(dst.shuffle && dst.shuffle_seed == 0xC0FFEE42 && dst.position_ms == 3725000);
    CHECK_STR(dst.track, src.track);

    /* Format v1 (versions 1.3.x, sans graine) toujours lu */
    const uint8_t v1[] = {1, 1, 0x10, 0x27, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'A', 'A', 0, 'F', 0, 'F', '/', 't', 0};
    CHECK(resume_decode(v1, sizeof(v1), &dst));
    CHECK(dst.removed && !dst.shuffle && dst.shuffle_seed == 0 && dst.position_ms == 10000);
    CHECK_STR(dst.uid, "AA");
    CHECK_STR(dst.track, "F/t");

    /* Mélange : permutation complète, identique pour une même graine, différente sinon */
    char names[10][12];
    char *a[10], *b[10], *c[10];
    for (int i = 0; i < 10; i++) {
        snprintf(names[i], sizeof(names[i]), "%02d", i);
        a[i] = b[i] = c[i] = names[i];
    }
    session_shuffle(a, 10, 1234);
    session_shuffle(b, 10, 1234);
    session_shuffle(c, 10, 1235);
    bool same = true, other_differs = false, moved = false;
    int seen = 0;
    for (int i = 0; i < 10; i++) {
        same = same && a[i] == b[i];
        other_differs = other_differs || a[i] != c[i];
        moved = moved || a[i] != names[i];
        seen |= 1 << atoi(a[i]);
    }
    CHECK(same && other_differs && moved && seen == 0x3FF);
    session_shuffle(a, 1, 99); /* un seul morceau : rien à faire */
    session_shuffle(a, 0, 99);

    /* Règles : réglage de la carte prioritaire sur le réglage général */
    resume_policy_t r = session_policy(CARD_DEFAULT, CARD_DEFAULT, 600, false);
    CHECK(r.timeout_s == 600 && !r.after_other);
    r = session_policy(0, 1, 600, false);
    CHECK(r.timeout_s == 0 && r.after_other);
    r = session_policy(120, 0, 0, true);
    CHECK(r.timeout_s == 120 && !r.after_other);
}
