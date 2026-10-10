#include "log_ring.h"

#include <string.h>

void log_ring_init(log_ring_t *r, char *buf, size_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->written = 0;
}

void log_ring_write(log_ring_t *r, const char *s, size_t n)
{
    if (!r->buf || !r->cap) {
        return;
    }
    if (n > r->cap) { /* seule la fin tient dans le tampon */
        r->written += n - r->cap;
        s += n - r->cap;
        n = r->cap;
    }
    size_t pos = (size_t)(r->written % r->cap);
    size_t first = r->cap - pos < n ? r->cap - pos : n;
    memcpy(r->buf + pos, s, first);
    memcpy(r->buf, s + first, n - first);
    r->written += n;
}

size_t log_ring_read(const log_ring_t *r, uint64_t since, char *out, size_t cap, uint64_t *next, bool *reset)
{
    *next = r->written;
    *reset = false;
    if (!cap) {
        return 0;
    }
    out[0] = '\0';
    if (!r->buf || !r->cap) {
        return 0;
    }
    uint64_t oldest = r->written > r->cap ? r->written - r->cap : 0;
    uint64_t from = since;
    if (since > r->written || since < oldest) {
        from = oldest; /* redémarrage, ou texte écrasé depuis la dernière lecture */
        *reset = since != 0 || oldest != 0;
    }
    if (r->written - from > cap - 1) {
        from = r->written - (cap - 1);
        *reset = true;
    }
    size_t len = (size_t)(r->written - from);
    size_t pos = (size_t)(from % r->cap);
    size_t first = r->cap - pos < len ? r->cap - pos : len;
    memcpy(out, r->buf + pos, first);
    memcpy(out + first, r->buf, len - first);
    out[len] = '\0';
    if (*reset && from > 0) { /* première ligne incomplète : on la saute */
        char *nl = memchr(out, '\n', len);
        if (nl) {
            size_t skip = (size_t)(nl + 1 - out);
            memmove(out, nl + 1, len - skip + 1);
            len -= skip;
        }
    }
    return len;
}
