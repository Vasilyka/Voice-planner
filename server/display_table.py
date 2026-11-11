"""Таблица с кириллицей: сервер готовит 1-битный кадр, ESP32 принимает готовые пиксели."""
from pathlib import Path
import os
from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 184, 384
ROWS_PER_PAGE = 6


def get_font(size):
    choices = [os.environ.get("PLANNER_FONT", ""), "C:/Windows/Fonts/arial.ttf",
               "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]
    for name in choices:
        if name and Path(name).is_file():
            return ImageFont.truetype(name, size)
    raise RuntimeError("Не найден шрифт с кириллицей. Укажи PLANNER_FONT.")


def wrap_text(draw, text, font, width, lines=3):
    text = " ".join(text.split())
    result = []
    remaining = text
    for index in range(lines):
        if not remaining:
            break
        candidate = ""
        for char in remaining:
            if draw.textlength(candidate + char, font=font) > width:
                break
            candidate += char
        if len(candidate) < len(remaining):
            split = candidate.rfind(" ")
            if split > 0:
                candidate = candidate[:split]
        if not candidate:
            candidate = remaining[:1]
        remaining = remaining[len(candidate):].lstrip()
        if index == lines - 1 and remaining:
            while candidate and draw.textlength(candidate + "…", font=font) > width:
                candidate = candidate[:-1]
            candidate += "…"
        result.append(candidate)
    return result


def render_table(snapshot, page=0):
    tasks = snapshot["tasks"]
    pages = max(1, (len(tasks) + ROWS_PER_PAGE - 1) // ROWS_PER_PAGE)
    page = min(max(0, page), pages - 1)
    image = Image.new("1", (WIDTH, HEIGHT), 1)
    draw = ImageDraw.Draw(image)
    font, small, heading = get_font(11), get_font(10), get_font(15)
    draw.text((5, 3), "ПЛАНИРОВЩИК", font=heading, fill=0)
    draw.text((5, 25), "Дата", font=small, fill=0)
    draw.text((38, 25), "Время", font=small, fill=0)
    draw.text((76, 25), "№ / Дело", font=small, fill=0)
    draw.line((0, 40, WIDTH - 1, 40), fill=0)
    shown = tasks[page * ROWS_PER_PAGE:(page + 1) * ROWS_PER_PAGE]
    for index, task in enumerate(shown):
        y = 43 + index * 50
        date = task["due_date"]
        date_label = date[8:10] + "." + date[5:7] if date else "—"
        draw.text((2, y), date_label, font=small, fill=0)
        # Год сохраняется в строке даты, чтобы задачи разных лет не выглядели одинаково.
        if date:
            draw.text((2, y + 13), date[:4], font=small, fill=0)
        draw.text((37, y), task["due_time"] or "—", font=small, fill=0)
        marker = "? " if task["needs_clarification"] else ("! " if task["priority"] == "high" else "")
        number = f"№{task['number']} " if task.get("number") else ""
        for line, text in enumerate(wrap_text(draw, number + marker + task["title"], font, 106)):
            draw.text((76, y + line * 13), text, font=font, fill=0)
        draw.line((0, y + 47, WIDTH - 1, y + 47), fill=0)
    if not shown:
        draw.text((12, 85), "Задач пока нет", font=font, fill=0)
    for x in (0, 34, 73, WIDTH - 1):
        draw.line((x, 40, x, 340), fill=0)
    draw.text((3, 346), f"Стр. {page + 1}/{pages}   Всего: {len(tasks)}", font=small, fill=0)
    footer = "? Повторите команду с №" if snapshot.get("clarifications") else "? уточнить  ! важно"
    draw.text((3, 362), footer, font=small, fill=0)
    # Pillow mode 1: 1 = белый, старший бит слева, как кадровый буфер ESP32.
    return image, {"page": page, "pages": pages, "count": len(tasks)}
