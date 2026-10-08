#pragma once
/*
 * Traitement du son, avant le volume (sans dépendance ESP-IDF, testé sur PC) :
 *  - normalisation : égalise le niveau d'un morceau et d'une playlist à l'autre. Le niveau
 *    sonore est mesuré en continu (blocs de 100 ms, silences et passages calmes ignorés) et
 *    le gain suit lentement vers le haut, vite vers le bas : un morceau plus fort est
 *    corrigé en une fraction de seconde, un morceau plus calme remonte en quelques secondes ;
 *  - compression : réduit les écarts de volume à l'intérieur d'un morceau (passages calmes
 *    remontés, passages forts atténués), sans remonter le souffle des silences ;
 *  - limiteur : empêche toute saturation quand l'un des deux augmente le gain.
 * Trois intensités pour chacun (1 légère, 2 moyenne, 3 forte), 0 = désactivé.
 * Signal mono, flottants dans [-1, 1].
 */
#include <stdbool.h>
#include <stdint.h>

#define DSP_LEVEL_MAX 3
#define DSP_TARGET_DB (-16.0f) /* niveau visé par la normalisation (puissance moyenne, dBFS) */
#define DSP_CEILING 0.89f      /* plafond du limiteur (-1 dBFS) */

typedef struct {
    uint32_t rate;
    uint8_t normalize, compress;
    /* normalisation */
    float hp_a, hp_x1, hp_y1;       /* passe-haut du détecteur (graves écartés de la mesure) */
    double blk_sum;
    uint32_t blk_n, blk_len;        /* blocs de mesure de 100 ms */
    float loud_pow;                 /* puissance moyenne estimée */
    uint32_t loud_blocks;           /* blocs mesurés depuis la remise à zéro */
    uint32_t blocks_since_reset;
    float agc_db, agc_target_db;
    /* compression */
    float env, env_att, env_rel;
    /* limiteur */
    float lim, lim_rel;
    /* gains interpolés par sous-blocs */
    float g_agc, g_agc_step, g_comp, g_comp_step;
    uint32_t sub;
} dsp_t;

void dsp_init(dsp_t *d, uint32_t rate);
/* Change la fréquence d'échantillonnage en gardant le niveau mesuré. */
void dsp_set_rate(dsp_t *d, uint32_t rate);
void dsp_set_levels(dsp_t *d, uint8_t normalize, uint8_t compress);
/* Nouvelle playlist : la mesure du niveau repart de zéro (gain neutre). */
void dsp_reset(dsp_t *d);
static inline bool dsp_active(const dsp_t *d)
{
    return d->normalize || d->compress;
}
void dsp_process(dsp_t *d, float *x, uint32_t n);
/* Gain de normalisation en cours (dB) : diagnostic et tests. */
float dsp_agc_db(const dsp_t *d);
