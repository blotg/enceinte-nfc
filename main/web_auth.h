#pragma once
/*
 * Connexions à l'interface web (logique pure, testée sur PC).
 *
 * Une connexion est un jeton aléatoire envoyé au navigateur dans le cookie « sid ».
 * web_server.c enregistre la table en NVS : on reste connecté après un redémarrage
 * (mise à jour, coupure de courant). Une connexion est oubliée après 30 jours sans
 * visite ; chaque visite la prolonge, au plus une fois par jour pour ménager la flash.
 *
 * Les heures sont en secondes (heure réelle) ; 0 = heure inconnue (pas encore de NTP),
 * le jeton est alors accepté sans vérifier son âge.
 */
#include <stdbool.h>
#include <stdint.h>

#define WEB_AUTH_SLOTS 8
#define WEB_AUTH_KEY_LEN 16
#define WEB_AUTH_TOKEN_LEN (2 * WEB_AUTH_KEY_LEN) /* jeton en hexadécimal */
#define WEB_AUTH_IDLE_S (30 * 24 * 3600)
#define WEB_AUTH_RENEW_S (24 * 3600)

typedef struct {
    int64_t seen; /* dernière visite enregistrée, 0 si inconnue */
    uint8_t key[WEB_AUTH_KEY_LEN];
    uint8_t used;
} web_auth_slot_t;

typedef struct {
    web_auth_slot_t slot[WEB_AUTH_SLOTS];
} web_auth_t;

typedef enum {
    WEB_AUTH_DENIED,
    WEB_AUTH_OK,
    WEB_AUTH_RENEWED, /* acceptée et prolongée : table à enregistrer, cookie à renvoyer */
    WEB_AUTH_EXPIRED, /* refusée et oubliée : table à enregistrer */
} web_auth_result_t;

/* Ajoute une connexion (la plus ancienne est remplacée si la table est pleine) et écrit son jeton. */
void web_auth_add(web_auth_t *t, const uint8_t key[WEB_AUTH_KEY_LEN], int64_t now,
                  char token[WEB_AUTH_TOKEN_LEN + 1]);

web_auth_result_t web_auth_check(web_auth_t *t, const char *token, int64_t now);

/* Oublie la connexion de ce jeton ; vrai si elle existait. */
bool web_auth_remove(web_auth_t *t, const char *token);
