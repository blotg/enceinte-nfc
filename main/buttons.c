#include "buttons.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "player.h"
#include "sdkconfig.h"

static const char *TAG = "buttons";

#define POLL_MS 10
#define DEBOUNCE_MS 30
#define REPEAT_DELAY_MS 500
#define REPEAT_PERIOD_MS 150
#define TICK_MIN_US (200 * 1000) /* au plus un bip de contrôle toutes les 200 ms */

typedef struct {
    int gpio;
    int dir;       /* +1 ou -1 */
    bool pressed;  /* état stable */
    int change_ms; /* durée pendant laquelle l'état lu diffère de l'état stable */
    int held_ms;
    int next_ms;   /* prochaine répétition */
} button_t;

static button_t s_buttons[] = {
    {.gpio = CONFIG_ENC_VOL_UP_GPIO, .dir = 1},
    {.gpio = CONFIG_ENC_VOL_DOWN_GPIO, .dir = -1},
};

/* Cran suivant ou précédent, aligné sur un multiple du pas, borné par le volume maximum. */
static void step(int dir)
{
    const int st = CONFIG_ENC_VOL_STEP;
    int v = player_get_volume(), max = player_get_max_volume();
    int nv = dir > 0 ? (v / st + 1) * st : ((v + st - 1) / st - 1) * st;
    nv = nv > max ? max : (nv < 0 ? 0 : nv);
    if (nv != v) {
        player_set_volume(nv);
        ESP_LOGI(TAG, "volume %d (max %d)", nv, max);
    }
    static int64_t last_tick;
    player_status_t ps;
    player_get_status(&ps);
    int64_t now = esp_timer_get_time();
    if (ps.state != PLAYER_PLAYING && now - last_tick >= TICK_MIN_US) {
        last_tick = now;
        player_beep(BEEP_TICK);
    }
}

static void buttons_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++) {
            button_t *b = &s_buttons[i];
            if (b->gpio < 0) {
                continue;
            }
            bool raw = gpio_get_level(b->gpio) == 0;
            if (raw != b->pressed) {
                b->change_ms += POLL_MS;
                if (b->change_ms >= DEBOUNCE_MS) {
                    b->pressed = raw;
                    b->change_ms = 0;
                    b->held_ms = 0;
                    if (raw) {
                        step(b->dir);
                        b->next_ms = REPEAT_DELAY_MS;
                    }
                }
            } else {
                b->change_ms = 0;
                if (b->pressed) {
                    b->held_ms += POLL_MS;
                    if (b->held_ms >= b->next_ms) {
                        step(b->dir);
                        b->next_ms += REPEAT_PERIOD_MS;
                    }
                }
            }
        }
    }
}

esp_err_t buttons_start(void)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++) {
        if (s_buttons[i].gpio >= 0) {
            mask |= 1ULL << s_buttons[i].gpio;
        }
    }
    if (!mask) {
        return ESP_OK; /* aucun bouton configuré */
    }
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "broches des boutons : %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "boutons de volume : + GPIO%d, - GPIO%d", CONFIG_ENC_VOL_UP_GPIO, CONFIG_ENC_VOL_DOWN_GPIO);
    return xTaskCreate(buttons_task, "buttons", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
