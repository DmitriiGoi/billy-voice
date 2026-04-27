import os
import asyncio
import logging
import numpy as np
import io
import wave
import urllib.request
import json as json_lib
from fastapi import FastAPI, UploadFile, File, HTTPException
from fastapi.responses import StreamingResponse
from faster_whisper import WhisperModel
from dotenv import load_dotenv

load_dotenv()

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("billy-voice")

app = FastAPI(title="Billy Voice")

# Config
WHISPER_MODEL = os.getenv("WHISPER_MODEL", "small")
WHISPER_LANGUAGE = os.getenv("WHISPER_LANGUAGE", "ru")
OPENCLAW_SESSION_KEY = os.getenv("OPENCLAW_SESSION_KEY", "billy-voice")
KOKORO_URL = os.getenv("KOKORO_URL", "http://kokoro-tts.agent-sandbox.svc.cluster.local:8880")
KOKORO_VOICE = os.getenv("KOKORO_VOICE", "af_heart")

SAMPLE_RATE = 16000
SAMPLE_WIDTH = 2  # 16-bit

# Lazy-loaded model
_whisper: WhisperModel | None = None


def get_whisper() -> WhisperModel:
    global _whisper
    if _whisper is None:
        log.info(f"Loading Whisper model '{WHISPER_MODEL}'...")
        _whisper = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
        log.info("Whisper model loaded.")
    return _whisper


def transcribe_sync(audio_bytes: bytes) -> str:
    """Run faster-whisper STT. audio_bytes = raw PCM 16kHz 16-bit mono."""
    model = get_whisper()
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(audio_bytes)
    buf.seek(0)
    segments, _ = model.transcribe(buf, language=WHISPER_LANGUAGE, beam_size=5)
    return " ".join(s.text.strip() for s in segments).strip()


OPENCLAW_GATEWAY_URL = os.getenv("OPENCLAW_GATEWAY_URL", "http://10.43.4.151:18789")
OPENCLAW_GATEWAY_TOKEN = os.getenv("OPENCLAW_GATEWAY_TOKEN", "")


def ask_billy_sync(text: str) -> str:
    """Send text to OpenClaw via WebSocket protocol, return reply text."""
    import websocket
    import threading

    ws_url = OPENCLAW_GATEWAY_URL.replace("http://", "ws://").replace("https://", "wss://")
    token = OPENCLAW_GATEWAY_TOKEN
    session_key = OPENCLAW_SESSION_KEY

    reply_text = [None]
    error_text = [None]
    done = threading.Event()
    connect_nonce = [None]
    req_id = ["req-1"]

    def on_message(ws, message):
        try:
            msg = json_lib.loads(message)
            evt = msg.get("event") or msg.get("type")

            if evt == "connect.challenge":
                nonce = msg["payload"]["nonce"]
                connect_nonce[0] = nonce
                # Auth with token
                ws.send(json_lib.dumps({
                    "type": "method",
                    "method": "connect",
                    "id": "auth-1",
                    "payload": {
                        "auth": {"token": token},
                        "role": "operator",
                        "scopes": ["agent:run"],
                        "nonce": nonce,
                    }
                }))

            elif msg.get("id") == "auth-1" and msg.get("type") == "response":
                # Auth OK — send message
                ws.send(json_lib.dumps({
                    "type": "method",
                    "method": "agent.run",
                    "id": req_id[0],
                    "payload": {
                        "sessionKey": session_key,
                        "message": text,
                    }
                }))

            elif msg.get("id") == req_id[0] and msg.get("type") == "response":
                result = msg.get("payload") or {}
                reply_text[0] = result.get("reply") or result.get("text") or result.get("message") or str(result)
                done.set()
                ws.close()

            elif msg.get("type") == "error" or (msg.get("type") == "response" and msg.get("error")):
                error_text[0] = str(msg.get("error") or msg)
                done.set()
                ws.close()

        except Exception as e:
            log.error(f"ws parse error: {e}")

    def on_error(ws, error):
        error_text[0] = str(error)
        done.set()

    def on_close(ws, *args):
        done.set()

    ws = websocket.WebSocketApp(
        ws_url,
        header={"Authorization": f"Bearer {token}"},
        on_message=on_message,
        on_error=on_error,
        on_close=on_close,
    )
    t = threading.Thread(target=ws.run_forever)
    t.daemon = True
    t.start()
    done.wait(timeout=60)

    if error_text[0]:
        log.error(f"openclaw ws error: {error_text[0]}")
        return "Чёт я завис, бро. Попробуй ещё раз."
    return reply_text[0] or "Пустой ответ от Билли."


def stream_tts(text: str):
    """Generator: yields MP3 chunks from Kokoro as they arrive."""
    url = f"{KOKORO_URL}/v1/audio/speech"
    payload = json_lib.dumps({
        "model": "kokoro",
        "input": text,
        "voice": KOKORO_VOICE,
        "response_format": "mp3",
        "speed": 1.0,
    }).encode()
    req = urllib.request.Request(
        url, data=payload,
        headers={"Content-Type": "application/json"}
    )
    chunk_size = 4096  # 4KB chunks
    with urllib.request.urlopen(req, timeout=30) as resp:
        while True:
            chunk = resp.read(chunk_size)
            if not chunk:
                break
            yield chunk


@app.get("/health")
async def health():
    return {"status": "ok", "service": "billy-voice"}


@app.post("/voice")
async def voice(file: UploadFile = File(...)):
    """
    Accepts a WAV file (16kHz, 16-bit, mono PCM).
    Returns streaming MP3 audio response (Билли's voice reply).
    """
    raw = await file.read()

    # Strip WAV header if present, extract raw PCM
    try:
        buf = io.BytesIO(raw)
        with wave.open(buf, "rb") as wf:
            pcm_bytes = wf.readframes(wf.getnframes())
        log.info(f"Received WAV: {len(pcm_bytes)} PCM bytes")
    except Exception:
        # Assume raw PCM if not valid WAV
        pcm_bytes = raw
        log.info(f"Received raw PCM: {len(pcm_bytes)} bytes")

    if len(pcm_bytes) < SAMPLE_RATE * SAMPLE_WIDTH * 0.3:
        raise HTTPException(status_code=400, detail="Audio too short")

    # Run STT + LLM in thread pool (blocking ops)
    loop = asyncio.get_event_loop()

    log.info("Running STT...")
    transcript = await loop.run_in_executor(None, transcribe_sync, pcm_bytes)
    log.info(f"Transcript: '{transcript}'")

    if not transcript:
        raise HTTPException(status_code=204, detail="No speech detected")

    log.info("Asking Billy...")
    reply = await loop.run_in_executor(None, ask_billy_sync, transcript)
    log.info(f"Reply: '{reply[:100]}'")

    log.info("Streaming TTS...")
    return StreamingResponse(
        stream_tts(reply),
        media_type="audio/mpeg",
        headers={"X-Transcript": transcript[:200]},  # debug: ESP32 can log this
    )
