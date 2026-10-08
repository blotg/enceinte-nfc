/*
 * PN5180 simulé au niveau des commandes de son interface hôte : registres, champ RF et une
 * carte ISO 14443A (identifiant de 4, 7 ou 10 octets) ou une étiquette ISO 15693.
 * Vérifie aussi la discipline du protocole : CRC coupé pour le réveil et l'anticollision,
 * activé pour la sélection et l'inventaire, émission seulement en mode « transceive ».
 */
#include <string.h>

#include "pn5180_proto.h"
#include "test.h"

typedef struct {
    bool absent;
    uint32_t reg[0x30];
    bool rf;
    uint8_t cfg_tx;
    uint8_t rx[32];
    size_t rx_len;
    int kind;           /* 0 aucune, 1 ISO 14443A, 2 ISO 15693 */
    uint8_t uid[10];
    int uid_len;
    bool corrupt_bcc;
    bool awake;         /* carte réveillée depuis l'allumage du champ */
    int violations;     /* écarts au protocole */
    int sends;
} fake_t;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void reply(fake_t *f, const uint8_t *d, size_t n)
{
    memcpy(f->rx, d, n);
    f->rx_len = n;
    f->reg[0x13] = (uint32_t)n; /* RX_STATUS */
    f->reg[0x02] |= 0x01;       /* RX_IRQ */
}

static bool crc_on(const fake_t *f)
{
    return (f->reg[0x12] & 1) && (f->reg[0x19] & 1);
}

static bool crc_off(const fake_t *f)
{
    return !(f->reg[0x12] & 1) && !(f->reg[0x19] & 1);
}

/* Niveau de cascade : 4 octets de l'identifiant (ou 0x88 + 3 octets) et SAK. */
static void cascade(const fake_t *f, int level, uint8_t out[4], uint8_t *sak)
{
    int levels = f->uid_len == 4 ? 1 : (f->uid_len == 7 ? 2 : 3);
    if (level < levels - 1) {
        out[0] = 0x88;
        memcpy(out + 1, f->uid + 3 * level, 3);
        *sak = 0x04;
    } else {
        memcpy(out, f->uid + 3 * level, 4);
        *sak = 0x08;
    }
}

static void send_data(fake_t *f, uint8_t bits, const uint8_t *d, size_t n)
{
    f->sends++;
    if ((f->reg[0x00] & 7) != 3) {
        f->violations++; /* émission hors du mode transceive */
        return;
    }
    if (!f->rf) {
        f->violations++;
        return;
    }
    if (f->kind == 1 && f->cfg_tx == 0x00) {
        if (n == 1 && bits == 7 && (d[0] == 0x52 || d[0] == 0x26)) {
            if (!crc_off(f)) {
                f->violations++;
            }
            f->awake = true;
            const uint8_t atqa[2] = {f->uid_len == 4 ? 0x04 : 0x44, 0x00};
            reply(f, atqa, 2);
            return;
        }
        int level = d[0] == 0x93 ? 0 : (d[0] == 0x95 ? 1 : (d[0] == 0x97 ? 2 : -1));
        if (!f->awake || level < 0) {
            return;
        }
        uint8_t cl[4], sak;
        cascade(f, level, cl, &sak);
        if (n == 2 && d[1] == 0x20) {
            if (!crc_off(f)) {
                f->violations++;
            }
            uint8_t r[5] = {cl[0], cl[1], cl[2], cl[3], (uint8_t)(cl[0] ^ cl[1] ^ cl[2] ^ cl[3])};
            if (f->corrupt_bcc) {
                r[4] ^= 0x10;
            }
            reply(f, r, 5);
        } else if (n == 7 && d[1] == 0x70) {
            if (!crc_on(f)) {
                f->violations++;
            }
            if (memcmp(d + 2, cl, 4) == 0) {
                reply(f, &sak, 1);
            }
        }
    } else if (f->kind == 2 && f->cfg_tx == 0x0D) {
        if (n == 3 && d[0] == 0x26 && d[1] == 0x01) {
            if (!crc_on(f)) {
                f->violations++;
            }
            uint8_t r[10] = {0x00, 0x00};
            for (int i = 0; i < 8; i++) {
                r[2 + i] = f->uid[7 - i]; /* octet de poids faible en premier */
            }
            reply(f, r, 10);
        }
    }
}

static esp_err_t xfer(void *ctx, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    fake_t *f = ctx;
    if (f->absent) {
        memset(rx, 0xFF, rx_len); /* MISO tirée au niveau haut */
        return ESP_OK;
    }
    uint32_t *reg = f->reg;
    switch (tx[0]) {
    case 0x00:
        if (tx[1] == 0x03) {
            reg[0x02] &= ~le32(tx + 2); /* IRQ_CLEAR */
        } else {
            reg[tx[1]] = le32(tx + 2);
        }
        break;
    case 0x01:
        reg[tx[1]] |= le32(tx + 2);
        break;
    case 0x02:
        reg[tx[1]] &= le32(tx + 2);
        break;
    case 0x04: {
        uint32_t v = reg[tx[1]];
        if (tx[1] == 0x1D) {
            v = (reg[0x00] & 7) == 3 ? 1u << 24 : 0; /* WaitTransmit en mode transceive */
        }
        for (size_t i = 0; i < rx_len; i++) {
            rx[i] = (uint8_t)(v >> (8 * i));
        }
        break;
    }
    case 0x07: {
        const uint8_t fw[2] = {0x00, 0x04}, pr[2] = {0x00, 0x04};
        memcpy(rx, tx[1] == 0x12 ? fw : pr, rx_len < 2 ? rx_len : 2);
        break;
    }
    case 0x09:
        f->reg[0x02] = 0;
        f->reg[0x13] = 0;
        send_data(f, tx[1], tx + 2, tx_len - 2);
        break;
    case 0x0A:
        memcpy(rx, f->rx, rx_len < f->rx_len ? rx_len : f->rx_len);
        break;
    case 0x11:
        f->cfg_tx = tx[1];
        /* la configuration ISO 15693 active le CRC, celle de l'ISO 14443A non */
        f->reg[0x12] = f->reg[0x19] = tx[1] == 0x0D ? 1 : 0;
        break;
    case 0x16:
        f->rf = true;
        f->awake = false;
        break;
    case 0x17:
        f->rf = false;
        f->awake = false;
        break;
    default:
        f->violations++;
    }
    return ESP_OK;
}

static void delay_ms(void *ctx, uint32_t ms)
{
}

static fake_t g_fake;
static const pn5180_bus_t g_bus = {.xfer = xfer, .delay_ms = delay_ms, .ctx = &g_fake};

static void card(int kind, const char *hex, bool corrupt)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.kind = kind;
    g_fake.corrupt_bcc = corrupt;
    g_fake.uid_len = (int)strlen(hex) / 2;
    for (int i = 0; i < g_fake.uid_len; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        g_fake.uid[i] = (uint8_t)v;
    }
}

static bool read_ok(pn5180_proto_t prefer, const char *expect_hex, pn5180_proto_t expect_proto)
{
    uint8_t uid[PN5180_UID_MAX];
    uint8_t len = 0;
    pn5180_proto_t found;
    esp_err_t err = pn5180_poll(&g_bus, prefer, uid, &len, &found);
    if (err != ESP_OK || found != expect_proto || g_fake.rf) {
        return false;
    }
    if (!expect_hex) {
        return true;
    }
    char hex[2 * PN5180_UID_MAX + 1] = "";
    for (int i = 0; i < len; i++) {
        snprintf(hex + 2 * i, 3, "%02X", uid[i]);
    }
    return strcmp(hex, expect_hex) == 0;
}

void test_pn5180(void)
{
    uint16_t fw = 0, pr = 0;
    card(0, "", false);
    CHECK(pn5180_read_versions(&g_bus, &fw, &pr) == ESP_OK && fw == 0x0400);
    g_fake.absent = true;
    CHECK(pn5180_read_versions(&g_bus, &fw, &pr) == ESP_ERR_NOT_FOUND);

    /* Aucune carte : rien trouvé, champ coupé */
    card(0, "", false);
    CHECK(read_ok(PN5180_ISO14443A, NULL, PN5180_NONE));
    CHECK(g_fake.violations == 0);

    /* MIFARE Classic (4 octets), NTAG (7 octets), identifiant de 10 octets */
    card(1, "DEADBEEF", false);
    CHECK(read_ok(PN5180_NONE, "DEADBEEF", PN5180_ISO14443A));
    card(1, "04AB53A96F2681", false);
    CHECK(read_ok(PN5180_ISO14443A, "04AB53A96F2681", PN5180_ISO14443A));
    card(1, "0411223344556677889A", false);
    CHECK(read_ok(PN5180_ISO14443A, "0411223344556677889A", PN5180_ISO14443A));
    CHECK(g_fake.violations == 0);

    /* Octet de contrôle faux (collision, lecture en limite de portée) : pas de carte */
    card(1, "04AB53A96F2681", true);
    CHECK(read_ok(PN5180_ISO14443A, NULL, PN5180_NONE));

    /* Étiquette ISO 15693 : identifiant affiché octet de poids fort en premier */
    card(2, "E004015012345678", false);
    CHECK(read_ok(PN5180_ISO14443A, "E004015012345678", PN5180_ISO15693));
    CHECK(read_ok(PN5180_ISO15693, "E004015012345678", PN5180_ISO15693));
    CHECK(g_fake.violations == 0);

    /* Carte ISO 14443A trouvée même si l'ISO 15693 est essayée en premier */
    card(1, "04AB53A96F2681", false);
    CHECK(read_ok(PN5180_ISO15693, "04AB53A96F2681", PN5180_ISO14443A));
    CHECK(g_fake.violations == 0);
}
