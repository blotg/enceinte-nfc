#include "rss.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "radio.h"
#include "util.h"

#define TAG_MAX 1536 /* contenu d'une balise (<enclosure url="..." .../>) */
#define TEXT_MAX 512 /* texte gardé pour un champ */

typedef enum {
    S_TEXT,
    S_TAG,
    S_COMMENT,
    S_CDATA,
} state_t;

typedef enum {
    F_NONE,
    F_CHANNEL_TITLE,
    F_TITLE,
    F_GUID,
    F_PUBDATE,
} field_t;

struct rss_parser {
    rss_item_cb_t cb;
    void *ctx;
    state_t state;
    char tag[TAG_MAX];
    size_t tag_len;
    char quote; /* guillemet ouvert dans la balise */
    char text[TEXT_MAX];
    size_t text_len;
    field_t field;
    int tail; /* fin de commentaire « --> » ou de CDATA « ]]> » : caractères reconnus */
    bool in_channel, in_item, in_image, have_channel_title, stopped;
    char channel_title[RSS_TITLE_MAX];
    char pubdate[64];
    rss_item_t item;
};

rss_parser_t *rss_new(rss_item_cb_t cb, void *ctx)
{
    rss_parser_t *p = calloc(1, sizeof(*p));
    if (p) {
        p->cb = cb;
        p->ctx = ctx;
    }
    return p;
}

void rss_free(rss_parser_t *p)
{
    free(p);
}

const char *rss_channel_title(const rss_parser_t *p)
{
    return p->channel_title;
}

static void put_utf8(char *out, size_t *o, size_t cap, unsigned long cp)
{
    char b[4];
    int n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else if (cp < 0x110000) {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    } else {
        return;
    }
    if (*o + (size_t)n < cap) {
        memcpy(out + *o, b, (size_t)n);
        *o += (size_t)n;
    }
}

/* Entités XML décodées, espaces réunis et retirés aux extrémités. */
static void xml_text(const char *in, size_t len, char *out, size_t cap)
{
    size_t o = 0;
    bool space = true;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '&') {
            const char *semi = memchr(in + i, ';', len - i < 12 ? len - i : 12);
            if (semi) {
                const char *e = in + i + 1;
                size_t el = (size_t)(semi - e);
                unsigned long cp = 0;
                bool ok = true;
                if (el && e[0] == '#') {
                    char num[12];
                    memcpy(num, e + 1, el - 1);
                    num[el - 1] = '\0';
                    cp = (num[0] == 'x' || num[0] == 'X') ? strtoul(num + 1, NULL, 16) : strtoul(num, NULL, 10);
                    ok = cp > 0;
                } else if (el == 3 && memcmp(e, "amp", 3) == 0) {
                    cp = '&';
                } else if (el == 2 && memcmp(e, "lt", 2) == 0) {
                    cp = '<';
                } else if (el == 2 && memcmp(e, "gt", 2) == 0) {
                    cp = '>';
                } else if (el == 4 && memcmp(e, "quot", 4) == 0) {
                    cp = '"';
                } else if (el == 4 && memcmp(e, "apos", 4) == 0) {
                    cp = '\'';
                } else {
                    ok = false;
                }
                if (ok) {
                    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0) {
                        if (!space) {
                            out[o++] = ' ';
                        }
                        space = true;
                    } else {
                        put_utf8(out, &o, cap, cp);
                        space = false;
                    }
                    i = (size_t)(semi - in);
                    continue;
                }
            }
        }
        if (isspace(c)) {
            if (!space) {
                out[o++] = ' ';
            }
            space = true;
            continue;
        }
        out[o++] = (char)c;
        space = false;
    }
    while (o > 0 && out[o - 1] == ' ') {
        o--;
    }
    out[o] = '\0';
}

/* Valeur d'un attribut de la balise (entités décodées). */
static bool attr(const char *tag, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = tag; (p = strstr(p, name)) != NULL; p += nl) {
        if (p > tag && !isspace((unsigned char)p[-1])) {
            continue;
        }
        const char *q = p + nl;
        while (isspace((unsigned char)*q)) {
            q++;
        }
        if (*q != '=') {
            continue;
        }
        q++;
        while (isspace((unsigned char)*q)) {
            q++;
        }
        if (*q != '"' && *q != '\'') {
            continue;
        }
        const char *end = strchr(q + 1, *q);
        if (!end) {
            return false;
        }
        xml_text(q + 1, (size_t)(end - q - 1), out, cap);
        return true;
    }
    return false;
}

static void end_item(rss_parser_t *p)
{
    rss_item_t *it = &p->item;
    if (it->url[0] && !p->stopped) {
        if (!it->guid[0]) {
            str_copy(it->guid, it->url, sizeof(it->guid));
        }
        if (!rss_parse_date(p->pubdate, &it->pub, it->day, it->hm)) {
            it->pub = 0;
            it->day[0] = it->hm[0] = '\0';
        }
        if (p->cb && !p->cb(it, p->ctx)) {
            p->stopped = true;
        }
    }
}

static void field_done(rss_parser_t *p)
{
    char buf[TEXT_MAX];
    xml_text(p->text, p->text_len, buf, sizeof(buf));
    switch (p->field) {
    case F_CHANNEL_TITLE:
        str_copy(p->channel_title, buf, sizeof(p->channel_title));
        p->have_channel_title = true;
        break;
    case F_TITLE:
        str_copy(p->item.title, buf, sizeof(p->item.title));
        break;
    case F_GUID:
        str_copy(p->item.guid, buf, sizeof(p->item.guid));
        break;
    case F_PUBDATE:
        str_copy(p->pubdate, buf, sizeof(p->pubdate));
        break;
    default:
        break;
    }
    p->field = F_NONE;
}

static void handle_tag(rss_parser_t *p)
{
    char *t = p->tag;
    t[p->tag_len] = '\0';
    if (t[0] == '?' || t[0] == '!') {
        return; /* <?xml ...?>, <!DOCTYPE ...> */
    }
    bool closing = t[0] == '/';
    bool selfclose = p->tag_len > 0 && t[p->tag_len - 1] == '/';
    const char *name = closing ? t + 1 : t;
    size_t nl = strcspn(name, " \t\r\n/");
    char n[32];
    if (nl >= sizeof(n)) {
        return;
    }
    memcpy(n, name, nl);
    n[nl] = '\0';
    if (closing) {
        if (p->field != F_NONE) {
            field_done(p);
        }
        if (strcmp(n, "item") == 0 && p->in_item) {
            end_item(p);
            p->in_item = false;
        } else if (strcmp(n, "image") == 0) {
            p->in_image = false;
        } else if (strcmp(n, "channel") == 0) {
            p->in_channel = false;
        }
        return;
    }
    if (strcmp(n, "channel") == 0) {
        p->in_channel = true;
    } else if (strcmp(n, "image") == 0 && !selfclose) {
        p->in_image = true;
    } else if (strcmp(n, "item") == 0) {
        p->in_item = true;
        memset(&p->item, 0, sizeof(p->item));
        p->pubdate[0] = '\0';
    } else if (p->in_item && strcmp(n, "enclosure") == 0 && !p->item.url[0]) {
        char url[RSS_URL_MAX];
        if (attr(t, "url", url, sizeof(url)) && radio_is_url(url)) {
            str_copy(p->item.url, url, sizeof(p->item.url));
            attr(t, "type", p->item.type, sizeof(p->item.type));
        }
    } else if (!selfclose) {
        field_t f = F_NONE;
        if (p->in_item) {
            f = strcmp(n, "title") == 0 ? F_TITLE : strcmp(n, "guid") == 0 ? F_GUID : strcmp(n, "pubDate") == 0 ? F_PUBDATE : F_NONE;
        } else if (p->in_channel && !p->in_image && !p->have_channel_title && strcmp(n, "title") == 0) {
            f = F_CHANNEL_TITLE;
        }
        if (f != F_NONE) {
            p->field = f;
            p->text_len = 0;
        }
    }
}

static void text_char(rss_parser_t *p, char c)
{
    if (p->field != F_NONE && p->text_len < sizeof(p->text)) {
        p->text[p->text_len++] = c;
    }
}

bool rss_feed(rss_parser_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len && !p->stopped; i++) {
        char c = data[i];
        switch (p->state) {
        case S_TEXT:
            if (c == '<') {
                p->state = S_TAG;
                p->tag_len = 0;
                p->quote = 0;
            } else {
                text_char(p, c);
            }
            break;
        case S_TAG:
            if (p->quote) {
                if (c == p->quote) {
                    p->quote = 0;
                }
            } else if (c == '"' || c == '\'') {
                p->quote = c;
            } else if (c == '>') {
                handle_tag(p);
                p->state = S_TEXT;
                break;
            }
            if (p->tag_len < sizeof(p->tag) - 1) {
                p->tag[p->tag_len++] = c;
            }
            if (p->tag_len == 3 && memcmp(p->tag, "!--", 3) == 0) {
                p->state = S_COMMENT;
                p->tail = 0;
            } else if (p->tag_len == 8 && memcmp(p->tag, "![CDATA[", 8) == 0) {
                p->state = S_CDATA;
                p->tail = 0;
            }
            break;
        case S_COMMENT:
            if (c == '-') {
                p->tail = p->tail < 2 ? p->tail + 1 : 2;
            } else if (c == '>' && p->tail == 2) {
                p->state = S_TEXT;
            } else {
                p->tail = 0;
            }
            break;
        case S_CDATA:
            if (c == ']') {
                if (p->tail == 2) {
                    text_char(p, ']'); /* « ]]] » : le premier fait partie du texte */
                } else {
                    p->tail++;
                }
            } else if (c == '>' && p->tail == 2) {
                p->state = S_TEXT;
            } else {
                for (; p->tail > 0; p->tail--) {
                    text_char(p, ']');
                }
                text_char(p, c);
            }
            break;
        }
    }
    return !p->stopped;
}

/* Jours depuis le 1er janvier 1970 (calendrier grégorien). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool rss_parse_date(const char *s, int64_t *epoch, char day[11], char hm[6])
{
    static const char *months = "janfebmaraprmayjunjulaugsepoctnovdec";
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    long off = 0; /* secondes */
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (isdigit((unsigned char)s[0]) && sscanf(s, "%4d-%2d-%2d", &y, &mo, &d) == 3) { /* ISO 8601 */
        const char *t = strpbrk(s, "Tt ");
        if (t && sscanf(t + 1, "%2d:%2d:%2d", &h, &mi, &sec) < 2) {
            h = mi = sec = 0;
        }
        const char *z = t ? t + 1 + strspn(t + 1, "0123456789:.") : NULL;
        if (z && (*z == '+' || *z == '-') && isdigit((unsigned char)z[1])) {
            int zh = 0, zm = 0;
            sscanf(z + 1, "%2d:%2d", &zh, &zm);
            off = (*z == '-' ? -1 : 1) * (zh * 3600L + zm * 60L);
        }
    } else {
        const char *comma = strchr(s, ',');
        if (comma) {
            s = comma + 1;
        }
        char mon[16] = "", zone[8] = "";
        int used = 0;
        if (sscanf(s, "%d %15s %d%n", &d, mon, &y, &used) != 3) {
            return false;
        }
        const char *r = s + used;
        int n = 0;
        if (sscanf(r, " %d:%d%n", &h, &mi, &n) == 2) { /* heure, secondes facultatives */
            r += n;
            if (*r == ':' && sscanf(r + 1, "%d%n", &sec, &n) == 1) {
                r += 1 + n;
            }
            sscanf(r, " %7s", zone);
        }
        const char *f = NULL;
        if (strlen(mon) >= 3) {
            char m3[4] = {(char)tolower((unsigned char)mon[0]), (char)tolower((unsigned char)mon[1]),
                          (char)tolower((unsigned char)mon[2]), 0};
            f = strstr(months, m3);
        }
        if (!f || (f - months) % 3) {
            return false;
        }
        mo = (int)(f - months) / 3 + 1;
        if (y < 100) {
            y += y < 70 ? 2000 : 1900;
        }
        if (zone[0] == '+' || zone[0] == '-') {
            long z = strtol(zone + 1, NULL, 10);
            off = (zone[0] == '-' ? -1 : 1) * ((z / 100) * 3600L + (z % 100) * 60L);
        } else {
            static const struct {
                const char *name;
                int hours;
            } zones[] = {{"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5}, {"MST", -7}, {"MDT", -6},
                         {"PST", -8}, {"PDT", -7}, {"CET", 1},  {"CEST", 2}, {"BST", 1}};
            for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
                if (strcasecmp(zone, zones[i].name) == 0) {
                    off = zones[i].hours * 3600L;
                }
            }
        }
    }
    if (y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) {
        return false;
    }
    *epoch = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - off;
    snprintf(day, 11, "%04d-%02d-%02d", y, mo, d);
    snprintf(hm, 6, "%02dh%02d", h, mi);
    return true;
}

/* Extension du fichier audio : d'après l'adresse, sinon le type, sinon mp3. */
static const char *episode_ext(const rss_item_t *it)
{
    static const char *known[] = {"mp3", "m4a", "aac", "ogg", "opus", "oga", "flac", "wav"};
    size_t n = strcspn(it->url, "?#");
    const char *dot = NULL;
    for (size_t i = 0; i < n; i++) {
        if (it->url[i] == '/') {
            dot = NULL;
        } else if (it->url[i] == '.') {
            dot = it->url + i;
        }
    }
    if (dot) {
        size_t el = (size_t)(it->url + n - dot - 1);
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            if (el == strlen(known[i]) && strncasecmp(dot + 1, known[i], el) == 0) {
                return known[i];
            }
        }
    }
    if (strstr(it->type, "mp4") || strstr(it->type, "m4a")) {
        return "m4a";
    }
    if (strstr(it->type, "aac")) {
        return "aac";
    }
    if (strstr(it->type, "ogg") || strstr(it->type, "opus")) {
        return "ogg";
    }
    return "mp3";
}

void rss_episode_name(const rss_item_t *it, char *out, size_t len)
{
    const char *ext = episode_ext(it);
    char title[100];
    name_from_text(it->title, title, sizeof(title), "Épisode");
    char base[140];
    if (it->day[0]) {
        snprintf(base, sizeof(base), "%s %s - %s", it->day, it->hm, title);
    } else {
        snprintf(base, sizeof(base), "%s", title);
    }
    snprintf(out, len, "%s.%s", base, ext);
}

void rss_keep_newest(rss_item_t *top, int keep, int *count, const rss_item_t *it)
{
    if (keep <= 0) {
        return;
    }
    int pos = *count;
    while (pos > 0 && it->pub > top[pos - 1].pub) {
        pos--;
    }
    if (pos >= keep) {
        return; /* plus ancien que tous ceux gardés */
    }
    int n = *count < keep ? *count : keep - 1;
    memmove(&top[pos + 1], &top[pos], (size_t)(n - pos) * sizeof(rss_item_t));
    top[pos] = *it;
    if (*count < keep) {
        (*count)++;
    }
}
