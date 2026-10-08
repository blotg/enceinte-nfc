#pragma once
/*
 * Commandes de volume sur l'enceinte, sur deux broches (volume + et volume -) :
 *  - boutons poussoirs entre la broche et GND (résistance de tirage interne) : un appui change
 *    le volume d'un cran ; maintenu, il continue tous les 150 ms ;
 *  - ou touches tactiles : une électrode (pièce de monnaie, disque de cuivre) collée sous le
 *    bois, reliée à la broche (canal tactile de l'ESP32-S3, GPIO 1 à 14). Il faut maintenir le
 *    doigt avant le premier cran (réglable), cf. touch_keys.h.
 * Le mode et les réglages des touches se changent à chaud (interface web). Le volume reste
 * entre 0 et le volume maximum. Hors lecture, un bip court au nouveau volume.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool touch;          /* mode tactile actif */
    bool touch_ok;       /* capteur tactile démarré */
    uint16_t threshold;  /* millièmes */
    struct {
        uint32_t value;
        uint32_t baseline;
        int delta_permille;
        bool touched;
    } key[2];            /* 0 : volume +, 1 : volume - */
} buttons_diag_t;

esp_err_t buttons_start(void);
/* Mesures des touches tactiles en direct (réglage du seuil depuis l'interface). */
void buttons_get_diag(buttons_diag_t *out);
