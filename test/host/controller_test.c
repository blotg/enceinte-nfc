/*
 * Test du contrôleur de cartes avec le vrai lecteur (I2S et décodeur simulés).
 * Délai de reprise compilé à 2 s (CONFIG_ENC_RESUME_TIMEOUT_S).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "controller.h"
#include "player.h"
#include "test.h"

int g_failures;
int g_checks;

void mock_set_resume(uint32_t timeout_s, bool after_other);

/* Associations simulées ; la carte EE reprend même après une autre carte (réglage propre). */
bool cards_get(const char *uid, card_entry_t *out)
{
    const char *f = strcmp(uid, "AA") == 0   ? "Histoire"
                    : strcmp(uid, "BB") == 0 ? "Comptines"
                    : strcmp(uid, "EE") == 0 ? "Livre"
                                             : NULL;
    if (!f) {
        return false;
    }
    snprintf(out->uid, sizeof(out->uid), "%s", uid);
    snprintf(out->folder, sizeof(out->folder), "%s", f);
    out->resume_s = CARD_DEFAULT;
    out->resume_other = strcmp(uid, "EE") == 0 ? 1 : CARD_DEFAULT;
    return true;
}

static bool copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[65536];
    size_t n;
    bool ok = a && b;
    while (ok && (n = fread(buf, 1, sizeof(buf), a)) > 0) {
        ok = fwrite(buf, 1, n, b) == n;
    }
    if (a) {
        fclose(a);
    }
    if (b) {
        fclose(b);
    }
    return ok;
}

static player_status_t status(void)
{
    player_status_t s;
    player_get_status(&s);
    return s;
}

static bool wait_state(player_state_t want, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (status().state == want) {
            return true;
        }
        usleep(10000);
    }
    return false;
}

static void make_file(const char *path, size_t size)
{
    FILE *f = fopen(path, "wb");
    char *buf = calloc(1, size);
    fwrite(buf, 1, size, f);
    free(buf);
    fclose(f);
}

static void card(bool on, const char *uid)
{
    controller_on_nfc(on, uid);
    usleep(150000); /* le contrôleur traite l'évènement */
}

int main(int argc, char **argv)
{
    const char *sd = MUSIC_ROOT;
    char p[512];
    mkdir(sd, 0755);
    snprintf(p, sizeof(p), "%s/Histoire", sd);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Comptines", sd);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Histoire/1 debut.mp3", sd);
    make_file(p, 12 << 20); /* ~70 s simulées, ~3,5 s réelles */
    snprintf(p, sizeof(p), "%s/Histoire/2 suite.mp3", sd);
    make_file(p, 12 << 20);
    snprintf(p, sizeof(p), "%s/Comptines/a.mp3", sd);
    make_file(p, 2 << 20); /* ~12 s simulées */
    snprintf(p, sizeof(p), "%s/Comptines/b.mp3", sd);
    make_file(p, 2 << 20); /* ~12 s simulées */

    CHECK(player_init(30, 100, controller_on_player_event) == ESP_OK);
    CHECK(controller_start() == ESP_OK);

    /* 1. Carte posée : lecture du dossier associé */
    card(true, "AA");
    CHECK(wait_state(PLAYER_PLAYING, 2000));
    CHECK_STR(status().file, "Histoire/1 debut.mp3");
    CHECK(status().queue_len == 2);

    /* 2. Retrait : pause, reprise possible */
    usleep(600000);
    card(false, "AA");
    CHECK(wait_state(PLAYER_PAUSED, 1000));
    uint32_t e1 = status().elapsed_ms;
    CHECK(e1 > 3000);
    controller_status_t cs;
    controller_get_status(&cs);
    CHECK(cs.resume_remaining_s >= 1 && cs.resume_remaining_s <= 2);

    /* 3. Reposée dans le délai : reprise au même endroit */
    usleep(300000);
    card(true, "AA");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK_STR(status().file, "Histoire/1 debut.mp3");
    CHECK(status().elapsed_ms >= e1);

    /* 4. Une autre carte entre-temps : la première recommence au début */
    card(false, "AA");
    CHECK(wait_state(PLAYER_PAUSED, 1000));
    card(true, "BB");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(strncmp(status().file, "Comptines/", 10) == 0);
    card(false, "BB");
    card(true, "AA");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK_STR(status().file, "Histoire/1 debut.mp3");
    CHECK(status().elapsed_ms < e1);

    /* 5. Carte inconnue entre-temps : même règle (et le lecteur s'arrête) */
    usleep(400000);
    card(false, "AA");
    CHECK(wait_state(PLAYER_PAUSED, 1000));
    uint32_t e2 = status().elapsed_ms;
    card(true, "CC");
    CHECK(status().state == PLAYER_PAUSED); /* la carte inconnue ne touche pas au lecteur */
    controller_get_status(&cs);
    CHECK_STR(cs.last_unknown_uid, "CC");
    card(false, "CC");
    card(true, "AA");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(status().elapsed_ms < e2);

    /* 6. Délai dépassé : arrêt, puis recommence au début */
    usleep(400000);
    card(false, "AA");
    CHECK(wait_state(PLAYER_PAUSED, 1000));
    uint32_t e3 = status().elapsed_ms;
    CHECK(wait_state(PLAYER_STOPPED, 4000)); /* 2 s + tic d'une seconde */
    card(true, "AA");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK_STR(status().file, "Histoire/1 debut.mp3");
    CHECK(status().elapsed_ms < e3);

    /* 7. Playlist terminée carte posée, puis reposée : recommence */
    card(false, "AA");
    card(true, "BB");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(wait_state(PLAYER_STOPPED, 3000)); /* deux morceaux courts */
    card(false, "BB");
    card(true, "BB");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(status().song == 0);

    /* 8. File modifiée par une application pendant l'absence de la carte : recommence */
    card(false, "BB");
    card(true, "AA");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    usleep(400000);
    card(false, "AA");
    CHECK(wait_state(PLAYER_PAUSED, 1000));
    uint32_t e4 = status().elapsed_ms;
    player_queue_add("Comptines/a.mp3", -1, NULL);
    card(true, "AA");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(status().queue_len == 2);
    CHECK(status().elapsed_ms < e4);

    /* 9. Mode association : la carte est capturée, la lecture n'est pas affectée */
    card(false, "AA");
    CHECK(wait_state(PLAYER_PAUSED, 1000));
    controller_learn_start();
    usleep(100000);
    controller_get_status(&cs);
    CHECK(cs.learning);
    card(true, "DD");
    controller_get_status(&cs);
    CHECK(!cs.learning);
    CHECK_STR(cs.learned_uid, "DD");
    CHECK(status().state == PLAYER_PAUSED);
    card(false, "DD");
    card(true, "AA"); /* la carte d'association ne compte pas comme « autre carte » */
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(status().elapsed_ms > 1000);

    /* 10. Délai « toujours » : la carte reprend même après le délai habituel */
    card(false, "AA");
    CHECK(wait_state(PLAYER_PAUSED, 1000));
    uint32_t e5 = status().elapsed_ms;
    mock_set_resume(0, false);
    usleep(3200000); /* plus que les 2 s du réglage par défaut */
    CHECK(status().state == PLAYER_PAUSED);
    controller_get_status(&cs);
    CHECK(cs.resume_remaining_s == -1);
    card(true, "AA");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(status().elapsed_ms >= e5);
    mock_set_resume(2, false);

    /* 11. Reprise après une autre carte (réglage de la carte EE) : le dossier est rechargé et
     *     la lecture repart au morceau et à la position mémorisés, dans un vrai fichier WAV. */
    if (argc > 1) {
        char src[512];
        snprintf(p, sizeof(p), "%s/Livre", sd);
        mkdir(p, 0755);
        snprintf(src, sizeof(src), "%s/long.wav", argv[1]);
        snprintf(p, sizeof(p), "%s/Livre/1.wav", sd);
        CHECK(copy_file(src, p));
        snprintf(p, sizeof(p), "%s/Livre/2.wav", sd);
        CHECK(copy_file(src, p));
        card(false, "AA");
        card(true, "EE");
        CHECK(wait_state(PLAYER_PLAYING, 1000));
        usleep(350000);
        card(false, "EE");
        CHECK(wait_state(PLAYER_PAUSED, 1000));
        player_status_t before = status();
        CHECK_STR(before.file, "Livre/1.wav");
        CHECK(before.elapsed_ms > 5000);
        card(true, "BB"); /* autre carte : la file est remplacée */
        CHECK(wait_state(PLAYER_PLAYING, 1000));
        CHECK(strncmp(status().file, "Comptines/", 10) == 0);
        card(false, "BB");
        controller_on_nfc(true, "EE");
        usleep(60000);
        player_status_t after = status();
        CHECK(after.state == PLAYER_PLAYING);
        CHECK_STR(after.file, "Livre/1.wav");
        CHECK(after.elapsed_ms + 50 >= before.elapsed_ms && after.elapsed_ms < before.elapsed_ms + 3000);
        /* la carte AA (réglage général) recommence, elle, au début */
        card(false, "EE");
        card(true, "AA");
        CHECK(wait_state(PLAYER_PLAYING, 1000));
        CHECK_STR(status().file, "Histoire/1 debut.mp3");
        CHECK(status().elapsed_ms < 5000);
    } else {
        printf("(scénario de reprise après une autre carte ignoré : fichiers d'exemple absents)\n");
    }

    printf("contrôleur : %d vérifications, %d échec(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
