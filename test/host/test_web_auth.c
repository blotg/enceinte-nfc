#include <string.h>

#include "test.h"
#include "web_auth.h"

#define DAY (24 * 3600LL)
#define T0 1760000000LL /* octobre 2025 */

static void key_of(uint8_t key[WEB_AUTH_KEY_LEN], uint8_t n)
{
    memset(key, 0, WEB_AUTH_KEY_LEN);
    key[0] = n;
    key[WEB_AUTH_KEY_LEN - 1] = 0xA5;
}

void test_web_auth(void)
{
    web_auth_t t = {0};
    uint8_t key[WEB_AUTH_KEY_LEN];
    char tok[WEB_AUTH_TOKEN_LEN + 1];
    char tok2[WEB_AUTH_TOKEN_LEN + 1];

    /* Jeton absent, vide, mal formé ou inconnu */
    CHECK(web_auth_check(&t, NULL, T0) == WEB_AUTH_DENIED);
    CHECK(web_auth_check(&t, "", T0) == WEB_AUTH_DENIED);
    CHECK(web_auth_check(&t, "00000000000000000000000000000000", T0) == WEB_AUTH_DENIED); /* case vide */
    key_of(key, 1);
    web_auth_add(&t, key, T0, tok);
    CHECK_STR(tok, "010000000000000000000000000000A5");
    CHECK(web_auth_check(&t, tok, T0) == WEB_AUTH_OK);
    CHECK(web_auth_check(&t, "010000000000000000000000000000a5", T0) == WEB_AUTH_DENIED);
    CHECK(web_auth_check(&t, "010000000000000000000000000000A", T0) == WEB_AUTH_DENIED);
    CHECK(web_auth_check(&t, "010000000000000000000000000000A5X", T0) == WEB_AUTH_DENIED);
    CHECK(web_auth_check(&t, "020000000000000000000000000000A5", T0) == WEB_AUTH_DENIED);

    /* Prolongée au plus une fois par jour */
    CHECK(web_auth_check(&t, tok, T0 + DAY - 1) == WEB_AUTH_OK);
    CHECK(web_auth_check(&t, tok, T0 + DAY) == WEB_AUTH_RENEWED);
    CHECK(web_auth_check(&t, tok, T0 + DAY + 3600) == WEB_AUTH_OK);
    /* 29 jours sans visite : encore valable (et prolongée) */
    CHECK(web_auth_check(&t, tok, T0 + 30 * DAY - 1) == WEB_AUTH_RENEWED);
    /* 30 jours sans visite : oubliée, même si elle revient ensuite */
    CHECK(web_auth_check(&t, tok, T0 + 60 * DAY - 1) == WEB_AUTH_EXPIRED);
    CHECK(web_auth_check(&t, tok, T0 + 60 * DAY) == WEB_AUTH_DENIED);
    CHECK(web_auth_check(&t, tok, 0) == WEB_AUTH_DENIED);

    /* Heure inconnue (démarrage sans NTP) : acceptée, datée à la première visite à l'heure */
    memset(&t, 0, sizeof(t));
    key_of(key, 2);
    web_auth_add(&t, key, 0, tok);
    CHECK(web_auth_check(&t, tok, 0) == WEB_AUTH_OK);
    CHECK(web_auth_check(&t, tok, T0) == WEB_AUTH_RENEWED);
    CHECK(web_auth_check(&t, tok, T0 + 10) == WEB_AUTH_OK);
    CHECK(web_auth_check(&t, tok, 0) == WEB_AUTH_OK);
    /* L'horloge recule : rien n'expire */
    CHECK(web_auth_check(&t, tok, T0 - 40 * DAY) == WEB_AUTH_OK);

    /* Déconnexion : seul ce jeton est oublié */
    key_of(key, 3);
    web_auth_add(&t, key, T0, tok2);
    CHECK(web_auth_remove(&t, tok));
    CHECK(!web_auth_remove(&t, tok));
    CHECK(web_auth_check(&t, tok, T0) == WEB_AUTH_DENIED);
    CHECK(web_auth_check(&t, tok2, T0) == WEB_AUTH_OK);
    CHECK(!web_auth_remove(&t, "zz"));

    /* Table pleine : la connexion la moins récemment vue est remplacée */
    memset(&t, 0, sizeof(t));
    char toks[WEB_AUTH_SLOTS][WEB_AUTH_TOKEN_LEN + 1];
    for (int i = 0; i < WEB_AUTH_SLOTS; i++) {
        key_of(key, (uint8_t)(10 + i));
        web_auth_add(&t, key, T0 + i * DAY, toks[i]);
    }
    CHECK(web_auth_check(&t, toks[0], T0 + 9 * DAY) == WEB_AUTH_RENEWED); /* la n° 1 devient la plus ancienne */
    key_of(key, 99);
    web_auth_add(&t, key, T0 + 9 * DAY, tok);
    CHECK(web_auth_check(&t, tok, T0 + 9 * DAY) == WEB_AUTH_OK);
    CHECK(web_auth_check(&t, toks[0], T0 + 9 * DAY) == WEB_AUTH_OK);
    CHECK(web_auth_check(&t, toks[1], T0 + 9 * DAY) == WEB_AUTH_DENIED);
    for (int i = 2; i < WEB_AUTH_SLOTS; i++) {
        CHECK(web_auth_check(&t, toks[i], T0 + 9 * DAY) != WEB_AUTH_DENIED);
    }
    /* Une case libérée est réutilisée avant de remplacer qui que ce soit */
    CHECK(web_auth_remove(&t, toks[5]));
    key_of(key, 100);
    web_auth_add(&t, key, T0 + 9 * DAY, tok2);
    for (int i = 2; i < WEB_AUTH_SLOTS; i++) {
        CHECK(i == 5 || web_auth_check(&t, toks[i], T0 + 9 * DAY) == WEB_AUTH_OK);
    }
    CHECK(web_auth_check(&t, tok, T0 + 9 * DAY) == WEB_AUTH_OK);
    CHECK(web_auth_check(&t, tok2, T0 + 9 * DAY) == WEB_AUTH_OK);
}
