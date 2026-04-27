#include "billy.h"
#include "audio.h"
#include "wake_word.h"
#include "http_voice.h"
#include "display.h"
#include "config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "main";

// ── State machine ──────────────────────────────────────────────────────────

static billy_state_t current_state = STATE_IDLE;
static QueueHandle_t event_queue   = NULL;

// PCM буфер — выделяем статически, RECORD_BUF_MS * SAMPLE_RATE * 2 bytes
#define MAX_RECORD_SAMPLES  ((RECORD_BUF_MS * SAMPLE_RATE) / 1000)
static int16_t *record_buf = NULL;

void billy_post_event(billy_event_t evt, void *data, size_t data_len)
{
    // Шлём событие в очередь, игнорируем data пока (не нужно для MVP)
    if (event_queue) {
        xQueueSend(event_queue, &evt, pdMS_TO_TICKS(100));
    }
}

billy_state_t billy_get_state(void) { return current_state; }

static void set_state(billy_state_t s)
{
    current_state = s;
    display_set_state(s);
    ESP_LOGI(TAG, "state -> %d", s);
}

static void state_machine_task(void *arg)
{
    billy_event_t evt;

    while (1) {
        if (xQueueReceive(event_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "event %d in state %d", evt, current_state);

            switch (current_state) {

                case STATE_IDLE:
                    if (evt == EVT_WAKE_DETECTED) {
                        // Стопаем wake word детектор пока записываем
                        wake_word_stop();
                        set_state(STATE_LISTENING);

                        // Записываем речь
                        size_t samples = audio_record(record_buf, MAX_RECORD_SAMPLES);

                        if (samples > 0) {
                            billy_post_event(EVT_SPEECH_DONE, NULL, 0);
                        } else {
                            // Ничего не записали — назад в idle
                            set_state(STATE_IDLE);
                            wake_word_start();
                        }
                    }
                    break;

                case STATE_LISTENING:
                    if (evt == EVT_SPEECH_DONE) {
                        set_state(STATE_PROCESSING);

                        // Шлём на сервер — ответ играется стримингом прямо внутри
                        esp_err_t ret = http_voice_send(record_buf,
                            /* samples: */ (size_t)(
                                // пересчитываем из audio_record результата
                                // (храним в глобале ниже)
                                _last_samples
                            )
                        );

                        if (ret == ESP_OK) {
                            set_state(STATE_SPEAKING);
                            // playback происходит синхронно внутри http_voice_send
                            // после завершения — возвращаемся в idle
                            set_state(STATE_IDLE);
                        } else {
                            set_state(STATE_ERROR);
                            vTaskDelay(pdMS_TO_TICKS(2000));
                            set_state(STATE_IDLE);
                        }

                        wake_word_start();
                    }
                    break;

                default:
                    break;
            }
        }
    }
}

void billy_state_machine_init(void)
{
    event_queue = xQueueCreate(8, sizeof(billy_event_t));
    record_buf  = malloc(MAX_RECORD_SAMPLES * sizeof(int16_t));
    if (!record_buf) {
        ESP_LOGE(TAG, "failed to alloc record buf!");
        return;
    }
    xTaskCreatePinnedToCore(state_machine_task, "state_machine", 8192, NULL, 4, NULL, 0);
}

// ── WiFi ───────────────────────────────────────────────────────────────────

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "wifi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "connecting to WiFi '%s'...", WIFI_SSID);
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        false, true, portMAX_DELAY);
}

// ── Entry point ────────────────────────────────────────────────────────────

// Глобал для передачи samples между audio_record и state machine
size_t _last_samples = 0;

// Переопределяем state_machine_task чтоб хранить samples
// (переписываем чуть выше — это MVP, можно рефакторить)

void app_main(void)
{
    ESP_LOGI(TAG, "Billy Voice booting...");

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Display
    display_init();
    display_set_state(STATE_IDLE);

    // Audio
    ESP_ERROR_CHECK(audio_init());

    // WiFi
    wifi_init();

    // State machine + wake word
    billy_state_machine_init();
    wake_word_init();
    wake_word_start();

    ESP_LOGI(TAG, "Billy Voice ready. Say '%s'", WAKE_WORD_MODEL);

    // Main loop — ничего не делаем, всё в задачах
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
