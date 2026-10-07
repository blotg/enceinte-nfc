#pragma once
/*
 * Construction et analyse des trames PN532 (sans dépendance matérielle).
 *
 * Trame : 00 00 FF LEN LCS TFI DATA... DCS 00
 * ACK   : 00 00 FF 00 FF 00
 * NACK  : 00 00 FF FF 00 00
 */
#include <stddef.h>
#include <stdint.h>

#define PN532_TFI_HOST 0xD4
#define PN532_TFI_PN532 0xD5
#define PN532_MAX_DATA 254 /* trame normale : LEN <= 255 octets, TFI compris */

typedef enum {
    PN532_PARSE_MORE = 0,   /* trame incomplète */
    PN532_PARSE_ACK,
    PN532_PARSE_NACK,
    PN532_PARSE_FRAME,      /* trame valide dans parser->data (TFI compris), longueur parser->len */
    PN532_PARSE_ERROR_FRAME,/* trame d'erreur applicative (0x7F) */
    PN532_PARSE_BAD,        /* somme de contrôle invalide : l'analyseur repart de zéro */
} pn532_parse_result_t;

typedef struct {
    int state;
    uint8_t len;
    uint8_t pos;
    uint8_t sum;
    uint8_t data[PN532_MAX_DATA + 1];
} pn532_parser_t;

/* Construit une trame à partir de TFI+données. Retourne la taille écrite, 0 si out est trop petit. */
size_t pn532_build_frame(const uint8_t *payload, size_t len, uint8_t *out, size_t outlen);
void pn532_parser_reset(pn532_parser_t *p);
pn532_parse_result_t pn532_parser_feed(pn532_parser_t *p, uint8_t byte);
