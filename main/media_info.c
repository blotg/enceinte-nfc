#include "media_info.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TEXT_FRAME_MAX 512

/* ---- Conversion de texte vers UTF-8 ---- */

static size_t put_utf8(char *out, size_t o, size_t outlen, uint32_t cp)
{
    char tmp[4];
    size_t n;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (o + n >= outlen) {
        return o; /* tronqué proprement, sans couper un caractère */
    }
    memcpy(out + o, tmp, n);
    return o + n;
}

static void latin1_to_utf8(const uint8_t *in, size_t len, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; i < len && in[i]; i++) {
        o = put_utf8(out, o, outlen, in[i]);
    }
    out[o] = '\0';
}

static void utf16_to_utf8(const uint8_t *in, size_t len, bool be, char *out, size_t outlen)
{
    size_t o = 0;
    size_t i = 0;
    if (len >= 2 && ((in[0] == 0xFF && in[1] == 0xFE) || (in[0] == 0xFE && in[1] == 0xFF))) {
        be = in[0] == 0xFE;
        i = 2;
    }
    for (; i + 1 < len; i += 2) {
        uint32_t u = be ? (in[i] << 8 | in[i + 1]) : (in[i + 1] << 8 | in[i]);
        if (u == 0) {
            break;
        }
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < len) {
            uint32_t l = be ? (in[i + 2] << 8 | in[i + 3]) : (in[i + 3] << 8 | in[i + 2]);
            if (l >= 0xDC00 && l < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
                i += 2;
            } else {
                u = 0xFFFD;
            }
        } else if (u >= 0xD800 && u < 0xE000) {
            u = 0xFFFD;
        }
        o = put_utf8(out, o, outlen, u);
    }
    out[o] = '\0';
}

static void utf8_copy(const uint8_t *in, size_t len, char *out, size_t outlen)
{
    size_t n = 0;
    while (n < len && in[n]) {
        n++;
    }
    if (n >= outlen) {
        n = outlen - 1;
        while (n > 0 && (in[n] & 0xC0) == 0x80) {
            n--; /* ne pas couper une séquence UTF-8 */
        }
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

static void id3_text(const uint8_t *data, size_t len, char *out, size_t outlen)
{
    if (len < 1 || outlen == 0) {
        return;
    }
    uint8_t enc = data[0];
    data++;
    len--;
    switch (enc) {
    case 1:
        utf16_to_utf8(data, len, false, out, outlen);
        break;
    case 2:
        utf16_to_utf8(data, len, true, out, outlen);
        break;
    case 3:
        utf8_copy(data, len, out, outlen);
        break;
    default:
        latin1_to_utf8(data, len, out, outlen);
        break;
    }
    trim(out);
}

/* ---- ID3v2 ---- */

static uint32_t syncsafe(const uint8_t *b)
{
    return (uint32_t)(b[0] & 0x7F) << 21 | (uint32_t)(b[1] & 0x7F) << 14 | (uint32_t)(b[2] & 0x7F) << 7 |
           (b[3] & 0x7F);
}

static uint32_t be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static char *id3_target(media_info_t *mi, const char *id, size_t *len)
{
    static const struct {
        const char *v34, *v22;
        size_t off, len;
    } map[] = {
        {"TIT2", "TT2", offsetof(media_info_t, title), sizeof(((media_info_t *)0)->title)},
        {"TPE1", "TP1", offsetof(media_info_t, artist), sizeof(((media_info_t *)0)->artist)},
        {"TALB", "TAL", offsetof(media_info_t, album), sizeof(((media_info_t *)0)->album)},
        {"TPE2", "TP2", offsetof(media_info_t, album_artist), sizeof(((media_info_t *)0)->album_artist)},
        {"TCON", "TCO", offsetof(media_info_t, genre), sizeof(((media_info_t *)0)->genre)},
        {"TRCK", "TRK", offsetof(media_info_t, track), sizeof(((media_info_t *)0)->track)},
        {"TDRC", "TYE", offsetof(media_info_t, date), sizeof(((media_info_t *)0)->date)},
        {"TYER", NULL, offsetof(media_info_t, date), sizeof(((media_info_t *)0)->date)},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(id, map[i].v34) == 0 || (map[i].v22 && strcmp(id, map[i].v22) == 0)) {
            *len = map[i].len;
            return (char *)mi + map[i].off;
        }
    }
    return NULL;
}

/* Retourne la taille totale du tag ID3v2 en tête de fichier (0 si absent). */
static uint32_t parse_id3v2(FILE *f, media_info_t *mi)
{
    uint8_t h[10];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) {
        return 0;
    }
    uint8_t ver = h[3];
    uint8_t flags = h[5];
    uint32_t size = syncsafe(h + 6);
    uint32_t total = size + 10 + ((flags & 0x10) ? 10 : 0);
    if (ver < 2 || ver > 4 || (flags & 0x80)) {
        return total; /* version inconnue ou désynchronisation : tags ignorés */
    }
    uint32_t pos = 10;
    if (flags & 0x40 && ver >= 3) {
        uint8_t eh[4];
        if (fread(eh, 1, 4, f) != 4) {
            return total;
        }
        pos += ver == 4 ? syncsafe(eh) : be32(eh) + 4;
    }
    uint8_t *buf = malloc(TEXT_FRAME_MAX);
    if (!buf) {
        return total;
    }
    size_t hdr = ver == 2 ? 6 : 10;
    while (pos + hdr <= size + 10) {
        uint8_t fh[10];
        if (fseek(f, pos, SEEK_SET) != 0 || fread(fh, 1, hdr, f) != hdr || fh[0] == 0) {
            break; /* fin ou bourrage */
        }
        char id[5] = {0};
        uint32_t fsize;
        if (ver == 2) {
            memcpy(id, fh, 3);
            fsize = (uint32_t)fh[3] << 16 | (uint32_t)fh[4] << 8 | fh[5];
        } else {
            memcpy(id, fh, 4);
            fsize = ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        }
        pos += hdr;
        if (fsize == 0 || pos + fsize > size + 10) {
            break;
        }
        size_t tlen;
        char *target = id3_target(mi, id, &tlen);
        bool compressed = ver >= 3 && (fh[9] & (ver == 4 ? 0x0C : 0xC0));
        if (target && !compressed && target[0] == '\0') {
            size_t n = fsize < TEXT_FRAME_MAX ? fsize : TEXT_FRAME_MAX;
            if (fread(buf, 1, n, f) == n) {
                id3_text(buf, n, target, tlen);
            }
        }
        pos += fsize;
    }
    free(buf);
    return total;
}

static void parse_id3v1(FILE *f, media_info_t *mi, long file_size)
{
    uint8_t t[128];
    if (file_size < 128 || fseek(f, file_size - 128, SEEK_SET) != 0 || fread(t, 1, 128, f) != 128 ||
        memcmp(t, "TAG", 3) != 0) {
        return;
    }
    mi->audio_end = (uint32_t)(file_size - 128);
    if (!mi->title[0]) {
        latin1_to_utf8(t + 3, 30, mi->title, sizeof(mi->title));
        trim(mi->title);
    }
    if (!mi->artist[0]) {
        latin1_to_utf8(t + 33, 30, mi->artist, sizeof(mi->artist));
        trim(mi->artist);
    }
    if (!mi->album[0]) {
        latin1_to_utf8(t + 63, 30, mi->album, sizeof(mi->album));
        trim(mi->album);
    }
    if (!mi->date[0]) {
        latin1_to_utf8(t + 93, 4, mi->date, sizeof(mi->date));
        trim(mi->date);
    }
    if (!mi->track[0] && t[125] == 0 && t[126] != 0) {
        snprintf(mi->track, sizeof(mi->track), "%u", t[126]);
    }
}

/* ---- MP3 ---- */

typedef struct {
    int version; /* 1, 2 ou 25 */
    int layer;
    uint32_t bitrate;
    uint32_t sample_rate;
    uint32_t samples;
    uint32_t frame_len;
    bool mono;
} mp3_hdr_t;

static bool mp3_parse_header(const uint8_t *b, mp3_hdr_t *h)
{
    static const uint16_t br_v1[3][16] = {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    };
    static const uint16_t br_v2[3][16] = {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    };
    static const uint32_t sr[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) {
        return false;
    }
    int ver_bits = (b[1] >> 3) & 3;
    int layer_bits = (b[1] >> 1) & 3;
    int br_idx = b[2] >> 4;
    int sr_idx = (b[2] >> 2) & 3;
    if (ver_bits == 1 || layer_bits == 0 || br_idx == 0 || br_idx == 15 || sr_idx == 3) {
        return false;
    }
    h->version = ver_bits == 3 ? 1 : (ver_bits == 2 ? 2 : 25);
    h->layer = 4 - layer_bits;
    int vi = h->version == 1 ? 0 : (h->version == 2 ? 1 : 2);
    h->sample_rate = sr[vi][sr_idx];
    h->bitrate = (h->version == 1 ? br_v1[h->layer - 1][br_idx] : br_v2[h->layer - 1][br_idx]) * 1000u;
    h->mono = ((b[3] >> 6) & 3) == 3;
    int pad = (b[2] >> 1) & 1;
    if (h->layer == 1) {
        h->samples = 384;
        h->frame_len = (12 * h->bitrate / h->sample_rate + pad) * 4;
    } else if (h->layer == 2 || h->version == 1) {
        h->samples = 1152;
        h->frame_len = 144 * h->bitrate / h->sample_rate + pad;
    } else {
        h->samples = 576;
        h->frame_len = 72 * h->bitrate / h->sample_rate + pad;
    }
    return h->frame_len > 4;
}

static void probe_mp3(FILE *f, media_info_t *mi, uint32_t start)
{
    enum { SCAN = 8192 };
    uint8_t *buf = malloc(SCAN);
    if (!buf) {
        return;
    }
    size_t n = 0;
    if (fseek(f, start, SEEK_SET) == 0) {
        n = fread(buf, 1, SCAN, f);
    }
    mp3_hdr_t h;
    size_t i = 0;
    bool found = false;
    for (; i + 4 <= n; i++) {
        if (!mp3_parse_header(buf + i, &h)) {
            continue;
        }
        /* Confirmation par l'en-tête de la trame suivante quand c'est possible. */
        mp3_hdr_t h2;
        if (i + h.frame_len + 4 <= n && !mp3_parse_header(buf + i + h.frame_len, &h2)) {
            continue;
        }
        found = true;
        break;
    }
    if (!found) {
        free(buf);
        return;
    }
    mi->audio_start = start + (uint32_t)i;
    mi->sample_rate = h.sample_rate;
    uint32_t audio_bytes = mi->audio_end > mi->audio_start ? mi->audio_end - mi->audio_start : 0;

    /* En-tête Xing/Info (VBR ou CBR LAME) */
    size_t side = h.version == 1 ? (h.mono ? 17 : 32) : (h.mono ? 9 : 17);
    const uint8_t *x = buf + i + 4 + side;
    const uint8_t *v = buf + i + 4 + 32;
    if (x + 16 <= buf + n && (memcmp(x, "Xing", 4) == 0 || memcmp(x, "Info", 4) == 0)) {
        uint32_t flags = be32(x + 4);
        const uint8_t *p = x + 8;
        uint32_t frames = 0;
        if (flags & 1) {
            frames = be32(p);
            p += 4;
        }
        if (flags & 2) {
            uint32_t bytes = be32(p);
            if (bytes > 0 && bytes <= audio_bytes + 4096) {
                audio_bytes = bytes;
            }
            p += 4;
        }
        if ((flags & 4) && p + 100 <= buf + n) {
            memcpy(mi->toc, p, 100);
            mi->has_toc = true;
        }
        if (frames) {
            uint64_t ms = (uint64_t)frames * h.samples * 1000 / h.sample_rate;
            mi->duration_ms = (uint32_t)ms;
        }
    } else if (v + 26 <= buf + n && memcmp(v, "VBRI", 4) == 0) {
        uint32_t bytes = be32(v + 10);
        uint32_t frames = be32(v + 14);
        if (bytes > 0) {
            audio_bytes = bytes;
        }
        if (frames) {
            mi->duration_ms = (uint32_t)((uint64_t)frames * h.samples * 1000 / h.sample_rate);
        }
    }
    if (mi->duration_ms) {
        mi->bitrate = (uint32_t)((uint64_t)audio_bytes * 8 * 1000 / mi->duration_ms);
    } else {
        mi->bitrate = h.bitrate; /* CBR */
        if (h.bitrate) {
            mi->duration_ms = (uint32_t)((uint64_t)audio_bytes * 8 * 1000 / h.bitrate);
        }
    }
    free(buf);
}

/* ---- FLAC ---- */

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0];
}

static void vorbis_comment(media_info_t *mi, const char *c, size_t len)
{
    static const struct {
        const char *key;
        size_t off, len;
    } map[] = {
        {"TITLE", offsetof(media_info_t, title), sizeof(((media_info_t *)0)->title)},
        {"ARTIST", offsetof(media_info_t, artist), sizeof(((media_info_t *)0)->artist)},
        {"ALBUM", offsetof(media_info_t, album), sizeof(((media_info_t *)0)->album)},
        {"ALBUMARTIST", offsetof(media_info_t, album_artist), sizeof(((media_info_t *)0)->album_artist)},
        {"GENRE", offsetof(media_info_t, genre), sizeof(((media_info_t *)0)->genre)},
        {"DATE", offsetof(media_info_t, date), sizeof(((media_info_t *)0)->date)},
        {"TRACKNUMBER", offsetof(media_info_t, track), sizeof(((media_info_t *)0)->track)},
    };
    const char *eq = memchr(c, '=', len);
    if (!eq) {
        return;
    }
    size_t klen = (size_t)(eq - c);
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strlen(map[i].key) == klen && strncasecmp(c, map[i].key, klen) == 0) {
            char *t = (char *)mi + map[i].off;
            if (!t[0]) {
                utf8_copy((const uint8_t *)eq + 1, len - klen - 1, t, map[i].len);
            }
            return;
        }
    }
}

static void probe_flac(FILE *f, media_info_t *mi, uint32_t start)
{
    uint8_t h[4];
    if (fseek(f, start, SEEK_SET) != 0 || fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4) != 0) {
        return;
    }
    uint32_t pos = start + 4;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t bh[4];
        if (fseek(f, pos, SEEK_SET) != 0 || fread(bh, 1, 4, f) != 4) {
            return;
        }
        bool last = bh[0] & 0x80;
        int type = bh[0] & 0x7F;
        uint32_t len = (uint32_t)bh[1] << 16 | (uint32_t)bh[2] << 8 | bh[3];
        if (type == 0 && len >= 18) {
            uint8_t si[18];
            if (fread(si, 1, 18, f) == 18) {
                uint32_t rate = (uint32_t)si[10] << 12 | (uint32_t)si[11] << 4 | si[12] >> 4;
                uint64_t total = (uint64_t)(si[13] & 0x0F) << 32 | be32(si + 14);
                mi->sample_rate = rate;
                if (rate) {
                    mi->duration_ms = (uint32_t)(total * 1000 / rate);
                }
            }
        } else if (type == 4 && len < 65536) {
            uint8_t *vc = malloc(len);
            if (vc && fread(vc, 1, len, f) == len && len >= 8) {
                uint32_t vlen = le32(vc);
                uint32_t p = 4 + vlen;
                if (p + 4 <= len) {
                    uint32_t count = le32(vc + p);
                    p += 4;
                    for (uint32_t i = 0; i < count && p + 4 <= len; i++) {
                        uint32_t clen = le32(vc + p);
                        p += 4;
                        if (clen > len - p) {
                            break;
                        }
                        vorbis_comment(mi, (const char *)vc + p, clen);
                        p += clen;
                    }
                }
            }
            free(vc);
        }
        pos += 4 + len;
        if (last) {
            break;
        }
    }
}

/* ---- WAV ---- */

static void probe_wav(FILE *f, media_info_t *mi)
{
    uint8_t h[12];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 ||
        memcmp(h + 8, "WAVE", 4) != 0) {
        return;
    }
    uint32_t byte_rate = 0;
    uint32_t pos = 12;
    for (int guard = 0; guard < 32; guard++) {
        uint8_t ch[8];
        if (fseek(f, pos, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8) {
            return;
        }
        uint32_t len = le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0 && len >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) == 16) {
                mi->sample_rate = le32(fmt + 4);
                byte_rate = le32(fmt + 8);
                mi->bitrate = byte_rate * 8;
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            if (byte_rate) {
                mi->duration_ms = (uint32_t)((uint64_t)len * 1000 / byte_rate);
            }
            return;
        }
        pos += 8 + len + (len & 1);
    }
}

bool media_probe(FILE *f, audio_fmt_t fmt, media_info_t *mi)
{
    memset(mi, 0, sizeof(*mi));
    if (fseek(f, 0, SEEK_END) != 0) {
        return false;
    }
    long size = ftell(f);
    if (size < 0) {
        return false;
    }
    mi->audio_end = (uint32_t)size;
    uint32_t id3 = parse_id3v2(f, mi);
    if (id3 > (uint32_t)size) {
        id3 = 0;
    }
    switch (fmt) {
    case AUDIO_FMT_MP3:
        parse_id3v1(f, mi, size);
        probe_mp3(f, mi, id3);
        break;
    case AUDIO_FMT_FLAC:
        probe_flac(f, mi, id3);
        break;
    case AUDIO_FMT_WAV:
        probe_wav(f, mi);
        break;
    default:
        break;
    }
    return true;
}

uint32_t media_seek_offset(const media_info_t *mi, audio_fmt_t fmt, uint32_t ms)
{
    if (fmt != AUDIO_FMT_MP3 || mi->duration_ms == 0 || mi->audio_end <= mi->audio_start) {
        return 0;
    }
    if (ms >= mi->duration_ms) {
        ms = mi->duration_ms - 1;
    }
    uint32_t bytes = mi->audio_end - mi->audio_start;
    double frac = (double)ms / mi->duration_ms;
    double pos;
    if (mi->has_toc) {
        double pct = frac * 100.0;
        int i = (int)pct;
        if (i > 99) {
            i = 99;
        }
        double a = mi->toc[i];
        double b = i < 99 ? mi->toc[i + 1] : 256.0;
        pos = (a + (b - a) * (pct - i)) / 256.0 * bytes;
    } else {
        pos = frac * bytes;
    }
    uint32_t off = mi->audio_start + (uint32_t)pos;
    return off > mi->audio_start ? off : mi->audio_start + 1;
}
