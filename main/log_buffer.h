#pragma once
/*
 * Journal consultable dans l'interface web : les messages ESP_LOG (toujours envoyés sur le
 * port série) sont aussi gardés en mémoire, les plus récents d'abord conservés (48 Ko en
 * PSRAM). Au démarrage, la raison du redémarrage et le résumé d'un éventuel plantage y
 * sont inscrits.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* À appeler au tout début de app_main. */
void log_buffer_start(void);
/* Raison du dernier redémarrage, résumé du plantage précédent (après log_buffer_start). */
void log_buffer_report_boot(void);
/* Cf. log_ring_read. Taille utile : log_buffer_size() + 1. */
size_t log_buffer_read(uint64_t since, char *out, size_t cap, uint64_t *next, bool *reset);
size_t log_buffer_size(void);
/* Marge de pile de la tâche appelante : inscrit dans le journal chaque nouveau plus bas
 * (par paliers de 256 octets) pour vérifier les tailles de pile en usage réel. *low vaut
 * UINT32_MAX au départ ; detail (format lu…) peut être NULL. */
void log_buffer_stack_check(const char *task, const char *detail, uint32_t *low);
