#include <stdlib.h>

#include "radio.h"
#include "test.h"

static char g_urls[4][RADIO_URL_MAX];
static int g_nurls;

static void collect(const char *url, void *ctx)
{
    if (g_nurls < 4) {
        snprintf(g_urls[g_nurls++], RADIO_URL_MAX, "%s", url);
    }
}

void test_radio(void)
{
    /* Listes .m3u : commentaires, fins de ligne Windows, chemins locaux ignorés */
    g_nurls = 0;
    CHECK(radio_playlist_urls("#EXTM3U\r\n#EXTINF:-1,France Inter\r\n  http://icecast.radiofrance.fr/franceinter-midfi.mp3 \r\n"
                              "musique/locale.mp3\r\nhttps://exemple.fr/flux.aac\r\n",
                              collect, NULL) == 2);
    CHECK_STR(g_urls[0], "http://icecast.radiofrance.fr/franceinter-midfi.mp3");
    CHECK_STR(g_urls[1], "https://exemple.fr/flux.aac");
    /* .pls */
    char url[RADIO_URL_MAX];
    CHECK(radio_playlist_first("[playlist]\nNumberOfEntries=2\nFile1=http://a.fr:8000/live\nTitle1=A\nFile2=http://b.fr/x\n",
                               url, sizeof(url)));
    CHECK_STR(url, "http://a.fr:8000/live");
    CHECK(!radio_playlist_first("#EXTM3U\n/sdcard/x.mp3\n", url, sizeof(url)));
    /* adresse trop longue : ignorée */
    char *longtext = malloc(RADIO_URL_MAX + 40);
    strcpy(longtext, "http://");
    memset(longtext + 7, 'a', RADIO_URL_MAX + 10);
    strcpy(longtext + 7 + RADIO_URL_MAX + 10, "\n");
    CHECK(!radio_playlist_first(longtext, url, sizeof(url)));
    free(longtext);

    CHECK(radio_is_url("HTTPS://x") && radio_is_url("http://x") && !radio_is_url("Radios/x.mp3") && !radio_is_url(NULL));

    /* Listes ou flux, format */
    CHECK(radio_is_playlist("audio/x-mpegurl", "http://x/live"));
    CHECK(radio_is_playlist("audio/x-scpls", "http://x/live"));
    CHECK(radio_is_playlist(NULL, "http://x/radio.M3U?id=3"));
    CHECK(radio_is_playlist("text/plain", "http://x/radio.pls"));
    CHECK(!radio_is_playlist("audio/mpeg", "http://x/radio.m3u"));
    CHECK(radio_fmt("audio/mpeg", "http://x/a.aac") == AUDIO_FMT_MP3);
    CHECK(radio_fmt("audio/aacp", "http://x/a") == AUDIO_FMT_AAC);
    CHECK(radio_fmt("Audio/AAC; charset=x", "http://x/a") == AUDIO_FMT_AAC);
    CHECK(radio_fmt("application/ogg", "http://x/a") == AUDIO_FMT_OGG);
    CHECK(radio_fmt(NULL, "http://x/flux.opus?token=1") == AUDIO_FMT_OGG);
    CHECK(radio_fmt("application/octet-stream", "http://x.fr/live") == AUDIO_FMT_MP3);
    CHECK(radio_fmt(NULL, "http://x.fr/dossier.aac/live") == AUDIO_FMT_MP3); /* extension du dossier : non */

    /* ICY : métadonnées retirées du flux, quel que soit le découpage des lectures */
    uint8_t stream[200];
    size_t n = 0;
    memcpy(stream + n, "AAAAAAAA", 8);
    n += 8;
    stream[n++] = 2; /* 32 octets de métadonnées */
    char meta[32] = "StreamTitle='L'Écho - X';";
    memcpy(stream + n, meta, 32);
    n += 32;
    memcpy(stream + n, "BBBBBBBB", 8);
    n += 8;
    stream[n++] = 0; /* bloc vide */
    memcpy(stream + n, "CC", 2);
    n += 2;
    for (size_t step = 1; step <= n; step++) {
        icy_t *s = malloc(sizeof(icy_t));
        icy_init(s, 8);
        uint8_t buf[200], audio[200];
        size_t got = 0;
        for (size_t off = 0; off < n; off += step) {
            size_t len = n - off < step ? n - off : step;
            memcpy(buf, stream + off, len);
            size_t k = icy_strip(s, buf, len);
            memcpy(audio + got, buf, k);
            got += k;
        }
        bool ok = got == 18 && memcmp(audio, "AAAAAAAABBBBBBBBCC", 18) == 0 && s->title_changed &&
                  strcmp(s->title, "L'Écho - X") == 0;
        if (!ok) {
            fprintf(stderr, "découpage %zu : %zu octets, titre « %s »\n", step, got, s->title);
        }
        CHECK(ok);
        free(s);
    }
    /* Sans métadonnées : flux inchangé */
    icy_t *s = malloc(sizeof(icy_t));
    icy_init(s, 0);
    uint8_t raw[4] = {1, 2, 3, 4};
    CHECK(icy_strip(s, raw, 4) == 4 && raw[3] == 4);
    free(s);

    /* Titres : Latin-1 converti, apostrophes, titre absent */
    char t[64];
    CHECK(icy_parse_title("StreamTitle='Caf\xe9 de la gare';StreamUrl='';", t, sizeof(t)));
    CHECK_STR(t, "Café de la gare");
    CHECK(icy_parse_title("StreamTitle='';", t, sizeof(t)) && t[0] == '\0');
    CHECK(!icy_parse_title("StreamUrl='x';", t, sizeof(t)));
    text_to_utf8("Élan vital", t, 4); /* « É » fait deux octets : coupé sans le casser */
    CHECK_STR(t, "\xc3\x89l");
}
