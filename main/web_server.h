#pragma once
/*
 * Interface web d'administration (port 80) et API JSON.
 *  - assistant de première configuration (mot de passe administrateur, Wi-Fi) ;
 *  - connexion par mot de passe (cookie de session, protection CSRF par en-tête) ;
 *  - lecture, associations de cartes, gestion des fichiers, réglages, mises à jour ;
 *  - export et import des réglages en JSON ;
 *  - portail captif quand le point d'accès est actif.
 */
#include <stdbool.h>

#include "esp_err.h"

esp_err_t web_server_start(void);
/* Vrai pendant un envoi de fichier ou de firmware, et 5 min après (une mise à jour doit attendre). */
bool web_server_busy(void);
/* Envoi en cours (ou terminé il y a moins de 10 s : le navigateur envoie les fichiers l'un après l'autre). */
bool web_server_transfers_active(void);
/* Démarre ou arrête HTTPS selon le réglage enregistré. */
void web_server_https_apply(void);
