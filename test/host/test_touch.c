/* Touches tactiles : appuis brefs ignorés, maintien, main à plat, dérive, objet posé. */
#include "test.h"
#include "touch_keys.h"

#define TICK 20
#define BASE 30000u

/* Fait évoluer les deux touches pendant ms millisecondes ; retourne la somme des crans. */
static int run(touch_keys_t *t, uint32_t plus, uint32_t minus, int ms, int *ups, int *downs)
{
    int sum = 0;
    for (int e = 0; e < ms; e += TICK) {
        uint32_t v[2] = {plus, minus};
        int s = touch_keys_update(t, v, TICK);
        sum += s;
        if (ups && s > 0) {
            (*ups)++;
        }
        if (downs && s < 0) {
            (*downs)++;
        }
    }
    return sum;
}

static uint32_t pct(double p)
{
    return (uint32_t)(BASE * (1.0 + p / 100.0));
}

void test_touch(void)
{
    touch_keys_t t;
    touch_keys_init(&t, 20, 800); /* seuil 2 %, maintien 0,8 s */
    CHECK(run(&t, BASE, BASE, 3000, NULL, NULL) == 0); /* étalonnage puis repos */

    /* Effleurement de 0,3 s : rien */
    CHECK(run(&t, pct(5), BASE, 300, NULL, NULL) == 0);
    CHECK(run(&t, BASE, BASE, 500, NULL, NULL) == 0);

    /* Maintien de 1,2 s sur « + » : un cran à 0,8 s, un autre à 1,1 s */
    int ups = 0, downs = 0;
    run(&t, pct(5), BASE, 1200, &ups, &downs);
    CHECK(ups == 2 && downs == 0);
    run(&t, BASE, BASE, 500, NULL, NULL);
    CHECK(touch_keys_delta_permille(&t, 0) == 0);

    /* « - » maintenue 0,9 s : un cran vers le bas */
    ups = downs = 0;
    run(&t, BASE, pct(4), 900, &ups, &downs);
    CHECK(downs == 1 && ups == 0);
    run(&t, BASE, BASE, 500, NULL, NULL);

    /* Main à plat sur les deux : rien, même longtemps */
    CHECK(run(&t, pct(6), pct(6), 3000, NULL, NULL) == 0);
    /* puis une seule reste touchée : toujours rien tant qu'elle n'a pas été lâchée */
    CHECK(run(&t, pct(6), BASE, 2000, NULL, NULL) == 0);
    run(&t, BASE, BASE, 500, NULL, NULL);
    ups = 0;
    run(&t, pct(6), BASE, 900, &ups, NULL);
    CHECK(ups == 1);
    run(&t, BASE, BASE, 500, NULL, NULL);

    /* Sous le seuil (1,5 % pour un seuil de 2 %) : rien */
    CHECK(run(&t, pct(1.5), BASE, 2000, NULL, NULL) == 0);
    run(&t, BASE, BASE, 2000, NULL, NULL);

    /* Humidité : dérive de +10 % en 5 minutes, aucun cran */
    int sum = 0;
    for (int s = 0; s < 300; s++) {
        sum += run(&t, pct(10.0 * s / 300), pct(10.0 * s / 300), 1000, NULL, NULL);
    }
    CHECK(sum == 0);
    /* et un vrai appui fonctionne toujours au nouveau niveau */
    ups = 0;
    run(&t, pct(15), pct(10), 900, &ups, NULL);
    CHECK(ups == 1);
    run(&t, pct(10), pct(10), 2000, NULL, NULL);

    /* Objet posé sur « + » : quelques crans, puis plus rien après 20 s ; une fois l'objet
     * retiré, la touche refonctionne */
    ups = 0;
    run(&t, pct(18), pct(10), 30000, &ups, NULL);
    int expected = 1 + (TOUCH_STUCK_MS - 800) / TOUCH_REPEAT_MS;
    CHECK(ups >= expected - 1 && ups <= expected + 1);
    ups = 0;
    run(&t, pct(18), pct(10), 5000, &ups, NULL);
    CHECK(ups == 0);
    run(&t, pct(10), pct(10), 8000, NULL, NULL); /* objet retiré, valeur de repos revenue */
    ups = 0;
    run(&t, pct(15), pct(10), 900, &ups, NULL);
    CHECK(ups == 1);

    /* Réglages modifiés à chaud : maintien 0,3 s */
    run(&t, pct(10), pct(10), 500, NULL, NULL);
    touch_keys_config(&t, 20, 300);
    ups = 0;
    run(&t, pct(15), pct(10), 400, &ups, NULL);
    CHECK(ups == 1);
}
