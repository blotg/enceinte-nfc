#pragma once
/* Mini-cadre de tests pour les modules sans dépendance matérielle. */
#include <stdio.h>
#include <string.h>

extern int g_failures;
extern int g_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                        \
    } while (0)

#define CHECK_STR(a, b)                                                                     \
    do {                                                                                    \
        g_checks++;                                                                         \
        const char *_a = (a), *_b = (b);                                                    \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                            \
            g_failures++;                                                                   \
            fprintf(stderr, "ÉCHEC %s:%d : \"%s\" != \"%s\"\n", __FILE__, __LINE__,          \
                    _a ? _a : "(null)", _b ? _b : "(null)");                                \
        }                                                                                   \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                  \
    do {                                                                                       \
        g_checks++;                                                                            \
        long _a = (long)(a), _b = (long)(b);                                                   \
        if (_a - _b > (tol) || _b - _a > (tol)) {                                              \
            g_failures++;                                                                      \
            fprintf(stderr, "ÉCHEC %s:%d : %ld != %ld (±%d)\n", __FILE__, __LINE__, _a, _b, (tol)); \
        }                                                                                      \
    } while (0)

void test_util(void);
void test_pn532(void);
void test_session(void);
void test_dns(void);
void test_mpd_proto(void);
void test_media(const char *fixtures);
void test_dsp(void);
void test_config_json(void);
void test_touch(void);
void test_pn5180(void);
