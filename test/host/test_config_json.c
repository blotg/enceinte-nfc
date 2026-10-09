/* Réglages et cartes <-> JSON (fichiers de la carte SD, export/import). */
#include "test.h"

#ifdef HAVE_CJSON
#include <stdlib.h>

#include "config_json.h"

static config_t sample(void)
{
    config_t c = {.mpd_hash = -1};
    settings_t *s = &c.s;
    strcpy(s->hostname, "salon");
    strcpy(s->wifi_ssid, "Maison");
    strcpy(s->wifi_pass, "motdepasse");
    strcpy(s->ota_url, "https://github.com/blotg/enceinte-nfc");
    s->ota_interval_h = 24;
    s->resume_timeout_s = 600;
    s->resume_after_other = true;
    s->shuffle = false;
    s->https_enabled = true;
    s->normalize = 2;
    s->compress = 1;
    s->max_volume = 70;
    s->vol_touch = true;
    s->touch_threshold = 25;
    s->touch_hold_ms = 1000;
    s->ip = (ip_config_t){.static_ip = true, .address = 0xC0A80132, .netmask = 0xFFFFFF00, .gateway = 0xC0A80101};
    c.admin_hash = true;
    for (int i = 0; i < SETTINGS_PW_HASH_LEN; i++) {
        c.admin[i] = (uint8_t)(i + 1);
    }
    c.admin[0] = 0x10; /* 10 000 itérations, petit-boutiste */
    c.admin[1] = 0x27;
    c.admin[2] = c.admin[3] = 0;
    c.mpd_hash = 0;
    return c;
}

static bool parse(const char *json, config_t *c, char *err)
{
    cJSON *o = cJSON_Parse(json);
    bool ok = config_from_json(o, c, err, 128);
    cJSON_Delete(o);
    return ok;
}

static void test_roundtrip(void)
{
    config_t a = sample();
    cJSON *o = config_to_json(&a, true);
    char *txt = cJSON_Print(o);
    cJSON_Delete(o);
    config_t b = {.mpd_hash = -1};
    b.s.ota_interval_h = 1;
    b.s.max_volume = 100;
    strcpy(b.s.hostname, "enceinte");
    char err[128] = "";
    CHECK(parse(txt, &b, err));
    free(txt);
    CHECK_STR(b.s.hostname, "salon");
    CHECK_STR(b.s.wifi_ssid, "Maison");
    CHECK_STR(b.s.wifi_pass, "motdepasse");
    CHECK(b.wifi);
    CHECK(b.s.ip.static_ip && b.s.ip.address == 0xC0A80132 && b.s.ip.netmask == 0xFFFFFF00 &&
          b.s.ip.gateway == 0xC0A80101 && b.s.ip.dns == 0);
    CHECK(b.s.normalize == 2 && b.s.compress == 1 && b.s.max_volume == 70);
    CHECK(b.s.resume_timeout_s == 600 && b.s.resume_after_other && !b.s.shuffle && b.s.https_enabled);
    CHECK(b.s.ota_interval_h == 24);
    CHECK(b.s.vol_touch && b.s.touch_threshold == 25 && b.s.touch_hold_ms == 1000);
    CHECK(b.admin_hash && memcmp(b.admin, a.admin, SETTINGS_PW_HASH_LEN) == 0);
    CHECK(b.mpd_hash == 0);

    /* Sans secrets : ni mot de passe Wi-Fi ni empreintes ; le réseau n'est pas modifié */
    o = config_to_json(&a, false);
    txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    CHECK(strstr(txt, "motdepasse") == NULL && strstr(txt, "password_hash") == NULL);
    config_t c = {.mpd_hash = -1};
    strcpy(c.s.hostname, "x");
    strcpy(c.s.wifi_ssid, "Ancien");
    strcpy(c.s.wifi_pass, "ancienmdp");
    c.s.ota_interval_h = 1;
    c.s.max_volume = 100;
    CHECK(parse(txt, &c, err));
    free(txt);
    CHECK_STR(c.s.wifi_ssid, "Ancien");
    CHECK(!c.wifi && !c.admin_hash && c.mpd_hash == -1);
}

static void test_partial_and_invalid(void)
{
    config_t c = sample(), before;
    char err[128];
    /* Clés absentes : valeurs conservées */
    CHECK(parse("{\"normalize\": 3}", &c, err));
    CHECK(c.s.normalize == 3 && c.s.compress == 1 && c.s.max_volume == 70);
    CHECK_STR(c.s.hostname, "salon");
    CHECK(parse("{\"controls\": {\"type\": \"buttons\", \"touch_threshold_pct\": 4.5}}", &c, err));
    CHECK(!c.s.vol_touch && c.s.touch_threshold == 45 && c.s.touch_hold_ms == 1000);
    CHECK(parse("{\"ip\": {\"mode\": \"dhcp\"}, \"hostname\": \"Cuisine\"}", &c, err));
    CHECK(!c.s.ip.static_ip);
    CHECK_STR(c.s.hostname, "cuisine");
    /* Une valeur invalide : rien n'est modifié */
    before = c;
    const char *bad[] = {
        "{\"normalize\": 4}",
        "{\"compress\": -1}",
        "{\"max_volume\": 0}",
        "{\"max_volume\": 50.5}",
        "{\"max_volume\": \"80\"}",
        "{\"resume_s\": 99999999}",
        "{\"hostname\": \"mon enceinte\"}",
        "{\"shuffle\": 1}",
        "{\"ota_url\": \"ftp://x\"}",
        "{\"ota_interval_h\": 0}",
        "{\"wifi\": {\"ssid\": \"Maison\", \"password\": \"court\"}}",
        "{\"ip\": {\"mode\": \"static\", \"address\": \"192.168.1.255\", \"netmask\": \"255.255.255.0\", \"gateway\": \"192.168.1.1\"}}",
        "{\"ip\": {\"mode\": \"static\", \"address\": \"192.168.1.20\"}}",
        "{\"ip\": {\"mode\": \"auto\"}}",
        "{\"admin_password_hash\": \"abc\"}",
        "{\"normalize\": 2, \"compress\": 9}",
        "{\"controls\": {\"type\": \"capacitif\"}}",
        "{\"controls\": {\"touch_threshold_pct\": 0.1}}",
        "{\"controls\": {\"touch_threshold_pct\": 31}}",
        "{\"controls\": {\"touch_hold_ms\": 5000}}",
        "[]",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err[0] = '\0';
        bool ok = parse(bad[i], &c, err);
        CHECK(!ok && err[0]);
        if (ok) {
            fprintf(stderr, "accepté à tort : %s\n", bad[i]);
        }
    }
    CHECK(memcmp(&c, &before, sizeof(c)) == 0);
    /* MPD : null = pas de mot de passe */
    CHECK(parse("{\"mpd_password_hash\": null}", &c, err) && c.mpd_hash == 0);
}

static void test_document(void)
{
    char err[128];
    cJSON *d = config_document(CONFIG_FORMAT, "AABBCCDDEEFF");
    CHECK(config_document_check(d, CONFIG_FORMAT, err, sizeof(err)));
    CHECK(!config_document_check(d, CARDS_FORMAT, err, sizeof(err)));
    cJSON_ReplaceItemInObject(d, "version", cJSON_CreateNumber(CONFIG_VERSION + 1));
    CHECK(!config_document_check(d, CONFIG_FORMAT, err, sizeof(err)));
    cJSON_Delete(d);
}

static void test_cards(void)
{
    card_entry_t e = {.resume_s = 0,
                      .resume_other = 1,
                      .shuffle = CARD_DEFAULT,
                      .normalize = 3,
                      .compress = CARD_DEFAULT,
                      .sleep_tracks = 4};
    strcpy(e.uid, "04AB53A96F2681");
    strcpy(e.folder, "Histoires/Été");
    cJSON *o = card_to_json(&e, true);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    card_entry_t f;
    char err[128];
    o = cJSON_Parse(txt);
    CHECK(card_from_json(o, &f, true, err, sizeof(err)));
    cJSON_Delete(o);
    free(txt);
    CHECK_STR(f.uid, e.uid);
    CHECK_STR(f.folder, e.folder);
    CHECK(f.resume_s == 0 && f.resume_other == 1 && f.shuffle == CARD_DEFAULT && f.normalize == 3 &&
          f.compress == CARD_DEFAULT && f.sleep_tracks == 4 && f.sleep_minutes == 0);
    CHECK(cards_entry_valid(&f));
    /* Fichier d'un dossier : sans chemin, clés absentes = réglage général, minuscules acceptées */
    o = cJSON_Parse("{\"uid\": \"04ab53a9\"}");
    CHECK(card_from_json(o, &f, false, err, sizeof(err)));
    cJSON_Delete(o);
    CHECK_STR(f.uid, "04AB53A9");
    CHECK(f.resume_s == CARD_DEFAULT && f.shuffle == CARD_DEFAULT && f.compress == CARD_DEFAULT);
    CHECK(f.sleep_tracks == 0 && f.sleep_minutes == 0); /* fichiers des versions précédentes */
    o = cJSON_Parse("{\"uid\": \"04ab53a9\", \"sleep_minutes\": 720, \"sleep_tracks\": null}");
    CHECK(card_from_json(o, &f, false, err, sizeof(err)));
    cJSON_Delete(o);
    CHECK(f.sleep_minutes == 720 && f.sleep_tracks == 0);
    const char *bad[] = {
        "{\"uid\": \"04AB\"}",
        "{\"uid\": \"04AB53A96\"}",
        "{\"uid\": \"04AB53ZZ\"}",
        "{\"uid\": \"04AB53A9\", \"folder\": \"A\", \"compress\": 4}",
        "{\"uid\": \"04AB53A9\", \"folder\": \"A\", \"shuffle\": 1}",
        "{\"uid\": \"04AB53A9\", \"folder\": \"A\", \"resume_s\": -5}",
        "{\"uid\": \"04AB53A9\", \"folder\": \"A\", \"sleep_tracks\": 1000}",
        "{\"uid\": \"04AB53A9\", \"folder\": \"A\", \"sleep_minutes\": 721}",
        "{\"uid\": \"04AB53A9\", \"folder\": \"A\", \"sleep_minutes\": 2.5}",
        "{\"uid\": \"04AB53A9\", \"folder\": \"A\", \"sleep_tracks\": -1}",
        "{\"uid\": \"04AB53A9\", \"folder\": \"A\", \"sleep_tracks\": \"3\"}",
        "{\"uid\": \"04AB53A9\", \"folder\": \"../etc\"}",
        "{\"uid\": \"04AB53A9\"}", /* dossier exigé à l'export */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        o = cJSON_Parse(bad[i]);
        CHECK(!card_from_json(o, &f, true, err, sizeof(err)));
        cJSON_Delete(o);
    }
    /* Les cas refusés le sont pour leur réglage, pas pour le dossier */
    o = cJSON_Parse("{\"uid\": \"04AB53A9\", \"folder\": \"A\", \"sleep_tracks\": 999}");
    CHECK(card_from_json(o, &f, true, err, sizeof(err)));
    cJSON_Delete(o);
}

void test_config_json(void)
{
    test_roundtrip();
    test_partial_and_invalid();
    test_document();
    test_cards();
}
#else
void test_config_json(void)
{
    fprintf(stderr, "(tests JSON ignorés : bibliothèque cJSON absente, paquet libcjson-dev)\n");
}
#endif
