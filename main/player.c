#include "player.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "changes.h"
#include "driver/i2s_std.h"
#include "dsp.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media_info.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "player";

#define PCM_FRAMES 512      /* trames converties par écriture I2S */
#define CMD_WAIT_MS 2000
#define MAX_FAIL_STREAK 10

/* ---------- Structures ---------- */

typedef struct {
    uint8_t *data;
    uint32_t gen;
    uint32_t start_ms; /* position réelle de départ (après une recherche) */
    int len;
    bool eof;
    bool first;
    bool error;
} chunk_t;

typedef struct {
    char path[ABS_PATH_MAX]; /* vide = fermer */
    uint32_t gen;
    uint32_t seek_ms;
} read_req_t;

typedef struct {
    uint32_t id;
    char *path;
    bool played; /* mode aléatoire */
} item_t;

typedef enum {
    CMD_PLAY_POS,
    CMD_PLAY_ID,
    CMD_PAUSE,
    CMD_STOP,
    CMD_NEXT,
    CMD_PREV,
    CMD_SEEK,
    CMD_CURRENT_REMOVED,
    CMD_BEEP,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    int arg;
    uint32_t arg2;
    uint32_t seq;
} cmd_t;

/* ---------- État partagé (protégé par s_lock) ---------- */

static SemaphoreHandle_t s_lock;
static item_t *s_queue;
static int s_qlen, s_qcap;
static uint32_t s_qversion = 1;
static uint32_t s_next_id = 1;
static uint32_t s_cur_id;
static int s_removed_pos = -1;
static player_state_t s_state;
static int64_t s_paused_at_us;
static uint32_t s_elapsed_ms, s_duration_ms, s_base_ms;
static uint32_t s_bitrate_kbps;
static uint32_t s_rate = 44100;
static uint8_t s_bits = 16, s_channels = 2;
static int s_volume, s_max_volume = 100;
static volatile int32_t s_gain_q15;
static volatile uint8_t s_normalize, s_compress;
static volatile bool s_dsp_reset; /* nouvelle playlist : la normalisation repart de zéro */
static bool s_repeat, s_random, s_consume, s_seekable;
static uint8_t s_single;
static char s_error[96];
static media_info_t s_probe;
static uint32_t s_probe_gen;

/* ---------- Propre à la tâche player ---------- */

static QueueHandle_t s_cmd_q, s_req_q, s_free_q, s_filled_q;
static SemaphoreHandle_t s_post_lock;
static TaskHandle_t s_player_task;
static volatile uint32_t s_cmd_seq, s_done_seq;
static uint32_t s_gen;
static chunk_t *s_chunk;
static esp_audio_simple_dec_raw_t s_raw;
static esp_audio_simple_dec_handle_t s_dec;
static uint8_t *s_out;
static uint32_t s_out_cap;
static int16_t *s_pcm;
static i2s_chan_handle_t s_tx;
static uint32_t s_i2s_rate;
static uint64_t s_frames;
static bool s_need_info;
static int s_dec_errors, s_stall, s_fail_streak;
static bool s_skip_pending;
static int s_chunk_size, s_nchunks;
static uint8_t *s_bounce; /* tampon interne compatible DMA pour la lecture SD */
static player_event_cb_t s_cb;
static dsp_t s_dsp;
static float *s_fpcm; /* PCM_FRAMES échantillons mono pour la normalisation et la compression */

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ---------- Utilitaires de file (s_lock tenu) ---------- */

static int find_pos_locked(uint32_t id)
{
    if (id == 0) {
        return -1;
    }
    for (int i = 0; i < s_qlen; i++) {
        if (s_queue[i].id == id) {
            return i;
        }
    }
    return -1;
}

static void queue_changed_locked(void)
{
    s_qversion++;
    changes_notify(CHG_PLAYLIST);
}

/* Prochain morceau sans effet de bord (affichage). -1 si inconnu ou aucun. */
static int peek_next_locked(int cur)
{
    if (s_qlen == 0 || s_random) {
        return -1;
    }
    if (s_single) {
        return s_repeat && cur >= 0 ? cur : -1;
    }
    int n = cur + 1;
    if (n >= s_qlen) {
        return s_repeat ? 0 : -1;
    }
    return n;
}

static int compute_next_locked(int cur, bool natural)
{
    if (s_qlen == 0) {
        return -1;
    }
    if (natural && s_single) {
        return s_repeat && cur >= 0 ? cur : -1;
    }
    if (s_random) {
        int candidates = 0;
        for (int i = 0; i < s_qlen; i++) {
            if (!s_queue[i].played && i != cur) {
                candidates++;
            }
        }
        if (candidates == 0) {
            if (!s_repeat) {
                return -1;
            }
            for (int i = 0; i < s_qlen; i++) {
                s_queue[i].played = false;
            }
            candidates = s_qlen - (cur >= 0 ? 1 : 0);
            if (candidates <= 0) {
                return cur;
            }
        }
        int pick = (int)(esp_random() % (uint32_t)candidates);
        for (int i = 0; i < s_qlen; i++) {
            if (!s_queue[i].played && i != cur) {
                if (pick-- == 0) {
                    return i;
                }
            }
        }
        return -1;
    }
    int n = cur + 1;
    if (n >= s_qlen) {
        return s_repeat ? 0 : -1;
    }
    return n;
}

static bool remove_range_locked(int start, int end)
{
    int cur = find_pos_locked(s_cur_id);
    bool removed_current = cur >= start && cur < end;
    for (int i = start; i < end; i++) {
        free(s_queue[i].path);
    }
    memmove(&s_queue[start], &s_queue[end], (s_qlen - end) * sizeof(item_t));
    s_qlen -= end - start;
    if (removed_current) {
        s_removed_pos = start;
    }
    queue_changed_locked();
    return removed_current;
}

static void update_gain_locked(void)
{
    int v = s_volume < s_max_volume ? s_volume : s_max_volume;
    float x = v / 100.0f;
    s_gain_q15 = (int32_t)(x * x * 32767.0f); /* courbe quadratique, proche de la perception de l'oreille */
}

/* ---------- Commandes ---------- */

static esp_err_t post(cmd_type_t type, int arg, uint32_t arg2, bool wait)
{
    if (!s_cmd_q || !s_player_task) {
        return ESP_ERR_INVALID_STATE; /* lecteur non initialisé (I2S en défaut) */
    }
    cmd_t c = {.type = type, .arg = arg, .arg2 = arg2};
    xSemaphoreTake(s_post_lock, portMAX_DELAY);
    c.seq = ++s_cmd_seq;
    BaseType_t ok = xQueueSend(s_cmd_q, &c, pdMS_TO_TICKS(500));
    xSemaphoreGive(s_post_lock);
    if (ok != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!wait || xTaskGetCurrentTaskHandle() == s_player_task) {
        return ESP_OK;
    }
    for (int waited = 0; (int32_t)(s_done_seq - c.seq) < 0; waited += 2) {
        if (waited >= CMD_WAIT_MS) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_OK;
}

/* ---------- Tâche de lecture SD ---------- */

static void send_chunk_error(uint32_t gen)
{
    chunk_t *c;
    if (xQueueReceive(s_free_q, &c, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;
    }
    c->gen = gen;
    c->len = 0;
    c->eof = true;
    c->first = true;
    c->error = true;
    c->start_ms = 0;
    xQueueSend(s_filled_q, &c, portMAX_DELAY);
}

static void reader_task(void *arg)
{
    int fd = -1;
    uint32_t gen = 0, pos = 0, size = 0, start_ms = 0;
    uint32_t header_left = 0, jump_to = 0; /* repositionnement : en-tête d'abord, puis saut */
    bool first = false;
    media_info_t *mi = malloc(sizeof(media_info_t));
    for (;;) {
        read_req_t req;
        if (xQueueReceive(s_req_q, &req, fd >= 0 ? 0 : portMAX_DELAY) == pdTRUE) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
            gen = req.gen;
            if (!req.path[0]) {
                continue;
            }
            audio_fmt_t fmt = audio_fmt_from_name(req.path);
            memset(mi, 0, sizeof(*mi));
            FILE *f = fopen(req.path, "rb");
            if (!f) {
                ESP_LOGW(TAG, "ouverture impossible : %s (errno %d)", req.path, errno);
                if (errno == EIO) {
                    storage_report_io_error();
                }
                send_chunk_error(gen);
                continue;
            }
            media_probe(f, fmt, mi);
            media_seek_t sk = {0};
            start_ms = 0;
            if (req.seek_ms && media_seek(f, mi, fmt, req.seek_ms, &sk)) {
                start_ms = sk.actual_ms;
            } else {
                memset(&sk, 0, sizeof(sk));
            }
            fclose(f);
            LOCK();
            s_probe = *mi;
            s_probe_gen = gen;
            UNLOCK();
            fd = open(req.path, O_RDONLY);
            struct stat st;
            if (fd < 0 || fstat(fd, &st) != 0) {
                if (fd >= 0) {
                    close(fd);
                    fd = -1;
                }
                send_chunk_error(gen);
                continue;
            }
            size = (uint32_t)st.st_size;
            pos = 0;
            header_left = 0;
            if (sk.offset && sk.header_end) {
                /* FLAC, Ogg, WAV : le décodeur doit d'abord recevoir l'en-tête du fichier */
                header_left = sk.header_end;
                jump_to = sk.offset;
            } else if (sk.offset && lseek(fd, sk.offset, SEEK_SET) == (off_t)sk.offset) {
                pos = sk.offset;
            } else if (sk.offset) {
                lseek(fd, 0, SEEK_SET);
                start_ms = 0;
            }
            if (start_ms) {
                ESP_LOGI(TAG, "reprise à %u,%03u s", (unsigned)(start_ms / 1000), (unsigned)(start_ms % 1000));
            }
            first = true;
            continue;
        }
        if (fd < 0) {
            continue;
        }
        chunk_t *c;
        if (xQueueReceive(s_free_q, &c, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue; /* réservoir plein : on revient surveiller les requêtes */
        }
        size_t want = s_chunk_size;
        if (header_left && want > header_left) {
            want = header_left;
        }
        int n = read(fd, s_bounce, want);
        if (n > 0 && header_left) {
            header_left -= (uint32_t)n < header_left ? (uint32_t)n : header_left;
            if (header_left == 0) {
                if (lseek(fd, jump_to, SEEK_SET) == (off_t)jump_to) {
                    pos = jump_to - n; /* "pos += n" ci-dessous : pos = jump_to */
                } else {
                    start_ms = 0; /* saut impossible : lecture depuis le début */
                }
            }
        }
        c->gen = gen;
        c->first = first;
        c->start_ms = start_ms;
        first = false;
        if (n < 0) {
            ESP_LOGW(TAG, "erreur de lecture SD (errno %d)", errno);
            storage_report_io_error();
            c->len = 0;
            c->error = true;
            c->eof = true;
        } else {
            if (n > 0 && c->data != s_bounce) {
                memcpy(c->data, s_bounce, n);
            }
            c->len = n;
            c->error = false;
            pos += n;
            c->eof = n == 0 || pos >= size;
        }
        xQueueSend(s_filled_q, &c, portMAX_DELAY);
        if (c->eof) {
            close(fd);
            fd = -1;
        }
    }
}

/* ---------- Sortie I2S ---------- */

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 600; /* ~80 ms à 44,1 kHz ; le réservoir de lecture absorbe le reste */
    chan_cfg.auto_clear_after_cb = true; /* silence si plus de données (pause) */
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = CONFIG_ENC_I2S_BCLK_GPIO,
                .ws = CONFIG_ENC_I2S_WS_GPIO,
                .dout = CONFIG_ENC_I2S_DOUT_GPIO,
                .din = I2S_GPIO_UNUSED,
            },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    s_i2s_rate = 44100;
    return err;
}

static void i2s_set_rate(uint32_t rate)
{
    if (rate == s_i2s_rate || rate < 8000 || rate > 96000) {
        return;
    }
    i2s_channel_disable(s_tx);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    if (i2s_channel_reconfig_std_clock(s_tx, &clk) == ESP_OK) {
        s_i2s_rate = rate;
    }
    i2s_channel_enable(s_tx);
}

static void i2s_write_frames(const int16_t *pcm, uint32_t frames)
{
    static int64_t last_log;
    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx, pcm, frames * 4, &written, pdMS_TO_TICKS(1000));
    if ((err != ESP_OK || written != frames * 4) && esp_timer_get_time() - last_log > 5000000) {
        last_log = esp_timer_get_time();
        ESP_LOGE(TAG, "écriture I2S : %s (%u/%u octets)", esp_err_to_name(err), (unsigned)written,
                 (unsigned)(frames * 4));
    }
}

static void play_beep(beep_t beep)
{
    int count = beep == BEEP_UNKNOWN ? 2 : (beep == BEEP_ERROR ? 3 : 1);
    float freq = beep == BEEP_OK ? 880.0f : (beep == BEEP_UNKNOWN ? 660.0f : (beep == BEEP_ERROR ? 330.0f : 1000.0f));
    uint32_t rate = s_i2s_rate;
    /* Au volume réglé : crête à 25 % de la pleine échelle, soit à peu près le niveau moyen
     * d'une musique (-15 dBFS), multipliée par le gain du volume. Volume 0 : silence. */
    float amp = s_gain_q15 * 0.25f;
    uint32_t tone = rate * (beep == BEEP_TICK ? 45 : 120) / 1000, gap = rate * (beep == BEEP_TICK ? 20 : 90) / 1000;
    ESP_LOGI(TAG, "bip x%d (%.0f Hz, amplitude %d/32767, %u Hz)", count, freq, (int)amp, (unsigned)rate);
    for (int b = 0; b < count; b++) {
        for (uint32_t done = 0; done < tone + gap;) {
            uint32_t n = tone + gap - done > PCM_FRAMES ? PCM_FRAMES : tone + gap - done;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t t = done + i;
                float v = 0;
                if (t < tone) {
                    float env = t < rate / 200 ? (float)t / (rate / 200) : 1.0f; /* attaque douce */
                    if (tone - t < rate / 200) {
                        env = (float)(tone - t) / (rate / 200);
                    }
                    v = amp * env * sinf(2.0f * (float)M_PI * freq * t / rate);
                }
                s_pcm[2 * i] = s_pcm[2 * i + 1] = (int16_t)v;
            }
            i2s_write_frames(s_pcm, n);
            done += n;
        }
    }
}

/* ---------- Décodage ---------- */

static void release_chunk(chunk_t *c)
{
    if (c) {
        xQueueSend(s_free_q, &c, portMAX_DELAY);
    }
}

static void drop_current_chunk(void)
{
    release_chunk(s_chunk);
    s_chunk = NULL;
    s_raw.len = 0;
}

static void close_decoder(void)
{
    if (s_dec) {
        esp_audio_simple_dec_close(s_dec);
        s_dec = NULL;
    }
}

static bool open_decoder(audio_fmt_t fmt)
{
    union {
        esp_aac_dec_cfg_t aac;
        esp_m4a_dec_cfg_t m4a;
    } extra;
    memset(&extra, 0, sizeof(extra));
    esp_audio_simple_dec_cfg_t cfg = {0};
    switch (fmt) {
    case AUDIO_FMT_MP3:
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        break;
    case AUDIO_FMT_AAC:
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        extra.aac.aac_plus_enable = true;
        cfg.dec_cfg = &extra.aac;
        cfg.cfg_size = sizeof(extra.aac);
        break;
    case AUDIO_FMT_M4A:
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
        extra.m4a.aac_plus_enable = true;
        cfg.dec_cfg = &extra.m4a;
        cfg.cfg_size = sizeof(extra.m4a);
        break;
    case AUDIO_FMT_FLAC:
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        break;
    case AUDIO_FMT_WAV:
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        break;
    case AUDIO_FMT_OGG:
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
        break;
    default:
        return false;
    }
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &s_dec);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "décodeur indisponible (%d)", ret);
        s_dec = NULL;
        return false;
    }
    return true;
}

static void set_error(const char *msg, const char *path)
{
    LOCK();
    snprintf(s_error, sizeof(s_error), "%s : %s", msg, path_basename(path));
    UNLOCK();
    ESP_LOGW(TAG, "%s", s_error);
}

static void do_stop(void)
{
    s_skip_pending = false;
    close_decoder();
    drop_current_chunk();
    s_gen++;
    read_req_t req = {.gen = s_gen};
    xQueueOverwrite(s_req_q, &req);
    LOCK();
    s_state = PLAYER_STOPPED;
    s_elapsed_ms = 0;
    UNLOCK();
    changes_notify(CHG_PLAYER);
}

static void advance(bool natural);

static void start_song(uint32_t id, uint32_t seek_ms)
{
    char rel[REL_PATH_MAX];
    LOCK();
    int pos = find_pos_locked(id);
    if (pos >= 0) {
        str_copy(rel, s_queue[pos].path, sizeof(rel));
        s_queue[pos].played = true;
    }
    UNLOCK();
    if (pos < 0) {
        do_stop();
        return;
    }
    s_skip_pending = false;
    close_decoder();
    drop_current_chunk();
    s_gen++;
    read_req_t req = {.gen = s_gen, .seek_ms = seek_ms};
    if (!path_to_abs(rel, req.path, sizeof(req.path))) {
        req.path[0] = '\0';
    }
    xQueueOverwrite(s_req_q, &req);

    audio_fmt_t fmt = audio_fmt_from_name(rel);
    bool ok = req.path[0] && open_decoder(fmt);
    LOCK();
    s_cur_id = id;
    s_state = PLAYER_PLAYING;
    s_elapsed_ms = seek_ms;
    s_base_ms = seek_ms;
    s_duration_ms = 0;
    s_bitrate_kbps = 0;
    s_seekable = false;
    UNLOCK();
    s_frames = 0;
    s_need_info = true;
    s_dec_errors = 0;
    s_stall = 0;
    changes_notify(CHG_PLAYER);
    ESP_LOGI(TAG, "lecture : %s", rel);
    if (!ok) {
        set_error("format non pris en charge", rel);
        s_fail_streak++;
        s_skip_pending = true; /* traité par la boucle de la tâche, sans récursion */
    }
}

static void advance(bool natural)
{
    LOCK();
    int limit = s_qlen < MAX_FAIL_STREAK ? s_qlen : MAX_FAIL_STREAK;
    bool give_up = s_fail_streak > 0 && s_fail_streak >= limit;
    int cur = find_pos_locked(s_cur_id);
    int next = give_up ? -1 : compute_next_locked(cur, natural);
    uint32_t next_id = next >= 0 ? s_queue[next].id : 0;
    if (natural && s_consume && cur >= 0) {
        remove_range_locked(cur, cur + 1);
        s_removed_pos = -1; /* géré ici même */
        if (next_id == s_cur_id) {
            next_id = 0;
        }
    }
    if (natural && s_single == 2) {
        s_single = 0;
        changes_notify(CHG_OPTIONS);
    }
    UNLOCK();
    if (give_up) {
        ESP_LOGW(TAG, "trop d'échecs consécutifs, arrêt");
        s_fail_streak = 0;
    }
    if (next_id) {
        start_song(next_id, 0);
    } else {
        do_stop();
        if (natural && !give_up && s_cb) {
            s_cb(PLAYER_EVT_QUEUE_END);
        }
    }
}

static inline int32_t read_sample(const uint8_t *p, int bps)
{
    switch (bps) {
    case 2:
        return (int16_t)(p[0] | p[1] << 8);
    case 3:
        return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 16;
    default:
        return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) >> 16;
    }
}

static void output_pcm(const uint8_t *buf, uint32_t size)
{
    if (s_need_info) {
        esp_audio_simple_dec_info_t info;
        if (esp_audio_simple_dec_get_info(s_dec, &info) != ESP_AUDIO_ERR_OK || info.sample_rate == 0) {
            return;
        }
        LOCK();
        s_rate = info.sample_rate;
        s_bits = info.bits_per_sample;
        s_channels = info.channel;
        if (!s_bitrate_kbps && info.bitrate) {
            s_bitrate_kbps = info.bitrate / 1000;
        }
        UNLOCK();
        i2s_set_rate(info.sample_rate);
        dsp_set_rate(&s_dsp, info.sample_rate);
        s_need_info = false;
        changes_notify(CHG_PLAYER);
    }
    int bps = s_bits / 8;
    int ch = s_channels;
    if (bps < 2 || bps > 4 || ch < 1) {
        return;
    }
    if (s_normalize != s_dsp.normalize || s_compress != s_dsp.compress) {
        dsp_set_levels(&s_dsp, s_normalize, s_compress);
    }
    if (s_dsp_reset) {
        s_dsp_reset = false;
        dsp_reset(&s_dsp);
    }
    bool processing = s_fpcm && dsp_active(&s_dsp);
    uint32_t frames = size / (uint32_t)(bps * ch);
    const uint8_t *p = buf;
    int32_t gain = s_gain_q15;
    while (frames) {
        uint32_t n = frames > PCM_FRAMES ? PCM_FRAMES : frames;
        for (uint32_t i = 0; i < n; i++) {
            int32_t l = read_sample(p, bps);
            int32_t r = ch > 1 ? read_sample(p + bps, bps) : l;
            p += bps * ch;
            /* Un seul haut-parleur : mixage mono sur les deux canaux, ainsi le MAX98357A joue
             * tout le morceau quel que soit le câblage de sa broche SD (gauche, droite ou mixage). */
            if (processing) {
                s_fpcm[i] = (float)((l + r) / 2) * (1.0f / 32768.0f);
            } else {
                int16_t m = (int16_t)((((l + r) / 2) * gain) >> 15);
                s_pcm[2 * i] = m;
                s_pcm[2 * i + 1] = m;
            }
        }
        if (processing) {
            /* Normalisation, compression et limiteur (crêtes sous -1 dBFS), puis volume */
            dsp_process(&s_dsp, s_fpcm, n);
            float g = (float)gain;
            for (uint32_t i = 0; i < n; i++) {
                int32_t m = (int32_t)lrintf(s_fpcm[i] * g);
                m = m > 32767 ? 32767 : (m < -32768 ? -32768 : m);
                s_pcm[2 * i] = (int16_t)m;
                s_pcm[2 * i + 1] = (int16_t)m;
            }
        }
        i2s_write_frames(s_pcm, n);
        frames -= n;
        s_frames += n;
    }
    LOCK();
    s_elapsed_ms = s_base_ms + (uint32_t)(s_frames * 1000 / (s_rate ? s_rate : 44100));
    UNLOCK();
}

static void track_failed(const char *msg)
{
    char rel[REL_PATH_MAX] = "";
    LOCK();
    int pos = find_pos_locked(s_cur_id);
    if (pos >= 0) {
        str_copy(rel, s_queue[pos].path, sizeof(rel));
    }
    UNLOCK();
    set_error(msg, rel);
    s_fail_streak++;
    advance(false);
}

static void decode_step(void)
{
    if (s_raw.len == 0) {
        if (s_chunk) {
            bool eof = s_chunk->eof;
            drop_current_chunk();
            if (eof) {
                advance(true);
                return;
            }
        }
        chunk_t *c;
        if (xQueueReceive(s_filled_q, &c, pdMS_TO_TICKS(10)) != pdTRUE) {
            return; /* pas encore de données : l'I2S joue du silence */
        }
        if (c->gen != s_gen) {
            release_chunk(c);
            return;
        }
        if (c->first) {
            LOCK();
            if (s_probe_gen == c->gen) {
                s_duration_ms = s_probe.duration_ms;
                s_bitrate_kbps = s_probe.bitrate / 1000;
                s_seekable = s_probe.seekable;
            }
            s_base_ms = c->start_ms;
            s_elapsed_ms = c->start_ms;
            UNLOCK();
        }
        if (c->error) {
            release_chunk(c);
            track_failed("lecture impossible");
            return;
        }
        if (c->len == 0) {
            release_chunk(c);
            advance(true);
            return;
        }
        s_chunk = c;
        s_raw.buffer = c->data;
        s_raw.len = c->len;
        s_raw.eos = c->eof;
        s_raw.consumed = 0;
    }
    if (!s_dec) {
        drop_current_chunk();
        return;
    }
    esp_audio_simple_dec_out_t out = {.buffer = s_out, .len = s_out_cap};
    esp_audio_err_t ret = esp_audio_simple_dec_process(s_dec, &s_raw, &out);
    if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        uint8_t *nb = heap_caps_realloc(s_out, out.needed_size, MALLOC_CAP_8BIT);
        if (!nb) {
            track_failed("mémoire insuffisante");
            return;
        }
        s_out = nb;
        s_out_cap = out.needed_size;
        return;
    }
    if (ret != ESP_AUDIO_ERR_OK) {
        /* Données refusées (paquet corrompu, métadonnées inattendues...) : on saute la partie
         * consommée si le décodeur l'indique, sinon tout le bloc ; il se resynchronise ensuite. */
        if (s_raw.consumed > 0 && s_raw.consumed < s_raw.len) {
            s_raw.len -= s_raw.consumed;
            s_raw.buffer += s_raw.consumed;
        } else {
            s_raw.len = 0;
        }
        if (++s_dec_errors > 20) {
            track_failed("fichier illisible");
        }
        return;
    }
    if (out.decoded_size) {
        s_dec_errors = 0;
        s_stall = 0;
        s_fail_streak = 0;
        output_pcm(out.buffer, out.decoded_size);
    } else if (s_raw.consumed == 0) {
        if (++s_stall > 8) {
            s_raw.len = 0; /* le décodeur ne progresse plus sur ce bloc */
            s_stall = 0;
        }
        return;
    }
    if (s_raw.consumed > s_raw.len) {
        s_raw.consumed = s_raw.len;
    }
    s_raw.len -= s_raw.consumed;
    s_raw.buffer += s_raw.consumed;
}

static void handle_cmd(const cmd_t *c)
{
    switch (c->type) {
    case CMD_PLAY_POS: {
        uint32_t id = 0;
        LOCK();
        player_state_t st = s_state;
        if (c->arg >= 0) {
            id = c->arg < s_qlen ? s_queue[c->arg].id : 0;
        } else if (st == PLAYER_STOPPED) {
            id = find_pos_locked(s_cur_id) >= 0 ? s_cur_id : 0;
            if (!id && s_qlen) {
                int first = s_random ? compute_next_locked(-1, false) : 0;
                id = first >= 0 ? s_queue[first].id : 0;
            }
        }
        UNLOCK();
        if (c->arg < 0 && st == PLAYER_PAUSED) {
            LOCK();
            s_state = PLAYER_PLAYING;
            UNLOCK();
            changes_notify(CHG_PLAYER);
        } else if (id) {
            s_fail_streak = 0;
            player_clear_error();
            start_song(id, 0);
        }
        break;
    }
    case CMD_PLAY_ID:
        s_fail_streak = 0;
        player_clear_error();
        start_song(c->arg2, 0);
        break;
    case CMD_PAUSE: {
        LOCK();
        player_state_t st = s_state;
        if (st == PLAYER_PLAYING && (c->arg == 1 || c->arg == -1)) {
            s_state = PLAYER_PAUSED;
            s_paused_at_us = esp_timer_get_time();
        } else if (st == PLAYER_PAUSED && (c->arg == 0 || c->arg == -1)) {
            s_state = PLAYER_PLAYING;
        }
        bool changed = st != s_state;
        UNLOCK();
        if (changed) {
            changes_notify(CHG_PLAYER);
        }
        break;
    }
    case CMD_STOP:
        do_stop();
        break;
    case CMD_NEXT:
        if (s_state != PLAYER_STOPPED) {
            s_fail_streak = 0;
            advance(false);
        }
        break;
    case CMD_PREV: {
        if (s_state == PLAYER_STOPPED) {
            break;
        }
        LOCK();
        int cur = find_pos_locked(s_cur_id);
        int prev = cur - 1;
        if (prev < 0) {
            prev = s_repeat && s_qlen ? s_qlen - 1 : (cur >= 0 ? cur : -1);
        }
        uint32_t id = prev >= 0 && prev < s_qlen ? s_queue[prev].id : 0;
        UNLOCK();
        if (id) {
            start_song(id, 0);
        }
        break;
    }
    case CMD_SEEK: {
        player_state_t before = s_state;
        start_song(c->arg2, (uint32_t)c->arg);
        if (before == PLAYER_PAUSED) {
            LOCK();
            s_state = PLAYER_PAUSED;
            s_paused_at_us = esp_timer_get_time();
            UNLOCK();
        }
        break;
    }
    case CMD_CURRENT_REMOVED: {
        LOCK();
        bool still = find_pos_locked(s_cur_id) >= 0;
        int pos = s_removed_pos;
        s_removed_pos = -1;
        uint32_t id = (!still && pos >= 0 && pos < s_qlen) ? s_queue[pos].id : 0;
        bool playing = s_state == PLAYER_PLAYING;
        if (!still) {
            s_cur_id = 0;
        }
        UNLOCK();
        if (still) {
            break;
        }
        if (playing && id) {
            start_song(id, 0);
        } else if (s_state != PLAYER_STOPPED) {
            do_stop();
        }
        break;
    }
    case CMD_BEEP:
        if (s_state != PLAYER_PLAYING) {
            play_beep((beep_t)c->arg);
        }
        break;
    }
}

static void player_task(void *arg)
{
    for (;;) {
        cmd_t c;
        TickType_t wait = (s_state == PLAYER_PLAYING || s_skip_pending) ? 0 : portMAX_DELAY;
        if (xQueueReceive(s_cmd_q, &c, wait) == pdTRUE) {
            handle_cmd(&c);
            s_done_seq = c.seq;
            continue;
        }
        if (s_skip_pending) {
            s_skip_pending = false;
            advance(false);
            continue;
        }
        if (s_state == PLAYER_PLAYING) {
            decode_step();
        }
    }
}

/* ---------- API publique ---------- */

esp_err_t player_init(uint8_t volume, uint8_t max_volume, player_event_cb_t cb)
{
    s_cb = cb;
    s_lock = xSemaphoreCreateMutex();
    s_post_lock = xSemaphoreCreateMutex();
    s_max_volume = max_volume && max_volume <= 100 ? max_volume : 100;
    s_volume = volume < s_max_volume ? volume : s_max_volume;
    update_gain_locked();
    dsp_init(&s_dsp, 44100);

    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    bool psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    s_chunk_size = psram ? 8192 : 4096;
    s_nchunks = psram ? 32 : 6; /* 2 s de MP3 à 128 kbit/s sans PSRAM, 16 s avec */
    s_cmd_q = xQueueCreate(16, sizeof(cmd_t));
    s_req_q = xQueueCreate(1, sizeof(read_req_t));
    s_free_q = xQueueCreate(s_nchunks, sizeof(chunk_t *));
    s_filled_q = xQueueCreate(s_nchunks, sizeof(chunk_t *));
    s_bounce = heap_caps_malloc(s_chunk_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_out_cap = 8192;
    s_out = heap_caps_malloc(s_out_cap, MALLOC_CAP_8BIT);
    s_pcm = heap_caps_malloc(PCM_FRAMES * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_fpcm = heap_caps_malloc(PCM_FRAMES * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_cmd_q || !s_req_q || !s_free_q || !s_filled_q || !s_bounce || !s_out || !s_pcm) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_nchunks; i++) {
        chunk_t *c = calloc(1, sizeof(chunk_t));
        if (!c) {
            return ESP_ERR_NO_MEM;
        }
        c->data = psram ? heap_caps_malloc(s_chunk_size, MALLOC_CAP_SPIRAM) : malloc(s_chunk_size);
        if (!c->data) {
            return ESP_ERR_NO_MEM;
        }
        xQueueSend(s_free_q, &c, 0);
    }
    ESP_LOGI(TAG, "réservoir de lecture : %d x %d o (%s)", s_nchunks, s_chunk_size, psram ? "PSRAM" : "interne");

    esp_err_t err = i2s_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S : %s", esp_err_to_name(err));
        return err;
    }
    xTaskCreatePinnedToCore(reader_task, "reader", 4608, NULL, 9, NULL, 1);
    /* Les décodeurs (Opus, Vorbis, FLAC...) demandent environ 20 Ko de pile selon Espressif. */
    xTaskCreatePinnedToCore(player_task, "player", 24576, NULL, 10, &s_player_task, 1);
    return ESP_OK;
}

void player_get_status(player_status_t *st)
{
    memset(st, 0, sizeof(*st));
    LOCK();
    st->state = s_state;
    int cur = find_pos_locked(s_cur_id);
    st->song = cur;
    st->song_id = cur >= 0 ? s_cur_id : 0;
    int nx = peek_next_locked(cur);
    st->next_song = nx;
    st->next_song_id = nx >= 0 ? s_queue[nx].id : 0;
    st->queue_len = s_qlen;
    st->queue_version = s_qversion;
    st->elapsed_ms = s_state == PLAYER_STOPPED ? 0 : s_elapsed_ms;
    st->duration_ms = s_duration_ms;
    st->bitrate_kbps = s_bitrate_kbps;
    st->sample_rate = s_rate;
    st->bits = s_bits;
    st->channels = s_channels;
    st->volume = (uint8_t)s_volume;
    st->repeat = s_repeat;
    st->random = s_random;
    st->consume = s_consume;
    st->single = s_single;
    st->seekable = s_seekable;
    st->paused_s = s_state == PLAYER_PAUSED ? (uint32_t)((esp_timer_get_time() - s_paused_at_us) / 1000000) : 0;
    if (cur >= 0) {
        str_copy(st->file, s_queue[cur].path, sizeof(st->file));
    }
    str_copy(st->error, s_error, sizeof(st->error));
    UNLOCK();
}

int player_queue_length(void)
{
    LOCK();
    int n = s_qlen;
    UNLOCK();
    return n;
}

uint32_t player_queue_version(void)
{
    LOCK();
    uint32_t v = s_qversion;
    UNLOCK();
    return v;
}

bool player_queue_get(int pos, queue_item_t *out)
{
    LOCK();
    bool ok = pos >= 0 && pos < s_qlen;
    if (ok) {
        out->id = s_queue[pos].id;
        str_copy(out->path, s_queue[pos].path, sizeof(out->path));
    }
    UNLOCK();
    return ok;
}

int player_queue_pos_of_id(uint32_t id)
{
    LOCK();
    int pos = find_pos_locked(id);
    UNLOCK();
    return pos;
}

static bool grow_locked(int need)
{
    if (need <= s_qcap) {
        return true;
    }
    int cap = s_qcap ? s_qcap : 32;
    while (cap < need) {
        cap *= 2;
    }
    item_t *n = realloc(s_queue, cap * sizeof(item_t));
    if (!n) {
        return false;
    }
    s_queue = n;
    s_qcap = cap;
    return true;
}

esp_err_t player_queue_add(const char *rel_path, int pos, uint32_t *id_out)
{
    char *p = strdup(rel_path);
    if (!p) {
        return ESP_ERR_NO_MEM;
    }
    LOCK();
    if (pos < 0) {
        pos = s_qlen;
    }
    if (pos > s_qlen || !grow_locked(s_qlen + 1)) {
        UNLOCK();
        free(p);
        return pos > s_qlen ? ESP_ERR_INVALID_ARG : ESP_ERR_NO_MEM;
    }
    memmove(&s_queue[pos + 1], &s_queue[pos], (s_qlen - pos) * sizeof(item_t));
    s_queue[pos] = (item_t){.id = s_next_id++, .path = p, .played = false};
    s_qlen++;
    if (id_out) {
        *id_out = s_queue[pos].id;
    }
    queue_changed_locked();
    UNLOCK();
    return ESP_OK;
}

esp_err_t player_queue_delete(int start, int end)
{
    LOCK();
    if (start < 0 || end > s_qlen || start >= end) {
        UNLOCK();
        return ESP_ERR_INVALID_ARG;
    }
    bool cur = remove_range_locked(start, end);
    UNLOCK();
    if (cur) {
        post(CMD_CURRENT_REMOVED, 0, 0, false);
    }
    return ESP_OK;
}

esp_err_t player_queue_move(int start, int end, int to)
{
    LOCK();
    int count = end - start;
    if (start < 0 || end > s_qlen || count <= 0 || to < 0 || to > s_qlen - count) {
        UNLOCK();
        return ESP_ERR_INVALID_ARG;
    }
    item_t *tmp = malloc(count * sizeof(item_t));
    if (!tmp) {
        UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    memcpy(tmp, &s_queue[start], count * sizeof(item_t));
    memmove(&s_queue[start], &s_queue[end], (s_qlen - end) * sizeof(item_t));
    int remaining = s_qlen - count;
    memmove(&s_queue[to + count], &s_queue[to], (remaining - to) * sizeof(item_t));
    memcpy(&s_queue[to], tmp, count * sizeof(item_t));
    free(tmp);
    queue_changed_locked();
    UNLOCK();
    return ESP_OK;
}

esp_err_t player_queue_clear(void)
{
    LOCK();
    bool had = s_qlen > 0;
    bool cur = had && remove_range_locked(0, s_qlen);
    s_removed_pos = -1;
    UNLOCK();
    if (cur) {
        post(CMD_CURRENT_REMOVED, 0, 0, false);
    }
    return ESP_OK;
}

esp_err_t player_queue_shuffle(void)
{
    LOCK();
    for (int i = s_qlen - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        item_t t = s_queue[i];
        s_queue[i] = s_queue[j];
        s_queue[j] = t;
    }
    if (s_qlen > 1) {
        queue_changed_locked();
    }
    UNLOCK();
    return ESP_OK;
}

esp_err_t player_queue_replace(const path_list_t *list)
{
    item_t *nq = list->count ? malloc(list->count * sizeof(item_t)) : NULL;
    if (list->count && !nq) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < list->count; i++) {
        nq[i].path = strdup(list->items[i]);
        nq[i].played = false;
        if (!nq[i].path) {
            for (int j = 0; j < i; j++) {
                free(nq[j].path);
            }
            free(nq);
            return ESP_ERR_NO_MEM;
        }
    }
    LOCK();
    bool had_current = find_pos_locked(s_cur_id) >= 0;
    for (int i = 0; i < s_qlen; i++) {
        free(s_queue[i].path);
    }
    free(s_queue);
    s_queue = nq;
    s_qlen = s_qcap = list->count;
    for (int i = 0; i < s_qlen; i++) {
        s_queue[i].id = s_next_id++;
    }
    s_removed_pos = -1;
    queue_changed_locked();
    UNLOCK();
    s_dsp_reset = true;
    if (had_current) {
        post(CMD_CURRENT_REMOVED, 0, 0, false);
    }
    return ESP_OK;
}

bool player_queue_rename(const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to);
    bool changed = false;
    LOCK();
    for (int i = 0; i < s_qlen; i++) {
        const char *p = s_queue[i].path;
        if (strncmp(p, from, fl) == 0 && (p[fl] == '\0' || p[fl] == '/')) {
            char *np = malloc(tl + strlen(p + fl) + 1);
            if (np) {
                memcpy(np, to, tl);
                strcpy(np + tl, p + fl);
                free(s_queue[i].path);
                s_queue[i].path = np;
                changed = true;
            }
        }
    }
    if (changed) {
        queue_changed_locked();
    }
    UNLOCK();
    return changed;
}

esp_err_t player_play(int pos)
{
    return post(CMD_PLAY_POS, pos, 0, true);
}

esp_err_t player_play_id(uint32_t id)
{
    if (player_queue_pos_of_id(id) < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return post(CMD_PLAY_ID, 0, id, true);
}

esp_err_t player_pause(int mode)
{
    return post(CMD_PAUSE, mode, 0, true);
}

esp_err_t player_stop(void)
{
    return post(CMD_STOP, 0, 0, true);
}

esp_err_t player_next(void)
{
    return post(CMD_NEXT, 0, 0, true);
}

esp_err_t player_previous(void)
{
    return post(CMD_PREV, 0, 0, true);
}

esp_err_t player_seek(int pos, uint32_t ms)
{
    LOCK();
    uint32_t id;
    if (pos < 0) {
        id = s_state != PLAYER_STOPPED ? s_cur_id : 0;
    } else {
        id = pos < s_qlen ? s_queue[pos].id : 0;
    }
    bool current = id && id == s_cur_id && s_state != PLAYER_STOPPED;
    bool seekable = s_seekable;
    UNLOCK();
    if (!id) {
        return ESP_ERR_INVALID_ARG;
    }
    if (current && !seekable && ms > 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return post(CMD_SEEK, (int)ms, id, true);
}

void player_clear_error(void)
{
    LOCK();
    bool had = s_error[0] != '\0';
    s_error[0] = '\0';
    UNLOCK();
    if (had) {
        changes_notify(CHG_PLAYER);
    }
}

void player_beep(beep_t beep)
{
    post(CMD_BEEP, beep, 0, false);
}

void player_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    LOCK();
    if (volume > s_max_volume) {
        volume = s_max_volume;
    }
    bool changed = s_volume != volume;
    s_volume = volume;
    update_gain_locked();
    UNLOCK();
    if (changed) {
        changes_notify(CHG_MIXER);
        settings_set_volume_deferred((uint8_t)volume);
    }
}

int player_get_volume(void)
{
    LOCK();
    int v = s_volume;
    UNLOCK();
    return v;
}

void player_set_max_volume(uint8_t max_volume)
{
    LOCK();
    s_max_volume = max_volume && max_volume <= 100 ? max_volume : 100;
    bool lowered = s_volume > s_max_volume;
    if (lowered) {
        s_volume = s_max_volume;
    }
    int v = s_volume;
    update_gain_locked();
    UNLOCK();
    if (lowered) {
        settings_set_volume_deferred((uint8_t)v);
    }
    changes_notify(CHG_MIXER);
}

int player_get_max_volume(void)
{
    LOCK();
    int v = s_max_volume;
    UNLOCK();
    return v;
}

void player_set_sound(uint8_t normalize, uint8_t compress)
{
    s_normalize = normalize > DSP_LEVEL_MAX ? DSP_LEVEL_MAX : normalize;
    s_compress = compress > DSP_LEVEL_MAX ? DSP_LEVEL_MAX : compress;
}

void player_get_sound(uint8_t *normalize, uint8_t *compress)
{
    *normalize = s_normalize;
    *compress = s_compress;
}

#define SET_OPTION(var, value)        \
    do {                              \
        LOCK();                       \
        bool changed = (var) != (value); \
        (var) = (value);              \
        UNLOCK();                     \
        if (changed) {                \
            changes_notify(CHG_OPTIONS); \
        }                             \
    } while (0)

void player_set_repeat(bool on)
{
    SET_OPTION(s_repeat, on);
}

void player_set_random(bool on)
{
    LOCK();
    for (int i = 0; i < s_qlen; i++) {
        s_queue[i].played = false;
    }
    UNLOCK();
    SET_OPTION(s_random, on);
}

void player_set_single(uint8_t mode)
{
    SET_OPTION(s_single, mode > 2 ? 1 : mode);
}

void player_set_consume(bool on)
{
    SET_OPTION(s_consume, on);
}
