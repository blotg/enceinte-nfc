#include <stdlib.h>

#include "mpd_proto.h"
#include "test.h"

typedef struct {
    const char *tags[MPD_TAG_COUNT];
    const char *file;
} fake_song_t;

static const char *get(void *ctx, int tag)
{
    fake_song_t *s = ctx;
    if (tag == MPD_TAG_FILE) {
        return s->file;
    }
    return tag < MPD_TAG_COUNT ? s->tags[tag] : NULL;
}

static void test_tokenize(void)
{
    char line[] = "add \"Comptines/Il \\\"était\\\" une fois.mp3\"  2";
    char *argv[MPD_MAX_ARGS];
    int argc = mpd_tokenize(line, argv, MPD_MAX_ARGS);
    CHECK(argc == 3);
    CHECK_STR(argv[0], "add");
    CHECK_STR(argv[1], "Comptines/Il \"était\" une fois.mp3");
    CHECK_STR(argv[2], "2");

    char l2[] = "find \"(artist == \\\"A\\\\B\\\")\"";
    argc = mpd_tokenize(l2, argv, MPD_MAX_ARGS);
    CHECK(argc == 2);
    CHECK_STR(argv[1], "(artist == \"A\\B\")");

    char l3[] = "status \"non fermé";
    CHECK(mpd_tokenize(l3, argv, MPD_MAX_ARGS) == -1);
    char l4[] = "   ";
    CHECK(mpd_tokenize(l4, argv, MPD_MAX_ARGS) == 0);
    char l5[] = "\"a\"b";
    CHECK(mpd_tokenize(l5, argv, MPD_MAX_ARGS) == -1);
    char l6[] = "a b c d";
    CHECK(mpd_tokenize(l6, argv, 3) == -1);
}

static void test_ranges(void)
{
    int s, e;
    CHECK(mpd_parse_range("3", &s, &e) && s == 3 && e == 4);
    CHECK(mpd_parse_range("2:5", &s, &e) && s == 2 && e == 5);
    CHECK(mpd_parse_range("4:", &s, &e) && s == 4 && e == -1);
    CHECK(!mpd_parse_range("5:2", &s, &e));
    CHECK(!mpd_parse_range("-1", &s, &e));
    CHECK(!mpd_parse_range(":3", &s, &e));
    CHECK(!mpd_parse_range("x", &s, &e));
    int v;
    CHECK(mpd_parse_int("-12", &v) && v == -12);
    CHECK(!mpd_parse_int("12a", &v));
    CHECK(!mpd_parse_int("", &v));
    double d;
    CHECK(mpd_parse_float("12.5", &d) && d == 12.5);
    CHECK(!mpd_parse_float("1x", &d));
}

static void test_filters(void)
{
    fake_song_t song = {.file = "Enfants/Comptines/01 - Une souris verte.mp3"};
    song.tags[MPD_TAG_ARTIST] = "Anne Sylvestre";
    song.tags[MPD_TAG_ALBUM] = "Fabulettes";
    song.tags[MPD_TAG_TITLE] = "Une souris verte";

    char err[96];
    mpd_filter_t *f = mpd_filter_parse_expr("(artist == \"Anne Sylvestre\")", false, err, sizeof(err));
    CHECK(f && mpd_filter_match(f, get, &song));
    mpd_filter_free(f);

    f = mpd_filter_parse_expr("((artist == 'Anne Sylvestre') AND (album contains 'Fabu'))", false, err, sizeof(err));
    CHECK(f && mpd_filter_match(f, get, &song));
    mpd_filter_free(f);

    f = mpd_filter_parse_expr("((artist == 'Anne Sylvestre') AND (album == 'Autre'))", false, err, sizeof(err));
    CHECK(f && !mpd_filter_match(f, get, &song));
    mpd_filter_free(f);

    f = mpd_filter_parse_expr("(!(title contains 'souris'))", false, err, sizeof(err));
    CHECK(f && !mpd_filter_match(f, get, &song));
    mpd_filter_free(f);

    f = mpd_filter_parse_expr("(base 'Enfants/Comptines')", false, err, sizeof(err));
    CHECK(f && mpd_filter_match(f, get, &song));
    mpd_filter_free(f);
    f = mpd_filter_parse_expr("(base 'Enfants/Comp')", false, err, sizeof(err));
    CHECK(f && !mpd_filter_match(f, get, &song)); /* préfixe de nom, pas de dossier */
    mpd_filter_free(f);

    /* "search" : insensible à la casse */
    f = mpd_filter_parse_expr("(any contains 'SOURIS')", true, err, sizeof(err));
    CHECK(f && mpd_filter_match(f, get, &song));
    mpd_filter_free(f);
    f = mpd_filter_parse_expr("(any contains 'SOURIS')", false, err, sizeof(err));
    CHECK(f && !mpd_filter_match(f, get, &song));
    mpd_filter_free(f);

    /* Casse des lettres accentuées */
    song.tags[MPD_TAG_GENRE] = "Chanson française";
    f = mpd_filter_parse_expr("(genre contains 'FRANÇAISE')", true, err, sizeof(err));
    CHECK(f && mpd_filter_match(f, get, &song));
    mpd_filter_free(f);
    char folded[64];
    mpd_utf8_fold("ÉTÉ À ÇA ŒUVRE Ÿ Ĺ ß 日本", folded, sizeof(folded));
    CHECK_STR(folded, "été à ça œuvre ÿ ĺ ß 日本");
    song.tags[MPD_TAG_GENRE] = NULL;

    /* Tag absent : == "" correspond */
    f = mpd_filter_parse_expr("(genre == '')", false, err, sizeof(err));
    CHECK(f && mpd_filter_match(f, get, &song));
    mpd_filter_free(f);

    /* Erreurs de syntaxe */
    CHECK(!mpd_filter_parse_expr("(artist =~ 'x')", false, err, sizeof(err)));
    CHECK(!mpd_filter_parse_expr("(inconnu == 'x')", false, err, sizeof(err)));
    CHECK(!mpd_filter_parse_expr("(artist == 'x'", false, err, sizeof(err)));
    CHECK(!mpd_filter_parse_expr("(artist == 'x') reste", false, err, sizeof(err)));
    CHECK(!mpd_filter_parse_expr("((((((((((artist == 'x'))))))))))", false, err, sizeof(err)));

    /* Syntaxe historique */
    char *pairs[] = {"artist", "anne", "title", "VERTE"};
    f = mpd_filter_parse_pairs(4, pairs, true, err, sizeof(err));
    CHECK(f && mpd_filter_match(f, get, &song));
    mpd_filter_free(f);
    f = mpd_filter_parse_pairs(4, pairs, false, err, sizeof(err));
    CHECK(f && !mpd_filter_match(f, get, &song)); /* find : exact et sensible à la casse */
    mpd_filter_free(f);
    char *odd[] = {"artist"};
    CHECK(!mpd_filter_parse_pairs(1, odd, false, err, sizeof(err)));

    /* Arguments avec "window" / "group" */
    char *args[] = {"artist", "Anne Sylvestre", "window", "0:10"};
    int used = 0;
    f = mpd_filter_parse_args(4, args, false, &used, err, sizeof(err));
    CHECK(f && used == 2 && mpd_filter_match(f, get, &song));
    mpd_filter_free(f);
    char *args2[] = {"(title starts_with 'Une')", "sort", "Title"};
    f = mpd_filter_parse_args(3, args2, false, &used, err, sizeof(err));
    CHECK(f && used == 1 && mpd_filter_match(f, get, &song));
    mpd_filter_free(f);
    char *args3[] = {"group", "albumartist"};
    f = mpd_filter_parse_args(2, args3, false, &used, err, sizeof(err));
    CHECK(!f && used == 0); /* pas de filtre : tout correspond */

    CHECK(mpd_tag_from_name("albumartist") == MPD_TAG_ALBUMARTIST);
    CHECK(mpd_tag_from_name("FILE") == MPD_TAG_FILE);
    CHECK(mpd_tag_from_name("xyz") == -1);
}

void test_mpd_proto(void)
{
    test_tokenize();
    test_ranges();
    test_filters();
}
