#pragma once
/*
 * Détection des touches tactiles de volume (sans dépendance ESP-IDF, testée sur PC).
 *
 * Chaque touche est une électrode (par exemple une pièce de monnaie) collée sous le bois.
 * La mesure du capteur augmente quand un doigt s'approche : la touche est « appuyée » quand
 * elle dépasse sa valeur de repos d'au moins le seuil (en millièmes). Contre les appuis
 * involontaires des enfants :
 *  - il faut maintenir le doigt (hold_ms) avant le premier cran, puis un cran toutes les 300 ms ;
 *  - les deux touches à la fois (main posée à plat) : ignoré jusqu'à ce qu'elles soient lâchées ;
 *  - plus de 20 s d'appui (objet posé, humidité) : la valeur de repos est recalée et la touche
 *    ignorée jusqu'à ce qu'elle soit lâchée.
 * La valeur de repos suit lentement les variations dues à l'humidité et à la température.
 */
#include <stdbool.h>
#include <stdint.h>

#define TOUCH_KEYS 2 /* 0 : volume +, 1 : volume - */
#define TOUCH_REPEAT_MS 300
#define TOUCH_STUCK_MS 20000

typedef struct {
    float baseline;     /* valeur de repos */
    uint32_t value;     /* dernière mesure */
    bool touched;
    bool locked;        /* ignorée jusqu'au relâchement */
    uint32_t held_ms;
    uint32_t next_ms;   /* prochain cran */
} touch_key_t;

typedef struct {
    touch_key_t k[TOUCH_KEYS];
    uint16_t threshold_permille;
    uint16_t hold_ms;
    uint32_t age_ms; /* depuis l'initialisation : première seconde = étalonnage */
} touch_keys_t;

void touch_keys_init(touch_keys_t *t, uint16_t threshold_permille, uint16_t hold_ms);
void touch_keys_config(touch_keys_t *t, uint16_t threshold_permille, uint16_t hold_ms);
/* Nouvelles mesures des deux touches, dt_ms après les précédentes.
 * Retourne +1 (cran vers le haut), -1 (vers le bas) ou 0. */
int touch_keys_update(touch_keys_t *t, const uint32_t value[TOUCH_KEYS], uint32_t dt_ms);
/* Écart à la valeur de repos, en millièmes (diagnostic dans l'interface). */
int touch_keys_delta_permille(const touch_keys_t *t, int key);
