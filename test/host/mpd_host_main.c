/*
 * Serveur MPD + lecteur réels exécutés sur PC (I2S, décodeur et carte SD simulés).
 * Utilisé par mpd_integration.py. Variables : MPD_PASSWORD, SHIM_SPEEDUP, SHIM_LOG.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "mpd_server.h"
#include "player.h"

void mock_set_mpd_password(const char *pw);

static void on_player_event(player_event_t evt)
{
    if (evt == PLAYER_EVT_QUEUE_END) {
        printf("EVENT queue_end\n");
        fflush(stdout);
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    mock_set_mpd_password(getenv("MPD_PASSWORD"));
    if (player_init(30, 100, on_player_event) != ESP_OK || mpd_server_start() != ESP_OK) {
        fprintf(stderr, "initialisation impossible\n");
        return 1;
    }
    usleep(200000);
    printf("READY\n");
    fflush(stdout);
    for (;;) {
        pause();
    }
}
