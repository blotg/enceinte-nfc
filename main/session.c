#include "session.h"

#include <string.h>

session_action_t session_on_card(const card_session_t *s, const char *uid, int64_t now_us, uint32_t queue_version,
                                 sess_player_state_t player, int64_t timeout_us)
{
    if (!s->uid[0] || strcmp(s->uid, uid) != 0) {
        return SESSION_START_NEW;
    }
    if (s->queue_version != queue_version || s->finished) {
        return SESSION_START_NEW;
    }
    switch (player) {
    case SESS_PLAYER_PLAYING:
        return SESSION_NOTHING;
    case SESS_PLAYER_PAUSED:
        if (s->removed && now_us - s->removed_at_us > timeout_us) {
            return SESSION_START_NEW;
        }
        return SESSION_RESUME;
    default:
        return SESSION_START_NEW;
    }
}

bool session_expired(const card_session_t *s, int64_t now_us, int64_t timeout_us)
{
    return s->uid[0] && s->removed && !s->finished && now_us - s->removed_at_us > timeout_us;
}
