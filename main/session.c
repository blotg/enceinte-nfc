#include "session.h"

#include <string.h>

bool session_expired(const resume_point_t *p, const resume_policy_t *pol, int64_t now_us)
{
    return p->uid[0] && p->removed && !p->finished && pol->timeout_s > 0 &&
           now_us - p->removed_at_us > (int64_t)pol->timeout_s * 1000000;
}

session_action_t session_decide(const resume_point_t *p, const resume_policy_t *pol, int64_t now_us,
                                uint32_t queue_version, sess_player_state_t player, uint32_t card_seq_now)
{
    if (!p || !p->uid[0] || p->finished || session_expired(p, pol, now_us)) {
        return SESSION_START_NEW;
    }
    /* La file est-elle toujours celle de la carte ? Après un redémarrage elle n'existe plus,
     * sans qu'une autre carte ait été posée pour autant. */
    bool live = !p->restored && p->queue_version == queue_version;
    bool other = card_seq_now - p->card_seq > 1 || (!live && !p->restored);
    if (other && !pol->after_other) {
        return SESSION_START_NEW;
    }
    if (live) {
        switch (player) {
        case SESS_PLAYER_PLAYING:
            return SESSION_NOTHING; /* relancée depuis l'application pendant l'absence */
        case SESS_PLAYER_PAUSED:
            return SESSION_RESUME_LIVE;
        default:
            return SESSION_START_NEW; /* arrêt demandé : on repart du début */
        }
    }
    return p->track[0] ? SESSION_RESUME_SEEK : SESSION_START_NEW;
}

resume_policy_t session_policy(int32_t card_resume_s, int8_t card_resume_other, uint32_t default_timeout_s,
                               bool default_after_other)
{
    resume_policy_t pol = {
        .timeout_s = card_resume_s >= 0 ? (uint32_t)card_resume_s : default_timeout_s,
        .after_other = card_resume_other >= 0 ? card_resume_other == 1 : default_after_other,
    };
    return pol;
}

/* Générateur pseudo-aléatoire fixé une fois pour toutes (splitmix32) : un ordre mémorisé doit
 * se retrouver à l'identique après une mise à jour du firmware. */
static uint32_t next_rand(uint32_t *state)
{
    uint32_t z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

void session_shuffle(char **items, int count, uint32_t seed)
{
    uint32_t state = seed;
    for (int i = count - 1; i > 0; i--) {
        int j = (int)(next_rand(&state) % (uint32_t)(i + 1));
        char *t = items[i];
        items[i] = items[j];
        items[j] = t;
    }
}

/* Format v2 : version, drapeaux, position (u32), heure du retrait (i64), graine de l'ordre
 * aléatoire (u32), puis uid, dossier et morceau terminés par un octet nul. Entiers
 * petit-boutistes. Le format v1 (sans graine) est encore lu. */
#define RESUME_FORMAT 2
#define HEADER_V1 14
#define HEADER_V2 18

static void put_le(uint8_t *b, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t get_le(const uint8_t *b, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) {
        v = v << 8 | b[i];
    }
    return v;
}

size_t resume_encode(const resume_point_t *p, uint8_t *out, size_t cap)
{
    size_t lu = strlen(p->uid) + 1, lf = strlen(p->folder) + 1, lt = strlen(p->track) + 1;
    size_t need = HEADER_V2 + lu + lf + lt;
    if (need > cap) {
        return 0;
    }
    out[0] = RESUME_FORMAT;
    out[1] = (uint8_t)((p->removed ? 1 : 0) | (p->finished ? 2 : 0) | (p->shuffle ? 4 : 0));
    put_le(out + 2, p->position_ms, 4);
    put_le(out + 6, (uint64_t)p->removed_epoch, 8);
    put_le(out + 14, p->shuffle_seed, 4);
    memcpy(out + HEADER_V2, p->uid, lu);
    memcpy(out + HEADER_V2 + lu, p->folder, lf);
    memcpy(out + HEADER_V2 + lu + lf, p->track, lt);
    return need;
}

static bool get_str(const uint8_t *in, size_t len, size_t *pos, char *out, size_t cap)
{
    const uint8_t *end = memchr(in + *pos, 0, len - *pos);
    if (!end) {
        return false;
    }
    size_t n = (size_t)(end - (in + *pos));
    if (n >= cap) {
        return false;
    }
    memcpy(out, in + *pos, n + 1);
    *pos += n + 1;
    return true;
}

bool resume_decode(const uint8_t *in, size_t len, resume_point_t *p)
{
    memset(p, 0, sizeof(*p));
    size_t header = len >= 1 && in[0] == 1 ? HEADER_V1 : len >= 1 && in[0] == 2 ? HEADER_V2 : 0;
    if (!header || len < header) {
        return false;
    }
    p->removed = in[1] & 1;
    p->finished = in[1] & 2;
    p->position_ms = (uint32_t)get_le(in + 2, 4);
    p->removed_epoch = (int64_t)get_le(in + 6, 8);
    if (header == HEADER_V2) {
        p->shuffle = in[1] & 4;
        p->shuffle_seed = (uint32_t)get_le(in + 14, 4);
    }
    size_t pos = header;
    if (!get_str(in, len, &pos, p->uid, sizeof(p->uid)) || !get_str(in, len, &pos, p->folder, sizeof(p->folder)) ||
        !get_str(in, len, &pos, p->track, sizeof(p->track)) || !p->uid[0]) {
        memset(p, 0, sizeof(*p));
        return false;
    }
    p->restored = true;
    return true;
}
