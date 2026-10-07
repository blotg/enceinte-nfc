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
    uint32_t audio_start;  /* début des données audio (première trame / page audio) */
    uint32_t audio_end;    /* fin des données audio (avant un tag ID3v1...) */
    bool seekable;         /* repositionnement possible (media_seek) */
    bool has_toc;          /* MP3 VBR avec table Xing */
    uint8_t toc[100];
    /* FLAC */
    uint64_t flac_total_samples;
    uint32_t flac_seektable_pos; /* 0 si pas de table de points de recherche */
    uint16_t flac_seektable_n;
    uint16_t flac_block_size;    /* taille de bloc fixe, 0 si variable */
    /* WAV */
    uint32_t wav_byte_rate;
    uint16_t wav_block_align;
    /* Ogg */
    bool ogg_opus;
    uint16_t ogg_preskip;
    uint32_t ogg_serial;
} media_info_t;

typedef struct {
    uint32_t offset;     /* position (octets) où reprendre la lecture */
    uint32_t header_end; /* > 0 : envoyer d'abord les octets [0, header_end) au décodeur */
    uint32_t actual_ms;  /* position réellement atteinte (début de trame ou de page) */
} media_seek_t;

/* Analyse le fichier ouvert. Retourne false seulement en cas d'erreur de lecture. */
bool media_probe(FILE *f, audio_fmt_t fmt, media_info_t *mi);

/*
 * Calcule où reprendre la lecture pour atteindre "ms" (MP3, FLAC, WAV, Ogg Opus/Vorbis).
 * Le fichier doit être celui analysé par media_probe. Retourne false si impossible.
 */
bool media_seek(FILE *f, const media_info_t *mi, audio_fmt_t fmt, uint32_t ms, media_seek_t *out);
