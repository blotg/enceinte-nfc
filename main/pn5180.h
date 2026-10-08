#pragma once
/*
 * Pilote PN5180 (NXP) : bus SPI dédié (SPI3), signaux BUSY et RST.
 * Lit les cartes ISO 14443A (comme le PN532) et les étiquettes ISO 15693 (plus longue
 * portée). Champ RF coupé entre deux interrogations. Cf. pn5180_proto.h.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Bus et broches. ESP_ERR_NOT_SUPPORTED si les broches ne sont pas configurées (-1). */
esp_err_t pn5180_init(void);
/* Réinitialisation et détection. Description du lecteur (« PN5180, firmware 4.0 »). */
esp_err_t pn5180_setup(char *desc, size_t desc_len);
/* Cherche une carte. found=false si aucune carte (ce n'est pas une erreur). */
esp_err_t pn5180_read_uid(uint8_t *uid, uint8_t *uid_len, bool *found);
