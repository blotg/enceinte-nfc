#pragma once
/*
 * Pilote PN532 en mode HSU (UART 115200 bauds).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PN532_UID_MAX 10

esp_err_t pn532_init(void);
/* Réveil, configuration SAM, nombre d'essais borné. Retourne la version du firmware. */
esp_err_t pn532_setup(uint32_t *firmware_version);
esp_err_t pn532_rf_field(bool on);
/* Cherche une carte ISO14443A. found=false si aucune carte (ce n'est pas une erreur). */
esp_err_t pn532_read_uid(uint8_t *uid, uint8_t *uid_len, bool *found);
