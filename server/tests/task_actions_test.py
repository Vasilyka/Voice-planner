"""Голосовые команды проверяются на отдельной временной базе, без запросов в облако."""
from pathlib import Path
from contextlib import closing
import sys
import sqlite3
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from cloud import CloudError
from planner import Planner
from task_actions import spoken_numbers
from task_models import CommandBatch
from display_table import render_table


def draft(title, date=None, time=None):
    return dict(title=title, due_date=date, due_time=time, priority="normal",
                needs_clarification=False, note="")


def command(action, title=None, number=None, task=None, date=None, time=None, clarify=False):
    return dict(action=action, task=task, target_title=title, target_number=number,
                target_date=date, target_time=time, needs_clarification=clarify, note="")


class FakeCloud:
    def __init__(self, transcript, commands, fail=False):
        self.transcript, self.commands, self.fail = transcript, commands, fail
    def transcribe(self, path):
        return self.transcript
    def extract(self, transcript, reference_time, catalog=None):
        if self.fail:
            raise CloudError("Временный сбой модели")
        return CommandBatch.model_validate({"commands": self.commands})


class ActionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.store = Planner(self.base)
        self.store.initialize()
        self.sequence = 0

    def run_command(self, text, commands, fail=False):
        self.sequence += 1
        job = self.store.create_job(f"recording-{self.sequence}.wav", True, f"hash-{self.sequence}")
        self.assertTrue(self.store.process_next(FakeCloud(text, commands, fail)))
        return self.store.job(job["id"])

    def add(self, title, date=None, time=None):
        job = self.run_command("Добавь " + title, [command("create", task=draft(title, date, time))])
        self.assertEqual(job["state"], "done")
        return job["result"]["tasks"][0]

    def test_delete_by_title_archives_without_erasing(self):
        task = self.add("Купить хлеб")
        result = self.run_command("Удали задачу купить хлеб", [command("delete", title="купить ХЛЕБ!")])
        self.assertEqual(result["result"]["actions"][0]["outcome"], "applied")
        snapshot = self.store.snapshot()
        self.assertEqual(snapshot["tasks"], [])
        self.assertEqual(snapshot["archived_tasks"][0]["status"], "deleted")
        self.assertEqual(snapshot["archived_tasks"][0]["id"], task["id"])

    def test_complete_and_restore_preserve_identity(self):
        task = self.add("Лабораторная по физике")
        self.run_command("Лабораторная по физике выполнена", [command("complete", title=task["title"])])
        self.assertEqual(self.store.snapshot()["archived_tasks"][0]["status"], "done")
        self.run_command("Верни задачу лабораторная по физике", [command("restore", title=task["title"])])
        restored = self.store.snapshot()["tasks"][0]
        self.assertEqual((restored["id"], restored["number"]), (task["id"], task["number"]))
        self.assertEqual(self.store.snapshot()["archived_tasks"], [])

    def test_ambiguity_does_not_change_tasks_and_number_resolves(self):
        first = self.add("Купить хлеб")
        second = self.add("Купить хлеб")
        result = self.run_command("Удали купить хлеб", [command("delete", title=first["title"])])
        self.assertEqual(len(self.store.snapshot()["tasks"]), 2)
        self.assertEqual(len(result["result"]["clarifications"][0]["candidates"]), 2)
        snapshot = self.store.snapshot()
        self.assertTrue(snapshot["clarifications"])
        image, _ = render_table(snapshot)
        self.assertEqual(len(image.tobytes()), 8832)
        self.run_command("Удали задачу номер два", [command("delete", number=second["number"])])
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [first["id"]])
        self.assertEqual(self.store.snapshot()["clarifications"], [])

    def test_model_cannot_invent_task_number(self):
        task = self.add("Купить хлеб")
        result = self.run_command("Удали купить хлеб", [command("delete", number=task["number"])])
        self.assertEqual(len(self.store.snapshot()["tasks"]), 1)
        self.assertTrue(result["result"]["clarifications"])

    def test_date_disambiguates_same_title(self):
        first = self.add("Купить хлеб", "2026-11-11")
        second = self.add("Купить хлеб", "2026-11-12")
        self.run_command("Удали купить хлеб на 12 ноября",
                         [command("delete", title=first["title"], date="2026-11-12")])
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [first["id"]])
        self.assertEqual(self.store.snapshot()["archived_tasks"][0]["id"], second["id"])

    def test_undo_last_uses_creation_order_not_due_date(self):
        first = self.add("Первое дело", "2026-11-11")
        last = self.add("Второе дело", "2026-11-12")
        self.run_command("Отмени последнее добавленное дело", [command("undo_last")])
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [first["id"]])
        self.assertEqual(self.store.snapshot()["archived_tasks"][0]["id"], last["id"])

    def test_restore_last_uses_deletion_history(self):
        first = self.add("Первое дело")
        last = self.add("Второе дело")
        self.run_command("Удали второе дело", [command("delete", title=last["title"])])
        self.run_command("Удали первое дело", [command("delete", title=first["title"])])
        self.run_command("Отмени последнее удаление", [command("restore_last")])
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [first["id"]])
        self.assertEqual(self.store.snapshot()["archived_tasks"][0]["id"], last["id"])

    def test_done_job_retry_does_not_repeat_mutation(self):
        task = self.add("Купить хлеб")
        job = self.run_command("Удали купить хлеб", [command("delete", title=task["title"])])
        self.store.retry(job["id"])
        self.assertFalse(self.store.process_next(FakeCloud("", [])))
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM task_events").fetchone()[0], 2)

    def test_create_then_cancel_in_one_recording(self):
        result = self.run_command("Добавь купить хлеб и отмени последнее добавленное",
            [command("create", task=draft("Купить хлеб")), command("undo_last")])
        self.assertEqual(self.store.snapshot()["tasks"], [])
        self.assertEqual(result["result"]["tasks"][0]["status"], "deleted")
        self.assertEqual(len(result["result"]["actions"]), 2)

    def test_no_target_or_uncertain_command_does_not_delete(self):
        self.add("Купить хлеб")
        self.run_command("Удали всё", [command("delete")])
        self.assertEqual(len(self.store.snapshot()["tasks"]), 1)
        self.run_command("Непонятная команда", [command("undo_last", clarify=True)])
        self.assertEqual(len(self.store.snapshot()["tasks"]), 1)

    def test_retry_old_undo_never_deletes_newer_creation(self):
        first = self.add("Первое дело")
        old = self.run_command("Отмени последнее добавленное", [command("undo_last")], fail=True)
        latest = self.add("Добавленное позже")
        self.store.retry(old["id"])
        self.store.process_next(FakeCloud("", [command("undo_last")]))
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [latest["id"]])
        self.assertEqual(self.store.snapshot()["archived_tasks"][0]["id"], first["id"])

    def test_retry_old_command_does_not_override_newer_status(self):
        task = self.add("Купить хлеб")
        old = self.run_command("Удали купить хлеб", [command("delete", title=task["title"])], fail=True)
        self.run_command("Купить хлеб выполнено", [command("complete", title=task["title"])])
        self.store.retry(old["id"])
        self.store.process_next(FakeCloud("", [command("delete", title=task["title"])]))
        snapshot = self.store.snapshot()
        self.assertEqual(snapshot["archived_tasks"][0]["status"], "done")
        self.assertTrue(snapshot["clarifications"]) # Уточнение последнего обработанного запроса, даже старого.

    def test_old_undo_does_not_jump_over_later_completed_task(self):
        first = self.add("Первое дело")
        last = self.add("Последнее дело")
        old = self.run_command("Отмени последнее добавленное", [command("undo_last")], fail=True)
        self.run_command("Последнее дело выполнено", [command("complete", title=last["title"])])
        self.store.retry(old["id"])
        self.store.process_next(FakeCloud("", [command("undo_last")]))
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [first["id"]])
        self.assertTrue(self.store.snapshot()["clarifications"])

    def test_old_ambiguous_delete_does_not_become_unique_after_completion(self):
        first = self.add("Купить хлеб")
        second = self.add("Купить хлеб")
        old = self.run_command("Удали купить хлеб", [command("delete", title=first["title"])], fail=True)
        self.run_command("Выполнена задача номер два", [command("complete", number=second["number"])])
        self.store.retry(old["id"])
        self.store.process_next(FakeCloud("", [command("delete", title=first["title"])]))
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [first["id"]])
        self.assertTrue(self.store.snapshot()["clarifications"])

    def test_old_restore_last_does_not_restore_previous_deleted_task(self):
        first = self.add("Первое дело")
        second = self.add("Второе дело")
        self.run_command("Удали первое дело", [command("delete", title=first["title"])])
        self.run_command("Удали второе дело", [command("delete", title=second["title"])])
        old = self.run_command("Отмени последнее удаление", [command("restore_last")], fail=True)
        self.run_command("Верни второе дело", [command("restore", title=second["title"])])
        self.store.retry(old["id"])
        self.store.process_next(FakeCloud("", [command("restore_last")]))
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [second["id"]])
        self.assertEqual(self.store.snapshot()["archived_tasks"][0]["id"], first["id"])

    def test_numbers_survive_restart_and_are_not_reused(self):
        first = self.add("Купить хлеб")
        self.run_command("Удали купить хлеб", [command("delete", title=first["title"])])
        self.store.initialize()
        second = self.add("Купить молоко")
        self.assertEqual((first["number"], second["number"]), (1, 2))
        self.assertEqual(self.store.snapshot()["archived_tasks"][0]["status"], "deleted")

    def test_old_database_migrates_without_losing_tasks(self):
        legacy = self.base / "legacy"
        legacy.mkdir()
        with closing(sqlite3.connect(legacy / "tasks.db")) as db:
            db.execute("""CREATE TABLE planner_tasks (
                id TEXT PRIMARY KEY, source_job TEXT NOT NULL, position INTEGER NOT NULL,
                title TEXT NOT NULL, due_date TEXT, due_time TEXT, priority TEXT NOT NULL,
                needs_clarification INTEGER NOT NULL, note TEXT NOT NULL, created_at TEXT NOT NULL)""")
            db.execute("INSERT INTO planner_tasks VALUES (?,?,?,?,?,?,?,?,?,?)",
                       ("old-id","old-job",0,"Старое дело",None,None,"normal",0,"","2026-11-10T10:00:00+03:00"))
            db.commit()
        migrated = Planner(legacy)
        migrated.initialize()
        task = migrated.snapshot()["tasks"][0]
        self.assertEqual((task["id"], task["status"], task["number"]), ("old-id", "pending", 1))
        migrated.initialize()
        self.assertEqual(len(migrated.snapshot()["tasks"]), 1)


    def test_delete_active_title_does_not_select_completed_namesake(self):
        old = self.add("Купить хлеб")
        self.run_command("Купить хлеб выполнено", [command("complete", title=old["title"])])
        current = self.add("Купить хлеб")
        self.run_command("Удали купить хлеб", [command("delete", title=current["title"])])
        archive = {task["id"]: task["status"] for task in self.store.snapshot()["archived_tasks"]}
        self.assertEqual(archive, {old["id"]: "done", current["id"]: "deleted"})

    def test_multiple_spoken_numbers_in_one_recording(self):
        first = self.add("Первое дело")
        second = self.add("Второе дело")
        result = self.run_command("Удали задачу номер один и выполни задачу номер два",
             [command("delete", number=first["number"]), command("complete", number=second["number"])])
        self.assertFalse(result["result"]["clarifications"])
        self.assertEqual([action["after_status"] for action in result["result"]["actions"]], ["deleted", "done"])

    def test_del_archives_all_active_and_preserves_existing_archive(self):
        first = self.add("Первое активное")
        second = self.add("Второе активное")
        done = self.add("Уже выполненное")
        deleted = self.add("Уже удалённое")
        self.run_command("Уже выполненное выполнено", [command("complete", title=done["title"])])
        self.run_command("Удали уже удалённое", [command("delete", title=deleted["title"])])
        result = self.run_command("Удали все события", [command("del")])
        self.assertEqual(result["state"], "done")
        action = result["result"]["actions"][0]
        self.assertEqual((action["outcome"], action["count"]), ("applied", 2))
        self.assertEqual({t["id"] for t in action["tasks"]}, {first["id"], second["id"]})
        self.assertEqual(self.store.snapshot()["tasks"], [])
        archive = {t["id"]: t["status"] for t in self.store.snapshot()["archived_tasks"]}
        self.assertEqual(archive, {first["id"]: "deleted", second["id"]: "deleted",
                                  done["id"]: "done", deleted["id"]: "deleted"})
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM task_events WHERE job_id=?",
                                        (result["id"],)).fetchone()[0], 2)
        self.run_command("Верни задачу номер один", [command("restore", number=first["number"])])
        self.assertEqual(self.store.snapshot()["tasks"][0]["id"], first["id"])

    def test_del_empty_and_finished_job_retry_do_not_touch_new_tasks(self):
        empty = self.run_command("Удали все дела", [command("del")])
        self.assertEqual(empty["result"]["actions"][0]["outcome"], "unchanged")
        self.add("Первая задача")
        job = self.run_command("Удали всё", [command("del")])
        added_later = self.add("После удаления")
        self.store.retry(job["id"])
        self.assertFalse(self.store.process_next(FakeCloud("", [command("del")])))
        self.assertEqual(self.store.snapshot()["tasks"][0]["id"], added_later["id"])

    def test_old_del_excludes_later_tasks_and_rejects_newer_status(self):
        first = self.add("Первое дело")
        old = self.run_command("Удали все задачи", [command("del")], fail=True)
        later = self.add("Добавлено позже")
        self.store.retry(old["id"])
        self.store.process_next(FakeCloud("", [command("del")]))
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [later["id"]])
        self.assertEqual(self.store.snapshot()["archived_tasks"][0]["id"], first["id"])
        next_task = self.add("Ещё дело")
        interrupted = self.run_command("Удали все задачи", [command("del")], fail=True)
        self.run_command("Ещё дело выполнено", [command("complete", title=next_task["title"])])
        self.store.retry(interrupted["id"])
        self.store.process_next(FakeCloud("", [command("del")]))
        self.assertEqual([t["id"] for t in self.store.snapshot()["tasks"]], [later["id"]])
        self.assertTrue(self.store.snapshot()["clarifications"])

    def test_del_in_mixed_recording_preserves_command_order(self):
        result = self.run_command("Добавь хлеб и удали все дела и добавь молоко",
            [command("create", task=draft("Хлеб")), command("del"),
             command("create", task=draft("Молоко"))])
        self.assertEqual(result["state"], "done")
        self.assertEqual([t["title"] for t in self.store.snapshot()["tasks"]], ["Молоко"])
        self.assertEqual([t["title"] for t in self.store.snapshot()["archived_tasks"]], ["Хлеб"])

    def test_del_requires_explicit_unfiltered_request(self):
        self.add("Купить хлеб")
        for text in ("Удали хлеб", "Не удали все задачи", "Удали все дела на завтра",
                     "Удали всё кроме хлеба"):
            with self.subTest(text=text):
                result = self.run_command(text, [command("del")])
                self.assertTrue(result["result"]["clarifications"])
                self.assertEqual(len(self.store.snapshot()["tasks"]), 1)
        invalid = self.run_command("Удали все дела на завтра", [command("del", date="2026-11-11")])
        self.assertEqual(invalid["state"], "failed")
        self.assertEqual(len(self.store.snapshot()["tasks"]), 1)

    def test_old_audit_primary_key_migrates_with_history(self):
        self.add("Первое дело")
        self.add("Второе дело")
        with self.store.connect() as db:
            db.execute("ALTER TABLE task_events RENAME TO old_events")
            db.execute("""CREATE TABLE task_events (
                job_id TEXT NOT NULL, command_index INTEGER NOT NULL, task_id TEXT NOT NULL,
                action TEXT NOT NULL, before_status TEXT, after_status TEXT NOT NULL,
                created_at TEXT NOT NULL, PRIMARY KEY (job_id,command_index))""")
            db.execute("INSERT INTO task_events SELECT * FROM old_events")
            db.execute("DROP TABLE old_events")
        self.store.initialize()
        result = self.run_command("Удали все события", [command("del")])
        self.assertEqual(result["result"]["actions"][0]["count"], 2)
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM task_events").fetchone()[0], 4)
        self.store.initialize()
        self.assertEqual(len(self.store.snapshot()["archived_tasks"]), 2)

    def test_spoken_numbers(self):
        self.assertEqual(spoken_numbers("Удали задачу номер двадцать пять"), {25})
        self.assertEqual(spoken_numbers("Верни дело № 123"), {123})
        self.assertEqual(spoken_numbers("Удали задачу 5"), {5})
        self.assertEqual(spoken_numbers("Удали встречу 12 ноября"), set())
        self.assertEqual(spoken_numbers("Удали номер пять и верни номер три"), {5, 3})


if __name__ == "__main__":
    unittest.main(verbosity=2)
