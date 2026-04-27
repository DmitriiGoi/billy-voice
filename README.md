# billy-voice 🎤

Голосовой пайплайн: ESP32 → STT → Билли → TTS → ESP32

## Архитектура

```
ESP32 (wake word "алло хуесос")
  → WebSocket ws://<node-ip>:30880/ws/voice
  → PCM 16kHz 16-bit mono chunks
  → VAD (silence detection)
  → faster-whisper STT
  → OpenClaw API (session: billy-voice)
  → ElevenLabs TTS
  → MP3 bytes обратно на ESP32
```

## Деплой

### 1. Узнать внутреннее имя OpenClaw сервиса

```bash
kubectl get svc -n personal-agent
```

Обновить `OPENCLAW_API_URL` в `k8s/configmap.yaml` если нужно.

### 2. Заполнить секреты

```bash
# Отредактировать k8s/secret.yaml — вставить реальные ключи
# ELEVENLABS_API_KEY и ELEVENLABS_VOICE_ID
```

### 3. Собрать Docker образ (на хост-ноде k3s)

```bash
cd billy-voice/server
docker build -t billy-voice:latest .
# Для k3s — импортировать образ:
docker save billy-voice:latest | k3s ctr images import -
```

### 4. Применить манифесты

```bash
kubectl apply -f k8s/pvc.yaml
kubectl apply -f k8s/configmap.yaml
kubectl apply -f k8s/secret.yaml
kubectl apply -f k8s/deployment.yaml
kubectl apply -f k8s/service.yaml
```

### 5. Проверить

```bash
kubectl get pods -n personal-agent -l app=billy-voice
kubectl logs -n personal-agent -l app=billy-voice -f

# Health check
curl http://<node-ip>:30880/health
```

## WebSocket протокол (для прошивки ESP32)

**Клиент → Сервер:** сырые PCM байты (16kHz, 16-bit, mono)
- Отправлять чанками по ~640 байт (20ms)
- Непрерывно после wake word

**Сервер → Клиент:**
- `EMPTY` (text) — тишина, речь не распознана
- `ERROR: <msg>` (text) — ошибка пайплайна
- `<4 bytes big-endian length><MP3 bytes>` (binary) — аудио ответ

## Переменные окружения

| Переменная | Дефолт | Описание |
|---|---|---|
| WHISPER_MODEL | small | Модель Whisper (tiny/base/small/medium) |
| WHISPER_LANGUAGE | ru | Язык STT |
| OPENCLAW_API_URL | http://localhost:3000 | URL OpenClaw |
| OPENCLAW_SESSION_KEY | billy-voice | Ключ сессии |
| SILENCE_THRESHOLD | 300 | RMS порог тишины |
| SILENCE_DURATION | 1.5 | Секунд тишины для окончания речи |
| ELEVENLABS_API_KEY | — | ElevenLabs API ключ |
| ELEVENLABS_VOICE_ID | — | ID голоса ElevenLabs |
