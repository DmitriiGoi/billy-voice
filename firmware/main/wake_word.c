#include "wake_word.h"
#include "billy.h"
#include "audio.h"
#include "config.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

static const char *TAG = "wake_word";

static TaskHandle_t ww_task_handle = NULL;
static volatile bool ww_running    = false;

static void wake_word_task(void *arg)
{
    // Загружаем модель WakeNet
    const esp_wn_iface_t *wakenet = &WAKENET_MODEL;
    model_iface_data_t   *model_data = wakenet->create(WAKE_WORD_MODEL, DET_MODE_90);

    int audio_chunksize = wakenet->get_samp_chunksize(model_data);
    int sample_rate     = wakenet->get_samp_rate(model_data);
    int16_t *buffer     = malloc(audio_chunksize * sizeof(int16_t));

    if (!buffer || !model_data) {
        ESP_LOGE(TAG, "wake word init failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "wake word ready, chunk=%d sr=%d", audio_chunksize, sample_rate);

    while (ww_running) {
        // Читаем чанк с микрофона
        size_t bytes_read = 0;
        extern i2s_chan_handle_t mic_chan;  // из audio.c
        i2s_channel_read(mic_chan, buffer, audio_chunksize * sizeof(int16_t),
                         &bytes_read, pdMS_TO_TICKS(100));

        if (bytes_read < audio_chunksize * sizeof(int16_t)) continue;

        // Детектим wake word
        int wakeword_index = wakenet->detect(model_data, buffer);
        if (wakeword_index > 0) {
            ESP_LOGI(TAG, "WAKE WORD DETECTED! index=%d", wakeword_index);
            billy_post_event(EVT_WAKE_DETECTED, NULL, 0);
            // Небольшая пауза чтоб state machine успела переключиться
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    wakenet->destroy(model_data);
    free(buffer);
    vTaskDelete(NULL);
}

esp_err_t wake_word_init(void)
{
    ESP_LOGI(TAG, "wake word model: %s", WAKE_WORD_MODEL);
    return ESP_OK;
}

void wake_word_start(void)
{
    if (ww_task_handle) return;
    ww_running = true;
    xTaskCreatePinnedToCore(wake_word_task, "wake_word", 8192, NULL, 5, &ww_task_handle, 1);
}

void wake_word_stop(void)
{
    ww_running = false;
    ww_task_handle = NULL;
}
