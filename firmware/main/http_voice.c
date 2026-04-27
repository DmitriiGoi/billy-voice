#include "http_voice.h"
#include "audio.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http_voice";

// WAV header — 44 байта
static void write_wav_header(uint8_t *hdr, uint32_t pcm_bytes)
{
    uint32_t data_size    = pcm_bytes;
    uint32_t file_size    = data_size + 36;
    uint32_t byte_rate    = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
    uint16_t block_align  = CHANNELS * (BITS_PER_SAMPLE / 8);

    memcpy(hdr,      "RIFF", 4);
    memcpy(hdr + 4,  &file_size,    4);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_size = 16; memcpy(hdr + 16, &fmt_size, 4);
    uint16_t audio_fmt = 1; memcpy(hdr + 20, &audio_fmt, 2);
    uint16_t channels  = CHANNELS; memcpy(hdr + 22, &channels, 2);
    uint32_t sr        = SAMPLE_RATE; memcpy(hdr + 24, &sr, 4);
    memcpy(hdr + 28, &byte_rate,    4);
    memcpy(hdr + 30, &block_align,  2);
    uint16_t bps = BITS_PER_SAMPLE; memcpy(hdr + 32, &bps, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_size,    4);
}

// Данные для multipart стриминга
typedef struct {
    const uint8_t *wav_header;
    const uint8_t *pcm_data;
    size_t         pcm_bytes;
    size_t         sent;       // сколько уже отправлено
} upload_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Получаем MP3 чанки — сразу играем!
            if (evt->data_len > 0) {
                bool is_last = false;  // узнаем точно только по завершению
                audio_play_mp3_chunk((const uint8_t *)evt->data, evt->data_len, is_last);
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            // Сигналим что последний чанк
            audio_play_mp3_chunk(NULL, 0, true);
            ESP_LOGI(TAG, "response complete");
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "http error");
            break;

        default:
            break;
    }
    return ESP_OK;
}

esp_err_t http_voice_send(const int16_t *pcm_buf, size_t samples)
{
    size_t pcm_bytes = samples * sizeof(int16_t);

    // Собираем WAV заголовок
    uint8_t wav_hdr[44];
    write_wav_header(wav_hdr, (uint32_t)pcm_bytes);

    // Собираем полный WAV в памяти (заголовок + PCM)
    // Для больших буферов можно оптимизировать через write callback
    size_t   total   = sizeof(wav_hdr) + pcm_bytes;
    uint8_t *wav_buf = malloc(total);
    if (!wav_buf) {
        ESP_LOGE(TAG, "no memory for WAV buf (%u bytes)", (unsigned)total);
        return ESP_ERR_NO_MEM;
    }
    memcpy(wav_buf, wav_hdr, sizeof(wav_hdr));
    memcpy(wav_buf + sizeof(wav_hdr), pcm_buf, pcm_bytes);

    // HTTP POST
    esp_http_client_config_t cfg = {
        .url            = SERVER_URL,
        .method         = HTTP_METHOD_POST,
        .event_handler  = http_event_handler,
        .timeout_ms     = 30000,
        .buffer_size    = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(wav_buf);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, (const char *)wav_buf, (int)total);

    ESP_LOGI(TAG, "POST %s (%u bytes WAV)", SERVER_URL, (unsigned)total);
    esp_err_t ret = esp_http_client_perform(client);

    if (ret == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status: %d", status);
        if (status != 200) {
            ESP_LOGW(TAG, "server returned %d", status);
            ret = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "http perform failed: %s", esp_err_to_name(ret));
    }

    esp_http_client_cleanup(client);
    free(wav_buf);
    return ret;
}
