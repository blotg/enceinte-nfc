/*
 * Test du contrôleur de cartes avec le vrai lecteur (I2S et décodeur simulés).
 * Délai de reprise compilé à 2 s (CONFIG_ENC_RESUME_TIMEOUT_S).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "controller.h"
#include "player.h"
#include "resume_store.h"
#include "test.h"

int g_failures;
int g_checks;

void mock_set_resume(uint32_t timeout_s, bool after_other);
void mock_set_shuffle(bool on);
void mock_set_repeat(bool on);
int shim_stream_opens(void);
void shim_stream_cut(void);
void mock_set_sound(uint8_t normalize, uint8_t compress);
int shim_i2s_level(void);

/* Mémoire permanente simulée : passe par la vraie sérialisation */
static uint8_t g_store[16][600];
static size_t g_store_len[16];
static int g_saves;
static int g_pos_saves; /* écritures de la position seule */
static int64_t g_full_at_us; /* heure du dernier point complet */

static int64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void resume_store_load(resume_point_t *points, int count)
{
    for (int i = 0; i < count; i++) {
        if (g_store_len[i]) {
            resume_decode(g_store[i], g_store_len[i], &points[i]);
        }
    }
}

esp_err_t resume_store_save(int slot, const resume_point_t *p)
{
    g_store_len[slot] = resume_encode(p, g_store[slot], sizeof(g_store[slot]));
    g_saves++;
    g_full_at_us = mono_us();
    return ESP_OK;
}

esp_err_t resume_store_save_position(int slot, uint32_t position_ms)
{
    resume_point_t p;
    if (g_store_len[slot] && resume_decode(g_store[slot], g_store_len[slot], &p)) {
        p.position_ms = position_ms;
        g_store_len[slot] = resume_encode(&p, g_store[slot], sizeof(g_store[slot]));
    }
    g_saves++;
    g_pos_saves++;
    return ESP_OK;
}

esp_err_t resume_store_erase(int slot)
{
    g_store_len[slot] = 0;
    g_saves++;
    return ESP_OK;
}

static bool stored(const char *uid, resume_point_t *out)
{
    for (int i = 0; i < 16; i++) {
        if (g_store_len[i] && resume_decode(g_store[i], g_store_len[i], out) && strcmp(out->uid, uid) == 0) {
            return true;
        }
    }
    return false;
}

/* Les écritures sont espacées d'au moins 10 s par carte : on attend l'écriture différée. */
static bool wait_stored(const char *uid, const char *track, bool removed, resume_point_t *out)
{
    for (int i = 0; i < 130; i++) {
        if (stored(uid, out) && strcmp(out->track, track) == 0 && out->removed == removed) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

/* Associations simulées ; la carte EE reprend même après une autre carte (réglage propre),
 * la carte SS aussi, lit son dossier dans un ordre aléatoire et compresse le son (niveau 2).
 * Mode sommeil : ZZ s'arrête après un morceau, TT après g_tt_minutes d'écoute.
 * Répétition : RR recommence sa playlist (g_rr_repeat). */
static char g_ee_folder[64] = "Livre";
static uint16_t g_tt_minutes = 1;
static int8_t g_rr_repeat = 1;

bool cards_get(const char *uid, card_entry_t *out)
{
    const char *f = strcmp(uid, "AA") == 0   ? "Histoire"
                    : strcmp(uid, "BB") == 0 ? "Comptines"
                    : strcmp(uid, "EE") == 0 ? g_ee_folder
                    : strcmp(uid, "SS") == 0 ? "Melange"
                    : strcmp(uid, "ZZ") == 0 ? "Comptines"
                    : strcmp(uid, "TT") == 0 ? "Sommeil"
                    : strcmp(uid, "RR") == 0 ? "Comptines"
                    : strcmp(uid, "WR") == 0 ? "Radio"
                    : strcmp(uid, "WK") == 0 ? "Radio absente"
                                             : NULL;
    if (!f) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->sleep_tracks = strcmp(uid, "ZZ") == 0 ? 1 : 0;
    out->sleep_minutes = strcmp(uid, "TT") == 0 ? g_tt_minutes : 0;
    snprintf(out->uid, sizeof(out->uid), "%s", uid);
    snprintf(out->folder, sizeof(out->folder), "%s", f);
    out->resume_s = CARD_DEFAULT;
    out->resume_other = strcmp(uid, "EE") == 0 || strcmp(uid, "SS") == 0 ? 1 : CARD_DEFAULT;
    out->shuffle = strcmp(uid, "SS") == 0 ? 1 : CARD_DEFAULT;
    out->repeat = strcmp(uid, "RR") == 0 ? g_rr_repeat : CARD_DEFAULT;
    out->normalize = CARD_DEFAULT;
    out->compress = strcmp(uid, "SS") == 0 ? 2 : CARD_DEFAULT;
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

/* Fichier « audio » non silencieux : le décodeur simulé en fait des échantillons de 8192. */
static void make_loud_file(const char *path, size_t size)
{
    FILE *f = fopen(path, "wb");
    char *buf = malloc(size);
    for (size_t i = 0; i < size; i += 2) {
        buf[i] = 0x00;
        buf[i + 1] = 0x20;
    }
    fwrite(buf, 1, size, f);
    free(buf);
    fclose(f);
}

static bool wait_sleep_done(int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        player_status_t s = status();
        if (s.state == PLAYER_PAUSED && s.sleep_done) {
            return true;
        }
        usleep(10000);
    }
    return false;
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
    snprintf(p, sizeof(p), "%s/Melange", sd);
    mkdir(p, 0755);
    for (int i = 0; i < 8; i++) {
        snprintf(p, sizeof(p), "%s/Melange/%d.mp3", sd, i);
        make_file(p, 3 << 20); /* ~18 s simulées, ~0,9 s réelles */
    }
    snprintf(p, sizeof(p), "%s/Sommeil", sd);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Sommeil/berceuse.mp3", sd);
    make_loud_file(p, 36 << 20); /* ~210 s simulées, ~10,5 s réelles */

    /* Livre audio de deux fichiers WAV réels (repositionnables), si disponibles */
    bool have_wav = argc > 1;
    if (have_wav) {
        char src[512];
        snprintf(p, sizeof(p), "%s/Livre", sd);
        mkdir(p, 0755);
        snprintf(src, sizeof(src), "%s/long.wav", argv[1]);
        snprintf(p, sizeof(p), "%s/Livre/1.wav", sd);
        have_wav = copy_file(src, p);
        snprintf(p, sizeof(p), "%s/Livre/2.wav", sd);
        have_wav = have_wav && copy_file(src, p);
        /* Position enregistrée avant une coupure de courant : carte EE, 2e fichier, 15 s */
        resume_point_t saved = {0};
        strcpy(saved.uid, "EE");
        strcpy(saved.folder, "Livre");
        strcpy(saved.track, "Livre/2.wav");
        saved.position_ms = 15000;
        g_store_len[3] = resume_encode(&saved, g_store[3], sizeof(g_store[3]));
    }

    CHECK(player_init(30, 100, controller_on_player_event) == ESP_OK);
    CHECK(controller_start() == ESP_OK);

    /* 0. Au démarrage, la position enregistrée est rechargée : la carte reprend là */
    if (have_wav) {
        mock_set_resume(0, false); /* sans délai : l'écriture différée a le temps de se faire */
        controller_on_nfc(true, "EE");
        usleep(80000);
        player_status_t st0 = status();
        CHECK(st0.state == PLAYER_PLAYING);
        CHECK_STR(st0.file, "Livre/2.wav");
        CHECK(st0.elapsed_ms >= 15000 && st0.elapsed_ms < 18000);
        card(false, "EE");
        CHECK(wait_state(PLAYER_PAUSED, 1000));
        resume_point_t sp;
        CHECK(wait_stored("EE", "Livre/2.wav", true, &sp) && sp.position_ms >= 15000);
        mock_set_resume(2, false);
    }

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
    if (have_wav) {
        card(false, "AA");
        card(true, "EE"); /* sa position précédente a expiré (délai de 2 s) : début du livre */
        CHECK(wait_state(PLAYER_PLAYING, 1000));
        CHECK_STR(status().file, "Livre/1.wav");
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

        /* 12. Dossier déplacé : la position mémorisée suit (fichiers et mémoire permanente) */
        mock_set_resume(0, false);
        card(false, "AA");
        card(true, "EE"); /* EE reprend (règle « après une autre carte ») */
        CHECK(wait_state(PLAYER_PLAYING, 1000));
        usleep(300000);
        card(false, "EE");
        char from[512], to[512];
        snprintf(p, sizeof(p), "%s/Audio", sd);
        mkdir(p, 0755);
        snprintf(from, sizeof(from), "%s/Livre", sd);
        snprintf(to, sizeof(to), "%s/Audio/Livre", sd);
        CHECK(rename(from, to) == 0);
        snprintf(g_ee_folder, sizeof(g_ee_folder), "Audio/Livre"); /* l'association suit aussi */
        controller_on_path_renamed("Livre", "Audio/Livre");
        resume_point_t mv;
        CHECK(wait_stored("EE", "Audio/Livre/1.wav", true, &mv) && strcmp(mv.folder, "Audio/Livre") == 0);
        CHECK_STR(status().file, "Audio/Livre/1.wav"); /* la file de lecture a suivi */
        card(true, "EE"); /* reprise immédiate de la carte, dans le dossier déplacé */
        CHECK(wait_state(PLAYER_PLAYING, 1000));
        CHECK_STR(status().file, "Audio/Livre/1.wav");
        CHECK(status().elapsed_ms > 5000);
        card(false, "EE");
        mock_set_resume(2, false);

        /* 13. Écritures espacées : retraits et poses rapides n'écrivent pas à chaque fois */
        int full_before = g_saves - g_pos_saves;
        for (int i = 0; i < 4; i++) {
            card(false, "AA");
            card(true, "AA");
        }
        CHECK(g_saves - g_pos_saves - full_before <= 2);

        /* 14. Lecture continue (temps réel) : la position seule est enregistrée toutes les
         * 2 s, carte toujours posée, sans attendre un retrait */
        setenv("SHIM_SPEEDUP", "1", 1);
        card(false, "AA");
        usleep(2500000); /* délai de reprise (2 s) dépassé pour EE */
        card(true, "EE"); /* début d'un morceau de 30 s */
        CHECK(wait_state(PLAYER_PLAYING, 1000));
        CHECK_STR(status().file, "Audio/Livre/1.wav");
        uint32_t e0 = status().elapsed_ms;
        CHECK(e0 < 2000);
        int pos_before = g_pos_saves;
        bool periodic = false;
        for (int i = 0; i < 250 && !periodic; i++) {
            usleep(100000);
            player_status_t now = status();
            resume_point_t sp;
            periodic = g_pos_saves - pos_before >= 3 && stored("EE", &sp) && !sp.removed &&
                       strcmp(sp.track, now.file) == 0 && sp.position_ms >= e0 + 4000 &&
                       sp.position_ms + 3500 >= now.elapsed_ms;
        }
        if (!periodic) {
            resume_point_t sp;
            bool ok = stored("EE", &sp);
            printf("e0=%u écritures position=%d stocké=%d %s %u, lecture %s %u\n", e0, g_pos_saves - pos_before, ok,
                   sp.track, sp.position_ms, status().file, status().elapsed_ms);
        }
        CHECK(periodic);
        /* un retrait en cours de lecture est enregistré tout de suite (plus de 10 s depuis
         * le dernier point complet), malgré les écritures de position récentes */
        while (mono_us() - g_full_at_us < 10500000) {
            usleep(100000);
        }
        card(false, "EE");
        resume_point_t rm;
        CHECK(stored("EE", &rm) && rm.removed);
        card(true, "EE");
        CHECK(wait_state(PLAYER_PLAYING, 1000));
        card(false, "EE");
        setenv("SHIM_SPEEDUP", "20", 1);
    } else {
        printf("(scénario de reprise après une autre carte ignoré : fichiers d'exemple absents)\n");
    }

    /* 15. Lecture aléatoire (réglage de la carte) : ordre mélangé, retrouvé à l'identique
     * quand la carte reprend après une autre carte (file rechargée) */
    card(true, "SS");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    char order[8][REL_PATH_MAX];
    CHECK(player_queue_length() == 8);
    bool in_order = true;
    for (int i = 0; i < 8; i++) {
        queue_item_t it;
        CHECK(player_queue_get(i, &it));
        snprintf(order[i], sizeof(order[i]), "%s", it.path);
        in_order = in_order && (i == 0 || strcmp(order[i - 1], order[i]) < 0);
    }
    CHECK(!in_order);
    CHECK_STR(status().file, order[0]);
    usleep(1200000); /* au milieu du 2e morceau */
    card(false, "SS");
    CHECK(wait_state(PLAYER_PAUSED, 1000));
    player_status_t at_removal = status();
    CHECK(at_removal.song >= 1);
    card(true, "BB");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    card(false, "BB");
    controller_on_nfc(true, "SS");
    usleep(60000); /* vérifie avant la fin du morceau repris */
    CHECK(status().state == PLAYER_PLAYING);
    bool same_order = player_queue_length() == 8;
    for (int i = 0; i < 8 && same_order; i++) {
        queue_item_t it;
        same_order = player_queue_get(i, &it) && strcmp(it.path, order[i]) == 0;
    }
    CHECK(same_order);
    CHECK(status().song == at_removal.song);
    CHECK_STR(status().file, at_removal.file);
    /* Son : compression propre à la carte SS, normalisation du réglage général */
    uint8_t norm = 9, comp = 9;
    player_get_sound(&norm, &comp);
    CHECK(norm == 0 && comp == 2);
    card(false, "SS");
    /* réglage général : la carte AA (sans réglage propre) passe en ordre aléatoire */
    mock_set_shuffle(true);
    usleep(2500000); /* délai de reprise dépassé : AA recommence */
    card(true, "AA");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(player_queue_length() == 2);
    resume_point_t ap;
    bool flagged = false;
    for (int i = 0; i < 120 && !flagged; i++) { /* écriture éventuellement différée */
        flagged = stored("AA", &ap) && ap.shuffle;
        usleep(100000);
    }
    CHECK(flagged);
    /* Carte sans réglage de son : réglage général, appliqué en cours de lecture (tic d'1 s) */
    player_get_sound(&norm, &comp);
    CHECK(norm == 0 && comp == 0);
    mock_set_sound(3, 1);
    usleep(1300000);
    player_get_sound(&norm, &comp);
    CHECK(norm == 3 && comp == 1);
    mock_set_sound(0, 0);
    card(false, "AA");
    mock_set_shuffle(false);

    /* 16. Volume borné par le volume maximum, y compris quand celui-ci baisse */
    player_set_max_volume(60);
    player_set_volume(90);
    CHECK(player_get_volume() == 60);
    player_set_volume(45);
    player_set_max_volume(40);
    CHECK(player_get_volume() == 40);
    player_set_volume(-5);
    CHECK(player_get_volume() == 0);
    player_set_max_volume(100);

    /* 17. Mode sommeil, en nombre de morceaux : pause au début du morceau suivant */
    player_set_volume(50);
    card(true, "ZZ");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK_STR(status().file, "Comptines/a.mp3");
    CHECK(status().sleep_tracks == 1);
    CHECK(wait_sleep_done(2500));
    player_status_t zs = status();
    CHECK_STR(zs.file, "Comptines/b.mp3");
    CHECK(zs.elapsed_ms < 500 && zs.sleep_tracks == 0);
    usleep(1200000);
    CHECK(status().state == PLAYER_PAUSED); /* carte toujours posée : rien ne repart */
    /* reposée : reprise au morceau suivant avec un nouveau décompte, puis fin de la playlist */
    card(false, "ZZ");
    card(true, "ZZ");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK_STR(status().file, "Comptines/b.mp3");
    CHECK(status().sleep_tracks == 1 && !status().sleep_done);
    CHECK(wait_state(PLAYER_STOPPED, 2500));
    CHECK(status().sleep_tracks == 0 && !status().sleep_done);
    card(false, "ZZ");

    /* 18. Mode sommeil, en durée d'écoute (1 min simulée) : fondu, puis pause */
    card(true, "TT");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(status().sleep_s >= 58 && status().sleep_s <= 60);
    usleep(600000);
    int loud = shim_i2s_level();
    CHECK(loud > 1500); /* 8192 au volume 50 (gain 0,25) */
    bool faded = false;
    for (int i = 0; i < 1000 && !faded; i++) { /* ~2,4 s réelles avant la fin du décompte */
        player_status_t s = status();
        faded = s.state == PLAYER_PLAYING && s.sleep_s > 0 && s.sleep_s <= 5;
        usleep(5000);
    }
    CHECK(faded);
    CHECK(shim_i2s_level() < loud / 3);
    CHECK(wait_sleep_done(1500));
    uint32_t slept_at = status().elapsed_ms;
    CHECK(slept_at >= 59000 && slept_at <= 62000);
    /* les pauses ne comptent pas ; reposée, la carte repart au même endroit pour 1 min */
    card(false, "TT");
    card(true, "TT");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(status().elapsed_ms >= slept_at && status().sleep_s >= 58);
    usleep(300000);
    CHECK(shim_i2s_level() > 1500); /* volume normal après le fondu */
    /* réglage modifié pendant l'écoute : nouveau décompte (tic d'1 s) */
    g_tt_minutes = 2;
    usleep(1300000);
    CHECK(status().sleep_s >= 80); /* sans ce changement : moins de 30 s */
    /* file modifiée depuis une application : le mode sommeil de la carte ne s'applique plus */
    player_queue_add("Comptines/a.mp3", -1, NULL);
    usleep(1300000);
    CHECK(status().state == PLAYER_PLAYING && status().sleep_s == 0 && status().sleep_tracks == 0);
    card(false, "TT");
    player_stop();

    /* 19. Répétition (réglage de la carte) : la playlist recommence au lieu de s'arrêter */
    card(true, "RR");
    CHECK(wait_state(PLAYER_PLAYING, 1000));
    CHECK(status().repeat);
    usleep(1800000); /* deux morceaux de ~0,6 s : la playlist a déjà recommencé */
    CHECK(status().state == PLAYER_PLAYING && status().queue_len == 2);
    /* réglage modifié pendant l'écoute : appliqué au tic suivant */
    g_rr_repeat = 0;
    usleep(1300000);
    CHECK(!status().repeat);
    g_rr_repeat = 1;
    card(false, "RR");
    /* carte sans réglage propre : réglage général (s'arrêter), puis réglage général modifié */
    card(true, "BB");
    usleep(1300000);
    CHECK(!status().repeat);
    card(false, "BB");
    mock_set_repeat(true);
    card(true, "BB");
    usleep(1300000);
    CHECK(status().repeat);
    card(false, "BB");
    mock_set_repeat(false);
    player_stop();

    /* 20. Webradio : une liste .m3u du dossier donne l'adresse du flux, joué en direct */
    snprintf(p, sizeof(p), "%s/Flux", sd);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Flux/direct.mp3", sd);
    make_file(p, 2 << 20); /* ~12 s simulées, rejouées en boucle par le flux simulé */
    snprintf(p, sizeof(p), "%s/Radio", sd);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Radio/webradio.m3u", sd);
    FILE *m3u = fopen(p, "w");
    fputs("#EXTM3U\r\n#EXTINF:-1,Radio test\r\nhttp://test/Flux/direct.mp3\r\n", m3u);
    fclose(m3u);
    int opens = shim_stream_opens();
    card(true, "WR");
    CHECK(wait_state(PLAYER_PLAYING, 4000));
    player_status_t ws = status();
    CHECK(ws.stream && ws.queue_len == 1 && !ws.seekable && ws.duration_ms == 0);
    CHECK_STR(ws.file, "http://test/Flux/direct.mp3");
    CHECK(shim_stream_opens() == opens + 1);
    int pos_saves = g_pos_saves;
    usleep(2500000); /* 50 s simulées : bien plus que le fichier, le direct continue */
    ws = status();
    CHECK(ws.state == PLAYER_PLAYING && ws.elapsed_ms > 20000);
    CHECK_STR(ws.stream_title, "Titre simulé");
    CHECK(g_pos_saves == pos_saves); /* pas de position à enregistrer pour un direct */
    /* coupure du réseau : reconnexion, la lecture continue */
    shim_stream_cut();
    usleep(1800000);
    CHECK(shim_stream_opens() == opens + 2 && status().state == PLAYER_PLAYING);
    /* retirée : pause (connexion fermée) ; reposée : reprise en direct, nouvelle connexion */
    card(false, "WR");
    CHECK(wait_state(PLAYER_PAUSED, 1000));
    card(true, "WR");
    CHECK(wait_state(PLAYER_PLAYING, 4000));
    CHECK(shim_stream_opens() == opens + 3);
    CHECK(status().elapsed_ms < 15000);
    card(false, "WR");
    /* radio injoignable : message clair, lecture arrêtée */
    snprintf(p, sizeof(p), "%s/Radio absente", sd);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Radio absente/radio.pls", sd);
    m3u = fopen(p, "w");
    fputs("[playlist]\nFile1=http://absent.example/flux\n", m3u);
    fclose(m3u);
    card(true, "WK");
    CHECK(wait_state(PLAYER_STOPPED, 5000));
    CHECK(strstr(status().error, "injoignable") != NULL);
    card(false, "WK");
    player_clear_error();

    printf("contrôleur : %d vérifications, %d échec(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
