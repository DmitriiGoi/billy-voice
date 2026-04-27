# Firmware — Billy Voice ESP32-S3

## Зависимости

- ESP-IDF v5.2+
- ESP-SR (wake word) — входит в IDF компоненты
- minimp3 — header-only, нужно скачать в `main/`

```bash
# Скачать minimp3
curl -o main/minimp3.h https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h
```

## Сборка

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Конфиг

Отредактировать `main/config.h`:
- `WIFI_SSID` / `WIFI_PASSWORD`
- `SERVER_HOST` — IP хост-ноды k8s (где висит NodePort 30880)

## Структура

```
main/
  main.c        — app_main, WiFi, state machine
  audio.c/h     — I2S mic + speaker, VAD, MP3 decode (minimp3)
  wake_word.c/h — ESP-SR WakeNet детектор
  http_voice.c/h — POST WAV → GET streaming MP3
  display.c/h   — GC9A01 LCD, цветовая индикация состояний
  config.h      — все настройки
  billy.h       — типы, state machine API
```

## Состояния (цвета экрана)

| Состояние   | Цвет    | Смысл                    |
|-------------|---------|--------------------------|
| IDLE        | ⚫ чёрный | ждём "алло хуесос"      |
| LISTENING   | 🟢 зелёный | пишем речь              |
| PROCESSING  | 🟡 жёлтый | сервер думает           |
| SPEAKING    | 🔵 синий  | играем ответ Билли      |
| ERROR       | 🔴 красный | что-то пошло не так    |

## TODO

- [ ] Обучить кастомный wake word "алло хуесос" через ESP-SR WakeNet tool
- [ ] Заменить `WAKE_WORD_MODEL` на кастомную модель
- [ ] Пины I2S уточнить по схеме Waveshare ESP32-S3 1.85"
