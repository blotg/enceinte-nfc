#include "session.h"

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
    bool live = p->queue_version == queue_version; /* la file est toujours celle de la carte */
    bool other = card_seq_now - p->card_seq > 1 || !live;
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
