"""Хранение заданий распознавания и проверенных задач."""
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from uuid import uuid4
import json
import sqlite3
import tempfile
import threading
import time

from cloud import CloudError
from task_actions import apply_commands

MOSCOW = timezone(timedelta(hours=3), "Europe/Moscow")


def now():
    return datetime.now(MOSCOW).isoformat(timespec="seconds")


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=path.name + ".", suffix=".tmp", delete=False) as output:
            temp = Path(output.name)
            json.dump(value, output, ensure_ascii=False, indent=2)
        for attempt in range(6):
            try:
                temp.replace(path)
                break
            except PermissionError:
                # Windows может ненадолго блокировать файл при одновременной замене.
                if attempt == 5:
                    raise
                time.sleep(0.01 * (attempt + 1))
    finally:
        if temp is not None:
            temp.unlink(missing_ok=True)


class Planner:
    def __init__(self, base):
        self.base = Path(base)
        self.db_path = self.base / "tasks.db"
        self.recordings = self.base / "recordings"
        self._export_lock = threading.Lock()

    @contextmanager
    def connect(self):
        connection = sqlite3.connect(self.db_path, timeout=10)
        connection.row_factory = sqlite3.Row
        try:
            with connection:
                yield connection
        finally:
            connection.close()

    def initialize(self):
        self.recordings.mkdir(parents=True, exist_ok=True)
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS voice_jobs (
                    id TEXT PRIMARY KEY, file TEXT NOT NULL, created_at TEXT NOT NULL,
                    state TEXT NOT NULL, transcript TEXT, result TEXT, error TEXT,
                    recording_id TEXT UNIQUE, body_hash TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS planner_tasks (
                    id TEXT PRIMARY KEY, source_job TEXT NOT NULL, position INTEGER NOT NULL,
                    title TEXT NOT NULL, due_date TEXT, due_time TEXT,
                    priority TEXT NOT NULL, needs_clarification INTEGER NOT NULL,
                    note TEXT NOT NULL, created_at TEXT NOT NULL
                );
            """)
            db.execute("BEGIN IMMEDIATE")
            job_columns = {row["name"] for row in db.execute("PRAGMA table_info(voice_jobs)")}
            if "finished_order" not in job_columns:
                db.execute("ALTER TABLE voice_jobs ADD COLUMN finished_order INTEGER NOT NULL DEFAULT 0")
                db.execute("UPDATE voice_jobs SET finished_order=rowid WHERE state='done'")
            columns = {row["name"] for row in db.execute("PRAGMA table_info(planner_tasks)")}
            for name, definition in (
                ("status", "TEXT NOT NULL DEFAULT 'pending'"),
                ("number", "INTEGER"),
                ("updated_at", "TEXT"),
            ):
                if name not in columns:
                    db.execute(f"ALTER TABLE planner_tasks ADD COLUMN {name} {definition}")
            db.execute("UPDATE planner_tasks SET number=rowid WHERE number IS NULL")
            db.execute("UPDATE planner_tasks SET updated_at=created_at WHERE updated_at IS NULL")
            db.execute("CREATE UNIQUE INDEX IF NOT EXISTS planner_task_number ON planner_tasks(number)")
            db.execute("""CREATE TABLE IF NOT EXISTS task_events (
                job_id TEXT NOT NULL, command_index INTEGER NOT NULL, task_id TEXT NOT NULL,
                action TEXT NOT NULL, before_status TEXT, after_status TEXT NOT NULL,
                created_at TEXT NOT NULL, PRIMARY KEY (job_id,command_index,task_id)
            )""")
            event_columns = list(db.execute("PRAGMA table_info(task_events)"))
            primary_key = [row["name"] for row in sorted(event_columns, key=lambda row: row["pk"]) if row["pk"]]
            if primary_key != ["job_id", "command_index", "task_id"]:
                db.execute("""CREATE TABLE task_events_upgrade (
                    job_id TEXT NOT NULL, command_index INTEGER NOT NULL, task_id TEXT NOT NULL,
                    action TEXT NOT NULL, before_status TEXT, after_status TEXT NOT NULL,
                    created_at TEXT NOT NULL, PRIMARY KEY (job_id,command_index,task_id)
                )""")
                db.execute("""INSERT INTO task_events_upgrade
                    SELECT job_id,command_index,task_id,action,before_status,after_status,created_at FROM task_events""")
                db.execute("DROP TABLE task_events")
                db.execute("ALTER TABLE task_events_upgrade RENAME TO task_events")
            db.execute("CREATE INDEX IF NOT EXISTS task_events_task ON task_events(task_id)")
        # Незавершённый вызов мог быть оплачен. Повтор — только явно.
        self.fail_interrupted_jobs("Сервер перезапущен во время обработки. Можно повторить запрос.")
        self.export()

    def fail_interrupted_jobs(self, message):
        with self.connect() as db:
            db.execute("UPDATE voice_jobs SET state='failed', error=? WHERE state='processing'", (message,))

    def create_job(self, file, configured, body_hash, recording_id=None):
        job_id = uuid4().hex
        with self.connect() as db:
            db.execute("""INSERT INTO voice_jobs
                       (id,file,created_at,state,recording_id,body_hash) VALUES (?,?,?,?,?,?)""",
                       (job_id, file, now(), "queued" if configured else "needs_configuration",
                        recording_id, body_hash))
        return self.job(job_id)

    def find_recording(self, recording_id):
        with self.connect() as db:
            row = db.execute("SELECT * FROM voice_jobs WHERE recording_id=?", (recording_id,)).fetchone()
        return dict(row) if row else None

    def job(self, job_id):
        with self.connect() as db:
            row = db.execute("SELECT * FROM voice_jobs WHERE id=?", (job_id,)).fetchone()
        if row is None:
            return None
        result = dict(row)
        result.pop("recording_id", None)
        result.pop("body_hash", None)
        if result["result"]:
            result["result"] = json.loads(result["result"])
        return result

    def recent_jobs(self):
        with self.connect() as db:
            ids = [row[0] for row in db.execute("SELECT id FROM voice_jobs ORDER BY rowid DESC LIMIT 20")]
        return [self.job(item) for item in ids]

    def retry(self, job_id):
        with self.connect() as db:
            db.execute("""UPDATE voice_jobs SET state='queued', error=NULL
                          WHERE id=? AND state IN ('failed','needs_configuration')""", (job_id,))
        return self.job(job_id)

    def process_next(self, cloud):
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM voice_jobs WHERE state='queued' ORDER BY rowid LIMIT 1").fetchone()
            if row is None:
                return False
            job = dict(row)
            db.execute("UPDATE voice_jobs SET state='processing', error=NULL WHERE id=?", (job["id"],))
        try:
            transcript = job["transcript"] or cloud.transcribe(self.recordings / job["file"])
            with self.connect() as db:
                db.execute("UPDATE voice_jobs SET transcript=? WHERE id=?", (transcript, job["id"]))
            catalog = self.command_catalog(job["id"])
            batch = cloud.extract(transcript, job["created_at"], catalog)
            with self.connect() as db:
                db.execute("BEGIN IMMEDIATE")
                tasks, actions, clarifications = apply_commands(db, batch, job, transcript)
                result = {"schema_version": 2, "timezone": "Europe/Moscow",
                          "recorded_at": job["created_at"], "transcript": transcript,
                          "tasks": tasks, "actions": actions, "clarifications": clarifications}
                finished_order = db.execute("SELECT COALESCE(MAX(finished_order),0)+1 FROM voice_jobs").fetchone()[0]
                db.execute("UPDATE voice_jobs SET state='done', result=?, finished_order=? WHERE id=?",
                           (json.dumps(result, ensure_ascii=False), finished_order, job["id"]))
        except Exception as error:
            message = str(error) if isinstance(error, CloudError) else "Ошибка обработки. Изменения не применены."
            with self.connect() as db:
                db.execute("UPDATE voice_jobs SET state='failed', error=? WHERE id=?", (message, job["id"]))
            return True
        try:
            write_json((self.recordings / job["file"]).with_suffix(".json"), result)
            self.export()
        except OSError:
            print("Не удалось обновить JSON-файл; данные сохранены в SQLite.", flush=True)
        return True

    def command_catalog(self, job_id):
        # Только названия: номера модель должна извлечь из речи, а не выбирать сама.
        with self.connect() as db:
            rows = db.execute("""
                SELECT t.title FROM planner_tasks t LEFT JOIN voice_jobs source ON source.id=t.source_job
                WHERE COALESCE(source.rowid,0) <= (SELECT rowid FROM voice_jobs WHERE id=?)
                ORDER BY t.status='pending' DESC, t.rowid DESC LIMIT 200
            """, (job_id,)).fetchall()
        return list(dict.fromkeys(row["title"] for row in rows))

    def snapshot(self):
        with self.connect() as db:
            rows = db.execute("""SELECT * FROM planner_tasks ORDER BY
                due_date IS NULL, due_date, due_time IS NULL, due_time, created_at, position, number""").fetchall()
            latest = db.execute("SELECT result FROM voice_jobs WHERE state='done' ORDER BY finished_order DESC,rowid DESC LIMIT 1").fetchone()
        tasks, archived = [], []
        for row in rows:
            task = dict(row)
            task.pop("position")
            task["needs_clarification"] = bool(task["needs_clarification"])
            (tasks if task["status"] == "pending" else archived).append(task)
        clarifications = json.loads(latest["result"]).get("clarifications", []) if latest and latest["result"] else []
        return {"schema_version": 2, "timezone": "Europe/Moscow", "tasks": tasks,
                "archived_tasks": archived, "clarifications": clarifications}

    def export(self):
        with self._export_lock:
            snapshot = self.snapshot()
            write_json(self.base / "data/planner.json", snapshot)
            return snapshot
