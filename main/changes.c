#include "changes.h"

#include <strings.h>

static uint32_t s_counters[CHG_COUNT];

static const char *const s_names[CHG_COUNT] = {
    [CHG_DATABASE] = "database",
    [CHG_UPDATE] = "update",
    [CHG_STORED_PLAYLIST] = "stored_playlist",
    [CHG_PLAYLIST] = "playlist",
    [CHG_PLAYER] = "player",
    [CHG_MIXER] = "mixer",
    [CHG_OUTPUT] = "output",
    [CHG_OPTIONS] = "options",
};

void changes_notify(change_t c)
{
    if (c < CHG_COUNT) {
        __atomic_add_fetch(&s_counters[c], 1, __ATOMIC_RELAXED);
    }
}

void changes_snapshot(uint32_t out[CHG_COUNT])
{
    for (int i = 0; i < CHG_COUNT; i++) {
        out[i] = __atomic_load_n(&s_counters[i], __ATOMIC_RELAXED);
    }
}

const char *changes_name(change_t c)
{
    return c < CHG_COUNT ? s_names[c] : "";
}

int changes_from_name(const char *name)
{
    for (int i = 0; i < CHG_COUNT; i++) {
        if (strcasecmp(name, s_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}
