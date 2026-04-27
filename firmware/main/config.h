#pragma once

// ── WiFi ──────────────────────────────────────────────────────────────────
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

// ── Billy Voice Server ────────────────────────────────────────────────────
// NodePort 30880, хост-нода кластера
#define SERVER_HOST      "192.168.178.33"
#define SERVER_PORT      30880
#define SERVER_URL       "http://" SERVER_HOST ":" "30880" "/voice"

// ── Audio ─────────────────────────────────────────────────────────────────
#define SAMPLE_RATE      16000
#define BITS_PER_SAMPLE  16
#define CHANNELS         1
#define RECORD_BUF_MS    30000   // максимум 30 сек записи
#define SILENCE_THRESH   500     // RMS порог тишины (ADC units)
#define SILENCE_MS       1500    // мс тишины для стопа записи
#define MIN_SPEECH_MS    300     // минимум речи чтоб не слать шум

// ── Wake Word ─────────────────────────────────────────────────────────────
// ESP-SR WakeNet — используем встроенную "Hi ESP" пока нет кастомной модели
// После обучения заменить на кастомную "алло хуесос"
#define WAKE_WORD_MODEL  "wn9_hilexin"

// ── Display ───────────────────────────────────────────────────────────────
#define LCD_WIDTH        360
#define LCD_HEIGHT       360
