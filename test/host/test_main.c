#include <stdio.h>

#include "test.h"

int g_failures;
int g_checks;

int main(int argc, char **argv)
{
    test_util();
    test_pn532();
    test_session();
    test_dns();
    test_mpd_proto();
    if (argc > 1) {
        test_media(argv[1]);
    } else {
        fprintf(stderr, "(tests media ignorés : dossier de fichiers d'exemple non fourni)\n");
    }
    printf("%d vérifications, %d échec(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
