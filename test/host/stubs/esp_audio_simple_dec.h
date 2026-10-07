#pragma once
/* Substitut du décodeur : les octets du fichier sont rendus tels quels comme PCM 16 bits stéréo 44,1 kHz.
 * Un bloc commençant par "CORRUPT" provoque une erreur de décodage. */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ESP_AUDIO_ERR_OK = 0,
    ESP_AUDIO_ERR_FAIL = -1,
    ESP_AUDIO_ERR_MEM_LACK = -2,
    ESP_AUDIO_ERR_INVALID_PARAMETER = -5,
    ESP_AUDIO_ERR_NOT_SUPPORT = -7,
    ESP_AUDIO_ERR_BUFF_NOT_ENOUGH = -8,
    ESP_AUDIO_ERR_NOT_FOUND = -9,
} esp_audio_err_t;

typedef void *esp_audio_simple_dec_handle_t;
typedef enum {
    ESP_AUDIO_SIMPLE_DEC_TYPE_NONE = 0,
    ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
    ESP_AUDIO_SIMPLE_DEC_TYPE_AAC,
    ESP_AUDIO_SIMPLE_DEC_TYPE_M4A,
    ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC,
    ESP_AUDIO_SIMPLE_DEC_TYPE_WAV,
    ESP_AUDIO_SIMPLE_DEC_TYPE_OGG,
} esp_audio_simple_dec_type_t;

typedef struct {
    esp_audio_simple_dec_type_t dec_type;
    void *dec_cfg;
    int cfg_size;
    bool use_frame_dec;
} esp_audio_simple_dec_cfg_t;

typedef struct {
    uint8_t *buffer;
    uint32_t len;
    bool eos;
    uint32_t consumed;
    int frame_recover;
} esp_audio_simple_dec_raw_t;

typedef struct {
    uint8_t *buffer;
    uint32_t len;
    uint32_t needed_size;
    uint32_t decoded_size;
} esp_audio_simple_dec_out_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channel;
    uint32_t bitrate;
    uint32_t frame_size;
} esp_audio_simple_dec_info_t;

typedef struct {
    int32_t sample_rate;
    uint8_t channel;
    uint8_t bits_per_sample;
    bool no_adts_header;
    bool aac_plus_enable;
} esp_aac_dec_cfg_t;

typedef struct {
    uint8_t track_idx;
    bool aac_plus_enable;
} esp_m4a_dec_cfg_t;

esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *cfg, esp_audio_simple_dec_handle_t *h);
esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t h, esp_audio_simple_dec_raw_t *raw,
                                             esp_audio_simple_dec_out_t *out);
esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t h, esp_audio_simple_dec_info_t *info);
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t h);
