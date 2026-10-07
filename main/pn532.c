#include "pn532.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pn532_frame.h"
#include "sdkconfig.h"

static const char *TAG = "pn532";

#define PN532_UART ((uart_port_t)CONFIG_ENC_PN532_UART_PORT)
#define ACK_TIMEOUT_MS 30

#define CMD_GET_FIRMWARE_VERSION 0x02
#define CMD_SAM_CONFIGURATION 0x14
#define CMD_RF_CONFIGURATION 0x32
#define CMD_IN_LIST_PASSIVE_TARGET 0x4A
#define CMD_IN_RELEASE 0x52

static const uint8_t ACK_FRAME[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

esp_err_t pn532_init(void)
{
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(PN532_UART, 512, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(PN532_UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(PN532_UART, CONFIG_ENC_PN532_TX_GPIO, CONFIG_ENC_PN532_RX_GPIO, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    return err;
}

/*
 * Attend l'ACK puis la trame de réponse. Une seule boucle de lecture : l'ACK et la
 * réponse peuvent arriver dans le même paquet UART.
 */
static pn532_parse_result_t read_reply(pn532_parser_t *parser, uint32_t timeout_ms, bool *got_ack)
{
    int64_t start = esp_timer_get_time();
    int64_t ack_deadline = start + ACK_TIMEOUT_MS * 1000;
    int64_t deadline = ack_deadline + (int64_t)timeout_ms * 1000;
    uint8_t buf[32];
    *got_ack = false;
    for (;;) {
        int64_t remaining_us = (*got_ack ? deadline : ack_deadline) - esp_timer_get_time();
        if (remaining_us <= 0) {
            return PN532_PARSE_MORE; /* délai dépassé */
        }
        TickType_t ticks = pdMS_TO_TICKS(remaining_us / 1000);
        int n = uart_read_bytes(PN532_UART, buf, 1, ticks ? ticks : 1);
        if (n <= 0) {
            continue;
        }
        int more = uart_read_bytes(PN532_UART, buf + 1, sizeof(buf) - 1, 0);
        if (more > 0) {
            n += more;
        }
        for (int i = 0; i < n; i++) {
            pn532_parse_result_t r = pn532_parser_feed(parser, buf[i]);
            if (r == PN532_PARSE_MORE) {
                continue;
            }
            if (r == PN532_PARSE_ACK) {
                *got_ack = true;
                continue;
            }
            if (!*got_ack && r == PN532_PARSE_FRAME) {
                continue; /* trame périmée d'une commande précédente */
            }
            return r;
        }
    }
}

static esp_err_t command(const uint8_t *cmd, size_t cmd_len, uint8_t *resp, size_t resp_cap, size_t *resp_len,
                         uint32_t timeout_ms)
{
    uint8_t payload[PN532_MAX_DATA];
    uint8_t frame[PN532_MAX_DATA + 8];
    if (cmd_len + 1 > sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    payload[0] = PN532_TFI_HOST;
    memcpy(payload + 1, cmd, cmd_len);
    size_t flen = pn532_build_frame(payload, cmd_len + 1, frame, sizeof(frame));

    uart_flush_input(PN532_UART);
    if (uart_write_bytes(PN532_UART, frame, flen) != (int)flen) {
        return ESP_FAIL;
    }

    pn532_parser_t parser;
    pn532_parser_reset(&parser);
    bool got_ack;
    pn532_parse_result_t r = read_reply(&parser, timeout_ms, &got_ack);
    if (r != PN532_PARSE_FRAME) {
        if (got_ack) {
            /* Un ACK de l'hôte annule la commande en cours côté PN532. */
            uart_write_bytes(PN532_UART, ACK_FRAME, sizeof(ACK_FRAME));
        }
        ESP_LOGD(TAG, "commande 0x%02X : ack=%d résultat=%d", cmd[0], got_ack, r);
        return r == PN532_PARSE_MORE ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    }
    if (parser.len < 2 || parser.data[0] != PN532_TFI_PN532 || parser.data[1] != cmd[0] + 1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t n = parser.len - 2;
    if (n > resp_cap) {
        n = resp_cap;
    }
    if (resp && n) {
        memcpy(resp, parser.data + 2, n);
    }
    if (resp_len) {
        *resp_len = n;
    }
    return ESP_OK;
}

esp_err_t pn532_setup(uint32_t *firmware_version)
{
    /* Réveil HSU : préambule long, puis SAMConfiguration en premier. */
    static const uint8_t wake[] = {0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uart_write_bytes(PN532_UART, wake, sizeof(wake));
    uart_wait_tx_done(PN532_UART, pdMS_TO_TICKS(20));

    /* Mode normal, délai 1 s, broche IRQ utilisée */
    const uint8_t sam[] = {CMD_SAM_CONFIGURATION, 0x01, 0x14, 0x01};
    esp_err_t err = command(sam, sizeof(sam), NULL, 0, NULL, 100);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t fw[] = {CMD_GET_FIRMWARE_VERSION};
    uint8_t resp[4];
    size_t len = 0;
    err = command(fw, sizeof(fw), resp, sizeof(resp), &len, 100);
    if (err != ESP_OK) {
        return err;
    }
    if (len < 4 || resp[0] != 0x32) {
        ESP_LOGW(TAG, "circuit inattendu : 0x%02X", len ? resp[0] : 0);
    }
    if (firmware_version) {
        *firmware_version = (uint32_t)resp[0] << 24 | (uint32_t)resp[1] << 16 | (uint32_t)resp[2] << 8 | resp[3];
    }

    /*
     * Point clé de la fiabilité : par défaut le PN532 cherche une carte indéfiniment
     * (MxRtyPassiveActivation = 0xFF) et reste sourd aux commandes suivantes.
     * On borne à 3 tentatives pour obtenir une réponse "aucune carte" rapide.
     */
    const uint8_t retries[] = {CMD_RF_CONFIGURATION, 0x05, 0xFF, 0x01, 0x02};
    err = command(retries, sizeof(retries), NULL, 0, NULL, 100);
    if (err != ESP_OK) {
        return err;
    }
    return pn532_rf_field(true);
}

esp_err_t pn532_rf_field(bool on)
{
    const uint8_t cmd[] = {CMD_RF_CONFIGURATION, 0x01, on ? 0x01 : 0x00};
    return command(cmd, sizeof(cmd), NULL, 0, NULL, 50);
}

esp_err_t pn532_read_uid(uint8_t *uid, uint8_t *uid_len, bool *found)
{
    *found = false;
    const uint8_t cmd[] = {CMD_IN_LIST_PASSIVE_TARGET, 0x01, 0x00}; /* 1 cible, 106 kbit/s type A */
    uint8_t resp[64];
    size_t len = 0;
    esp_err_t err = command(cmd, sizeof(cmd), resp, sizeof(resp), &len, 200);
    if (err != ESP_OK) {
        return err;
    }
    /* NbTg, Tg, SENS_RES(2), SEL_RES, NFCIDLength, NFCID1... */
    if (len < 1 || resp[0] == 0) {
        return ESP_OK;
    }
    if (len < 6) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t n = resp[5];
    if (n == 0 || n > PN532_UID_MAX || len < 6u + n) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(uid, resp + 6, n);
    *uid_len = n;
    *found = true;

    /* Libère la cible pour que la prochaine interrogation reparte d'un état propre. */
    const uint8_t rel[] = {CMD_IN_RELEASE, 0x00};
    command(rel, sizeof(rel), NULL, 0, NULL, 50);
    return ESP_OK;
}
