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
        /* Confirmation par les en-têtes des deux trames suivantes quand c'est possible
         * (évite de prendre des données quelconques pour du MP3). */
        mp3_hdr_t h2, h3;
        size_t i2 = i + h.frame_len;
        if (i2 + 4 <= n && !mp3_parse_header(buf + i2, &h2)) {
            continue;
        }
        if (i2 + 4 <= n && i2 + h2.frame_len + 4 <= n && !mp3_parse_header(buf + i2 + h2.frame_len, &h3)) {
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

/* ---- Commentaires Vorbis (FLAC, Ogg Vorbis, Opus) ---- */

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0];
}

static uint64_t le64(const uint8_t *b)
{
    return (uint64_t)le32(b + 4) << 32 | le32(b);
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

/* Bloc de commentaires : longueur+fournisseur, nombre, puis "CLÉ=valeur" (petit-boutiste). */
static void vorbis_comments(media_info_t *mi, const uint8_t *vc, uint32_t len)
{
    if (len < 8) {
        return;
    }
    uint32_t vlen = le32(vc);
    if (vlen > len - 8) {
        return;
    }
    uint32_t p = 4 + vlen;
    uint32_t count = le32(vc + p);
    p += 4;
    for (uint32_t i = 0; i < count && p + 4 <= len; i++) {
        uint32_t clen = le32(vc + p);
        p += 4;
        if (clen > len - p) {
            break; /* commentaire tronqué (souvent une pochette encodée) */
        }
        vorbis_comment(mi, (const char *)vc + p, clen);
        p += clen;
    }
}

/* ---- FLAC ---- */

static void probe_flac(FILE *f, media_info_t *mi, uint32_t start)
{
    uint8_t h[4];
    if (fseek(f, start, SEEK_SET) != 0 || fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4) != 0) {
        return;
    }
    uint32_t pos = start + 4;
    uint64_t total = 0;
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
                uint16_t min_block = (uint16_t)(si[0] << 8 | si[1]);
                uint16_t max_block = (uint16_t)(si[2] << 8 | si[3]);
                uint32_t rate = (uint32_t)si[10] << 12 | (uint32_t)si[11] << 4 | si[12] >> 4;
                total = (uint64_t)(si[13] & 0x0F) << 32 | be32(si + 14);
                mi->sample_rate = rate;
                mi->flac_block_size = min_block == max_block ? min_block : 0;
                mi->flac_total_samples = total;
                if (rate) {
                    mi->duration_ms = (uint32_t)(total * 1000 / rate);
                }
            }
        } else if (type == 3 && len >= 18) {
            mi->flac_seektable_pos = pos + 4;
            mi->flac_seektable_n = (uint16_t)(len / 18 > 65535 ? 65535 : len / 18);
        } else if (type == 4 && len < 65536) {
            uint8_t *vc = malloc(len);
            if (vc && fread(vc, 1, len, f) == len) {
                vorbis_comments(mi, vc, len);
            }
            free(vc);
        }
        pos += 4 + len;
        if (last) {
            mi->audio_start = pos; /* première trame audio */
            break;
        }
    }
    if (mi->audio_start && mi->sample_rate && total && mi->audio_end > mi->audio_start) {
        mi->bitrate = (uint32_t)((uint64_t)(mi->audio_end - mi->audio_start) * 8 * mi->sample_rate / total);
        mi->seekable = true;
    }
}

static uint8_t crc8(const uint8_t *d, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) {
            c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
        }
    }
    return c;
}

/* En-tête de trame FLAC valide (CRC-8 compris) ? Renvoie le premier échantillon de la trame. */
static bool flac_frame_header(const uint8_t *b, size_t avail, const media_info_t *mi, uint64_t *sample)
{
    if (avail < 16 || b[0] != 0xFF || (b[1] & 0xFE) != 0xF8) {
        return false;
    }
    int bs_code = b[2] >> 4, sr_code = b[2] & 0x0F, ch = b[3] >> 4, ss = (b[3] >> 1) & 7;
    if (bs_code == 0 || sr_code == 15 || ch > 10 || ss == 3 || (b[3] & 1)) {
        return false;
    }
    /* numéro de trame ou d'échantillon, codé comme de l'UTF-8 (jusqu'à 7 octets) */
    size_t p = 4;
    uint8_t first = b[p++];
    int extra;
    uint64_t v;
    if (!(first & 0x80)) {
        v = first;
        extra = 0;
    } else if ((first & 0xE0) == 0xC0) {
        v = first & 0x1F;
        extra = 1;
    } else if ((first & 0xF0) == 0xE0) {
        v = first & 0x0F;
        extra = 2;
    } else if ((first & 0xF8) == 0xF0) {
        v = first & 0x07;
        extra = 3;
    } else if ((first & 0xFC) == 0xF8) {
        v = first & 0x03;
        extra = 4;
    } else if ((first & 0xFE) == 0xFC) {
        v = first & 0x01;
        extra = 5;
    } else if (first == 0xFE) {
        v = 0;
        extra = 6;
    } else {
        return false;
    }
    for (int i = 0; i < extra; i++) {
        if ((b[p] & 0xC0) != 0x80) {
            return false;
        }
        v = v << 6 | (b[p++] & 0x3F);
    }
    p += bs_code == 6 ? 1 : (bs_code == 7 ? 2 : 0);
    p += sr_code == 12 ? 1 : ((sr_code == 13 || sr_code == 14) ? 2 : 0);
    if (p >= avail || crc8(b, p) != b[p]) {
        return false;
    }
    bool variable = b[1] & 1;
    if (variable) {
        *sample = v;
    } else if (mi->flac_block_size) {
        *sample = v * mi->flac_block_size;
    } else {
        return false;
    }
    return true;
}

static bool seek_flac(FILE *f, const media_info_t *mi, uint32_t ms, media_seek_t *out)
{
    uint64_t target = (uint64_t)ms * mi->sample_rate / 1000;
    uint32_t start = 0;
    /* 1. Table de points de recherche, si elle existe (positions exactes). */
    if (mi->flac_seektable_pos && fseek(f, mi->flac_seektable_pos, SEEK_SET) == 0) {
        uint64_t best_sample = 0, best_off = 0;
        bool found = false;
        uint8_t pt[18];
        for (uint32_t i = 0; i < mi->flac_seektable_n && fread(pt, 1, 18, f) == 18; i++) {
            uint64_t smp = (uint64_t)be32(pt) << 32 | be32(pt + 4);
            if (smp == UINT64_MAX || smp > target) {
                continue; /* point réservé, ou au-delà de la cible */
            }
            if (!found || smp >= best_sample) {
                best_sample = smp;
                best_off = (uint64_t)be32(pt + 8) << 32 | be32(pt + 12);
                found = true;
            }
        }
        if (found && mi->audio_start + best_off < mi->audio_end) {
            start = (uint32_t)(mi->audio_start + best_off);
        }
    }
    /* 2. Sinon, estimation proportionnelle, avec une marge en arrière. */
    if (!start) {
        double frac = mi->flac_total_samples ? (double)target / mi->flac_total_samples : 0;
        uint32_t est = mi->audio_start + (uint32_t)(frac * (mi->audio_end - mi->audio_start));
        start = est > mi->audio_start + 32768 ? est - 32768 : mi->audio_start;
    }
    /* Parcourt les trames à partir de "start" : garde la dernière qui ne dépasse pas la cible. */
    enum { WIN = 32768, SPAN = 4 * WIN };
    uint8_t *buf = malloc(WIN);
    if (!buf) {
        return false;
    }
    bool ok = false, done = false;
    uint64_t first_smp = 0;
    uint32_t first_off = 0;
    for (uint32_t base = start; !done && base < mi->audio_end && base < start + SPAN; base += WIN - 16) {
        size_t n = 0;
        if (fseek(f, base, SEEK_SET) == 0) {
            n = fread(buf, 1, WIN, f);
        }
        for (size_t i = 0; i + 16 <= n; i++) {
            uint64_t smp;
            if (buf[i] != 0xFF || !flac_frame_header(buf + i, n - i, mi, &smp) || smp > mi->flac_total_samples) {
                continue;
            }
            if (!first_off) {
                first_off = base + (uint32_t)i;
                first_smp = smp;
            }
            if (smp > target) {
                done = true;
                break;
            }
            out->offset = base + (uint32_t)i;
            out->actual_ms = (uint32_t)(smp * 1000 / mi->sample_rate);
            ok = true;
            i += 15; /* une trame fait plus de 16 octets */
        }
        if (n < WIN) {
            break;
        }
    }
    free(buf);
    if (!ok && first_off) { /* estimation trop loin : première trame trouvée */
        out->offset = first_off;
        out->actual_ms = (uint32_t)(first_smp * 1000 / mi->sample_rate);
        ok = true;
    }
    out->header_end = mi->audio_start;
    return ok;
}

/* ---- WAV ---- */

static void probe_wav(FILE *f, media_info_t *mi)
{
    uint8_t h[12];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 ||
        memcmp(h + 8, "WAVE", 4) != 0) {
        return;
    }
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
                mi->wav_byte_rate = le32(fmt + 8);
                mi->wav_block_align = (uint16_t)(fmt[12] | fmt[13] << 8);
                mi->bitrate = mi->wav_byte_rate * 8;
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            mi->audio_start = pos + 8;
            if (pos + 8 + (uint64_t)len < mi->audio_end) {
                mi->audio_end = pos + 8 + len;
            }
            if (mi->wav_byte_rate) {
                mi->duration_ms = (uint32_t)((uint64_t)len * 1000 / mi->wav_byte_rate);
                mi->seekable = mi->wav_block_align > 0;
            }
            return;
        }
        pos += 8 + len + (len & 1);
    }
}

/* ---- Ogg (Opus, Vorbis) ---- */

typedef struct {
    uint32_t off;
    uint32_t size; /* en-tête + données */
    int64_t granule;
    uint32_t serial;
    uint8_t type;
    uint8_t nseg;
    uint8_t lacing[255];
} ogg_page_t;

static bool ogg_parse_page(const uint8_t *b, size_t avail, ogg_page_t *pg)
{
    if (avail < 27 || memcmp(b, "OggS", 4) != 0 || b[4] != 0) {
        return false;
    }
    pg->type = b[5];
    pg->granule = (int64_t)le64(b + 6);
    pg->serial = le32(b + 14);
    pg->nseg = b[26];
    if (avail < 27u + pg->nseg) {
        return false;
    }
    uint32_t body = 0;
    for (int i = 0; i < pg->nseg; i++) {
        pg->lacing[i] = b[27 + i];
        body += b[27 + i];
    }
    pg->size = 27 + pg->nseg + body;
    return true;
}

/* Première page commençant à "from" ou après (recherche du motif "OggS"). */
static bool ogg_find_page(FILE *f, uint32_t from, uint32_t end, uint32_t serial, ogg_page_t *pg)
{
    enum { WIN = 16384 };
    uint8_t *buf = malloc(WIN);
    if (!buf) {
        return false;
    }
    bool ok = false;
    for (uint32_t base = from; !ok && base < end; base += WIN - 300) {
        size_t n = 0;
        if (fseek(f, base, SEEK_SET) == 0) {
            n = fread(buf, 1, WIN, f);
        }
        for (size_t i = 0; i + 27 <= n; i++) {
            if (buf[i] == 'O' && ogg_parse_page(buf + i, n - i, pg) && (!serial || pg->serial == serial)) {
                pg->off = base + (uint32_t)i;
                ok = pg->off < end;
                break;
            }
        }
        if (n < WIN) {
            break;
        }
    }
    free(buf);
    return ok;
}

static uint32_t ogg_granule_ms(const media_info_t *mi, int64_t g)
{
    if (mi->ogg_opus) {
        g -= mi->ogg_preskip;
        return g > 0 ? (uint32_t)(g / 48) : 0;
    }
    return mi->sample_rate && g > 0 ? (uint32_t)((uint64_t)g * 1000 / mi->sample_rate) : 0;
}

/* Position (granule) de la dernière page du flux : lecture de la fin du fichier en une fois,
 * recherche de la dernière page en partant de la fin (fenêtre agrandie si besoin). */
static int64_t ogg_last_granule(FILE *f, const media_info_t *mi)
{
    int64_t result = -1;
    for (uint32_t win = 16384; win <= 262144 && result < 0; win *= 4) {
        uint32_t start = mi->audio_end > mi->audio_start + win ? mi->audio_end - win : mi->audio_start;
        uint32_t len = mi->audio_end - start;
        uint8_t *buf = malloc(len);
        if (!buf) {
            break;
        }
        size_t n = 0;
        if (fseek(f, start, SEEK_SET) == 0) {
            n = fread(buf, 1, len, f);
        }
        ogg_page_t pg;
        for (size_t i = n >= 27 ? n - 27 : 0; n >= 27; i--) {
            if (buf[i] == 'O' && ogg_parse_page(buf + i, n - i, &pg) && pg.serial == mi->ogg_serial &&
                pg.granule != -1) {
                result = pg.granule;
                break;
            }
            if (i == 0) {
                break;
            }
        }
        free(buf);
        if (start == mi->audio_start) {
            break;
        }
    }
    return result;
}

/*
 * Commentaires Vorbis lus au fil de l'eau (paquet OpusTags ou "\x03vorbis") : les
 * commentaires volumineux, typiquement une pochette METADATA_BLOCK_PICTURE de plusieurs
 * centaines de Ko, sont sautés sans être lus.
 */
enum { VC_MAGIC, VC_VENDOR_LEN, VC_SKIP_VENDOR, VC_COUNT, VC_LEN, VC_DATA, VC_SKIP, VC_DONE };

typedef struct {
    int state;
    uint8_t acc[8];
    int acc_n;
    uint32_t need;
    uint32_t left; /* commentaires restants */
    char cbuf[512];
    uint32_t clen;
} vc_stream_t;

static void vc_next_comment(vc_stream_t *v)
{
    v->state = v->left-- > 0 ? VC_LEN : VC_DONE;
    v->acc_n = 0;
}

static void vc_feed(vc_stream_t *v, media_info_t *mi, const uint8_t *d, size_t n)
{
    size_t i = 0;
    while (i < n && v->state != VC_DONE) {
        switch (v->state) {
        case VC_MAGIC:
            v->acc[v->acc_n++] = d[i++];
            if (v->acc_n == 7 && memcmp(v->acc, "\x03vorbis", 7) == 0) {
                v->state = VC_VENDOR_LEN;
                v->acc_n = 0;
            } else if (v->acc_n == 8) {
                v->state = memcmp(v->acc, "OpusTags", 8) == 0 ? VC_VENDOR_LEN : VC_DONE;
                v->acc_n = 0;
            }
            break;
        case VC_VENDOR_LEN:
        case VC_COUNT:
        case VC_LEN:
            v->acc[v->acc_n++] = d[i++];
            if (v->acc_n == 4) {
                uint32_t val = le32(v->acc);
                v->acc_n = 0;
                if (v->state == VC_VENDOR_LEN) {
                    v->need = val;
                    v->state = val ? VC_SKIP_VENDOR : VC_COUNT;
                } else if (v->state == VC_COUNT) {
                    v->left = val;
                    vc_next_comment(v);
                } else {
                    v->clen = val;
                    v->need = val;
                    v->state = val < sizeof(v->cbuf) ? VC_DATA : VC_SKIP;
                    if (val == 0) {
                        vc_next_comment(v);
                    }
                }
            }
            break;
        case VC_SKIP_VENDOR:
        case VC_SKIP: {
            size_t take = n - i < v->need ? n - i : v->need;
            i += take;
            v->need -= (uint32_t)take;
            if (v->need == 0) {
                if (v->state == VC_SKIP_VENDOR) {
                    v->state = VC_COUNT;
                    v->acc_n = 0;
                } else {
                    vc_next_comment(v);
                }
            }
            break;
        }
        case VC_DATA: {
            size_t take = n - i < v->need ? n - i : v->need;
            memcpy(v->cbuf + (v->clen - v->need), d + i, take);
            i += take;
            v->need -= (uint32_t)take;
            if (v->need == 0) {
                vorbis_comment(mi, v->cbuf, v->clen);
                vc_next_comment(v);
            }
            break;
        }
        }
    }
}

static bool vc_skipping(const vc_stream_t *v, uint32_t n)
{
    return (v->state == VC_SKIP || v->state == VC_SKIP_VENDOR) && v->need >= n;
}

/* Saute n octets sans les lire (à n'appeler que si vc_skipping(v, n)). */
static void vc_skip_bytes(vc_stream_t *v, uint32_t n)
{
    if (!vc_skipping(v, n)) {
        return;
    }
    v->need -= n;
    if (v->need == 0) {
        if (v->state == VC_SKIP_VENDOR) {
            v->state = VC_COUNT;
            v->acc_n = 0;
        } else {
            vc_next_comment(v);
        }
    }
}

static void probe_ogg(FILE *f, media_info_t *mi)
{
    enum { PKT_CAP = 512, BODY_MAX = 255 * 255 };
    uint8_t *pkt = malloc(PKT_CAP);
    uint8_t *body = malloc(BODY_MAX);
    vc_stream_t *vc = calloc(1, sizeof(vc_stream_t));
    if (!pkt || !body || !vc) {
        free(pkt);
        free(body);
        free(vc);
        return;
    }
    /* Parcourt les pages d'en-tête jusqu'à la première page audio. Paquet 0 :
     * identification ; paquet 1 : commentaires, lus au fil de l'eau. */
    uint32_t off = 0;
    size_t plen = 0;
    int packet = 0;
    ogg_page_t pg;
    for (int guard = 0; guard < 4096 && off < mi->audio_end; guard++) {
        uint8_t hdr[27 + 255];
        size_t n = 0;
        if (fseek(f, off, SEEK_SET) == 0) {
            n = fread(hdr, 1, sizeof(hdr), f);
        }
        if (!ogg_parse_page(hdr, n, &pg)) {
            break;
        }
        if (guard == 0) {
            mi->ogg_serial = pg.serial;
        }
        if (pg.serial == mi->ogg_serial) {
            /* Les en-têtes (2 paquets pour Opus, 3 pour Vorbis) se terminent en fin de page :
             * la page suivante est la première page audio. */
            int header_packets = mi->ogg_opus ? 2 : 3;
            if (packet >= header_packets) {
                mi->audio_start = off;
                break;
            }
            uint32_t blen = pg.size - 27 - pg.nseg;
            /* Inutile de lire une page entièrement couverte par un saut (pochette). */
            bool read_body = packet == 0 || (packet == 1 && vc->state != VC_DONE && !vc_skipping(vc, blen));
            if (read_body && (fseek(f, off + 27 + pg.nseg, SEEK_SET) != 0 || fread(body, 1, blen, f) != blen)) {
                break;
            }
            uint32_t bp = 0;
            for (int i = 0; i < pg.nseg; i++) {
                uint32_t seg = pg.lacing[i];
                if (packet == 0 && read_body && plen + seg <= PKT_CAP) {
                    memcpy(pkt + plen, body + bp, seg);
                    plen += seg;
                } else if (packet == 1) {
                    if (read_body) {
                        vc_feed(vc, mi, body + bp, seg);
                    } else {
                        vc_skip_bytes(vc, seg);
                    }
                }
                bp += seg;
                if (seg < 255) { /* fin de paquet */
                    if (packet == 0 && plen >= 19 && memcmp(pkt, "OpusHead", 8) == 0) {
                        mi->ogg_opus = true;
                        mi->ogg_preskip = (uint16_t)(pkt[10] | pkt[11] << 8);
                        mi->sample_rate = 48000;
                    } else if (packet == 0 && plen >= 16 && memcmp(pkt, "\x01vorbis", 7) == 0) {
                        mi->sample_rate = le32(pkt + 12);
                    }
                    packet++;
                    plen = 0;
                }
            }
        }
        off += pg.size;
    }
    free(pkt);
    free(body);
    free(vc);
    if (!mi->audio_start || !mi->sample_rate) {
        return;
    }
    int64_t last = ogg_last_granule(f, mi);
    if (last > 0) {
        mi->duration_ms = ogg_granule_ms(mi, last);
        if (mi->duration_ms) {
            mi->bitrate = (uint32_t)((uint64_t)(mi->audio_end - mi->audio_start) * 8 * 1000 / mi->duration_ms);
            mi->seekable = true;
        }
    }
}

/*
 * Recherche par dichotomie de la dernière page dont la position (granule) précède la
 * cible ; la lecture reprend à la page suivante. Les en-têtes (OpusHead, OpusTags...)
 * sont renvoyés d'abord au décodeur.
 */
static bool seek_ogg(FILE *f, const media_info_t *mi, uint32_t ms, media_seek_t *out)
{
    int64_t target = mi->ogg_opus ? (int64_t)ms * 48 + mi->ogg_preskip
                                  : (int64_t)((uint64_t)ms * mi->sample_rate / 1000);
    ogg_page_t pg, best = {0};
    bool have_best = false;
    uint32_t lo = mi->audio_start, hi = mi->audio_end;
    for (int iter = 0; iter < 40 && hi - lo > 8192; iter++) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (!ogg_find_page(f, mid, hi, mi->ogg_serial, &pg)) {
            hi = mid;
            continue;
        }
        if (pg.granule != -1 && pg.granule <= target) {
            best = pg;
            have_best = true;
            lo = pg.off + pg.size;
        } else {
            hi = mid;
        }
    }
    /* Affinage linéaire sur les quelques pages restantes. */
    uint32_t from = have_best ? best.off + best.size : mi->audio_start;
    for (int i = 0; i < 64 && ogg_find_page(f, from, mi->audio_end, mi->ogg_serial, &pg); i++) {
        if (pg.granule != -1 && pg.granule > target) {
            break;
        }
        if (pg.granule != -1) {
            best = pg;
            have_best = true;
        }
        from = pg.off + pg.size;
    }
    if (!have_best) {
        return false;
    }
    out->offset = best.off + best.size;
    out->actual_ms = ogg_granule_ms(mi, best.granule);
    out->header_end = mi->audio_start;
    return out->offset < mi->audio_end;
}

/* ---- Point d'entrée ---- */

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
    uint32_t id3 = fmt == AUDIO_FMT_OGG ? 0 : parse_id3v2(f, mi);
    if (id3 > (uint32_t)size) {
        id3 = 0;
    }
    switch (fmt) {
    case AUDIO_FMT_MP3:
        parse_id3v1(f, mi, size);
        probe_mp3(f, mi, id3);
        mi->seekable = mi->duration_ms > 0 && mi->audio_end > mi->audio_start;
        break;
    case AUDIO_FMT_FLAC:
        probe_flac(f, mi, id3);
        break;
    case AUDIO_FMT_WAV:
        probe_wav(f, mi);
        break;
    case AUDIO_FMT_OGG:
        probe_ogg(f, mi);
        break;
    default:
        break;
    }
    return true;
}

static bool seek_mp3(const media_info_t *mi, uint32_t ms, media_seek_t *out)
{
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
    out->offset = mi->audio_start + (uint32_t)pos; /* le décodeur se recale sur la trame suivante */
    out->header_end = 0;
    out->actual_ms = ms;
    return true;
}

static bool seek_wav(const media_info_t *mi, uint32_t ms, media_seek_t *out)
{
    uint64_t bytes = (uint64_t)ms * mi->wav_byte_rate / 1000;
    bytes -= bytes % mi->wav_block_align;
    out->offset = mi->audio_start + (uint32_t)bytes;
    out->header_end = mi->audio_start;
    out->actual_ms = (uint32_t)(bytes * 1000 / mi->wav_byte_rate);
    return out->offset < mi->audio_end;
}

bool media_seek(FILE *f, const media_info_t *mi, audio_fmt_t fmt, uint32_t ms, media_seek_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!mi->seekable || ms == 0) {
        return false;
    }
    if (mi->duration_ms && ms >= mi->duration_ms) {
        ms = mi->duration_ms - 1;
    }
    switch (fmt) {
    case AUDIO_FMT_MP3:
        return seek_mp3(mi, ms, out);
    case AUDIO_FMT_FLAC:
        return f && seek_flac(f, mi, ms, out);
    case AUDIO_FMT_WAV:
        return seek_wav(mi, ms, out);
    case AUDIO_FMT_OGG:
        return f && seek_ogg(f, mi, ms, out);
    default:
        return false;
    }
}
