#include "audio.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

// Waveshare ESP32-S3 1.85" Round LCD pins (проверить по схеме платы!)
// I2S для микрофона (PDM)
#define MIC_I2S_NUM        I2S_NUM_0
#define MIC_CLK_PIN        GPIO_NUM_42
#define MIC_DATA_PIN       GPIO_NUM_41

// I2S для динамика
#define SPK_I2S_NUM        I2S_NUM_1
#define SPK_BCK_PIN        GPIO_NUM_17
#define SPK_WS_PIN         GPIO_NUM_18
#define SPK_DATA_PIN       GPIO_NUM_16

static const char *TAG = "audio";

static i2s_chan_handle_t mic_chan  = NULL;
static i2s_chan_handle_t spk_chan  = NULL;

// Простой MP3 декодер через minimp3
// Включаем header-only библиотеку
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

static mp3dec_t mp3d;

esp_err_t audio_init(void)
{
    esp_err_t ret;

    // ── Микрофон (PDM mono) ──────────────────────────────────────────────
    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_NUM, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&mic_chan_cfg, NULL, &mic_chan);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "mic channel init failed: %d", ret); return ret; }

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk  = MIC_CLK_PIN,
            .din  = MIC_DATA_PIN,
            .invert_flags = { .clk_inv = false },
        },
    };
    ret = i2s_channel_init_pdm_rx_mode(mic_chan, &pdm_rx_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "mic pdm init failed: %d", ret); return ret; }
    i2s_channel_enable(mic_chan);

    // ── Динамик (стандартный I2S) ────────────────────────────────────────
    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_NUM, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&spk_chan_cfg, &spk_chan, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "spk channel init failed: %d", ret); return ret; }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCK_PIN,
            .ws   = SPK_WS_PIN,
            .dout = SPK_DATA_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(spk_chan, &std_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "spk std init failed: %d", ret); return ret; }
    i2s_channel_enable(spk_chan);

    mp3dec_init(&mp3d);
    ESP_LOGI(TAG, "audio init OK");
    return ESP_OK;
}

float audio_rms(const int16_t *buf, size_t samples)
{
    if (!buf || samples == 0) return 0.0f;
    double sum = 0;
    for (size_t i = 0; i < samples; i++) {
        double s = buf[i];
        sum += s * s;
    }
    return (float)sqrt(sum / samples);
}

size_t audio_record(int16_t *buf, size_t max_samples)
{
    const size_t chunk_samples = (SAMPLE_RATE * 20) / 1000;  // 20ms chunks
    const size_t chunk_bytes   = chunk_samples * sizeof(int16_t);
    const size_t silence_chunks_needed = SILENCE_MS / 20;

    size_t total_samples    = 0;
    size_t silence_chunks   = 0;
    size_t speech_samples   = 0;
    bool   speech_started   = false;

    ESP_LOGI(TAG, "recording...");

    while (total_samples + chunk_samples <= max_samples) {
        size_t bytes_read = 0;
        int16_t *dst = buf + total_samples;
        i2s_channel_read(mic_chan, dst, chunk_bytes, &bytes_read, pdMS_TO_TICKS(100));
        size_t got = bytes_read / sizeof(int16_t);
        if (got == 0) continue;

        float rms = audio_rms(dst, got);
        total_samples += got;

        if (rms > SILENCE_THRESH) {
            speech_started = true;
            speech_samples += got;
            silence_chunks  = 0;
        } else if (speech_started) {
            silence_chunks++;
            if (silence_chunks >= silence_chunks_needed) {
                ESP_LOGI(TAG, "silence detected, stopping. speech=%.1fms",
                         (float)speech_samples * 1000 / SAMPLE_RATE);
                break;
            }
        }
    }

    // Отбрасываем если речи было мало
    size_t min_samples = (SAMPLE_RATE * MIN_SPEECH_MS) / 1000;
    if (speech_samples < min_samples) {
        ESP_LOGW(TAG, "too short (%.0fms), discarding", (float)speech_samples * 1000 / SAMPLE_RATE);
        return 0;
    }

    ESP_LOGI(TAG, "recorded %u samples (%.1f sec)", (unsigned)total_samples,
             (float)total_samples / SAMPLE_RATE);
    return total_samples;
}

esp_err_t audio_play_mp3_chunk(const uint8_t *chunk, size_t len, bool is_last)
{
    static uint8_t  stream_buf[16 * 1024];
    static size_t   stream_len = 0;

    // Добавляем чанк в буфер
    if (stream_len + len > sizeof(stream_buf)) {
        // Сбрасываем накопленное перед добавлением
        ESP_LOGW(TAG, "mp3 stream buf overflow, flushing");
        stream_len = 0;
    }
    memcpy(stream_buf + stream_len, chunk, len);
    stream_len += len;

    // Декодируем и играем всё что можем
    mp3dec_frame_info_t info;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    int offset = 0;
    while (1) {
        int samples = mp3dec_decode_frame(&mp3d, stream_buf + offset,
                                          (int)(stream_len - offset), pcm, &info);
        if (samples <= 0 || info.frame_bytes == 0) break;
        offset += info.frame_bytes;

        size_t bytes_written = 0;
        i2s_channel_write(spk_chan, pcm, samples * sizeof(mp3d_sample_t) * info.channels,
                          &bytes_written, pdMS_TO_TICKS(500));
    }

    // Сдвигаем оставшееся в начало
    if (offset > 0 && offset < (int)stream_len) {
        memmove(stream_buf, stream_buf + offset, stream_len - offset);
        stream_len -= offset;
    } else if (offset >= (int)stream_len) {
        stream_len = 0;
    }

    if (is_last) {
        stream_len = 0;
        mp3dec_init(&mp3d);  // reset decoder
    }

    return ESP_OK;
}

esp_err_t audio_play_mp3(const uint8_t *data, size_t len)
{
    return audio_play_mp3_chunk(data, len, true);
}

void audio_stop(void)
{
    i2s_channel_disable(spk_chan);
    i2s_channel_enable(spk_chan);
}
