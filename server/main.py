import os
import asyncio
import logging
import subprocess
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


def ask_billy_sync(text: str) -> str:
    """Send text to OpenClaw via CLI, return reply text."""
    result = subprocess.run(
        ["openclaw", "agent",
         "--session-id", OPENCLAW_SESSION_KEY,
         "--message", text,
         "--json"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    if result.returncode != 0:
        log.error(f"openclaw agent failed: {result.stderr}")
        return "Чёт я завис, бро. Попробуй ещё раз."
    try:
        data = json_lib.loads(result.stdout)
        return data.get("reply") or data.get("text") or data.get("message") or result.stdout.strip()
    except Exception:
        return result.stdout.strip() or "Пустой ответ от Билли."


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
