#pragma once
/*
 * Copie des réglages et des associations sur la carte SD, export et import JSON.
 *
 * Carte SD :
 *  - /.enceinte.json : réglages généraux, mots de passe compris (Wi-Fi en clair,
 *    mots de passe administrateur et MPD sous forme d'empreintes) ;
 *  - <dossier>/.cartes.json : cartes associées à ce dossier et leurs réglages propres.
 *    Le fichier suit le dossier quand on le déplace, ici ou sur un ordinateur.
 * Ces fichiers sont réécrits à chaque modification. Au démarrage (et quand une carte SD est
 * remise en place), l'enceinte les charge : une carte SD clonée se comporte dans une autre
 * enceinte exactement comme dans la première. Sans /.enceinte.json (carte neuve, ou
 * firmware 1.3 et antérieurs), réglages et associations de l'enceinte y sont recopiés.
 *
 * Le volume courant et les positions de reprise restent dans la flash interne (écrits trop
 * souvent pour la carte SD).
 */
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/* Au démarrage, carte SD montée, avant les autres modules : réglages de la carte SD. */
void backup_boot(void);
/* Tâche : associations de la carte SD, cartes remises en place, observateurs. */
void backup_start(void);

/* Document d'export : réglages et associations. secrets : mots de passe compris. */
cJSON *backup_export(bool secrets);
/* Import d'un document d'export. Résumé (ou erreur) en français dans msg. *ip_test : une
 * nouvelle adresse IP est à confirmer (cf. wifi_mgr_ip_test). */
esp_err_t backup_import(const cJSON *doc, char *msg, size_t msglen, bool *ip_test);

/* Réinitialisation usine : efface les fichiers de réglages et d'associations de la carte SD. */
void backup_factory_reset(void);
