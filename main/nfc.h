#pragma once
/*
 * Tâche de détection des cartes : interrogation périodique du PN532,
 * anti-rebond du retrait, réinitialisation automatique du lecteur.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*nfc_event_cb_t)(bool present, const char *uid);

esp_err_t nfc_start(nfc_event_cb_t cb);
bool nfc_reader_ok(void);
uint32_t nfc_firmware_version(void);
/* UID de la carte actuellement posée ("" si aucune). */
void nfc_current_uid(char *out, size_t len);
