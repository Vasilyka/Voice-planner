"""Формат задач, общий для облачного разбора, JSON-файла и таблицы."""
from datetime import date, time
from typing import Literal
from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


class TaskDraft(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, str_strip_whitespace=True)
    title: str = Field(min_length=1, max_length=120)
    due_date: str | None
    due_time: str | None
    priority: Literal["low", "normal", "high"]
    needs_clarification: bool
    note: str = Field(max_length=240)

    @field_validator("due_date")
    @classmethod
    def check_date(cls, value):
        if value is not None:
            if len(value) != 10 or date.fromisoformat(value).isoformat() != value:
                raise ValueError("Нужна дата YYYY-MM-DD")
        return value

    @field_validator("due_time")
    @classmethod
    def check_time(cls, value):
        if value is not None:
            if len(value) != 5 or time.fromisoformat(value).strftime("%H:%M") != value:
                raise ValueError("Нужно время HH:MM")
        return value

    @model_validator(mode="after")
    def ambiguous_time(self):
        if self.due_time and not self.due_date:
            self.needs_clarification = True
            self.note = (self.note + " Уточнить дату для указанного времени.").strip()[:240]
        return self


class TaskBatch(BaseModel):
    model_config = ConfigDict(extra="forbid")
    tasks: list[TaskDraft] = Field(max_length=20)


class PlannerCommand(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, str_strip_whitespace=True)
    action: Literal["create", "delete", "complete", "undo_last", "restore", "restore_last", "del"]
    task: TaskDraft | None
    target_title: str | None = Field(min_length=1, max_length=120)
    target_number: int | None = Field(ge=1)
    target_date: str | None
    target_time: str | None
    needs_clarification: bool
    note: str = Field(max_length=240)

    @field_validator("target_date")
    @classmethod
    def check_target_date(cls, value):
        return TaskDraft.check_date(value)

    @field_validator("target_time")
    @classmethod
    def check_target_time(cls, value):
        return TaskDraft.check_time(value)

    @model_validator(mode="after")
    def check_shape(self):
        selectors = (self.target_title, self.target_number, self.target_date, self.target_time)
        if self.action == "create":
            if self.task is None or any(value is not None for value in selectors):
                raise ValueError("Создание требует task без селекторов существующей задачи")
        elif self.task is not None:
            raise ValueError("Изменение статуса не создаёт новую задачу")
        if self.action in ("undo_last", "restore_last", "del") and any(value is not None for value in selectors):
            raise ValueError("Команда последнего дела или удаления всех не принимает селекторы")
        return self


class CommandBatch(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    commands: list[PlannerCommand] = Field(max_length=20)
