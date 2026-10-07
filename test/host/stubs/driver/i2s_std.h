#pragma once
/* Substitut I2S : l'écriture dort le temps de jouer les échantillons (accéléré). */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef struct shim_i2s *i2s_chan_handle_t;
typedef struct {
    int id, role;
    uint32_t dma_desc_num, dma_frame_num;
    bool auto_clear_after_cb, auto_clear_before_cb, allow_pd;
    int intr_priority;
} i2s_chan_config_t;
typedef struct { uint32_t sample_rate_hz; } i2s_std_clk_config_t;
typedef struct { int data_bit_width, slot_mode; } i2s_std_slot_config_t;
typedef struct { int mclk, bclk, ws, dout, din; } i2s_std_gpio_config_t;
typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_GPIO_UNUSED -1
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define I2S_CHANNEL_DEFAULT_CONFIG(i, r) {.id = (i), .role = (r), .dma_desc_num = 6, .dma_frame_num = 240}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) {.sample_rate_hz = (rate)}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) {.data_bit_width = (bits), .slot_mode = (mode)}

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t *cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t h);
esp_err_t i2s_channel_disable(i2s_chan_handle_t h);
esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t h, const i2s_std_clk_config_t *clk);
esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *src, size_t size, size_t *written, TickType_t ticks);

/* Observation pour les tests */
uint64_t shim_i2s_frames_written(void);
uint32_t shim_i2s_rate(void);
