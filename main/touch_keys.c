#include "touch_keys.h"

#include <string.h>

#define CALIBRATION_MS 1000
#define TAU_FAST_MS 200.0f    /* étalonnage */
#define TAU_REST_MS 4000.0f   /* au repos */
#define TAU_NEAR_MS 30000.0f  /* entre le repos et le seuil : dérive lente, pas un doigt */

void touch_keys_init(touch_keys_t *t, uint16_t threshold_permille, uint16_t hold_ms)
{
    memset(t, 0, sizeof(*t));
    touch_keys_config(t, threshold_permille, hold_ms);
}

void touch_keys_config(touch_keys_t *t, uint16_t threshold_permille, uint16_t hold_ms)
{
    t->threshold_permille = threshold_permille ? threshold_permille : 1;
    t->hold_ms = hold_ms;
}

static float delta_of(const touch_key_t *k)
{
    return k->baseline > 0 ? ((float)k->value - k->baseline) / k->baseline : 0.0f;
}

int touch_keys_delta_permille(const touch_keys_t *t, int key)
{
    return (int)(delta_of(&t->k[key]) * 1000.0f + (delta_of(&t->k[key]) >= 0 ? 0.5f : -0.5f));
}

static void follow(touch_key_t *k, float tau_ms, uint32_t dt_ms)
{
    float a = (float)dt_ms / tau_ms;
    k->baseline += ((float)k->value - k->baseline) * (a > 1.0f ? 1.0f : a);
}

int touch_keys_update(touch_keys_t *t, const uint32_t value[TOUCH_KEYS], uint32_t dt_ms)
{
    float on = t->threshold_permille / 1000.0f, off = on * 0.6f;
    bool calibrating = t->age_ms < CALIBRATION_MS;
    t->age_ms += dt_ms;
    for (int i = 0; i < TOUCH_KEYS; i++) {
        touch_key_t *k = &t->k[i];
        k->value = value[i];
        if (k->baseline <= 0) {
            k->baseline = (float)value[i];
        }
        if (calibrating) {
            follow(k, TAU_FAST_MS, dt_ms);
            continue;
        }
        float d = delta_of(k);
        if (!k->touched) {
            if (d > on) {
                k->touched = true;
                k->held_ms = 0;
                k->next_ms = t->hold_ms;
            } else {
                follow(k, d < on * 0.5f ? TAU_REST_MS : TAU_NEAR_MS, dt_ms);
            }
        } else if (d < off) {
            k->touched = false;
            k->locked = false;
        }
    }
    if (calibrating) {
        return 0;
    }
    /* Main posée à plat sur les deux touches : rien jusqu'à ce qu'elles soient lâchées. */
    if (t->k[0].touched && t->k[1].touched) {
        t->k[0].locked = t->k[1].locked = true;
    }
    int step = 0;
    for (int i = 0; i < TOUCH_KEYS; i++) {
        touch_key_t *k = &t->k[i];
        if (!k->touched || k->locked) {
            continue;
        }
        k->held_ms += dt_ms;
        if (k->held_ms >= TOUCH_STUCK_MS) {
            /* Appui interminable : objet posé ou dérive. Nouvelle valeur de repos. */
            k->baseline = (float)k->value;
            k->locked = true;
            continue;
        }
        if (k->held_ms >= k->next_ms) {
            k->next_ms += TOUCH_REPEAT_MS;
            step = i == 0 ? 1 : -1;
        }
    }
    return step;
}
