#include "pn532_frame.h"

#include <stdbool.h>

enum {
    ST_START0 = 0, /* attente de 0x00 */
    ST_START1,     /* attente de 0xFF */
    ST_LEN,
    ST_LCS,
    ST_DATA,
    ST_DCS,
};

size_t pn532_build_frame(const uint8_t *payload, size_t len, uint8_t *out, size_t outlen)
{
    if (len == 0 || len > PN532_MAX_DATA || outlen < len + 7) {
        return 0;
    }
    size_t o = 0;
    out[o++] = 0x00;
    out[o++] = 0x00;
    out[o++] = 0xFF;
    out[o++] = (uint8_t)len;
    out[o++] = (uint8_t)(0x100 - len);
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        out[o++] = payload[i];
        sum += payload[i];
    }
    out[o++] = (uint8_t)(0x100 - sum);
    out[o++] = 0x00;
    return o;
}

void pn532_parser_reset(pn532_parser_t *p)
{
    p->state = ST_START0;
    p->len = 0;
    p->pos = 0;
    p->sum = 0;
}

pn532_parse_result_t pn532_parser_feed(pn532_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case ST_START0:
        if (b == 0x00) {
            p->state = ST_START1;
        }
        return PN532_PARSE_MORE;
    case ST_START1:
        if (b == 0xFF) {
            p->state = ST_LEN;
        } else if (b != 0x00) {
            p->state = ST_START0;
        }
        return PN532_PARSE_MORE;
    case ST_LEN:
        p->len = b;
        p->state = ST_LCS;
        return PN532_PARSE_MORE;
    case ST_LCS:
        if (p->len == 0x00 && b == 0xFF) {
            pn532_parser_reset(p);
            return PN532_PARSE_ACK;
        }
        if (p->len == 0xFF && b == 0x00) {
            pn532_parser_reset(p);
            return PN532_PARSE_NACK;
        }
        if ((uint8_t)(p->len + b) != 0 || p->len == 0 || p->len > PN532_MAX_DATA) {
            /* LCS invalide ou trame étendue (non utilisée) */
            pn532_parser_reset(p);
            return PN532_PARSE_BAD;
        }
        p->pos = 0;
        p->sum = 0;
        p->state = ST_DATA;
        return PN532_PARSE_MORE;
    case ST_DATA:
        p->data[p->pos++] = b;
        p->sum += b;
        if (p->pos == p->len) {
            p->state = ST_DCS;
        }
        return PN532_PARSE_MORE;
    case ST_DCS: {
        bool ok = (uint8_t)(p->sum + b) == 0;
        uint8_t len = p->len;
        p->state = ST_START0;
        if (!ok) {
            return PN532_PARSE_BAD;
        }
        if (len == 1 && p->data[0] == 0x7F) {
            return PN532_PARSE_ERROR_FRAME;
        }
        return PN532_PARSE_FRAME;
    }
    default:
        pn532_parser_reset(p);
        return PN532_PARSE_MORE;
    }
}
