#include "web_auth.h"

#include <stddef.h>
#include <string.h>

#include "util.h"

/* Comparaison en temps constant : la durée ne dit pas combien de caractères sont justes. */
static web_auth_slot_t *find(web_auth_t *t, const char *token)
{
    if (!token || strlen(token) != WEB_AUTH_TOKEN_LEN) {
        return NULL;
    }
    web_auth_slot_t *found = NULL;
    for (int i = 0; i < WEB_AUTH_SLOTS; i++) {
        web_auth_slot_t *s = &t->slot[i];
        char hex[WEB_AUTH_TOKEN_LEN + 1];
        bytes_to_hex(s->key, WEB_AUTH_KEY_LEN, hex);
        uint8_t diff = 0;
        for (int k = 0; k < WEB_AUTH_TOKEN_LEN; k++) {
            diff |= (uint8_t)(hex[k] ^ token[k]);
        }
        if (s->used && diff == 0) {
            found = s;
        }
    }
    return found;
}

void web_auth_add(web_auth_t *t, const uint8_t key[WEB_AUTH_KEY_LEN], int64_t now,
                  char token[WEB_AUTH_TOKEN_LEN + 1])
{
    web_auth_slot_t *s = NULL;
    for (int i = 0; i < WEB_AUTH_SLOTS && !s; i++) {
        if (!t->slot[i].used) {
            s = &t->slot[i];
        }
    }
    if (!s) {
        s = &t->slot[0]; /* la plus ancienne est remplacée */
        for (int i = 1; i < WEB_AUTH_SLOTS; i++) {
            if (t->slot[i].seen < s->seen) {
                s = &t->slot[i];
            }
        }
    }
    memset(s, 0, sizeof(*s));
    memcpy(s->key, key, WEB_AUTH_KEY_LEN);
    s->seen = now;
    s->used = 1;
    bytes_to_hex(key, WEB_AUTH_KEY_LEN, token);
}

web_auth_result_t web_auth_check(web_auth_t *t, const char *token, int64_t now)
{
    web_auth_slot_t *s = find(t, token);
    if (!s) {
        return WEB_AUTH_DENIED;
    }
    if (!now) {
        return WEB_AUTH_OK;
    }
    if (s->seen && now - s->seen >= WEB_AUTH_IDLE_S) {
        memset(s, 0, sizeof(*s));
        return WEB_AUTH_EXPIRED;
    }
    if (!s->seen || now - s->seen >= WEB_AUTH_RENEW_S) {
        s->seen = now;
        return WEB_AUTH_RENEWED;
    }
    return WEB_AUTH_OK;
}

bool web_auth_remove(web_auth_t *t, const char *token)
{
    web_auth_slot_t *s = find(t, token);
    if (s) {
        memset(s, 0, sizeof(*s));
    }
    return s != NULL;
}
