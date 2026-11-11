"""Последовательная обработка облачных заданий с восстановлением после сбоев хранилища."""
import asyncio
import logging

log = logging.getLogger(__name__)


async def process_jobs(planner, cloud, configured, interval=0.5):
    recover = False
    while True:
        if configured:
            def process():
                if recover:
                    planner.fail_interrupted_jobs("Ошибка хранилища прервала обработку. Повторите явно.")
                return planner.process_next(cloud)

            work = asyncio.create_task(asyncio.to_thread(process))
            try:
                processed = await asyncio.shield(work)
            except asyncio.CancelledError:
                # Отмена to_thread не останавливает поток. Дожидаемся завершения сохранения,
                # прежде чем закрывать сервер и разрешать следующий запуск с той же базой.
                try:
                    await work
                except Exception as error:
                    log.error("Ошибка завершения обработчика: %s", type(error).__name__)
                raise
            except Exception as error:
                log.error("Сбой фонового обработчика: %s; повтор проверки очереди позже.",
                          type(error).__name__)
                recover = True
            else:
                recover = False
                if processed:
                    continue
        await asyncio.sleep(interval)
