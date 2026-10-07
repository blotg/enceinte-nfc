#pragma once
/*
 * Serveur MPD (Music Player Daemon), port 6600, protocole 0.23.
 * Permet de piloter l'enceinte depuis n'importe quel client MPD (mpc, M.A.L.P.,
 * Cantata, ncmpcpp...). Les dossiers de premier niveau de la carte SD apparaissent
 * comme des listes de lecture (lecture seule).
 */
#include "esp_err.h"

esp_err_t mpd_server_start(void);
