# Настройка Yandex Cloud

В [консоли Yandex Cloud](https://console.yandex.cloud/) используйте каталог в регионе Россия с подключённым платёжным аккаунтом.

1. Назначьте сервисному аккаунту роли `ai.speechkit-stt.user` и `ai.languageModels.user` на каталог.
2. Создайте API-ключ с областями `yc.ai.speechkitStt.execute` и `yc.ai.languageModels.execute`.
3. Скопируйте [secrets.example.json](secrets.example.json) в `secrets.json` рядом с ним. Укажите ключ в `YANDEX_API_KEY` и идентификатор каталога в `YANDEX_FOLDER_ID`. Модель задаётся в `YANDEX_GPT_MODEL`, по умолчанию `yandexgpt/latest`. Одноимённые переменные окружения имеют приоритет над файлом.

`secrets.json` хранится локально и исключён из Git. [Запуск сервера](README.md#запуск).
