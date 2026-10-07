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
        uint32_t mid = media_seek_offset(&mi, AUDIO_FMT_MP3, 2500);
        uint32_t expect = mi.audio_start + (mi.audio_end - mi.audio_start) / 2;
        CHECK_NEAR(mid, expect, 2000);
    }

    /* MP3 VBR avec Xing + table, ID3v2.4 (UTF-8) */
    if (probe(dir, "vbr.mp3", &mi)) {
        CHECK_STR(mi.title, "Été indien");
        CHECK_STR(mi.artist, "日本の歌手");
        CHECK_NEAR(mi.duration_ms, 5000, 150);
        CHECK(mi.has_toc);
        uint32_t a = media_seek_offset(&mi, AUDIO_FMT_MP3, 1000);
        uint32_t b = media_seek_offset(&mi, AUDIO_FMT_MP3, 4000);
        CHECK(a > mi.audio_start && b > a && b < mi.audio_end);
        CHECK(media_seek_offset(&mi, AUDIO_FMT_FLAC, 1000) == 0); /* recherche MP3 uniquement */
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
