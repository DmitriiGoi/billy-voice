#pragma once

/**
 * Wake word detection через ESP-SR WakeNet.
 * Работает в фоновой задаче, постит EVT_WAKE_DETECTED в state machine.
 */

#include "esp_err.h"

esp_err_t wake_word_init(void);
void      wake_word_start(void);
void      wake_word_stop(void);
