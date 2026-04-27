#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * Отправить WAV на сервер, получить стримингом MP3 и сразу играть.
 */
esp_err_t http_voice_send(const int16_t *pcm_buf, size_t samples);
