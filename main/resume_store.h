#pragma once
/*
 * Points de reprise en mémoire permanente (partition NVS "cfg"). Par carte : une clé
 * "rpN" (point complet) et une clé "rqN" (position seule, écrite souvent).
 * Les écritures sont espacées par le contrôleur pour ménager la flash.
 */
#include "esp_err.h"
#include "session.h"

/* Charge les points enregistrés dans le tableau (emplacements vides sinon). */
void resume_store_load(resume_point_t *points, int count);
/* Enregistre tout le point (carte, dossier, morceau, position...) : ~600 octets. */
esp_err_t resume_store_save(int slot, const resume_point_t *p);
/* Enregistre seulement la position (clé de 32 octets), pour les mises à jour fréquentes
 * pendant la lecture d'un même morceau. */
esp_err_t resume_store_save_position(int slot, uint32_t position_ms);
esp_err_t resume_store_erase(int slot);
