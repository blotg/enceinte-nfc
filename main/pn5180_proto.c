#include "pn5180_proto.h"

#include <string.h>

/* Commandes de l'interface hôte */
#define CMD_WRITE_REGISTER 0x00
#define CMD_WRITE_REGISTER_OR_MASK 0x01
#define CMD_WRITE_REGISTER_AND_MASK 0x02
#define CMD_READ_REGISTER 0x04
#define CMD_READ_EEPROM 0x07
#define CMD_SEND_DATA 0x09
#define CMD_READ_DATA 0x0A
#define CMD_LOAD_RF_CONFIG 0x11
#define CMD_RF_ON 0x16
#define CMD_RF_OFF 0x17

/* Registres */
#define REG_SYSTEM_CONFIG 0x00
#define REG_IRQ_STATUS 0x02
#define REG_IRQ_CLEAR 0x03
#define REG_CRC_RX_CONFIG 0x12
#define REG_RX_STATUS 0x13
#define REG_CRC_TX_CONFIG 0x19
#define REG_RF_STATUS 0x1D

#define EEPROM_PRODUCT_VERSION 0x10
#define EEPROM_FIRMWARE_VERSION 0x12

#define IRQ_RX 0x01u
#define SYSCFG_COMMAND_MASK 0x07u
#define SYSCFG_TRANSCEIVE 0x03u
#define SYSCFG_MFC_CRYPTO 0x40u
#define TRANSCEIVE_STATE(rf) (((rf) >> 24) & 0x07u)
#define STATE_WAIT_TRANSMIT 1

/* Configurations RF : ISO 14443A 106 kbit/s, ISO 15693 26 kbit/s */
#define RF_14443A_TX 0x00
#define RF_14443A_RX 0x80
#define RF_15693_TX 0x0D
#define RF_15693_RX 0x8D

#define POWER_UP_MS 5         /* la carte s'alimente après l'allumage du champ */
#define TIMEOUT_14443A_MS 3   /* une carte répond en moins d'une milliseconde */
#define TIMEOUT_15693_MS 15   /* inventaire à 26 kbit/s : environ 6 ms */

static esp_err_t cmd(const pn5180_bus_t *b, const uint8_t *tx, size_t n)
{
    return b->xfer(b->ctx, tx, n, NULL, 0);
}

static esp_err_t reg_op(const pn5180_bus_t *b, uint8_t op, uint8_t reg, uint32_t v)
{
    const uint8_t c[6] = {op, reg, (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return cmd(b, c, sizeof(c));
}

static esp_err_t read_reg(const pn5180_bus_t *b, uint8_t reg, uint32_t *v)
{
    const uint8_t c[2] = {CMD_READ_REGISTER, reg};
    uint8_t r[4];
    esp_err_t err = b->xfer(b->ctx, c, sizeof(c), r, sizeof(r));
    *v = (uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24;
    return err;
}

static esp_err_t simple(const pn5180_bus_t *b, uint8_t op, uint8_t a, uint8_t c2)
{
    const uint8_t c[3] = {op, a, c2};
    return cmd(b, c, op == CMD_RF_ON || op == CMD_RF_OFF ? 2 : 3);
}

static esp_err_t set_crc(const pn5180_bus_t *b, bool on)
{
    esp_err_t err;
    if (on) {
        err = reg_op(b, CMD_WRITE_REGISTER_OR_MASK, REG_CRC_RX_CONFIG, 0x01);
        if (err == ESP_OK) {
            err = reg_op(b, CMD_WRITE_REGISTER_OR_MASK, REG_CRC_TX_CONFIG, 0x01);
        }
    } else {
        err = reg_op(b, CMD_WRITE_REGISTER_AND_MASK, REG_CRC_RX_CONFIG, ~0x01u);
        if (err == ESP_OK) {
            err = reg_op(b, CMD_WRITE_REGISTER_AND_MASK, REG_CRC_TX_CONFIG, ~0x01u);
        }
    }
    return err;
}

/*
 * Émission puis réception. last_bits : bits valides du dernier octet (0 = 8). *rx_len = 0
 * si aucune réponse dans le délai.
 */
static esp_err_t transceive(const pn5180_bus_t *b, const uint8_t *data, size_t len, uint8_t last_bits, uint8_t *rx,
                            size_t rx_cap, size_t *rx_len, uint32_t timeout_ms)
{
    *rx_len = 0;
    esp_err_t err = reg_op(b, CMD_WRITE_REGISTER_AND_MASK, REG_SYSTEM_CONFIG, ~SYSCFG_COMMAND_MASK); /* repos */
    if (err == ESP_OK) {
        err = reg_op(b, CMD_WRITE_REGISTER_OR_MASK, REG_SYSTEM_CONFIG, SYSCFG_TRANSCEIVE);
    }
    if (err == ESP_OK) {
        err = reg_op(b, CMD_WRITE_REGISTER, REG_IRQ_CLEAR, 0x000FFFFF);
    }
    uint32_t v = 0;
    for (int i = 0; err == ESP_OK; i++) {
        err = read_reg(b, REG_RF_STATUS, &v);
        if (err != ESP_OK || TRANSCEIVE_STATE(v) == STATE_WAIT_TRANSMIT) {
            break;
        }
        if (i >= 3) {
            return ESP_ERR_INVALID_STATE; /* le PN5180 n'est pas prêt à émettre */
        }
        b->delay_ms(b->ctx, 1);
    }
    if (err != ESP_OK) {
        return err;
    }
    uint8_t frame[2 + 16];
    if (len > sizeof(frame) - 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    frame[0] = CMD_SEND_DATA;
    frame[1] = last_bits;
    memcpy(frame + 2, data, len);
    err = cmd(b, frame, len + 2);
    for (uint32_t t = 0; err == ESP_OK; t++) {
        err = read_reg(b, REG_IRQ_STATUS, &v);
        if (err != ESP_OK || (v & IRQ_RX)) {
            break;
        }
        if (t >= timeout_ms) {
            return ESP_OK; /* aucune réponse */
        }
        b->delay_ms(b->ctx, 1);
    }
    if (err == ESP_OK) {
        err = read_reg(b, REG_RX_STATUS, &v);
    }
    size_t n = v & 0x1FF;
    if (err != ESP_OK || n == 0) {
        return err;
    }
    if (n > rx_cap) {
        n = rx_cap;
    }
    const uint8_t rd[2] = {CMD_READ_DATA, 0x00};
    err = b->xfer(b->ctx, rd, sizeof(rd), rx, n);
    if (err == ESP_OK) {
        *rx_len = n;
    }
    return err;
}

static esp_err_t field_on(const pn5180_bus_t *b, uint8_t tx_cfg, uint8_t rx_cfg)
{
    esp_err_t err = simple(b, CMD_LOAD_RF_CONFIG, tx_cfg, rx_cfg);
    if (err == ESP_OK) {
        err = simple(b, CMD_RF_ON, 0, 0);
    }
    if (err == ESP_OK) {
        b->delay_ms(b->ctx, POWER_UP_MS);
    }
    return err;
}

esp_err_t pn5180_field_off(const pn5180_bus_t *bus)
{
    return simple(bus, CMD_RF_OFF, 0, 0);
}

/* ISO 14443-3 : réveil (WUPA), puis anticollision et sélection niveau par niveau. */
static esp_err_t poll_14443a(const pn5180_bus_t *b, uint8_t *uid, uint8_t *uid_len, bool *found)
{
    *found = false;
    esp_err_t err = field_on(b, RF_14443A_TX, RF_14443A_RX);
    if (err == ESP_OK) {
        err = reg_op(b, CMD_WRITE_REGISTER_AND_MASK, REG_SYSTEM_CONFIG, ~SYSCFG_MFC_CRYPTO);
    }
    if (err == ESP_OK) {
        err = set_crc(b, false);
    }
    uint8_t rx[8];
    size_t n = 0;
    const uint8_t wupa = 0x52; /* réveille aussi une carte laissée à l'arrêt (HALT) */
    if (err == ESP_OK) {
        err = transceive(b, &wupa, 1, 7, rx, sizeof(rx), &n, TIMEOUT_14443A_MS);
    }
    if (err != ESP_OK || n != 2) {
        return err; /* pas de carte (ATQA de 2 octets attendu) */
    }
    static const uint8_t SEL[3] = {0x93, 0x95, 0x97};
    uint8_t len = 0;
    for (int level = 0; level < 3; level++) {
        uint8_t ac[2] = {SEL[level], 0x20};
        err = set_crc(b, false);
        if (err == ESP_OK) {
            err = transceive(b, ac, sizeof(ac), 0, rx, sizeof(rx), &n, TIMEOUT_14443A_MS);
        }
        if (err != ESP_OK || n != 5 || (rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) {
            return err; /* pas de réponse, collision ou octet de contrôle faux : on réessaiera */
        }
        uint8_t sel[7] = {SEL[level], 0x70, rx[0], rx[1], rx[2], rx[3], rx[4]};
        err = set_crc(b, true);
        uint8_t sak[4];
        size_t sn = 0;
        if (err == ESP_OK) {
            err = transceive(b, sel, sizeof(sel), 0, sak, sizeof(sak), &sn, TIMEOUT_14443A_MS);
        }
        if (err != ESP_OK || sn < 1) {
            return err;
        }
        if (sak[0] & 0x04) { /* identifiant incomplet : octet de cascade 0x88 puis 3 octets */
            if (rx[0] != 0x88 || level == 2) {
                return ESP_OK;
            }
            memcpy(uid + len, sel + 3, 3);
            len += 3;
            continue;
        }
        memcpy(uid + len, sel + 2, 4);
        *uid_len = len + 4;
        *found = true;
        return ESP_OK;
    }
    return ESP_OK;
}

/* ISO 15693 : inventaire à un seul créneau (une étiquette attendue). */
static esp_err_t poll_15693(const pn5180_bus_t *b, uint8_t *uid, uint8_t *uid_len, bool *found)
{
    *found = false;
    esp_err_t err = field_on(b, RF_15693_TX, RF_15693_RX);
    if (err == ESP_OK) {
        err = set_crc(b, true);
    }
    /* drapeaux : débit élevé + inventaire ; commande INVENTORY ; longueur de masque 0 */
    const uint8_t inv[3] = {0x26, 0x01, 0x00};
    uint8_t rx[16];
    size_t n = 0;
    if (err == ESP_OK) {
        err = transceive(b, inv, sizeof(inv), 0, rx, sizeof(rx), &n, TIMEOUT_15693_MS);
    }
    if (err != ESP_OK || n < 10 || (rx[0] & 0x01)) {
        return err; /* pas d'étiquette, ou réponse d'erreur */
    }
    for (int i = 0; i < 8; i++) {
        uid[i] = rx[9 - i]; /* reçu octet de poids faible en premier */
    }
    *uid_len = 8;
    *found = true;
    return ESP_OK;
}

esp_err_t pn5180_poll(const pn5180_bus_t *bus, pn5180_proto_t prefer, uint8_t *uid, uint8_t *uid_len,
                      pn5180_proto_t *found)
{
    *found = PN5180_NONE;
    pn5180_proto_t order[2] = {PN5180_ISO14443A, PN5180_ISO15693};
    if (prefer == PN5180_ISO15693) {
        order[0] = PN5180_ISO15693;
        order[1] = PN5180_ISO14443A;
    }
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 2 && err == ESP_OK && *found == PN5180_NONE; i++) {
        bool ok = false;
        err = order[i] == PN5180_ISO14443A ? poll_14443a(bus, uid, uid_len, &ok) : poll_15693(bus, uid, uid_len, &ok);
        if (ok) {
            *found = order[i];
        }
        esp_err_t off = pn5180_field_off(bus); /* la carte repart de zéro à l'interrogation suivante */
        if (err == ESP_OK) {
            err = off;
        }
    }
    return err;
}

esp_err_t pn5180_read_versions(const pn5180_bus_t *bus, uint16_t *firmware, uint16_t *product)
{
    uint8_t c[3] = {CMD_READ_EEPROM, EEPROM_FIRMWARE_VERSION, 2};
    uint8_t fw[2] = {0xFF, 0xFF}, pr[2] = {0xFF, 0xFF};
    esp_err_t err = bus->xfer(bus->ctx, c, sizeof(c), fw, sizeof(fw));
    if (err == ESP_OK) {
        c[1] = EEPROM_PRODUCT_VERSION;
        err = bus->xfer(bus->ctx, c, sizeof(c), pr, sizeof(pr));
    }
    if (err != ESP_OK) {
        return err;
    }
    /* Broche MISO en l'air (tirée au niveau haut) ou à la masse : pas de PN5180. */
    if (fw[1] == 0 || fw[1] > 0x0F || (fw[0] == 0xFF && fw[1] == 0xFF)) {
        return ESP_ERR_NOT_FOUND;
    }
    *firmware = (uint16_t)(fw[1] << 8 | fw[0]);
    *product = (uint16_t)(pr[1] << 8 | pr[0]);
    return ESP_OK;
}
