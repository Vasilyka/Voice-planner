# Проверка драйвера экрана без платы

Тест компилирует основной status_display.cpp с имитацией GPIO, SPI и очереди FreeRTOS.
Проверяются поворот координат, команды полного/частичного обновления, восстановление
предыдущего кадра после сна, сохранение запроса очистки при замене состояний в очереди
и запрет полной очистки при активной записи.

Из корня репозитория, с g++ в PATH:

    New-Item -ItemType Directory -Force tests/firmware_display/.build
    g++ -std=c++17 -Wall -Wextra -pedantic -Itests/firmware_display/stubs tests/firmware_display/status_display_test.cpp -o tests/firmware_display/.build/status_display_test.exe
    & ./tests/firmware_display/.build/status_display_test.exe

Эта проверка контролирует логику и SPI-команды, но не моделирует электрофизику панели.
Качество частичного обновления, отсутствие остаточных следов и реальную ориентацию
нужно проверить на GDEM035T81 по инструкции firmware/README.md.

## Проверка сетевого протокола

planner_protocol_test.cpp проверяет неверные подтверждения загрузки, неизвестные
состояния заданий, отключённый/оборванный поток кадра и фильтрацию запоздалых событий.
Оба набора проверок запускаются из корня репозитория:

    & ./tests/firmware_display/run_checks.ps1
