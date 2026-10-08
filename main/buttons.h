#pragma once
/*
 * Boutons poussoirs de volume (entre la broche et GND, résistance de tirage interne).
 * Un appui change le volume d'un cran ; maintenu, il continue tous les 150 ms. Le volume
 * reste entre 0 et le volume maximum. Hors lecture, un bip court au nouveau volume.
 */
#include "esp_err.h"

esp_err_t buttons_start(void);
