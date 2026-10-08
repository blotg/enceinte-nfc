#include "dsp.h"

#include <math.h>
#include <string.h>

#define BLOCK_S 0.1f        /* bloc de mesure du niveau */
#define SUB 32              /* échantillons entre deux calculs de gain */
#define GATE_ABS_DB (-50.0f) /* blocs plus calmes ignorés (silences) */
#define GATE_REL_DB (-20.0f) /* blocs 20 dB sous le niveau mesuré ignorés (passages calmes) */
#define LOUD_UP 0.15f        /* mesure : niveau en hausse suivi en ~0,6 s */
#define LOUD_DOWN 0.025f     /* niveau en baisse suivi en ~4 s */
#define ACQUIRE_BLOCKS 30    /* 3 s après une remise à zéro : montée rapide */
#define UP_DB_S 2.0f
#define UP_FAST_DB_S 8.0f
#define DOWN_DB_S 20.0f
#define MAX_TOTAL_DB 24.0f

/* Normalisation : part de l'écart corrigée, gain maximal, atténuation maximale */
static const struct {
    float strength, boost, cut;
} NORM[DSP_LEVEL_MAX + 1] = {{0, 0, 0}, {0.5f, 6, 9}, {0.75f, 9, 12}, {1.0f, 12, 16}};

/* Compression : seuil (dBFS), taux, gain de compensation (calculé à DSP_TARGET_DB) */
static const struct {
    float threshold, ratio;
} COMP[DSP_LEVEL_MAX + 1] = {{0, 1}, {-22, 2}, {-28, 3}, {-34, 5}};

static inline float db_to_lin(float db)
{
    return expf(db * 0.11512925f);
}

static inline float pow_to_db(float p)
{
    return 4.3429448f * logf(p + 1e-12f);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float one_pole(float tau_s, uint32_t rate)
{
    return 1.0f - expf(-1.0f / (tau_s * (float)rate));
}

static void coefficients(dsp_t *d)
{
    float rc = 1.0f / (2.0f * (float)M_PI * 100.0f); /* passe-haut à 100 Hz */
    float dt = 1.0f / (float)d->rate;
    d->hp_a = rc / (rc + dt);
    d->blk_len = (uint32_t)(BLOCK_S * (float)d->rate);
    d->env_att = one_pole(0.010f, d->rate);
    d->env_rel = one_pole(0.200f, d->rate);
    d->lim_rel = one_pole(0.100f, d->rate);
}

void dsp_init(dsp_t *d, uint32_t rate)
{
    memset(d, 0, sizeof(*d));
    d->rate = rate ? rate : 44100;
    d->g_agc = d->g_comp = d->lim = 1.0f;
    coefficients(d);
}

void dsp_set_rate(dsp_t *d, uint32_t rate)
{
    if (rate && rate != d->rate) {
        d->rate = rate;
        d->blk_sum = 0;
        d->blk_n = 0;
        coefficients(d);
    }
}

void dsp_reset(dsp_t *d)
{
    d->hp_x1 = d->hp_y1 = 0;
    d->blk_sum = 0;
    d->blk_n = 0;
    d->loud_pow = 0;
    d->loud_blocks = 0;
    d->blocks_since_reset = 0;
    d->agc_target_db = 0;
    d->agc_db = 0;
}

void dsp_set_levels(dsp_t *d, uint8_t normalize, uint8_t compress)
{
    normalize = normalize > DSP_LEVEL_MAX ? DSP_LEVEL_MAX : normalize;
    compress = compress > DSP_LEVEL_MAX ? DSP_LEVEL_MAX : compress;
    if (normalize && !d->normalize) {
        dsp_reset(d); /* mesure périmée */
    }
    if (!dsp_active(d)) {
        d->g_agc = d->g_comp = d->lim = 1.0f;
        d->g_agc_step = d->g_comp_step = 0;
        d->env = 0;
    }
    d->normalize = normalize;
    d->compress = compress;
}

float dsp_agc_db(const dsp_t *d)
{
    return d->agc_db;
}

/* Fin d'un bloc de 100 ms : mise à jour du niveau mesuré et du gain visé. */
static void agc_block(dsp_t *d)
{
    float ms = (float)(d->blk_sum / d->blk_n);
    d->blk_sum = 0;
    d->blk_n = 0;
    d->blocks_since_reset++;
    float level = pow_to_db(ms);
    if (level < GATE_ABS_DB || (d->loud_blocks && level < pow_to_db(d->loud_pow) + GATE_REL_DB)) {
        return;
    }
    if (d->loud_blocks < 10) {
        d->loud_pow += (ms - d->loud_pow) / (float)(d->loud_blocks + 1); /* moyenne de la première seconde */
    } else {
        d->loud_pow += (ms - d->loud_pow) * (ms > d->loud_pow ? LOUD_UP : LOUD_DOWN);
    }
    d->loud_blocks++;
    float correction = (DSP_TARGET_DB - pow_to_db(d->loud_pow)) * NORM[d->normalize].strength;
    d->agc_target_db = clampf(correction, -NORM[d->normalize].cut, NORM[d->normalize].boost);
}

static float comp_gain_db(const dsp_t *d, float level)
{
    float t = COMP[d->compress].threshold, r = COMP[d->compress].ratio;
    float reduction = level > t ? (level - t) * (1.0f / r - 1.0f) : 0.0f;
    float ref = DSP_TARGET_DB > t ? (DSP_TARGET_DB - t) * (1.0f - 1.0f / r) : 0.0f;
    /* Compensation pleine au-dessus de -55 dBFS, nulle sous -70 dBFS : le souffle et les
     * silences ne sont pas remontés. */
    float makeup = ref * clampf((level + 70.0f) / 15.0f, 0.0f, 1.0f);
    return reduction + makeup;
}

/* Tous les SUB échantillons : gains visés, atteints par interpolation linéaire. */
static void next_gains(dsp_t *d)
{
    float dt = (float)SUB / (float)d->rate;
    float agc = 0;
    if (d->normalize) {
        float up = d->blocks_since_reset < ACQUIRE_BLOCKS ? UP_FAST_DB_S : UP_DB_S;
        float delta = clampf(d->agc_target_db - d->agc_db, -DOWN_DB_S * dt, up * dt);
        d->agc_db += delta;
        agc = d->agc_db;
    }
    float comp = d->compress ? comp_gain_db(d, pow_to_db(d->env)) : 0.0f;
    if (agc + comp > MAX_TOTAL_DB) {
        comp = MAX_TOTAL_DB - agc;
    }
    d->g_agc_step = (db_to_lin(agc) - d->g_agc) / SUB;
    d->g_comp_step = (db_to_lin(comp) - d->g_comp) / SUB;
}

void dsp_process(dsp_t *d, float *x, uint32_t n)
{
    if (!dsp_active(d)) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (d->sub == 0) {
            next_gains(d);
        }
        d->sub = (d->sub + 1) % SUB;
        float in = x[i];
        if (d->normalize) {
            float hp = d->hp_a * (d->hp_y1 + in - d->hp_x1);
            d->hp_x1 = in;
            d->hp_y1 = hp;
            d->blk_sum += hp * hp;
            if (++d->blk_n >= d->blk_len) {
                agc_block(d);
            }
        }
        d->g_agc += d->g_agc_step;
        float y = in * d->g_agc;
        if (d->compress) {
            float e = y * y;
            d->env += (e - d->env) * (e > d->env ? d->env_att : d->env_rel);
        }
        d->g_comp += d->g_comp_step;
        y *= d->g_comp;
        /* Limiteur : attaque immédiate (jamais de dépassement), retour progressif. */
        d->lim += (1.0f - d->lim) * d->lim_rel;
        float a = fabsf(y);
        if (a * d->lim > DSP_CEILING) {
            d->lim = DSP_CEILING / a;
        }
        x[i] = y * d->lim;
    }
}
