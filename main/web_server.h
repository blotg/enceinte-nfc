#pragma once
/*
 * Interface web d'administration (port 80) et API JSON.
 *  - assistant de première configuration (mot de passe administrateur, Wi-Fi) ;
 *  - connexion par mot de passe (cookie de session, protection CSRF par en-tête) ;
 *  - lecture, associations de cartes, gestion des fichiers, réglages, mises à jour ;
 *  - portail captif quand le point d'accès est actif.
 */
#include <stdbool.h>

#include "esp_err.h"

esp_err_t web_server_start(void);
/* Vrai pendant un envoi de fichier ou de firmware (une mise à jour doit attendre). */
bool web_server_busy(void);
