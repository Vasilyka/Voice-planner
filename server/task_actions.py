"""Применение голосовых команд. Выбор задачи проверяет сервер, а не облачная модель."""
import re
import unicodedata
from uuid import uuid5, NAMESPACE_URL


def normalize(text):
    return " ".join(re.findall(r"[^\W_]+", unicodedata.normalize("NFKC", text).casefold().replace("ё", "е")))


def spoken_numbers(text):
    words = {
        "один": 1, "одна": 1, "одну": 1, "первый": 1, "первую": 1,
        "два": 2, "две": 2, "второй": 2, "вторую": 2,
        "три": 3, "третий": 3, "третью": 3, "четыре": 4, "пять": 5,
        "шесть": 6, "семь": 7, "восемь": 8, "девять": 9, "десять": 10,
        "одиннадцать": 11, "двенадцать": 12, "тринадцать": 13, "четырнадцать": 14,
        "пятнадцать": 15, "шестнадцать": 16, "семнадцать": 17, "восемнадцать": 18,
        "девятнадцать": 19, "двадцать": 20, "тридцать": 30, "сорок": 40,
        "пятьдесят": 50, "шестьдесят": 60, "семьдесят": 70, "восемьдесят": 80,
        "девяносто": 90, "сто": 100, "двести": 200, "триста": 300,
        "четыреста": 400, "пятьсот": 500, "шестьсот": 600, "семьсот": 700,
        "восемьсот": 800, "девятьсот": 900,
    }
    result = set()
    text = text.casefold().replace("ё", "е")
    for marker in re.finditer(r"\bномер\b|№", text):
        match = re.match(r"\s*(\d+(?!\w)|[а-я]+(?:\s+[а-я]+){0,4})", text[marker.end():])
        if not match:
            continue
        value = match.group(1)
        if value.isdigit():
            result.add(int(value))
            continue
        total, previous, valid = 0, 1000, True
        for word in value.split():
            if word not in words:
                break
            number = words[word]
            if number >= previous or previous < 20:
                valid = False
                break
            total += number
            previous = number
        if total and valid:
            result.add(total)
    result.update(int(m.group(1)) for m in re.finditer(r"\b(?:задач[ауыеи]|дел[оау])\s+(\d+)\b", text))
    return result


def explicit_delete_all(text):
    text = normalize(text)
    pattern = (r"\b(?:удали|удалить|убери|убрать|отмени|отменить)\s+(?:пожалуйста\s+)?"
               r"(?:все(?:\s+мои)?\s+(?:дела|задачи|события)|все)"
               r"(?:\s+пожалуйста)?(?=$|\s+и\s+(?:добавь|создай|верни|отметь|удали)\b)")
    for match in re.finditer(pattern, text):
        prefix = text[:match.start()].split()
        if not prefix or prefix[-1] not in ("не", "нельзя", "надо", "нужно"):
            return True
    return False


def task_view(row):
    return {key: row[key] for key in ("id", "number", "title", "due_date", "due_time", "status")}


def eligible_tasks(db, job_id):
    # Старый запрос после повтора не должен выбирать дела, добавленные позднее.
    return [dict(row) for row in db.execute("""
        SELECT t.*, COALESCE((
            SELECT e.after_status FROM task_events e JOIN voice_jobs event_job ON event_job.id=e.job_id
            WHERE e.task_id=t.id AND event_job.rowid <= (SELECT rowid FROM voice_jobs WHERE id=?)
            ORDER BY e.rowid DESC LIMIT 1
        ), t.status) AS selection_status
        FROM planner_tasks t LEFT JOIN voice_jobs source ON source.id=t.source_job
        WHERE COALESCE(source.rowid,0) <= (SELECT rowid FROM voice_jobs WHERE id=?)
        ORDER BY t.rowid DESC
    """, (job_id, job_id))]


def apply_commands(db, batch, job, transcript):
    results, clarifications, created_ids = [], [], []
    spoken = spoken_numbers(transcript)

    def clarify(index, command, message, candidates=()):
        item = {"index": index, "action": command.action, "outcome": "needs_clarification",
                "message": message, "candidates": [task_view(row) for row in candidates]}
        results.append(item)
        clarifications.append(item)

    for index, command in enumerate(batch.commands):
        if command.needs_clarification:
            clarify(index, command, command.note or "Уточните команду и повторите её полностью.")
            continue
        if command.action == "del":
            if not explicit_delete_all(transcript):
                clarify(index, command, "Для удаления всех дел произнесите явно: «удали все дела».")
                continue
            rows = [row for row in eligible_tasks(db, job["id"]) if row["selection_status"] == "pending"]
            changed_later = []
            for row in rows:
                newer = db.execute("""
                    SELECT 1 FROM task_events e JOIN voice_jobs j ON j.id=e.job_id
                    WHERE e.task_id=? AND j.rowid > (SELECT rowid FROM voice_jobs WHERE id=?) LIMIT 1
                """, (row["id"], job["id"])).fetchone()
                if newer or row["status"] != "pending":
                    changed_later.append(row)
            if changed_later:
                clarify(index, command, "Список дел уже менялся. Повторите команду новой записью.", changed_later)
                continue
            affected = []
            for row in rows:
                db.execute("UPDATE planner_tasks SET status='deleted', updated_at=? WHERE id=?",
                           (job["created_at"], row["id"]))
                db.execute("""INSERT INTO task_events
                    (job_id,command_index,task_id,action,before_status,after_status,created_at)
                    VALUES (?,?,?,?,?,?,?)""",
                    (job["id"],index,row["id"],command.action,"pending","deleted",job["created_at"]))
                row["status"] = "deleted"
                affected.append(task_view(row))
            results.append({"index": index, "action": command.action,
                            "outcome": "applied" if rows else "unchanged", "count": len(rows),
                            "tasks": affected, "before_status": "pending", "after_status": "deleted"})
            continue
        if command.action == "create":
            task = command.task.model_dump()
            task_id = uuid5(NAMESPACE_URL, job["id"] + ":" + str(index)).hex
            number = db.execute("SELECT COALESCE(MAX(number),0)+1 FROM planner_tasks").fetchone()[0]
            db.execute("""INSERT INTO planner_tasks
                (id,source_job,position,title,due_date,due_time,priority,needs_clarification,
                 note,created_at,status,number,updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (task_id,job["id"],index,task["title"],task["due_date"],task["due_time"],
                 task["priority"],int(task["needs_clarification"]),task["note"],job["created_at"],
                 "pending",number,job["created_at"]))
            created_ids.append(task_id)
            before, after = None, "pending"
            row = dict(db.execute("SELECT * FROM planner_tasks WHERE id=?", (task_id,)).fetchone())
        else:
            rows = eligible_tasks(db, job["id"])
            if command.action == "undo_last":
                matches = [row for row in rows if row["selection_status"] == "pending"][:1]
                after = "deleted"
            elif command.action == "restore_last":
                history = db.execute("""
                    SELECT e.task_id FROM task_events e JOIN voice_jobs event_job ON event_job.id=e.job_id
                    WHERE e.after_status='deleted'
                      AND event_job.rowid <= (SELECT rowid FROM voice_jobs WHERE id=?)
                    ORDER BY e.rowid DESC
                """, (job["id"],)).fetchall()
                deleted = {row["id"]: row for row in rows if row["selection_status"] == "deleted"}
                matches = next(([deleted[item["task_id"]]] for item in history if item["task_id"] in deleted), [])
                after = "pending"
            else:
                after = {"delete": "deleted", "complete": "done", "restore": "pending"}[command.action]
                if command.target_number is not None and command.target_number not in spoken:
                    clarify(index, command, "Произнесите номер явно: например, «удали задачу номер пять».")
                    continue
                if command.target_number is None and command.target_title is None:
                    clarify(index, command, "Укажите название или номер одного дела.")
                    continue
                matches = [row for row in rows
                    if (command.target_number is None or row["number"] == command.target_number)
                    and (command.target_title is None or normalize(row["title"]) == normalize(command.target_title))
                    and (command.target_date is None or row["due_date"] == command.target_date)
                    and (command.target_time is None or row["due_time"] == command.target_time)]
                allowed = {"delete": ("pending", "done"), "complete": ("pending",),
                           "restore": ("done", "deleted")}[command.action]
                eligible = [row for row in matches if row["selection_status"] in allowed]
                if command.action == "delete" and command.target_number is None:
                    active = [row for row in eligible if row["selection_status"] == "pending"]
                    eligible = active or eligible
                matches = eligible or [row for row in matches if row["selection_status"] == after]
            if len(matches) != 1:
                message = ("Подходит несколько дел. Повторите команду с номером."
                           if len(matches) > 1 else "Подходящее дело не найдено. Уточните название или номер.")
                clarify(index, command, message, matches)
                continue
            row = matches[0]
            task_id, before = row["id"], row["status"]
            if before == after:
                results.append({"index": index, "action": command.action, "outcome": "unchanged",
                                "task": task_view(row), "before_status": before, "after_status": after})
                continue
            newer = db.execute("""
                SELECT 1 FROM task_events e JOIN voice_jobs j ON j.id=e.job_id
                WHERE e.task_id=? AND j.rowid > (SELECT rowid FROM voice_jobs WHERE id=?) LIMIT 1
            """, (task_id, job["id"])).fetchone()
            if newer:
                clarify(index, command, "После этой записи дело уже менялось. Повторите команду новой записью.", [row])
                continue
            db.execute("UPDATE planner_tasks SET status=?, updated_at=? WHERE id=?",
                       (after, job["created_at"], task_id))
            row["status"] = after
        db.execute("""INSERT INTO task_events
            (job_id,command_index,task_id,action,before_status,after_status,created_at)
            VALUES (?,?,?,?,?,?,?)""",
            (job["id"],index,task_id,command.action,before,after,job["created_at"]))
        results.append({"index": index, "action": command.action, "outcome": "applied",
                        "task": task_view(row), "before_status": before, "after_status": after})

    created = []
    for task_id in created_ids:
        row = dict(db.execute("SELECT * FROM planner_tasks WHERE id=?", (task_id,)).fetchone())
        row.pop("position")
        row["needs_clarification"] = bool(row["needs_clarification"])
        created.append(row)
    return created, results, clarifications
