#pragma once
/*
 * Tâche de détection des cartes : détection automatique du lecteur (PN5180 puis PN532),
 * interrogation périodique, anti-rebond du retrait, réinitialisation automatique.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*nfc_event_cb_t)(bool present, const char *uid);

esp_err_t nfc_start(nfc_event_cb_t cb);
bool nfc_reader_ok(void);
/* Lecteur détecté (« PN5180, firmware 4.0 »), "" si aucun lecteur ne répond. */
void nfc_reader_desc(char *out, size_t len);
/* UID de la carte actuellement posée ("" si aucune). */
void nfc_current_uid(char *out, size_t len);
