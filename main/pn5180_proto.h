#pragma once
/*
 * Dialogue avec un PN5180 (NXP) par les commandes de son interface hôte, sans dépendance
 * matérielle (testé sur PC avec un PN5180 simulé). Le transport (SPI et signal BUSY) est
 * fourni par l'appelant (pn5180.c).
 *
 * Une interrogation allume le champ, cherche une carte ISO 14443A (MIFARE, NTAG : même
 * identifiant qu'avec le PN532) puis une étiquette ISO 15693 (ICODE SLIX, plus longue portée),
 * et coupe le champ.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PN5180_UID_MAX 10

typedef struct {
    /* Envoie une commande (tx) ; si rx_len > 0, lit ensuite rx_len octets de réponse. */
    esp_err_t (*xfer)(void *ctx, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} pn5180_bus_t;

typedef enum {
    PN5180_NONE = 0,
    PN5180_ISO14443A,
    PN5180_ISO15693,
} pn5180_proto_t;

/* Versions lues dans l'EEPROM : (majeur << 8) | mineur. ESP_ERR_NOT_FOUND si rien ne répond. */
esp_err_t pn5180_read_versions(const pn5180_bus_t *bus, uint16_t *firmware, uint16_t *product);
esp_err_t pn5180_field_off(const pn5180_bus_t *bus);
/*
 * Une interrogation. prefer : protocole essayé en premier (celui de la carte déjà posée).
 * found = PN5180_NONE si aucune carte (ce n'est pas une erreur). Identifiant dans l'ordre
 * habituel d'affichage (ISO 15693 : octet de poids fort en premier, E0...).
 */
esp_err_t pn5180_poll(const pn5180_bus_t *bus, pn5180_proto_t prefer, uint8_t *uid, uint8_t *uid_len,
                      pn5180_proto_t *found);
