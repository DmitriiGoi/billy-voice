#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// States машины состояний
typedef enum {
    STATE_IDLE,        // ждём wake word
    STATE_LISTENING,   // пишем речь
    STATE_PROCESSING,  // шлём на сервер, ждём ответа
    STATE_SPEAKING,    // играем ответ
    STATE_ERROR,       // что-то пошло по пизде
} billy_state_t;

// Events между задачами
typedef enum {
    EVT_WAKE_DETECTED,
    EVT_SPEECH_DONE,
    EVT_REPLY_READY,
    EVT_PLAYBACK_DONE,
    EVT_ERROR,
} billy_event_t;

// Audio буфер записи
typedef struct {
    int16_t *data;
    size_t   samples;   // реальное количество записанных сэмплов
    size_t   capacity;  // max сэмплов
} audio_buf_t;

void billy_state_machine_init(void);
void billy_post_event(billy_event_t evt, void *data, size_t data_len);
billy_state_t billy_get_state(void);
