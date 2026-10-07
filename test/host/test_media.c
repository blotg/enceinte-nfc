#include <stdlib.h>

#include "media_info.h"
#include "test.h"

static bool probe(const char *dir, const char *name, media_info_t *mi)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "fichier d'exemple manquant : %s\n", path);
        g_failures++;
        return false;
    }
    bool ok = media_probe(f, audio_fmt_from_name(name), mi);
    fclose(f);
    return ok;
}

static FILE *open_fixture(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "fichier d'exemple manquant : %s\n", path);
        g_failures++;
    }
    return f;
}

static bool bytes_at(FILE *f, uint32_t off, const void *expect, size_t n)
{
    uint8_t buf[8];
    return n <= sizeof(buf) && fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n &&
           memcmp(buf, expect, n) == 0;
}

/* Repositionnement dans des fichiers de 30 s produits par les vrais encodeurs. */
static void check_seek(const char *dir, const char *name, uint32_t target, uint32_t tolerance_before,
                       const char *magic, size_t magic_len, const char *title)
{
    FILE *f = open_fixture(dir, name);
    if (!f) {
        return;
    }
    media_info_t mi;
    audio_fmt_t fmt = audio_fmt_from_name(name);
    CHECK(media_probe(f, fmt, &mi));
    CHECK_NEAR(mi.duration_ms, 30000, 60);
    CHECK(mi.seekable);
    if (title) {
        CHECK_STR(mi.title, title);
    }
    media_seek_t sk;
    bool ok = media_seek(f, &mi, fmt, target, &sk);
    CHECK(ok);
    if (ok) {
        CHECK(sk.actual_ms <= target && sk.actual_ms + tolerance_before >= target);
        CHECK(sk.header_end == mi.audio_start && sk.header_end > 0 && sk.offset > sk.header_end);
        CHECK(magic == NULL || bytes_at(f, sk.offset, magic, magic_len));
    }
    /* au-delà de la fin : borné sans erreur */
    CHECK(media_seek(f, &mi, fmt, 999999, &sk) && sk.offset < mi.audio_end);
    fclose(f);
}

static void test_seek(const char *dir)
{
    check_seek(dir, "long.opus", 12000, 1500, "OggS", 4, "Chouette hulotte");
    check_seek(dir, "long.ogg", 12000, 1500, "OggS", 4, "Merle noir");
    check_seek(dir, "long_seektable.flac", 12000, 2100, "\xFF\xF8", 2, "Rouge-gorge");
    check_seek(dir, "long_noseektable.flac", 12000, 500, "\xFF\xF8", 2, NULL);
    check_seek(dir, "long.wav", 12000, 1, NULL, 0, NULL);

    /* Grosse pochette en tête des métadonnées : sautée sans être lue */
    check_seek(dir, "bigtags.opus", 12000, 1500, "OggS", 4, "Grosse pochette");

    FILE *f = open_fixture(dir, "long.opus");
    if (f) {
        media_info_t mi;
        media_probe(f, AUDIO_FMT_OGG, &mi);
        CHECK(mi.ogg_opus && mi.sample_rate == 48000);
        CHECK_STR(mi.artist, "Oiseaux de France");
        fclose(f);
    }
}

void test_media(const char *dir)
{
    media_info_t mi;

    /* MP3 CBR sans en-tête Xing, ID3v2.3 (ISO-8859-1 et UTF-16) */
    if (probe(dir, "cbr.mp3", &mi)) {
        CHECK_STR(mi.title, "Été indien");
        CHECK_STR(mi.artist, "日本の歌手");
        CHECK_STR(mi.album, "Album test");
        CHECK_STR(mi.track, "3/12");
        CHECK_STR(mi.genre, "Chanson");
        CHECK_NEAR(mi.duration_ms, 5000, 150);
        CHECK(mi.bitrate == 128000);
        CHECK(mi.sample_rate == 44100);
        CHECK(!mi.has_toc);
        media_seek_t sk;
        CHECK(mi.seekable && media_seek(NULL, &mi, AUDIO_FMT_MP3, 2500, &sk));
        uint32_t expect = mi.audio_start + (mi.audio_end - mi.audio_start) / 2;
        CHECK_NEAR(sk.offset, expect, 2000);
        CHECK(sk.header_end == 0 && sk.actual_ms == 2500);
        CHECK(!media_seek(NULL, &mi, AUDIO_FMT_MP3, 0, &sk)); /* 0 : rien à faire */
    }

    /* MP3 VBR avec Xing + table, ID3v2.4 (UTF-8) */
    if (probe(dir, "vbr.mp3", &mi)) {
        CHECK_STR(mi.title, "Été indien");
        CHECK_STR(mi.artist, "日本の歌手");
        CHECK_NEAR(mi.duration_ms, 5000, 150);
        CHECK(mi.has_toc);
        media_seek_t a, b;
        CHECK(media_seek(NULL, &mi, AUDIO_FMT_MP3, 1000, &a) && media_seek(NULL, &mi, AUDIO_FMT_MP3, 4000, &b));
        CHECK(a.offset > mi.audio_start && b.offset > a.offset && b.offset < mi.audio_end);
    }

    /* MP3 avec pochette intégrée (grosse trame APIC à sauter) */
    if (probe(dir, "art.mp3", &mi)) {
        CHECK_STR(mi.title, "Été indien");
        CHECK_STR(mi.album, "Album test");
        CHECK_NEAR(mi.duration_ms, 5000, 150);
    }

    /* MP3 avec seulement un tag ID3v1 */
    if (probe(dir, "v1.mp3", &mi)) {
        CHECK_STR(mi.title, "Vieux titre");
        CHECK_STR(mi.artist, "Vieil artiste");
        CHECK_STR(mi.album, "Vieil album");
        CHECK_STR(mi.date, "1999");
        CHECK_STR(mi.track, "7");
        CHECK_NEAR(mi.duration_ms, 5000, 150);
    }

    /* FLAC : durée exacte et commentaires Vorbis */
    if (probe(dir, "tone.flac", &mi)) {
        CHECK_STR(mi.title, "Titre FLAC");
        CHECK_STR(mi.artist, "Artiste FLAC");
        CHECK_STR(mi.track, "5");
        CHECK_NEAR(mi.duration_ms, 5000, 2);
        CHECK(mi.sample_rate == 44100);
    }

    /* WAV */
    if (probe(dir, "tone.wav", &mi)) {
        CHECK_NEAR(mi.duration_ms, 5000, 2);
        CHECK(mi.bitrate == 44100 * 16 * 2);
    }

    test_seek(dir);

    /* Fichier corrompu : pas de plantage, pas de durée */
    if (probe(dir, "garbage.mp3", &mi)) {
        CHECK(mi.duration_ms == 0);
        CHECK(mi.title[0] == '\0');
    }

    /* Tag ID3 dont la taille déclarée dépasse le fichier */
    if (probe(dir, "truncated.mp3", &mi)) {
        CHECK(mi.duration_ms == 0);
    }
}
