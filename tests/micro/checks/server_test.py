"""Проверка WAV из C++ через настоящий HTTP-сервер Uvicorn."""
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError
import io
import json
import shutil
import socket
import struct
import subprocess
import sys
import time
import unittest
import uuid
import wave

ROOT = Path(__file__).resolve().parents[1]


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = ROOT / ".build" / ("server-test-" + uuid.uuid4().hex)
        cls.folder.mkdir(parents=True)
        shutil.copyfile(ROOT / "server" / "main.py", cls.folder / "main.py")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        cls.url = f"http://127.0.0.1:{port}"
        cls.log = (cls.folder / "server.log").open("w", encoding="utf-8")
        cls.process = subprocess.Popen(
            [sys.executable, "-m", "uvicorn", "main:app", "--host", "127.0.0.1", "--port", str(port)],
            cwd=cls.folder, stdout=cls.log, stderr=subprocess.STDOUT,
        )
        for _ in range(300):
            try:
                with urlopen(cls.url + "/health", timeout=0.5) as response:
                    if response.status == 200:
                        return
            except (URLError, TimeoutError):
                if cls.process.poll() is not None:
                    break
                time.sleep(0.1)
        cls.process.terminate()
        cls.process.wait(timeout=5)
        cls.log.close()
        raise RuntimeError((cls.folder / "server.log").read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        cls.process.wait(timeout=5)
        cls.log.close()

    def post(self, body, content_type="audio/wav"):
        request = Request(self.url + "/voice", data=body, headers={"Content-Type": content_type})
        try:
            with urlopen(request, timeout=5) as response:
                return response.status, json.load(response)
        except HTTPError as response:
            with response:
                return response.code, json.load(response)

    def wav(self, frames=3200, channels=1):
        output = io.BytesIO()
        with wave.open(output, "wb") as audio:
            audio.setnchannels(channels)
            audio.setsampwidth(2)
            audio.setframerate(16000)
            audio.writeframes(b"\x00\x00" * frames * channels)
        return output.getvalue()

    def test_cpp_wav_round_trip(self):
        body = (ROOT / ".build" / "fixture.wav").read_bytes()
        with wave.open(io.BytesIO(body), "rb") as audio:
            self.assertEqual((audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getnframes()), (1, 2, 16000, 16000))
            samples = struct.unpack("<32h", audio.readframes(32))
            self.assertEqual(samples, (4000,) * 16 + (-4000,) * 16)
        status, result = self.post(body)
        self.assertEqual(status, 200)
        self.assertEqual(result["seconds"], 1.0)
        self.assertEqual((self.folder / "recordings" / result["file"]).read_bytes(), body)
        status, second = self.post(body)
        self.assertEqual(status, 200)
        self.assertNotEqual(result["file"], second["file"])

    def test_duration_boundaries(self):
        for frames, expected in ((3199, 400), (3200, 200), (480000, 200), (480001, 400)):
            with self.subTest(frames=frames):
                self.assertEqual(self.post(self.wav(frames))[0], expected)

    def test_invalid_audio(self):
        before = set((self.folder / "recordings").glob("*.wav"))
        for body in (b"not a wav", self.wav()[:-2], self.wav(channels=2)):
            self.assertEqual(self.post(body)[0], 400)
        self.assertEqual(self.post(b"{}", "multipart/form-data")[0], 415)
        self.assertEqual(self.post(b"x" * 1_000_001)[0], 413)
        self.assertEqual(before, set((self.folder / "recordings").glob("*.wav")))

    def test_legacy_json_and_tasks(self):
        data = {"task": "server-test", "time": "12:00"}
        self.assertEqual(self.post(json.dumps(data).encode(), "application/json")[0], 200)
        with urlopen(self.url + "/tasks", timeout=5) as response:
            tasks = json.load(response)["tasks"]
        self.assertEqual(tasks[0]["task"], data["task"])
        self.assertEqual(self.post(b"[]", "application/json")[0], 400)


if __name__ == "__main__":
    unittest.main(verbosity=2)
