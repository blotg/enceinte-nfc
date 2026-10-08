/*
 * Enceinte NFC — ESP32-S3
 *
 * Une carte NFC posée sur l'enceinte joue le dossier de la carte SD qui lui est
 * associé. Retirer la carte met en pause ; la reposer dans les 10 minutes reprend
 * la lecture (si aucune autre carte n'a été posée et que la playlist n'est pas finie).
 */
#include <stdlib.h>
#include <time.h>

#include "backup.h"
#include "buttons.h"
#include "cards.h"
#include "controller.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mpd_server.h"
#include "nfc.h"
#include "nvs_flash.h"
#include "ota.h"
#include "player.h"
#include "sdkconfig.h"
#include "settings.h"
#include "storage.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

#define FACTORY_RESET_HOLD_MS 10000
#define BOOT_CONFIRM_MS 30000

/* Bouton BOOT maintenu 10 s : réinitialisation usine (mot de passe oublié...). */
static void system_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_ENC_RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held_ms = 0;
    int uptime_ms = 0;
    bool confirmed = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        uptime_ms += 100;
        if (!confirmed && uptime_ms >= BOOT_CONFIRM_MS) {
            ota_confirm_boot(); /* 30 s sans plantage : le nouveau firmware est conservé */
            confirmed = true;
        }
        if (gpio_get_level(CONFIG_ENC_RESET_BUTTON_GPIO) == 0) {
            held_ms += 100;
            if (held_ms == 3000) {
                ESP_LOGW(TAG, "bouton maintenu : réinitialisation usine dans 7 s");
                player_beep(BEEP_OK);
            }
            if (held_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "réinitialisation usine");
                player_beep(BEEP_ERROR);
                vTaskDelay(pdMS_TO_TICKS(1000));
                backup_factory_reset(); /* sinon la carte SD rétablirait réglages et mot de passe */
                settings_factory_reset();
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Enceinte NFC version %s", app->version);

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); /* heure de Paris */
    tzset();

    /* NVS par défaut : calibration radio du Wi-Fi */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(settings_init());
    cards_init();
    storage_init();
    backup_boot(); /* réglages de la carte SD (carte clonée : comme dans l'enceinte d'origine) */

    settings_t cfg;
    settings_get(&cfg);
    err = player_init(cfg.volume, cfg.max_volume, controller_on_player_event);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lecteur audio : %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(controller_start());
    nfc_start(controller_on_nfc);
    buttons_start();

    wifi_mgr_start();
    web_server_start();
    mpd_server_start();
    ota_start();
    backup_start(); /* associations de la carte SD, cartes remises en place */

    xTaskCreate(system_task, "system", 6144, NULL, 2, NULL); /* réinitialisation usine : parcours de la carte SD */
    ESP_LOGI(TAG, "démarrage terminé");
}
