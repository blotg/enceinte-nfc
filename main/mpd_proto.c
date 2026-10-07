#include "mpd_proto.h"

#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *const mpd_tag_names[MPD_TAG_COUNT] = {
    [MPD_TAG_ARTIST] = "Artist", [MPD_TAG_ALBUMARTIST] = "AlbumArtist", [MPD_TAG_ALBUM] = "Album",
    [MPD_TAG_TITLE] = "Title",   [MPD_TAG_TRACK] = "Track",             [MPD_TAG_GENRE] = "Genre",
    [MPD_TAG_DATE] = "Date",
};

int mpd_tag_from_name(const char *name)
{
    for (int i = 0; i < MPD_TAG_COUNT; i++) {
        if (strcasecmp(name, mpd_tag_names[i]) == 0) {
            return i;
        }
    }
    if (strcasecmp(name, "file") == 0 || strcasecmp(name, "filename") == 0) {
        return MPD_TAG_FILE;
    }
    if (strcasecmp(name, "any") == 0) {
        return MPD_TAG_ANY;
    }
    return -1;
}

int mpd_tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            return argc;
        }
        if (argc >= max) {
            return -1;
        }
        if (*p == '"') {
            p++;
            char *dst = p;
            argv[argc++] = p;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    p++;
                }
                *dst++ = *p++;
            }
            if (*p != '"') {
                return -1;
            }
            p++;
            *dst = '\0';
            if (*p && *p != ' ' && *p != '\t') {
                return -1;
            }
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            if (*p) {
                *p++ = '\0';
            }
        }
    }
}

bool mpd_parse_int(const char *s, int *out)
{
    if (!s || !*s) {
        return false;
    }
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*end || errno || v < -2147483647L || v > 2147483647L) {
        return false;
    }
    *out = (int)v;
    return true;
}

bool mpd_parse_float(const char *s, double *out)
{
    if (!s || !*s) {
        return false;
    }
    char *end;
    double v = strtod(s, &end);
    if (*end) {
        return false;
    }
    *out = v;
    return true;
}

bool mpd_parse_range(const char *s, int *start, int *end)
{
    const char *colon = strchr(s, ':');
    if (!colon) {
        int v;
        if (!mpd_parse_int(s, &v) || v < 0) {
            return false;
        }
        *start = v;
        *end = v + 1;
        return true;
    }
    char a[16];
    size_t n = (size_t)(colon - s);
    if (n == 0 || n >= sizeof(a)) {
        return false;
    }
    memcpy(a, s, n);
    a[n] = '\0';
    if (!mpd_parse_int(a, start) || *start < 0) {
        return false;
    }
    if (colon[1] == '\0') {
        *end = -1;
        return true;
    }
    return mpd_parse_int(colon + 1, end) && *end >= *start;
}

/* ---- Filtres ---- */

static mpd_filter_t *node(mpd_filter_op_t op)
{
    mpd_filter_t *f = calloc(1, sizeof(mpd_filter_t));
    if (f) {
        f->op = op;
    }
    return f;
}

void mpd_filter_free(mpd_filter_t *f)
{
    if (!f) {
        return;
    }
    mpd_filter_free(f->a);
    mpd_filter_free(f->b);
    free(f->value);
    free(f);
}

typedef struct {
    const char *p;
    bool icase;
    char *err;
    size_t errlen;
} parser_t;

static void skip_ws(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t') {
        ps->p++;
    }
}

static bool fail(parser_t *ps, const char *msg)
{
    if (ps->err && ps->errlen) {
        snprintf(ps->err, ps->errlen, "%s", msg);
    }
    return false;
}

/* Identifiant (tag, opérateur alphabétique) ou opérateur symbolique (==, !=, =~). */
static char *parse_word(parser_t *ps)
{
    skip_ws(ps);
    const char *s = ps->p;
    if (strchr("=!~<>", *ps->p) && *ps->p) {
        while (*ps->p && strchr("=!~<>", *ps->p)) {
            ps->p++;
        }
    } else {
        while (*ps->p && (isalnum((unsigned char)*ps->p) || *ps->p == '_' || *ps->p == '-')) {
            ps->p++;
        }
    }
    size_t n = (size_t)(ps->p - s);
    if (n == 0) {
        return NULL;
    }
    char *w = malloc(n + 1);
    if (w) {
        memcpy(w, s, n);
        w[n] = '\0';
    }
    return w;
}

static char *parse_quoted(parser_t *ps)
{
    skip_ws(ps);
    char q = *ps->p;
    if (q != '"' && q != '\'') {
        return NULL;
    }
    ps->p++;
    size_t cap = strlen(ps->p) + 1;
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    while (*ps->p && *ps->p != q) {
        if (*ps->p == '\\' && ps->p[1]) {
            ps->p++;
        }
        out[o++] = *ps->p++;
    }
    if (*ps->p != q) {
        free(out);
        return NULL;
    }
    ps->p++;
    out[o] = '\0';
    return out;
}

static mpd_filter_t *parse_expr(parser_t *ps, int depth);

static mpd_filter_t *parse_expr(parser_t *ps, int depth)
{
    skip_ws(ps);
    if (depth > 8 || *ps->p != '(') {
        fail(ps, "'(' attendu");
        return NULL;
    }
    ps->p++;
    skip_ws(ps);
    mpd_filter_t *f = NULL;
    if (*ps->p == '!') {
        ps->p++;
        mpd_filter_t *inner = parse_expr(ps, depth + 1);
        if (!inner) {
            return NULL;
        }
        f = node(MPD_F_NOT);
        if (!f) {
            mpd_filter_free(inner);
            return NULL;
        }
        f->a = inner;
    } else if (*ps->p == '(') {
        f = parse_expr(ps, depth + 1);
        for (;;) {
            if (!f) {
                return NULL;
            }
            skip_ws(ps);
            if (strncasecmp(ps->p, "AND", 3) != 0) {
                break;
            }
            ps->p += 3;
            mpd_filter_t *rhs = parse_expr(ps, depth + 1);
            mpd_filter_t *and = rhs ? node(MPD_F_AND) : NULL;
            if (!and) {
                mpd_filter_free(f);
                mpd_filter_free(rhs);
                return NULL;
            }
            and->a = f;
            and->b = rhs;
            f = and;
        }
    } else {
        char *name = parse_word(ps);
        if (!name) {
            fail(ps, "tag attendu");
            return NULL;
        }
        if (strcasecmp(name, "base") == 0) {
            free(name);
            f = node(MPD_F_BASE);
            if (f) {
                f->value = parse_quoted(ps);
                if (!f->value) {
                    mpd_filter_free(f);
                    fail(ps, "valeur attendue");
                    return NULL;
                }
            }
        } else {
            int tag = mpd_tag_from_name(name);
            free(name);
            if (tag < 0) {
                fail(ps, "tag inconnu");
                return NULL;
            }
            char *op = parse_word(ps);
            mpd_filter_op_t fop;
            if (op && strcmp(op, "==") == 0) {
                fop = MPD_F_EQ;
            } else if (op && strcmp(op, "!=") == 0) {
                fop = MPD_F_NE;
            } else if (op && strcasecmp(op, "contains") == 0) {
                fop = MPD_F_CONTAINS;
            } else if (op && strcasecmp(op, "starts_with") == 0) {
                fop = MPD_F_STARTS;
            } else {
                free(op);
                fail(ps, "opérateur non pris en charge");
                return NULL;
            }
            free(op);
            f = node(fop);
            if (f) {
                f->tag = tag;
                f->icase = ps->icase;
                f->value = parse_quoted(ps);
                if (!f->value) {
                    mpd_filter_free(f);
                    fail(ps, "valeur attendue");
                    return NULL;
                }
            }
        }
    }
    if (!f) {
        return NULL;
    }
    skip_ws(ps);
    if (*ps->p != ')') {
        mpd_filter_free(f);
        fail(ps, "')' attendu");
        return NULL;
    }
    ps->p++;
    return f;
}

mpd_filter_t *mpd_filter_parse_expr(const char *expr, bool icase, char *err, size_t errlen)
{
    parser_t ps = {.p = expr, .icase = icase, .err = err, .errlen = errlen};
    mpd_filter_t *f = parse_expr(&ps, 0);
    skip_ws(&ps);
    if (f && *ps.p) {
        mpd_filter_free(f);
        fail(&ps, "texte inattendu après le filtre");
        return NULL;
    }
    return f;
}

mpd_filter_t *mpd_filter_parse_pairs(int argc, char **argv, bool search, char *err, size_t errlen)
{
    if (argc < 2 || argc % 2) {
        if (err && errlen) {
            snprintf(err, errlen, "nombre d'arguments incorrect");
        }
        return NULL;
    }
    mpd_filter_t *root = NULL;
    for (int i = 0; i + 1 < argc; i += 2) {
        mpd_filter_t *f;
        if (strcasecmp(argv[i], "base") == 0) {
            f = node(MPD_F_BASE);
        } else {
            int tag = mpd_tag_from_name(argv[i]);
            if (tag < 0) {
                mpd_filter_free(root);
                if (err && errlen) {
                    snprintf(err, errlen, "tag inconnu \"%s\"", argv[i]);
                }
                return NULL;
            }
            f = node(search ? MPD_F_CONTAINS : MPD_F_EQ);
            if (f) {
                f->tag = tag;
                f->icase = search;
            }
        }
        if (!f || !(f->value = strdup(argv[i + 1]))) {
            mpd_filter_free(f);
            mpd_filter_free(root);
            return NULL;
        }
        if (!root) {
            root = f;
        } else {
            mpd_filter_t *and = node(MPD_F_AND);
            if (!and) {
                mpd_filter_free(f);
                mpd_filter_free(root);
                return NULL;
            }
            and->a = root;
            and->b = f;
            root = and;
        }
    }
    return root;
}

mpd_filter_t *mpd_filter_parse_args(int argc, char **argv, bool search, int *used, char *err, size_t errlen)
{
    if (argc >= 1 && argv[0][0] == '(') {
        *used = 1;
        return mpd_filter_parse_expr(argv[0], search, err, errlen);
    }
    int n = argc - (argc % 2);
    /* "sort", "window", "group" terminent les paires */
    for (int i = 0; i < n; i += 2) {
        if (strcasecmp(argv[i], "sort") == 0 || strcasecmp(argv[i], "window") == 0 ||
            strcasecmp(argv[i], "group") == 0) {
            n = i;
            break;
        }
    }
    *used = n;
    return mpd_filter_parse_pairs(n, argv, search, err, errlen);
}

/*
 * Minuscules UTF-8 pour les comparaisons sans casse : ASCII, Latin-1 (À-Þ) et
 * Latin étendu A (Œ, Ā...). Suffisant pour le français et la plupart des langues
 * européennes. Les séquences invalides sont recopiées telles quelles.
 */
void mpd_utf8_fold(const char *in, char *out, size_t outlen)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;
    while (*p && o + 4 < outlen) {
        uint32_t cp;
        int n;
        if (p[0] < 0x80) {
            cp = p[0];
            n = 1;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
            n = 2;
        } else {
            out[o++] = (char)*p++; /* autres séquences : recopiées */
            continue;
        }
        if (cp >= 'A' && cp <= 'Z') {
            cp += 32;
        } else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) {
            cp += 32;
        } else if (cp >= 0x100 && cp <= 0x17F) {
            if (cp == 0x178) {
                cp = 0xFF; /* Ÿ */
            } else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
                cp += (cp & 1); /* paires décalées (Ĺĺ, Źź...) */
            } else if (cp != 0x130 && cp != 0x131 && cp != 0x138 && cp != 0x149 && cp != 0x17F) {
                cp |= 1;
            }
        }
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
        p += n;
    }
    out[o] = '\0';
}

static bool match_value(const mpd_filter_t *f, const char *v)
{
    if (!v) {
        v = "";
    }
    char va[256], vb[256];
    const char *a = v, *b = f->value;
    if (f->icase) {
        mpd_utf8_fold(v, va, sizeof(va));
        mpd_utf8_fold(f->value, vb, sizeof(vb));
        a = va;
        b = vb;
    }
    switch (f->op) {
    case MPD_F_EQ:
        return strcmp(a, b) == 0;
    case MPD_F_NE:
        return strcmp(a, b) != 0;
    case MPD_F_CONTAINS:
        return strstr(a, b) != NULL;
    case MPD_F_STARTS:
        return strncmp(a, b, strlen(b)) == 0;
    default:
        return false;
    }
}

bool mpd_filter_match(const mpd_filter_t *f, mpd_tag_getter_t get, void *ctx)
{
    if (!f) {
        return true;
    }
    switch (f->op) {
    case MPD_F_AND:
        return mpd_filter_match(f->a, get, ctx) && mpd_filter_match(f->b, get, ctx);
    case MPD_F_NOT:
        return !mpd_filter_match(f->a, get, ctx);
    case MPD_F_BASE: {
        const char *file = get(ctx, MPD_TAG_FILE);
        size_t n = strlen(f->value);
        return file && (n == 0 || (strncmp(file, f->value, n) == 0 && file[n] == '/'));
    }
    default:
        break;
    }
    if (f->tag == MPD_TAG_ANY) {
        if (f->op == MPD_F_NE) {
            for (int t = 0; t <= MPD_TAG_FILE; t++) {
                const char *v = get(ctx, t);
                if (v && *v && strcmp(v, f->value) == 0) {
                    return false;
                }
            }
            return true;
        }
        for (int t = 0; t <= MPD_TAG_FILE; t++) {
            const char *v = get(ctx, t);
            if (v && *v && match_value(f, v)) {
                return true;
            }
        }
        return false;
    }
    return match_value(f, get(ctx, f->tag));
}
