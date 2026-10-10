#include "test.h"
#include "util.h"

static void test_url_decode(void)
{
    char out[64];
    CHECK(url_decode("a%20b+c", out, sizeof(out)));
    CHECK_STR(out, "a b c");
    CHECK(url_decode("%C3%A9t%C3%A9", out, sizeof(out)));
    CHECK_STR(out, "été");
    CHECK(!url_decode("%zz", out, sizeof(out)));
    CHECK(!url_decode("%4", out, sizeof(out)));
    CHECK(!url_decode("a%00b", out, sizeof(out))); /* octet nul refusé */
    CHECK(!url_decode("abcdef", out, 4));          /* trop long */
    CHECK(url_decode("", out, sizeof(out)));
    CHECK_STR(out, "");
}

static void test_path_sanitize(void)
{
    char out[REL_PATH_MAX];
    CHECK(path_sanitize("/Comptines//Été 2024/", out, sizeof(out)));
    CHECK_STR(out, "Comptines/Été 2024");
    CHECK(path_sanitize("", out, sizeof(out)));
    CHECK_STR(out, "");
    CHECK(path_sanitize("/", out, sizeof(out)));
    CHECK_STR(out, "");
    /* Traversée de répertoire et caractères interdits */
    CHECK(!path_sanitize("../etc", out, sizeof(out)));
    CHECK(!path_sanitize("a/../../b", out, sizeof(out)));
    CHECK(!path_sanitize("a/./b", out, sizeof(out)));
    CHECK(!path_sanitize("a\\b", out, sizeof(out)));
    CHECK(!path_sanitize("a:b", out, sizeof(out)));
    CHECK(!path_sanitize("a\nb", out, sizeof(out)));
    CHECK(!path_sanitize("dossier./x", out, sizeof(out)));
    CHECK(!path_sanitize("dossier /x", out, sizeof(out)));
    char small[8];
    CHECK(!path_sanitize("abcdefghij", small, sizeof(small)));
    CHECK(path_sanitize("...fin", out, sizeof(out))); /* nom commençant par des points mais valide */
}

static void test_names(void)
{
    char abs[ABS_PATH_MAX];
    CHECK(path_to_abs("a/b.mp3", abs, sizeof(abs)));
    CHECK_STR(abs, "/sdcard/a/b.mp3");
    CHECK(path_to_abs("", abs, sizeof(abs)));
    CHECK_STR(abs, "/sdcard");
    CHECK_STR(path_basename("a/b/c.mp3"), "c.mp3");
    CHECK_STR(path_basename("c.mp3"), "c.mp3");
    char dir[64];
    path_dirname("a/b/c.mp3", dir, sizeof(dir));
    CHECK_STR(dir, "a/b");
    path_dirname("c.mp3", dir, sizeof(dir));
    CHECK_STR(dir, "");
    CHECK(audio_fmt_from_name("x.MP3") == AUDIO_FMT_MP3);
    CHECK(audio_fmt_from_name("x.flac") == AUDIO_FMT_FLAC);
    CHECK(audio_fmt_from_name("x.m4a") == AUDIO_FMT_M4A);
    CHECK(audio_fmt_from_name("x.opus") == AUDIO_FMT_OGG);
    CHECK(audio_fmt_from_name("x.txt") == AUDIO_FMT_NONE);
    CHECK(audio_fmt_from_name("mp3") == AUDIO_FMT_NONE);
    CHECK(is_audio_file("01 - Titre.mp3"));
    CHECK(!is_audio_file("._01 - Titre.mp3")); /* métadonnées macOS */
    CHECK(name_is_hidden(".Trashes"));
    CHECK(!name_is_valid(""));
    CHECK(!name_is_valid(".."));
    CHECK(name_is_valid("Chansons d'été"));

    /* Noms tirés d'un texte libre */
    char nm[32];
    name_from_text("  France Inter : le 7/9 ?  ", nm, sizeof(nm), "Radio");
    CHECK_STR(nm, "France Inter - le 7-9");
    name_from_text("\"Les \"histoires\" du soir...\"", nm, sizeof(nm), "Podcast");
    CHECK_STR(nm, "Les histoires du soir");
    name_from_text("..", nm, sizeof(nm), "Podcast");
    CHECK_STR(nm, "Podcast");
    name_from_text("", nm, sizeof(nm), "Sans nom");
    CHECK_STR(nm, "Sans nom");
    name_from_text("Été été été été été été été", nm, 12, "x"); /* coupé entre deux caractères */
    CHECK_STR(nm, "Été été");
    CHECK(name_is_valid(nm));
}

static void test_natural(void)
{
    CHECK(natural_casecmp("piste 2.mp3", "piste 10.mp3") < 0);
    CHECK(natural_casecmp("Piste 10.mp3", "piste 2.mp3") > 0);
    CHECK(natural_casecmp("02 - b", "2 - a") > 0); /* égalité numérique : on compare la suite */
    CHECK(natural_casecmp("abc", "ABD") < 0);
    CHECK(natural_casecmp("abc", "abc") == 0);
    CHECK(natural_casecmp("abc", "abcd") < 0);
    CHECK(natural_casecmp("CD1/10.mp3", "CD1/9.mp3") > 0);
    CHECK(natural_casecmp("CD1/10.mp3", "CD2/1.mp3") < 0);
    CHECK(natural_casecmp("a", "A") != 0); /* ordre stable mais distinct */
}

static void test_semver(void)
{
    CHECK(semver_cmp("1.0.0", "1.0.1") < 0);
    CHECK(semver_cmp("1.10.0", "1.9.9") > 0);
    CHECK(semver_cmp("v2.0.0", "1.99.99") > 0);
    CHECK(semver_cmp("1.2", "1.2.0") == 0);
    CHECK(semver_cmp("1.2.3", "1.2.3") == 0);
    CHECK(semver_cmp("1.2.3-dev", "1.2.3") == 0);
}

static void test_hostname(void)
{
    char out[40];
    CHECK(hostname_normalize("Enceinte-Salon", out, sizeof(out)));
    CHECK_STR(out, "enceinte-salon");
    CHECK(!hostname_normalize("-x", out, sizeof(out)));
    CHECK(!hostname_normalize("x-", out, sizeof(out)));
    CHECK(!hostname_normalize("é", out, sizeof(out)));
    CHECK(!hostname_normalize("a b", out, sizeof(out)));
    CHECK(!hostname_normalize("", out, sizeof(out)));
    CHECK(!hostname_normalize("abcdefghijabcdefghijabcdefghijabc", out, sizeof(out))); /* 33 caractères */
    uint8_t b[] = {0x04, 0xA1, 0xFF, 0x00};
    char hex[9];
    bytes_to_hex(b, sizeof(b), hex);
    CHECK_STR(hex, "04A1FF00");
}


static void test_ip4(void)
{
    uint32_t ip;
    char buf[16];
    CHECK(ip4_parse("192.168.1.50", &ip) && ip == 0xC0A80132u);
    ip4_format(ip, buf);
    CHECK_STR(buf, "192.168.1.50");
    CHECK(ip4_parse("0.0.0.0", &ip) && ip == 0);
    CHECK(ip4_parse("255.255.255.255", &ip) && ip == 0xFFFFFFFFu);
    CHECK(!ip4_parse("256.1.1.1", &ip));
    CHECK(!ip4_parse("1.2.3", &ip));
    CHECK(!ip4_parse("1.2.3.4.5", &ip));
    CHECK(!ip4_parse("01.2.3.4", &ip));
    CHECK(!ip4_parse("1.2.3.4 ", &ip));
    CHECK(!ip4_parse("1..3.4", &ip));
    CHECK(!ip4_parse("", &ip));
    CHECK(!ip4_parse("1234.1.1.1", &ip));

    const char *why = NULL;
    uint32_t a, m, g;
    ip4_parse("192.168.1.50", &a);
    ip4_parse("255.255.255.0", &m);
    ip4_parse("192.168.1.1", &g);
    CHECK(ip4_config_check(a, m, g, &why) && why == NULL);
    uint32_t bad;
    ip4_parse("255.0.255.0", &bad);
    CHECK(!ip4_config_check(a, bad, g, &why) && why != NULL); /* masque non contigu */
    ip4_parse("192.168.1.255", &bad);
    CHECK(!ip4_config_check(bad, m, g, &why));                 /* diffusion */
    ip4_parse("192.168.1.0", &bad);
    CHECK(!ip4_config_check(bad, m, g, &why));                 /* réseau */
    ip4_parse("192.168.2.1", &bad);
    CHECK(!ip4_config_check(a, m, bad, &why));                 /* passerelle hors du sous-réseau */
    CHECK(!ip4_config_check(a, m, a, &why));                   /* passerelle = adresse */
    ip4_parse("10.0.0.7", &a);
    ip4_parse("255.0.0.0", &m);
    ip4_parse("10.254.0.1", &g);
    CHECK(ip4_config_check(a, m, g, NULL));                    /* /8 */
    ip4_parse("255.255.255.254", &m);
    CHECK(!ip4_config_check(a, m, g, NULL));                   /* /31 refusé */
}

static void test_base64(void)
{
    char enc[64];
    uint8_t dec[64];
    base64_encode((const uint8_t *)"Man", 3, enc);
    CHECK_STR(enc, "TWFu");
    base64_encode((const uint8_t *)"Ma", 2, enc);
    CHECK_STR(enc, "TWE=");
    base64_encode((const uint8_t *)"M", 1, enc);
    CHECK_STR(enc, "TQ==");
    base64_encode((const uint8_t *)"", 0, enc);
    CHECK_STR(enc, "");
    CHECK(base64_decode("TWFu", dec, sizeof(dec)) == 3 && memcmp(dec, "Man", 3) == 0);
    CHECK(base64_decode("TWE=", dec, sizeof(dec)) == 2 && memcmp(dec, "Ma", 2) == 0);
    CHECK(base64_decode("TQ==", dec, sizeof(dec)) == 1 && dec[0] == 'M');
    CHECK(base64_decode("", dec, sizeof(dec)) == 0);
    CHECK(base64_decode("TQ=", dec, sizeof(dec)) == -1);
    CHECK(base64_decode("T=Q=", dec, sizeof(dec)) == -1);
    CHECK(base64_decode("TQ==TWFu", dec, sizeof(dec)) == -1);
    CHECK(base64_decode("TW*u", dec, sizeof(dec)) == -1);
    CHECK(base64_decode("TWFu", dec, 2) == -1);
    uint8_t all[52];
    for (int i = 0; i < 52; i++) {
        all[i] = (uint8_t)(i * 37 + 11);
    }
    char e2[80];
    base64_encode(all, sizeof(all), e2);
    CHECK(base64_decode(e2, dec, sizeof(dec)) == 52 && memcmp(dec, all, 52) == 0);
}

void test_util(void)
{
    test_url_decode();
    test_path_sanitize();
    test_names();
    test_natural();
    test_semver();
    test_hostname();
    test_ip4();
    test_base64();
}
