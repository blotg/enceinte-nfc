#pragma once
/*
 * Points de reprise en mémoire permanente (partition NVS "cfg", une clé par carte).
 * Les écritures sont espacées par le contrôleur pour ménager la flash.
 */
#include "esp_err.h"
#include "session.h"

/* Charge les points enregistrés dans le tableau (emplacements vides sinon). */
void resume_store_load(resume_point_t *points, int count);
esp_err_t resume_store_save(int slot, const resume_point_t *p);
esp_err_t resume_store_erase(int slot);
