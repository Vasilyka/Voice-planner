"""Приём WAV от ESP32. Запуск: py -m uvicorn main:app --host 0.0.0.0 --port 8000."""
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path
from uuid import uuid4
import io
import json
import sqlite3
import wave

from fastapi import FastAPI, HTTPException, Request

BASE_DIR = Path(__file__).resolve().parent
DB_PATH = BASE_DIR / "tasks.db"
RECORDINGS_DIR = BASE_DIR / "recordings"
MAX_BODY_BYTES = 1_000_000  # WAV 30 с, mono, PCM16, 16 кГц = 960044 байта.


def init_db():
    with sqlite3.connect(DB_PATH) as connection:
        connection.execute("""
            CREATE TABLE IF NOT EXISTS tasks (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                task TEXT NOT NULL,
                time TEXT,
                created_at TEXT NOT NULL
            )
        """)


@asynccontextmanager
async def lifespan(app):
    init_db()
    RECORDINGS_DIR.mkdir(parents=True, exist_ok=True)
    yield


app = FastAPI(lifespan=lifespan)


@app.get("/health")
def health():
    return {"status": "ok", "audio": "WAV PCM16 mono 16000 Hz"}


async def read_limited_body(request):
    body = bytearray()
    async for chunk in request.stream():
        if len(body) + len(chunk) > MAX_BODY_BYTES:
            raise HTTPException(413, "Запись слишком большая: максимум 30 секунд.")
        body.extend(chunk)
    return bytes(body)


def validate_wav(body):
    try:
        with wave.open(io.BytesIO(body), "rb") as audio:
            if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getcomptype()) != (1, 2, 16000, "NONE"):
                raise HTTPException(400, "Нужен WAV: PCM16, mono, 16000 Hz.")
            frames = audio.getnframes()
            if frames < 3200 or frames > 480000:
                raise HTTPException(400, "Длительность записи должна быть от 0.2 до 30 секунд.")
            if len(audio.readframes(frames)) != frames * 2:
                raise HTTPException(400, "Неполный WAV: аудиоданные обрезаны.")
            return frames / 16000
    except (wave.Error, EOFError, ValueError) as error:
        raise HTTPException(400, "Некорректный WAV.") from error


@app.post("/voice")
async def voice(request: Request):
    content_type = request.headers.get("content-type", "").split(";")[0].strip().lower()
    if content_type not in {"audio/wav", "audio/x-wav", "application/json"}:
        raise HTTPException(415, "Отправляй WAV в теле POST с Content-Type: audio/wav.")
    body = await read_limited_body(request)
    # Сохраняем совместимость со старым скетчем: JSON -> таблица tasks.
    if content_type == "application/json":
        try:
            data = json.loads(body)
        except (ValueError, UnicodeError) as error:
            raise HTTPException(400, "Некорректный JSON.") from error
        if not isinstance(data, dict):
            raise HTTPException(400, "Ожидается JSON-объект.")
        task = data.get("task", "")
        task_time = data.get("time")
        if not isinstance(task, str) or (task_time is not None and not isinstance(task_time, str)):
            raise HTTPException(400, "task и time должны быть строками.")
        with sqlite3.connect(DB_PATH) as connection:
            connection.execute(
                "INSERT INTO tasks (task, time, created_at) VALUES (?, ?, ?)",
                (task, task_time, datetime.now().isoformat()),
            )
        return {"status": "ok", "received": data}

    seconds = validate_wav(body)
    name = datetime.now().strftime("%Y%m%d_%H%M%S_") + uuid4().hex[:12] + ".wav"
    target = RECORDINGS_DIR / name
    with target.open("xb") as output:
        output.write(body)
    print(f"Получена запись: {target}, {seconds:.2f} с", flush=True)
    return {"status": "ok", "file": name, "seconds": seconds, "bytes": len(body)}


@app.get("/tasks")
def tasks():
    with sqlite3.connect(DB_PATH) as connection:
        connection.row_factory = sqlite3.Row
        rows = connection.execute("SELECT * FROM tasks ORDER BY id DESC LIMIT 10").fetchall()
    return {"tasks": [dict(row) for row in rows]}
