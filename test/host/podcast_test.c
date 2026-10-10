/*
 * Test du module podcast (abonnement, téléchargements, épisodes gardés, renommage) avec un
 * « Internet » simulé : http://test/<chemin> sert WEB_ROOT/<chemin>, toute autre adresse est
 * injoignable. Carte SD simulée : MUSIC_ROOT.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cards.h"
#include "controller.h"
#include "net_http.h"
#include "player.h"
#include "podcast.h"
#include "test.h"
#include "wifi_mgr.h"

int g_failures;
int g_checks;

/* ---------- Internet simulé ---------- */

struct esp_http_client {
    FILE *f;
};

esp_http_client_handle_t http_get_open(const char *url, bool icy, int timeout_ms, http_info_t *info, char *final_url,
                                       size_t url_len, char *err, size_t errlen)
{
    memset(info, 0, sizeof(*info));
    const char *prefix = "http://test/";
    if (strncmp(url, prefix, strlen(prefix)) != 0) {
        snprintf(err, errlen, "serveur injoignable (simulation)");
        return NULL;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%.*s", WEB_ROOT, (int)strcspn(url + strlen(prefix), "?"), url + strlen(prefix));
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "erreur HTTP 404");
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    info->content_length = ftell(f);
    rewind(f);
    snprintf(info->content_type, sizeof(info->content_type), "%s",
             strstr(path, ".xml") ? "application/rss+xml" : "audio/mpeg");
    if (final_url) {
        snprintf(final_url, url_len, "%s", url);
    }
    struct esp_http_client *h = calloc(1, sizeof(*h));
    h->f = f;
    return h;
}

int esp_http_client_read(esp_http_client_handle_t h, char *buf, int len)
{
    return (int)fread(buf, 1, (size_t)len, h->f);
}

void http_close(esp_http_client_handle_t h)
{
    if (h) {
        fclose(h->f);
        free(h);
    }
}

/* ---------- Reste de l'enceinte, simulé ---------- */

static char g_playing[REL_PATH_MAX]; /* morceau « en cours de lecture » */
static char g_renamed_from[REL_PATH_MAX], g_renamed_to[REL_PATH_MAX];
static int g_card_renames;

void player_get_status(player_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->state = g_playing[0] ? PLAYER_PLAYING : PLAYER_STOPPED;
    snprintf(st->file, sizeof(st->file), "%s", g_playing);
}

void wifi_mgr_get_status(wifi_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->sta_connected = true;
}

void cards_on_folder_renamed(const char *old_rel, const char *new_rel)
{
    g_card_renames++;
    snprintf(g_renamed_from, sizeof(g_renamed_from), "%s", old_rel);
    snprintf(g_renamed_to, sizeof(g_renamed_to), "%s", new_rel);
}

void controller_on_path_renamed(const char *from, const char *to)
{
}

/* ---------- Outils ---------- */

static void write_file(const char *path, const char *text, size_t len)
{
    FILE *f = fopen(path, "wb");
    fwrite(text, 1, len, f);
    fclose(f);
}

static void make_episode(const char *name, int kb)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", WEB_ROOT, name);
    char *data = malloc((size_t)kb * 1024);
    for (int i = 0; i < kb * 1024; i++) {
        data[i] = (char)(name[2] + i % 97); /* contenu propre à chaque épisode */
    }
    write_file(path, data, (size_t)kb * 1024);
    free(data);
}

/* Flux RSS : épisodes 1 à n (le plus récent en dernier), « missing » : fichier absent. */
static void write_feed(int n, int missing)
{
    char *xml = malloc(16384);
    size_t o = (size_t)sprintf(xml, "<?xml version=\"1.0\"?><rss><channel><title>Contes du soir &amp; co</title>");
    for (int i = n; i >= 1; i--) { /* du plus récent au plus ancien, comme la plupart des flux */
        o += (size_t)sprintf(xml + o,
                             "<item><title>Épisode %d</title><guid>ep-%d</guid>"
                             "<pubDate>Wed, 0%d Oct 2026 06:00:00 +0200</pubDate>"
                             "<enclosure url=\"http://test/%s%d.mp3?source=rss\" type=\"audio/mpeg\"/></item>",
                             i, i, i, i == missing ? "absent" : "ep", i);
    }
    o += (size_t)sprintf(xml + o, "</channel></rss>");
    char path[512];
    snprintf(path, sizeof(path), "%s/feed.xml", WEB_ROOT);
    write_file(path, xml, o);
    free(xml);
}

static bool wait_idle(void)
{
    for (int i = 0; i < 500; i++) {
        if (!podcast_busy()) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

static bool sd_has(const char *rel)
{
    char abs[ABS_PATH_MAX];
    struct stat st;
    return path_to_abs(rel, abs, sizeof(abs)) && stat(abs, &st) == 0;
}

static bool same_content(const char *rel, const char *web_name)
{
    char a[ABS_PATH_MAX], b[512];
    path_to_abs(rel, a, sizeof(a));
    snprintf(b, sizeof(b), "%s/%s", WEB_ROOT, web_name);
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    bool same = fa && fb;
    while (same) {
        int ca = fgetc(fa), cb = fgetc(fb);
        same = ca == cb;
        if (ca == EOF || cb == EOF) {
            break;
        }
    }
    if (fa) {
        fclose(fa);
    }
    if (fb) {
        fclose(fb);
    }
    return same;
}

static int part_files(const char *rel)
{
    char abs[ABS_PATH_MAX];
    path_to_abs(rel, abs, sizeof(abs));
    DIR *d = opendir(abs);
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        n += strstr(e->d_name, ".part") || strstr(e->d_name, ".tmp") ? 1 : 0;
    }
    if (d) {
        closedir(d);
    }
    return n;
}

int main(void)
{
    mkdir(MUSIC_ROOT, 0755);
    mkdir(WEB_ROOT, 0755);
    for (int i = 1; i <= 7; i++) {
        char name[24];
        snprintf(name, sizeof(name), "ep%d.mp3", i);
        make_episode(name, 20 + i);
    }
    podcast_start();
    podcast_info_t pi;
    char folder[REL_PATH_MAX], err[96];

    /* 1. Abonnement sans nom : dossier provisoire, renommé d'après le titre du flux (les
     * cartes suivent) ; les 2 épisodes les plus récents sont téléchargés */
    write_feed(3, 0);
    CHECK(podcast_subscribe("http://test/feed.xml", "", 2, folder, sizeof(folder), err, sizeof(err)) == ESP_OK);
    CHECK_STR(folder, "Podcasts/Nouveau podcast");
    CHECK(wait_idle());
    const char *pf = "Podcasts/Contes du soir & co";
    CHECK(podcast_is_folder(pf) && !sd_has("Podcasts/Nouveau podcast"));
    CHECK(g_card_renames == 1);
    CHECK_STR(g_renamed_to, pf);
    CHECK(podcast_get(pf, &pi));
    CHECK(pi.episodes == 2 && pi.keep == 2 && pi.last_check > 0 && !pi.syncing && pi.last_error[0] == '\0');
    CHECK_STR(pi.title, "Contes du soir & co");
    CHECK(same_content("Podcasts/Contes du soir & co/2026-10-03 06h00 - Épisode 3.mp3", "ep3.mp3"));
    CHECK(same_content("Podcasts/Contes du soir & co/2026-10-02 06h00 - Épisode 2.mp3", "ep2.mp3"));
    CHECK(!sd_has("Podcasts/Contes du soir & co/2026-10-01 06h00 - Épisode 1.mp3"));
    CHECK(part_files(pf) == 0);

    /* 2. Nouvel épisode : téléchargé ; le plus ancien sort des 2 gardés, mais on l'écoute :
     * gardé pour l'instant, supprimé à la vérification suivante */
    write_feed(4, 0);
    snprintf(g_playing, sizeof(g_playing), "%s/2026-10-02 06h00 - Épisode 2.mp3", pf);
    podcast_sync_now(pf);
    CHECK(wait_idle());
    CHECK(sd_has("Podcasts/Contes du soir & co/2026-10-04 06h00 - Épisode 4.mp3"));
    CHECK(sd_has("Podcasts/Contes du soir & co/2026-10-02 06h00 - Épisode 2.mp3"));
    g_playing[0] = '\0';
    podcast_sync_now(pf);
    CHECK(wait_idle());
    CHECK(!sd_has("Podcasts/Contes du soir & co/2026-10-02 06h00 - Épisode 2.mp3"));
    CHECK(podcast_get(pf, &pi) && pi.episodes == 2);

    /* 3. Épisode supprimé à la main : pas retéléchargé */
    char abs[ABS_PATH_MAX];
    path_to_abs("Podcasts/Contes du soir & co/2026-10-03 06h00 - Épisode 3.mp3", abs, sizeof(abs));
    unlink(abs);
    podcast_sync_now(pf);
    CHECK(wait_idle());
    CHECK(!sd_has("Podcasts/Contes du soir & co/2026-10-03 06h00 - Épisode 3.mp3"));
    CHECK(podcast_get(pf, &pi) && pi.episodes == 1);

    /* 4. Épisode introuvable sur le serveur : erreur signalée, rien de corrompu */
    write_feed(5, 5);
    podcast_sync_now(pf);
    CHECK(wait_idle());
    CHECK(podcast_get(pf, &pi) && strstr(pi.last_error, "404") != NULL && pi.episodes == 1);
    CHECK(part_files(pf) == 0);

    /* 5. Réglages : flux injoignable, puis de nouveau joignable avec 3 épisodes gardés */
    CHECK(podcast_update(pf, "http://ailleurs.example/feed.xml", -1) == ESP_OK);
    CHECK(wait_idle());
    CHECK(podcast_get(pf, &pi) && strstr(pi.last_error, "flux injoignable") != NULL);
    write_feed(5, 0);
    CHECK(podcast_update(pf, "http://test/feed.xml", 3) == ESP_OK);
    CHECK(wait_idle());
    CHECK(podcast_get(pf, &pi) && pi.keep == 3 && pi.last_error[0] == '\0');
    CHECK(sd_has("Podcasts/Contes du soir & co/2026-10-05 06h00 - Épisode 5.mp3"));
    CHECK(pi.episodes == 2); /* 5 et 4 ; le 3, supprimé à la main, n'est pas revenu */
    CHECK(podcast_update(pf, "ftp://x", -1) == ESP_ERR_INVALID_ARG);

    /* 5 bis. Plus d'épisodes gardés : les anciens, supprimés quand ils sont sortis de la
     * liste, reviennent ; celui supprimé à la main, non */
    CHECK(podcast_update(pf, NULL, 5) == ESP_OK);
    CHECK(wait_idle());
    CHECK(podcast_get(pf, &pi) && pi.keep == 5 && pi.episodes == 4);
    CHECK(sd_has("Podcasts/Contes du soir & co/2026-10-01 06h00 - Épisode 1.mp3"));
    CHECK(sd_has("Podcasts/Contes du soir & co/2026-10-02 06h00 - Épisode 2.mp3"));
    CHECK(!sd_has("Podcasts/Contes du soir & co/2026-10-03 06h00 - Épisode 3.mp3"));

    /* 6. Désabonnement : les épisodes restent */
    CHECK(podcast_unsubscribe(pf) == ESP_OK);
    CHECK(!podcast_is_folder(pf) && sd_has("Podcasts/Contes du soir & co/2026-10-05 06h00 - Épisode 5.mp3"));
    CHECK(!podcast_get(pf, &pi));

    /* 7. Abonnement nommé : pas de renommage ; adresse invalide refusée */
    g_card_renames = 0;
    CHECK(podcast_subscribe("http://test/feed.xml", "Mes histoires", 1, folder, sizeof(folder), err, sizeof(err)) ==
          ESP_OK);
    CHECK_STR(folder, "Podcasts/Mes histoires");
    CHECK(wait_idle());
    CHECK(g_card_renames == 0 && podcast_get(folder, &pi) && pi.episodes == 1);
    CHECK_STR(pi.title, "Contes du soir & co"); /* titre du flux, le dossier garde son nom */
    CHECK(podcast_subscribe("javascript:alert(1)", "", 1, folder, sizeof(folder), err, sizeof(err)) != ESP_OK);
    CHECK(strstr(err, "invalide") != NULL);

    /* 8. Tous les épisodes (0) : rien n'est jamais supprimé */
    write_feed(6, 0);
    CHECK(podcast_subscribe("http://test/feed.xml", "Intégrale", 0, folder, sizeof(folder), err, sizeof(err)) ==
          ESP_OK);
    CHECK(wait_idle());
    CHECK(podcast_get(folder, &pi) && pi.keep == 0 && pi.episodes == 6 && pi.last_error[0] == '\0');
    write_feed(7, 0);
    podcast_sync_now(folder);
    CHECK(wait_idle());
    CHECK(podcast_get(folder, &pi) && pi.episodes == 7);
    CHECK(sd_has("Podcasts/Intégrale/2026-10-01 06h00 - Épisode 1.mp3"));
    CHECK(podcast_update(folder, NULL, 2) == ESP_OK); /* puis 2 : les plus anciens partent */
    CHECK(wait_idle());
    CHECK(podcast_get(folder, &pi) && pi.keep == 2 && pi.episodes == 2);
    CHECK(sd_has("Podcasts/Intégrale/2026-10-07 06h00 - Épisode 7.mp3"));

    /* 9. Manifeste de la version 1 (épisodes dans le JSON) : repris, puis réécrit */
    CHECK(path_to_abs("Podcasts/Ancien", abs, sizeof(abs)) && mkdir(abs, 0755) == 0);
    const char *v1 = "{\"format\":\"enceinte-podcast\",\"version\":1,\"url\":\"http://test/feed.xml\","
                     "\"title\":\"Ancien\",\"keep\":2,\"last_check\":0,\"last_error\":\"\","
                     "\"episodes\":[{\"guid\":\"ep-7\",\"file\":\"supprimé.mp3\",\"pub\":1}]}";
    CHECK(path_to_abs("Podcasts/Ancien/.podcast.json", abs, sizeof(abs)));
    write_file(abs, v1, strlen(v1));
    podcast_sync_now("Podcasts/Ancien");
    CHECK(wait_idle());
    CHECK(podcast_get("Podcasts/Ancien", &pi) && pi.episodes == 1 && pi.keep == 2); /* le 6 ; le 7 est connu */
    CHECK(sd_has("Podcasts/Ancien/2026-10-06 06h00 - Épisode 6.mp3"));
    CHECK(sd_has("Podcasts/Ancien/.podcast-episodes.txt"));
    FILE *mf = fopen(abs, "rb");
    char mtxt[2048] = "";
    if (mf) {
        mtxt[fread(mtxt, 1, sizeof(mtxt) - 1, mf)] = '\0';
        fclose(mf);
    }
    CHECK(strstr(mtxt, "episodes") == NULL && strstr(mtxt, "\"version\":\t2") != NULL);
    podcast_sync_now("Podcasts/Ancien"); /* relu depuis le fichier des épisodes : rien de nouveau */
    CHECK(wait_idle());
    CHECK(podcast_get("Podcasts/Ancien", &pi) && pi.episodes == 1 && pi.last_error[0] == '\0');
    CHECK(!sd_has("Podcasts/Ancien/2026-10-07 06h00 - Épisode 7.mp3"));

    printf("podcasts : %d vérifications, %d échec(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
