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

/* Associations simulées */
bool cards_lookup(const char *uid, char *folder, size_t len)
{
    const char *f = strcmp(uid, "AA") == 0 ? "Histoire" : strcmp(uid, "BB") == 0 ? "Comptines" : NULL;
    if (f) {
        snprintf(folder, len, "%s", f);
    }
    return f != NULL;
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
    CHECK(wait_state(PLAYER_STOPPED, 1000));
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

    printf("contrôleur : %d vérifications, %d échec(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
