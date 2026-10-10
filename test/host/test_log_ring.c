#include <stdlib.h>

#include "log_ring.h"
#include "test.h"

void test_log_ring(void)
{
    char buf[32], out[64];
    log_ring_t r;
    uint64_t next;
    bool reset;
    log_ring_init(&r, buf, sizeof(buf));

    /* Vide */
    CHECK(log_ring_read(&r, 0, out, sizeof(out), &next, &reset) == 0 && next == 0 && !reset && out[0] == '\0');

    /* Lecture complète puis incrémentale */
    log_ring_write(&r, "un\n", 3);
    log_ring_write(&r, "deux\n", 5);
    CHECK(log_ring_read(&r, 0, out, sizeof(out), &next, &reset) == 8 && next == 8 && !reset);
    CHECK_STR(out, "un\ndeux\n");
    log_ring_write(&r, "trois\n", 6);
    CHECK(log_ring_read(&r, next, out, sizeof(out), &next, &reset) == 6 && next == 14 && !reset);
    CHECK_STR(out, "trois\n");
    CHECK(log_ring_read(&r, next, out, sizeof(out), &next, &reset) == 0 && next == 14 && !reset);

    /* Tampon plein : les plus anciens sont écrasés, la lecture repart à une ligne entière */
    log_ring_write(&r, "quatre\ncinq\nsix\nsept\n", 21); /* 35 octets écrits pour 32 de place */
    CHECK(log_ring_read(&r, 0, out, sizeof(out), &next, &reset) > 0 && next == 35 && reset);
    CHECK_STR(out, "trois\nquatre\ncinq\nsix\nsept\n");
    /* Un lecteur en retard (position 2, écrasée) est prévenu */
    log_ring_read(&r, 2, out, sizeof(out), &next, &reset);
    CHECK(reset && strncmp(out, "trois\n", 6) == 0);
    /* Un lecteur à jour suit sans perte, même à cheval sur la fin du tampon */
    uint64_t pos = next;
    log_ring_write(&r, "huit\nneuf\n", 10);
    CHECK(log_ring_read(&r, pos, out, sizeof(out), &next, &reset) == 10 && !reset && next == 45);
    CHECK_STR(out, "huit\nneuf\n");
    /* Après un redémarrage, la position demandée dépasse le total : tout depuis le début */
    log_ring_read(&r, 1000, out, sizeof(out), &next, &reset);
    CHECK(reset && next == 45);

    /* Sortie plus petite que le texte : les octets les plus récents, en lignes entières */
    char small[12];
    log_ring_read(&r, 0, small, sizeof(small), &next, &reset);
    CHECK(reset);
    CHECK_STR(small, "huit\nneuf\n");
    char tiny[9];
    log_ring_read(&r, 0, tiny, sizeof(tiny), &next, &reset);
    CHECK_STR(tiny, "neuf\n");

    /* Écriture plus grande que le tampon : seule la fin est gardée */
    char big[100];
    for (int i = 0; i < 100; i++) {
        big[i] = (char)('a' + i % 26);
    }
    log_ring_write(&r, big, sizeof(big));
    log_ring_read(&r, 0, out, sizeof(out), &next, &reset);
    CHECK(next == 145 && reset);
    /* pas de saut de ligne : rien à sauter, les 32 derniers octets */
    CHECK(strlen(out) == 32 && memcmp(out, big + 68, 32) == 0);
}
