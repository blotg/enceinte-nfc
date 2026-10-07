#pragma once
/*
 * Lecture des métadonnées (tags, durée) et calcul de positions de recherche.
 * Sans dépendance ESP-IDF : testé sur PC (test/host).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "util.h"

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char album_artist[128];
    char genre[64];
    char date[16];
    char track[16];
    uint32_t duration_ms;  /* 0 si inconnue */
    uint32_t bitrate;      /* bit/s moyen, 0 si inconnu */
    uint32_t sample_rate;
    uint32_t audio_start;  /* MP3 : position de la première trame */
    uint32_t audio_end;    /* MP3 : fin des données audio (avant un tag ID3v1) */
    bool has_toc;          /* MP3 VBR avec table Xing */
    uint8_t toc[100];
} media_info_t;

/* Analyse le fichier ouvert. Retourne false seulement en cas d'erreur de lecture. */
bool media_probe(FILE *f, audio_fmt_t fmt, media_info_t *mi);

/* Position (octets) correspondant à un temps donné. 0 si la recherche n'est pas possible. */
uint32_t media_seek_offset(const media_info_t *mi, audio_fmt_t fmt, uint32_t ms);
