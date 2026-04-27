#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * Инициализация микрофона и динамика (ESP32-S3 I2S).
 * Waveshare ESP32-S3 1.85" использует встроенный codec.
 */
esp_err_t audio_init(void);

/**
 * Начать запись в буфер.
 * Пишет пока не наступит тишина или не кончится буфер.
 * Возвращает количество записанных байт (PCM 16kHz 16-bit mono).
 */
size_t audio_record(int16_t *buf, size_t max_samples);

/**
 * Играть MP3 данные из буфера.
 * data — указатель на mp3 байты, len — размер.
 * Блокирует пока не доиграет.
 */
esp_err_t audio_play_mp3(const uint8_t *data, size_t len);

/**
 * Играть MP3 чанками (для стриминга).
 * Вызывается повторно по мере прихода данных.
 * is_last — последний чанк, после него декодер сбрасывается.
 */
esp_err_t audio_play_mp3_chunk(const uint8_t *chunk, size_t len, bool is_last);

/**
 * RMS энергия буфера — для VAD.
 */
float audio_rms(const int16_t *buf, size_t samples);

/**
 * Остановить воспроизведение.
 */
void audio_stop(void);
