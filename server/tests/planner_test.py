"""Проверка цепочки с подменённым облаком. Платных запросов нет."""
from pathlib import Path
import io
import json
import sys
import tempfile
import unittest
import wave
import asyncio
import threading
import sqlite3
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import patch
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cloud import CloudSettings, CloudError, YandexCloud
from planner import Planner, write_json
from job_worker import process_jobs
import cloud as cloud_module
from task_models import TaskBatch
from display_table import render_table

TASKS = {"tasks": [
    {"title": "Сделать лабораторную по физике", "due_date": "2026-11-11", "due_time": "18:00",
     "priority": "normal", "needs_clarification": False, "note": ""},
    {"title": "Купить хлеб", "due_date": None, "due_time": None,
     "priority": "normal", "needs_clarification": False, "note": ""},
    {"title": "Созвониться вечером", "due_date": None, "due_time": "19:00",
     "priority": "high", "needs_clarification": True, "note": "Дата не названа"},
]}


def create_commands(batch):
    return {"commands": [{
        "action": "create", "task": task, "target_title": None, "target_number": None,
        "target_date": None, "target_time": None, "needs_clarification": False, "note": ""
    } for task in batch["tasks"]]}


class PipelineTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.planner = Planner(self.base)
        self.planner.initialize()
        output = io.BytesIO()
        with wave.open(output, "wb") as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(16000)
            audio.writeframes(b"\x01\x00" * 3200)
        (self.base / "recordings/test.wav").write_bytes(output.getvalue())
        self.job = self.planner.create_job("test.wav", True, "hash")
        self.calls = []

    def transport(self, url, body, content_type):
        self.calls.append(url)
        if "speech/v1/" in url:
            self.assertEqual(body, b"\x01\x00" * 3200)
            self.assertIn("sampleRateHertz=16000", url)
            return {"result": "Завтра в 18 сделать лабораторную, купить хлеб и созвониться вечером"}
        payload = json.loads(body)
        self.assertIn(self.job["created_at"], payload["messages"][0]["text"])
        self.assertIn("schema", payload["jsonSchema"])
        return {"result": {"alternatives": [{"status": "ALTERNATIVE_STATUS_FINAL",
                "message": {"text": json.dumps(create_commands(TASKS), ensure_ascii=False)}}]}}

    def test_wav_to_json_and_display(self):
        cloud = YandexCloud(CloudSettings("fake-not-a-key", "folder"), self.transport)
        self.assertTrue(self.planner.process_next(cloud))
        job = self.planner.job(self.job["id"])
        self.assertEqual(job["state"], "done")
        self.assertEqual(len(job["result"]["tasks"]), 3)
        saved = json.loads((self.base / "recordings/test.json").read_text(encoding="utf-8"))
        self.assertEqual(saved, job["result"])
        snapshot = self.planner.snapshot()
        self.assertEqual(snapshot["tasks"][0]["title"], TASKS["tasks"][0]["title"])
        self.assertEqual(json.loads((self.base / "data/planner.json").read_text(encoding="utf-8")), snapshot)
        image, metadata = render_table(snapshot)
        self.assertEqual(image.size, (184, 384))
        self.assertEqual(len(image.tobytes()), 8832)
        self.assertEqual(metadata["count"], 3)
        self.planner.retry(job["id"])
        self.assertFalse(self.planner.process_next(cloud))
        self.assertEqual(len(self.calls), 2)

    def test_reject_bad_calendar_and_extra_fields(self):
        bad = json.loads(json.dumps(TASKS))
        bad["tasks"][0]["due_date"] = "2026-02-30"
        with self.assertRaises(ValueError):
            TaskBatch.model_validate(bad)
        bad = json.loads(json.dumps(TASKS))
        bad["tasks"][0]["execute"] = "remove database"
        with self.assertRaises(ValueError):
            TaskBatch.model_validate(bad)

    def test_failure_retry_reuses_transcript_without_duplicate_tasks(self):
        cloud = YandexCloud(CloudSettings("fake", "folder"), self.transport)
        good = cloud.extract
        cloud.extract = lambda *_: (_ for _ in ()).throw(CloudError("Ошибка модели"))
        self.planner.process_next(cloud)
        self.assertEqual(self.planner.job(self.job["id"])["state"], "failed")
        self.assertEqual(self.planner.snapshot()["tasks"], [])
        cloud.extract = good
        self.planner.retry(self.job["id"])
        self.planner.process_next(cloud)
        self.assertEqual(self.planner.job(self.job["id"])["state"], "done")
        self.assertEqual(sum("speech/v1/" in url for url in self.calls), 1)
        self.assertEqual(len(self.planner.snapshot()["tasks"]), 3)

    def test_truncated_model_result(self):
        def transport(url, *_):
            if "speech/v1/" in url:
                return {"result": "Купить хлеб"}
            return {"alternatives": [{"status": "ALTERNATIVE_STATUS_TRUNCATED_FINAL"}]}
        cloud = YandexCloud(CloudSettings("fake", "folder"), transport)
        self.planner.process_next(cloud)
        self.assertEqual(self.planner.snapshot()["tasks"], [])
        job = self.planner.job(self.job["id"])
        self.assertEqual(job["state"], "failed")
        self.assertIn("не закончила", job["error"])

    def test_empty_transcription_does_not_call_language_model(self):
        calls = []
        def transport(url, *_):
            calls.append(url)
            return {"result": ""}
        self.planner.process_next(YandexCloud(CloudSettings("fake", "folder"), transport))
        self.assertEqual(len(calls), 1)
        self.assertEqual(self.planner.job(self.job["id"])["state"], "failed")
        self.assertEqual(self.planner.snapshot()["tasks"], [])

    def test_invalid_task_rolls_back_whole_batch(self):
        invalid = json.loads(json.dumps(TASKS))
        invalid["tasks"][1]["due_date"] = "2026-02-30"
        def transport(url, *_):
            if "speech/v1/" in url:
                return {"result": "Диктовка из нескольких дел"}
            return {"alternatives": [{"status": "ALTERNATIVE_STATUS_FINAL",
                    "message": {"text": json.dumps(create_commands(invalid))}}]}
        self.planner.process_next(YandexCloud(CloudSettings("fake", "folder"), transport))
        self.assertEqual(self.planner.job(self.job["id"])["state"], "failed")
        self.assertEqual(self.planner.snapshot()["tasks"], [])
        self.assertFalse((self.base / "recordings/test.json").exists())

    def test_pagination_keeps_all_tasks_in_json(self):
        snapshot = {"tasks": [dict(TASKS["tasks"][0]) for _ in range(13)]}
        image, meta = render_table(snapshot, 2)
        self.assertEqual(meta, {"page": 2, "pages": 3, "count": 13})
        self.assertEqual(len(image.tobytes()), 8832)
        self.assertEqual(len(snapshot["tasks"]), 13)


    def test_concurrent_json_exports_have_independent_temp_files(self):
        target = self.base / "data/concurrent.json"
        barrier = threading.Barrier(2)
        original_replace = Path.replace
        local = threading.local()

        def synchronized_replace(source, destination):
            if Path(destination) == target and not getattr(local, "arrived", False):
                local.arrived = True
                barrier.wait(timeout=3)
            return original_replace(source, destination)

        with patch.object(Path, "replace", synchronized_replace):
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(write_json, target, {"value": i}) for i in (1, 2)]
                for future in futures:
                    future.result(timeout=5)
        self.assertIn(json.loads(target.read_text(encoding="utf-8"))["value"], (1, 2))
        self.assertEqual(list(target.parent.glob("*.tmp")), [])

    def test_cloud_http_error_closes_response(self):
        body = io.BytesIO(b"sensitive error payload")
        error = HTTPError("https://example.invalid", 403, "Forbidden", {}, body)
        cloud = YandexCloud(CloudSettings("fake", "folder"))
        with patch.object(cloud_module, "urlopen", side_effect=error):
            with self.assertRaises(CloudError) as failure:
                cloud._request("https://example.invalid", b"test", "application/json")
        self.assertTrue(body.closed)
        self.assertNotIn("sensitive", str(failure.exception))

    def test_configuration_trims_spaces_and_rejects_wrong_types(self):
        config = self.base / "secrets.json"
        config.write_text(json.dumps({"YANDEX_API_KEY": " fake ", "YANDEX_FOLDER_ID": " folder "}))
        with patch.object(cloud_module, "BASE_DIR", self.base), patch.dict(cloud_module.os.environ, {}, clear=True):
            settings = cloud_module.load_settings()
            self.assertEqual(settings.api_key, "fake")
            self.assertEqual(settings.folder_id, "folder")
            config.write_text('{"YANDEX_API_KEY": null}')
            with self.assertRaises(ValueError):
                cloud_module.load_settings()


class WorkerTest(unittest.IsolatedAsyncioTestCase):
    async def test_worker_survives_temporary_database_failure(self):
        recovered = asyncio.Event()
        loop = asyncio.get_running_loop()

        class Store:
            calls = 0
            recoveries = 0
            def fail_interrupted_jobs(self, _message):
                self.recoveries += 1
            def process_next(self, _cloud):
                self.calls += 1
                if self.calls == 1:
                    raise sqlite3.OperationalError("database is locked")
                loop.call_soon_threadsafe(recovered.set)
                return False

        store = Store()
        with self.assertLogs("job_worker", level="ERROR"):
            task = asyncio.create_task(process_jobs(store, None, True, interval=0.01))
            try:
                await asyncio.wait_for(recovered.wait(), timeout=3)
                self.assertFalse(task.done())
                self.assertEqual(store.recoveries, 1)
            finally:
                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await task

    async def test_shutdown_waits_for_inflight_save(self):
        started = asyncio.Event()
        release = threading.Event()
        saved = threading.Event()
        loop = asyncio.get_running_loop()

        class Store:
            def process_next(self, _cloud):
                loop.call_soon_threadsafe(started.set)
                release.wait(timeout=3)
                saved.set()
                return True

        task = asyncio.create_task(process_jobs(Store(), None, True))
        try:
            await asyncio.wait_for(started.wait(), timeout=3)
            task.cancel()
            await asyncio.sleep(0)
            self.assertFalse(task.done())
        finally:
            release.set()
            with self.assertRaises(asyncio.CancelledError):
                await task
        self.assertTrue(saved.is_set())


if __name__ == "__main__":
    unittest.main(verbosity=2)
