# Подключение ESP32-S3

[Схема и компоненты](../hardware/README.md). Номера GPIO заданы в [board_pins.h](voice_planner/board_pins.h).

| Компонент | Сигнал | ESP32-S3 |
| --- | --- | --- |
| INMP441 | SCK / BCLK | GPIO12 |
| INMP441 | WS / LRCLK | GPIO13 |
| INMP441 | SD | GPIO14 |
| INMP441 | VDD | 3V3 |
| INMP441 | GND и L/R | GND (левый канал) |
| DESPI-C02 | BUSY | GPIO11 |
| DESPI-C02 | RES / RST | GPIO10 |
| DESPI-C02 | D/C | GPIO9 |
| DESPI-C02 | CS | GPIO21 |
| DESPI-C02 | SCK | GPIO6 |
| DESPI-C02 | SDI / MOSI | GPIO8 |
| DESPI-C02 | 3.3V | 3V3 |
| DESPI-C02 | GND | GND |
| Кнопка | Замыкает вход на GND, внутренняя подтяжка | GPIO4 |
| LED1 | Через R1 1 кОм, катод на GND | GPIO17 |
| LED2 | Через R2 1 кОм, катод на GND | GPIO16 |
| Делитель аккумулятора | Средняя точка R3/R4 | GPIO5 |
| USB-C | D− / D+ | GPIO19 / GPIO20 |

Порядок контактов DESPI-C02 по подписям платы — BUSY, RES, D/C, CS, SCK, SDI, GND, 3.3V. Переключатель P3 / RESE для GDEM035T81 — 2.2. Подключайте шлейф при отключённом питании.

LED1 сигнализирует о записи и отправке. LED2 горит при подключении к Wi-Fi.
