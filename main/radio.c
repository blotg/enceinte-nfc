#include "radio.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Ligne suivante de text (sans fin de ligne ni espaces autour) ; NULL à la fin. */
static const char *next_line(const char *p, char *line, size_t len)
{
    if (!*p) {
        return NULL;
    }
    const char *end = p + strcspn(p, "\r\n");
    const char *a = p, *b = end;
    while (a < b && isspace((unsigned char)*a)) {
        a++;
    }
    while (b > a && isspace((unsigned char)b[-1])) {
        b--;
    }
    size_t n = (size_t)(b - a);
    if (n >= len) {
        line[0] = '\0'; /* trop longue : ignorée */
    } else {
        memcpy(line, a, n);
        line[n] = '\0';
    }
    while (*end == '\r' || *end == '\n') {
        end++;
    }
    return end;
}

int radio_playlist_urls(const char *text, void (*cb)(const char *url, void *ctx), void *ctx)
{
    char line[RADIO_URL_MAX + 16];
    int count = 0;
    for (const char *p = text; (p = next_line(p, line, sizeof(line))) != NULL;) {
        const char *url = line;
        if (strncasecmp(line, "file", 4) == 0 && isdigit((unsigned char)line[4])) { /* .pls : File1=... */
            const char *eq = strchr(line, '=');
            url = eq ? eq + 1 : "";
            while (isspace((unsigned char)*url)) {
                url++;
            }
        }
        if (line[0] == '#' || !radio_is_url(url) || strlen(url) >= RADIO_URL_MAX) {
            continue;
        }
        if (cb) {
            cb(url, ctx);
        }
        count++;
    }
    return count;
}

/* « schéma: » en tête (http:, https:...) ? */
static bool has_scheme(const char *s)
{
    if (!isalpha((unsigned char)s[0])) {
        return false;
    }
    for (const char *p = s + 1; *p; p++) {
        if (*p == ':') {
            return true;
        }
        if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.') {
            return false;
        }
    }
    return false;
}

bool url_resolve(const char *base, const char *ref, char *out, size_t len)
{
    if (!ref[0]) {
        return false;
    }
    int n;
    if (has_scheme(ref)) {
        n = snprintf(out, len, "%s", ref);
        return n > 0 && (size_t)n < len;
    }
    const char *auth = strstr(base, "://");
    if (!auth) {
        return false;
    }
    auth += 3;
    const char *path = auth + strcspn(auth, "/?#"); /* après l'hôte */
    size_t path_len = strcspn(path, "?#");          /* chemin sans la requête */
    if (ref[0] == '/' && ref[1] == '/') {
        n = snprintf(out, len, "%.*s%s", (int)(auth - 2 - base), base, ref); /* « https: » */
    } else if (ref[0] == '/') {
        n = snprintf(out, len, "%.*s%s", (int)(path - base), base, ref);
    } else if (ref[0] == '?') {
        n = snprintf(out, len, "%.*s%s", (int)(path + path_len - base), base, ref);
    } else {
        const char *slash = NULL;
        for (const char *p = path; p < path + path_len; p++) {
            if (*p == '/') {
                slash = p;
            }
        }
        n = slash ? snprintf(out, len, "%.*s%s", (int)(slash + 1 - base), base, ref)
                  : snprintf(out, len, "%.*s/%s", (int)(path - base), base, ref);
    }
    return n > 0 && (size_t)n < len;
}

bool radio_is_playlist_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    return !name_is_hidden(name) && ext &&
           (strcasecmp(ext, ".m3u") == 0 || strcasecmp(ext, ".m3u8") == 0 || strcasecmp(ext, ".pls") == 0);
}

#define PLAYLIST_FILE_MAX (16 * 1024)

int radio_playlist_file(const char *abs_path, void (*cb)(const char *url, void *ctx), void *ctx)
{
    FILE *f = fopen(abs_path, "rb");
    if (!f) {
        return 0;
    }
    char *text = malloc(PLAYLIST_FILE_MAX + 1);
    int count = 0;
    if (text) {
        size_t n = fread(text, 1, PLAYLIST_FILE_MAX, f);
        text[n] = '\0';
        count = radio_playlist_urls(text, cb, ctx);
        free(text);
    }
    fclose(f);
    return count;
}

typedef struct {
    char *out;
    size_t len;
    bool found;
} first_ctx_t;

static void keep_first(const char *url, void *ctx)
{
    first_ctx_t *f = ctx;
    if (!f->found) {
        str_copy(f->out, url, f->len);
        f->found = true;
    }
}

bool radio_playlist_first(const char *text, char *url, size_t len)
{
    first_ctx_t f = {.out = url, .len = len};
    radio_playlist_urls(text, keep_first, &f);
    return f.found;
}

/* Extension de l'adresse (avant « ? » et « # »), en minuscules. */
static void url_ext(const char *url, char *ext, size_t len)
{
    ext[0] = '\0';
    size_t n = strcspn(url, "?#");
    const char *slash = NULL;
    for (size_t i = 0; i < n; i++) {
        if (url[i] == '/') {
            slash = url + i;
        }
    }
    const char *dot = NULL;
    for (const char *q = slash ? slash : url; q < url + n; q++) {
        if (*q == '.') {
            dot = q;
        }
    }
    if (!dot || (size_t)(url + n - dot - 1) >= len) {
        return;
    }
    size_t i = 0;
    for (const char *q = dot + 1; q < url + n; q++) {
        ext[i++] = (char)tolower((unsigned char)*q);
    }
    ext[i] = '\0';
}

/* « what » (en minuscules) figure-t-il dans ct, sans tenir compte de la casse ? */
static bool ct_has(const char *ct, const char *what)
{
    if (!ct) {
        return false;
    }
    size_t n = strlen(what);
    for (const char *p = ct; *p; p++) {
        if (strncasecmp(p, what, n) == 0) {
            return true;
        }
    }
    return false;
}

bool radio_is_playlist(const char *ct, const char *url)
{
    if (ct_has(ct, "mpegurl") || ct_has(ct, "scpls") || ct_has(ct, "x-pls")) {
        return true;
    }
    if (ct_has(ct, "audio/") || ct_has(ct, "application/ogg")) {
        return false;
    }
    char ext[8];
    url_ext(url, ext, sizeof(ext));
    return strcmp(ext, "m3u") == 0 || strcmp(ext, "m3u8") == 0 || strcmp(ext, "pls") == 0;
}

audio_fmt_t radio_fmt(const char *ct, const char *url)
{
    if (ct_has(ct, "audio/mpeg") || ct_has(ct, "audio/mp3") || ct_has(ct, "audio/x-mpeg")) {
        return AUDIO_FMT_MP3;
    }
    if (ct_has(ct, "aac")) { /* audio/aac, audio/aacp, audio/x-aac */
        return AUDIO_FMT_AAC;
    }
    if (ct_has(ct, "ogg") || ct_has(ct, "opus") || ct_has(ct, "vorbis")) {
        return AUDIO_FMT_OGG;
    }
    if (ct_has(ct, "flac")) {
        return AUDIO_FMT_FLAC;
    }
    if (ct_has(ct, "audio/mp4") || ct_has(ct, "m4a")) {
        return AUDIO_FMT_M4A;
    }
    char ext[8], name[12];
    url_ext(url, ext, sizeof(ext));
    snprintf(name, sizeof(name), "x.%s", ext);
    audio_fmt_t f = ext[0] ? audio_fmt_from_name(name) : AUDIO_FMT_NONE;
    return f != AUDIO_FMT_NONE ? f : AUDIO_FMT_MP3; /* la plupart des radios */
}

void icy_init(icy_t *s, uint32_t metaint)
{
    memset(s, 0, sizeof(*s));
    s->metaint = metaint;
    s->until_meta = metaint;
}

size_t icy_strip(icy_t *s, uint8_t *buf, size_t len)
{
    if (!s->metaint) {
        return len;
    }
    size_t in = 0, out = 0;
    while (in < len) {
        if (s->in_len) {
            s->meta_left = (uint32_t)buf[in++] * 16;
            s->meta_len = 0;
            s->in_len = false;
            if (s->meta_left == 0) {
                s->until_meta = s->metaint;
            }
        } else if (s->meta_left) {
            size_t n = len - in < s->meta_left ? len - in : s->meta_left;
            memcpy(s->meta + s->meta_len, buf + in, n);
            s->meta_len += (uint32_t)n;
            s->meta_left -= (uint32_t)n;
            in += n;
            if (s->meta_left == 0) {
                s->meta[s->meta_len] = '\0';
                char title[RADIO_TITLE_MAX];
                if (icy_parse_title(s->meta, title, sizeof(title)) && strcmp(title, s->title) != 0) {
                    str_copy(s->title, title, sizeof(s->title));
                    s->title_changed = true;
                }
                s->until_meta = s->metaint;
            }
        } else {
            size_t n = len - in < s->until_meta ? len - in : s->until_meta;
            memmove(buf + out, buf + in, n);
            out += n;
            in += n;
            s->until_meta -= (uint32_t)n;
            if (s->until_meta == 0) {
                s->in_len = true;
            }
        }
    }
    return out;
}

static bool utf8_valid(const unsigned char *s)
{
    while (*s) {
        int n = *s < 0x80 ? 0 : (*s >> 5) == 6 ? 1 : (*s >> 4) == 14 ? 2 : (*s >> 3) == 30 ? 3 : -1;
        if (n < 0) {
            return false;
        }
        s++;
        for (int i = 0; i < n; i++, s++) {
            if ((*s & 0xC0) != 0x80) {
                return false;
            }
        }
    }
    return true;
}

void text_to_utf8(const char *in, char *out, size_t len)
{
    if (!len) {
        return;
    }
    if (utf8_valid((const unsigned char *)in)) {
        size_t n = strlen(in);
        if (n >= len) { /* coupe sans couper un caractère */
            n = len - 1;
            while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) {
                n--;
            }
        }
        memcpy(out, in, n);
        out[n] = '\0';
        return;
    }
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (*p < 0x80) {
            if (o + 1 >= len) {
                break;
            }
            out[o++] = (char)*p;
        } else {
            if (o + 2 >= len) {
                break;
            }
            out[o++] = (char)(0xC0 | (*p >> 6));
            out[o++] = (char)(0x80 | (*p & 0x3F));
        }
    }
    out[o] = '\0';
}

bool icy_parse_title(const char *meta, char *out, size_t len)
{
    const char *p = strstr(meta, "StreamTitle='");
    if (!p) {
        return false;
    }
    p += strlen("StreamTitle='");
    const char *end = strstr(p, "';"); /* le titre peut contenir des apostrophes */
    if (!end) {
        end = strrchr(p, '\'');
    }
    if (!end) {
        end = p + strlen(p);
    }
    char raw[RADIO_TITLE_MAX * 2];
    size_t n = (size_t)(end - p) < sizeof(raw) - 1 ? (size_t)(end - p) : sizeof(raw) - 1;
    memcpy(raw, p, n);
    raw[n] = '\0';
    while (n > 0 && isspace((unsigned char)raw[n - 1])) {
        raw[--n] = '\0';
    }
    text_to_utf8(raw, out, len);
    return true;
}
