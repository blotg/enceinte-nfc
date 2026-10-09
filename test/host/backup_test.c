/*
 * Copie des réglages et des associations sur la carte SD (vrais modules backup, cards et
 * config_json ; NVS simulée dans un fichier par enceinte, carte SD = dossier MUSIC_ROOT).
 * Chaque démarrage d'enceinte est un processus fils : deux enceintes partagent une carte SD
 * clonée.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backup.h"
#include "cJSON.h"
#include "cards.h"
#include "esp_mac.h"
#include "settings.h"
#include "storage.h"
#include "test.h"

int g_failures;
int g_checks;

/* ---------- Substituts : réglages, carte SD, réseau ---------- */

static settings_t g_cfg = {.hostname = "enceinte",
                           .ota_interval_h = 24,
                           .max_volume = 100,
                           .resume_timeout_s = 600,
                           .touch_threshold = 20,
                           .touch_hold_ms = 800};
static uint8_t g_admin[SETTINGS_PW_HASH_LEN], g_mpd[SETTINGS_PW_HASH_LEN];
static void (*g_observer)(void);
static bool g_mounted = true;
static uint32_t g_mounts = 1;
static uint8_t g_mac[6] = {0xA0, 0xB1, 0xC2, 0, 0, 1};

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    memcpy(mac, g_mac, 6);
    return ESP_OK;
}

void settings_get(settings_t *out)
{
    *out = g_cfg;
}

esp_err_t settings_set_all(const settings_t *cfg)
{
    settings_t c = *cfg;
    c.volume = g_cfg.volume;
    c.admin_set = g_cfg.admin_set;
    c.mpd_pass_set = g_cfg.mpd_pass_set;
    g_cfg = c;
    if (g_observer) {
        g_observer();
    }
    return ESP_OK;
}

void settings_set_observer(void (*cb)(void))
{
    g_observer = cb;
}

bool settings_get_password_hash(bool admin, uint8_t out[SETTINGS_PW_HASH_LEN])
{
    if (admin ? !g_cfg.admin_set : !g_cfg.mpd_pass_set) {
        return false;
    }
    memcpy(out, admin ? g_admin : g_mpd, SETTINGS_PW_HASH_LEN);
    return true;
}

esp_err_t settings_set_password_hash(bool admin, const uint8_t *hash)
{
    if (admin) {
        memcpy(g_admin, hash, SETTINGS_PW_HASH_LEN);
        g_cfg.admin_set = true;
    } else {
        g_cfg.mpd_pass_set = hash != NULL;
        if (hash) {
            memcpy(g_mpd, hash, SETTINGS_PW_HASH_LEN);
        }
    }
    if (g_observer) {
        g_observer();
    }
    return ESP_OK;
}

static void set_max_volume(uint8_t v)
{
    g_cfg.max_volume = v;
    if (g_observer) {
        g_observer();
    }
}

bool storage_is_mounted(void)
{
    return g_mounted;
}

uint32_t storage_mount_count(void)
{
    return g_mounts;
}

bool storage_is_dir(const char *rel)
{
    char abs[ABS_PATH_MAX];
    struct stat st;
    return g_mounted && path_to_abs(rel, abs, sizeof(abs)) && stat(abs, &st) == 0 && S_ISDIR(st.st_mode);
}

esp_err_t storage_list_dir(const char *rel, dir_entry_t **entries, int *count)
{
    char abs[ABS_PATH_MAX];
    *entries = NULL;
    *count = 0;
    DIR *d = path_to_abs(rel, abs, sizeof(abs)) ? opendir(abs) : NULL;
    if (!d) {
        return ESP_ERR_NOT_FOUND;
    }
    int n = 0;
    dir_entry_t *list = calloc(64, sizeof(dir_entry_t));
    struct dirent *de;
    while ((de = readdir(d)) && n < 64) {
        if (name_is_hidden(de->d_name)) {
            continue;
        }
        char p[ABS_PATH_MAX + 260];
        snprintf(p, sizeof(p), "%s/%s", abs, de->d_name);
        struct stat st;
        if (stat(p, &st) == 0) {
            list[n].name = strdup(de->d_name);
            list[n].is_dir = S_ISDIR(st.st_mode);
            n++;
        }
    }
    closedir(d);
    *entries = list;
    *count = n;
    return ESP_OK;
}

void storage_free_dir(dir_entry_t *entries, int count)
{
    for (int i = 0; entries && i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}

void player_set_max_volume(uint8_t v)
{
}

void wifi_mgr_set_hostname(const char *h)
{
}

void wifi_mgr_reconnect_later(uint32_t ms)
{
}

static bool g_ip_tested;
void wifi_mgr_ip_test(const ip_config_t *ip)
{
    g_ip_tested = true;
}

void web_server_https_apply(void)
{
}

/* ---------- Outils ---------- */

static void sd_path(const char *rel, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", MUSIC_ROOT, rel);
}

static void mk(const char *rel)
{
    char p[512];
    sd_path(rel, p, sizeof(p));
    mkdir(p, 0755);
}

static bool exists(const char *rel)
{
    char p[512];
    struct stat st;
    sd_path(rel, p, sizeof(p));
    return stat(p, &st) == 0;
}

static void write_text(const char *rel, const char *txt)
{
    char p[512];
    sd_path(rel, p, sizeof(p));
    FILE *f = fopen(p, "w");
    fputs(txt, f);
    fclose(f);
}

static cJSON *read_doc(const char *rel)
{
    char p[512], buf[16384];
    sd_path(rel, p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) {
        return NULL;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return cJSON_Parse(buf);
}

/* Identifiants des cartes d'un fichier .cartes.json, séparés par des virgules. */
static void file_uids(const char *dir, char *out, size_t len)
{
    char rel[300];
    snprintf(rel, sizeof(rel), "%s/.cartes.json", dir);
    out[0] = '\0';
    cJSON *doc = read_doc(rel);
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(doc, "cards"))
    {
        const cJSON *u = cJSON_GetObjectItem(it, "uid");
        snprintf(out + strlen(out), len - strlen(out), "%s%s", out[0] ? "," : "", u ? u->valuestring : "?");
    }
    cJSON_Delete(doc);
}

static const char *root_str(const char *key)
{
    static char v[64];
    cJSON *doc = read_doc(".enceinte.json");
    const cJSON *it = cJSON_GetObjectItem(doc, key);
    if (!it) {
        it = cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "settings"), key);
    }
    snprintf(v, sizeof(v), "%s", cJSON_IsString(it) ? it->valuestring : (cJSON_IsNumber(it) ? "#" : ""));
    if (cJSON_IsNumber(it)) {
        snprintf(v, sizeof(v), "%d", it->valueint);
    }
    cJSON_Delete(doc);
    return v;
}

static int card_count(void)
{
    card_entry_t *l;
    int n = cards_list(&l);
    free(l);
    return n;
}

static bool card_in(const char *uid, const char *folder)
{
    card_entry_t e;
    return cards_get(uid, &e) && strcmp(e.folder, folder) == 0;
}

static card_entry_t entry(const char *uid, const char *folder)
{
    card_entry_t e = {.resume_s = CARD_DEFAULT, .resume_other = CARD_DEFAULT, .shuffle = CARD_DEFAULT,
                      .normalize = CARD_DEFAULT, .compress = CARD_DEFAULT};
    snprintf(e.uid, sizeof(e.uid), "%s", uid);
    snprintf(e.folder, sizeof(e.folder), "%s", folder);
    return e;
}

/* Démarrage d'une enceinte : NVS du fichier nvs, adresse MAC mac_last. */
static void boot(const char *nvs, uint8_t mac_last)
{
    char p[512];
    snprintf(p, sizeof(p), "%s.%s", MUSIC_ROOT, nvs);
    setenv("NVS_FILE", p, 1);
    g_mac[5] = mac_last;
    cards_init();
    backup_boot();
    backup_start();
    usleep(400000); /* parcours de la carte SD par la tâche */
}

static void remount(void)
{
    g_mounted = true;
    g_mounts++;
    usleep(2600000); /* la tâche vérifie toutes les 2 s */
}

/* ---------- Scénarios (un processus par démarrage) ---------- */

/* 1. Enceinte n°1 (firmware précédent : associations en NVS seulement), nouvelle carte SD. */
static void first_boot(void)
{
    setenv("NVS_FILE", MUSIC_ROOT ".nvs1", 1);
    cards_init();
    card_entry_t a = entry("04AAAAAA", "Comptines"), b = entry("04BBBBBB", "Histoires/Contes");
    b.normalize = 3;
    b.sleep_minutes = 45;
    CHECK(cards_set(&a) == ESP_OK && cards_set(&b) == ESP_OK);
    strcpy(g_cfg.hostname, "salon");
    g_cfg.admin_set = true;
    memset(g_admin, 7, sizeof(g_admin));
    g_admin[0] = 0x10;
    g_admin[1] = 0x27;
    g_admin[2] = g_admin[3] = 0;
    g_mac[5] = 1;
    backup_boot();
    backup_start();
    usleep(400000);
    char uids[128];
    CHECK(exists(".enceinte.json"));
    CHECK_STR(root_str("device"), "A0B1C2000001");
    CHECK_STR(root_str("hostname"), "salon");
    file_uids("Comptines", uids, sizeof(uids));
    CHECK_STR(uids, "04AAAAAA");
    file_uids("Histoires/Contes", uids, sizeof(uids));
    CHECK_STR(uids, "04BBBBBB");
    /* Modifications : le fichier du dossier suit */
    card_entry_t c = entry("04CCCCCC", "Comptines");
    CHECK(cards_set(&c) == ESP_OK);
    file_uids("Comptines", uids, sizeof(uids));
    CHECK_STR(uids, "04AAAAAA,04CCCCCC");
    CHECK(cards_remove("04AAAAAA") == ESP_OK);
    file_uids("Comptines", uids, sizeof(uids));
    CHECK_STR(uids, "04CCCCCC");
    set_max_volume(70);
    CHECK_STR(root_str("max_volume"), "70");
}

/* 2. Enceinte n°2 sortie d'usine avec la carte SD clonée : elle se comporte comme la n°1. */
static void clone_boot(void)
{
    boot("nvs2", 2);
    CHECK_STR(g_cfg.hostname, "salon");
    CHECK(g_cfg.max_volume == 70);
    CHECK(g_cfg.admin_set && g_admin[4] == 7);
    CHECK(card_count() == 2);
    CHECK(card_in("04CCCCCC", "Comptines"));
    card_entry_t b;
    CHECK(cards_get("04BBBBBB", &b) && strcmp(b.folder, "Histoires/Contes") == 0 && b.normalize == 3);
    CHECK(b.sleep_minutes == 45 && b.sleep_tracks == 0); /* mode sommeil recopié sur la carte SD */
    CHECK_STR(root_str("device"), "A0B1C2000002"); /* la carte SD est désormais celle de la n°2 */
}

/* 3. Enceinte n°2 : la carte SD modifiée sur un ordinateur fait foi. */
static void sd_edits(void)
{
    boot("nvs2", 2);
    CHECK(card_count() == 2);
    card_entry_t bb;
    CHECK(cards_get("04BBBBBB", &bb) && bb.sleep_minutes == 45); /* relu depuis la NVS */
    g_mounted = false;
    char a[512], b[512];
    sd_path("Histoires", a, sizeof(a));
    sd_path("Contes du soir", b, sizeof(b));
    CHECK(rename(a, b) == 0); /* dossier déplacé : son fichier suit */
    char p[512];
    sd_path("Comptines/.cartes.json", p, sizeof(p));
    unlink(p);
    mk("Autre");
    write_text("Autre/.cartes.json",
               "{\"format\": \"enceinte-cartes\", \"version\": 1, \"cards\": [{\"uid\": \"04dddddd\", \"shuffle\": true}]}");
    remount();
    CHECK(card_count() == 2);
    CHECK(card_in("04BBBBBB", "Contes du soir/Contes"));
    CHECK(card_in("04DDDDDD", "Autre"));
    card_entry_t d;
    CHECK(cards_get("04DDDDDD", &d) && d.shuffle == 1);

    /* Fichier abîmé : les associations de l'enceinte sont gardées et le fichier réécrit */
    write_text("Autre/.cartes.json", "{\"format\": \"encei");
    remount();
    CHECK(card_in("04DDDDDD", "Autre"));
    char uids[128];
    file_uids("Autre", uids, sizeof(uids));
    CHECK_STR(uids, "04DDDDDD");

    /* Modifications pendant l'absence de la carte SD : recopiées à son retour */
    g_mounted = false;
    CHECK(cards_remove("04DDDDDD") == ESP_OK);
    set_max_volume(55);
    remount();
    CHECK(!exists("Autre/.cartes.json"));
    CHECK(card_count() == 1);
    CHECK_STR(root_str("max_volume"), "55");

    /* Export puis import : associations remplacées, dossier absent ignoré */
    cJSON *doc = backup_export(false);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(doc, "cards")) == 1);
    CHECK(strstr(cJSON_PrintUnformatted(doc), "password") == NULL);
    cJSON_Delete(doc);
    doc = cJSON_Parse("{\"format\": \"enceinte-reglages\", \"version\": 1, \"settings\": {\"normalize\": 2},"
                      " \"cards\": [{\"uid\": \"04EEEEEE\", \"folder\": \"Autre\", \"compress\": 1},"
                      " {\"uid\": \"04FFFFFF\", \"folder\": \"Introuvable\"}]}");
    char msg[200];
    bool ip_test = true;
    CHECK(backup_import(doc, msg, sizeof(msg), &ip_test) == ESP_OK);
    cJSON_Delete(doc);
    CHECK(!ip_test && strstr(msg, "1 carte associée") && strstr(msg, "1 carte ignorée"));
    CHECK(g_cfg.normalize == 2);
    CHECK(card_count() == 1 && card_in("04EEEEEE", "Autre"));
    file_uids("Autre", uids, sizeof(uids));
    CHECK_STR(uids, "04EEEEEE");
    CHECK(!exists("Contes du soir/Contes/.cartes.json"));
    /* Import d'une adresse IP fixe : seulement essayée */
    doc = cJSON_Parse("{\"format\": \"enceinte-reglages\", \"settings\": {\"ip\": {\"mode\": \"static\", "
                      "\"address\": \"192.168.1.50\", \"netmask\": \"255.255.255.0\", \"gateway\": \"192.168.1.1\"}}}");
    CHECK(backup_import(doc, msg, sizeof(msg), &ip_test) == ESP_OK && ip_test && g_ip_tested);
    CHECK(!g_cfg.ip.static_ip);
    cJSON_Delete(doc);
    /* Fichier refusé : rien n'est modifié */
    doc = cJSON_Parse("{\"format\": \"enceinte-reglages\", \"settings\": {\"normalize\": 7}, \"cards\": []}");
    CHECK(backup_import(doc, msg, sizeof(msg), &ip_test) != ESP_OK && card_count() == 1);
    cJSON_Delete(doc);

    /* Réinitialisation usine : plus de fichiers de réglages sur la carte SD */
    backup_factory_reset();
    CHECK(!exists(".enceinte.json") && !exists("Autre/.cartes.json"));
}

/* 4. Enceinte n°1 de nouveau : carte SD sans réglages (réinitialisée) : recopie depuis l'enceinte. */
static void migrate_again(void)
{
    boot("nvs1", 1);
    CHECK(exists(".enceinte.json"));
    CHECK_STR(root_str("device"), "A0B1C2000001");
    char uids[128];
    file_uids("Comptines", uids, sizeof(uids));
    CHECK_STR(uids, "04CCCCCC");
    CHECK(card_count() == 2); /* l'association vers un dossier disparu reste dans l'enceinte */
}

static int run(void (*fn)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        printf("  %d vérifications, %d échec(s)\n", g_checks, g_failures);
        fflush(stdout);
        _exit(g_failures ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

int main(void)
{
    char p[512];
    snprintf(p, sizeof(p), "rm -rf '%s' '%s'.nvs*", MUSIC_ROOT, MUSIC_ROOT);
    if (system(p) != 0) {
        return 1;
    }
    mkdir(MUSIC_ROOT, 0755);
    mk("Comptines");
    mk("Histoires");
    mk("Histoires/Contes");
    int failed = 0;
    printf("1. premier démarrage (copie vers la carte SD)\n");
    failed += run(first_boot);
    printf("2. carte SD clonée dans une autre enceinte\n");
    failed += run(clone_boot);
    printf("3. carte SD modifiée, absente, export et import\n");
    failed += run(sd_edits);
    printf("4. carte SD réinitialisée, enceinte d'origine\n");
    failed += run(migrate_again);
    printf("sauvegarde sur carte SD : %s\n", failed ? "ÉCHEC" : "OK");
    return failed ? 1 : 0;
}
