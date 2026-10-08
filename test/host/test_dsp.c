/* Normalisation, compression et limiteur sur des signaux synthétiques (44,1 kHz). */
#include <math.h>
#include <stdlib.h>

#include "dsp.h"
#include "test.h"

#define RATE 44100

static float s_phase;
static uint32_t s_noise = 12345;

/* Sinusoïde de 1 kHz dont la puissance moyenne vaut level_db (dBFS). */
static void sine(float *x, uint32_t n, float level_db)
{
    float a = powf(10.0f, level_db / 20.0f) * sqrtf(2.0f);
    for (uint32_t i = 0; i < n; i++) {
        x[i] = a * sinf(s_phase);
        s_phase += 2.0f * (float)M_PI * 1000.0f / RATE;
        if (s_phase > 2.0f * (float)M_PI) {
            s_phase -= 2.0f * (float)M_PI;
        }
    }
}

static void noise(float *x, uint32_t n, float level_db)
{
    float a = powf(10.0f, level_db / 20.0f) * sqrtf(3.0f); /* uniforme : puissance a²/3 */
    for (uint32_t i = 0; i < n; i++) {
        s_noise = s_noise * 1664525u + 1013904223u;
        x[i] = a * ((float)(s_noise >> 8) / 8388608.0f - 1.0f);
    }
}

static float level(const float *x, uint32_t n)
{
    double sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum += (double)x[i] * x[i];
    }
    return 10.0f * log10f((float)(sum / n) + 1e-20f);
}

static float peak(const float *x, uint32_t n)
{
    float p = 0;
    for (uint32_t i = 0; i < n; i++) {
        p = fabsf(x[i]) > p ? fabsf(x[i]) : p;
    }
    return p;
}

/* Joue "seconds" secondes de sinusoïde à level_db ; retourne le niveau de sortie de la
 * dernière seconde et le crête maximal. */
static float run(dsp_t *d, float seconds, float level_db, float *max_peak, bool use_noise)
{
    static float buf[RATE / 10];
    float last = -200;
    float pk = 0;
    int blocks = (int)(seconds * 10);
    float acc = 0;
    int counted = 0;
    for (int b = 0; b < blocks; b++) {
        if (use_noise) {
            noise(buf, RATE / 10, level_db);
        } else {
            sine(buf, RATE / 10, level_db);
        }
        dsp_process(d, buf, RATE / 10);
        float p = peak(buf, RATE / 10);
        pk = p > pk ? p : pk;
        if (b >= blocks - 10) {
            acc += powf(10.0f, level(buf, RATE / 10) / 10.0f);
            counted++;
        }
    }
    if (counted) {
        last = 10.0f * log10f(acc / counted);
    }
    if (max_peak) {
        *max_peak = pk;
    }
    return last;
}

static void test_bypass(void)
{
    dsp_t d;
    dsp_init(&d, RATE);
    float a[1000], b[1000];
    sine(a, 1000, -10);
    for (int i = 0; i < 1000; i++) {
        b[i] = a[i];
    }
    dsp_process(&d, b, 1000);
    bool same = true;
    for (int i = 0; i < 1000; i++) {
        same = same && a[i] == b[i];
    }
    CHECK(same);
    CHECK(!dsp_active(&d));
}

static void test_normalize(void)
{
    dsp_t d;
    /* Forte : un morceau calme et un morceau fort arrivent au même niveau */
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 3, 0);
    float quiet = run(&d, 10, -26, NULL, false);
    CHECK_NEAR(quiet, DSP_TARGET_DB, 1);
    dsp_reset(&d);
    float pk;
    float loud = run(&d, 10, -6, &pk, false);
    CHECK_NEAR(loud, DSP_TARGET_DB, 1);
    CHECK(pk <= DSP_CEILING + 1e-4f);

    /* Légère : l'écart de 20 dB est seulement réduit de moitié */
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 1, 0);
    quiet = run(&d, 10, -26, NULL, false);
    dsp_reset(&d);
    loud = run(&d, 10, -6, NULL, false);
    CHECK_NEAR(loud - quiet, 10, 1);

    /* Gain plafonné : un morceau très calme n'est pas remonté de plus de 12 dB */
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 3, 0);
    CHECK_NEAR(run(&d, 12, -40, NULL, false), -28, 1);

    /* Après une nouvelle playlist, gain neutre : jamais plus fort qu'à l'origine */
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 3, 0);
    run(&d, 10, -30, NULL, false); /* gain +12 dB */
    CHECK(dsp_agc_db(&d) > 11);
    dsp_reset(&d);
    CHECK(dsp_agc_db(&d) == 0);
}

/* Morceau fort juste après un morceau calme (même playlist) : corrigé en moins de 2 s. */
static void test_loud_after_quiet(void)
{
    dsp_t d;
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 3, 0);
    run(&d, 10, -30, NULL, false);
    float pk;
    run(&d, 0.5f, -6, &pk, false);
    CHECK(pk <= DSP_CEILING + 1e-4f); /* le limiteur empêche toute saturation */
    float after = run(&d, 1.5f, -6, NULL, false);
    CHECK(after < DSP_TARGET_DB + 3);
    /* Puis un passage calme (-25 dB sous le niveau) ne fait pas remonter le gain */
    float g = dsp_agc_db(&d);
    run(&d, 3, -32, NULL, false);
    CHECK(dsp_agc_db(&d) <= g + 0.5f);
}

static void test_silence(void)
{
    dsp_t d;
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 3, 3);
    run(&d, 8, -20, NULL, false);
    float g = dsp_agc_db(&d);
    static float z[RATE];
    for (int i = 0; i < 5; i++) {
        for (int k = 0; k < RATE; k++) {
            z[k] = 0;
        }
        dsp_process(&d, z, RATE);
        CHECK(peak(z, RATE) == 0);
    }
    CHECK_NEAR(dsp_agc_db(&d) * 10, g * 10, 5);
    /* Souffle à -75 dBFS : pas remonté par la compression */
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 0, 3);
    CHECK(run(&d, 3, -75, NULL, true) < -72);
}

/* Alternance de passages forts et calmes (30 dB d'écart) : l'écart diminue avec l'intensité. */
static float comp_range(int lvl)
{
    dsp_t d;
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 0, (uint8_t)lvl);
    float loud = 0, quiet = 0;
    for (int i = 0; i < 4; i++) {
        loud = run(&d, 2, -8, NULL, false);
        quiet = run(&d, 2, -38, NULL, false);
    }
    return loud - quiet;
}

static void test_compress(void)
{
    float r1 = comp_range(1), r2 = comp_range(2), r3 = comp_range(3);
    CHECK(r1 < 26 && r1 > 18);
    CHECK(r2 < r1 - 4);
    CHECK(r3 < r2 - 4);
    CHECK(r3 > 3);
    /* Niveau moyen (cible) à peu près conservé */
    dsp_t d;
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 0, 2);
    CHECK_NEAR(run(&d, 3, DSP_TARGET_DB, NULL, false), DSP_TARGET_DB, 1);
}

static void test_limiter(void)
{
    dsp_t d;
    dsp_init(&d, RATE);
    dsp_set_levels(&d, 3, 3);
    static float x[RATE];
    float worst = 0;
    for (int s = 0; s < 6; s++) {
        for (int i = 0; i < RATE; i++) {
            x[i] = (i / 50) % 2 ? 1.0f : -1.0f; /* carré pleine échelle */
            if (s % 2) {
                x[i] *= 0.01f;
            }
        }
        dsp_process(&d, x, RATE);
        float p = peak(x, RATE);
        worst = p > worst ? p : worst;
    }
    CHECK(worst <= DSP_CEILING + 1e-4f);
    /* Changement de fréquence d'échantillonnage sans à-coup */
    dsp_set_rate(&d, 48000);
    CHECK(d.blk_len == 4800);
}

void test_dsp(void)
{
    test_bypass();
    test_normalize();
    test_loud_after_quiet();
    test_silence();
    test_compress();
    test_limiter();
}
