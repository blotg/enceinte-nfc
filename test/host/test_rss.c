#include <stdlib.h>

#include "rss.h"
#include "test.h"

static const char *FEED =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!-- généré par un hébergeur -->\n"
    "<rss version=\"2.0\" xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\">\n"
    "<channel>\n"
    "  <title>Histoires &amp; contes du soir</title>\n"
    "  <image><url>https://x.fr/logo.png</url><title>Logo</title></image>\n"
    "  <description><![CDATA[<p>Des <b>histoires</b> pour les enfants]]></description>\n"
    "  <item>\n"
    "    <title><![CDATA[Épisode 3 : « L'arbre » [suite]]]></title>\n"
    "    <itunes:title>Autre titre</itunes:title>\n"
    "    <guid isPermaLink=\"false\">abc-3</guid>\n"
    "    <pubDate>Wed, 08 Oct 2026 06:00:00 +0200</pubDate>\n"
    "    <enclosure url=\"https://cdn.x.fr/ep3.mp3?a=1&amp;b=2\" length=\"123\" type=\"audio/mpeg\"/>\n"
    "  </item>\n"
    "  <item><title>Annonce sans fichier</title><guid>abc-0</guid></item>\n"
    "  <item>\n"
    "    <title>Le loup &#233;tait l&#xE0;</title>\n"
    "    <description>&lt;p&gt;Texte &lt;b&gt;long&lt;/b&gt;&lt;/p&gt;</description>\n"
    "    <pubDate>8 Oct 2026 18:30 GMT</pubDate>\n"
    "    <enclosure type='audio/x-m4a' url='https://cdn.x.fr/loup' />\n"
    "  </item>\n"
    "</channel></rss>\n";

typedef struct {
    rss_item_t items[4];
    int n;
    int stop_after; /* 0 : jamais */
} got_t;

static bool on_item(const rss_item_t *it, void *ctx)
{
    got_t *g = ctx;
    if (g->n < 4) {
        g->items[g->n] = *it;
    }
    g->n++;
    return !(g->stop_after && g->n >= g->stop_after);
}

static bool parse_in_chunks(size_t step, got_t *g, char *title, size_t tlen)
{
    memset(g, 0, sizeof(*g));
    rss_parser_t *p = rss_new(on_item, g);
    size_t len = strlen(FEED);
    for (size_t off = 0; off < len; off += step) {
        rss_feed(p, FEED + off, len - off < step ? len - off : step);
    }
    snprintf(title, tlen, "%s", rss_channel_title(p));
    rss_free(p);
    return g->n == 2;
}

void test_rss(void)
{
    got_t *g = malloc(sizeof(got_t));
    char title[RSS_TITLE_MAX];
    CHECK(parse_in_chunks(strlen(FEED), g, title, sizeof(title)));
    CHECK_STR(title, "Histoires & contes du soir");
    rss_item_t *a = malloc(2 * sizeof(rss_item_t)), *b = a + 1; /* copies : g resservira */
    *a = g->items[0];
    *b = g->items[1];
    CHECK_STR(a->title, "Épisode 3 : « L'arbre » [suite]");
    CHECK_STR(a->guid, "abc-3");
    CHECK_STR(a->url, "https://cdn.x.fr/ep3.mp3?a=1&b=2");
    CHECK_STR(a->type, "audio/mpeg");
    CHECK(a->pub == 1791432000); /* 2026-10-08 04:00:00 UTC */
    CHECK_STR(a->day, "2026-10-08");
    CHECK_STR(a->hm, "06h00");
    CHECK_STR(b->title, "Le loup était là");
    CHECK_STR(b->guid, "https://cdn.x.fr/loup"); /* pas de guid : l'adresse du fichier */
    CHECK_STR(b->hm, "18h30");
    CHECK(b->pub == 1791484200); /* 18:30 UTC */

    /* Découpage quelconque du flux : même résultat */
    bool all = true;
    for (size_t step = 1; step < 300; step++) {
        got_t *h = malloc(sizeof(got_t));
        char t2[RSS_TITLE_MAX];
        bool ok = parse_in_chunks(step, h, t2, sizeof(t2)) && strcmp(t2, title) == 0 &&
                  memcmp(h->items, g->items, 2 * sizeof(rss_item_t)) == 0;
        if (!ok && all) {
            fprintf(stderr, "flux RSS lu par morceaux de %zu : différent\n", step);
        }
        all = all && ok;
        free(h);
    }
    CHECK(all);

    /* Arrêt demandé par le rappel */
    memset(g, 0, sizeof(*g));
    g->stop_after = 1;
    rss_parser_t *p = rss_new(on_item, g);
    CHECK(!rss_feed(p, FEED, strlen(FEED)) && g->n == 1);
    rss_free(p);

    /* Noms de fichiers des épisodes */
    char name[160];
    rss_episode_name(a, name, sizeof(name));
    CHECK_STR(name, "2026-10-08 06h00 - Épisode 3 - « L'arbre » [suite].mp3");
    rss_episode_name(b, name, sizeof(name));
    CHECK_STR(name, "2026-10-08 18h30 - Le loup était là.m4a");
    rss_item_t c = {.url = "https://x/y", .title = ""};
    rss_episode_name(&c, name, sizeof(name));
    CHECK_STR(name, "Épisode.mp3");

    /* Dates */
    int64_t t;
    char day[11], hm[6];
    CHECK(rss_parse_date("Thu, 01 Jan 1970 00:00:00 GMT", &t, day, hm) && t == 0);
    CHECK(rss_parse_date("Tue, 07 Oct 2026 23:30:00 -0400", &t, day, hm) && t == 1791430200);
    CHECK_STR(day, "2026-10-07");
    CHECK(rss_parse_date("07 Oct 26 23:30 EDT", &t, day, hm) && t == 1791430200);
    CHECK(rss_parse_date("2026-10-08T03:30:00Z", &t, day, hm) && t == 1791430200);
    CHECK(rss_parse_date("2026-10-08T05:30:00+02:00", &t, day, hm) && t == 1791430200);
    CHECK(!rss_parse_date("demain", &t, day, hm));
    CHECK(!rss_parse_date("32 Oct 2026", &t, day, hm));
    CHECK(!rss_parse_date("", &t, day, hm));

    /* Les N plus récents, du plus récent au plus ancien ; sans date : les plus anciens */
    rss_item_t *top = calloc(3, sizeof(rss_item_t));
    int count = 0;
    int64_t pubs[] = {50, 10, 0, 90, 70, 30, 90};
    for (int i = 0; i < 7; i++) {
        rss_item_t it = {.pub = pubs[i]};
        snprintf(it.guid, sizeof(it.guid), "g%d", i);
        rss_keep_newest(top, 3, &count, &it);
    }
    CHECK(count == 3 && top[0].pub == 90 && top[1].pub == 90 && top[2].pub == 70);
    CHECK_STR(top[0].guid, "g3"); /* à date égale, l'ordre du flux */
    count = 0;
    rss_item_t undated = {.pub = 0};
    rss_keep_newest(top, 3, &count, &undated);
    rss_item_t dated = {.pub = 5};
    rss_keep_newest(top, 3, &count, &dated);
    CHECK(count == 2 && top[0].pub == 5 && top[1].pub == 0);
    free(top);

    /* Liste des épisodes retenus : les 3 plus récents, ou tous (0) */
    rss_list_t l;
    rss_list_init(&l, 3);
    for (int i = 0; i < 7; i++) {
        rss_item_t it = {.pub = pubs[i]};
        snprintf(it.guid, sizeof(it.guid), "g%d", i);
        CHECK(rss_list_add(&l, &it));
    }
    rss_list_finish(&l);
    CHECK(l.count == 3 && l.items[0].pub == 90 && l.items[1].pub == 90 && l.items[2].pub == 70);
    CHECK_STR(l.items[0].guid, "g3");
    CHECK_STR(l.items[1].guid, "g6");
    rss_list_free(&l);
    rss_list_init(&l, 0);
    for (int i = 0; i < 7; i++) {
        rss_item_t it = {.pub = pubs[i]};
        snprintf(it.guid, sizeof(it.guid), "g%d", i);
        CHECK(rss_list_add(&l, &it));
    }
    rss_list_finish(&l);
    CHECK(l.count == 7 && l.items[0].pub == 90 && l.items[2].pub == 70 && l.items[6].pub == 0);
    CHECK_STR(l.items[0].guid, "g3");
    rss_list_free(&l);
    /* flux du plus ancien au plus récent, plus long que la liste : les plus récents restent */
    rss_list_init(&l, 0);
    bool added = true;
    for (int i = 1; i <= RSS_LIST_MAX + 500; i++) {
        rss_item_t it = {.pub = i};
        added = rss_list_add(&l, &it) && added;
    }
    rss_list_finish(&l);
    CHECK(added && l.count == RSS_LIST_MAX && l.items[0].pub == RSS_LIST_MAX + 500 && l.items[RSS_LIST_MAX - 1].pub == 501);
    rss_list_free(&l);
    rss_list_init(&l, 100000);
    CHECK(l.limit == RSS_LIST_MAX);
    CHECK(rss_guid_id("ep-1") == rss_guid_id("ep-1") && rss_guid_id("ep-1") != rss_guid_id("ep-2"));
    free(a);
    free(g);
}
