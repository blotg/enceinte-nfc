#pragma once
/*
 * Mises à jour du firmware.
 *
 * Automatique : au démarrage puis périodiquement, depuis
 *  - un dépôt GitHub "https://github.com/<compte>/<dépôt>" : fichier enceinte.bin de la
 *    dernière release stable, dont la version est lue dans l'en-tête du binaire ;
 *  - ou un manifeste JSON { "version": "1.2.0", "url": "enceinte-1.2.0.bin" }
 *    (url absolue ou relative au manifeste).
 * Si la version est plus récente, la mise à jour est téléchargée dès que l'enceinte est
 * inactive, puis l'enceinte redémarre.
 *
 * Sécurité : si le nouveau firmware ne démarre pas correctement (plantage avant
 * validation), le chargeur de démarrage revient automatiquement à l'ancien.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    OTA_IDLE = 0,
    OTA_CHECKING,
    OTA_WAITING_IDLE, /* mise à jour disponible, attend que la lecture soit finie */
    OTA_DOWNLOADING,
    OTA_REBOOTING,
    OTA_ERROR,
} ota_state_t;

typedef struct {
    ota_state_t state;
    char current_version[32];
    char available_version[32];
    char message[128];
    int progress; /* % */
    int64_t last_check; /* heure (time_t), 0 si jamais */
} ota_status_t;

void ota_start(void);
/* Validation du firmware après un démarrage réussi (annule le retour arrière). */
void ota_confirm_boot(void);
void ota_check_now(void);
void ota_get_status(ota_status_t *st);

/* Envoi manuel d'un firmware (.bin) depuis l'interface web. */
esp_err_t ota_upload_begin(size_t size);
esp_err_t ota_upload_write(const uint8_t *data, size_t len);
esp_err_t ota_upload_end(bool commit, char *msg, size_t msglen);

/* Redémarrage différé (laisse le temps de répondre à la requête HTTP). */
void ota_schedule_restart(uint32_t delay_ms);
