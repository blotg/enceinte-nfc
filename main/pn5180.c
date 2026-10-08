#include "pn5180.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pn5180_proto.h"
#include "sdkconfig.h"

static const char *TAG = "pn5180";

#define PN5180_HOST SPI3_HOST
#define SPI_HZ (2 * 1000 * 1000) /* le PN5180 accepte 7 MHz ; 2 MHz tolère des fils plus longs */
#define BUSY_RISE_US 2000        /* BUSY monte à la fin d'une trame (pas toujours visible si très court) */
#define BUSY_DONE_US 100000      /* commande traitée : BUSY redescend (chargement d'une configuration RF : ~ms) */

static spi_device_handle_t s_dev;
static pn5180_proto_t s_last = PN5180_NONE;

static bool configured(void)
{
    return CONFIG_ENC_PN5180_SCK_GPIO >= 0 && CONFIG_ENC_PN5180_MOSI_GPIO >= 0 && CONFIG_ENC_PN5180_MISO_GPIO >= 0 &&
           CONFIG_ENC_PN5180_NSS_GPIO >= 0 && CONFIG_ENC_PN5180_BUSY_GPIO >= 0;
}

/* Attend que BUSY prenne le niveau demandé. Attente active brève, puis par pas d'1 ms. */
static bool wait_busy(int level, int64_t timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(CONFIG_ENC_PN5180_BUSY_GPIO) != level) {
        int64_t waited = esp_timer_get_time() - start;
        if (waited >= timeout_us) {
            return false;
        }
        if (waited < 500) {
            esp_rom_delay_us(10);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return true;
}

/* Une trame : NSS bas, échange, attente de BUSY haut, NSS haut, attente de BUSY bas. */
static esp_err_t frame(const uint8_t *tx, uint8_t *rx, size_t n)
{
    if (!wait_busy(0, BUSY_DONE_US)) {
        return ESP_ERR_TIMEOUT;
    }
    gpio_set_level(CONFIG_ENC_PN5180_NSS_GPIO, 0);
    spi_transaction_t t = {.length = n * 8, .tx_buffer = tx, .rx_buffer = rx};
    esp_err_t err = spi_device_polling_transmit(s_dev, &t);
    wait_busy(1, BUSY_RISE_US);
    gpio_set_level(CONFIG_ENC_PN5180_NSS_GPIO, 1);
    if (err == ESP_OK && !wait_busy(0, BUSY_DONE_US)) {
        err = ESP_ERR_TIMEOUT;
    }
    return err;
}

static esp_err_t xfer(void *ctx, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    uint8_t in[24];
    if (tx_len > sizeof(in) || rx_len > sizeof(in)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = frame(tx, in, tx_len);
    if (err == ESP_OK && rx_len) {
        uint8_t ff[24];
        memset(ff, 0xFF, rx_len);
        err = frame(ff, rx, rx_len);
    }
    return err;
}

static void delay_ms(void *ctx, uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

static const pn5180_bus_t s_bus = {.xfer = xfer, .delay_ms = delay_ms};

esp_err_t pn5180_init(void)
{
    if (!configured()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_ENC_PN5180_MOSI_GPIO,
        .miso_io_num = CONFIG_ENC_PN5180_MISO_GPIO,
        .sclk_io_num = CONFIG_ENC_PN5180_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(PN5180_HOST, &bus, SPI_DMA_DISABLED);
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = SPI_HZ,
        .spics_io_num = -1, /* NSS piloté à la main : il faut attendre BUSY avant de le relâcher */
        .queue_size = 1,
    };
    if (err == ESP_OK) {
        err = spi_bus_add_device(PN5180_HOST, &dev, &s_dev);
    }
    if (err != ESP_OK) {
        return err;
    }
    /* Sans PN5180 : MISO tirée au niveau haut (lecture 0xFF), BUSY tirée au niveau bas. */
    gpio_set_pull_mode(CONFIG_ENC_PN5180_MISO_GPIO, GPIO_PULLUP_ONLY);
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << CONFIG_ENC_PN5180_BUSY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&in);
    uint64_t out_mask = 1ULL << CONFIG_ENC_PN5180_NSS_GPIO;
    if (CONFIG_ENC_PN5180_RST_GPIO >= 0) {
        out_mask |= 1ULL << CONFIG_ENC_PN5180_RST_GPIO;
    }
    gpio_config_t out = {.pin_bit_mask = out_mask, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_set_level(CONFIG_ENC_PN5180_NSS_GPIO, 1);
    if (CONFIG_ENC_PN5180_RST_GPIO >= 0) {
        gpio_set_level(CONFIG_ENC_PN5180_RST_GPIO, 1);
    }
    return ESP_OK;
}

esp_err_t pn5180_setup(char *desc, size_t desc_len)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (CONFIG_ENC_PN5180_RST_GPIO >= 0) {
        gpio_set_level(CONFIG_ENC_PN5180_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level(CONFIG_ENC_PN5180_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    uint16_t fw = 0, product = 0;
    esp_err_t err = pn5180_read_versions(&s_bus, &fw, &product);
    if (err != ESP_OK) {
        return err;
    }
    err = pn5180_field_off(&s_bus);
    snprintf(desc, desc_len, "PN5180, firmware %u.%u", (unsigned)(fw >> 8), (unsigned)(fw & 0xFF));
    ESP_LOGI(TAG, "%s (produit %u.%u)", desc, (unsigned)(product >> 8), (unsigned)(product & 0xFF));
    s_last = PN5180_NONE;
    return err;
}

esp_err_t pn5180_read_uid(uint8_t *uid, uint8_t *uid_len, bool *found)
{
    pn5180_proto_t p = PN5180_NONE;
    esp_err_t err = pn5180_poll(&s_bus, s_last, uid, uid_len, &p);
    *found = err == ESP_OK && p != PN5180_NONE;
    if (*found) {
        s_last = p; /* la carte posée est cherchée d'abord avec son protocole */
    }
    return err;
}
