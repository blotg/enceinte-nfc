#include "pn532_frame.h"
#include "test.h"

static pn532_parse_result_t feed_all(pn532_parser_t *p, const uint8_t *b, size_t n, size_t *consumed)
{
    for (size_t i = 0; i < n; i++) {
        pn532_parse_result_t r = pn532_parser_feed(p, b[i]);
        if (r != PN532_PARSE_MORE) {
            *consumed = i + 1;
            return r;
        }
    }
    *consumed = n;
    return PN532_PARSE_MORE;
}

void test_pn532(void)
{
    /* GetFirmwareVersion : 00 00 FF 02 FE D4 02 2A 00 */
    uint8_t out[32];
    const uint8_t cmd[] = {0xD4, 0x02};
    size_t n = pn532_build_frame(cmd, sizeof(cmd), out, sizeof(out));
    const uint8_t expected[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
    CHECK(n == sizeof(expected) && memcmp(out, expected, n) == 0);
    CHECK(pn532_build_frame(cmd, sizeof(cmd), out, 5) == 0);

    pn532_parser_t p;
    size_t used;

    /* ACK puis réponse dans le même paquet, précédés de bruit */
    const uint8_t stream[] = {0x55, 0x12, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, /* ACK */
                              0x00, 0x00, 0xFF, 0x06, 0xFA, 0xD5, 0x03, 0x32, 0x01, 0x06, 0x07, 0xE8, 0x00};
    pn532_parser_reset(&p);
    CHECK(feed_all(&p, stream, sizeof(stream), &used) == PN532_PARSE_ACK);
    size_t off = used;
    CHECK(feed_all(&p, stream + off, sizeof(stream) - off, &used) == PN532_PARSE_FRAME);
    CHECK(p.len == 6 && p.data[0] == 0xD5 && p.data[1] == 0x03 && p.data[2] == 0x32);

    /* NACK */
    const uint8_t nack[] = {0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00};
    pn532_parser_reset(&p);
    CHECK(feed_all(&p, nack, sizeof(nack), &used) == PN532_PARSE_NACK);

    /* Trame d'erreur applicative */
    const uint8_t errf[] = {0x00, 0x00, 0xFF, 0x01, 0xFF, 0x7F, 0x81, 0x00};
    pn532_parser_reset(&p);
    CHECK(feed_all(&p, errf, sizeof(errf), &used) == PN532_PARSE_ERROR_FRAME);

    /* Somme de contrôle des données invalide */
    const uint8_t bad[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD5, 0x03, 0x00, 0x00};
    pn532_parser_reset(&p);
    CHECK(feed_all(&p, bad, sizeof(bad), &used) == PN532_PARSE_BAD);

    /* Somme de longueur invalide puis trame valide : l'analyseur se resynchronise */
    const uint8_t resync[] = {0x00, 0xFF, 0x05, 0x05, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD5, 0x15, 0x16, 0x00};
    pn532_parser_reset(&p);
    CHECK(feed_all(&p, resync, sizeof(resync), &used) == PN532_PARSE_BAD);
    off = used;
    CHECK(feed_all(&p, resync + off, sizeof(resync) - off, &used) == PN532_PARSE_FRAME);
    CHECK(p.len == 2 && p.data[1] == 0x15);

    /* Réponse InListPassiveTarget avec une carte (UID 7 octets) */
    const uint8_t uid_resp[] = {0xD5, 0x4B, 0x01, 0x01, 0x00, 0x44, 0x00, 0x07,
                                0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    n = pn532_build_frame(uid_resp, sizeof(uid_resp), out, sizeof(out));
    pn532_parser_reset(&p);
    CHECK(feed_all(&p, out, n, &used) == PN532_PARSE_FRAME);
    CHECK(p.len == sizeof(uid_resp) && p.data[7] == 7 && p.data[14] == 0x66);
}
