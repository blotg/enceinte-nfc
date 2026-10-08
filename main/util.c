#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void str_copy(char *dst, const char *src, size_t dstlen)
{
    if (dstlen == 0) {
        return;
    }
    size_t n = strnlen(src, dstlen - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool url_decode(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        char c = in[i];
        if (c == '%') {
            int h = hexval(in[i + 1]);
            int l = h < 0 ? -1 : hexval(in[i + 2]);
            if (h < 0 || l < 0) {
                return false;
            }
            c = (char)(h * 16 + l);
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c == '\0' || o + 1 >= outlen) {
            return false;
        }
        out[o++] = c;
    }
    if (outlen == 0) {
        return false;
    }
    out[o] = '\0';
    return true;
}

bool name_is_valid(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > 255) {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    if (name[len - 1] == ' ' || name[len - 1] == '.' || name[0] == ' ') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || strchr("\"*:<>?\\|/", *p)) {
            return false;
        }
    }
    return true;
}

bool path_sanitize(const char *in, char *out, size_t outlen)
{
    if (outlen == 0) {
        return false;
    }
    size_t o = 0;
    out[0] = '\0';
    const char *p = in;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char comp[256];
        if (len >= sizeof(comp)) {
            return false;
        }
        memcpy(comp, p, len);
        comp[len] = '\0';
        if (!name_is_valid(comp)) {
            return false;
        }
        size_t need = len + (o ? 1 : 0);
        if (o + need + 1 > outlen) {
            return false;
        }
        if (o) {
            out[o++] = '/';
        }
        memcpy(out + o, comp, len);
        o += len;
        out[o] = '\0';
        p += len;
    }
    return true;
}

bool path_to_abs(const char *rel, char *out, size_t outlen)
{
    int n = rel[0] ? snprintf(out, outlen, "%s/%s", MUSIC_ROOT, rel) : snprintf(out, outlen, "%s", MUSIC_ROOT);
    return n > 0 && (size_t)n < outlen;
}

const char *path_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void path_dirname(const char *path, char *out, size_t outlen)
{
    const char *s = strrchr(path, '/');
    if (!s) {
        str_copy(out, "", outlen);
        return;
    }
    size_t len = (size_t)(s - path);
    if (len >= outlen) {
        len = outlen - 1;
    }
    memcpy(out, path, len);
    out[len] = '\0';
}

bool name_is_hidden(const char *name)
{
    return name[0] == '.' || strcasecmp(name, "System Volume Information") == 0;
}

audio_fmt_t audio_fmt_from_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return AUDIO_FMT_NONE;
    }
    ext++;
    if (strcasecmp(ext, "mp3") == 0) {
        return AUDIO_FMT_MP3;
    }
    if (strcasecmp(ext, "aac") == 0) {
        return AUDIO_FMT_AAC;
    }
    if (strcasecmp(ext, "flac") == 0) {
        return AUDIO_FMT_FLAC;
    }
    if (strcasecmp(ext, "wav") == 0) {
        return AUDIO_FMT_WAV;
    }
    if (strcasecmp(ext, "m4a") == 0 || strcasecmp(ext, "mp4") == 0) {
        return AUDIO_FMT_M4A;
    }
    if (strcasecmp(ext, "ogg") == 0 || strcasecmp(ext, "opus") == 0 || strcasecmp(ext, "oga") == 0) {
        return AUDIO_FMT_OGG;
    }
    return AUDIO_FMT_NONE;
}

int natural_casecmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x && *y) {
        if (isdigit(*x) && isdigit(*y)) {
            while (*x == '0' && isdigit(x[1])) {
                x++;
            }
            while (*y == '0' && isdigit(y[1])) {
                y++;
            }
            size_t lx = 0, ly = 0;
            while (isdigit(x[lx])) {
                lx++;
            }
            while (isdigit(y[ly])) {
                ly++;
            }
            if (lx != ly) {
                return lx < ly ? -1 : 1;
            }
            int c = memcmp(x, y, lx);
            if (c) {
                return c;
            }
            x += lx;
            y += ly;
            continue;
        }
        int cx = tolower(*x), cy = tolower(*y);
        if (cx != cy) {
            return cx < cy ? -1 : 1;
        }
        x++;
        y++;
    }
    if (*x == *y) {
        return strcmp(a, b); /* départage stable (casse) */
    }
    return *x ? 1 : -1;
}

static const char *parse_num(const char *p, long *v)
{
    *v = 0;
    while (isdigit((unsigned char)*p)) {
        *v = *v * 10 + (*p - '0');
        p++;
    }
    return p;
}

int semver_cmp(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V') {
        a++;
    }
    if (*b == 'v' || *b == 'V') {
        b++;
    }
    for (int i = 0; i < 3; i++) {
        long va, vb;
        a = parse_num(a, &va);
        b = parse_num(b, &vb);
        if (va != vb) {
            return va < vb ? -1 : 1;
        }
        if (*a == '.') {
            a++;
        }
        if (*b == '.') {
            b++;
        }
    }
    return 0;
}

void bytes_to_hex(const uint8_t *bytes, size_t n, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 0xF];
    }
    out[2 * n] = '\0';
}

bool hostname_normalize(const char *in, char *out, size_t outlen)
{
    size_t len = strlen(in);
    if (len == 0 || len > 32 || len >= outlen) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = (char)tolower((unsigned char)in[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
        out[i] = c;
    }
    out[len] = '\0';
    return out[0] != '-' && out[len - 1] != '-';
}

bool ip4_parse(const char *s, uint32_t *out)
{
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (part > 0 && *s++ != '.') {
            return false;
        }
        if (*s < '0' || *s > '9' || (s[0] == '0' && s[1] >= '0' && s[1] <= '9')) {
            return false;
        }
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) {
            v = v * 10 + (unsigned)(*s++ - '0');
            digits++;
        }
        if (v > 255) {
            return false;
        }
        ip = ip << 8 | v;
    }
    if (*s) {
        return false;
    }
    *out = ip;
    return true;
}

void ip4_format(uint32_t ip, char out[16])
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 0xFF), (unsigned)(ip >> 8 & 0xFF),
             (unsigned)(ip & 0xFF));
}

bool ip4_config_check(uint32_t ip, uint32_t netmask, uint32_t gateway, const char **why)
{
    const char *msg = NULL;
    uint32_t host = ~netmask;
    if ((host & (host + 1)) != 0 || netmask < 0xFF000000u || netmask > 0xFFFFFFFCu) {
        msg = "masque de sous-réseau invalide (exemple : 255.255.255.0)";
    } else if (ip == 0 || (ip >> 24) == 127 || (ip >> 24) >= 224) {
        msg = "adresse IP invalide";
    } else if ((ip & host) == 0 || (ip & host) == host) {
        msg = "adresse réservée au réseau ou à la diffusion : choisissez-en une autre";
    } else if ((gateway & netmask) != (ip & netmask)) {
        msg = "la passerelle doit être dans le même sous-réseau que l'adresse";
    } else if (gateway == ip || (gateway & host) == 0 || (gateway & host) == host) {
        msg = "passerelle invalide";
    }
    if (why) {
        *why = msg;
    }
    return msg == NULL;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < n) {
            v |= in[i + 2];
        }
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = '\0';
}

int base64_decode(const char *in, uint8_t *out, size_t outlen)
{
    size_t len = strlen(in), o = 0;
    if (len % 4) {
        return -1;
    }
    for (size_t i = 0; i < len; i += 4) {
        uint32_t v = 0;
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            char c = in[i + k];
            const char *p = c ? strchr(B64, c) : NULL;
            if (c == '=' && i + 4 == len && k >= 2) {
                pad++;
                v <<= 6;
                continue;
            }
            if (!p || pad) {
                return -1;
            }
            v = v << 6 | (uint32_t)(p - B64);
        }
        for (int k = 0; k < 3 - pad; k++) {
            if (o >= outlen) {
                return -1;
            }
            out[o++] = (uint8_t)(v >> (16 - 8 * k));
        }
    }
    return (int)o;
}
