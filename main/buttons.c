#include "buttons.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/touch_sens.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "player.h"
#include "sdkconfig.h"
#include "settings.h"
#include "touch_keys.h"

static const char *TAG = "buttons";

#define POLL_MS 10
#define TOUCH_POLL_MS 20
#define SETTINGS_POLL_MS 500
#define DEBOUNCE_MS 30
#define REPEAT_DELAY_MS 500
#define REPEAT_PERIOD_MS 150
#define TICK_MIN_US (200 * 1000) /* au plus un bip de contrôle toutes les 200 ms */

static const int s_gpio[2] = {CONFIG_ENC_VOL_UP_GPIO, CONFIG_ENC_VOL_DOWN_GPIO};

/* ---- Boutons poussoirs ---- */

typedef struct {
    bool pressed;  /* état stable */
    int change_ms; /* durée pendant laquelle l'état lu diffère de l'état stable */
    int held_ms;
    int next_ms;   /* prochaine répétition */
} button_t;

static button_t s_buttons[2];

/* ---- Touches tactiles ---- */

static touch_sensor_handle_t s_sens;
static touch_channel_handle_t s_chan[2];
static touch_keys_t s_keys;
static SemaphoreHandle_t s_lock; /* s_keys lu par l'interface web */

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

static void buttons_setup(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < 2; i++) {
        if (s_gpio[i] >= 0) {
            mask |= 1ULL << s_gpio[i];
        }
    }
    memset(s_buttons, 0, sizeof(s_buttons));
    if (!mask) {
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    ESP_LOGI(TAG, "boutons poussoirs : + GPIO%d, - GPIO%d", s_gpio[0], s_gpio[1]);
}

static void buttons_poll(void)
{
    for (int i = 0; i < 2; i++) {
        button_t *b = &s_buttons[i];
        if (s_gpio[i] < 0) {
            continue;
        }
        bool raw = gpio_get_level(s_gpio[i]) == 0;
        if (raw != b->pressed) {
            b->change_ms += POLL_MS;
            if (b->change_ms >= DEBOUNCE_MS) {
                b->pressed = raw;
                b->change_ms = 0;
                b->held_ms = 0;
                if (raw) {
                    step(i == 0 ? 1 : -1);
                    b->next_ms = REPEAT_DELAY_MS;
                }
            }
        } else {
            b->change_ms = 0;
            if (b->pressed) {
                b->held_ms += POLL_MS;
                if (b->held_ms >= b->next_ms) {
                    step(i == 0 ? 1 : -1);
                    b->next_ms += REPEAT_PERIOD_MS;
                }
            }
        }
    }
}

static void touch_teardown(void)
{
    if (!s_sens) {
        return;
    }
    touch_sensor_stop_continuous_scanning(s_sens);
    touch_sensor_disable(s_sens);
    for (int i = 0; i < 2; i++) {
        if (s_chan[i]) {
            touch_sensor_del_channel(s_chan[i]);
            s_chan[i] = NULL;
        }
    }
    touch_sensor_del_controller(s_sens);
    s_sens = NULL;
}

static bool touch_setup(const settings_t *cfg)
{
    for (int i = 0; i < 2; i++) {
        if (s_gpio[i] < (int)SOC_TOUCH_MIN_CHAN_ID || s_gpio[i] > (int)SOC_TOUCH_MAX_CHAN_ID) {
            ESP_LOGE(TAG, "GPIO%d n'est pas un canal tactile (GPIO %d à %d)", s_gpio[i], SOC_TOUCH_MIN_CHAN_ID,
                     SOC_TOUCH_MAX_CHAN_ID);
            return false;
        }
        gpio_reset_pin(s_gpio[i]); /* plus de résistance de tirage (mode boutons) */
    }
    touch_sensor_sample_config_t sample[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2),
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample);
    esp_err_t err = touch_sensor_new_controller(&sens_cfg, &s_sens);
    /* Le seuil matériel n'est pas utilisé : la détection est faite par touch_keys. */
    touch_channel_config_t chan_cfg = {
        .active_thresh = {2000},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    for (int i = 0; i < 2 && err == ESP_OK; i++) {
        err = touch_sensor_new_channel(s_sens, s_gpio[i], &chan_cfg, &s_chan[i]);
    }
    touch_sensor_filter_config_t filter = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    if (err == ESP_OK) {
        err = touch_sensor_config_filter(s_sens, &filter);
    }
    if (err == ESP_OK) {
        err = touch_sensor_enable(s_sens);
    }
    if (err == ESP_OK) {
        err = touch_sensor_start_continuous_scanning(s_sens);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capteur tactile : %s", esp_err_to_name(err));
        touch_teardown();
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    touch_keys_init(&s_keys, cfg->touch_threshold, cfg->touch_hold_ms);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "touches tactiles : + GPIO%d, - GPIO%d (seuil %u ‰, maintien %u ms)", s_gpio[0], s_gpio[1],
             cfg->touch_threshold, cfg->touch_hold_ms);
    return true;
}

static void touch_poll(void)
{
    uint32_t v[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        uint32_t data[TOUCH_SAMPLE_CFG_NUM] = {0};
        if (touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_SMOOTH, data) != ESP_OK) {
            return;
        }
        v[i] = data[0];
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int dir = touch_keys_update(&s_keys, v, TOUCH_POLL_MS);
    xSemaphoreGive(s_lock);
    if (dir) {
        step(dir);
    }
}

static void buttons_task(void *arg)
{
    bool touch = false, started = false;
    uint16_t threshold = 0, hold = 0;
    int since_settings = SETTINGS_POLL_MS;
    for (;;) {
        if (since_settings >= SETTINGS_POLL_MS) {
            since_settings = 0;
            settings_t cfg;
            settings_get(&cfg);
            if (!started || cfg.vol_touch != touch) {
                touch_teardown();
                touch = cfg.vol_touch;
                if (touch) {
                    touch_setup(&cfg);
                } else {
                    buttons_setup();
                }
                threshold = cfg.touch_threshold;
                hold = cfg.touch_hold_ms;
                started = true;
            } else if (touch && (cfg.touch_threshold != threshold || cfg.touch_hold_ms != hold)) {
                threshold = cfg.touch_threshold;
                hold = cfg.touch_hold_ms;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                touch_keys_config(&s_keys, threshold, hold);
                xSemaphoreGive(s_lock);
            }
        }
        int period = touch ? TOUCH_POLL_MS : POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(period));
        since_settings += period;
        if (touch) {
            if (s_sens) {
                touch_poll();
            }
        } else {
            buttons_poll();
        }
    }
}

esp_err_t buttons_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_gpio[0] < 0 && s_gpio[1] < 0) {
        return ESP_OK; /* aucune commande configurée */
    }
    return xTaskCreate(buttons_task, "buttons", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void buttons_get_diag(buttons_diag_t *out)
{
    memset(out, 0, sizeof(*out));
    settings_t cfg;
    settings_get(&cfg);
    out->touch = cfg.vol_touch;
    out->threshold = cfg.touch_threshold;
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->touch_ok = s_sens != NULL;
    for (int i = 0; i < 2 && out->touch_ok; i++) {
        out->key[i].value = s_keys.k[i].value;
        out->key[i].baseline = (uint32_t)s_keys.k[i].baseline;
        out->key[i].delta_permille = touch_keys_delta_permille(&s_keys, i);
        out->key[i].touched = s_keys.k[i].touched;
    }
    xSemaphoreGive(s_lock);
}
